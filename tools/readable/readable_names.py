#!/usr/bin/env python3
"""Names for the drafts (docs/READABLE.md, "Names"): what readable_draft.py
calls the functions, globals, parameters and locals it writes, beyond the
machine's addresses and registers.

The inputs, all committed:

  - the sweep manifests' name column (src/game/<layer>/sweep/manifest.txt):
    the swept functions' names; a caller's constant for one is named after it
    (kFun109BD0 becomes kSendFramePacket);
  - readable_names.tsv (next to this file): names with their evidence and
    confidence, a row each: kind, key, name, confidence, evidence. Kinds:
      function  ADDRESS            a swept function's name (`apply` writes it
                                   into the manifests' name column)
      global    ADDRESS            a fixed address's ee::Global
      param     ADDRESS:N          a function's Nth parameter (param_N); ADDRESS:argK /
                                   ADDRESS:fargK the drafter's register parameters (no signature)
    Function rows are used at every confidence (a low one's draft says so
    in a TODO line), param and global rows only high and medium;
  - the readable sources (read by readable_draft.py): a global's name from the
    hand-written ee::Global at its address, a callee's parameter names from
    its signature or from the comment of its hand-written constant
    (`kPoolGive = 0x001AC6F0u; // (pool, block) back`).

What the drafts gain, at build time (game.cmake runs the drafter):

  - globals: the hand-written name at the address (one read at the draft's
    width, then a public one, then the commonest; never DM2's own or the
    runtime's) or the table's, instead of g_XXXXXX;
  - callee constants: the swept callee's manifest name, instead of kFunXXXXXX;
  - fixed data addresses written as numbers: the hand-written game constant of
    the address (kMeshPacket) or kName of the hand-written global there;
  - struct fields: a read at a raw offset from a variable of a known struct
    type (wrapped as ee::Ptr<T> in the function, never reassigned), at an
    offset a hand-written static_assert proves, as `v + offsetof(T, field)`;
  - parameters (param_N): the table's, else the name the callee gives the
    argument it is handed unchanged, else the type it is wrapped in
    (ee::Ptr<zgui::Node>(param_1): node); the most suggested one, a tie to
    the type's;
  - locals named by register (v0, a1_2): a variable set once, by a call's
    result or a named global's value, is named after what it holds (the
    callee's result as its constant's comment says, `-> a voice`, or the
    callee's name less its verb: kFindObject's result is `object`); last, for
    a value nothing else says, its expression over named variables
    (_expression_name: flagsBit4, speedBelowLimit, nextEntry, heightMinusOffset,
    quaternionHigh, guiScrollViewByte for a load no field names);
  - parameters nothing names: how they are used (_parameter_role: callback,
    flags, limit, index, enabled);
  - labels (at_104728): what the code after them does first (done, setHeight,
    checkSpeed, callDrawPolys, join).

Names never change behaviour: only identifiers in the code (never comments)
are renamed, a name already used in the function or file is never taken, and
every renamed draft is the same code (readable_draft.py's output compiles to
the same calls in the same order; fncheck_corpus.py and logiccheck check it).

    readable_names.py apply [--dry-run]     table function rows -> manifests (the table wins)
    readable_names.py check                 the table's rows well formed, function names unique
    readable_names.py infer SWEEP_DIR       the swept functions' parameter names, as their drafts
                                            name them, for their callers (`inferred:` rows); drafts
                                            made with NAMES_NO_ROLES=1 (a name an expression or a use
                                            gives is no parameter's or field's name)
    readable_names.py sync-hand             function, global and parameter rows of hand-written
                                            code take the hand-written name (the sources are the
                                            authority); a name another function has is left out, said
                                            as a CLASH line, exit 1: rename one of the two
    readable_names.py pin-literals SWEEP_DIR [--dry-run]
                                            the .lit4 floats the drafts read pinned into the
                                            manifests' `elf` column (the drafts write them as literals)
"""
import hashlib
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
import readable_symbols

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_SRC = zaxis_paths.TREE     # (the readable code is the tree's: zaxis_paths.walk, key_of, readable)
TABLE = os.path.join(HERE, 'readable_names.tsv')
LAYOUT = os.path.join(HERE, 'readable_names_layout.tsv')
LAYERS = ('zge', 'aggressive', 'sce')
USED = ('high', 'medium')
# A function's name is applied at every confidence (the user, 2026-10-26: no
# fn_ left; where unsure, the best name the evidence supports, with its
# confidence): a low one's draft says so in a TODO line.
FUNCTION_USED = ('high', 'medium', 'low')
INFERRED = 'inferred:'
CALLERS = 'callers:'
OWN = 'own:'


CPP_RESERVED = set('''alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if
inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public
register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch
template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
wchar_t while xor xor_eq ee std game zge aggressive sce sweep ctx frame passes main abs free malloc memcpy memset
strlen strcmp strcpy sin cos sqrt exit time random rand printf sprintf assert errno min max NULL offsetof
size_t ptrdiff_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t'''.split())

REGISTER_NAME = re.compile(r'^(?:v[01]|a[0-7]|t[0-9]|s[0-8]|at|k[01]|gp|fp|ra|f(?:[0-9]|[12][0-9]|3[01])|'
                           r'product\d*|high\d*|taken|acc|lo|hi|stk_[0-9A-Fa-f]+)(?:_hi)?(?:_\d+)?$')
PARAM_NAME = re.compile(r'^(?:param_\d+|f?arg\d+)$')
VERBS = ('get', 'find', 'read', 'load', 'new', 'create', 'make', 'lookup', 'query', 'calc', 'calculate', 'compute',
         'alloc', 'allocate', 'take', 'pick', 'fetch', 'open', 'build', 'choose', 'locate', 'pop')
BOOL_VERBS = ('is', 'has', 'can', 'should', 'was', 'does')
TYPE_WORDS = set('''vec4 vec3 vec2 float int uint8 int8 uint16 int16 uint32 int32 uint64 int64 qword word byte
pointer ptr void char bool matrix quaternion string text value result address count size number'''.split())
ARTICLES = ('a', 'an', 'the', 'its', 'his', 'her', 'their', 'that', 'this', 'one')


# ---------------------------------------------------------------------------
# The table and the manifests' names
# ---------------------------------------------------------------------------

# The EE's hardware registers (the SDK's eeregs.h names, in camel case): drafts
# write them as constants, a value read from one takes its name.
HARDWARE_REGISTERS = {
    0x10000000: 'T0Count', 0x10000010: 'T0Mode', 0x10000020: 'T0Comp', 0x10000030: 'T0Hold',
    0x10000800: 'T1Count', 0x10000810: 'T1Mode', 0x10000820: 'T1Comp', 0x10000830: 'T1Hold',
    0x10001000: 'T2Count', 0x10001010: 'T2Mode', 0x10001020: 'T2Comp',
    0x10001800: 'T3Count', 0x10001810: 'T3Mode', 0x10001820: 'T3Comp',
    0x10002000: 'IpuCmd', 0x10002010: 'IpuCtrl', 0x10002020: 'IpuBp', 0x10002030: 'IpuTop',
    0x10003000: 'GifCtrl', 0x10003010: 'GifMode', 0x10003020: 'GifStat',
    0x10003800: 'Vif0Stat', 0x10003810: 'Vif0Fbrst', 0x10003820: 'Vif0Err', 0x10003830: 'Vif0Mark',
    0x10003840: 'Vif0Cycle', 0x10003850: 'Vif0Mode', 0x10003860: 'Vif0Num', 0x10003870: 'Vif0Mask',
    0x10003880: 'Vif0Code', 0x10003890: 'Vif0Itops', 0x100038D0: 'Vif0Itop',
    0x10003900: 'Vif0R0', 0x10003910: 'Vif0R1', 0x10003920: 'Vif0R2', 0x10003930: 'Vif0R3',
    0x10003940: 'Vif0C0', 0x10003950: 'Vif0C1', 0x10003960: 'Vif0C2', 0x10003970: 'Vif0C3',
    0x10003C00: 'Vif1Stat', 0x10003C10: 'Vif1Fbrst', 0x10003C20: 'Vif1Err', 0x10003C30: 'Vif1Mark',
    0x10003C40: 'Vif1Cycle', 0x10003C50: 'Vif1Mode', 0x10003C60: 'Vif1Num', 0x10003C70: 'Vif1Mask',
    0x10003C80: 'Vif1Code', 0x10003C90: 'Vif1Itops', 0x10003CA0: 'Vif1Base', 0x10003CB0: 'Vif1Ofst',
    0x10003CC0: 'Vif1Tops', 0x10003CD0: 'Vif1Itop', 0x10003CE0: 'Vif1Top',
    0x10003D00: 'Vif1R0', 0x10003D10: 'Vif1R1', 0x10003D20: 'Vif1R2', 0x10003D30: 'Vif1R3',
    0x10003D40: 'Vif1C0', 0x10003D50: 'Vif1C1', 0x10003D60: 'Vif1C2', 0x10003D70: 'Vif1C3',
    0x10004000: 'Vif0Fifo', 0x10004008: 'Vif0FifoHigh', 0x10005000: 'Vif1Fifo', 0x10005008: 'Vif1FifoHigh',
    0x10008000: 'D0Chcr', 0x10008010: 'D0Madr', 0x10008020: 'D0Qwc', 0x10008030: 'D0Tadr',
    0x10009000: 'D1Chcr', 0x10009010: 'D1Madr', 0x10009020: 'D1Qwc', 0x10009030: 'D1Tadr',
    0x1000A000: 'D2Chcr', 0x1000A010: 'D2Madr', 0x1000A020: 'D2Qwc', 0x1000A030: 'D2Tadr',
    0x1000B000: 'D3Chcr', 0x1000B010: 'D3Madr', 0x1000B020: 'D3Qwc',
    0x1000B400: 'D4Chcr', 0x1000B410: 'D4Madr', 0x1000B420: 'D4Qwc', 0x1000B430: 'D4Tadr',
    0x1000C000: 'D5Chcr', 0x1000C010: 'D5Madr', 0x1000C020: 'D5Qwc',
    0x1000C400: 'D6Chcr', 0x1000C410: 'D6Madr', 0x1000C420: 'D6Qwc',
    0x1000C800: 'D7Chcr', 0x1000C810: 'D7Madr', 0x1000C820: 'D7Qwc',
    0x1000D000: 'D8Chcr', 0x1000D010: 'D8Madr', 0x1000D020: 'D8Qwc', 0x1000D080: 'D8Sadr',
    0x1000D400: 'D9Chcr', 0x1000D410: 'D9Madr', 0x1000D420: 'D9Qwc', 0x1000D430: 'D9Tadr', 0x1000D480: 'D9Sadr',
    0x1000E000: 'DCtrl', 0x1000E010: 'DStat', 0x1000E020: 'DPcr', 0x1000E030: 'DSqwc',
    0x1000E040: 'DRbsr', 0x1000E050: 'DRbor', 0x1000E060: 'DStadr',
    0x1000F000: 'IStat', 0x1000F010: 'IMask', 0x1000F130: 'SioIsr', 0x1000F180: 'SioTxFifo',
    0x1000F520: 'DEnableR', 0x1000F590: 'DEnableW',
}


_STATE = {}


def read_table(path=TABLE):
    """[(kind, key, name, confidence, evidence)] of the table."""
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        cols = line.split('\t') + [''] * 5
        rows.append(tuple(c.strip() for c in cols[:5]))
    return rows


def manifest_names(game_src=GAME_SRC):
    """{address: (name, file)} of the sweep manifests' rows."""
    out = {}
    for layer in LAYERS:
        path = zaxis_paths.readable(game_src, layer, 'sweep', 'manifest.txt')
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8'):
            if not line.startswith('0x'):
                continue
            cols = line.rstrip('\n').split('\t')
            if len(cols) > 6:
                out[int(cols[0], 16)] = (cols[6] if cols[6] != '-' else '', cols[2])
    return out


def state():
    if 'rows' not in _STATE:   # (other caches share _STATE: layout_results, written_lit4)
        rows = read_table()
        _STATE['rows'] = rows
        # Global rows of every confidence name their globals (the user: the best name the
        # evidence supports, with a confidence); a low one's declaration says so.
        _STATE['globals'] = {int(k, 16): n for kind, k, n, c, _ in rows if kind == 'global' and c in FUNCTION_USED}
        _STATE['globals_guessed'] = {int(k, 16) for kind, k, n, c, _ in rows if kind == 'global' and c == 'low'}
        _STATE['guessed'] = {int(k, 16): ev for kind, k, n, c, ev in rows if kind == 'function' and c == 'low'}
        params = {}
        inferred = {}
        callers = {}
        argparams = {}
        for kind, k, n, c, ev in rows:
            if kind == 'param' and c in USED and re.match(r'^0x[0-9A-Fa-f]+:f?arg\d+$', k):
                # (a function no signature declares: the drafter's argN / fargN, by register)
                a, v = k.split(':')
                argparams[(int(a, 16), v)] = n
                continue
            if kind == 'param' and c in USED:
                a, i = k.split(':')
                # `infer` rows (a swept function's parameter as its own draft
                # names it) name its callers' arguments only; `callers:` rows
                # (the name its callers' arguments have) its own parameter
                # where its own code gives it none.
                (inferred if ev.startswith(INFERRED) else callers if ev.startswith(CALLERS)
                 else params)[(int(a, 16), int(i))] = n
        _STATE['params'] = params
        _STATE['argparams'] = argparams
        _STATE['ptypes'] = {(int(k.split(':')[0], 16), int(k.split(':')[1])): n for kind, k, n, c, _ in rows
                            if kind == 'ptype' and c in USED}
        _STATE['inferred'] = inferred
        # gtype rows: a table global holding a pointer to a struct (its loads typed so)
        _STATE['gtypes'] = {int(k, 16): n for kind, k, n, c, _ in rows if kind == 'gtype' and c in USED}
        # rtype rows: a function's result points to a struct (the values set from its calls typed so)
        _STATE['rtypes'] = {int(k, 16): n for kind, k, n, c, _ in rows if kind == 'rtype' and c in USED}
        # vtype rows: a local of a function points to a struct (by the name its draft gives it)
        vtypes = {}
        for kind, k, n, c, _ in rows:
            if kind == 'vtype' and c in USED:
                a, v = k.split(':')
                vtypes.setdefault(int(a, 16), {})[v] = n
        _STATE['vtypes'] = vtypes
        _STATE['callers'] = callers
        _STATE['functions'] = manifest_names()
        _STATE['unswept'] = {int(k, 16): n for kind, k, n, c, _ in rows if kind == 'function' and c in FUNCTION_USED and
                             int(k, 16) not in _STATE['functions']}
        by_file = {}
        for a, (n, f) in _STATE['functions'].items():
            by_file.setdefault(f, set()).add(n)
        _STATE['file_names'] = by_file
    return _STATE


def digest():
    """What the drafts' names depend on here (readable_draft.py's DraftCache
    key): the table and every manifest's (address, name)."""
    s = state()
    h = hashlib.sha1()
    h.update(open(__file__, 'rb').read())
    h.update(repr(s['rows']).encode())
    h.update(repr(sorted((a, n) for a, (n, _) in s['functions'].items())).encode())
    # the struct layouts (static_asserts, members' offset comments, the layout
    # table) and the hand-written globals' records the drafts' names come from
    h.update(repr(sorted((t, sorted(f.items())) for t, f in struct_fields().items() if t)).encode())
    h.update(repr(sorted(record_globals().items())).encode())
    if os.path.exists(LAYOUT):
        h.update(open(LAYOUT, 'rb').read())   # (its confidences and `name` rows too)
    return h.hexdigest()


def machine_function_name(name):
    return not name or re.match(r'^(?:fn|FUN|sub|thunk_FUN)_[0-9A-Fa-f]+', name) is not None


def global_guessed(address):
    """Whether a global's table name is a low-confidence guess."""
    return address in state()['globals_guessed']


def function_name(address):
    """A swept function's name from its manifest row, None while it is
    machine-shaped; a function no manifest has (a game override's host
    function, a call into another's middle) by its table row."""
    n = state()['functions'].get(address, ('', ''))[0]
    if machine_function_name(n) and address not in state()['functions']:
        n = state()['unswept'].get(address, '')
    return None if machine_function_name(n) else n


def constant_for(name):
    """kName for a callee's constant; kNameFunction when kName is itself a
    function's name (libkernl's kFindAddress beside its FindAddress)."""
    name = re.sub(r'\W', '', name)
    k = 'k' + name[0].upper() + name[1:]
    if 'all_names' not in _STATE:
        s = state()
        _STATE['all_names'] = {n for n, _f in s['functions'].values() if n} | set(s['unswept'].values())
    return k + 'Function' if k in _STATE['all_names'] else k


def unambiguous_constant(ws, address, name):
    """A callee constant's name: the hand-written sources' (name), unless they
    give that name to other addresses too (kFree for heapFree, freeSized and
    freeMemory: a batch file with two would number them); then the
    function's own name's constant (its manifest's or readable name)."""
    if 'constant_addresses' not in _STATE:
        by = {}
        # (untracked: the batch notes the addresses it looks at below)
        for a, lst in dict.items(getattr(ws, 'constants', None) or {}):
            for n, _c in lst:
                by.setdefault(n, set()).add(a)
        _STATE['constant_addresses'] = by
    if len(_STATE['constant_addresses'].get(name, ())) < 2:
        return name
    for a in sorted(_STATE['constant_addresses'][name]):
        ws.constants.get(a)
    own = function_name(address) or (ws.name_of(address) if hasattr(ws, 'name_of') else None)
    if not own or machine_function_name(own):
        return name
    k = constant_for(own)
    return k if len(_STATE['constant_addresses'].get(k, {address}) - {address}) == 0 else name


# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------

def _type_size(t):
    if not t:
        return None
    if t in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64'):
        return int(t[1:]) // 8
    if t == 'f':
        return 4
    m = re.search(r'int(8|16|32|64)_t', t)
    if m:
        return int(m.group(1)) // 8
    if re.search(r'Float|float|Ptr<', t):
        return 4
    return None


def global_name(address, known, t=None):
    """The name of a fixed address's ee::Global: the table's, else the
    hand-written one (known: readable_draft.py's [(name, type, namespace)]),
    the commonest of several (the first in order on a tie); None for none."""
    table = state()['globals'].get(address)
    if table:
        return table
    # DM2's sources name DM2's addresses (dm2::...), the runtime's (ee::...) its
    # own words: never an AI global's name.
    known = [k for k in known or () if not re.match(r'^(?:dm2|ee)(?:::|$)', k[2] or '')]
    if not known:
        return record_global_name(address)
    counts = {}
    public = set()
    sized = set()
    for n, kt, ns in known:
        counts[n] = counts.get(n, 0) + 1
        if t is not None and _type_size(kt) == _type_size(t):
            sized.add(n)
        if '(anonymous)' not in (ns or ''):
            public.add(n)
    # One read as the draft reads it (a word, not its low byte), a public name
    # (the one other code uses), then the commonest.
    best = sorted(counts, key=lambda n: (bool(sized) and n not in sized, n not in public, -counts[n],
                                         [k[0] for k in known].index(n)))[0]
    if not re.match(r'^[A-Za-z_]\w*$', best) or best in CPP_RESERVED:
        return None
    return best if best.startswith('g_') else 'g_' + best[0].lower() + best[1:]


# ---------------------------------------------------------------------------
# The .lit4 literals
# ---------------------------------------------------------------------------

# The ELF's .lit4 section: the compiler's pool of the source's float literals
# (gp-relative loads of 1.0f, 0.0166667f, ...), never written by the game.
LIT4 = (0x00272900, 0x0027322C)
# Words of it the runtime writes, so read from memory still: (address, size): why.
RUNTIME_WRITTEN = {
    (0x00272944, 12): "dm2/rider/host/ai_world.cpp's Dm2SplitTolerances: DM2's three split tolerances copied over "
                      "AI's while a DM2 body walks AI's world",
}


def _written_lit4():
    if 'written_lit4' not in _STATE:
        out = set()
        for (a, n) in RUNTIME_WRITTEN:
            out.update(range(a & ~3, a + n, 4))
        # (any word hand-written code names by a raw address constant: it may write it)
        for a in range(LIT4[0], LIT4[1], 4):
            if game_constant_names(a):
                out.add(a)
        _STATE['written_lit4'] = out
    return _STATE['written_lit4']


def float_text(bits):
    """C++ text of a float's bits: ee::Float(0.0166667f) (the shortest decimal
    that reads back as the same bits), ee::Float::fromBits(0x...u) for a NaN,
    an infinity or a denormal."""
    import struct
    bits &= 0xFFFFFFFF
    f = struct.unpack('<f', struct.pack('<I', bits))[0]
    if not (f != f or f in (float('inf'), float('-inf')) or (bits & 0x7F800000) == 0 and bits & 0x7FFFFF):
        for prec in range(1, 10):
            s = '%.*g' % (prec, f)
            if struct.unpack('<I', struct.pack('<f', float(s)))[0] == bits:
                if 'e' not in s and '.' not in s:
                    s += '.0'
                return 'ee::Float(%sf)' % s
    return 'ee::Float::fromBits(0x%08Xu)' % bits


def literal_float(ws, address):
    """A float read from a fixed address of .lit4 as the literal it is
    (ee::Float(0.5f)) instead of a global; None elsewhere, for the words the
    runtime writes, and while the word is not known (the build knows the
    words its manifests' `elf` columns pin: readable_names.py pin-literals)."""
    if not (LIT4[0] <= address < LIT4[1]) or address & 3 or address in _written_lit4():
        return None
    word = ws.elf_word(address)
    return None if word is None else float_text(word)


def pin_literals(sweep_dir, dry_run=False):
    """Pins the .lit4 words the swept functions read as floats into their
    manifest rows' `elf` column (as readable_sweep.py pins the words a draft
    reads), from a full draft of the sweep made without them (SWEEP_DIR: its
    spans.json and batch files) and the ELF. Returns the number of words added."""
    import json
    rd = _draft_module()
    ws = rd.Workspace.__new__(rd.Workspace)
    ws.workspace = zaxis_paths.get('AI_WORKSPACE')
    spans = json.load(open(os.path.join(sweep_dir, 'spans.json')))
    decl = re.compile(r'ee::Global<ee::Float, 0x([0-9A-Fa-f]{8})u>\s+(\w+);')
    reads = {}
    for f, fns in spans.items():
        text = open(os.path.join(sweep_dir, f), encoding='utf-8').read()
        globs = {}
        for m in decl.finditer(text):
            a = int(m.group(1), 16)
            if LIT4[0] <= a < LIT4[1] and a not in _written_lit4():
                globs[m.group(2)] = a
        if not globs:
            continue
        lines = text.split('\n')
        pat = re.compile(r'\*(%s)\b' % '|'.join(sorted(globs, key=len, reverse=True)))
        for k, (s0, s1) in fns.items():
            for line in lines[s0 - 1:s1]:
                for m in pat.finditer(code_part(line)[0]):
                    reads.setdefault(int(k, 16), set()).add(globs[m.group(1)])
    added = 0
    for layer in LAYERS:
        path = zaxis_paths.readable(GAME_SRC, layer, 'sweep', 'manifest.txt')
        header, entries = rd.read_manifest(path)
        n = 0
        for e in entries:
            for a in sorted(reads.get(e['address'], ())):
                w = ws._elf_word(a)
                if w is not None and e['elf'].get(a) != w:
                    e['elf'] = dict(e['elf'])
                    e['elf'][a] = w
                    n += 1
        if n and not dry_run:
            rd.write_manifest(path, header, entries)
        sys.stderr.write('%s: %d words\n' % (zaxis_paths.key_of(path, GAME_SRC), n))
        added += n
    return added


# ---------------------------------------------------------------------------
# Words to names
# ---------------------------------------------------------------------------

def lower_camel(name):
    """FooBar -> fooBar, VU1Upload -> vu1Upload, DMAKick -> dmaKick."""
    m = re.match(r'^([A-Z][A-Z0-9]*)(?=[A-Z][a-z]|$)', name)
    if m and len(m.group(1)) > 1:
        return m.group(1).lower() + name[m.end():]
    return name[:1].lower() + name[1:]


def phrase_name(text):
    """A name from a short phrase of a comment ('heap block' -> heapBlock,
    'name hash *' -> nameHash); None unless it is one to three plain words."""
    t = re.sub(r'[*&]', ' ', text)
    t = re.sub(r'\b(?:const|unsigned|signed|uint\d+|int\d+|u?int\d+_t|struct|f\d+|[avst]\d)\b', ' ', t)
    words = t.split()
    while words and words[0].lower() in ARTICLES:
        words = words[1:]
    if not 1 <= len(words) <= 3:
        return None
    if not all(re.match(r'^[A-Za-z][A-Za-z0-9]*$', w) for w in words):
        return None
    if len(words) == 1 and words[0].lower() in TYPE_WORDS:
        return None
    first = words[0].lower() if words[0].isupper() else words[0][:1].lower() + words[0][1:]   # (UI asset: uiAsset)
    name = first + ''.join(w[:1].upper() + w[1:] for w in words[1:])
    name = lower_camel(name)
    if name in CPP_RESERVED or REGISTER_NAME.match(name) or PARAM_NAME.match(name):
        return None
    return name


