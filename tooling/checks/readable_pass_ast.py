#!/usr/bin/env python3
"""The source tree as readable_pass.py sees it: clang's AST of every file of game/zge, game/aggressive
and game/sce, per function, with exact source ranges, and a list of text edits.

Why the compiler's AST and not a parser of our own: a pass may only rewrite what it can prove, and most
of what it has to prove is what the compiler already knows (what type an argument has, what a name
refers to, whether a statement is the only body of an `if`).

The dump: `clang++ <the file's flags from compile_commands.json> -fsyntax-only -Xclang -ast-dump=json
-Xclang -ast-dump-filter=<namespace>::`, one run per top-level namespace the file opens: the filter
keeps the dump to the game's own declarations (2 MB where a whole TU's is 1.4 GB).
"""
import concurrent.futures as futures
import json
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zlayout  # noqa: E402  (a tree's folders, old or new)

HERE = os.path.dirname(os.path.abspath(__file__))
LAYERS = ('zge', 'aggressive', 'sce')
FUNCTIONS = ('FunctionDecl', 'CXXMethodDecl', 'CXXConstructorDecl', 'CXXDestructorDecl')
NAMESPACE = re.compile(r'^\s*(?:inline\s+)?namespace\s+(\w+)', re.M)


class Text:
    """A file's bytes: offsets from clang are byte offsets, so everything here works on bytes
    decoded as latin-1 (one character a byte) and written back the same way."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.raw = f.read().decode('latin-1')
        self.starts = None

    def line_of(self, offset):
        return self.raw.count('\n', 0, offset) + 1

    def line_start(self, offset):
        return self.raw.rfind('\n', 0, offset) + 1

    def line_end(self, offset):
        """The offset of the newline ending the line, or the end of the file."""
        i = self.raw.find('\n', offset)
        return len(self.raw) if i < 0 else i

    def write(self, text):
        with open(self.path, 'wb') as f:
            f.write(text.encode('latin-1'))
        self.raw = text


class Edits:
    """Replacements per file, in transactions: the edits of one change go in together or not at all, so a
    change that collides with another this round is simply seen again next round, whole."""

    def __init__(self):
        self.by_file = {}
        self.pending = []

    def add(self, path, start, end, text=''):
        self.pending.append((path, start, end, text))

    def commit(self):
        """True if the pending edits went in; False if one overlaps what is in already (all dropped).
        Two deletions may overlap: they come down to their union. Anything else that overlaps is a conflict,
        an insertion inside a deleted range too."""
        pending, self.pending = self.pending, []
        mine = {}
        for path, start, end, text in pending:
            if start is None or end is None:  # (a place inside a macro: no offset)
                return False
            mine.setdefault(path, set()).add((start, end, text))
        for path, edits in mine.items():
            together = sorted(edits | self.by_file.get(path, set()))
            reach, reach_text = -1, False   # how far the edits so far go
            for start, end, text in together:
                if start < reach and (text or reach_text):
                    return False
                if start == reach and start == end and reach_text:
                    return False
                if end > reach:
                    reach, reach_text = end, bool(text)
                elif end == reach and text:
                    reach_text = True
        for path, edits in mine.items():
            self.by_file.setdefault(path, set()).update(edits)
        return bool(pending)

    def rollback(self):
        self.pending = []

    def __len__(self):
        return sum(len(v) for v in self.by_file.values())

    def apply(self, texts):
        """Writes the files."""
        for path, edits in self.by_file.items():
            text = texts[path]
            raw = text.raw
            out, last = [], 0
            for start, end, new in sorted(edits):
                if start < last:
                    if new:
                        raise RuntimeError('%s: overlapping edits at %d' % (path, start))
                    last = max(last, end)  # (a deletion inside a deletion)
                    continue
                out.append(raw[last:start])
                out.append(new)
                last = end
            out.append(raw[last:])
            text.write(''.join(out))


def begin(node):
    return node['range']['begin'].get('offset')


def end(node):
    """The offset just past the node's last token."""
    e = node['range']['end']
    if 'offset' not in e:
        return None
    return e['offset'] + e.get('tokLen', 0)


