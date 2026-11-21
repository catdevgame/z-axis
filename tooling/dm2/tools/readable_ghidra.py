#!/usr/bin/env python3
"""The readable write-back's input: what the readable rewrite knows about the engine's functions,
written per build as one file Ghidra applies (aggressive-inline ghidra/scripts/ApplyReadable.java, the pipelines'
readable stage; MATCH3.md section 7).

    python -I tools/readable_ghidra.py scan [--src ZAXIS_TREE] [--cache DIR]
        clang over the readable sources: struct layouts, registrations with their C++
        signatures, ee::Global variables (cached per source tree state)
    python -I tools/readable_ghidra.py gen BUILD OUTDIR [--src ZAXIS_TREE] [--cache DIR]
        writes OUTDIR/readable-BUILD.h (the C header for Ghidra's C parser: the structs and
        the prototypes) and OUTDIR/readable-BUILD.jsonl (names, signatures, globals, the
        expected layouts and argument registers ApplyReadable reads back)
    python -I tools/readable_ghidra.py all OUTDIR      gen for the nine builds below
    python -I tools/readable_ghidra.py layouts BUILD [AI_READABLE_DECOMP] [--json]
        which readable structs have AI PS2's layout on the build (layout_verdicts)
    python -I tools/readable_ghidra.py stats BEFORE.c AFTER.c
        before/after counts over two decompilations of one program (FUN_ names, raw offset
        dereferences, field accesses, param_N, DAT_ globals)

BUILD: ai_ps2 (everything: readable registrations, the readable constants that name functions,
Planned names, signatures, structs, globals), dm2_ps2 / bmx_ps2 (the same through identical
matches only: names, signatures and structs), ai_xbox / ai_gc / dm2_xbox / dm2_gc / bmx_xbox /
bmx_gc (names through high and medium counterparts, DM2's and BMX XXX's through their PS2
build; the structs whose layout the build is shown to share, layout_verdicts, and signatures on
high counterparts in the build's own convention where Ghidra's analysis takes the same
arguments, signature_other). The layout check reads AI PS2's readable decompilation
(AI_READABLE_DECOMP, default the readable exports folder's, tools/exports.py), the signature
check each build's base decompilation.

Sources: the work database (workdb.py, read only: readable, readable_symbols, counterparts,
best_names) and the readable sources themselves (clang's -fdump-record-layouts-complete and
its JSON AST of the namespaces aggressive, zge and sce; the compile flags come from a
compile_commands.json under ZAXIS_TREE/out). The output holds the games' addresses: keep it in
scratch, never commit it.
"""
import concurrent.futures, glob, hashlib, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import zpaths  # noqa: E402  (tooling/paths.env)
VENV_PY = os.path.join(zpaths.ZAXIS_SCRATCH, 'venv', 'bin', 'python')
try:
    import duckdb