def split_args(text):
    """The top-level comma-separated parts of text."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '(<[{':
            depth += 1
        elif ch in ')>]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def comment_signature(comment):
    """(parameter names, result name) from a hand-written constant's comment:
    `(sample, flags) -> a voice, 255 for none`; `FUN_001528d8 ((sender, target,
    type, data, synchronous))`. Names are None where a part is not plain words."""
    if not comment:
        return None, None
    c = re.sub(r'^(?:thunk_)?FUN_[0-9a-fA-F]+\s*', '', comment.strip())
    if c.startswith('((') and c.endswith(')'):
        c = c[1:-1]
    if not c.startswith('('):
        return None, None
    depth = 0
    end = None
    for i, ch in enumerate(c):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None, None
    inner = c[1:end]
    params = [phrase_name(p) for p in split_args(inner)] if inner.strip() else []
    result = None
    m = re.match(r'^\s*->\s*([^,;:()]+)', c[end + 1:])
    if m:
        result = phrase_name(m.group(1).strip())
    return params, result


# libgcc's soft-float and 64-bit helpers (GCC 2.95's fp-bit.c and libgcc2.c,
# docs/READABLE.md "Layout"): what their results are.
LIBGCC_RESULTS = {'dpadd': 'sum', 'fpadd': 'sum', 'dpsub': 'difference', 'fpsub': 'difference', 'dpmul': 'product',
                  'fpmul': 'product', 'dpdiv': 'quotient', 'fpdiv': 'quotient', 'dpneg': 'negated', 'fpneg': 'negated',
                  'dpcmp': 'comparison', 'fpcmp': 'comparison', 'fptodp': 'asDouble', 'litodp': 'asDouble',
                  'dptofp': 'asFloat', 'litofp': 'asFloat', 'dptoli': 'asInteger', 'fptoli': 'asInteger',
                  'fptoui': 'asUnsigned', 'dptoui': 'asUnsigned', 'muldi3': 'product', 'divdi3': 'quotient',
                  'udivdi3': 'quotient', 'moddi3': 'remainder', 'umoddi3': 'remainder', 'ashldi3': 'shifted',
                  'ashrdi3': 'shifted', 'lshrdi3': 'shifted', 'cmpdi2': 'comparison', 'ucmpdi2': 'comparison',
                  'negdi2': 'negated', 'fixdfdi': 'asInteger', 'fixsfdi': 'asInteger', 'floatdidf': 'asDouble',
                  'floatdisf': 'asFloat', 'fixunsdfdi': 'asUnsigned', 'fixunssfdi': 'asUnsigned',
                  # the C library's, whose names are C++'s too
                  'malloc': 'block', 'strlen': 'length', 'strcmp': 'comparison', 'abs': 'absolute',
                  'sqrt': 'root', 'sin': 'sine', 'cos': 'cosine', 'random': 'randomValue', 'rand': 'randomValue',
                  'time': 'timeValue', 'min': 'minimum', 'max': 'maximum'}


def noun_of_function(name):
    """What a function's result is, from its name: findObject -> object,
    objectNameHash -> objectNameHash, isOpen -> isOpen; None for a verb alone."""
    name = re.sub(r'^k(?=[A-Z])', '', name)
    if machine_function_name(name) or re.match(r'^Fun[0-9A-F]{6}', name):
        return None
    name = name.lstrip('_')   # (newlib's __sprint, _nextBit: no reserved names)
    if not name:
        return None
    if name in LIBGCC_RESULTS or lower_camel(name) in LIBGCC_RESULTS:
        return LIBGCC_RESULTS.get(name) or LIBGCC_RESULTS[lower_camel(name)]
    base = lower_camel(name)
    base = re.sub(r'(?<=[a-z0-9])Of$', '', base)   # pickupOf -> pickup
    # A name that ends in a verb (searchTreeFind, poolTake) says what it does.
    if re.search(r'(?<=[a-z0-9])(?:Find|Get|Insert|Take|Give|Add|Remove|Push|Pop|Lock|Unlock|Init|Reset|Copy|Set|'
                 r'Update|Draw|Create|Free|Alloc|Release|Load|Read|Write|Open|Close|Clear|Call|Run|Send|Start|'
                 r'Stop|Play|Kill|Destroy|Delete|Attach|Detach|Fill|Append|Format|Print|Swap|Wait|Sync|Flush|'
                 r'Notify|Queue|Enable|Disable|Show|Hide|Begin|End|Enter|Leave|Exit)$', base):
        return None
    for v in BOOL_VERBS:
        if re.match(r'^%s[A-Z]' % v, base):
            return base
    for v in sorted(VERBS, key=len, reverse=True):
        if base == v:
            return None
        if re.match(r'^%s[A-Z0-9]' % v, base):
            rest = lower_camel(base[len(v):])
            return rest if rest not in CPP_RESERVED and not REGISTER_NAME.match(rest) else None
    # A name that starts with another verb (sendMessage, playVoice) says what
    # it does, not what it gives back.
    if re.match(r'^(?:set|send|play|stop|start|update|draw|render|init|reset|clear|add|remove|free|release|'
                r'write|copy|move|apply|call|run|do|process|handle|mark|push|insert|attach|detach|enable|'
                r'disable|show|hide|kill|destroy|delete|register|unregister|post|emit|spawn|award|give|'
                r'rotate|transform|scale|translate|normalize|sort|hash|parse|print|close|lock|unlock|'
                r'wait|sync|flush|begin|end|enter|exit|leave|tick|step|advance|on|toggle|swap|fill|'
                r'zero|wrap|bind|link|unlink|use|save|store|put|queue|dispatch|signal|notify|request)'
                r'(?:[A-Z0-9]|$)', base):
        return None
    if base in CPP_RESERVED or REGISTER_NAME.match(base):
        return None
    return base


_GAME_CONSTANTS = {}


def game_constant_names(address):
    """The names hand-written game code gives a fixed address: the constants
    under aggressive/, zge/, sce/ and DM2's host side (dm2/**/ai_*, and kAi* elsewhere
    in dm2/); not the runtime's own (ee/: profile.cpp's kCodeEnd is a profiler bound) nor DM2's."""
    if not _GAME_CONSTANTS:
        pat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;')
        _GAME_CONSTANTS[None] = True
        for root, dirs, files in zaxis_paths.walk(GAME_SRC):
            dirs.sort()
            rel = zaxis_paths.key_of(root, GAME_SRC).split(os.sep)
            if rel[0] == 'ee' or os.path.basename(root) in ('drafts', 'sweep'):
                dirs[:] = []
                continue
            for fn in sorted(files):
                if not fn.endswith(('.cpp', '.h')):
                    continue
                ai_only = rel[0] == 'dm2' and not fn.startswith('ai_')
                for m in pat.finditer(readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read())):
                    # (DM2's other files: only their kAi* names of AI addresses)
                    if ai_only and not re.match(r'^kAi[A-Z]', m.group(1)):
                        continue
                    _GAME_CONSTANTS.setdefault(int(m.group(2), 16), set()).add(m.group(1))
    return _GAME_CONSTANTS.get(address, set())


_FIELDS = {}
_FIELD_HEADERS = {}   # struct -> the header that defines it (relative to src/game)
_FIELD_SOURCES = {}   # struct -> the source file (.cpp) that alone defines it
_STRUCT_FILES = {}    # struct -> the files that define a struct of that name
# Variables the names module named after what they hold, typed by that name
# when the function wraps them in no type (`player` is handed where a
# readable function takes a Player): their fields are named like a wrapped one's.
NAME_TYPES = {'player': 'aggressive::skater::Player', 'sceneObject': 'zge::SceneObject'}


def _ai_layouts_only(raw):
    """The source as AI's build sees it where another game's layout stands in `#if ZX_GAME_DM2` (or the #else of
    `#if !ZX_GAME_DM2`): those lines blanked (line numbers kept), so another game's offsets are not AI's."""
    out, stack = [], []  # per open #if: (is a game test, its branch is skipped now)
    for line in raw.split('\n'):
        d = line.strip()
        skip = any(s for _g, s in stack)
        if d.startswith('#if'):
            m = re.match(r'#if\s+(!?)ZX_GAME_(DM2|BMX)\s*$', d)
            stack.append((bool(m), bool(m) and not m.group(1)))
            out.append('' if m or skip else line)
            continue
        if d.startswith('#else') and stack and stack[-1][0]:
            stack[-1] = (True, not stack[-1][1])
            out.append('')
            continue
        if d.startswith('#endif') and stack:
            g, _s = stack.pop()
            out.append('' if g or any(s for _g, s in stack) else line)
            continue
        out.append('' if skip else line)
    return '\n'.join(out)


def struct_fields():
    """{struct's qualified name: {offset: (field, how)}}: the offsets a
    hand-written layout gives a name, how being
      'proven'  a static_assert(offsetof(Player, pendingScore) == 0x8C8) proves it;
      'member'  a member of the struct whose comment gives its offset
                (`uint32_t node; // +0x014 ...`, or the comment block before
                it starting `// +0x002 ...`): written as offsetof() with a
                static_assert of the draft's own, so the compiler checks it;
      'table'   readable_names_layout.tsv's row for an offset the struct has
                no member at (padding): written as a constant kStructField.
    An offset two layouts of one name give different fields is left out."""
    if not _FIELDS:
        _FIELDS[None] = {}
        pat = re.compile(r'offsetof\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)u?\b')
        found = {}
        members = {}
        cpp_members = {}
        for root, dirs, files in zaxis_paths.walk(GAME_SRC):
            dirs.sort()
            if os.path.basename(root) in ('drafts', 'sweep'):
                dirs[:] = []
                continue
            for fn in sorted(files):
                if fn.endswith(('.cpp', '.h')):
                    raw = _ai_layouts_only(readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()))
                    text = _code_only(raw)
                    for m in pat.finditer(text):
                        # Keyed by the qualified name: zge::Node and zge::zgui::Node differ.
                        ns = '::'.join(n for n in _namespace_at(text, m.start()) if n != '(anonymous)')
                        key = (ns + '::' if ns else '') + m.group(1)
                        found.setdefault(key, {}).setdefault(int(m.group(3), 0), set()).add(m.group(2))
                    rel = zaxis_paths.key_of(os.path.join(root, fn), GAME_SRC).replace(os.sep, '/')
                    commented = _commented_members(raw, text)
                    for q in {k for k, _n, _t, _o in _struct_decls(raw, text)}:
                        _STRUCT_FILES.setdefault(q, set()).add(rel)
                    if fn.endswith('.h'):
                        for key in re.findall(r'(?<![\w>])struct\s+(?:alignas\(\d+\)\s+)?(\w+)\s*\{', text):
                            for q in [k for k in commented if k.split('::')[-1] == key]:
                                _FIELD_HEADERS.setdefault(q, set()).add(rel)
                    # A source file's own struct (one only it defines: drafts
                    # cannot see it, so its fields are written as constants).
                    for key, offs in commented.items():
                        cpp_members.setdefault(key, []).append((rel, offs))
        for key, defs in cpp_members.items():
            files = _STRUCT_FILES.get(key, set())
            if len(files) != 1:
                continue   # two structs of one name (or none): neither
            for rel, offs in defs:
                if rel.endswith('.cpp'):
                    _FIELD_SOURCES[key] = rel
                for o, f in offs.items():
                    members.setdefault(key, {}).setdefault(o, set()).update(f)
        for t, offs in found.items():
            _FIELDS[t] = {o: (next(iter(f)), 'proven') for o, f in offs.items() if len(f) == 1}
        for t, offs in members.items():
            have = _FIELDS.setdefault(t, {})
            names = {f for f, _ in have.values()}
            for o, f in offs.items():
                if len(f) == 1 and o not in have and next(iter(f)) not in names:
                    have[o] = (next(iter(f)), 'member')
        for t, o, f in layout_rows():
            have = _FIELDS.setdefault(t, {})
            if o not in have:
                have[o] = (f, 'table')
        # A struct the layout table derives from another (a zgui widget kind
        # from GuiWidget, GuiWidget from zgui's Node) has its base's fields
        # too, written as the base's (offsetof(Node, name), kGuiWidgetOnCreate).
        bases = layout_bases()

        def inherit(t, seen=()):
            b = bases.get(t)
            if not b or b in seen:
                return
            inherit(b, seen + (t,))
            have = _FIELDS.setdefault(t, {})
            for o, f in _FIELDS.get(b, {}).items():
                if o not in have:
                    have[o] = f
                    _FIELD_OWNER[(t, o)] = _FIELD_OWNER.get((b, o), b)
        for t in sorted(bases):
            inherit(t)
    return _FIELDS


_FIELD_OWNER = {}   # (struct, offset) -> the base struct the field is of


def field_owner(t, o):
    return _FIELD_OWNER.get((t, o), t)


_MEMBER_TYPES = {}


def member_types():
    """{qualified struct: {member: its declared type, as written}} of the
    hand-written headers' top-level structs (`Controls controls; // +0x550`,
    `ee::Ptr<Node> firstChild; // +0x04`). A member two headers declare
    with different types is left out."""
    if not _MEMBER_TYPES:
        _MEMBER_TYPES[None] = {}
        seen = {}
        for root, dirs, files in zaxis_paths.walk(GAME_SRC):
            dirs.sort()
            if os.path.basename(root) in ('drafts', 'sweep'):
                dirs[:] = []
                continue
            for fn in sorted(files):
                if fn.endswith('.h'):
                    raw = readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read())
                    for key, name, t, _off in _struct_decls(raw, _code_only(raw)):
                        seen.setdefault(key, {}).setdefault(name, set()).add(t)
        for key, ms in seen.items():
            _MEMBER_TYPES[key] = {n: next(iter(ts)) for n, ts in ms.items() if len(ts) == 1}
    return _MEMBER_TYPES


# Names too general to say a variable's type (`message` is a script or an
# engine message, `color` mostly a value), whatever the structs are called.
GENERIC_TYPE_NAMES = {'message', 'color', 'box', 'pad', 'playing', 'list', 'node', 'entry', 'record', 'block', 'data',
                      'item', 'value', 'state', 'temporary', 'interpolate', 'layer', 'edge', 'zone', 'slot', 'prop',
                      'ride', 'listeners', 'carrier', 'spinners',
                      # the park editor's: an edit object of any class (its parts are typed: EditToolPart, ...)
                      'editObject'}
_NAME_TYPES = {}


def name_types():
    """{variable name: struct} for the hand-written structs whose short name,
    in lower camel case, is one struct's (not DM2's: its structs are DM2's),
    with fields and one header: `chunkReader` -> zge::ChunkReader."""
    if not _NAME_TYPES:
        _NAME_TYPES[None] = None
        by = {}
        for t, fs in struct_fields().items():
            if t and fs and not t.startswith(('game::dm2', 'dm2::')):
                by.setdefault(lower_camel(t.split('::')[-1]), set()).add(t)
        for n, ts in by.items():
            q = next(iter(ts))
            if len(ts) == 1 and n not in GENERIC_TYPE_NAMES and (len(_FIELD_HEADERS.get(q, ())) == 1 or
                                                                  not _FIELD_HEADERS.get(q) and _FIELD_SOURCES.get(q)) and \
                    not _machine(n) and n not in CPP_RESERVED:
                _NAME_TYPES[n] = q
        # the structs infer recovered for the variables of one name
        for n, q in layout_names().items():
            if struct_fields().get(q) and not _FIELD_HEADERS.get(q) and not _FIELD_SOURCES.get(q):
                _NAME_TYPES.setdefault(n, q)
    return {n: q for n, q in _NAME_TYPES.items() if n}


_SIZES = {}


def struct_sizes():
    """{qualified struct: size} of the static_assert(sizeof(T) == N)s in the
    hand-written sources (one size each)."""
    if not _SIZES:
        _SIZES[None] = None
        pat = re.compile(r'static_assert\(\s*sizeof\(\s*([\w:]+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)u?\b')
        found = {}
        for root, dirs, files in zaxis_paths.walk(GAME_SRC):
            dirs.sort()
            if os.path.basename(root) in ('drafts', 'sweep'):
                dirs[:] = []
                continue
            for fn in sorted(files):
                if fn.endswith(('.cpp', '.h')):
                    text = _code_only(readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()))
                    for m in pat.finditer(text):
                        ns = '::'.join(n for n in _namespace_at(text, m.start()) if n != '(anonymous)')
                        t = m.group(1)
                        key = t if '::' in t else (ns + '::' if ns else '') + t
                        found.setdefault(key, set()).add(int(m.group(2), 0))
        for t, ns in found.items():
            if len(ns) == 1:
                _SIZES[t] = next(iter(ns))
        # the layout table's `size` rows: a recovered struct's size (none a source proves)
        if os.path.exists(LAYOUT):
            for line in open(LAYOUT, encoding='utf-8'):
                c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
                if not line.startswith('#') and c[1] == 'size' and c[3] in FUNCTION_USED and \
                        re.match(r'^(?:0x[0-9A-Fa-f]+|\d+)$', c[2]):
                    _SIZES.setdefault(c[0], int(c[2], 0))
    return {t: n for t, n in _SIZES.items() if t}


def _resolve_struct(t, owner):
    """The qualified struct a member's type names, looked up from its owner's
    namespace outward among the structs with known fields; (kind, struct):
    'embed' for a struct member (`Controls controls;`), 'ptr' for a pointer
    to one (`ee::Ptr<Node> parent;`, `Node *parent;`); None otherwise."""
    t = re.sub(r'\bconst\b|\bvolatile\b', '', t).strip()
    m = re.match(r'^ee::Ptr<\s*([\w:]+)\s*>$', t) or re.match(r'^([\w:]+)\s*\*$', t)
    kind, name = ('ptr', m.group(1)) if m else ('embed', t)
    if not re.match(r'^(?:\w+::)*[A-Z]\w*$', name):
        return None
    fields = struct_fields()
    spaces = owner.split('::')[:-1]
    for i in range(len(spaces), -1, -1):
        q = '::'.join(spaces[:i] + [name])
        if fields.get(q):
            return kind, q
    return None


def embedded_field(q, fs, o):
    """An offset inside a struct's embedded struct member (Player +0x560 in
    `Controls controls; // +0x550`): ((member offset, member, how), (the
    member's struct, its field, how)) when the struct's last named field
    before o is an embedded struct with a field (proven or commented) at the
    rest; None otherwise."""
    before = [b for b in fs if b < o]
    if not before:
        return None
    b = max(before)
    field, how = fs[b]
    owner = field_owner(q, b)
    t = layout_types().get(owner, {}).get(field) if how == 'table' else member_types().get(owner, {}).get(field)
    got = _resolve_struct(t, owner) if t else None
    if not got or got[0] != 'embed':
        return None
    inner = struct_fields().get(got[1], {}).get(o - b)
    if not inner or inner[1] == 'table' and how != 'table':
        return None
    return (b, field, how), (got[1], inner[0], inner[1])


def inside_field(q, fs, o):
    """(member offset, member, how) of the embedded struct member of q that
    offset o is inside (past its start, within its type's size) with no
    member of its own there (zge::Matrix's float m[16]); None otherwise."""
    before = [b for b in fs if b < o]
    if not before:
        return None
    b = max(before)
    field, how = fs[b]
    owner = field_owner(q, b)
    t = layout_types().get(owner, {}).get(field) if how == 'table' else member_types().get(owner, {}).get(field)
    got = _resolve_struct(t, owner) if t else None
    if not got or got[0] != 'embed':
        return None
    size = struct_sizes().get(got[1]) or PRIMITIVE_SIZES.get(got[1])
    if not size or o - b >= size or struct_fields().get(got[1], {}).get(o - b):
        return None
    return b, field, how


def _field_type_of(rhs, typed):
    """The struct a right-hand side's value points at when it is a typed
    base's field: the address of an embedded struct member, or a pointer
    member's value; None otherwise."""
    kind = 'embed'
    expr = rhs
    m = re.match(r'^' + CAST + r'(?:\*ee::at|ee::io::loadAny)<\s*(?:u?int32_t|ee::Ptr<[\w:]+>)\s*>\(\s*(.*?)\s*\)\)*$', rhs)
    if m:
        kind, expr = 'ptr', m.group(1)
    m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?\)*$', expr)
    if not m or m.group(1) not in typed:
        return None
    q, _short, fs = typed[m.group(1)]
    o = int(m.group(2), 0)
    f = fs.get(o)
    if not f:
        return None
    owner = field_owner(q, o)
    t = layout_types().get(owner, {}).get(f[0])
    if t:
        got = ('ptr', t[:-1]) if t.endswith('*') else ('embed', t)
        return got[1] if got[0] == kind and struct_fields().get(got[1]) else None
    if f[1] == 'table':
        return None
    t = member_types().get(owner, {}).get(f[0])
    got = _resolve_struct(t, owner) if t else None
    if not got or got[0] != kind:
        return None
    return got[1]


_CONSTANT_NAMES = set()


def _constant_names():
    """The names callee and data constants can have in a draft: kName of
    every swept function's name, and the hand-written constants' names."""
    if not _CONSTANT_NAMES:
        _CONSTANT_NAMES.add(None)
        for a, (n, _f) in state()['functions'].items():
            if n and not machine_function_name(n):
                _CONSTANT_NAMES.add(constant_for(n))
        game_constant_names(0)
        for a, ns in _GAME_CONSTANTS.items():
            if a is not None and isinstance(ns, set):
                _CONSTANT_NAMES.update(ns)
    return _CONSTANT_NAMES


_LAYOUT_TYPES = {}


def layout_types():
    """{struct: {field: member type}} of readable_names_layout.tsv's `type`
    rows (`STRUCT type FIELD=TYPE`): a layout field that is an embedded
    struct (EditCamera's eye a zge::Vec4), so an offset inside it is named
    as the field's constant plus the inner struct's member; `FIELD=TYPE*`
    a pointer to one (a Prop's definition: the values loaded from it are
    typed so)."""
    # (its own cache: a key in _STATE would make state() think it had run)
    if not _LAYOUT_TYPES:
        out = {}
        if os.path.exists(LAYOUT):
            for line in open(LAYOUT, encoding='utf-8'):
                c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
                m = re.match(r'^([a-z]\w*)=([\w:]+\*?)$', c[2]) if c[1] == 'type' and c[3] in FUNCTION_USED else None
                if m and not line.startswith('#'):
                    out.setdefault(c[0], {})[m.group(1)] = m.group(2)
        _LAYOUT_TYPES[None] = out
    return _LAYOUT_TYPES[None]


def layout_bases():
    """{struct: base struct} of readable_names_layout.tsv's `base` rows."""
    out = {}
    if os.path.exists(LAYOUT):
        for line in open(LAYOUT, encoding='utf-8'):
            c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
            if not line.startswith('#') and c[1] == 'base' and c[3] in USED:
                out[c[0]] = c[2]
    return out


_LOW_FIELDS = set()   # (struct, offset) of the low-confidence layout rows


def layout_rows():
    """[(struct, offset, field)] of readable_names_layout.tsv's rows: struct
    (qualified), offset, field, confidence, evidence. Every confidence (a
    low one's constant says so in its comment)."""
    rows = []
    if os.path.exists(LAYOUT):
        for line in open(LAYOUT, encoding='utf-8'):
            if not line.strip() or line.startswith('#'):
                continue
            c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
            if c[1] in ('base', 'name', 'size', 'result', 'type'):
                continue
            if c[3] in FUNCTION_USED and re.match(r'^[A-Za-z_][\w:]*$', c[0]) and re.match(r'^[a-z]\w*$', c[2]):
                rows.append((c[0], int(c[1], 0), c[2]))
                if c[3] == 'low':
                    _LOW_FIELDS.add((c[0], int(c[1], 0)))
    return rows


def layout_names():
    """{variable name: struct} of readable_names_layout.tsv's `name` rows
    (the structs `infer` recovered for the variables of one name)."""
    out = {}
    if os.path.exists(LAYOUT):
        for line in open(LAYOUT, encoding='utf-8'):
            c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
            if not line.startswith('#') and c[1] == 'name' and c[3] in FUNCTION_USED:
                out[c[2]] = c[0]
    return out


def layout_results():
    """{(callee address, argument number, argument value): (struct, variable
    name)} of readable_names_layout.tsv's `result` rows (`STRUCT result
    CALLEE:N=VALUE`): a call of CALLEE whose Nth argument is VALUE returns a
    STRUCT (editObjectPart(object, "EditRailTool"): the object's rail tool
    part), its variable named as STRUCT's first `name` row says."""
    if 'results' not in _STATE:
        names, out = {}, {}
        if os.path.exists(LAYOUT):
            for line in open(LAYOUT, encoding='utf-8'):
                c = [x.strip() for x in line.rstrip('\n').split('\t')] + [''] * 5
                if line.startswith('#') or c[3] not in FUNCTION_USED:
                    continue
                if c[1] == 'name':
                    names.setdefault(c[0], c[2])
                m = re.match(r'^0x([0-9A-Fa-f]+):(\d+)=0x([0-9A-Fa-f]+)$', c[2]) if c[1] == 'result' else None
                if m:
                    out[(int(m.group(1), 16), int(m.group(2)), int(m.group(3), 16))] = c[0]
        _STATE['results'] = {k: (q, names.get(q) or lower_camel(q.split('::')[-1])) for k, q in out.items()}
    return _STATE['results']