def source_begin(node):
    """Where a node's text starts; in a macro, where the macro is invoked."""
    b = node['range']['begin']
    return b.get('offset', b.get('expansionLoc', {}).get('offset'))


def source_end(node, raw):
    """Past a node's text; in a macro (offsetof), past the invocation with its arguments."""
    e = node['range']['end']
    if 'offset' in e:
        return e['offset'] + e.get('tokLen', 0)
    x = e.get('expansionLoc', {})
    if 'offset' not in x:
        return None
    i = x['offset'] + x.get('tokLen', 0)
    j = i
    while raw[j:j + 1] in (' ', '\t'):
        j += 1
    if raw[j:j + 1] != '(':
        return i
    depth = 0
    while j < len(raw):
        depth += raw[j] == '('
        depth -= raw[j] == ')'
        j += 1
        if depth == 0:
            break
    return j


def in_macro(node):
    r = node.get('range', {})
    return any('spellingLoc' in r.get(k, {}) or 'expansionLoc' in r.get(k, {}) for k in ('begin', 'end'))


def walk(node, parent=None):
    """Every (node, parent) under `node`, itself first, in source order."""
    stack = [(node, parent)]
    while stack:
        n, p = stack.pop()
        yield n, p
        inner = n.get('inner')
        if inner:
            for child in reversed(inner):
                if child:
                    stack.append((child, n))


def walk_code(node, parent=None):
    """walk(), but a lambda counts once and only for its body: the AST repeats the body in the closure
    class, and captures show as references at the place of the first use."""
    stack = [(node, parent)]
    while stack:
        n, p = stack.pop()
        yield n, p
        inner = n.get('inner')
        if inner:
            if n.get('kind') == 'LambdaExpr':
                inner = [c for c in inner if c and c.get('kind') == 'CompoundStmt'][-1:]
            for child in reversed(inner):
                if child:
                    stack.append((child, n))


def children(node):
    return [c for c in node.get('inner', []) if c]


def desugar(type_node):
    return type_node.get('desugaredQualType', type_node.get('qualType', ''))


def strip_implicit(node):
    """Through ImplicitCastExpr, ExprWithCleanups, MaterializeTemporaryExpr and the like."""
    while node.get('kind') in ('ImplicitCastExpr', 'ExprWithCleanups', 'MaterializeTemporaryExpr', 'CXXBindTemporaryExpr',
                               'ParenExpr', 'ConstantExpr', 'SubstNonTypeTemplateParmExpr') and children(node):
        node = children(node)[-1] if node.get('kind') == 'SubstNonTypeTemplateParmExpr' else children(node)[0]
    return node


class Function:
    """A function with a body in the tree."""

    def __init__(self, path, decl, enclosing):
        self.path, self.decl, self.enclosing = path, decl, enclosing
        self.name = decl.get('name', '')
        self.body = next((c for c in children(decl) if c.get('kind') == 'CompoundStmt'), None)

    @property
    def key(self):
        return (self.path, begin(self.decl))


class CompileError(Exception):
    def __init__(self, path, errors):
        Exception.__init__(self, path)
        self.path, self.errors = path, errors