except ImportError:
    if os.path.exists(VENV_PY) and os.path.realpath(sys.prefix) != \
            os.path.realpath(os.path.dirname(os.path.dirname(VENV_PY))):
        os.execv(VENV_PY, [VENV_PY, '-I', os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit('readable_ghidra: needs the duckdb module (%s)' % VENV_PY)

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser('~')
SCRATCH = os.environ.get('AI_SCRATCH', zpaths.ZAXIS_SCRATCH)
WORKDB = os.environ.get('WORKDB', os.environ.get('WORKDB_DIR', SCRATCH + '/workdb') + '/work.duckdb')
SRC = os.environ.get('ZAXIS_TREE', zpaths.ZAXIS_MAIN)
CACHE = os.environ.get('READABLE_GHIDRA_CACHE', SCRATCH + '/ghidranames/cache')
NAMESPACES = ('aggressive', 'zge', 'sce')
BUILDS = ('ai_ps2', 'dm2_ps2', 'bmx_ps2', 'ai_xbox', 'ai_gc', 'dm2_xbox', 'dm2_gc', 'bmx_xbox', 'bmx_gc')
# what each build takes: counterparts accepted, and whether signatures and types apply
CARRY = {
    'ai_ps2': (None, True),
    'dm2_ps2': ("cls = 'identical'", True),
    'bmx_ps2': ("cls = 'identical'", True),
    'ai_xbox': ("confidence IN ('high', 'medium')", True),
    'ai_gc': ("confidence IN ('high', 'medium')", True),
    # DM2's and BMX XXX's Xbox and GameCube builds: through their own PS2 build's pairing (the
    # counterparts' via; the confidence is the lower of the two links)
    'dm2_xbox': ("confidence IN ('high', 'medium')", True),
    'dm2_gc': ("confidence IN ('high', 'medium')", True),
    'bmx_xbox': ("confidence IN ('high', 'medium')", True),
    'bmx_gc': ("confidence IN ('high', 'medium')", True),
}
VERSION = 1
# AI PS2's readable decompilation (the layout check's evidence; MATCH3.md section 7)
AI_READABLE_DECOMP = os.environ.get('AI_READABLE_DECOMP', os.environ.get(
    'READABLE_EXPORTS_DIR', SCRATCH + '/readable-exports') + '/ai_ps2/decomp.c')


def log(*a):
    print('readable_ghidra:', *a, file=sys.stderr, flush=True)


# ------------------------------------------------------------------ clang

def compile_flags(src):
    """The game TUs and their flags (from the newest compile_commands.json under src/out)."""
    dbs = sorted(glob.glob(os.path.join(src, 'out', '*', 'compile_commands.json')),
                 key=os.path.getmtime, reverse=True)
    for db in dbs:
        tus = []
        for e in json.load(open(db)):
            f = e['file']
            if '/src/game/' not in f or not f.endswith('.cpp'):
                continue
            args = e.get('arguments') or e['command'].split()
            rel = f[re.search(r'runtime/src/game/', f).start():]
            out, skip = [], False
            for a in args[1:]:
                if skip:
                    skip = False
                    continue
                if a in ('-o', '-c'):
                    skip = True
                    continue
                if a.startswith(('-O', '-g', '-arch')) or a == 'arm64' or a == 'x86_64':
                    continue
                # paths in the checkout the database was made for: point them at src
                m = re.match(r'(-I|-isystem)?(.*/(?:PS2Recomp|z-axis))(/(?:runtime|iop)/.*)$', a)
                if m:
                    a = (m.group(1) or '') + src + m.group(3)
                out.append(a)
            tus.append((os.path.join(src, rel), out))
        if tus:
            return db, sorted(set((t, tuple(f)) for t, f in tus))
    sys.exit('readable_ghidra: no compile_commands.json with the game sources under %s/out' % src)


def tree_state(src):
    """A digest of the readable sources (game/ and ee/ files), for the cache and the output."""
    h = hashlib.sha256()
    root = zpaths.tree_path(src, 'runtime', 'src', 'game')
    for p in sorted(glob.glob(root + '/**/*', recursive=True)):
        if p.endswith(('.h', '.cpp')):
            h.update(p[len(root):].encode())
            h.update(open(p, 'rb').read())
    try:
        commit = subprocess.run(['git', '-C', src, 'log', '-1', '--format=%h %s'], capture_output=True,
                                text=True).stdout.strip()
    except OSError:
        commit = ''
    return h.hexdigest(), commit


def json_objects(text):
    dec, i, n = json.JSONDecoder(), 0, len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        o, i = dec.raw_decode(text, i)
        yield o


LAYOUT_HEAD = re.compile(r'^\s*(\d+) \| (struct|class|union) (.*)$')
LAYOUT_FIELD = re.compile(r'^\s*(\d+)(?::\d+-\d+)? \|( +)(.*?) (\w+)$')
LAYOUT_SIZE = re.compile(r'^\s*\| \[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)')


def parse_layouts(text):
    """clang -fdump-record-layouts-complete: {qualified name: (size, align, [(off, type, name)])}."""
    out, cur = {}, None
    for ln in text.splitlines():
        if ln.startswith('*** Dumping AST Record Layout'):
            cur = None
            continue
        m = LAYOUT_HEAD.match(ln)
        if m and cur is None and m.group(1) == '0':
            cur = {'kind': m.group(2), 'name': m.group(3).strip(), 'fields': [], 'bad': False}
            continue
        if cur is None:
            continue
        m = LAYOUT_SIZE.match(ln)
        if m:
            out.setdefault(cur['name'], (int(m.group(1)), int(m.group(2)), cur['fields'], cur['kind'],
                                         cur['bad']))
            cur = None
            continue
        m = LAYOUT_FIELD.match(ln)
        if m:
            depth = len(m.group(2))
            if depth == 3:
                if ':' in ln.split('|')[0]:
                    cur['bad'] = True  # bit-field
                cur['fields'].append((int(m.group(1)), m.group(3).strip(), m.group(4)))
        elif '| (' in ln and 'vtable pointer' in ln or '(primary base)' in ln or '(base)' in ln:
            cur['bad'] = True
    return out


def walk_ast(node, ns, sink):
    k = node.get('kind')
    name = node.get('name')
    if k == 'NamespaceDecl':
        ns = ns + [name or '(anonymous namespace)']
    elif k in ('CXXRecordDecl', 'RecordDecl') and name and node.get('completeDefinition'):
        ns2 = ns + [name]
        sink['records'].add('::'.join(ns2))
        for c in node.get('inner', []) or []:
            walk_ast(c, ns2, sink)
        return
    elif k == 'EnumDecl' and name:
        q = '::'.join(ns + [name])
        t = node.get('fixedUnderlyingType', {})
        consts = []
        for c in node.get('inner', []) or []:
            if c.get('kind') == 'EnumConstantDecl':
                consts.append(c['name'])
        sink['enums'][q] = {'underlying': t.get('desugaredQualType') or t.get('qualType') or 'int',
                            'constants': consts}
        return
    elif k in ('TypedefDecl', 'TypeAliasDecl') and name:
        t = node.get('type', {})
        sink['typedefs']['::'.join(ns + [name])] = t.get('desugaredQualType') or t.get('qualType')
        return
    elif k == 'FunctionDecl':
        params = [(c.get('name') or '', c['type'].get('desugaredQualType') or c['type']['qualType'])
                  for c in node.get('inner', []) or [] if c.get('kind') == 'ParmVarDecl']
        ret = node['type'].get('desugaredQualType') or node['type']['qualType']
        sink['functions'][node['id']] = {'name': '::'.join(ns + [name]), 'type': ret, 'params': params}
        return  # bodies are not needed
    elif k == 'VarDecl' and name:
        if name.startswith('g_readableRegistration_'):
            sink['regs'].append(registration(node))
            return
        t = node.get('type', {})
        dt = t.get('desugaredQualType') or t.get('qualType') or ''
        if dt.startswith(('ee::Global<', 'const ee::Global<')):
            sink['globals'].append({'name': name, 'ns': '::'.join(ns), 'type': dt})
        return
    for c in node.get('inner', []) or []:
        walk_ast(c, ns, sink)


def symbol_address(name):
    """A sym:: constant's address (runtime/tools/readable_symbols.py)."""
    sys.path.insert(0, os.environ.get('READABLE_TOOLS', zpaths.tree_path(SRC, 'runtime', 'tools')))
    import readable_symbols
    entry = readable_symbols.table().syms.get(name)
    return entry[1] if entry else None


def registration(node):
    ints, strs, fns = [], [], []

    def w(n):
        k = n.get('kind')
        if k == 'IntegerLiteral':
            ints.append(int(n['value']))
        elif k == 'StringLiteral':
            strs.append(json.loads(n['value']))
        elif k == 'DeclRefExpr' and n.get('referencedDecl', {}).get('kind') == 'FunctionDecl':
            fns.append(n['referencedDecl'])
        elif k == 'DeclRefExpr' and n.get('referencedDecl', {}).get('kind') == 'VarDecl':
            # READABLE_FUNCTION's address: a sym:: constant
            address = symbol_address(n['referencedDecl'].get('name'))
            if address is not None:
                ints.append(address)
        for c in n.get('inner', []) or []:
            w(c)
    w(node)
    name = strs[0] if strs else None
    fn = None
    for f in fns:
        if name and f['name'] == name.split('::')[-1]:
            fn = f
    return {'addr': ints[0] if ints else None, 'name': name, 'file': strs[1] if len(strs) > 1 else None,
            'fn': fn and fn['id'], 'fntype': fn and fn['type'].get('qualType')}


def scan_tu(args):
    tu, flags, src = args
    sink = {'records': set(), 'enums': {}, 'functions': {}, 'regs': [], 'globals': [], 'typedefs': {}}
    for ns in NAMESPACES:
        p = subprocess.run(['clang++'] + list(flags) + ['-fsyntax-only', '-w', '-Xclang', '-ast-dump=json',
                            '-Xclang', '-ast-dump-filter=' + ns, tu], capture_output=True, text=True, cwd=src)
        if p.returncode != 0:
            return tu, None, p.stderr[-2000:]
        for o in json_objects(p.stdout):
            walk_ast(o, [], sink)
    p = subprocess.run(['clang++'] + list(flags) + ['-fsyntax-only', '-w', '-Xclang', '-fdump-record-layouts-complete',
                        tu], capture_output=True, text=True, cwd=src)
    if p.returncode != 0:
        return tu, None, p.stderr[-2000:]
    lay = {k: v for k, v in parse_layouts(p.stdout).items()
           if k.split('::')[0].split('<')[0] in NAMESPACES + ('ee',)}
    sink['records'] = sorted(sink['records'])
    sink['layouts'] = lay
    return tu, sink, None


def scan(src=SRC, cache=CACHE):
    digest, commit = tree_state(src)
    path = os.path.join(cache, 'scan-v%d-%s.json' % (VERSION, digest[:16]))
    if os.path.exists(path):
        return json.load(open(path))
    db, tus = compile_flags(src)
    log('scan: %d translation units (%s)' % (len(tus), db))
    S = {'digest': digest, 'commit': commit, 'records': set(), 'enums': {}, 'typedefs': {}, 'regs': {},
         'globals': {}, 'layouts': {}, 'errors': []}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for tu, s, err in ex.map(scan_tu, [(t, f, src) for t, f in tus]):
            if s is None:
                S['errors'].append((tu, err))
                log('clang failed on', tu, err[-400:])
                continue
            S['records'].update(s['records'])
            S['enums'].update(s['enums'])
            S['typedefs'].update(s['typedefs'])
            S['layouts'].update(s['layouts'])
            for r in s['regs']:
                f = s['functions'].get(r['fn'])
                if r['addr'] is None:
                    continue
                r = dict(r, decl=f)
                S['regs'][str(r['addr'])] = r
            for g in s['globals']:
                S['globals'][g['ns'] + '::' + g['name']] = g
    S['records'] = sorted(S['records'])
    os.makedirs(cache, exist_ok=True)
    json.dump(S, open(path + '.tmp', 'w'))
    os.replace(path + '.tmp', path)
    log('scan: %d records with layouts, %d registrations, %d globals, %d errors' % (
        len([r for r in S['records'] if r in S['layouts']]), len(S['regs']), len(S['globals']), len(S['errors'])))
    return S


# ------------------------------------------------------------------ C types

PRIM = {
    'bool': ('bool', 1), 'char': ('char', 1), 'signed char': ('char', 1), 'unsigned char': ('uchar', 1),
    'short': ('short', 2), 'unsigned short': ('ushort', 2), 'int': ('int', 4), 'unsigned int': ('uint', 4),
    'long': ('longlong', 8), 'unsigned long': ('ulonglong', 8), 'long long': ('longlong', 8),
    'unsigned long long': ('ulonglong', 8), 'float': ('float', 4),
    'int8_t': ('char', 1), 'uint8_t': ('uchar', 1), 'int16_t': ('short', 2), 'uint16_t': ('ushort', 2),
    'int32_t': ('int', 4), 'uint32_t': ('uint', 4), 'int64_t': ('longlong', 8), 'uint64_t': ('ulonglong', 8),
    'size_t': ('uint', 4), 'ee::Float': ('float', 4), 'const char': ('char', 1), '_Bool': ('bool', 1),
}
# Ghidra's names for the primitives above, spelled as its C parser takes them
CSPELL = {'uchar': 'unsigned char', 'ushort': 'unsigned short', 'uint': 'unsigned int',
          'longlong': 'long long', 'ulonglong': 'unsigned long long'}


def split_template(s):
    """'ee::Ptr<X<a, b>>' -> ('ee::Ptr', ['X<a, b>'])"""
    i = s.index('<')
    head, body = s[:i], s[i + 1:s.rindex('>')]
    args, depth, cur = [], 0, ''
    for ch in body:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    args.append(cur.strip())
    return head, args


class Types:
    """Readable C++ types to C for Ghidra: guest records (EE memory layouts) and what refers to them."""

    def __init__(self, S, allowed=None):
        self.S = S
        self.allowed = allowed  # C names whose layout holds on the build (None: all)
        self.lay = S['layouts']
        self.enums = S['enums']
        self.typedefs = {q: t for q, t in S.get('typedefs', {}).items() if q not in S['layouts']}
        self.by_short = {}
        for q in list(self.lay) + list(self.enums) + list(self.typedefs):
            if '<' not in q:
                self.by_short.setdefault(q.split('::')[-1], []).append(q)
        self.cname = {}
        self.structs = {}  # qualified -> spec
        self.reject = {}
        self.asserted = asserted_records(SRC_ROOT[0])
        cands = [q for q in self.lay if q.split('::')[0] in NAMESPACES and '<' not in q
                 and not q.startswith('zge::native')]
        short = {}
        for q in cands:
            short.setdefault(self.short(q), []).append(q)
        for s, qs in short.items():
            for q in qs:
                self.cname[q] = s if len(qs) == 1 else self.qualified_c(q)
        for q in sorted(cands):
            self.record(q)

    @staticmethod
    def short(q):
        return q.split('::')[-1]

    @staticmethod
    def qualified_c(q):
        parts = [p for p in q.split('::') if not p.startswith('(')]
        return '_'.join(parts[-2:]) if len(parts) > 1 else parts[0]

    @staticmethod
    def norm(t):
        t = re.sub(r'\b(struct|class|union|enum) ', '', t).strip()
        return t

    def record(self, q):
        if q in self.structs:
            return self.structs[q]
        if q in self.reject:
            return None
        if q not in self.lay:
            self.reject[q] = 'no layout'
            return None
        if self.allowed is not None and self.cname[q] not in self.allowed:
            self.reject[q] = 'layout not shown on this build'
            return None
        self.reject[q] = 'recursive'  # until done
        size, align, fields, kind, bad = self.lay[q]
        if bad:
            self.reject[q] = 'bases, vtable or bit-fields'
            return None
        out = []
        for off, t, name in fields:
            c = self.ctype(t, q)
            if c is None:
                self.reject[q] = 'field %s: %s' % (name, t)
                return None
            out.append((off, name, c[0], c[1]))
        del self.reject[q]
        spec = {'q': q, 'c': self.cname[q], 'size': size, 'kind': kind, 'fields': out,
                'asserted': self.short(q) in self.asserted}
        self.structs[q] = spec
        return spec

    def resolve(self, t, ctx):
        """A name as clang printed it (often as written, unqualified) -> the qualified name of a
        known record, enum or typedef: looked up from the innermost scope of ctx outward, else by
        a unique last component."""
        known = lambda q: q in self.lay or q in self.enums or q in self.typedefs
        if known(t):
            return t
        parts = ctx.split('::') if ctx else []
        for i in range(len(parts), -1, -1):
            q = '::'.join(parts[:i] + [t])
            if known(q):
                return q
        last = t.split('::')[-1]
        hits = self.by_short.get(last, [])
        if len(hits) == 1:
            return hits[0]
        # several: the one sharing the most leading scope with ctx
        if hits:
            def common(q):
                n = 0
                for a, b in zip(q.split('::'), parts):
                    if a != b:
                        break
                    n += 1
                return n
            hits = sorted(hits, key=common, reverse=True)
            if common(hits[0]) > common(hits[1]):
                return hits[0]
        return t

    def ctype(self, t, ctx='', param=False):
        """C++ type -> (C type text with %s for the declarator, size) or None. A parameter's
        host pointer is the guest address ee::entry converts (ee/abi.h)."""
        t = self.norm(t)
        if t.startswith('const '):
            t = t[6:]
        if t.startswith('volatile '):
            t = t[9:]
        if param and t.endswith('*') and 'std::' not in t:
            return (self.pointee(t[:-1].strip(), ctx) + ' *%s', 4)
        if t.endswith('*') or t.endswith('&') or 'std::' in t:
            return None  # a host pointer: not an EE layout
        m = re.match(r'^(.*)\[(\d+)\]$', t)
        if m:
            e = self.ctype(m.group(1), ctx)
            if e is None:
                return None
            n = int(m.group(2))
            return (e[0].replace('%s', '%s[%d]' % ('%s', n)), e[1] * n)
        if t in PRIM:
            c, n = PRIM[t]
            return (CSPELL.get(c, c) + ' %s', n)
        if t.startswith('ee::Ptr<'):
            _, (a,) = split_template(t)
            return (self.pointee(a, ctx) + ' *%s', 4)
        q = self.resolve(t, ctx)
        if q in self.typedefs:
            return self.ctype(self.typedefs[q], '::'.join(q.split('::')[:-1]))
        if q in self.enums:
            u = self.norm(self.enums[q]['underlying'])
            c, n = PRIM.get(u, ('int', 4))
            return (CSPELL.get(c, c) + ' %s', n)
        if q in self.cname:
            s = self.record(q)
            if s is None:
                return None
            return ('%s %%s' % s['c'], s['size'])
        return None

    def pointee(self, a, ctx=''):
        a = self.norm(a)
        for pre in ('const ', 'volatile '):
            if a.startswith(pre):
                a = a[len(pre):]
        if a in PRIM:
            c, _ = PRIM[a]
            return CSPELL.get(c, c)
        if a.startswith('ee::Ptr<'):
            _, (b,) = split_template(a)
            return self.pointee(b, ctx) + ' *'
        m = re.match(r'^(.*)\[(\d+)\]$', a)
        if m:
            return self.pointee(m.group(1), ctx)
        q = self.resolve(a, ctx)
        if q in self.typedefs:
            return self.pointee(self.typedefs[q], '::'.join(q.split('::')[:-1]))
        if q in self.cname:
            s = self.record(q)
            if s:
                return s['c']
        if q in self.enums:
            u = self.norm(self.enums[q]['underlying'])
            c, _ = PRIM.get(u, ('int', 4))
            return CSPELL.get(c, c)
        return 'void'

    def decl(self, ctext, name):
        return ctext.replace('%s', name) if '%s' in ctext else '%s %s' % (ctext, name)


SRC_ROOT = [SRC]


def asserted_records(src):
    names = set()
    root = zpaths.tree_path(src, 'runtime', 'src', 'game')
    for p in glob.glob(root + '/**/*', recursive=True):
        if not p.endswith(('.h', '.cpp')):
            continue
        for ln in open(p, errors='replace'):
            if 'static_assert' in ln:
                names.update(re.findall(r'(?:offsetof|sizeof)\((\w+)', ln))
    return names


# ------------------------------------------------------------------ the ABI

def eabi_storage(params):
    """The EE's EABI as ee/abi.h counts it: ints and pointers a0-a7, floats f12-f19."""
    ni = nf = 0
    out = []
    for name, c, size in params:
        if c.strip() == 'float %s' or c.strip() == 'float':
            out.append('f%d' % (12 + nf) if nf < 8 else 'stack')
            nf += 1
        elif size <= 8:
            regs = ['a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3']
            out.append(regs[ni] if ni < 8 else 'stack')
            ni += 1
        else:
            out.append('stack')
    return out


# ------------------------------------------------------------------ names

def ident(s):
    s = re.sub(r'\W', '_', s)
    return s if s and not s[0].isdigit() else '_' + s


def constant_name(n):
    """kCollisionWorld (a readable constant naming a function) -> collisionWorld"""
    if re.match(r'^k[A-Z]', n):
        n = n[1].lower() + n[2:]
    return n


def gen(build, outdir, src=SRC, cache=CACHE):
    if build not in CARRY:
        sys.exit('readable_ghidra: unknown build %s (%s)' % (build, ', '.join(BUILDS)))
    SRC_ROOT[0] = src
    S = scan(src, cache)
    T = Types(S)
    con = duckdb.connect(WORKDB, read_only=True)
    q = lambda sql, *a: con.execute(sql, list(a)).fetchall()
    built = q("SELECT value FROM meta WHERE key = 'built'")
    # AI's names: registrations (and Planned), else a readable constant naming the function
    ai = {}
    for addr, name, kind, status, f, line in q(
            "SELECT addr, name, kind, status, file, line FROM readable"):
        ai[addr] = {'n': name.split('::')[-1], 'src': 'planned' if kind == 'PLANNED' else 'readable',
                    'ev': '%s %s:%d' % (status, f, line)}
    for addr, name, f, line in q("""
            SELECT s.addr, s.name, s.file, s.line FROM readable_symbols s
            JOIN functions f ON f.build = 'ai_ps2' AND f.addr = s.addr
            WHERE s.kind = 'constant' ORDER BY s.file, s.line"""):
        if addr not in ai:
            ai[addr] = {'n': constant_name(name), 'src': 'readable_symbol', 'ev': '%s:%d %s' % (f, line, name)}
    # which AI functions have a signature (registered and implemented)
    regs = {int(a): r for a, r in S['regs'].items() if r.get('decl')}
    where, with_types = CARRY[build]
    plat = PLATFORM[build]
    verdicts = gsigs = None
    if plat != 'ps2':
        # struct layouts shown per build (layout_verdicts); Ghidra's own signatures of the build
        verdicts = layout_verdicts(build, AI_READABLE_DECOMP, struct_records(T), con)
        T = Types(S, {n for n, v in verdicts.items() if v['verdict'] in ('same', 'small')})
        gsigs = ghidra_signatures(BASE_DECOMP[build])
    if build == 'ai_ps2':
        target = {a: (a, 'self', None) for a in ai}
    else:
        target = {}
        rows = q("""SELECT ai_addr, addr, confidence, cls, via FROM counterparts
                    WHERE build = ? AND NOT inlined AND %s""" % where, build)
        by_t = {}
        for ai_addr, addr, conf, cls, via in rows:
            if ai_addr in ai:
                by_t.setdefault(addr, []).append((ai_addr, conf, cls, via))
        rank = {'high': 0, 'medium': 1, 'low': 2}
        for addr, cs in by_t.items():
            cs.sort(key=lambda c: (rank.get(c[1], 3), c[0]))
            best = [c for c in cs if rank.get(c[1], 3) == rank.get(cs[0][1], 3)]
            names = {ai[c[0]]['n'] for c in best}
            if len(names) > 1:
                # several AI functions with different names fold into one: name it after none
                continue
            target[cs[0][0]] = (addr, cs[0][1] + (' ' + cs[0][2] if cs[0][2] and cs[0][2] != cs[0][1] else ''),
                                cs[0][3])
    funcs = {}
    for ai_addr, (addr, conf, via) in target.items():
        funcs.setdefault(addr, []).append(ai_addr)
    # duplicate names get the address
    count = {}
    for addr, ais in funcs.items():
        count[ai[ais[0]]['n']] = count.get(ai[ais[0]]['n'], 0) + 1
    recs, protos = [], []
    for addr in sorted(funcs):
        a0 = funcs[addr][0]
        info = ai[a0]
        n = ident(info['n'])
        if count[info['n']] > 1:
            n = '%s_%08X' % (n, addr)
        _, conf, via = target[a0]
        r = {'k': 'fn', 'a': addr, 'n': n, 'src': info['src'], 'ai': a0, 'conf': conf, 'ev': info['ev']}
        if via:
            r['via'] = via
        recs.append(r)
        reg = regs.get(a0)
        if with_types and reg and info['src'] == 'readable' and (plat == 'ps2' or conf.startswith('high')):
            sig = signature(T, reg, addr) if plat == 'ps2' else signature_other(T, reg, addr, plat, gsigs.get(addr))
            if sig.get('proto'):
                protos.append(sig['proto'])
            sig.pop('proto', None)
            recs.append(sig)
    # globals (AI PS2 only: their addresses are AI's)
    if build == 'ai_ps2':
        # the same global declared in several files (or under two names): one record per
        # address, the other names kept as aliases (labels)
        by_a = {}
        for qn, g in sorted(S['globals'].items()):
            r = global_rec(T, g)
            if 'a' not in r:
                recs.append(r)
                continue
            by_a.setdefault(r['a'], []).append(r)
        for a in sorted(by_a):
            rs = sorted(by_a[a], key=lambda r: (not r.get('t'), -(r.get('size') or 0), r['n']))
            r = rs[0]
            aliases = sorted({x['n'] for x in rs[1:]} - {r['n']})
            if aliases:
                r['aliases'] = aliases
            if r.get('t'):
                r['tname'] = 'readable_gtype_%08X' % r['a']
                protos.append('typedef %s;' % T.decl(r['ctext'], r['tname']))
            for x in rs:
                x.pop('ctext', None)
            recs.append(r)
    structs = [s for s in T.structs.values()]
    structs.sort(key=lambda s: s['c'])
    header = c_header(T, structs, protos, build, S)
    os.makedirs(outdir, exist_ok=True)
    hpath = os.path.join(outdir, 'readable-%s.h' % build)
    open(hpath, 'w').write(header)
    hsha = hashlib.sha256(header.encode()).hexdigest()
    if verdicts is not None:
        json.dump(verdicts, open(os.path.join(outdir, 'layouts-%s.json' % build), 'w'), indent=1, sort_keys=True)
    meta = {'k': 'meta', 'version': VERSION, 'build': build, 'header': os.path.basename(hpath),
            'header_sha256': hsha, 'readable_digest': S['digest'], 'readable_commit': S['commit'],
            'workdb_built': built[0][0] if built else None, 'types': with_types}
    lines = [meta]
    if with_types:
        for s in structs:
            lines.append({'k': 'struct', 'n': s['c'], 'q': s['q'], 'size': s['size'], 'asserted': s['asserted'],
                          'fields': [[o, n, T.decl(c, '').strip(), sz] for o, n, c, sz in s['fields']]})
    lines += recs
    jpath = os.path.join(outdir, 'readable-%s.jsonl' % build)
    with open(jpath + '.tmp', 'w') as w:
        for r in lines:
            w.write(json.dumps(r, sort_keys=True) + '\n')
    os.replace(jpath + '.tmp', jpath)
    nf = sum(1 for r in recs if r['k'] == 'fn')
    ns = sum(1 for r in recs if r['k'] == 'sig')
    ng = sum(1 for r in recs if r['k'] == 'global')
    nskip = sum(1 for r in recs if r['k'] == 'sig' and 'skip' in r)
    log('%s: %d names, %d signatures (%d skipped), %d structs (%d asserted), %d globals -> %s' % (
        build, nf, ns, nskip, len(structs) if with_types else 0,
        sum(s['asserted'] for s in structs) if with_types else 0, ng, jpath))
    if verdicts is not None:
        vc = {}
        for v in verdicts.values():
            vc[v['verdict']] = vc.get(v['verdict'], 0) + 1
        log('%s: struct layouts %s' % (build, ', '.join('%s %d' % kv for kv in sorted(vc.items()))))
        why = {}
        for r in recs:
            if r['k'] == 'sig' and 'skip' in r:
                k = r['skip'].split(':')[0]
                why[k] = why.get(k, 0) + 1
        log('%s: signatures skipped: %s' % (build, ', '.join('%s %d' % kv for kv in sorted(why.items()))))
    return jpath


def signature(T, reg, addr):
    d = reg['decl']
    ctx = '::'.join(d['name'].split('::')[:-1])
    ps, bad = [], []
    for i, (pn, pt) in enumerate(d['params']):
        c = T.ctype(pt, ctx, True)
        if c is None:
            bad.append(pt)
            c = ('unsigned int %s', 4)
        ps.append((ident(pn or 'arg%d' % i), c[0], c[1]))
    rt = d['type']
    rt = rt[:rt.index('(')].strip()
    if rt == 'void':
        rc = ('void %s', 0)
    else:
        rc = T.ctype(rt, ctx, True)
        if rc is None:
            bad.append(rt)
            rc = ('unsigned int %s', 4)
    store = eabi_storage(ps)
    if bad or 'stack' in store:
        return {'k': 'sig', 'a': addr, 'skip': 'types %s' % ', '.join(bad) if bad else 'stack arguments'}
    pname = 'readable_sig_%08X' % addr
    args = ', '.join(T.decl(c, n) for n, c, _ in ps) or 'void'
    proto = '%s(%s);' % (T.decl(rc[0], pname), args)
    return {'k': 'sig', 'a': addr, 'proto_name': pname, 'cxx': reg.get('fntype'),
            'ret': T.decl(rc[0], '').strip(), 'params': [[n, T.decl(c, '').strip(), sz] for n, c, sz in ps],
            'storage': store, 'proto': proto}


PLATFORM = {'ai_ps2': 'ps2', 'dm2_ps2': 'ps2', 'bmx_ps2': 'ps2', 'ai_xbox': 'xbox', 'dm2_xbox': 'xbox',
            'bmx_xbox': 'xbox', 'ai_gc': 'gc', 'dm2_gc': 'gc', 'bmx_gc': 'gc'}
# each build's decompilation as analysed (Ghidra's own signatures: the check for signature_other)
sys.path.insert(0, HERE)
import exports  # noqa: E402
BASE_DECOMP = {b: exports.path(b, 'decomp', readable=False) for b in PLATFORM}
XBOX_CC = ('__cdecl', '__stdcall', '__fastcall')


def ghidra_signatures(path):
    """An ExportDecomp file -> {address: (convention, return type, [parameter types])}."""
    out = {}
    if not path or not os.path.exists(path):
        log('no base decompilation %s: no signatures on this build' % path)
        return out
    for a, (n, text) in load_decomp(path).items():
        m = SIG_LINE.search(text)
        if not m:
            continue
        head, args = m.group(1).split(), m.group(3).strip()
        cc = head[-1] if head and head[-1].startswith('__') else None
        ret = ' '.join(h for h in head if not h.startswith('__'))
        ps = [] if args in ('', 'void') else [re.sub(r'\s*\b\w+$', '', x.strip()) for x in args.split(',')]
        out[a] = (cc, ret, ps)
    return out


def arg_kind(t, plat):
    """A C type as Ghidra prints it -> f (floating point register or 4-byte stack float), i (a
    32-bit integer or pointer), w (anything wider: 8-byte integers, x86 doubles), v (void)."""
    t = t.strip()
    if t in ('void', ''):
        return 'v'
    if '*' in t:
        return 'i'
    if t in ('float', 'float10') or (t == 'double' and plat == 'gc'):
        return 'f'
    if t in ('double', 'undefined8', 'longlong', 'ulonglong', 'long long', 'unsigned long long', '...'):
        return 'w'
    return 'i'


def signature_other(T, reg, addr, plat, gsig):
    """A readable signature on an Xbox or GameCube build. It keeps the build's own convention:
    GameCube, the PowerPC EABI (integers and pointers r3-r10, floating point f1-f8, floats passed
    as doubles); Xbox, the convention Ghidra found (__cdecl, __stdcall: all on the stack;
    __fastcall: the first two integers in ECX and EDX). Applied only where Ghidra's analysis of
    the counterpart takes the same arguments in the same places (count and kind), and the return
    kinds do not contradict (void against a value is allowed: Ghidra's guess; an 8-byte return is
    not, the EE's 64-bit long being 32 bits on these builds)."""
    d = reg['decl']
    ctx = '::'.join(d['name'].split('::')[:-1])
    ps, bad = [], []
    for i, (pn, pt) in enumerate(d['params']):
        c = T.ctype(pt, ctx, True)
        if c is None:
            bad.append(pt)
            c = ('unsigned int %s', 4)
        if plat == 'gc' and c[0].strip() == 'float %s':
            c = ('double %s', 8)
        ps.append((ident(pn or 'arg%d' % i), c[0], c[1]))
    rt = d['type']
    rt = rt[:rt.index('(')].strip()
    if rt == 'void':
        rc = ('void %s', 0)
    else:
        rc = T.ctype(rt, ctx, True)
        if rc is None:
            bad.append(rt)
            rc = ('unsigned int %s', 4)
        elif plat == 'gc' and rc[0].strip() == 'float %s':
            rc = ('double %s', 8)
    if bad:
        return {'k': 'sig', 'a': addr, 'skip': 'types: %s' % ', '.join(bad)}
    if gsig is None:
        return {'k': 'sig', 'a': addr, 'skip': 'no Ghidra signature'}
    gcc, gret, gps = gsig
    mine = [arg_kind(T.decl(c, '').strip(), plat) for _, c, sz in ps]
    mine = ['w' if k == 'i' and sz > 4 else k for k, (_, _, sz) in zip(mine, ps)]
    theirs = [arg_kind(t, plat) for t in gps]
    if 'w' in mine or 'w' in theirs:
        return {'k': 'sig', 'a': addr, 'skip': 'wide arguments: %s against %s' % (''.join(mine), ''.join(theirs))}
    cc = None
    if plat == 'gc':
        if sorted(mine) != sorted(theirs):
            return {'k': 'sig', 'a': addr, 'skip': 'arguments differ: %s against Ghidra %s' % (''.join(mine), ''.join(theirs))}
        ni = nf = 0
        store = []
        for k in mine:
            if k == 'f':
                store.append('f%d' % (1 + nf) if nf < 8 else 'stack')
                nf += 1
            else:
                store.append('r%d' % (3 + ni) if ni < 8 else 'stack')
                ni += 1
    else:
        if gcc not in XBOX_CC:
            return {'k': 'sig', 'a': addr, 'skip': 'convention: %s' % (gcc or 'none (registers of its own)')}
        cc = gcc

        def slots(kinds):
            regs = ['ECX', 'EDX'] if cc == '__fastcall' else []
            out, n = [], 0
            for k in kinds:
                if k == 'i' and regs:
                    out.append(regs.pop(0))
                else:
                    out.append('stack')
            return out
        store = slots(mine)
        key = lambda kinds: ([(r, k) for r, k in zip(slots(kinds), kinds) if r != 'stack'],
                             [k for r, k in zip(slots(kinds), kinds) if r == 'stack'])
        if key(mine) != key(theirs):
            return {'k': 'sig', 'a': addr, 'skip': 'arguments differ: %s against Ghidra %s %s' % (
                ''.join(mine), gcc, ''.join(theirs))}
    rk, gk = arg_kind(T.decl(rc[0], '').strip(), plat), arg_kind(gret, plat)
    # a wide return (the EE's 64-bit long) is 32 bits in these builds' source: not applied
    if rk == 'w' or rk != 'v' and gk != 'v' and (gk == 'w' or rk != gk):
        return {'k': 'sig', 'a': addr, 'skip': 'return differs: %s against Ghidra %s' % (rk, gret)}
    if 'stack' in store and plat == 'gc':
        return {'k': 'sig', 'a': addr, 'skip': 'stack arguments'}
    pname = 'readable_sig_%08X' % addr
    args = ', '.join(T.decl(c, n) for n, c, _ in ps) or 'void'
    proto = '%s(%s);' % (T.decl(rc[0], pname), args)
    r = {'k': 'sig', 'a': addr, 'proto_name': pname, 'cxx': reg.get('fntype'),
         'ret': T.decl(rc[0], '').strip(), 'params': [[n, T.decl(c, '').strip(), sz] for n, c, sz in ps],
         'storage': store, 'proto': proto, 'ghidra': '%s %s(%s)' % (gret, gcc or '', ', '.join(gps))}
    if cc:
        r['cc'] = cc
    return r


def global_rec(T, g):
    _, args = split_template(g['type'].replace('const ee::Global<', 'ee::Global<'))
    a = args[-1].rstrip('uUlL')
    try:
        addr = int(a, 0)
    except ValueError:
        if a.startswith('sym::') and symbol_address(a[5:]) is not None:
            addr = symbol_address(a[5:])
        else:
            return {'k': 'global', 'n': g['name'], 'skip': 'address %s' % a}
    c = T.ctype(', '.join(args[:-1]), g['ns'])
    r = {'k': 'global', 'a': addr, 'n': g['name']}
    if c is None:
        r['skip_type'] = ', '.join(args[:-1])
    else:
        r['t'] = T.decl(c[0], '').strip()
        r['ctext'] = c[0]
        r['size'] = c[1]
    return r


def c_header(T, structs, protos, build, S):
    out = ['/* Readable structs and signatures for Ghidra (tools/readable_ghidra.py, build %s).' % build,
           '   Readable sources: %s (%s). Generated: do not edit, do not commit (it holds' % (
               S['commit'], S['digest'][:16]),
           '   the games\' addresses). Every struct is packed with its padding spelled out, so the',
           '   offsets are clang\'s own (-fdump-record-layouts). */', '', '#pragma pack(push, 1)', '']
    order, seen = [], set()

    def visit(s):
        if s['q'] in seen:
            return
        seen.add(s['q'])
        for o, n, c, sz in s['fields']:
            for q2, s2 in T.structs.items():
                if re.search(r'\b%s\b' % re.escape(s2['c']), c) and '*' not in c.split(s2['c'])[1][:2]:
                    visit(s2)
        order.append(s)
    for s in structs:
        visit(s)
    for s in structs:
        out.append('typedef %s %s %s;' % ('union' if s['kind'] == 'union' else 'struct', s['c'], s['c']))
    out.append('')
    for s in order:
        kw = 'union' if s['kind'] == 'union' else 'struct'
        out.append('/* %s, 0x%X bytes%s */' % (s['q'], s['size'], ', offsets static_asserted' if s['asserted'] else ''))
        out.append('%s %s {' % (kw, s['c']))
        pos = 0
        names = set()
        for o, n, c, sz in s['fields']:
            if kw == 'struct' and o > pos:
                out.append('    unsigned char _pad%X[%d];' % (pos, o - pos))
            nm = n
            while nm in names:
                nm += '_'
            names.add(nm)
            out.append('    %s; /* +0x%X */' % (T.decl(c, nm), o))
            pos = max(pos, o + sz) if kw == 'struct' else pos
        if kw == 'struct' and s['size'] > pos:
            out.append('    unsigned char _pad%X[%d];' % (pos, s['size'] - pos))
        if not s['fields'] and s['size'] and kw == 'union':
            out.append('    unsigned char _raw[%d];' % s['size'])
        out.append('};')
        out.append('')
    out.append('#pragma pack(pop)')
    out.append('')
    out.extend(protos)
    out.append('')
    return '\n'.join(out)


FN_HEAD = re.compile(r'^//// FUNCTION ([0-9a-fA-F]+) (\S+)')


def load_decomp(p):
    """An ExportDecomp file: {address: (name, text)}."""
    out, cur = {}, None
    for ln in open(p, errors='replace'):
        m = FN_HEAD.match(ln)
        if m:
            cur = int(m.group(1), 16)
            out[cur] = [m.group(2), []]
        elif cur is not None:
            out[cur][1].append(ln)
    return {a: (n, ''.join(b)) for a, (n, b) in out.items()}


# ------------------------------------------------------------------ layouts on other builds
#
# Does a readable struct have AI PS2's layout on an Xbox or GameCube build? The evidence: AI PS2's
# readable decompilation spells each field access (`camera->player->speed`), which gives per
# function the struct and the displacement its instruction uses (the access's field offsets
# summed up to the dereference); the counterpart on the other build (a high or medium pair) has
# its own instructions' displacements (the work database's `disps`: memory operands, x86 and
# PowerPC, stack and small-data bases left out on the GameCube). A field at the same offset shows
# the same displacement in the counterpart; a moved one shows another (the Xbox Player's +0x74 at
# +0x84). Only displacements AI PS2's own instructions hold count (the decompiler's field path can
# end inside an array or a sub-object the instruction does not address).

SIG_LINE = re.compile(r'^(?!\s)(?!//)(.*?)\b(\w+)\((.*)\)\s*$', re.M)
LOCAL_PTR = re.compile(r'^\s+(\w+) \*(\w+);', re.M)
CHAIN = re.compile(r'\b([A-Za-z_]\w*)((?:(?:->|\.)[A-Za-z_]\w*)+)')
HOP = re.compile(r'(->|\.)([A-Za-z_]\w*)')
PTR_T = re.compile(r'^(\w+) \*$')
INFORMATIVE = 0x10  # displacements below this are in nearly every function: not evidence


def struct_fields(structs):
    """{C name: {field name as the header spells it: (offset, C type)}} (the header's duplicate
    names get a trailing '_', as c_header writes them)."""
    out = {}
    for s in structs:
        names, m = set(), {}
        for o, n, c, sz in s['fields']:
            nm = n
            while nm in names:
                nm += '_'
            names.add(nm)
            m[nm] = (o, c)
        out[s['n']] = m
    return out


def field_accesses(text, SF):
    """One function of the readable decompilation -> {(struct, displacement)}."""
    m = SIG_LINE.search(text)
    var = {}
    if m:
        for p in m.group(3).split(','):
            pm = re.match(r'^\s*(?:const )?(\w+) \*(\w+)\s*$', p)
            if pm and pm.group(1) in SF:
                var[pm.group(2)] = pm.group(1)
    for t, v in LOCAL_PTR.findall(text):
        if t in SF:
            var[v] = t
    out = set()
    for base, chain in CHAIN.findall(text):
        st = var.get(base)
        if st is None or not chain.startswith('->'):
            continue
        seg, look, disp, ty, started = st, st, 0, None, False
        for op, f in HOP.findall(chain):
            if op == '->' and started:
                out.add((seg, disp))
                pm = PTR_T.match(ty or '')
                if not pm or pm.group(1) not in SF:
                    seg = None
                    break
                seg = look = pm.group(1)
                disp = 0
            elif op == '.':
                if ty not in SF:
                    seg = None
                    break
                look = ty
            started = True
            if f not in SF[look]:
                seg = None
                break
            off, ty = SF[look][f]
            disp += off
        if seg is not None:
            out.add((seg, disp))
    return out


def struct_records(T):
    return [{'n': x['c'], 'size': x['size'], 'fields': [[o, n, T.decl(c, '').strip(), sz] for o, n, c, sz in x['fields']]}
            for x in sorted(T.structs.values(), key=lambda x: x['c'])]


def layout_evidence(build, ai_decomp, structs, con):
    """Per struct: the AI PS2 displacements seen, and per displacement how often the build's
    counterpart holds it exactly, or only shifted (by how much)."""
    SF = struct_fields(structs)
    D = load_decomp(ai_decomp)
    q = lambda sql, *a: con.execute(sql, list(a)).fetchall()
    ps2 = {a: set(d or ()) for a, d in q("SELECT addr, disps FROM functions WHERE build = 'ai_ps2'")}
    other = {a: set(d or ()) for a, d in q("SELECT addr, disps FROM functions WHERE build = ?", build)}
    pairs = q("""SELECT ai_addr, addr, confidence FROM counterparts WHERE build = ? AND NOT inlined
                 AND confidence IN ('high', 'medium')""", build)
    ev = {}  # struct -> disp -> {'obs', 'hit', 'shift': {delta: n}, 'fns': set}
    for ai_addr, addr, conf in pairs:
        if ai_addr not in D or addr not in other:
            continue
        dx = other[addr]
        for st, d in field_accesses(D[ai_addr][1], SF):
            if d not in ps2.get(ai_addr, ()):
                continue
            e = ev.setdefault(st, {}).setdefault(d, {'obs': 0, 'hit': 0, 'shift': {}, 'fns': []})
            e['obs'] += 1
            e['fns'].append(ai_addr)
            if d in dx:
                e['hit'] += 1
            else:
                for delta in range(-0x100, 0x101, 4):
                    if delta and d + delta in dx:
                        e['shift'][delta] = e['shift'].get(delta, 0) + 1
    return ev


def layout_verdicts(build, ai_decomp, structs, con):
    """Per struct, from layout_evidence:
    - differs: an informative displacement (0x10 and up) that fails in most of two or more
      counterparts using it, or one the counterparts show consistently (twice or more) at another
      informative offset
    - same: none of those, at least 80 % of the informative observations hold, and at least two
      displacements or two observations (a single miss is codegen as often as layout: the
      counterpart computes the field's address first and reads at 0)
    - small: a struct under 0x10 bytes (no informative displacement) whose displacements all hold
      in two or more observations
    - unknown: the rest (too little evidence); the layout is not applied"""
    ev = layout_evidence(build, ai_decomp, structs, con)
    size = {s['n']: s['size'] for s in structs}
    out = {}
    for s in structs:
        st = s['n']
        e = ev.get(st, {})
        inf = {d: x for d, x in e.items() if d >= INFORMATIVE}
        obs = sum(x['obs'] for x in inf.values())
        hits = sum(x['hit'] for x in inf.values())
        bad, moved = {}, {}
        for d, x in sorted(inf.items()):
            if x['hit'] * 2 >= x['obs']:
                continue
            sh = [(k, n) for k, n in x['shift'].items() if d + k >= INFORMATIVE and n >= 2]
            if sh:
                moved[d] = max(sh, key=lambda kv: kv[1])
            if x['obs'] >= 2 or sh:
                bad[d] = (x['hit'], x['obs'], moved.get(d))
        # displacement 0 is no displacement on x86 (a plain [reg] operand): not counted there
        e0 = {d: x for d, x in e.items() if d or PLATFORM.get(build) != 'xbox'}
        allsmall = e0 and all(x['hit'] * 2 >= x['obs'] for x in e0.values())
        if bad:
            v = 'differs'
        elif obs and hits >= 0.8 * obs and (len(inf) >= 2 or obs >= 2):
            v = 'same'
        elif not inf and size[st] <= INFORMATIVE and allsmall and sum(x['obs'] for x in e0.values()) >= 2:
            v = 'small'
        else:
            v = 'unknown'
        out[st] = {'verdict': v, 'disps': len(e), 'informative': len(inf), 'obs': obs, 'hits': hits,
                   'functions': len({f for x in e.values() for f in x['fns']}),
                   'bad': {'0x%X' % d: {'hit': h, 'obs': o, 'best_shift': b and ('%+#x' % b[0], b[1])}
                           for d, (h, o, b) in bad.items()},
                   'moved': {'0x%X' % d: '0x%X' % (d + k) for d, (k, n) in moved.items()}}
    return out


def decomp_stats(before, after):
    """Before/after counts over two ExportDecomp files of one program."""
    B, A = load_decomp(before), load_decomp(after)
    pats = {
        'FUN_ names (calls and refs)': r'\bFUN_[0-9a-fA-F]{8}\b',
        'raw offset derefs *(T *)(x + 0xN)': r'\*\([\w ]+\*\)\([\w.>-]+ \+ (?:0x[0-9a-fA-F]+|\d+)\)',
        'field accesses ->f': r'->[A-Za-z_]\w*',
        'param_N': r'\bparam_\d+\b',
        'DAT_ globals': r'\b_?DAT_[0-9a-fA-F]{8}\b',
    }
    common = sorted(set(A) & set(B))
    print('functions: before %d, after %d, common %d, renamed %d' % (
        len(B), len(A), len(common), sum(1 for a in common if A[a][0] != B[a][0])))
    for k, p in pats.items():
        rb = sum(len(re.findall(p, B[a][1])) for a in common)
        ra = sum(len(re.findall(p, A[a][1])) for a in common)
        print('%-40s before %7d  after %7d  (%+d)' % (k, rb, ra, ra - rb))


def main(argv):
    src, cache = SRC, CACHE
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == '--src':
            src = os.path.abspath(argv[i + 1])
            i += 2
        elif argv[i] == '--cache':
            cache = os.path.abspath(argv[i + 1])
            i += 2
        else:
            args.append(argv[i])
            i += 1
    SRC_ROOT[0] = src
    if not args:
        sys.exit(__doc__)
    if args[0] == 'scan':
        S = scan(src, cache)
        T = Types(S)
        print('records with layouts %d, guest structs %d (%d asserted), rejected %d, registrations %d, '
              'globals %d, clang errors %d' % (len(S['layouts']), len(T.structs),
                                               sum(s['asserted'] for s in T.structs.values()), len(T.reject),
                                               len(S['regs']), len(S['globals']), len(S['errors'])))
        if '-v' in args:
            for q, why in sorted(T.reject.items()):
                if q.split('::')[0] in NAMESPACES:
                    print('  rejected %s: %s' % (q, why))
    elif args[0] == 'gen' and len(args) == 3:
        gen(args[1], args[2], src, cache)
    elif args[0] == 'layouts' and len(args) >= 2:
        S = scan(src, cache)
        T = Types(S)
        con = duckdb.connect(WORKDB, read_only=True)
        dec = args[2] if len(args) > 2 else AI_READABLE_DECOMP
        V = layout_verdicts(args[1], dec, struct_records(T), con)
        if '--json' in args:
            print(json.dumps(V, indent=1, sort_keys=True))
        else:
            n = {}
            for st, v in sorted(V.items(), key=lambda kv: (kv[1]['verdict'], -kv[1]['obs'])):
                n[v['verdict']] = n.get(v['verdict'], 0) + 1
                if v['verdict'] != 'unknown' or v['disps']:
                    print('%-8s %-28s functions %3d  displacements %3d (informative %3d)  hits %4d of %4d  %s' % (
                        v['verdict'], st, v['functions'], v['disps'], v['informative'], v['hits'], v['obs'],
                        ' '.join('%s:%d/%d%s' % (d, b['hit'], b['obs'], ('->' + b['best_shift'][0] + 'x%d' % b['best_shift'][1])
                                                   if b['best_shift'] else '') for d, b in v['bad'].items())))
            print(args[1], n)
    elif args[0] == 'stats' and len(args) == 3:
        decomp_stats(args[1], args[2])
    elif args[0] == 'all' and len(args) == 2:
        for b in BUILDS:
            gen(b, args[1], src, cache)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