def _result_of(rhs, consts):
    """(struct, variable name, callee, argument number, argument text) of a
    right-hand side that is a call a `result` row types (layout_results);
    consts: the draft's {constant name: address}. None for any other."""
    results = layout_results()
    if not results:
        return None
    m = re.match(r'^(?:static_cast<[\w:]+>\()?ee::call(?:Suspendable)?(?:<[^()]*?>)?\(\s*(k\w+)\s*,(.*)\)\)?$', rhs)
    if not m or m.group(1) not in consts:
        return None
    a = consts[m.group(1)]
    for i, arg in enumerate(split_args(m.group(2)), 1):
        x = re.sub(r'^static_cast<uint32_t>\((.*)\)$', r'\1', arg.strip())
        m2 = re.match(r'^0x([0-9A-Fa-f]{8})u$', x)
        v = int(m2.group(1), 16) if m2 else consts.get(x)
        hit = v is not None and results.get((a, i, v))
        if hit:
            return hit[0], hit[1], m.group(1), i, x
    return None


def _commented_members(raw, text):
    """{qualified struct: {offset: {member}}} of a header's top-level structs
    (not nested in another struct, not templates, without a base): members
    whose same-line comment, or the comment block just before, starts with
    their offset (`// +0x014`)."""
    out = {}
    for key, name, _type, off in _struct_decls(raw, text):
        if off is not None and not name.startswith('_'):
            out.setdefault(key, {}).setdefault(off, set()).add(name)
    return out


def _struct_decls(raw, text):
    """[(qualified struct, member, its declared type as written, offset or
    None)] of a header's top-level structs' data members (the structs
    _commented_members reads; the offset from the member's comment)."""
    out = []
    for m in re.finditer(r'(?<![\w>])struct\s+(?:alignas\(\d+\)\s+)?(\w+)\s*\{', text):
        before = text[:m.start()]
        if re.search(r'template\s*<[^;{}]*$', before):
            continue
        spaces = _namespace_at(text, m.start())
        depth = before.count('{') - before.count('}')
        if depth != len(spaces):
            continue   # nested in a struct, class or function
        ns = '::'.join(n for n in spaces if n != '(anonymous)')
        key = (ns + '::' if ns else '') + m.group(1)
        i, d = m.end(), 1
        start = i
        while i < len(text) and d:
            d += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        body_code = text[start:i - 1].split('\n')
        body_raw = raw[start:i - 1].split('\n')
        seq = _sequential_offsets(text[start:i - 1])
        if seq is not None and not re.search(r'//\s*\+0x[0-9A-Fa-f]+', raw[start:i - 1]):
            # no member's comment gives an offset: a plain struct's own layout
            # (ee::Float x, y, z, w;), each checked by the draft's static_assert
            for name, t, off in seq:
                out.append((key, name, t, off))
            continue
        level = 0
        pending = None
        for c, r in zip(body_code, body_raw):
            stripped = r.strip()
            if level == 0 and stripped.startswith('//'):
                mm = re.match(r'^//\s*\+0x([0-9A-Fa-f]+)\b', stripped)
                if mm:
                    pending = int(mm.group(1), 16)
                continue
            if level == 0 and c.strip():
                dm = re.match(r'^\s*(?!static\b|constexpr\b|using\b|typedef\b|enum\b|struct\b|union\b|friend\b)'
                              r'([\w:<>,\s\*&]+?[\s\*&])([A-Za-z]\w*)\s*(?:\[[^\]]*\])?\s*;\s*$', c)
                own = re.search(r'//\s*\+0x([0-9A-Fa-f]+)\b', r)
                # (`ee::Float du, dv; // +0x0C`: each at its own offset)
                many = re.match(r'^\s*(?!static\b|constexpr\b|using\b|typedef\b|enum\b|struct\b|union\b|friend\b)'
                                r'([\w:]+(?:<[\w:]+>)?)\s+([A-Za-z]\w*(?:\s*,\s*[A-Za-z]\w*)+)\s*;\s*$', c)
                if many:
                    off = int(own.group(1), 16) if own else pending
                    t = many.group(1)
                    size = 4 if t.startswith('ee::Ptr<') else SEQ_SIZES.get(t) or SEQ_SIZES.get(t.split('::')[-1])
                    for k, n in enumerate(x.strip() for x in many.group(2).split(',')):
                        out.append((key, n, t, off + k * size if off is not None and size else None))
                elif dm and '(' not in c and ':' not in c.replace('::', ''):
                    off = int(own.group(1), 16) if own else pending
                    out.append((key, dm.group(2), re.sub(r'\s+', ' ', dm.group(1)).strip(), off))
                pending = None
            level += c.count('{') - c.count('}')
    return out


SEQ_SIZES = {'uint8_t': 1, 'int8_t': 1, 'char': 1, 'bool': 1, 'uint16_t': 2, 'int16_t': 2, 'Angle': 2, 'uint32_t': 4,
             'int32_t': 4, 'int': 4, 'unsigned': 4, 'float': 4, 'ee::Float': 4, 'uint64_t': 8, 'int64_t': 8}


def _sequential_offsets(body):
    """[(member, type, offset)] of a struct body (comments blanked) whose
    data members are all of the plain types SEQ_SIZES knows (ee::Ptr<T> 4
    bytes), arrays of them included, each at its natural alignment: the
    offsets the compiler gives them; None when a member is anything else
    (a nested struct, a function, a bit field, a static)."""
    out, off = [], 0
    text = re.sub(r'\s+', ' ', body).strip()
    if not text or '{' in text or '(' in text or ':' in text.replace('::', ''):
        return None
    for decl in [d.strip() for d in text.split(';') if d.strip()]:
        m = re.match(r'^((?:const )?(?:ee::Ptr<[\w:]+>|[\w:]+(?: [\w:]+)?))\s+(.*)$', decl)
        if not m or re.match(r'^(?:static|constexpr|using|typedef|enum|struct|union|friend|template)\b', decl):
            return None
        t = m.group(1).replace('const ', '')
        size = 4 if t.startswith('ee::Ptr<') else SEQ_SIZES.get(t) or SEQ_SIZES.get(t.split('::')[-1])
        if not size:
            return None
        for d in [x.strip() for x in m.group(2).split(',')]:
            dm = re.match(r'^([A-Za-z]\w*)((?:\s*\[\s*\d+\s*\])*)$', d)
            if not dm:
                return None
            n = 1
            for k in re.findall(r'\d+', dm.group(2)):
                n *= int(k)
            off = (off + size - 1) // size * size
            out.append((dm.group(1), t, off))
            off += size * n
    return out or None


def _code_only(text):
    """text with comments and literals blanked, its length kept."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text, flags=re.S)


def _namespace_at(text, pos):
    """The namespaces open at pos (outermost first)."""
    stack, depth = [], 0
    for m in re.finditer(r'namespace\s+([\w:]*)\s*\{|\{|\}', text[:pos]):
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
    return [n for _, n in stack]


def name_fields(code, defs, d=None):
    """Reads at a raw offset from a variable of a known struct type
    (`ee::at<int32_t>(player + 0x8C8)`, the variable wrapped as
    ee::Ptr<aggressive::skater::Player> somewhere in the function, never
    reassigned) as `player + offsetof(aggressive::skater::Player, pendingScore)`
    (a member: d.names_asserts gets the static_assert that checks its offset
    unless one in the sources proves it) or `player + kPlayerTrickSlots` (the
    layout table's name of padding: d.names_field_constants gets the
    constant): the same address, the field's name. Returns the new code lines."""
    typed = _typed_vars(code, defs, d, by_name=d is not None, address=getattr(d, 'address', None))
    if not typed:
        return code
    code, nested = _hoist_nested(code, typed, d)
    pat = re.compile(r'(\bee::(?:at|io::loadAny|io::storeAny)<[^<>()]+(?:<[^<>()]+>)?>\(\s*)(%s)(\s*\+\s*)(0x[0-9A-Fa-f]+|\d+)u?(\s*[),])' %
                     '|'.join(re.escape(v) for v in sorted(typed, key=len, reverse=True)))

    def member_ref(owner, q, o, field, how):
        """offsetof(owner, field), its header and its check noted; None when
        its header is not one."""
        if owner != q or how == 'member':
            heads = _FIELD_HEADERS.get(owner, ())
            if owner != q and (len(heads) != 1 or d is None):
                return None
            if owner != q:
                d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
        if how == 'member':
            d.__dict__.setdefault('names_asserts', set()).add(
                'static_assert(offsetof(%s, %s) == 0x%X); // (its member\'s offset comment)' % (owner, field, o))
        return 'offsetof(%s, %s)' % (owner, field)

    def table_constant(owner, o, field):
        """The batch file's constant of a layout field (kEditCameraEye), None
        when another offset has its name."""
        if d is None:
            return None
        name = 'k' + owner.split('::')[-1] + field[:1].upper() + field[1:]
        if name in _constant_names():
            name += 'Offset'
        consts = d.__dict__.setdefault('names_field_constants', {})
        if consts.get(name, (o,))[0] != o:
            return None
        consts[name] = (o, '%s +0x%X (readable_names_layout.tsv%s)' % (owner, o, '; its name is a low-confidence guess'
                                                                     if (owner, o) in _LOW_FIELDS else ''))
        return name

    def sub(m):
        q, short, fs = typed[m.group(2)]
        o = int(m.group(4), 0)
        f = fs.get(o)
        if not f and d is not None:
            # Inside an embedded struct member: both offsets named,
            # offsetof(Player, controls) + offsetof(Controls, stickX).
            emb = embedded_field(q, fs, o)
            heads = _FIELD_HEADERS.get(emb[1][0], ()) if emb else ()
            if emb and (len(heads) == 1 or emb[1][2] == 'table'):
                (b, bfield, bhow), (s, sfield, show) = emb
                outer = (table_constant(field_owner(q, b), b, bfield) if bhow == 'table' else
                         member_ref(field_owner(q, b), q, b, bfield, bhow))
                if outer:
                    if heads:
                        d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
                    inner = (table_constant(field_owner(s, o - b), o - b, sfield) if show == 'table' else
                             member_ref(field_owner(s, o - b), s, o - b, sfield, show))
                    if inner:
                        return '%s%s%s%s + %s%s' % (m.group(1), m.group(2), m.group(3), outer, inner, m.group(5))
            # Inside an embedded struct member with no member there (a
            # matrix's words, copied two at a time): the member and the rest,
            # `editCraneTool + kEditCraneToolPartArmRestMatrix + 0x8`.
            inside = inside_field(q, fs, o)
            if inside:
                b, bfield, bhow = inside
                outer = (table_constant(field_owner(q, b), b, bfield) if bhow == 'table' else
                         member_ref(field_owner(q, b), q, b, bfield, bhow))
                if outer:
                    return '%s%s%s%s + 0x%X%s' % (m.group(1), m.group(2), m.group(3), outer, o - b, m.group(5))
            return m.group(0)
        if not f or d is None and f[1] != 'proven':
            return m.group(0)
        field, how = f
        owner = field_owner(q, o)
        if how == 'table' or not _FIELD_HEADERS.get(owner) and _FIELD_SOURCES.get(owner):
            # The layout table's name of padding, or a field of a struct one
            # source file alone defines (no draft can see it): a constant.
            if d is None:
                return m.group(0)
            name = 'k' + owner.split('::')[-1] + field[:1].upper() + field[1:]
            if name in _constant_names():
                name += 'Offset'   # (a function's constant has the name: kFollowerSpeed is followerSpeed's)
            consts = d.__dict__.setdefault('names_field_constants', {})
            if consts.get(name, (o,))[0] != o:
                return m.group(0)
            consts[name] = (o, ('%s +0x%X (readable_names_layout.tsv%s)' % (owner, o, '; its name is a low-confidence guess'
                                                                         if (owner, o) in _LOW_FIELDS else '')) if how == 'table'
                            else ('%s::%s +0x%X (%s)' % (owner, field, o, _FIELD_SOURCES[owner])))
            return '%s%s%s%s%s' % (m.group(1), m.group(2), m.group(3), name, m.group(5))
        if owner != q:
            # a base's member: its header (one) included
            heads = _FIELD_HEADERS.get(owner, ())
            if len(heads) != 1 or d is None:
                return m.group(0)
            d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
        if how == 'member':
            d.__dict__.setdefault('names_asserts', set()).add(
                'static_assert(offsetof(%s, %s) == 0x%X); // (its member\'s offset comment)' % (owner, field, o))
        return '%s%s%soffsetof(%s, %s)%s' % (m.group(1), m.group(2), m.group(3), owner, field, m.group(5))
    # (a quadword access's aligned address: `(matrix + 0x10 & ~0xFu)`)
    quad = re.compile(r'(\bee::(?:at|io::loadAny|io::storeAny)<[^<>()]+(?:<[^<>()]+>)?>\(\s*\()(%s)(\s*\+\s*)(0x[0-9A-Fa-f]+|\d+)u?(\s*&\s*~0xFu\))' %
                      '|'.join(re.escape(v) for v in sorted(typed, key=len, reverse=True)))
    out = [quad.sub(sub, pat.sub(sub, c)) for c in code]
    if d is not None:
        # The address of a field handed on (defineProperty(widget + 0x108, ...)):
        # `widget + kGuiButtonIsDisabledProperty`, the same number named; only
        # where the sum stands alone (an argument, a value set, a return).
        bare = re.compile(r'()(?<![\w.>])(%s)(\s*\+\s*)(0x[0-9A-Fa-f]+|\d+)u?()(?=\s*[,);])' %
                          '|'.join(re.escape(v) for v in sorted(typed, key=len, reverse=True)))

        def bare_sub(m):
            before = m.string[:m.start(2)].rstrip()
            if before and (before[-1] not in '(,=' or before.endswith(('==', '!=', '<=', '>='))) and \
                    not before.endswith('return'):
                return m.group(0)
            return sub(m)
        out = [bare.sub(bare_sub, c) for c in out]
    if nested:
        back = re.compile('|'.join(sorted(nested, key=len, reverse=True)))
        for _ in range(4):
            out = [back.sub(lambda m: nested[m.group(0)], c) if '__nested' in c else c for c in out]
    if d is not None and getattr(d, 'names_typed_includes', None) and out != code:
        d.includes.update(d.names_typed_includes)
    return out


NESTED_BASE = re.compile(r'\*ee::at<(?:u?int32_t)>\(\s*([A-Za-z_]\w*\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?)\s*\)(?=\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?\s*[),])|'
                         r'\*(g_\w+)(?=\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?\s*[),])')


def _hoist_nested(code, typed, d):
    """(code, {placeholder: text}) with a typed pointer read inside an access
    (`*ee::at<int32_t>(*ee::at<int32_t>(camera + 0x10) + 0x54)`, `*g_topState + 0x10`)
    replaced by placeholders of that struct type in `typed`, so that the offset
    on it is named as any variable's; the caller puts the text back."""
    if d is None:
        return code, {}
    gtypes = _global_types(d)
    fields = struct_fields()
    nested = {}

    def sub(m):
        if m.group(2):
            q = gtypes.get(m.group(2))
        else:
            q = _field_type_of('*ee::at<int32_t>(%s)' % m.group(1), typed)
        if not q or not fields.get(q):
            return m.group(0)
        n = '__nested%d' % len(nested)
        nested[n] = m.group(0)
        typed[n] = (q, q.split('::')[-1], fields[q])
        heads = _FIELD_HEADERS.get(q, ())
        if len(heads) == 1:
            d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
        return n
    for _ in range(3):
        new = [NESTED_BASE.sub(sub, c) if '*' in c else c for c in code]
        if new == code:
            break
        code = new
    return code, nested


# ---------------------------------------------------------------------------
# Globals inside a hand-written record or array
# ---------------------------------------------------------------------------

_RECORDS = {}
_TABLE_RECORDS = set()   # the records the table declares (no hand-written global at their address)

PRIMITIVE_SIZES = {'uint8_t': 1, 'int8_t': 1, 'char': 1, 'bool': 1, 'uint16_t': 2, 'int16_t': 2, 'uint32_t': 4,
                   'int32_t': 4, 'ee::Float': 4, 'float': 4, 'uint64_t': 8, 'int64_t': 8, 'Vec4': 16, 'zge::Vec4': 16,
                   'Quaternion': 16, 'zge::Quaternion': 16, 'Matrix': 64, 'zge::Matrix': 64}


def record_globals():
    """{base address: (name, element type, element size, count)} of the
    hand-written game globals (not the runtime's, not DM2's) of a record or an
    array type whose size is known (a static_assert(sizeof(T) == N), or a
    primitive): the globals a draft's address inside one is named after."""
    if not _RECORDS:
        _RECORDS[None] = None
        sizes = {}
        consts = {}
        globs = []
        spat = re.compile(r'static_assert\(\s*sizeof\(\s*(\w+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)')
        cpat = re.compile(r'constexpr\s+(?:u?int\d+_t|int|unsigned|size_t)\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\s*;')
        gpat = re.compile(r'ee::Global<\s*(.+?)\s*,\s*0x([0-9A-Fa-f]+)u?\s*>\s*(\w+)\s*;')
        for root, dirs, files in zaxis_paths.walk(GAME_SRC):
            dirs.sort()
            rel = zaxis_paths.key_of(root, GAME_SRC).split(os.sep)
            if rel[0] in ('ee', 'dm2') or os.path.basename(root) in ('drafts', 'sweep'):
                dirs[:] = []
                continue
            for fn in sorted(files):
                if not fn.endswith(('.cpp', '.h')):
                    continue
                text = _code_only(readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()))
                for m in spat.finditer(text):
                    sizes.setdefault(m.group(1), set()).add(int(m.group(2), 0))
                for m in cpat.finditer(text):
                    consts.setdefault(m.group(1), set()).add(int(m.group(2), 0))
                for m in gpat.finditer(text):
                    globs.append((int(m.group(2), 16), m.group(3), m.group(1)))
        for a, name, t in globs:
            mm = re.match(r'^(?:const\s+)?([\w:]+)\s*(?:\[\s*(\w+)\s*\])?$', t)
            if not mm:
                continue
            base, count = mm.group(1), mm.group(2)
            short = base.split('::')[-1]
            size = PRIMITIVE_SIZES.get(base) or PRIMITIVE_SIZES.get(short)
            if size is None and len(sizes.get(short, ())) == 1:
                size = next(iter(sizes[short]))
            if size is None and not count:
                # no sizeof: the extent of its known fields (a field past
                # them has no name to give anyway)
                quals = [q for q in struct_fields() if q and (q == base or q.endswith('::' + short))]
                if len(quals) == 1 and struct_fields()[quals[0]]:
                    size = max(struct_fields()[quals[0]]) + 4
            n = 1
            if count:
                if re.match(r'^(?:0x[0-9A-Fa-f]+|\d+)$', count):
                    n = int(count, 0)
                elif len(consts.get(count, ())) == 1:
                    n = next(iter(consts[count]))
                else:
                    continue
            if not size or (n == 1 and size in (1, 2, 4, 8)):
                continue
            if a in _RECORDS and (_RECORDS[a] is None or _RECORDS[a][0] != name):
                _RECORDS[a] = None   # two names or more: none
                continue
            _RECORDS[a] = (name, base, size, n)
        # The table's `record` rows (`record 0x0027E2D8:ee::Float:32 g_savedFloatRegisters`):
        # an array or record no hand-written code declares; one the sources do wins.
        for kind, k, name, c, _ in read_table():
            if kind != 'record' or c not in FUNCTION_USED:
                continue
            a, t, count = k.split(':', 1)[0], k.split(':', 1)[1].rsplit(':', 1)[0], k.rsplit(':', 1)[1]
            a = int(a, 16)
            size = PRIMITIVE_SIZES.get(t) or struct_sizes().get(t)
            if size and a not in _RECORDS:
                _RECORDS[a] = (name, t, size, int(count))
                _TABLE_RECORDS.add(a)
    return {a: r for a, r in _RECORDS.items() if a is not None and r}


def record_global_name(address):
    """g_<record><Field> / g_<array><index>[<Field>] for an address inside a
    hand-written record or array global (g_pads + 0x84: g_pads1 when
    PadState is 0x84 bytes; a field when its layout names the offset);
    None when the offset in the element has no name."""
    best = None
    for a, (name, t, size, n) in record_globals().items():
        if a < address < a + size * n or a == address and a in _TABLE_RECORDS:
            best = (a, name, t, size, n)
            break
    if not best:
        return None
    a, name, t, size, n = best
    index, rest = divmod(address - a, size)
    field = None
    if rest:
        short = t.split('::')[-1]
        quals = [q for q in struct_fields() if q and (q == t or q.endswith('::' + short))]
        if len(quals) != 1:
            return None
        f = struct_fields()[quals[0]].get(rest)
        if not f:
            return None
        field = f[0]
    base = name if name.startswith('g_') else 'g_' + name
    out = base + (str(index) if n > 1 else '') + ((field[:1].upper() + field[1:]) if field else '')
    return out if re.match(r'^g_\w+$', out) else None


def data_constant_name(ws, address):
    """The name of a fixed data address: a hand-written game constant of it
    (`kMeshPacket = 0x0027A9D0u`), else kName of the hand-written global there."""
    known = [k for k in ws.constants.get(address) or () if k[0] in game_constant_names(address)]
    if known:
        counts = {}
        for n, _ in known:
            counts[n] = counts.get(n, 0) + 1
        best = sorted(counts, key=lambda n: (-counts[n], [k[0] for k in known].index(n)))[0]
        if re.match(r'^k[A-Z]\w*$', best) and not re.match(r'^kFun[0-9A-F]{6}', best):
            return best
    g = global_name(address, ws.globals.get(address))
    if g and g.startswith('g_') and len(g) > 2:
        return 'k' + g[2].upper() + g[3:]
    return None


# ---------------------------------------------------------------------------
# Renaming a rendered draft
# ---------------------------------------------------------------------------

IDENT = re.compile(r'\b[A-Za-z_]\w*\b')


def code_part(line):
    """(code, comment) of a line: the comment from the first // outside a literal."""
    q = None
    i = 0
    while i < len(line):
        ch = line[i]
        if q:
            if ch == '\\':
                i += 2
                continue
            if ch == q:
                q = None
        elif ch in '"\'':
            q = ch
        elif line.startswith('//', i):
            return line[:i], line[i:]
        elif line.startswith('/*', i):
            j = line.find('*/', i + 2)
            if j < 0:
                return line[:i], line[i:]
            # a /*name*/ unused parameter: renamed with the code
            i = j + 2
            continue
        i += 1
    return line, ''


def calls_in(code):
    """[(callee, [args])] of the calls in a line's code: ee::call / tailCall
    through a constant (callee 'kName') and game::call<&ns::fn> (callee
    'ns::fn'), and direct calls of qualified readable functions."""
    out = []
    for m in re.finditer(r'\bee::(?:call|tailCall|callSuspendable)(?:<[^()]*?>)?\(\s*(k\w+)\s*(,|\))', code):
        start = m.end() if m.group(2) == ',' else None
        args = []
        if start is not None:
            args = split_args(_balanced(code, start))
        out.append((m.group(1), args))
    for m in re.finditer(r'\bgame::(?:call|tailCall|callAt)<&([\w:]+)>\(', code):
        out.append((m.group(1), split_args(_balanced(code, m.end()))))
    return out


def _balanced(text, start):
    depth = 1
    i = start
    while i < len(text):
        ch = text[i]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    return text[start:]


def signature_params(sig):
    """The parameter names of a C++ signature (None for an unnamed one)."""
    m = re.search(r'\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:const\s*)?$', sig.strip().rstrip('{').strip())
    if not m:
        return None
    names = []
    for p in split_args(m.group(1)):
        p = re.sub(r'=.*$', '', p).strip()
        mm = re.search(r'([A-Za-z_]\w*)\s*$', p)
        names.append(mm.group(1) if mm and len(p.split()) > 1 else None)
    return names