class Tree:
    def __init__(self, tree, build, cache=None, jobs=4):
        self.tree = os.path.abspath(tree)
        self.rt = zlayout.folder(self.tree, 'runtime')
        self.game = os.path.join(self.tree, self.rt, 'src', 'game')
        self.build = os.path.abspath(build)
        self.cache = cache
        self.jobs = jobs
        with open(os.path.join(self.build, 'compile_commands.json')) as f:
            commands = json.load(f)
        self.commands = {}
        self.build_tree = None
        for c in commands:
            if '/src/game/' in c['file'] and c['file'].endswith('.cpp'):
                rel = c['file'].split('/src/game/', 1)[1]
                # (the readable library's own compile, not a test's or a tool's)
                if rel not in self.commands or 'ps2_game_readable' in c['command']:
                    self.commands[rel] = c
                    self.build_tree = self.build_tree or re.split(r'/(?:%s)/src/game/' % '|'.join(zlayout.FOLDERS['runtime']), c['file'])[0]
        self.files = []
        for layer in LAYERS:
            for root, dirs, names in os.walk(os.path.join(self.game, layer)):
                for name in sorted(names):
                    if name.endswith(('.cpp', '.h')):
                        self.files.append(os.path.join(root, name))
        self.texts = {p: Text(p) for p in self.files}
        self.only_units = None      # readable_pass.py --units: read only these units' sources

    def rel(self, path):
        return os.path.relpath(path, self.game)

    def units(self):
        """The .cpp files of the three layers that the build compiles."""
        return [p for p in self.files if p.endswith('.cpp') and self.rel(p) in self.commands and (not self.only_units or any(u in p for u in self.only_units))]

    def dump_command(self, path):
        c = self.commands[self.rel(path)]
        args = shlex.split(c['command'])
        i = args.index('-o')
        del args[i:i + 2]
        # A compile_commands.json from another worktree's build still names that worktree.
        # (either tree's runtime folder, old or new: tools17's renames)
        here = re.compile(re.escape(self.build_tree) + r'/(?:%s)/' % '|'.join(zlayout.FOLDERS['runtime']))
        args = [here.sub(lambda _: self.tree + '/' + self.rt + '/', a) if a.startswith(('-I', '/')) else a
                for a in args if a != '-c' and a != c['file']]
        return args + ['-fsyntax-only', '-Wno-everything', '-ferror-limit=0', '-Xclang', '-ast-dump=json']

    def dump(self, path):
        """The top-level declarations of a unit, from every namespace it opens, each once."""
        docs = []
        namespaces = top_namespaces(self.texts[path].raw)
        base = self.dump_command(path)
        for ns in namespaces:
            run = subprocess.run(base + ['-Xclang', '-ast-dump-filter=%s::' % ns, path], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, cwd=self.build, encoding='latin-1')
            if run.returncode != 0:
                raise CompileError(path, run.stderr)
            docs += parse_stream(run.stdout)
        return docs

    def declared_elsewhere(self, paths):
        """{unit: [FunctionDecl without its body, with its file in `path_of`]}: what a unit's headers declare in the layers'
        namespaces it does not open itself (a zge unit sees sce's functions), for what can be called by name there."""
        def one(path):
            out = []
            opened = set(top_namespaces(self.texts[path].raw))
            base = self.dump_command(path)
            for ns in LAYERS:
                if ns in opened:
                    continue
                run = subprocess.run(base + ['-Xclang', '-ast-dump-filter=%s::' % ns, path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     cwd=self.build, encoding='latin-1')
                if run.returncode != 0:
                    continue
                current = None
                for doc in parse_stream(run.stdout):
                    current = decl_file(doc) or current
                    if doc.get('kind') == 'FunctionDecl' and doc.get('mangledName') and current in self.texts:
                        body = [c for c in children(doc) if c.get('kind') == 'CompoundStmt'][:1]
                        doc['inner'] = [{'kind': 'CompoundStmt', 'range': body[0].get('range', {})}] if body else []
                        doc['path_of'] = current
                        out.append(doc)
            return path, out

        with futures.ThreadPoolExecutor(self.jobs) as pool:
            return dict(pool.map(one, paths))

    def layouts(self):
        """Every record of the game the units see: parse_layouts' table, the unit dumps merged."""
        out = {}

        def one(path):
            args = [a for a in self.dump_command(path) if a != '-Xclang' and a != '-ast-dump=json']
            run = subprocess.run(args + ['-Xclang', '-fdump-record-layouts-complete', path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 cwd=self.build, encoding='latin-1')
            return run.stdout

        with futures.ThreadPoolExecutor(self.jobs) as pool:
            for text in pool.map(one, self.units()):
                parse_layouts(text, out)
        return out

    def asts(self, paths=None):
        """{unit: top-level declarations} over `paths` (all units), jobs at a time, and {unit: clang's errors}
        of the ones that do not compile."""
        paths = paths or self.units()
        out, failed = {}, {}

        def one(path):
            try:
                return path, self.dump(path), None
            except CompileError as e:
                return path, None, e.errors

        with futures.ThreadPoolExecutor(self.jobs) as pool:
            for path, docs, errors in pool.map(one, paths):
                if errors is None:
                    out[path] = docs
                else:
                    failed[path] = errors
        return out, failed


LAYOUT_LINE = re.compile(r'^\s*(\d+) \|( +)(.*)$')
LAYOUT_SIZE = re.compile(r'\[sizeof=(\d+), .*align=(\d+)')


def parse_layouts(text, out):
    """clang's -fdump-record-layouts-complete, the game's records only: {type: (size, align, [(offset, path, field type)])}."""
    for chunk in text.split('*** Dumping AST Record Layout\n')[1:]:
        lines = chunk.split('\n')
        m = LAYOUT_LINE.match(lines[0])
        if not m:
            continue
        name = re.sub(r'^(struct|class|union) ', '', m.group(3)).strip()
        if not name.startswith(('zge::', 'aggressive::', 'sce::')) or '(anonymous' in name and False:
            continue
        name = name.replace(' (empty)', '').replace('(anonymous namespace)::', '')
        fields, stack = [], []
        size = align = 0
        for line in lines[1:]:
            k = LAYOUT_SIZE.search(line)
            if k:
                size, align = int(k.group(1)), int(k.group(2))
                break
            f = LAYOUT_LINE.match(line)
            if not f:
                continue
            depth = (len(f.group(2)) - 1) // 2
            body = f.group(3).strip()
            if ' ' not in body:
                continue
            kind, _, field = body.rpartition(' ')
            kind = re.sub(r'^(struct|class|union) ', '', kind).strip()
            del stack[depth - 1:]
            stack.append(field)
            if 'ee::Float' in stack[:-1] or any('m_v' == x for x in stack[:-1]):
                continue
            path = '.'.join(stack)
            if field in ('m_v', 'address') and depth > 1:
                continue
            fields.append((int(f.group(1)), path, kind))
        if size:
            out.setdefault(name, (size, align, fields))


def top_namespaces(raw):
    """The namespaces a file opens at column 0 (nested ones come with them)."""
    return sorted(set(re.findall(r'^(?:inline\s+)?namespace\s+(\w+)', raw, re.M)))


def parse_stream(text):
    decoder = json.JSONDecoder()
    i, n, out = 0, len(text), []
    while i < n:
        while i < n and text[i] in ' \t\r\n':
            i += 1
        if i >= n:
            break
        doc, i = decoder.raw_decode(text, i)
        out.append(doc)
    return out


def decl_file(decl):
    """The file a top-level declaration is in (the dumper names it once a declaration)."""
    loc = decl.get('loc', {})
    for where in (loc, loc.get('spellingLoc', {}), loc.get('expansionLoc', {}), decl.get('range', {}).get('begin', {})):
        if 'file' in where:
            return where['file']
    return None


def functions_of(docs, game_root):
    """Every function definition with a body in game/{zge,aggressive,sce}, lambdas' enclosing ones
    included, top-level or in a class: (path, FunctionDecl). Deduplicated by the caller."""
    out = []
    current = None

    def visit(decl, path, enclosing):
        kind = decl.get('kind')
        if kind in FUNCTIONS:
            if any(c.get('kind') == 'CompoundStmt' for c in children(decl)):
                out.append((path, decl, enclosing))
        elif kind in ('NamespaceDecl', 'CXXRecordDecl', 'ClassTemplateDecl', 'FunctionTemplateDecl', 'LinkageSpecDecl'):
            for c in children(decl):
                visit(c, path, enclosing + [decl])

    for decl in docs:
        path = decl_file(decl) or current
        current = path
        if path and path.startswith(game_root):
            visit(decl, path, [])
    return out