def readable_signature(ws, qualified):
    """The parameter names of a readable function by its qualified name (the
    lookup of its entry is a fact of the draft cache)."""
    index = getattr(ws, '_names_index', None)
    if index is None:
        index = {}
        for a, r in dict.items(ws.readable):
            if r.get('kind') == 'PLANNED' or not r.get('signature'):
                continue
            ns = r.get('namespace') or ''
            index.setdefault((ns + '::' + r['name']) if ns else r['name'], a)
        ws._names_index = index
    a = index.get(qualified)
    if a is None:
        return None
    r = ws.readable.get(a)
    if not r or not r.get('file'):
        return None
    # The names are in the definition (readable_draft.py keeps the types only).
    texts = getattr(ws, '_names_texts', None)
    if texts is None:
        texts = ws._names_texts = {}
    if r['file'] not in texts:
        try:
            texts[r['file']] = readable_symbols.literal_view(open(r['file'], encoding='utf-8', errors='replace').read())
        except OSError:
            texts[r['file']] = ''
    best = None
    for m in re.finditer(r'(?:^|\n)[^\n;{}]*?\b' + re.escape(r['name']) +
                         r'\s*(\([^()]*(?:\([^()]*\)[^()]*)*\))\s*(?:const\s*)?\{', texts[r['file']]):
        best = m
    if not best:
        return None
    names = signature_params(best.group(1))
    if names is not None and r['signature'] and len(names) != len(r['signature'][1]):
        return None
    return _without_ordinals(names)


ORDINALS = ('first', 'second', 'third', 'fourth', 'fifth', 'sixth', 'seventh', 'eighth')


def _without_ordinals(names):
    """names with a run of three ordinals or more (formatString's first ..
    sixth: a variadic callee's argument slots, not what is passed) blanked:
    pcfsOpen's name and the registers it hands on were named first .. sixth,
    and the names went on up through its callers."""
    if not names:
        return names
    out = list(names)
    i = 0
    while i < len(out):
        j = i
        while j < len(out) and out[j] in ORDINALS:
            j += 1
        if j - i >= 3:
            out[i:j] = [None] * (j - i)
        i = max(j, i + 1)
    return out


def constant_signatures(d):
    """{constant name: (parameter names, result name)} of a draft's callee constants
    whose hand-written constant has a comment saying so."""
    out = {}
    for address, (name, _) in d.constants.items():
        known = d.ws.constants.get(address)
        best = (None, None)
        # the constant's own comment, else (a name unambiguous_constant chose)
        # any hand-written constant's of the address
        for kn, comment in sorted(known or (), key=lambda k: k[0] != name):
            p, r = comment_signature(comment)
            if p or r:
                best = (p, r)
                break
        fn = function_name(address)
        pnames = best[0]
        if not pnames:
            # A swept callee's parameters as the table has them (by position).
            s = state()
            rows = {i: n for (a, i), n in list(s['params'].items()) + list(s['inferred'].items()) +
                    list(s['callers'].items()) if a == address}
            rows.update({int(v[3:]) + 1: n for (a, v), n in s['argparams'].items() if a == address and v.startswith('arg')})
            if rows:
                pnames = [rows.get(i) for i in range(1, max(rows) + 1)]
        out[name] = (pnames, best[1] or noun_of_function(fn or name))
    return out


def _defs_of(code):
    """({variable: assignments}, {variable: [right-hand sides]}) of a draft's
    code: `T v = rhs;` and `v = rhs;` (an increment or a compound assignment
    counts twice: the variable is not set once)."""
    defs = {}
    rhss = {}
    for c in code:
        for m in re.finditer(r'(?<![\w.>:])([A-Za-z_]\w*)\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)', c):
            defs[m.group(1)] = defs.get(m.group(1), 0) + 1
        for m in re.finditer(r'(?:\+\+|--)\s*([A-Za-z_]\w*)|([A-Za-z_]\w*)\s*(?:\+\+|--)', c):
            v = m.group(1) or m.group(2)
            defs[v] = defs.get(v, 0) + 2
    for c in code[1:]:
        m = ASSIGN.match(c)
        if m:
            rhss.setdefault(m.group(1), []).append(m.group(2).strip())
    return defs, rhss


ASSIGN = re.compile(r'^\s*(?:const\s+)?(?:[\w:]+(?:<[\w:,\s*]+>)?\s+)?([A-Za-z_]\w*)\s*=\s*(.*);\s*$')
CONSTANT_RHS = re.compile(r'^(?:static_cast<[\w:]+>\()?-?(?:0x[0-9A-Fa-f]+|\d+)(?:u|ull|ll)?\)?$|^ee::Float\(-?[\d.]+f\)$')
CAST = r'(?:static_cast<[\w:]+>\(|ee::Float\()*'
# The registers the original reads without setting them (ee/abi.h): the
# caller's (ee::callerRegister: the value at entry) and the ones the last call
# left (ee::leftByCall): named for that role, as the readable functions do.
GPR_NAMES = ('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra').split()


def _upper1(s):
    return s[:1].upper() + s[1:]


def _leftover_name(rhs):
    m = re.match(r'^' + CAST + r'ee::leftByCall(High)?\(ee::reg::(\w+)\)\)*$', rhs)
    if m:
        return 'left' + _upper1(m.group(2)) + ('High' if m.group(1) else '')
    m = re.match(r'^' + CAST + r'ee::leftByCallFloat\((\d+)\)\)*$', rhs)
    if m:
        return 'leftF%s' % m.group(1)
    m = re.match(r'^' + CAST + r'ee::callerRegister(High)?\(\s*(\d+)\s*(?:/\*[^*]*\*/)?\s*\)\)*$', rhs)
    if m and int(m.group(2)) < 32:
        return 'caller' + _upper1(GPR_NAMES[int(m.group(2))]) + ('High' if m.group(1) else '')
    m = re.match(r'^' + CAST + r'ee::callerFloat\((\d+)\)\)*$', rhs)
    if m:
        return 'callerF%s' % m.group(1)
    return None


EMBEDDED_FIRST = {'zge::Vec3', 'zge::Vec4', 'zge::Matrix'}


def _typed_vars(code, defs, d=None, by_name=True, address=None):
    """{variable: (qualified struct, short name, its fields)} of the variables
    of a known struct type: wrapped as ee::Ptr<T> / ee::at<T> somewhere in the
    function and never reassigned, named for a type (NAME_TYPES), or a
    parameter the table's `ptype` rows type (a zgui widget kind's handlers:
    their widget; a more specific type than a base the code wraps it in)."""
    types = {}
    for c in code:
        for m in re.finditer(r'\bee::(?:Ptr|at)<((?:\w+::)*([A-Z]\w*))>\(\s*(?:static_cast<uint32_t>\()?([A-Za-z_]\w*)\)?\s*\)', c):
            # (a float read at offset 0 says nothing of the struct it is the first field of)
            if m.group(1) not in ('ee::Float', 'ee::Qword'):
                types.setdefault(m.group(3), set()).add((m.group(1), m.group(2)))
    fields = struct_fields()
    typed = {}
    for v, ts in types.items():
        if len(ts) == 1 and defs.get(v, 0) <= 1:
            q, short = next(iter(ts))
            if fields.get(q):
                typed[v] = (q, short, fields[q])
    # A variable set once by a call whose argument says what it returns
    # (layout `result` rows: editObjectPart(object, "EditRailTool")).
    if d is not None and code and layout_results():
        consts = {n: a for a, (n, _) in d.constants.items()}
        for v, rs in _defs_of(code)[1].items():
            if len(rs) != 1 or defs.get(v, 0) != 1:
                continue
            hit = _result_of(rs[0], consts)
            if hit:
                typed[v] = (hit[0], hit[0].split('::')[-1], fields.get(hit[0], {}))
    used = {m.group(0) for c in code for m in IDENT.finditer(c)}
    for v, q in NAME_TYPES.items():
        heads = _FIELD_HEADERS.get(q, ())
        if by_name and v in used and v not in types and defs.get(v, 0) <= 1 and fields.get(q) and len(heads) == 1:
            typed[v] = (q, q.split('::')[-1], fields[q])
            if d is not None:
                d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
    # A variable named for a hand-written struct (`collisionWorld`,
    # `chunkReader2`: one struct of that name, not DM2's, with one header),
    # set once, whose reads at an offset fit the struct (one at a field it
    # has, none past its size), is one.
    if by_name:
        nt = name_types()
        sizes = struct_sizes()
        for v in sorted(used):
            q = nt.get(re.sub(r'(?<=[a-zA-Z])\d+$', '', v))
            if not q or v in typed or v in types or defs.get(v, 0) > 1 or _machine(v):
                continue
            offs = {int(m.group(1), 0) for c in code for m in
                    re.finditer(r'\bee::(?:at<[^<>()]+(?:<[^<>()]+>)?>|io::loadAny<[^<>()]+>)\(\s*%s\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?\s*\)'
                                % re.escape(v), c)}
            if not offs & set(fields[q]) or (q in sizes and max(offs) >= sizes[q]):
                continue
            typed[v] = (q, q.split('::')[-1], fields[q])
            if d is not None and _FIELD_HEADERS.get(q):
                d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(_FIELD_HEADERS[q])))
    if address is not None and code:
        # (a row keys a local by the draft's name; rename_draft notes where the
        # names of its last passes took it)
        vtypes = getattr(d, 'names_vtypes', None) or state()['vtypes'].get(address, {})
        for v, q in vtypes.items():
            if v in used and v not in typed and fields.get(q):
                typed[v] = (q, q.split('::')[-1], fields[q])
                heads = _FIELD_HEADERS.get(q, ())
                if d is not None and len(heads) == 1:
                    d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
        names = signature_params(code[0]) or []
        bases = layout_bases()
        for (a, i), q in state()['ptypes'].items():
            if a != address or i > len(names) or not names[i - 1] or not fields.get(q):
                continue
            v = names[i - 1]
            chain, b = set(), q
            while b and b not in chain:
                chain.add(b)
                b = bases.get(b)
            # (a row wins over a vector's or a matrix's wrap: the struct's first member)
            if defs.get(v, 0) <= 1 and (v not in types or {x for x, _ in types[v]} <= chain | EMBEDDED_FIRST):
                typed[v] = (q, q.split('::')[-1], fields[q])
                heads = _FIELD_HEADERS.get(q, ())
                if d is not None and len(heads) == 1:
                    d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
    # A variable handed (unchanged, set once or a parameter) to a swept
    # callee's parameter that a `ptype` row types takes that type (every such
    # call the same, or one base chain's: the most derived).
    consts = {}
    if d is not None:
        consts = {n: a for a, (n, _) in d.constants.items()}
    if consts and code:
        bases = layout_bases()
        handed = {}
        for c in code[1:]:
            if 'ee::' not in c:
                continue
            for callee, args in calls_in(c):
                a = consts.get(callee)
                if a is None:
                    continue
                for i, arg in enumerate(args, 1):
                    q = state()['ptypes'].get((a, i))
                    m = re.match(r'^(?:static_cast<uint32_t>\()?([A-Za-z_]\w*)\)?$', arg.strip())
                    if q and m and fields.get(q):
                        handed.setdefault(m.group(1), set()).add(q)
        for v, qs in handed.items():
            if v in typed or v in types or defs.get(v, 0) > 1 or v in CPP_RESERVED:
                continue
            q = _most_derived(qs, bases)
            if q:
                typed[v] = (q, q.split('::')[-1], fields[q])
                heads = _FIELD_HEADERS.get(q, ())
                if len(heads) == 1:
                    d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
    # A variable set once from a typed base's field takes the member's
    # declared type: the address of an embedded struct (`controls = player +
    # 0x550`, Player's `Controls controls;`) or a pointer member's value
    # (`parent = *ee::at<int32_t>(node + 0x8)`, `ee::Ptr<Node> parent;`), in
    # turn (a field of that one, ...); and one set once from a hand-written
    # global of a pointer type (`ee::Global<ee::Ptr<Player>, ...>`). Its
    # type's header (one) is included.
    if code:
        rhss = _defs_of(code)[1]
        gtypes = _global_types(d) if d is not None else {}
        rtypes = {}
        if d is not None:
            rt = state()['rtypes']
            rtypes = {n: rt[a] for a, (n, _) in d.constants.items() if a in rt and fields.get(rt[a])}
            rtypes.update(_hand_results(d.ws))
        # What a function returns (its `rtype` row) and what is stored in a
        # pointer field of a typed base (`*ee::at<int32_t>(body + 0x388) = v`)
        # are of that type, whatever set them (an allocator's result, ...).
        given = {}
        rtype = state()['rtypes'].get(address) if address is not None else None
        if rtype and fields.get(rtype):
            for c in code[1:]:
                m = re.match(r'^\s*return\s+' + CAST + r'([A-Za-z_]\w*)\)*\s*;\s*$', c)
                if m:
                    given[m.group(1)] = rtype
        stores = []
        for c in code[1:]:
            if c.lstrip().startswith('*ee::at<'):
                m = STORE.match(c)
                if m and not _machine_constant(m.group(2)):
                    stores.append((m.group(1), m.group(2)))
        for _ in range(6):
            grown = False
            if stores or given:
                # (`dst2 = dst; return dst2;`: the value's type, whichever copy of it)
                for v, q in list(given.items()):
                    rs = [r for r in rhss.get(v, ()) if not re.match(r'^(?:static_cast<[\w:]+>\()?0u?\)?$', r)]
                    m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*$', rs[0]) if len(rs) == 1 else None
                    if m and not _machine_constant(m.group(1)):
                        given.setdefault(m.group(1), q)
                for addr, v in stores:
                    if v not in typed:
                        q = _field_type_of('*ee::at<int32_t>(%s)' % addr, typed)
                        if q:
                            given.setdefault(v, q)
                for v, q in given.items():
                    if v not in typed and v not in types and defs.get(v, 0) <= 2 and fields.get(q) and not _machine_constant(v):
                        typed[v] = (q, q.split('::')[-1], fields[q])
                        grown = True
            for v, rs in rhss.items():
                if v in typed or v in types or not rs or defs.get(v, 0) != len(rs):
                    continue
                # (set more than once: every value the same type's, or null
                # before it is set: `editor = *g_parkEditorCore` again after a call)
                qs = set()
                own = re.compile(r'\b%s\b' % re.escape(v))
                later = []
                for r in rs:
                    if re.match(r'^(?:static_cast<[\w:]+>\()?0u?\)?$', r) and len(rs) > 1:
                        continue
                    if re.match(r'^%s\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?$' % re.escape(v), r):
                        continue   # (a walk over an array of them)
                    if own.search(r):
                        later.append(r)   # (a walk along a list: `v = v->next`, its type's)
                        continue
                    q = _value_type(r, typed, gtypes, rtypes, rhss, defs)
                    qs.add(q)
                if len(qs) != 1 or None in qs:
                    continue
                if later:
                    q = next(iter(qs))
                    trial = dict(typed)
                    trial[v] = (q, q.split('::')[-1], fields[q])
                    if any(_value_type(r, trial, gtypes, rtypes, rhss, defs) != q for r in later):
                        continue
                q = qs.pop()
                heads = _FIELD_HEADERS.get(q, ())
                if d is not None:
                    layout_only = not heads and not _FIELD_SOURCES.get(q) and \
                        all(how == 'table' for _f, how in fields.get(q, {}).values())
                    if len(heads) != 1 and not (_FIELD_SOURCES.get(q) and not heads) and not layout_only:
                        continue
                    if heads:
                        d.__dict__.setdefault('names_typed_includes', set()).add(next(iter(heads)))
                typed[v] = (q, q.split('::')[-1], fields[q])
                grown = True
            if not grown:
                break
    return typed


_HAND_RESULTS = {}


def _hand_results(ws):
    """{qualified hand-written function: the struct its ee::Ptr<T> result
    points to}, for game::call results."""
    readable = getattr(ws, 'readable', None)
    if not readable:
        return {}
    if not _HAND_RESULTS:
        _HAND_RESULTS[None] = None
        for a, r in dict.items(readable):
            sig = r.get('signature')
            if r.get('kind') == 'PLANNED' or not sig or not isinstance(sig[0], str) or 'Ptr<' not in sig[0]:
                continue
            ns = r.get('namespace') or ''
            q = _resolve_struct(sig[0], (ns + '::x') if ns else 'x')
            if q and q[0] == 'ptr':
                _HAND_RESULTS[(ns + '::' + r['name']) if ns else r['name']] = q[1]
    return {k: v for k, v in _HAND_RESULTS.items() if k}


def _machine_constant(v):
    return bool(re.match(r'^(?:k[A-Z]|g_|frame$)', v))


def _value_type(r, typed, gtypes, rtypes, rhss, defs):
    """The struct a right-hand side's value points at: a typed base's field
    (_field_type_of), a table global's value, a typed callee's result, a copy
    of a typed variable, or an element of a typed array (`base + i * 0x48`,
    the struct's size the stride); None."""
    q = _field_type_of(r, typed)
    if q:
        return q
    m = re.match(r'^' + CAST + r'\*(g_\w+)\)*$', r)
    if m:
        return gtypes.get(m.group(1))
    m = re.match(r'^(?:static_cast<[\w:]+>\()?ee::call(?:Suspendable)?(?:<[^()]*?>)?\(\s*(k\w+)\b', r)
    if m:
        return rtypes.get(m.group(1))
    m = re.match(r'^(?:static_cast<[\w:]+>\()?game::call(?:At)?<&([\w:]+)(?:,\s*\w+)?>\(', r)
    if m:
        return rtypes.get(m.group(1))
    m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*$', r)
    if m:
        hit = typed.get(m.group(1))
        return hit[0] if hit else None
    # (`(i * 0x930) + *ee::at<int32_t>(*g_topState + 0x10)`: an element of an array a field points to)
    m = re.match(r'^(.+?[*]\s*(?:0x[0-9A-Fa-f]+|\d+)u?\)*)\s*\+\s*(\*ee::at<.+\))$', r)
    if m:
        q = _field_type_of(m.group(2), typed)
        if not q:
            mm = re.match(r'^\*ee::at<u?int32_t>\(\*(g_\w+)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?\)$', m.group(2))
            if mm and mm.group(1) in gtypes:
                q2 = gtypes[mm.group(1)]
                q = _field_type_of('*ee::at<int32_t>(gbase__ + %s)' % mm.group(2),
                                   {'gbase__': (q2, q2.split('::')[-1], struct_fields()[q2])})
        size = struct_sizes().get(q) if q else None
        if size and _stride_of(m.group(1), rhss, defs) == size:
            return q
    m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\s*\+\s*(.+?)\)*$', r) or \
        re.match(r'^' + CAST + r'(.+?)\s*\+\s*([A-Za-z_]\w*)\)*$', r)
    if m:
        a, b = m.group(1), m.group(2)
        if a not in typed:
            a, b = b, a
        hit = typed.get(a)
        if hit:
            size = struct_sizes().get(hit[0])
            if size and _stride_of(b, rhss, defs) == size:
                return hit[0]
    return None


def _stride_of(e, rhss, defs):
    """N of `i * N` / `i << k`, directly or in a variable set once."""
    e = e.strip()
    if re.match(r'^[A-Za-z_]\w*$', e) and defs.get(e, 0) == 1 and len(rhss.get(e, ())) == 1:
        e = rhss[e][0]
    m = re.match(r'^.*\*\s*(0x[0-9A-Fa-f]+|\d+)u?\)*$', e)
    if m:
        return int(m.group(1), 0)
    m = re.match(r'^.*<<\s*(\d+)u?\)*$', e)
    if m:
        return 1 << int(m.group(1))
    return None


def _most_derived(qs, bases):
    """The one struct of qs, or the most derived of one base chain; None."""
    qs = set(qs)
    for q in qs:
        chain, b = set(), q
        while b and b not in chain:
            chain.add(b)
            b = bases.get(b)
        if qs <= chain:
            return q
    return None


def _global_types(d):
    """{draft's global name: struct} of the hand-written globals of a pointer
    type the draft reads (`ee::Global<ee::Ptr<Player>, 0x...> g_player`): the
    struct with known fields its type names, looked up from the global's
    namespace outward; the types of all of them at the address agree."""
    out = {}
    gtypes = state()['gtypes']
    for a, g in d.globals.items():
        if a in gtypes and struct_fields().get(gtypes[a]):
            out[g[0]] = gtypes[a]   # (a table global's gtype row)
            continue
        known = [k for k in d.ws.globals.get(a) or () if not re.match(r'^(?:dm2|ee)(?:::|$)', k[2] or '')]
        got = set()
        for n, t, ns in known:
            r = _resolve_struct(t, ((ns or '').replace('(anonymous)::', '').replace('::(anonymous)', '') + '::x').lstrip(':'))
            got.add(r[1] if r and r[0] == 'ptr' else None)
        if len(got) == 1 and None not in got:
            out[g[0]] = got.pop()
    return out


def _field_of(expr, typed):
    """The field name of `base + off` (an address) for a typed base."""
    m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?\)*$', expr)
    if not m or m.group(1) not in typed:
        return None
    q, _short, fs = typed[m.group(1)]
    o = int(m.group(2), 0)
    f = fs.get(o)
    if f:
        return f[0]
    emb = embedded_field(q, fs, o)
    if not emb:
        return None
    # a vector's component is the member's (position + y: positionY)
    if emb[1][1] in ('x', 'y', 'z', 'w'):
        return emb[0][1] + emb[1][1].upper()
    return emb[1][1]


def _load_field(rhs, typed):
    """The field a right-hand side reads: `*ee::at<T>(player + 0x8C8)` (casts
    around it) of a typed base."""
    m = re.match(r'^' + CAST + r'(?:\*ee::at|ee::io::loadAny)<[^<>()]+(?:<[^<>()]+>)?>\(\s*(.*?)\s*\)\)*$', rhs)
    if m:
        return _field_of(m.group(1), typed)
    return None


def _machine(v):
    return bool(PARAM_NAME.match(v) or REGISTER_NAME.match(v))


def rename_draft(ws, d):
    """Renames a rendered draft's parameters and register-named locals in its
    lines (d.lines; the signature is its first line). Two passes: the names
    the calls, the types and the globals give, then (on the renamed code) the
    ones that follow from them: the fields a variable is loaded from or
    stored to, and the variables' roles (a loop's index, an array walk's
    entry, the function's result, a register the original reads unset)."""
    lines = d.lines
    split = [code_part(l) for l in lines]
    code = _fold_pages([c for c, _ in split], d)
    split = [(c, comment) for c, (_, comment) in zip(code, split)]
    s = state()
    if d.address in s['guessed'] and isinstance(getattr(d, 'todo', None), list):
        d.todo.append('its name is a low-confidence guess (readable_names.tsv: %s)' %
                      re.sub(r'\s+', ' ', s['guessed'][d.address])[:150].rstrip())
    used = set()
    for c in code:
        # (a name only ever qualified, ee::block or zge::nameHash, is free)
        used.update(m.group(0) for m in IDENT.finditer(c) if c[max(0, m.start() - 2):m.start()] not in ('::', '->')
                    and c[m.start() - 1:m.start()] != '.')
    used.update(n for n, _ in d.constants.values())
    used.update(e[0] for e in d.globals.values())
    own_file = s['functions'].get(d.address, ('', None))[1]
    taken = set(used) | s['file_names'].get(own_file, set()) | {d.name} | CPP_RESERVED
    csigs = constant_signatures(d)
    const_address = {n: a for a, (n, _c) in d.constants.items()}
    renames = {}
    fallback = {}

    def suggest(target, name, why):
        if name:
            renames.setdefault(target, []).append((name, why))

    def suggest_last(target, name, why):
        if name and target not in fallback:
            fallback[target] = (name, why)

    def rhs_name(rhs):
        """What a right-hand side names its variable: a call's result, a named
        global's value, a named variable's copy; (name, why) or None."""
        hit = _result_of(rhs, const_address)
        if hit:
            return hit[1], 'a %s: the result of %s with argument %d %s' % (hit[0].split('::')[-1], hit[2], hit[3], hit[4])
        mm = re.match(r'^(?:static_cast<[\w:]+>\()?ee::call(?:Suspendable)?(?:<[^()]*?>)?\(\s*(k\w+)\b', rhs)
        if mm:
            n = (csigs.get(mm.group(1)) or (None, None))[1]
            return (n, 'the result of %s' % mm.group(1)) if n else None
        mm = re.match(r'^(?:static_cast<[\w:]+>\()?game::call(?:At)?<&([\w:]+)>\(', rhs)
        if mm:
            n = noun_of_function(mm.group(1).split('::')[-1])
            return (n, 'the result of %s' % mm.group(1)) if n else None
        mm = re.match(r'^(?:static_cast<[\w:]+>\()?\*(g_[A-Za-z]\w*)\)?$', rhs)
        if mm and not re.match(r'^g_[0-9A-F]{6}', mm.group(1)):
            return (mm.group(1)[2:], 'the value of %s' % mm.group(1))
        return None

    # Arguments handed on unchanged, and values wrapped in a type.
    for c in code:
        for callee, args in calls_in(c):
            if callee.startswith('k'):
                pnames = (csigs.get(callee) or (None, None))[0]
            else:
                pnames = readable_signature(ws, callee)
            for i, arg in enumerate(args):
                m = re.match(r'^(?:ee::Ptr<[\w:]+>\(\s*)?([A-Za-z_]\w*)\s*\)?$', arg)
                if not m:
                    continue
                v = m.group(1)
                if not _machine(v):
                    continue
                if pnames and i < len(pnames) and pnames[i] and not PARAM_NAME.match(pnames[i]):
                    suggest(v, pnames[i], 'argument %d of %s' % (i + 1, callee))
        for m in re.finditer(r'\bee::Ptr<([\w:]+)>\(\s*([A-Za-z_]\w*)\s*\)', c):
            t, v = m.group(1).split('::')[-1], m.group(2)
            if _machine(v) and re.match(r'^[A-Z]\w*$', t):
                suggest(v, lower_camel(t), 'an ee::Ptr<%s>' % t)

    # Locals set by a call's result or a named global: once, or every time
    # they are set (a constant aside: the value before the loop or the
    # branch; and the caller's register or the one a call left, the value
    # the register held before it was set).
    defs, rhss = _defs_of(code)
    verb_results = {}   # (named last, when nothing else names them)
    for v, rs in rhss.items():
        if not REGISTER_NAME.match(v) or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        got = [rhs_name(r) for r in real]
        if got and all(got) and len({n for n, _ in got}) == 1:
            if ': the result of' in got[0][1]:
                # what the call returns, said by its argument (a `result` row): decisive
                renames[v] = [(got[0][0], got[0][1])]
                continue
            if got[0][1].startswith('the result of') and got[0][0] in WEAK_NAMES:
                # a general noun (an allocator's heapBlock, nameHash): what it is
                # stored as or handed to names it better (the later passes); last
                verb_results[v] = (got[0][0], got[0][1] + ', a general noun')
                continue
            suggest(v, got[0][0], got[0][1] + ('' if len(rs) == 1 else ' (each time it is set)'))
            continue
        # Set by calls whose results are one thing at different places
        # (firstAnchor, then nextAnchor: a walk's `anchor`; part, part2).
        if got and all(got) and len(got) > 1:
            nouns = {_walk_noun(n) for n, _ in got}
            if len(nouns) == 1 and None not in nouns:
                suggest(v, nouns.pop(), 'the results of %s (each time it is set)' % ', '.join(sorted({n for n, _ in got})))
                continue
        # A callee whose name is a verb's (sendMessage) gives no noun: its
        # result is `sendMessageResult` (each time it is set), the weakest name.
        got = [_result_name(r) for r in real]
        if got and all(got) and len({n for n, _ in got}) == 1:
            verb_results[v] = got[0]

    # The registers the original reads without setting them (they keep that
    # name: a callee's parameter they are handed through to says nothing of
    # what they hold).
    for v, rs in rhss.items():
        if REGISTER_NAME.match(v) and len(rs) == defs.get(v, 0) and rs:
            left = {_leftover_name(r) for r in rs}
            if len(left) == 1 and None not in left:
                renames.pop(v, None)
                suggest_last(v, left.pop(), 'a register the original reads without setting it')

    # The table's parameters (they win), then its callers' names for the rest.
    for (a, i), n in s['params'].items():
        if a == d.address:
            renames[('param_%d' % i)] = [(n, 'readable_names.tsv')]
    for (a, v), n in s['argparams'].items():
        if a == d.address:
            renames[v] = [(n, 'readable_names.tsv')]
    for (a, i), n in s['callers'].items():
        if a == d.address:
            suggest_last('param_%d' % i, n, 'its callers\' argument')


    def first_uses(codes):
        first = {}
        for i, c in enumerate(codes):
            for m in IDENT.finditer(c):
                first.setdefault(m.group(0), i)
        return first

    def choose(renames, fallback, final, used, first):
        for v in sorted(set(renames) | set(fallback), key=lambda v: (not PARAM_NAME.match(v), first.get(v, 0), v)):
            if v in final or v not in used:
                continue
            if v in renames:
                # The most suggested name; on a tie the strongest evidence's
                # (_why_rank: the type's, then what the value is, then a
                # use's), then the first in the function.
                votes, rank, order = {}, {}, {}
                for k, (n, why) in enumerate(renames[v]):
                    votes[n] = votes.get(n, 0) + 1
                    rank[n] = min(rank.get(n, 9), _why_rank(why))
                    order.setdefault(n, k)
                n = sorted(votes, key=lambda n: (-votes[n], rank[n], order[n]))[0]
            else:
                n = fallback[v][0]
            n = _free_name(n, taken, ('j', 'k') if n == 'i' else ())
            if not n:
                continue
            final[v] = n
            taken.add(n)

    final = {}
    choose(renames, fallback, final, used, first_uses(code))

    def apply(codes, names):
        if not names:
            return codes
        # (not a register's name in a comment: callerRegister(19 /* s3 */))
        pat = re.compile(r'(?<![\w.>:])(?<!/\* )(%s)\b' % '|'.join(re.escape(v) for v in sorted(names, key=len, reverse=True)))
        # .low / .high of a qword parameter stay (param_1.low becomes name.low).
        return [pat.sub(lambda m: names[m.group(1)], c) for c in codes]

    # Later passes, on the renamed code (until nothing more is named, four at
    # most): fields, copies, stack slots, types and roles.
    maps = [final]
    code_k = apply(code, final)
    names_k = {final.get(v, v) for v in used}
    for _ in range(8):
        later, later_fb = _later_pass(ws, d, code_k, names_k, csigs)
        fin = {}
        choose(later, later_fb, fin, names_k, first_uses(code_k))
        if not fin:
            break
        maps.append(fin)
        code_k = apply(code_k, fin)
        names_k = {fin.get(v, v) for v in names_k}

    def renamed(v):
        for mp in maps:
            v = mp.get(v, v)
        return v
    # Last, a verb's call result nothing else names: sendMessageResult.
    fb = {renamed(v): got for v, got in verb_results.items() if _machine(renamed(v)) and renamed(v) in names_k}
    # And the halves of a quadword moved through registers (nothing named its address).
    if not os.environ.get('NAMES_NO_ROLES'):
        rhss_k = _defs_of(code_k)[1]
        for v in names_k:
            real = _real_defs(rhss_k.get(v, [])) if _machine(v) else None
            if real and all(re.match(r'^(?:\*ee::at|ee::io::loadAny)<uint64_t>\(\(?.*& ~0xFu\)(?: \+ 8)?\)?$', r) for r in real):
                high = {r.rstrip(')').endswith('+ 8') for r in real}
                if len(high) == 1:
                    fb.setdefault(v, ('quadwordHigh' if high.pop() else 'quadwordLow', 'a half of a quadword'))
    fin = {}
    choose({}, fb, fin, names_k, first_uses(code_k))
    if fin:
        maps.append(fin)
        code_k = apply(code_k, fin)
        names_k = {fin.get(v, v) for v in names_k}
    # A name's hash (nameHash("Chest"), its address a `g_text` row): chestHash.
    if not os.environ.get('NAMES_NO_ROLES'):
        fin = {}
        for v, rs in _defs_of(code_k)[1].items():
            if re.match(r'^(?:nameHash|boneId|hash)\d*$', v) and len(set(rs)) == 1:
                m = re.search(r'\bnameHash>\(ee::Ptr<const char>\((?:static_cast<uint32_t>\()?0x([0-9A-F]{8})u\)?\)\)$', rs[0])
                t = s['globals'].get(int(m.group(1), 16), '') if m else ''
                if t.startswith('g_text') and len(t) > 7:
                    n = _free_name(t[6].lower() + t[7:] + 'Hash', taken)
                    if n:
                        taken.add(n)
                        fin[v] = n
        if fin:
            maps.append(fin)
            code_k = apply(code_k, fin)
            names_k = {fin.get(v, v) for v in names_k}

    # Fixed data addresses written as numbers (ee::at<Vif1Packet>(0x0027A9D0u)):
    # the hand-written name of the address, as a constant of the file.
    literals = {}
    for c in code:
        for m in re.finditer(r'(?<![\w.])0x([0-9A-F]{8})u\b', c):
            a = int(m.group(1), 16)
            if 0x00220000 <= a < 0x02000000 and a not in literals:
                literals[a] = None
    for c in code:
        for m in re.finditer(r'(?<![\w.])0x([0-9A-F]{8})u\b', c):
            if int(m.group(1), 16) in HARDWARE_REGISTERS:
                literals.setdefault(int(m.group(1), 16), None)
    for a in sorted(literals):
        if a in d.constants or ws.generated(a):
            continue
        n = 'k' + HARDWARE_REGISTERS[a] if a in HARDWARE_REGISTERS else data_constant_name(ws, a)
        if not n:
            continue
        base, k = n, 2
        while n in taken:
            n = '%s%d' % (base, k)
            k += 1
        if k > 9:
            continue
        taken.add(n)
        d.constants[a] = (n, 'hardware register' if a in HARDWARE_REGISTERS else 'DAT_%08x' % a)
        literals[a] = n
    literals = {a: n for a, n in literals.items() if n}

    if literals:
        lit = re.compile(r'(?<![\w.])0x(%s)u\b' % '|'.join('%08X' % a for a in literals))
        split = [(lit.sub(lambda m: literals[int(m.group(1), 16)], c), comment) for c, comment in split]
    codes = [c for c, _ in split]
    for mp in maps:
        codes = apply(codes, mp)
    renamed_defs = {renamed(v): n for v, n in defs.items()}
    # A local with a role's name (dst, heapBlock, item2) whose struct is known:
    # the struct's name says more.
    weak = {v: re.sub(r'\d+$', '', v) for v in names_k if ROLE_LOCAL.match(v)}
    extra = {}
    vt = {}
    for v, q in s['vtypes'].get(d.address, {}).items():
        vt[v] = q
        vt[renamed(v)] = q
    if vt:
        d.names_vtypes = vt
    if weak and vt and not os.environ.get('NAMES_NO_ROLES'):
        params = set(signature_params(codes[0]) or ())
        for v in sorted(weak):
            q = vt.get(v)
            if q and v not in params:
                n = _free_name(lower_camel(q.split('::')[-1]), taken)
                if n:
                    extra[v] = n
                    taken.add(n)
    codes = name_fields(codes, renamed_defs, d)
    if extra:
        codes = apply(codes, extra)
        merged_extra = extra
    else:
        merged_extra = {}
    codes = _name_labels(codes, taken)
    out = [c + comment for c, (_, comment) in zip(codes, split)]
    d.lines = out
    d.signature = out[0].strip()
    merged = {}
    for mp in maps + [merged_extra]:
        merged.update(mp)
    d.names_renamed = merged
    if os.environ.get('NAMES_DUMP_RENAMES'):
        # (for readable_names_rekey.py: a vtype row keyed by the register's name outlives any later rename)
        import json
        was = {}
        for mp in maps + [merged_extra]:
            for k, v in mp.items():
                was[v] = was.pop(k, k)
        with open(os.environ['NAMES_DUMP_RENAMES'], 'a', encoding='utf-8') as f:
            f.write(json.dumps({'address': d.address, 'names': was}) + '\n')
    return merged


PAGE_ACCESS = r'(\bee::(?:at|io::loadAny|io::storeAny)<[^<>()]+(?:<[^<>()]+>)?>\(\s*)(%s)(\s*[-+]\s*)(0x[0-9A-Fa-f]+|\d+)u?(?:\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?)?(?=\s*[),])'


def _fold_pages(code, d):
    """The lines with an access through a register that holds a page's high
    half (`*ee::at<int32_t>(s4 - 0x5920)`, s4 set to 0x00270000 and nothing
    else; a page constant's own `kHigh270000 + 0x10`) written at its address,
    which the later passes name: the same address, the thing it is."""
    pages = {n: a for a, (n, _c) in d.constants.items() if not a & 0xFFFF and 0x00220000 <= a < 0x02000000}
    defs, rhss = _defs_of(code)
    held = dict(pages)
    for v, rs in rhss.items():
        got = set()
        for r in rs:
            r = re.sub(r'^static_cast<u?int32_t>\((.*)\)$', r'\1', r.strip())
            if re.match(r'^0u?$', r):
                continue
            m = re.match(r'^0x([0-9A-F]{4})0000u$', r)
            got.add(pages.get(r) or (int(m.group(1), 16) << 16 if m else None))
        if len(got) == 1 and None not in got and defs.get(v, 0) == len(rs) and 0x00220000 <= next(iter(got)) < 0x02000000:
            held[v] = next(iter(got))
    if not held:
        return code
    pat = re.compile(PAGE_ACCESS % '|'.join(re.escape(v) for v in sorted(held, key=len, reverse=True)))

    def sub(m):
        a = held[m.group(2)]
        o = int(m.group(4), 0)
        a += -o if '-' in m.group(3) else o
        if m.group(5):
            a += int(m.group(5), 0)
        return '%s0x%08Xu' % (m.group(1), a)
    value = re.compile(r'(=\s*)(%s)(\s*[-+]\s*)(0x[0-9A-Fa-f]+|\d+)u?(?=\s*;\s*$)' %
                       '|'.join(re.escape(v) for v in sorted(held, key=len, reverse=True)))

    def value_sub(m):
        a = held[m.group(2)]
        o = int(m.group(4), 0)
        return '%s0x%08Xu' % (m.group(1), a - o if '-' in m.group(3) else a + o)
    # (`s6 = kCameraDefaultsHigh + 0xA10`: the address itself)
    return [value.sub(value_sub, pat.sub(sub, c) if 'ee::' in c else c) for c in code]


# The role names of locals rule-made names leave (a value's job, not what it is).
ROLE_LOCAL = re.compile(r'^(?:dst|src|block|item|record|entry|cursor|heapBlock|poolAllocResult|memoryFillResult|memoryCursor|'
                        r'result|value|node|list)\d*$|^[a-z][A-Za-z0-9]*(?:Word|Value|Field|Result)\d*$')
LABEL = re.compile(r'^\s*(at_[0-9a-f]+(?:_likely)?):\s*$')


def _label_name(code, i):
    """What the code after the label on line i does first, as the label's
    name: done (it returns), setHeight (the value it sets), checkSpeed (the
    variable it tests), callDrawPolys (the function it calls), join (where
    paths meet and nothing says more)."""
    for c in code[i + 1:i + 12]:
        s = c.strip()
        if not s or s in ('{', '}') or LABEL.match(c) or s.startswith(('ee::block(', 'ee::calleeSaves(', 'ee::afterCall(', 'ee::loopBack(', '//')):
            continue
        if s.startswith('return'):
            return 'done'
        m = re.match(r'^(?:const\s+)?(?:[\w:]+(?:<[\w:,\s*]+>)?\s+)?\*?(?:g_)?([A-Za-z_]\w*)\s*[-+*/|&^]?=[^=]', s)
        if m and not _machine(m.group(1)) and re.match(r'^[A-Za-z][A-Za-z0-9]*$', m.group(1)) and m.group(1) not in ('frame', 'ee'):
            return 'set' + _upper1(re.sub(r'(?<=[A-Za-z])\d+$', '', m.group(1)))
        m = re.match(r'^if \(\s*' + CAST + r'\*?(?:g_)?([A-Za-z_]\w*)', s)
        if m and not _machine(m.group(1)) and re.match(r'^[A-Za-z][A-Za-z0-9]*$', m.group(1)) and m.group(1) not in ('frame', 'ee'):
            return 'check' + _upper1(re.sub(r'(?<=[A-Za-z])\d+$', '', m.group(1)))
        m = re.search(r'\bgame::call(?:At)?<&(?:\w+::)*(\w+)>\(|\bee::(?:call|tailCall)(?:<[^()]*>)?\(\s*k_?([A-Za-z]\w*)\b', s)
        if m and re.match(r'^[A-Za-z][A-Za-z0-9]*$', m.group(1) or m.group(2)):
            return 'call' + _upper1(m.group(1) or m.group(2))
        # A store: the field (`node + offsetof(zge::Node, flags)`), else what it is stored through.
        m = re.match(r'^\*?(?:ee::at|ee::io::storeAny)<[^()]*>\(\s*([A-Za-z_]\w*)(?:\s*\+\s*(?:offsetof\([\w:]+,\s*(\w+)\)|k([A-Z]\w*)))?', s)
        if m and not _machine(m.group(1)) and m.group(1) not in ('frame', 'ee') and re.match(r'^[A-Za-z][A-Za-z0-9]*$', m.group(1)):
            return 'store' + _upper1(m.group(2) or m.group(3) or re.sub(r'(?<=[A-Za-z])\d+$', '', m.group(1)))
        # A test of anything named.
        if s.startswith('if ('):
            for w in re.findall(r'(?<![\w.:])([a-z][A-Za-z0-9]*)\b(?!\s*[<(:])', s[4:]):
                if not _machine(w) and w not in ('ee', 'frame', 'static_cast', 'if') and len(w) > 2:
                    return 'check' + _upper1(re.sub(r'(?<=[A-Za-z])\d+$', '', w))
        break
    return 'join'


def _name_labels(code, taken):
    """code with the draft's labels (at_104728) named for what follows them."""
    names = {}
    for i, c in enumerate(code):
        m = LABEL.match(c)
        if m and m.group(1) not in names:
            n = _label_name(code, i)
            n = _free_name(n, taken) or m.group(1)
            taken.add(n)
            names[m.group(1)] = n
    if not names:
        return code
    pat = re.compile(r'\b(%s)\b' % '|'.join(sorted(names, key=len, reverse=True)))
    return [pat.sub(lambda m: names[m.group(1)], c) if 'at_' in c else c for c in code]


WALK_WORDS = ('first', 'next', 'last', 'prev', 'previous', 'current', 'head', 'tail')


def _walk_noun(name):
    """The thing a walk's step names (firstAnchor, nextAnchor, anchor2:
    anchor); None for a name that is only such a word or not lower camel."""
    m = re.match(r'^(%s)?([A-Za-z][A-Za-z0-9]*?)\d*$' % '|'.join(WALK_WORDS), name or '')
    if not m:
        return None
    n = m.group(2)
    if m.group(1):
        if not n[:1].isupper():
            return None
        n = n[0].lower() + n[1:]
    return n if _semantic_name(n) and n not in WEAK_NAMES else None


def _real_defs(rs):
    """A web's right-hand sides that say what it holds: not a constant (the
    value before a loop or a branch), not the register's earlier value (the
    caller's, ee::callerRegister, or the one a call left, ee::leftByCall)."""
    return [r for r in rs if not CONSTANT_RHS.match(r) and not _leftover_name(r)]


def _result_name(rhs):
    """`<callee>Result` for a call's result whose callee's name is a verb's
    (kSendMessage: sendMessageResult); (name, why) or None."""
    mm = re.match(r'^(?:static_cast<[\w:]+>\()?ee::call(?:Suspendable)?(?:<[^()]*?>)?\(\s*k(\w+)\b', rhs)
    name = mm.group(1) if mm and not re.match(r'^Fun[0-9A-F]{6}', mm.group(1)) else None
    mm = re.match(r'^(?:static_cast<[\w:]+>\()?game::call(?:At)?<&([\w:]+)>\(', rhs)
    if mm:
        name = mm.group(1).split('::')[-1]
    name = (name or '').lstrip('_')
    if not name or not re.match(r'^[A-Za-z]\w*$', name):
        return None
    return lower_camel(name) + 'Result', 'the result of %s, a verb' % name


def _why_rank(why):
    """How strong a name's evidence is (choose: a tie of votes goes to the
    strongest): the table's, a type's, what the value is (a call's result, a
    global's value, the field or stack slot it is loaded from), the field it
    is stored to, an argument's parameter, the rest."""
    if why == 'readable_names.tsv' or ': the result of' in why:
        return 0
    if why.startswith('an ee::Ptr'):
        return 1
    if why.startswith(('the result of', 'the value of', 'the field it is loaded from', 'a reload of')):
        return 2
    if why.startswith('the field it is stored to'):
        return 3
    if why.startswith('argument'):
        return 4
    return 5


STORE = re.compile(r'^\s*\*ee::at<[^<>()]+(?:<[^<>()]+>)?>\(\s*(.*?)\s*\)\s*=\s*' + CAST + r'([A-Za-z_]\w*)\)*\s*;\s*$')
SLOT_STORE = re.compile(r'^\s*\*frame\.at<[^<>()]+(?:<[^<>()]+>)?>\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)\s*=\s*' + CAST +
                        r'([A-Za-z_]\w*)\)*\s*;\s*$')
SLOT_LOAD = re.compile(r'^' + CAST + r'\*frame\.at<[^<>()]+(?:<[^<>()]+>)?>\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)\)*$')
SLOT_REF = re.compile(r'\bframe\.(?:at<[^<>()]+(?:<[^<>()]+>)?>|address)\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)')


VECTOR_NAME = re.compile(r'(?i)vec|pos|point|out|dir|normal|veloc|offset|cent|min|max|high|low|start|end|target|axis|eye|scale')


class _NoIncludes:
    """A draft's view for typing its variables without noting includes (the
    names passes: name_fields notes the ones its field names need)."""

    def __init__(self, d):
        self.constants, self.globals, self.ws, self.address = d.constants, d.globals, d.ws, d.address


def _later_pass(ws, d, code, names, csigs):
    """The names that follow from the ones given (rename_draft's later passes,
    on the renamed code): ({variable: [(name, why)]}, {variable: (name, why)})
    of the variables still machine-named, by
      - the field every value it is set to is loaded from (a typed base's);
      - the field it is stored to;
      - the stack slot it is reloaded from: what was stored there (a named
        value), or the named parameter of the callee the slot's address is
        handed to (an out argument; a slot alone in its 16 bytes);
      - a copy of a named variable (every value it is set to);
      - its role: a loop's index, a count, an array walk's entry, the
        function's result;
      - its struct type (a typed parameter or local): the type's name."""
    defs, rhss = _defs_of(code)
    typed = _typed_vars(code, defs, _NoIncludes(d), address=d.address)
    ren, fb = {}, {}
    left = [v for v in rhss if _machine(v)] + sorted(v for v in names if PARAM_NAME.match(v) and v not in rhss)
    for v in left:
        rs = rhss.get(v, [])
        if rs and defs.get(v, 0) == len(rs):
            real = _real_defs(rs)
            fs = {_load_field(r, typed) or _field_of(r, typed) for r in real}
            if real and len(fs) == 1 and None not in fs:
                ren.setdefault(v, []).append((fs.pop(), 'the field it is loaded from'))
    # stored to a typed base's field (every web: the value it holds there)
    for c in code:
        m = STORE.match(c)
        if m and _machine(m.group(2)):
            f = _field_of(m.group(1), typed)
            if f:
                ren.setdefault(m.group(2), []).append((f, 'the field it is stored to'))
    # stored unchanged to a named global (`*g_memoryCardJobThreadEntry = param_1`): what it holds there
    for c in code[1:]:
        m = re.match(r'^\s*\*g_([A-Za-z]\w*)\s*=\s*' + CAST + r'([A-Za-z_]\w*)\)*\s*;\s*$', c)
        if m and _machine(m.group(2)) and not re.match(r'^[0-9A-F]{6}', m.group(1)):
            n = _semantic_name(m.group(1)[0].lower() + m.group(1)[1:])
            if n:
                ren.setdefault(m.group(2), []).append((n, 'the field it is stored to: g_%s' % m.group(1)))
    # The stack slots' names: a named value stored there, or the named
    # parameter the slot's address is handed to (a callee's out argument).
    refs = {}
    for c in code:
        for m in SLOT_REF.finditer(c):
            refs.setdefault(int(m.group(1), 0), 0)
            refs[int(m.group(1), 0)] += 1
    slots = {}
    records = {}
    for c in code[1:]:
        m = SLOT_STORE.match(c)
        if m and not _machine(m.group(2)) and m.group(2) not in ROLE_NAMES and \
                not re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z]|frame$)', m.group(2)):
            slots.setdefault(int(m.group(1), 0), set()).add(re.sub(r'(?<=[a-zA-Z])\d$', '', m.group(2)))
        if 'frame.address' not in c:
            continue
        for callee, args in calls_in(c):
            pnames = (csigs.get(callee) or (None, None))[0] if callee.startswith('k') else readable_signature(ws, callee)
            for i, arg in enumerate(args):
                mm = re.match(r'^frame\.address\(\s*(0x[0-9A-Fa-f]+|\d+)\s*\)$', arg.strip())
                if not mm:
                    continue
                o = int(mm.group(1), 0)
                if not pnames or i >= len(pnames) or not pnames[i] or PARAM_NAME.match(pnames[i]):
                    slots.setdefault(o, set()).add(None)   # handed to a parameter with no name: unknown
                    continue
                records.setdefault(o, set()).add(pnames[i])
                if any(refs.get(x) for x in range(o + 1, o + 16)):
                    continue   # a struct or an array there, not one value
                slots.setdefault(o, set()).add(pnames[i])
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or not rs or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        got = set()
        for r in real:
            m = SLOT_LOAD.match(r)
            got.add(int(m.group(1), 0) if m else None)
        if real and len(got) == 1 and None not in got:
            o = got.pop()
            if len(slots.get(o, ())) == 1 and None not in slots[o]:
                ren.setdefault(v, []).append((next(iter(slots[o])), 'a reload of the stack slot 0x%X' % o))
                continue
            # A float of a vector on the stack a named out parameter filled (`out + 8`: outZ).
            base = next((b for b in (o - o % 16, o - 4, o - 8, o - 0xC) if b >= 0 and len(records.get(b, ())) == 1), None)
            if base is not None and o % 4 == 0 and all('ee::Float' in r for r in real):
                n = next(iter(records[base]))
                if n and _semantic_name(n) and VECTOR_NAME.search(n):
                    ren.setdefault(v, []).append((n + 'XYZW'[(o - base) // 4], 'a component of the vector at the stack slot 0x%X' % base))
    for v in left:
        if v in ren:
            continue
        rs = rhss.get(v, [])
        if not rs or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        # A copy of a named variable (a register move), every value it is set to: its name.
        srcs = set()
        for r in real:
            mm = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*$', r)
            srcs.add(mm.group(1) if mm else None)
        if real and len(srcs) == 1 and None not in srcs:
            src = next(iter(srcs))
            if src != v and not _machine(src) and src in names and not re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z])', src) and \
                    src not in CPP_RESERVED and src != 'frame':
                fb[v] = (src, 'a copy of %s' % src)
                continue
            # (a register that holds one address constant each time it is set,
            # `s4 = kCameraDefaultsHigh` on every path: the base of its loads)
            if re.match(r'^k[A-Z][A-Za-z0-9]*$', src) and src != v:
                n = src[1].lower() + src[2:]
                if re.match(r'^[a-z][A-Za-z0-9]*$', n) and n not in CPP_RESERVED and not _machine(n):
                    fb[v] = (n, 'the constant %s' % src)
                    continue
        # The same: a fixed data address every time it is set (0x00270000u,
        # written kNamesHigh after these passes: its name's).
        vals = {r for r in rs if not re.match(r'^0u?$', r) and not _leftover_name(r)}   # (0: before the paths that set it)
        if len(vals) == 1:
            m = re.match(r'^(?:static_cast<u?int32_t>\()?0x([0-9A-F]{8})u\)?$', next(iter(vals)))
            a = int(m.group(1), 16) if m else None
            if a is not None and 0x00220000 <= a < 0x02000000 and not (hasattr(ws, 'generated') and ws.generated(a)):
                k = data_constant_name(ws, a) if hasattr(ws, 'constants') else None
                n = k[1].lower() + k[2:] if k else None
                if n and re.match(r'^[a-z][A-Za-z0-9]*$', n) and n not in CPP_RESERVED and not _machine(n):
                    fb[v] = (n, 'the address %s' % k)
                    continue
        # Copies of the steps of a walk (firstAnchor, then nextAnchor): `anchor`.
        if real and len(srcs) > 1 and None not in srcs and all(not _machine(x) and x in names for x in srcs):
            nouns = {_walk_noun(x) for x in srcs}
            if len(nouns) == 1 and None not in nouns:
                fb[v] = (nouns.pop(), 'copies of %s' % ', '.join(sorted(srcs)))
                continue
            # (the two arms of a branch: categoryPreviousPiece, categoryLastPiece: `piece`)
            tails = {re.findall(r'[A-Z]?[a-z]+', x)[-1].lower() if re.findall(r'[A-Z]?[a-z]+', x) else None for x in srcs}
            if len(tails) == 1 and None not in tails:
                n = _semantic_name(next(iter(tails)))
                if n and n not in TYPE_WORDS:
                    fb[v] = (n, 'copies of %s' % ', '.join(sorted(srcs)))
                    continue
        # A counter: set to a constant, stepped by one; compared (a loop's index) or not (a count).
        steps = [r for r in rs if not CONSTANT_RHS.match(r) and not _leftover_name(r)]
        if steps and all(re.match(r'^%s\s*[-+]\s*1u?$' % re.escape(v), r) for r in steps) and len(steps) < len(rs):
            compared = any(re.search(r'(?<![\w.])(?:static_cast<[\w:]+>\()?%s\)?\s*(?:<|>|<=|>=|!=)(?![<>])|'
                                     r'(?:<|>|<=|>=|!=)\s*(?:static_cast<[\w:]+>\()?%s\b' % (re.escape(v), re.escape(v)), c)
                           for c in code)
            fb[v] = ('i' if compared else 'count', 'a loop\'s index' if compared else 'a count')
            continue
        # An array walk: stepped by a constant stride, read or written through.
        if steps and all(re.match(r'^%s\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?$' % re.escape(v), r) for r in steps) and \
                any(re.search(r'\bee::at<[^()]*>\(\s*%s\s*(?:\+[^()]*)?\)' % re.escape(v), c) for c in code):
            fb[v] = ('entry', 'an array walk\'s entry')
            continue
        # Started from a named variable (a copy of it, or a field's address
        # in it) and stepped by a constant: a walk over it read or written
        # through (`srcCursor`), else a count from it: down by one
        # (`remainingBytes`), or up by one and compared (a loop's index).
        inits = [r for r in rs if not re.match(r'^%s\s*[-+]\s*(?:0x[0-9A-Fa-f]+|\d+)u?$' % re.escape(v), r)]
        stepped = [r for r in rs if r not in inits]
        if stepped and inits and all(not CONSTANT_RHS.match(r) for r in inits):
            srcs = set()
            for r in inits:
                mm = re.match(r'^' + CAST + r'([A-Za-z_]\w*)(?:\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?)?\)*$', r)
                srcs.add(mm.group(1) if mm else None)
            src = next(iter(srcs)) if len(srcs) == 1 else None
            if not src or src == v or re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z])', src) or src in CPP_RESERVED or \
                    src == 'frame':
                continue
            named = not _machine(src) and src in names and src not in ROLE_NAMES
            walked = any(re.search(r'\bee::at<[^()]*>\(\s*%s\s*(?:\+[^()]*)?\)' % re.escape(v), c) for c in code)
            ones = all(re.match(r'^%s\s*[-+]\s*1u?$' % re.escape(v), r) for r in stepped)
            if walked:
                fb[v] = ((src + 'Cursor') if named else 'cursor', 'a walk from %s' % src)
            elif ones and all('-' in r for r in stepped):
                fb[v] = (('remaining' + _upper1(src)) if named else 'remaining', 'a count down from %s' % src)
            elif ones and any(re.search(r'(?<![\w.])%s\)?\s*(?:<|>|<=|>=|!=)(?![<>])|(?:<|>|<=|>=|!=)\s*'
                                        r'(?:static_cast<[\w:]+>\()?%s\b' % (re.escape(v), re.escape(v)), c) for c in code):
                fb[v] = ('i', 'a loop\'s index from %s' % src)
            continue
    # A walk along a list from a plural's value (`s = *g_props; ... s = *(s +
    # 0x7C)`, the next pointer): one of them, `prop`.
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or not rs or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        nexts = [r for r in real if re.match(r'^' + CAST + r'\*ee::at<u?int32_t>\(\s*%s\s*(?:\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?)?\s*\)\)*$'
                                             % re.escape(v), r)]
        starts = [r for r in real if r not in nexts]
        if not nexts or len(starts) != 1 or len({re.sub(r'\s', '', r) for r in nexts}) != 1:
            continue
        m = re.match(r'^' + CAST + r'\*?(g_)?([A-Za-z_]\w*)\)*$', starts[0])
        if not m:
            continue
        src = m.group(2)
        if not m.group(1) and (src not in names or _machine(src)):
            continue
        one = re.sub(r'ies$', 'y', src) if src.endswith('ies') else src[:-1] if re.search(r'[^s]s$', src) else None
        if one and len(one) > 2 and re.match(r'^[a-z][A-Za-z0-9]*$', one) and one not in CPP_RESERVED:
            fb[v] = (one, 'a walk along %s' % src)
    # The difference of two values of one name (collisionClock5 - collisionClock2,
    # the clock read again): `collisionClockDelta`.
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or len(rs) != 1 or defs.get(v, 0) != 1:
            continue
        m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*\s*-\s*' + CAST + r'([A-Za-z_]\w*)\)*$', rs[0])
        if not m:
            continue
        x, y = (re.sub(r'(?<=[A-Za-z])\d+$', '', n) for n in (m.group(1), m.group(2)))
        if x == y and not _machine(m.group(1)) and not _machine(m.group(2)) and m.group(1) != m.group(2) and \
                re.match(r'^[a-z][A-Za-z0-9]*$', x) and x not in ROLE_NAMES:
            fb[v] = (x + 'Delta', 'the difference of two %s values' % x)
    # The compiler's unsigned-to-float conversion (`h = x >> 1`, then `f =
    # toFloat(x & 1 | h); r = f + f` when x is negative, else `r =
    # toFloat(x)`): x's halves and x as a float (dm2's unsignedToFloat's
    # `half`), named for x.
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or len(rs) != 1 or defs.get(v, 0) != 1:
            continue
        m = re.match(r'^static_cast<uint32_t>\(([A-Za-z_]\w*)\) >> 1$', rs[0])
        if not m or _machine(m.group(1)) or m.group(1) not in names or not re.match(r'^[a-z]\w*$', m.group(1)):
            continue
        x = m.group(1)
        half = re.compile(r'^ee::toFloat\(static_cast<int32_t>\(%s & 0x1u \| %s\)\)$' % (re.escape(x), re.escape(v)))
        fs = [w for w in left if w not in ren and w not in fb and len(rhss.get(w, [])) == 1 and
              defs.get(w, 0) == 1 and half.match(rhss[w][0])]
        if len(fs) != 1:
            continue
        f = fs[0]
        xb = re.sub(r'(?<=[A-Za-z])\d+$', '', x)
        rr = [w for w in left if w not in ren and w not in fb and defs.get(w, 0) == len(rhss.get(w, [])) == 2 and
              sorted(rhss[w]) == sorted(['%s + %s' % (f, f), 'ee::toFloat(%s)' % x])]
        fb[v] = (xb + 'Half', 'half of %s (an unsigned-to-float conversion)' % x)
        fb[f] = (xb + 'HalfFloat', 'half of %s as a float (an unsigned-to-float conversion)' % x)
        if len(rr) == 1:
            fb[rr[0]] = (xb + 'Float', '%s (unsigned) as a float' % x)
    # (the same with the half inline: `f = toFloat(x & 1 | x >> 1)`)
    for f in left:
        rs = rhss.get(f, [])
        if f in ren or f in fb or len(rs) != 1 or defs.get(f, 0) != 1:
            continue
        m = re.match(r'^ee::toFloat\(static_cast<int32_t>\(([A-Za-z_]\w*) & 0x1u \| (?:static_cast<uint32_t>\()?'
                     r'([A-Za-z_]\w*)\)? >> 1\)\)$', rs[0])
        if not m or m.group(1) != m.group(2) or _machine(m.group(1)) or m.group(1) not in names or \
                not re.match(r'^[a-z]\w*$', m.group(1)):
            continue
        x = m.group(1)
        xb = re.sub(r'(?<=[A-Za-z])\d+$', '', x)
        rr = [w for w in left if w not in ren and w not in fb and defs.get(w, 0) == len(rhss.get(w, [])) == 2 and
              sorted(rhss[w]) == sorted(['%s + %s' % (f, f), 'ee::toFloat(%s)' % x])]
        fb[f] = (xb + 'HalfFloat', 'half of %s as a float (an unsigned-to-float conversion)' % x)
        if len(rr) == 1:
            fb[rr[0]] = (xb + 'Float', '%s (unsigned) as a float' % x)
    # The value the function returns.
    for c in code:
        m = re.match(r'^\s*return\s+' + CAST + r'([A-Za-z_]\w*)\)*\s*;\s*$', c)
        if m:
            v = m.group(1)
            if REGISTER_NAME.match(v) and v not in ren and v not in fb:
                fb[v] = ('result', 'the function\'s result')
    # A typed variable (a parameter its `ptype` row types, a local typed by
    # the field it is loaded from) still unnamed: its type's name.
    rows = state()['vtypes'].get(d.address, {})
    for v, (q, short, _f) in typed.items():
        if _machine(v) and v in names and v not in ren and v not in fb:
            # (a row's plain vector says less than the field it is loaded from, a later pass's)
            if v in rows and short in ('Vec3', 'Vec4', 'Matrix', 'Quaternion'):
                continue
            n = lower_camel(short)
            if n not in CPP_RESERVED and not _machine(n):
                fb[v] = (n, 'a %s' % q)
    # A page's high half plus an offset (`s6 = cameraDefaultsHigh + 0xA10`, the
    # compiler keeping the `lui` for several addresses): the address's own name.
    pages = {}
    for a, (n, _c) in d.constants.items():
        if a & 0xFFFF == 0 and re.match(r'^k[A-Z]', n):
            pages[n] = pages[n[1].lower() + n[2:]] = a
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or PARAM_NAME.match(v) or not rs or defs.get(v, 0) != len(rs) or not hasattr(ws, 'constants'):
            continue
        got = set()
        for r in _real_defs(rs):
            m = re.match(r'^(?:static_cast<u?int32_t>\()?([A-Za-z]\w*?)\d*\)? ([-+]) (0x[0-9A-Fa-f]+|\d+)u?$', r)
            a = pages.get(m.group(1)) if m else None
            if a is None:
                got.add(None)
                continue
            a += int(m.group(3), 0) * (1 if m.group(2) == '+' else -1)
            k = data_constant_name(ws, a)
            got.add(k[1].lower() + k[2:] if k else None)
        if len(got) == 1 and None not in got:
            n = got.pop()
            if re.match(r'^[a-z][A-Za-z0-9]*$', n) and n not in CPP_RESERVED and not _machine(n):
                fb[v] = (n, 'the address in its page')
    # A counter stepped through a copy (`next = i + 1; ... i = next`), the
    # value a copy of it holds there, and the value copied to one named
    # variable and nowhere else.
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or PARAM_NAME.match(v) or not rs or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        copies = set()
        for r in real:
            m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*$', r)
            copies.add(m.group(1) if m else None)
        if len(copies) == 1 and None not in copies:
            w = next(iter(copies))
            ws_rs = rhss.get(w, [])
            if len(ws_rs) == 1 and defs.get(w, 0) == 1 and re.match(r'^%s\s*\+\s*1u?$' % re.escape(v), ws_rs[0]) and w != v:
                fb[v] = ('i', 'a loop\'s index, stepped through a copy')
                continue
    targets = {}
    for c in code[1:]:
        m = ASSIGN.match(c)
        if m and not _machine(m.group(1)) and m.group(1) in names:
            mm = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\)*$', m.group(2).strip())
            if mm and _machine(mm.group(1)):
                targets.setdefault(mm.group(1), set()).add(m.group(1))
    for v, ts in targets.items():
        if v in ren or v in fb or PARAM_NAME.match(v) or len(ts) != 1:
            continue
        n = _stem(next(iter(ts)), names)
        if n and n not in ROLE_NAMES:
            fb[v] = (n, 'a copy of it is %s' % next(iter(ts)))
    # (not for `infer`: a name said by an expression or a use is no parameter's
    # or field's name: its drafts are made with NAMES_NO_ROLES=1)
    if os.environ.get('NAMES_NO_ROLES'):
        return ren, fb
    # Last: what the value is, said by its expression over named variables.
    for v in left:
        rs = rhss.get(v, [])
        if v in ren or v in fb or PARAM_NAME.match(v) or not rs or defs.get(v, 0) != len(rs):
            continue
        real = _real_defs(rs)
        got = {_expression_name(r, v, names, code) for r in real}
        if real and len(got) == 1 and None not in got:
            fb[v] = (got.pop(), 'its expression')
            continue
        # A float constant a register keeps: its value.
        kept = {r for r in rs if not _leftover_name(r)}
        if not real and len(kept) == 1:
            m = re.match(r'^ee::Float\((-?[\d.]+(?:e[-+]?\d+)?)f\)$', next(iter(kept)))
            n = _literal_name(m.group(1)) if m else None
            if n:
                fb[v] = (n, 'its value')
                continue
        # Only ever set to one and zero: a flag. On a stack record's address: that.
        if not real and len(set(rs)) > 1 and all(re.match(r'^(?:0x)?[01]u?$', r) or _leftover_name(r) for r in rs):
            fb[v] = ('flag', 'set to 0 and 1')
    for v in left:
        if v in ren or v in fb or not PARAM_NAME.match(v) or v not in names:
            continue
        n = _parameter_role(v, code)
        if n:
            fb[v] = (n, 'how it is used')
    return ren, fb


def _parameter_role(v, code):
    """What a parameter's uses say it is, for one no call, field or type
    names: a function it calls, a mask of flags, a count a loop runs to, an
    index scaled into a table, a condition it only tests."""
    q = re.escape(v)
    plain = re.compile(r'(?<![\w.>:])%s\b' % q)
    body = [c for c in code[1:] if v in c and 'calleeSaves' not in c and plain.search(c)]
    if not body:
        return None
    called = re.compile(r'\bee::(?:call|tailCall)(?:<[^()]*>)?\(\s*(?:static_cast<\w+>\()?%s\b' % q)
    if any(called.search(c) for c in body):
        return 'callback'
    through = re.compile(r'\bee::at<[^()]*>\(\s*(?:\(\s*)?%s\b' % q)
    if any(through.search(c) for c in body):
        return None   # (a pointer: its type's to name)
    bits = re.compile(r'(?<![\w.>:])%s\s*&\s*0x' % q)
    bits = [c for c in body if bits.search(c)]
    if bits and len(bits) == len(body):
        return 'flags'
    if any(re.search(r'[A-Za-z_]\w*\)?\s*<\s*(?:static_cast<\w+>\()?%s\)?\s*\)' % q, c) for c in body if c.lstrip().startswith('if')) and \
            not any(re.search(r'(?<![\w.>:])%s\s*[-+*]' % q, c) for c in body):
        return 'limit'
    if any(re.search(r'(?<![\w.>:])%s\s*(?:<<|\*)\s*(?:0x)?[0-9A-F]+' % q, c) for c in body) and len(body) <= 3:
        return 'index'
    tests = [c for c in body if re.match(r'^\s*(?:\} )?(?:else )?if \(\s*(?:static_cast<\w+>\()?%s\)?\s*(?:(?:==|!=)\s*0u?\s*)?\)\s*$' % q, c)]
    if tests and len(tests) == len(body):
        return 'enabled'
    return None


NAME_LIMIT = 32
ROLE_WORDS = {'result', 'value', 'flag', 'temp', 'count'}
LOAD_WORDS = {'uint8_t': 'Byte', 'int8_t': 'Byte', 'uint16_t': 'Half', 'int16_t': 'Half', 'int32_t': 'Word', 'uint32_t': 'Word',
              'uint64_t': 'Dword', 'int64_t': 'Dword', 'ee::Float': 'Float'}
INVERSE = {'<': '>', '<=': '>=', '>': '<', '>=': '<=', '==': '==', '!=': '!='}
COMPARE_WORDS = {'<': 'Below', '<=': 'AtMost', '>': 'Above', '>=': 'AtLeast', '==': 'Is', '!=': 'Not'}
ARITHMETIC_WORDS = {'+': 'Plus', '-': 'Minus', '*': 'Times', '/': 'Over'}
OPERAND = r'(?:static_cast<[\w:]+>\(|ee::Float\()*([A-Za-z_]\w*)\)*'


def _stem(n, names):
    """A named variable as a word of another name (without the number a
    draft added; i, j, k: index); None for anything else: a register's name,
    a constant, a global, the register reads, a type, a number."""
    if not n or _machine(n) or n not in names or n == 'frame' or n in CPP_RESERVED or \
            re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z]|ee$|static_cast$)', n):
        return None
    n = re.sub(r'(?<=[A-Za-z])\d+$', '', n)
    if n in ('i', 'j', 'k'):
        return 'index'
    if n.endswith('High') or n in ROLE_WORDS:
        return None   # (a page's high half: no thing; a role: says nothing of the value)
    return n if re.match(r'^[a-z][A-Za-z0-9]*$', n) and len(n) > 1 else None


def _wrapped(t, i):
    """Whether the bracket at t[i] closes at the very end of t."""
    return t.endswith(')') and len(_balanced(t, i + 1)) == len(t) - i - 2


def _unwrapped(t):
    """t without the casts and brackets around all of it."""
    t = t.strip()
    while True:
        m = re.match(r'^(?:static_cast<[\w:]+>|ee::Float)\(', t)
        if m and _wrapped(t, m.end() - 1):
            t = t[m.end():-1].strip()
        elif t.startswith('(') and _wrapped(t, 0):
            t = t[1:-1].strip()
        else:
            return t


def _operand(text, names):
    """_stem of an operand, casts around it aside."""
    return _stem(_unwrapped(text), names) if re.match(r'^[A-Za-z_]\w*$', _unwrapped(text)) else None


def _split_binary(rhs, ops):
    """(left, operator, right) of rhs's last binary operator among ops outside
    all brackets (they are left to right); None if there is none."""
    depth = 0
    best = None
    for i, c in enumerate(rhs):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif depth == 0 and i and rhs[i - 1] == ' ':
            for op in ops:
                if rhs.startswith(op + ' ', i):
                    best = (rhs[:i - 1].strip(), op, rhs[i + len(op) + 1:].strip())
                    break
    return best


LITERAL_WORDS = {1.0: 'one', 2.0: 'two', 3.0: 'three', 4.0: 'four', 0.5: 'half', 0.25: 'quarter', 0.1: 'tenth',
                 0.75: 'threeQuarters', -1.0: 'minusOne', 10.0: 'ten', 100.0: 'hundred', 1000.0: 'thousand', 255.0: 'maxByte',
                 3.1415927: 'pi', 6.2831855: 'twoPi', 1.5707964: 'halfPi', 180.0: 'hundredEighty', 360.0: 'threeSixty',
                 0.01: 'hundredth', 0.05: 'twentieth', 0.001: 'thousandth'}


def _literal_name(text):
    """A float constant a register keeps through a function: its value, in
    words where it has any (half, twoPi), else `float0_95`."""
    v = float(text)
    n = LITERAL_WORDS.get(round(v, 7)) or LITERAL_WORDS.get(v)
    if n:
        return n
    t = text.lstrip('-').replace('.', '_').replace('+', '').replace('e-', 'e_').rstrip('_')
    # (ends in a letter: a number the free-name pass adds cannot read as a digit of it)
    return ('minus' if v < 0 else 'float') + t + 'f' if re.match(r'^[0-9_e]+$', t) and len(t) <= 8 else None


def _expression_name(rhs, v, names, code):
    """_expression_of, if it is short enough to read."""
    n = _expression_of(rhs, v, names, code)
    return n if n and len(n) <= NAME_LIMIT else None


def _expression_of(rhs, v, names, code):
    """What an expression over named variables computes, as a name: a bit
    test (flagsBit4), a comparison (speedBelowLimit), the step of a walk
    (nextEntry), a sum or a scale (heightMinusOffset, halfWidth), the high half
    of a quadword moved (quaternionHigh); None if an operand has no name or
    the name would be too long to read. The last of the names, for a value
    nothing else says."""
    r = _unwrapped(rhs)
    m = re.match(r'^ee::io::load<[\w:]+>\(0x([0-9A-F]{8})u\)$', r)
    if m and int(m.group(1), 16) in HARDWARE_REGISTERS:
        return lower_camel(HARDWARE_REGISTERS[int(m.group(1), 16)])
    if _split_binary(r, ('?', '||', '&&', '|', '^')):
        return None
    # (a load is a load whichever way it is written)
    m = re.match(r'^(?:ee::io::loadAny|loadUnaligned)(<[\w:]+>)\(', r)
    if m and _wrapped(r, m.end() - 1):
        r = '*ee::at' + m.group(1) + r[m.end() - 1:]
    if re.match(r'^frame\.address\(\s*(?:0x[0-9A-Fa-f]+|\d+)\s*\)$', r):
        return 'stackRecord'
    m = re.match(r'^1 << \(' + OPERAND + r' & 0x1Fu\)$', r)
    if m:
        s = _stem(m.group(1), names)
        return s + 'Mask' if s else None
    # The halves of a quadword (moved by two loads).
    m = re.match(r'^\*ee::at<uint64_t>\(\(?' + OPERAND + r'(?: \+ (?:0x[0-9A-Fa-f]+|\d+)u?)? & ~0xFu\)(?: \+ 8)?\)?$', r)
    if m:
        s = _stem(m.group(1), names)
        return s + ('High' if r.endswith('+ 8)') else 'Low') if s else None
    # A word of a named variable no field names yet (its size's).
    m = re.match(r'^\*ee::at<([\w:]+)>\(\s*' + OPERAND + r'\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+)u?\s*\)$', r)
    if m:
        s = _stem(m.group(2), names)
        return s + LOAD_WORDS.get(m.group(1), 'Word') if s else None
    m = re.match(r'^\*frame\.at<([\w:]+)>\(\s*(?:0x[0-9A-Fa-f]+|\d+)\s*\)$', r)
    if m:
        return 'stack' + LOAD_WORDS.get(m.group(1), 'Word')
    # Through a pointer.
    m = re.match(r'^\*ee::at<[\w:]+>\(\s*' + OPERAND + r'\s*\)$', r)
    if m:
        s = _stem(m.group(1), names)
        return s + 'Value' if s else None
    b = _split_binary(r, ('&',))
    if b:
        m = re.match(r'^0x([0-9A-Fa-f]+)(?:u|ull)?$', b[2])
        x = _operand(b[0], names)
        if m and x:
            k = int(m.group(1), 16)
            if k and k & (k - 1) == 0:
                return '%sBit%d' % (x, k.bit_length() - 1)
            return x + 'Masked'
        return None
    b = _split_binary(r, ('==', '!=')) or _split_binary(r, ('<=', '>=', '<', '>'))
    if b:
        x, op, y = _operand(b[0], names), b[1], _operand(b[2], names)
        if not x and y:
            x, y, op = y, None, INVERSE[op]
        if not x:
            return None
        zero = re.match(r'^(?:static_cast<[\w:]+>\()?(?:0x)?0+(?:u|ull|ll)?\)?$', b[2].strip())
        if zero and op in ('==', '!='):
            return x + ('IsZero' if op == '==' else 'NotZero')
        n = x + COMPARE_WORDS[op] + _upper1(y) if y else x + COMPARE_WORDS[op]
        if op in ('==', '!=') and not y:
            n = x + ('Matches' if op == '==' else 'Differs')
        return n if len(n) <= NAME_LIMIT else x + COMPARE_WORDS[op]
    b = _split_binary(r, ('<<', '>>'))
    if b:
        x = _operand(b[0], names)
        if not x:
            return None
        if b[1] == '<<':
            added = re.compile(r'(?<![\w.>:])%s\s*\+|\+\s*(?:static_cast<[\w:]+>\()?%s\b' % (re.escape(v), re.escape(v)))
            added = any(added.search(c) for c in code if v in c)
            return x + ('Offset' if added else 'Shifted')
        return x + 'Shifted'
    b = _split_binary(r, ('+', '-'))
    if not b:
        b = _split_binary(r, ('*', '/'))
    if b:
        x, op, y = _operand(b[0], names), b[1], _operand(b[2], names)
        half = re.match(r'^ee::Float\(0\.5f\)$', b[2])
        if not half and op == '*' and re.match(r'^ee::Float\(0\.5f\)$', b[0]):
            x, y, half = y, None, True
        one = re.match(r'^(?:1|0x1)u?$', b[2])
        number = re.match(r'^(?:ee::Float\()?-?(?:0x[0-9A-Fa-f]+|\d[\d.e+-]*)(?:u|ull|ll|f)?\)?$', b[2])
        if not x and y and op in '+*':
            x, y = y, None
        if not x:
            return None
        if op == '*' and half:
            return 'half' + _upper1(x)
        if one:
            return ('next' if op == '+' else 'previous') + _upper1(x)
        if not y and not number:
            return x + {'+': 'Sum', '-': 'Difference', '*': 'Product', '/': 'Quotient'}[op]
        if number:
            if op in '+-':
                # (a member's address when it or its base is read through)
                base = re.match(r'^' + OPERAND + r'$', _unwrapped(b[0]) if b[0] else '')
                base = base.group(1) if base else x
                through = re.compile(r'\bee::(?:at|io::loadAny|io::storeAny)<[^()]*>\(\s*(?:%s|%s)\b' % (re.escape(v), re.escape(base)))
                through = any(through.search(c) for c in code if 'ee::' in c and (v in c or base in c))
                return x + ('Field' if through else 'Offset')
            return x + 'Scaled'
        if y:
            n = x + ARITHMETIC_WORDS[op] + _upper1(y)
            if len(n) <= NAME_LIMIT:
                return n
            return x + {'+': 'Sum', '-': 'Difference', '*': 'Product', '/': 'Quotient'}[op]
        return None
    if r.startswith('-'):
        s = _operand(r[1:], names)
        return 'negated' + _upper1(s) if s else None
    m = re.match(r'^ee::(toFloat|toInt|sqrt|fabs|abs)\(', r)
    if m and _wrapped(r, m.end() - 1):
        s = _operand(r[m.end():-1], names)
        if s:
            return s + {'toFloat': 'Float', 'toInt': 'Int', 'sqrt': 'Root', 'fabs': 'Size', 'abs': 'Size'}[m.group(1)]
    return None


def _free_name(n, taken, others=()):
    """n (or one of others), else n2, n3, ... when n is taken (to n99; n9 for
    the register reads, leftT0's: the drafter's to remove); never one shaped
    like a register's (a2, s1); None when all are."""
    for o in (n,) + tuple(others):
        if o not in taken and not _machine(o):
            return o
    top = 9 if re.match(r'^(?:left|caller)[A-Z]', n) else 99
    base, k = n, 2
    while n in taken or _machine(n):
        n = '%s%d' % (base, k)
        k += 1
        if k > top:
            return None
    return n


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------

def apply_functions(dry_run=False):
    """Writes the table's high and medium function rows into the manifests'
    name column (the table is the authority for the rows it has). Returns the
    number changed."""
    rd = _draft_module()
    names = {int(k, 16): n for kind, k, n, c, _ in read_table() if kind == 'function' and c in FUNCTION_USED}
    changed = 0
    for layer in LAYERS:
        path = zaxis_paths.readable(GAME_SRC, layer, 'sweep', 'manifest.txt')
        header, entries = rd.read_manifest(path)
        n = 0
        for e in entries:
            new = names.get(e['address'])
            if new and new != e['name']:
                e['name'] = new
                n += 1
        if n and not dry_run:
            rd.write_manifest(path, header, entries)
        changed += n
        sys.stderr.write('%s: %d names\n' % (zaxis_paths.key_of(path, GAME_SRC), n))
    return changed


ROLE_NAMES = {'result', 'entry', 'i', 'j', 'k', 'count', 'this', 'nullptr', 'true', 'false', 'frame', 'block'}


def _arg_name(arg):
    """The plain name an argument is: `node`, `ee::Ptr<T>(node)`,
    `player.address()`, casts around it; None for an expression, a constant,
    a global, a machine-shaped or a role's name (result, i, entry, leftT0)."""
    a = arg.strip()
    for _ in range(4):
        m = re.match(r'^(?:static_cast<[\w:]+>|ee::Ptr<[\w:<>]+>|ee::Float)\((.*)\)$', a)
        if not m:
            break
        a = m.group(1).strip()
    m = re.match(r'^([A-Za-z_]\w*)(?:\.address\(\))?$', a)
    if not m:
        return None
    n = m.group(1)
    if _machine(n) or n in CPP_RESERVED or re.sub(r'\d+$', '', n) in ROLE_NAMES or \
            re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z])', n):
        return None
    if REGISTER_NAME.match(n) or re.match(r'^[A-Za-z]\d*$', n) and len(n) < 2:
        return None
    if re.sub(r'\d+$', '', n) in LIBGCC_RESULTS.values():
        return None   # (an arithmetic helper's result: asDouble, product say nothing of the parameter)
    # (without the number a draft added to tell two apart: zpdnBone11 is a
    # zpdnBone, as the parameter's own name is read: a multi-digit one kept
    # flipped the row each run)
    return re.sub(r'\d+$', '', n) if re.match(r'^[a-z]\w*?[A-Za-z]\d+$', n) else n


def _call_sites(texts):
    """[(callee address, [args])] of the calls through an address constant in
    texts (a file's text each: its own constants `constexpr uint32_t kName =
    0x...u;` say which address a call goes to)."""
    out = []
    cpat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;')
    for text in texts:
        consts = {m.group(1): int(m.group(2), 16) for m in cpat.finditer(text)}
        for line in text.split('\n'):
            code, _ = code_part(line)
            if 'call' not in code:
                continue
            for callee, args in calls_in(code):
                if callee in consts:
                    out.append((consts[callee], args))
    return out


def _known_globals():
    """readable_draft.py's hand-written globals ({address: [(name, type, namespace)]})."""
    rd = _draft_module()
    return rd.read_known_names(GAME_SRC)[1]


class _TextDraft:
    """A batch file's view as a draft (for infer): its callee constants and
    the globals it declares, and the hand-written globals' types."""

    def __init__(self, text, known_globals):
        self.constants = {int(m.group(2), 16): (m.group(1), '') for m in
                          re.finditer(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;', text)}
        self.globals = {int(m.group(2), 16): (m.group(3), m.group(1), '') for m in
                        re.finditer(r'ee::Global<\s*(.+?)\s*,\s*0x([0-9A-Fa-f]+)u?\s*>\s*(\w+)\s*;', text)}

        class _Ws:
            pass
        self.ws = _Ws()
        self.ws.globals = known_globals
        self.address = None


def infer_params(sweep_dir):
    """Rewrites the table's `inferred:` and `callers:` param rows from a
    build's drafts (<build>/runtime/readable_sweep):
      inferred:  each swept function whose parameters are all integers and
                 that names one (param_N renamed by this module), by position,
                 for its callers' arguments;
      callers:   a swept function's parameter its own draft leaves param_N,
                 named as its callers' arguments are (the drafts' calls and
                 the hand-written code's): the name more than half of the
                 named call sites pass, at two sites or more (or the one
                 site of a function called once).
    Returns the number of rows."""
    import json
    spans = json.load(open(os.path.join(sweep_dir, 'spans.json')))
    found = {}
    unnamed = {}
    texts = []
    old_callers = dict(state()['callers'])
    old_callers_ev = {(int(r[1].split(':')[0], 16), int(r[1].split(':')[1])): r[4] for r in read_table()
                      if r[0] == 'param' and r[4].startswith(CALLERS)}
    for f, d in sorted(spans.items()):
        text = open(os.path.join(sweep_dir, f), encoding='utf-8').read()
        texts.append(text)
        lines = text.split('\n')
        for a, (s0, s1) in d.items():
            for l in lines[s0 - 1:s1]:
                m = re.match(r'^    [^\s/].*?\b\w+\((.*)\)$', l)
                if not m:
                    continue
                ps = split_args(m.group(1))
                if ps and all(re.match(r'^(?:uint32_t|int32_t|uint64_t)\s', p) for p in ps):
                    kept = _without_ordinals([re.sub(r'^.*?\s/?\*?(\w+)\*?/?$', r'\1', p) for p in ps])
                    for i, p in enumerate(ps, 1):
                        n = re.sub(r'^.*?\s/?\*?(\w+)\*?/?$', r'\1', p)
                        if kept[i - 1] is None:
                            continue   # (a variadic callee's slots: first .. sixth)
                        if not (PARAM_NAME.match(n) or REGISTER_NAME.match(n)) and n not in CPP_RESERVED:
                            # (without the number the draft added to tell two apart: box2 is a box)
                            base = re.sub(r'(?<=[a-z][A-Za-z])\d+$', '', n)
                            if base in ROLE_NAMES or re.match(r'^(?:left|caller)[A-Z]', base):
                                continue   # (a role's name says nothing to a caller)
                            found[(int(a, 16), i)] = base if base not in CPP_RESERVED and not _machine(base) else n
                for i, p in enumerate(ps, 1):
                    k = (int(a, 16), i)
                    own = re.sub(r'(?<=[a-z][A-Za-z])\d+$', '', re.sub(r'^.*?\s/?\*?(\w+)\*?/?$', r'\1', p))
                    # (one its callers' row named: still the callers' to name)
                    if re.search(r'\bparam_%d\b' % i, p) or k in old_callers and \
                            own == re.sub(r'(?<=[a-z][A-Za-z])\d+$', '', old_callers[k]):
                        unnamed[k] = True
                break
    for root, dirs, files in zaxis_paths.walk(GAME_SRC):
        dirs.sort()
        rel = zaxis_paths.key_of(root, GAME_SRC).split(os.sep)
        if rel[0] in ('ee', 'dm2') or os.path.basename(root) in ('drafts', 'sweep'):
            dirs[:] = []
            continue
        for fn in sorted(files):
            if fn.endswith(('.cpp', '.h')):
                texts.append(readable_symbols.literal_view(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()))
    sites = {}
    for a, args in _call_sites(texts):
        for i, arg in enumerate(args, 1):
            if (a, i) in unnamed:
                sites.setdefault((a, i), []).append(_arg_name(arg))
    called = {}
    for a, _ in _call_sites(texts):
        called[a] = called.get(a, 0) + 1
    callers = {}
    for k, names in sites.items():
        named = [n for n in names if n]
        if not named:
            continue
        votes = {}
        for n in named:
            votes[n] = votes.get(n, 0) + 1
        best = sorted(votes, key=lambda n: (-votes[n], n))[0]
        # (and an eighth of all its sites: 2 of defineProperty's 190 passing
        # `widget` do not name its record parameter)
        if votes[best] * 2 > len(named) and (votes[best] >= 2 or called.get(k[0], 0) == 1) and votes[best] * 8 >= len(names):
            callers[k] = (best, votes[best], len(names))
    # A callers' row stays while no other name wins (its name feeds the call
    # sites' own names, so a run that does not see it again would undo it the
    # next: the rows went back and forth between two sets).
    for k, n in old_callers.items():
        if k not in callers and k in unnamed and _arg_name(n):
            callers[k] = (n, 0, 0)
    # Parameter types from the callers: a swept function's parameter that
    # the typed call sites (a variable wrapped in ee::Ptr<T>, typed by its
    # name, a typed parameter's or a typed field's) pass one struct type at,
    # more than half of them and two or more (or its one call site), takes
    # it: a `ptype` row (`callers:`), so its fields read by name.
    struct_fields()
    tsites = {}
    owns = {}
    handed_edges = {}   # (function, parameter) -> {(callee, parameter)}: handed on unchanged
    cpat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;')
    swept = set(state()['functions'])
    known_globals = _known_globals()
    for f, d in sorted(spans.items()):
        text = open(os.path.join(sweep_dir, f), encoding='utf-8').read()
        consts = {m.group(1): int(m.group(2), 16) for m in cpat.finditer(text)}
        view = _TextDraft(text, known_globals)
        lines = text.split('\n')
        for a, (s0, s1) in d.items():
            body = lines[s0 - 1:s1]
            start = next((j for j, l in enumerate(body) if re.match(r'^    [^\s/].*?\b\w+\((.*)\)$', l)), None)
            if start is None:
                continue
            code = [code_part(l)[0] for l in body[start:]]
            view.address = int(a, 16)
            defs0 = _defs_of(code)[0]
            typed = _typed_vars(code, defs0, view, address=int(a, 16))
            # Its own parameters' types, as its own code gives them (wrapped
            # in ee::Ptr<T>, handed to a typed callee's parameter, never
            # reassigned; not by their names, not by the table's ptype rows):
            # `own:` ptype rows, for its callers' arguments.
            # (from its own wrapping only: a type a callee's ptype row gave it
            # would come back round through its callers' rows)
            own = _typed_vars(code, defs0, None, by_name=False)
            pnames = signature_params(code[0]) or []
            for i, pn in enumerate(pnames, 1):
                if pn and pn in own and defs0.get(pn, 0) == 0:
                    owns[(int(a, 16), i)] = own[pn][0]
            pindex = {pn: i for i, pn in enumerate(pnames, 1) if pn and defs0.get(pn, 0) == 0}
            for c in code[1:]:
                if 'call' not in c:
                    continue
                for callee, args in calls_in(c):
                    target = consts.get(callee)
                    if target not in swept:
                        continue
                    for i, arg in enumerate(args, 1):
                        x = arg.strip()
                        mh = re.match(r'^(?:static_cast<uint32_t>\()?([A-Za-z_]\w*)\)?$', x)
                        if mh and mh.group(1) in pindex:
                            handed_edges.setdefault((int(a, 16), pindex[mh.group(1)]), set()).add((target, i))
                        m = re.match(r'^ee::Ptr<((?:\w+::)*[A-Z]\w*)>\(\s*(?:static_cast<uint32_t>\()?([A-Za-z_]\w*)\)?\s*\)$', x)
                        t = None
                        if m and struct_fields().get(m.group(1)):
                            t = m.group(1)
                        else:
                            m = re.match(r'^(?:static_cast<[\w:]+>\()?([A-Za-z_]\w*)\)?$', x)
                            if m and m.group(1) in typed:
                                t = typed[m.group(1)][0]
                        tsites.setdefault((target, i), []).append(t)
    ptypes = {}
    for k, ts in tsites.items():
        got = [t for t in ts if t]
        if not got:
            continue
        votes = {}
        for t in got:
            votes[t] = votes.get(t, 0) + 1
        best = sorted(votes, key=lambda t: (-votes[t], t))[0]
        # more than half of the typed sites, two or more (or the one site),
        # and a fifth of all its sites at least (2 of defineProperty's 190
        # sites passing a widget say nothing of its record parameter)
        if votes[best] * 2 > len(got) and (votes[best] >= 2 or called.get(k[0], 0) == 1) and votes[best] * 5 >= len(ts):
            ptypes[k] = (best, votes[best], len(ts))
    # Its own code's type wins over its callers' (unless theirs is a more
    # derived one of the same chain: a widget kind's handler handed a GuiMenu).
    bases = layout_bases()
    # A parameter handed on unchanged to callees' parameters whose type is
    # grounded (an explicit ptype row, or a callee's own wrapping, in turn)
    # takes it (the most derived of one chain): worked out here to a fixed
    # point from the code alone each run, not from the rows of the last (a
    # type from its callers' rows would come back round and keep itself).
    explicit_rows = {(int(r[1].split(':')[0], 16), int(r[1].split(':')[1])): r[2] for r in read_table()
                     if r[0] == 'ptype' and r[3] in USED and not r[4].startswith((CALLERS, OWN))}
    grounded = dict(owns)
    grounded.update(explicit_rows)
    handed_types = {}
    for _ in range(12):
        grown = False
        for k, outs in sorted(handed_edges.items()):
            if k in grounded:
                continue
            qs = {grounded[o] for o in outs if o in grounded}
            q = _most_derived(qs, bases) if qs else None
            if q and struct_fields().get(q):
                grounded[k] = handed_types[k] = q
                grown = True
        if not grown:
            break
    if os.environ.get('NAMES_MEASURE'):
        # (the measure of the rule: its types where the parameter's own code or
        # its callers type it too)
        agree = differ = 0
        for k, outs in sorted(handed_edges.items()):
            if k not in owns:
                continue
            qs = {grounded[o] for o in outs if o in grounded and o != k}
            q = _most_derived(qs, bases) if qs else None
            if q:
                if _most_derived({q, owns[k]}, bases):
                    agree += 1
                else:
                    differ += 1
                    sys.stderr.write('handed differs: 0x%08X:%d own %s handed %s\n' % (k[0], k[1], owns[k], q))
        sys.stderr.write('handed vs own: %d agree, %d differ\n' % (agree, differ))
        agree = differ = 0
        for k, q in handed_types.items():
            if k in ptypes and ptypes[k][1]:
                if _most_derived({q, ptypes[k][0]}, bases):
                    agree += 1
                else:
                    differ += 1
                    sys.stderr.write('handed vs callers differs: 0x%08X:%d callers %s handed %s\n' % (k[0], k[1], ptypes[k][0], q))
        sys.stderr.write('handed vs callers: %d agree, %d differ\n' % (agree, differ))
    for k, t in handed_types.items():
        owns[k] = t
    for k, t in owns.items():
        if k not in ptypes or _most_derived({t, ptypes[k][0]}, bases) != ptypes[k][0]:
            ptypes[k] = (t, 0, 0)
    rows = [r for r in read_table() if not (r[0] == 'param' and r[4].startswith((INFERRED, CALLERS))) and
            not (r[0] == 'ptype' and r[4].startswith((CALLERS, OWN)))]
    explicit_types = {r[1] for r in rows if r[0] == 'ptype'}
    explicit = {r[1] for r in rows if r[0] == 'param'}
    header = [l.rstrip('\n') for l in open(TABLE, encoding='utf-8') if l.startswith('#')]
    new = [('param', '0x%08X:%d' % k, n, 'medium', INFERRED + ' its own draft names it so (readable_names.py infer)')
           for k, n in sorted(found.items()) if '0x%08X:%d' % k not in explicit]
    new += [('param', '0x%08X:%d' % k, n, 'medium', (CALLERS + ' %d of its %d call sites pass `%s` (readable_names.py infer)'
             % (v, c, n)) if c else old_callers_ev.get(k, CALLERS + ' its call sites passed `%s` (readable_names.py infer)' % n))
            for k, (n, v, c) in sorted(callers.items()) if '0x%08X:%d' % k not in explicit]
    new = [r for r in new if not (r[4].startswith(INFERRED) and tuple(int(x, 16 if j == 0 else 10) for j, x in
                                                                   enumerate(r[1].split(':'))) in callers)]
    new += [('ptype', '0x%08X:%d' % k, t, 'medium', (CALLERS + ' %d of its %d call sites pass a %s (readable_names.py infer)'
             % (v, c, t.split('::')[-1])) if c else (OWN + ' its own draft types it a %s (readable_names.py infer)'
                                                     % t.split('::')[-1]))
            for k, (t, v, c) in sorted(ptypes.items()) if '0x%08X:%d' % k not in explicit_types]
    new = [(r[0], r[1], r[2], r[3], r[4].replace('its own draft types it', 'it hands it on to a parameter typed')
            if r[0] == 'ptype' and r[4].startswith(OWN) and
            tuple(int(x, 16 if j == 0 else 10) for j, x in enumerate(r[1].split(':'))) in handed_types else r[4])
           for r in new]
    body = sorted(rows + new, key=lambda r: (r[0] != 'function', r[0], r[1]))
    with open(TABLE, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(header) + '\n')
        for r in body:
            f.write('\t'.join(r) + '\n')
    sys.stderr.write('%d inferred, %d callers rows, %d parameter types (%d handed on)\n' %
                     (len(found), len(callers), len(ptypes), len(handed_types)))
    return len(new)


USES = 'uses:'
# Names that say what a variable does in its function, not what a field or a
# global holds (a loop's index, a walk's cursor, a verb's result).
WEAK_NAMES = {'value', 'data', 'temp', 'tmp', 'out', 'in', 'ptr', 'buf', 'address', 'base', 'other', 'first', 'last',
              'next', 'prev', 'previous', 'current', 'src', 'dst', 'arg', 'args', 'argument', 'cursor', 'size', 'count',
              'argA', 'argB', 'copy', 'local', 'big', 'small', 'hold', 'dv', 'str', 'string', 'text', 'name', 'buffer',
              'heapBlock', 'nameHash', 'block', 'randomValue'} | set(LIBGCC_RESULTS.values())
# Variable names too general to be one struct (an allocator's block, a walk's
# entry): never a recovered struct's.
GENERIC_BASE = re.compile(r'(?i)alloc|block|memory|buffer|result|data|record|entry|item|value|temp|^src|^dst|^out|'
                          r'^buf|ptr|address|base|^p$|^widget$|^node$|^list$|^object$|^element|^target|^current|^next|^prev')


def _semantic_name(n):
    """n without a trailing number when it says what a value is (not a
    machine's, a role's, a register read's, a constant's or a verb's
    result); None otherwise."""
    if not n or _machine(n) or n in CPP_RESERVED or re.match(r'^(?:k[A-Z]|g_|left[A-Z]|caller[A-Z])', n):
        return None
    n = re.sub(r'(?<=[A-Za-z])\d+$', '', n)
    if len(n) < 2 or n in ROLE_NAMES or n in WEAK_NAMES or n.endswith(('Result', 'Cursor')) or n.startswith('sce') or \
            re.match(r'^remaining(?:[A-Z]|$)', n) or not re.match(r'^[a-z][A-Za-z0-9]*$', n):
        return None
    return n


def _uses_votes(sweep_dir):
    """The names the drafts' data flow gives the fields with no name and the
    globals with none: {('field', struct or 'name:N', offset): [(name, how,
    function)]}, {('global', address): [...]}, and {'name:N': layers}."""
    import json
    rd = _draft_module()
    spans = json.load(open(os.path.join(sweep_dir, 'spans.json')))
    known_globals = _known_globals()
    known_consts = rd.read_known_names(GAME_SRC)[0]

    class _Ws:
        pass
    ws = _Ws()
    ws.readable = rd.read_readable(GAME_SRC)
    s = state()
    pnames_by = {}
    for (a, i), n in list(s['params'].items()) + list(s['inferred'].items()) + list(s['callers'].items()):
        pnames_by.setdefault(a, {})[i] = n
    for (a, v), n in s['argparams'].items():
        if v.startswith('arg'):
            pnames_by.setdefault(a, {})[int(v[3:]) + 1] = n
    for a, lst in known_consts.items():
        for n, c in lst:
            ps, _r = comment_signature(c)
            for i, x in enumerate(ps or (), 1):
                if x:
                    pnames_by.setdefault(a, {}).setdefault(i, x)
    used_names = {a: (n) for kind, k, n, c, ev in s['rows'] if kind == 'global' for a in [int(k, 16)]}
    uses_globals = {n: int(k, 16) for kind, k, n, c, ev in s['rows'] if kind == 'global' and ev.startswith(USES)}
    fields = struct_fields()
    sizes = struct_sizes()
    # a name ending in a known struct's (parentNode, rootNode: a Node) is that one's
    kinds = re.compile(r'(?:%s)$' % '|'.join(sorted({re.escape(t.split('::')[-1]) for t in fields if t}, key=len,
                                                   reverse=True)))
    votes = {}
    layers = {}
    widths = {}   # name:N -> {offset: {access kinds}}: one struct reads one kind at an offset
    cpat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;\s*//\s*([\w:]+) \+0x([0-9A-Fa-f]+) '
                      r'\(readable_names_layout\.tsv')
    load_pat = re.compile(r'^' + CAST + r'(?:\*ee::at|ee::io::loadAny)<[^<>()]+(?:<[^<>()]+>)?>\(\s*(.*?)\s*\)\)*$')
    for f, d in sorted(spans.items()):
        text = open(os.path.join(sweep_dir, f), encoding='utf-8').read()
        consts = {m.group(1): int(m.group(2), 16) for m in
                  re.finditer(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;', text)}
        table_consts = {m.group(1): (m.group(3), int(m.group(4), 16)) for m in cpat.finditer(text)}
        view = _TextDraft(text, known_globals)
        gnames = {g[0]: a for a, g in view.globals.items()}
        lines = text.split('\n')
        layer = f.split('/')[0]
        for a, (s0, s1) in d.items():
            body = lines[s0 - 1:s1]
            start = next((j for j, l in enumerate(body) if re.match(r'^    [^\s/].*?\b\w+\((.*)\)$', l)), None)
            if start is None:
                continue
            code = [code_part(l)[0] for l in body[start:]]
            view.address = int(a, 16)
            defs, rhss = _defs_of(code)
            typed = _typed_vars(code, defs, view, address=int(a, 16))
            for c in code:
                for m in re.finditer(r'ee::at<([^<>()]+)>\(\s*([A-Za-z_]\w*?)\d*\s*\+\s*(0x[0-9A-Fa-f]+|\d+)u?\s*\)', c):
                    t = m.group(1)
                    k = 'f' if 'Float' in t else '8' if t.endswith('64_t') else '2' if '16' in t else '1' if '8_t' in t else '4'
                    widths.setdefault('name:' + m.group(2), {}).setdefault(int(m.group(3), 0), set()).add(k)
            derefd = {m.group(1) for c in code for m in
                      re.finditer(r'\bee::(?:at<[^<>()]+(?:<[^<>()]+>)?>|io::loadAny<[^<>()]+>)\(\s*([A-Za-z_]\w*)\b', c)}

            def key_of(addr):
                """The field key of an address expression: a typed base's
                offset it has no field at (or a field the layout table's
                `uses:` rows named), or a recoverable name's offset."""
                m = re.match(r'^' + CAST + r'([A-Za-z_]\w*)\s*\+\s*(0x[0-9A-Fa-f]+|\d+|k\w+)u?\)*$', addr)
                if not m:
                    return None
                b, o = m.group(1), m.group(2)
                if o.startswith('k'):
                    if o not in table_consts:
                        return None
                    q, off = table_consts[o]
                    return ('field', q, off)
                o = int(o, 0)
                if b in typed:
                    q, _short, fs = typed[b]
                    if o in fs or embedded_field(q, fs, o):
                        return None
                    # inside the struct: its size, else its known fields' extent
                    if o >= sizes.get(q, max(fs) + 4 if fs else 0):
                        return None
                    return ('field', q, o)
                if _machine(b) or b in CPP_RESERVED or b == 'frame' or defs.get(b, 0) > 1 or o >= 0x1000 or \
                        b not in derefd:
                    return None
                base = re.sub(r'(?<=[A-Za-z])\d+$', '', b)
                if _semantic_name(base) and not GENERIC_BASE.search(base) and base not in GENERIC_TYPE_NAMES and \
                        not kinds.search(base):
                    layers.setdefault('name:' + base, {}).setdefault(layer, 0)
                    layers['name:' + base][layer] += 1
                    return ('field', 'name:' + base, o)
                return None

            def global_of(x):
                m = re.match(r'^' + CAST + r'\*(g_\w+)\)*$', x)
                if not m:
                    return None
                g = m.group(1)
                if re.match(r'^g_[0-9A-F]{6}$', g):
                    return ('global', int(g[2:], 16))
                if g in uses_globals:
                    return ('global', uses_globals[g])
                return None

            def vote(k, n, how):
                n = _semantic_name(n)
                if k and n:
                    votes.setdefault(k, []).append((n, how, a))
            # An accessor: one statement, the field read and returned (materialNameHash).
            stmts = [c.strip() for c in code[1:] if c.strip() and c.strip() not in '{}' and not c.strip().startswith('ee::block(')]
            if len(stmts) == 1 and stmts[0].startswith('return '):
                lm = load_pat.match(stmts[0][7:].rstrip(';').strip())
                own = s['functions'].get(int(a, 16), ('', ''))[0]
                if lm and own:
                    n = noun_of_function(own) or own
                    if n:
                        vote(key_of(lm.group(1)), n, 'what %s returns' % own)
            fn = lambda x: None
            for c in code[1:]:
                m = ASSIGN.match(c)
                if m:
                    v, rhs = m.group(1), m.group(2).strip()
                    lm = load_pat.match(rhs)
                    if lm:
                        vote(key_of(lm.group(1)), v, 'loaded into %s' % v)
                    vote(global_of(rhs), v, 'read into %s' % v)
                    gm = re.match(r'^\s*\*(g_\w+)\s*$', v) if False else None
                m = STORE.match(c)
                if m:
                    vote(key_of(m.group(1)), m.group(2), 'set from %s' % m.group(2))
                m = re.match(r'^\s*\*(g_\w+)\s*=\s*' + CAST + r'([A-Za-z_]\w*)\)*\s*;\s*$', c)
                if m:
                    vote(global_of('*' + m.group(1)), m.group(2), 'set from %s' % m.group(2))
                if 'call' not in c:
                    continue
                for callee, args in calls_in(c):
                    if callee.startswith('k'):
                        pn = pnames_by.get(consts.get(callee), {})
                        pl = [pn.get(i) for i in range(1, max(pn) + 1)] if pn else []
                    else:
                        pl = readable_signature(ws, callee) or []
                    for i, arg in enumerate(args):
                        if i >= len(pl) or not pl[i] or PARAM_NAME.match(pl[i]):
                            continue
                        x = arg.strip()
                        lm = load_pat.match(x)
                        if lm:
                            vote(key_of(lm.group(1)), pl[i], 'argument %d of %s' % (i + 1, callee))
                        else:
                            vote(key_of(x), pl[i], 'its address argument %d of %s' % (i + 1, callee))
                        vote(global_of(x), pl[i], 'argument %d of %s' % (i + 1, callee))
    # a name whose variables read one offset as a float and an integer (or at
    # two widths) is no one struct (an accessor's result, per kind): none
    mixed = {n for n, offs in widths.items() if any(len(ks) > 1 for ks in offs.values())}
    return votes, layers, mixed


def _winner(vs):
    """(name, votes, named votes) of the name more than half of the votes
    give (from two functions or more for medium, else low); None."""
    if not vs:
        return None
    c = {}
    for n, _how, _fn in vs:
        c[n] = c.get(n, 0) + 1
    best = sorted(c, key=lambda n: (-c[n], n))[0]
    if c[best] * 2 <= len(vs):
        return None
    fns = {fn for n, _h, fn in vs if n == best}
    if len(fns) < 2 and not re.search(r'[a-z0-9][A-Z]', best):
        return None   # one word (str, bytes, format) from one function says too little
    return best, c[best], len(vs), 'medium' if len(fns) >= 2 else 'low'


def infer_uses(sweep_dir, fresh=False):
    """`uses:` rows from the drafts' data flow (readable_names.py infer):
      - a field a typed base has no name at (readable_names_layout.tsv): the
        name of what more than half of the named values stored there, loaded
        from there, or handed on from there (a callee's parameter) are;
      - a recovered struct for the variables of one distinctive name (all
        `editObjectPart`s), its fields named so, and a `name` row typing
        the variables of that name;
      - a global with no name (readable_names.tsv): the same, `g_` + name.
    Medium when two functions or more agree, else low (applied, marked). The
    rows already there are kept (their names feed the drafts' own names, so
    they are not voted again) unless --fresh. Returns the number of rows."""
    votes, layers, mixed = _uses_votes(sweep_dir)
    fields = struct_fields()
    # the layout table
    header, keep, old = [], [], {}
    dropped_structs = set()
    for line in open(LAYOUT, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
            header.append(line)
            continue
        c = (line.split('\t') + [''] * 5)[:5]
        if c[4].startswith(USES):
            if c[1] == 'name' and 'name:' + c[2] in mixed:
                dropped_structs.add(c[0])
            elif not fresh:
                old[(c[0], c[1])] = c
            continue
        keep.append(c)
    explicit = set()   # (struct, offset) of the rows infer does not own
    for c in keep:
        try:
            explicit.add((c[0], int(c[1], 0)))
        except ValueError:
            pass
    taken = {}
    for t, fs in fields.items():
        if t:
            taken[t] = {f for f, _ in fs.values()}
    # (a struct recovered for a name now found mixed: its rows go with its name row)
    for key in [k for k in old if old[k][0] in dropped_structs]:
        del old[key]
    recovered = {}
    for k, vs in sorted(votes.items()):
        if k[0] != 'field' or not k[1].startswith('name:') or k[1] in mixed:
            continue
        n = k[1][5:]
        ls = layers.get(k[1], {})
        ns = sorted(ls, key=lambda x: -ls[x])[0] if ls else 'zge'
        q = {'aggressive': 'aggressive', 'zge': 'zge', 'sce': 'sce'}.get(ns, 'zge') + '::' + _upper1(n)
        if fields.get(q) and q not in [o[0] for o in old.values()]:
            continue   # a hand-written struct of that name: typed by name already
        recovered[k[1]] = q
    rows = []
    by_struct = {}
    for k, vs in sorted(votes.items()):
        if k[0] != 'field':
            continue
        q = recovered.get(k[1], k[1])
        if q.startswith('name:'):
            continue
        w = _winner(vs)
        if not w or not re.match(r'^[a-z]\w*$', w[0]):
            continue
        by_struct.setdefault(q, []).append((k[2], w, vs))
    for q, items in sorted(by_struct.items()):
        names = dict(((o, w[0]) for o, w, _ in items))
        counts = {}
        for o, n in names.items():
            counts[n] = counts.get(n, 0) + 1
        for o, w, vs in sorted(items):
            n = w[0]
            if (q, '0x%X' % o) in old or counts[n] > 1 or n in taken.get(q, ()) or (q, o) in explicit:
                continue   # kept, or two offsets of one name, or the struct has the name or a row there
            hows = sorted({h for nm, h, _ in vs if nm == n})[:3]
            rows.append([q, '0x%X' % o, n, w[3], '%s %d of %d named uses: %s (readable_names.py infer)' %
                         (USES, w[1], w[2], '; '.join(hows))])
    for q in sorted(set(recovered.values())):
        n = [k for k, v in recovered.items() if v == q][0][5:]
        if any(r[0] == q for r in rows) or any(o[0] == q and o[1] != 'name' for o in old.values()):
            if (q, 'name') not in old:
                rows.append([q, 'name', n, 'medium', '%s the variables named %s (readable_names.py infer)' % (USES, n)])
    with open(LAYOUT, 'w', encoding='utf-8', newline='\n') as f:
        for line in header:
            f.write(line + '\n')
        # the other rows as they are; infer's after them, by struct, name row first, then offset
        for c in keep + sorted(list(old.values()) + rows, key=lambda c: (c[0], c[1] != 'name',
                                                                         int(c[1], 16) if c[1].startswith('0x') else 0)):
            f.write('\t'.join(c) + '\n')
    # the globals
    trows = read_table()
    gtaken = {r[2] for r in trows if r[0] == 'global'}
    for lst in _known_globals().values():
        for n, _t, _ns in lst:
            gtaken.add(n if n.startswith('g_') else 'g_' + n[:1].lower() + n[1:])
    # (and the names of the addresses inside records: g_worldVertexFlags is one;
    # a second address of that name in a batch file is renamed by text, every use)
    for base, rec in record_globals().items():
        if base is None or not rec:
            continue
        span = rec[2] * rec[3] if len(rec) > 3 and rec[2] and rec[3] else 0
        for a in range(base, base + min(span, 0x10000)):
            n = record_global_name(a)
            if n:
                gtaken.add(n)
    have = {int(r[1], 16) for r in trows if r[0] == 'global'}
    if fresh:
        trows = [r for r in trows if not (r[0] == 'global' and r[4].startswith(USES))]
        have = {int(r[1], 16) for r in trows if r[0] == 'global'}
    grows = []
    for k, vs in sorted(votes.items()):
        if k[0] != 'global' or k[1] in have:
            continue
        w = _winner(vs)
        if not w:
            continue
        n = 'g_' + w[0]
        if n in gtaken:
            continue
        gtaken.add(n)
        hows = sorted({h for nm, h, _ in vs if nm == w[0]})[:3]
        grows.append(('global', '0x%08X' % k[1], n, w[3], '%s %d of %d named uses: %s (readable_names.py infer)' %
                      (USES, w[1], w[2], '; '.join(hows))))
    if grows or fresh:
        header_t = [l.rstrip('\n') for l in open(TABLE, encoding='utf-8') if l.startswith('#')]
        body = sorted(trows + grows, key=lambda r: (r[0] != 'function', r[0], r[1]))
        with open(TABLE, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(header_t) + '\n')
            for r in body:
                f.write('\t'.join(r) + '\n')
    sys.stderr.write('uses: %d field rows (%d recovered structs), %d global rows\n' %
                     (len(rows), len(set(recovered.values())), len(grows)))
    return len(rows) + len(grows)


def _draft_module():
    """readable_draft, from this folder: also under `python3 -I` (no script
    folder on the path), and after the standard library (a folder's bisect.py
    must not shadow the real one)."""
    if HERE not in sys.path:
        sys.path.append(HERE)
    import readable_draft
    return readable_draft


def sync_hand():
    """Function, global and parameter rows at hand-written addresses take the
    hand-written name: the sources are the authority, and a stale row would
    name the callers' constants wrongly. A name another function already has
    is left out and returned: ({kind: rows changed}, [clashes in words]). A
    name the table may not hold (CPP_RESERVED: `sweep`, a namespace of the
    drafts, is a hand-written parameter's name) stays out: the row keeps its own.
    Names stay unique across the layers (ee::readable and ZX_READABLE find
    functions by name), so such a clash needs one of the two renamed."""
    rd = _draft_module()
    hand = rd.read_readable(GAME_SRC)
    for r in hand.values():
        r['name'] = r['name'].removesuffix('_')  # (a GAME_CALL body: the function's name has no marker)
    lines = open(TABLE, encoding='utf-8').read().split('\n')
    rows = [line.split('\t') for line in lines]
    function_at = {int(c[1], 16): c[2] for c in rows if len(c) >= 5 and c[0] == 'function'}
    hand_at = {}   # name -> {(namespace, file): address}
    for a, r in hand.items():
        hand_at.setdefault(r['name'], {}).setdefault((r.get('namespace'), r.get('file')), a)
    manifest = {}
    for a, (n, _f) in manifest_names().items():
        if not machine_function_name(n):
            manifest.setdefault(n, a)

    def holder(name, address):
        """Who else has the name: (address, what it is), None."""
        for (ns, path), a in sorted(hand_at.get(name, {}).items(), key=lambda kv: kv[1]):
            if a != address:
                return a, 'hand-written in %s (%s)' % (ns or 'no namespace', zaxis_paths.key_of(path or '', GAME_SRC))
        for a, n in sorted(function_at.items()):
            if n == name and a != address and a not in hand:
                return a, 'a swept function\'s table row'
        a = manifest.get(name)
        if a is not None and a != address and a not in hand and a not in function_at:
            return a, 'a swept function\'s manifest name'
        return None

    counts = {'function': 0, 'global': 0, 'param': 0}
    clashes = []
    for i, c in enumerate(rows):
        if len(c) < 5 or c[0] != 'function' or int(c[1], 16) not in hand:
            continue
        a = int(c[1], 16)
        name = hand[a]['name']
        if name == c[2] or name in CPP_RESERVED:
            continue
        other = holder(name, a)
        if other:
            # (one name a function: ee::readable and ZX_READABLE find
            # functions by name, whatever their namespace)
            clashes.append('0x%08X wants %s, but 0x%08X has it: %s. The table keeps one name per function across '
                           'all layers, so its row stays %s: rename one of the two where it is written, then '
                           'sync again.' % (a, name, other[0], other[1], c[2]))
            continue
        c[4] = 'hand-written (was %s)' % c[2]
        c[2], c[3] = name, 'high'
        function_at[a] = name
        counts['function'] += 1
    # The hand-written globals are the authority on theirs: one name at the
    # address (a game's, not DM2's or the runtime's).
    known = rd.read_known_names(GAME_SRC)[1]
    taken = {c[2]: int(c[1], 16) for c in rows if len(c) >= 5 and c[0] == 'global'}
    hand_addresses = {}
    for a, gs in known.items():
        for g in gs:
            hand_addresses.setdefault(g[0], set()).add(a)
    for c in rows:
        if len(c) < 5 or c[0] != 'global':
            continue
        a = int(c[1], 16)
        names = {n for n, _t, ns in known.get(a) or () if n.startswith('g_') and not re.match(r'^(?:dm2|ee)(?:::|$)', ns or '')}
        if len(names) != 1 or c[2] in names:
            continue
        name = next(iter(names))
        if name in CPP_RESERVED:
            continue
        # (two files' own globals of one name, g_frameSeconds at two addresses:
        # the drafts take each hand-written name by address, the row is unread)
        if taken.get(name, a) != a or hand_addresses.get(name, {a}) != {a}:
            continue
        c[4] = 'hand-written (was %s)' % c[2]
        c[2], c[3] = name, 'high'
        taken[name] = a
        counts['global'] += 1
    # ... and the parameters of a hand-written function (a stale row names its
    # callers' arguments): by position, where the definition names them.
    class _Ws:
        pass
    ws = _Ws()
    ws.readable = hand
    for c in rows:
        if len(c) < 5 or c[0] != 'param' or not re.match(r'^0x[0-9A-Fa-f]+:\d+$', c[1]):
            continue
        a, i = c[1].split(':')
        r = hand.get(int(a, 16))
        if not r or r.get('kind') == 'PLANNED' or not r.get('signature'):
            continue
        ns = r.get('namespace') or ''
        names = readable_signature(ws, (ns + '::' + r['name']) if ns else r['name'])
        i = int(i)
        if not names or i > len(names) or not names[i - 1] or PARAM_NAME.match(names[i - 1]) or names[i - 1] == c[2] or \
                names[i - 1] in CPP_RESERVED:
            continue
        c[4] = 'hand-written (was %s)' % c[2]
        c[2], c[3] = names[i - 1], 'high'
        counts['param'] += 1
    with open(TABLE, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join('\t'.join(c) for c in rows))
    return counts, clashes


def check_table():
    rows = read_table()
    bad = 0
    seen = {}
    for kind, key, name, conf, ev in rows:
        if kind in ('part', 'datum'):      # (readable_symbols.py's)
            continue
        ok = kind in ('function', 'global', 'param', 'ptype', 'record', 'gtype', 'rtype', 'vtype') and conf in ('high', 'medium', 'low') and \
            re.match(r'^[A-Za-z_]\w*$' if kind not in ('ptype', 'gtype', 'rtype', 'vtype') else r'^[A-Za-z_][\w:]*$', name or '') and name not in CPP_RESERVED
        if kind in ('gtype', 'rtype', 'vtype'):
            ok = ok and bool(struct_fields().get(name))
        if kind == 'vtype':
            ok = ok and re.match(r'^0x[0-9A-Fa-f]{6,8}:[A-Za-z_]\w*$', key)
        if kind in ('function', 'global', 'gtype', 'rtype'):
            ok = ok and re.match(r'^0x[0-9A-Fa-f]{6,8}$', key)
        if kind in ('param', 'ptype'):
            ok = ok and re.match(r'^0x[0-9A-Fa-f]{6,8}:\d$' if kind == 'ptype' else r'^0x[0-9A-Fa-f]{6,8}:(?:\d|f?arg\d)$', key)
        if kind == 'record':
            # ADDRESS:ELEMENT TYPE:COUNT, the element a primitive or a struct of a known size
            m = re.match(r'^0x[0-9A-Fa-f]{6,8}:([A-Za-z_][\w:]*):(\d+)$', key)
            ok = ok and m and (m.group(1) in PRIMITIVE_SIZES or m.group(1) in struct_sizes()) and \
                name.startswith('g_')
        if (kind, key) in seen:
            ok = False
        seen[(kind, key)] = name
        if not ok:
            bad += 1
            sys.stderr.write('readable_names.tsv: bad row %r\n' % ((kind, key, name, conf),))
    # A function's name is unique: among the table, the manifests' other names
    # and the readable sources' functions (ee::readable finds functions by name).
    rd = _draft_module()
    table = {int(k, 16): n for kind, k, n, c, _ in rows if kind == 'function' and c in FUNCTION_USED}
    others = {}
    for a, (n, _) in manifest_names().items():
        if a not in table and not machine_function_name(n):
            others.setdefault(n, a)
    for a, r in rd.read_readable(GAME_SRC).items():
        if a not in table:
            others.setdefault(r['name'], a)
    seen_names = {}
    for a, n in sorted(table.items()):
        if n in seen_names or n in others:
            bad += 1
            sys.stderr.write('readable_names.tsv: 0x%08X %s: the name is taken (0x%08X)\n' %
                             (a, n, seen_names.get(n, others.get(n, 0))))
        seen_names[n] = a
    # A global's name is unique too: among the table's global and record rows
    # and the hand-written globals (any namespace). The drafter's batch-file
    # duplicate-name pass renames by text, so two addresses with one name in a
    # file can swap a store's address (names6: g_worldVertexFlags).
    hand = {}
    for a, gs in rd.read_known_names(GAME_SRC)[1].items():
        for g in gs:
            hand.setdefault(g[0], a)
    gseen = {}
    for kind, k, n, c, _ in rows:
        if kind not in ('global', 'record') or c not in FUNCTION_USED:
            continue
        a = int(k.split(':')[0], 16)
        other = gseen.get(n, hand.get(n, a))
        if other != a:
            bad += 1
            sys.stderr.write('readable_names.tsv: global 0x%08X %s: the name is taken (0x%08X)\n' % (a, n, other))
        gseen.setdefault(n, a)
    print('%d rows, %d bad' % (len(rows), bad))
    return bad == 0


def refuse_main(command):
    """A command that writes this tree's tables stops when this tree is the main checkout, unless the orchestrator's
    merge and batch scripts run it (ZAXIS_IN_MAIN=1: round-merge.sh's sync-hand). A lane runs its own worktree's copy
    (tools36: the tooling's zguard.py, for the tools that write into the main checkout from the wrong folder)."""
    main = zaxis_paths.get('ZAXIS_MAIN', required=False)
    if os.environ.get('ZAXIS_IN_MAIN') == '1' or not main or not os.path.isdir(os.path.join(main, '.git')):
        return
    tree = os.path.dirname(os.path.dirname(HERE))
    if os.path.realpath(tree) == os.path.realpath(main):
        sys.stderr.write('readable_names.py %s: refused: it writes %s, the main checkout\'s; only the orchestrator\'s '
                         'merge and batch scripts work there (ZAXIS_IN_MAIN=1). Run your worktree\'s copy; nothing was '
                         'written.\n' % (command, TABLE))
        sys.exit(2)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('apply', 'check', 'infer', 'pin-literals', 'sync-hand'))
    ap.add_argument('sweep', nargs='?', help='infer, pin-literals: a build\'s readable_sweep folder')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--fresh', action='store_true', help='infer: the `uses:` rows voted again (on drafts made without them)')
    args = ap.parse_args()
    if args.command != 'check' and not args.dry_run:
        refuse_main(args.command)
    if args.command == 'infer':
        print('%d inferred parameter rows' % infer_params(args.sweep))
        _STATE.clear()
        print('%d uses rows' % infer_uses(args.sweep, fresh=args.fresh))
        return
    if args.command == 'sync-hand':
        counts, clashes = sync_hand()
        print('%d function, %d global and %d parameter rows take the hand-written name' %
              (counts['function'], counts['global'], counts['param']))
        for c in clashes:
            print('CLASH: ' + c)
        sys.exit(1 if clashes else 0)
    if args.command == 'check':
        sys.exit(0 if check_table() else 1)
    if args.command == 'pin-literals':
        print('%d .lit4 words pinned' % pin_literals(args.sweep, args.dry_run))
        return
    if not check_table():
        sys.exit(1)
    print('%d manifest names written' % apply_functions(args.dry_run))


if __name__ == '__main__':
    main()
