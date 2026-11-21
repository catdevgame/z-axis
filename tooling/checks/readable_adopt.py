#!/usr/bin/env python3
"""Gives a swept draft committed source: its text goes from the build's generated batch file to the end of its
named file's namespace (readable_place.py's plan), its manifest row goes, and from then on it is edited by hand.

  readable_adopt.py move PLAN.tsv --build DIR [--only @FILE|NAME,...] [--layers zge,aggressive]
  readable_adopt.py bar [--names FILE] [--json FILE]     what still fails the lint, per moved function
  readable_adopt.py back @FILE [--drafts FILE]           takes functions out of their files: the rows go back in the manifest

`move` carries a draft with what it needs: the constants and globals of its batch's anonymous namespace, its batch's
includes, the declarations of readable functions its batch's header holds. It writes nothing else: readable_pass.py
then takes it through the same treatment as the hand-written code (marks, saves, frames, calls, fields, vectors), and
a function that does not reach the bar goes `back`, so nothing below the bar stays in source.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
GAME = os.environ.get('ADOPT_GAME') or ztree.GAME
LAYERS = ('zge', 'aggressive')
REG = re.compile(r'^\s*READABLE_(?:FUNCTION(?:_WAITS|_VU0)?|PLANNED)\(')
IDENT = re.compile(r'[A-Za-z_]\w*')
NAMESPACE_OF = {'zge': 'zge', 'aggressive': 'aggressive', 'aggressive/skater': 'aggressive::skater', 'aggressive/level': 'aggressive::level',
                'aggressive/fx': 'aggressive::fx', 'zge/zgui': 'zge::zgui', 'aggressive/hud': 'aggressive::hud',
                'aggressive/sound': 'aggressive::sound', 'aggressive/script': 'aggressive::script'}


def manifest_path(layer):
    return os.path.join(GAME, layer, 'sweep', 'manifest.txt')


def read_manifest(layer):
    rows = {}
    for line in open(manifest_path(layer), errors='replace'):
        if line.startswith('#') or not line.strip():
            continue
        p = line.rstrip('\n').split('\t')
        rows[int(p[0], 16)] = p
    return rows


def statement_end(lines, i, indent):
    """The last line of the statement starting at lines[i], at this indent."""
    start = lines[i].split(' // ')[0].rstrip()
    if start.count('{') == start.count('}') and start.endswith((';', '}')):
        return i
    j = i
    while j + 1 < len(lines):
        j += 1
        text = lines[j]
        if text.startswith(' ' * indent + '}') and text.rstrip().endswith((';', '}')):
            return j
        if text.startswith(' ' * indent) and not text.startswith(' ' * (indent + 1)) and text.rstrip().endswith(';') and '{' not in text:
            return j
    return j


class Batch:
    """A generated batch file: its includes, the declarations above its namespace, its anonymous namespace's statements
    by the name they declare, and each function's text."""

    def __init__(self, path):
        self.path = path
        lines = open(path, errors='replace').read().split('\n')
        self.includes = [l for l in lines if l.startswith('#include') and '"sym/' not in l]
        self.headers = [l for l in lines if re.match(r'namespace [\w:]+ \{ .*\}$', l)]
        self.statements = []    # (names it declares, text, names it uses)
        self.functions = {}     # name -> text
        self.order = []
        i = next(n for n, l in enumerate(lines) if l.startswith('namespace sweep'))
        i += 2
        pending = []
        while i < len(lines) and lines[i] != '}':
            line = lines[i]
            if line.strip() == '':
                pending = []
                i += 1
                continue
            if line == '    namespace':
                i += 2
                while lines[i] != '    }':
                    if lines[i].strip() == '':
                        i += 1
                        continue
                    j = statement_end(lines, i, 8)
                    text = lines[i:j + 1]
                    self.statements.append((self.declared(text), text))
                    i = j + 1
                i += 1
                continue
            if line.startswith('    //'):
                pending.append(line)
                i += 1
                continue
            j = i
            while not REG.match(lines[j]):
                j += 1
                if j >= len(lines) or lines[j] == '}':
                    raise SystemExit('%s:%d: no registration after this' % (path, i + 1))
            name = re.match(r'\s*READABLE_\w+\(([^)]*)\)', lines[j]).group(1).split(',')
            name = name[-2 if len(name) >= 2 else 0].strip().split('::')[-1]
            self.functions[name] = pending + lines[i:j + 1]
            self.order.append(name)
            pending = []
            i = j + 1

    @staticmethod
    def declared(text):
        first = text[0].strip()
        if first.startswith('static_assert'):
            return []
        m = re.search(r'\b(k\w+|g_\w+)\s*(?:=|\{|;)', first) or re.search(r'(\w+)\s*\((?!.*template)', first.split('>', 1)[-1] if first.startswith('template') else first)
        return [m.group(1)] if m else []

    def needs(self, text):
        """The statements a function's text uses, with what those use."""
        wanted = set(IDENT.findall('\n'.join(text)))
        out = []
        seen = set()
        changed = True
        while changed:
            changed = False
            for k, (names, body) in enumerate(self.statements):
                if k not in seen and any(n in wanted for n in names):
                    seen.add(k)
                    wanted |= set(IDENT.findall('\n'.join(body)))
                    changed = True
        for k, (names, body) in enumerate(self.statements):
            if k in seen:
                out.append((names, body))
        return out


def code_only(raw):
    sys.path.insert(0, HERE)
    import readable_lint
    return readable_lint.strip_comments(raw)


def insertion_point(raw):
    """(offset of the line closing the file's last namespace, its qualified name). Nested namespaces: the innermost
    that is last in its parent. A namespace closed later replaces an earlier one unless that one is inside it: the
    one-line declarations above a file's main namespace (`namespace aggressive::level { void f(); }`) have its
    name, and the drafts go into the main one."""
    code = code_only(raw)
    stack = []
    last = None
    for m in re.finditer(r'\bnamespace\s*([\w:]*)\s*(?=\{)|[{}]', code):
        t = m.group(0)
        if t.startswith('namespace'):
            stack.append(('ns', m.group(1), m.end()))
            continue
        if t == '{':
            if stack and stack[-1][0] == 'ns' and stack[-1][2] == m.start():
                stack[-1] = ('open', stack[-1][1], m.start())
            else:
                stack.append(('brace', '', m.start()))
            continue
        if not stack:
            continue
        kind, name, opened = stack.pop()
        if kind == 'open' and name and all(s[0] == 'open' and s[1] for s in stack):
            chain = [s[1] for s in stack] + [name]
            if last is None or last[0] < opened:
                last = (m.start(), chain)
    if last is None:
        return None, None
    return raw.rfind('\n', 0, last[0]) + 1, '::'.join(last[1])


def unqualify(text, chain):
    """Inside aggressive::skater, `aggressive::skater::x` and `aggressive::x` lose what is implied."""
    parts = chain.split('::')
    for n in range(len(parts), 0, -1):
        q = '::'.join(parts[:n])
        # (a function's address keeps it: a draft's local often has the name of its callee's body)
        text = re.sub(r'(?<![\w:&])' + re.escape(q) + r'::', '', text)
    return text


def namespace_scope(code):
    """A list of bools by offset: is this character outside every function and struct?"""
    flags = bytearray(len(code))
    stack = []
    pending = False
    i = 0
    for m in re.finditer(r'\bnamespace\b|[{};]', code):
        t = m.group(0)
        if t == 'namespace':
            pending = True
        elif t == ';':
            pending = False
        elif t == '{':
            stack.append(pending)
            pending = False
        elif stack:
            stack.pop()
        if all(stack):
            flags[m.end():] = b'\1' * (len(code) - m.end())
        else:
            flags[m.end():] = b'\0' * (len(code) - m.end())
    return flags


def declared_in(raw):
    """(constants and globals by name, every other name a declaration of the file takes at namespace scope)"""
    names = {}
    code = code_only(raw)
    scope = namespace_scope(code)
    for m in re.finditer(r'^\s*(?:inline\s+|static\s+)*(?:constexpr\s+[\w:]+|const\s+[\w:]+|ee::Global<[^;\n]*>)\s+(k\w+|g_\w+)\s*(?:=|\{|;)[^\n]*', code, re.M):
        if scope[m.start(1)]:
            names[m.group(1)] = m.group(0).strip()
    functions = set(m.group(1) for m in re.finditer(r'\b([a-zA-Z_]\w*)\s*\(', code) if scope[m.start()])
    return names, functions


def same_declaration(a, b):
    def shape(text):
        text = text.split(' // ')[0]
        return re.sub(r'\s+|\binline\b|\bstatic\b|(?:\bzge|\baggressive(?:::\w+)?)::', '', text).rstrip(';')
    return shape(a) == shape(b)


HEADER_WORDS = {}


def header_words(include):
    """The identifiers an include's own text declares, nothing it only mentions in a comment."""
    if include not in HEADER_WORDS:
        m = re.search(r'"([^"]+)"', include)
        path = os.path.join(GAME, m.group(1)) if m else ''
        HEADER_WORDS[include] = words = set()
        if os.path.exists(path):
            raw = open(path, errors='replace').read()
            words |= set(IDENT.findall(code_only(raw)))
            for inner in re.findall(r'^#include "[^"]+"', raw, re.M):
                local = '#include "%s/%s"' % (os.path.dirname(m.group(1)), inner.split('"')[1])
                if os.path.exists(os.path.join(GAME, local.split('"')[1])):
                    words |= header_words(local)
                else:
                    words |= header_words(inner)
    return HEADER_WORDS[include]


def game_calls(include):
    """The GAME_CALL lines an include brings, by name: a second one of a name does not compile."""
    m = re.search(r'"([^"]+)"', include)
    path = os.path.join(GAME, m.group(1)) if m else ''
    out = set()
    if os.path.exists(path):
        raw = open(path, errors='replace').read()
        out |= set(re.findall(r'GAME_CALL\w*\((\w+)\)', raw))
        for inner in re.findall(r'^#include "[^"]+"', raw, re.M):
            local = '#include "%s/%s"' % (os.path.dirname(m.group(1)), inner.split('"')[1])
            out |= game_calls(local if os.path.exists(os.path.join(GAME, local.split('"')[1])) else inner)
    return out


def needed_includes(rows, block):
    """A batch's includes serve all its functions: what this block names, and always the framework's."""
    used = set(IDENT.findall(block))
    common = set()
    for h in ('#include "ee/abi.h"', '#include "ee/readable.h"', '#include "ee/vu0.h"', '#include "ee/plain.h"'):
        common |= header_words(h)
    out = set()
    for addr, name, text, batch in rows:
        for i in batch.includes:
            if '<' in i or ' "ee/' in i or (header_words(i) - common) & used:
                out.add(i)
    return out


HEADER_GLOBALS = {}


def header_globals():
    """{name: its declaration}: the globals the game's headers declare, which a file including them sees."""
    if not HEADER_GLOBALS:
        for layer in LAYERS:
            for root, _, files in os.walk(os.path.join(GAME, layer)):
                for f in files:
                    if f.endswith('.h'):
                        text = open(os.path.join(root, f), errors='replace').read()
                        for m in re.finditer(r'^\s*(?:inline ee::Global<[^;\n]*>|(?:inline\s+)?constexpr\s+[\w:]+)\s+(k\w+|g_\w+)\b[^\n]*', text, re.M):
                            HEADER_GLOBALS[m.group(1)] = m.group(0).strip()
    return HEADER_GLOBALS


def new_file_text(rel, chain, includes):
    return ('// %s\n\n' % rel + '\n'.join(includes) + '\n\n#include <cstddef>\n#include <cstdint>\n#include "sym/%s.h"\n\nnamespace %s\n{\n}\n' % (rel, chain))


def move(args):
    plan = {}
    for line in open(args.plan):
        p = line.rstrip('\n').split('\t')
        if p[0].startswith('0x'):
            plan[int(p[0], 16)] = {'name': p[1], 'new': p[5]}
    only = None
    if args.only:
        only = set(l.split('\t')[0].strip() for l in open(args.only[1:])) if args.only.startswith('@') else set(args.only.split(','))
    sweep = os.path.join(args.build, 'runtime', 'readable_sweep')
    batches = {}
    per_file = collections.defaultdict(list)
    removed = collections.defaultdict(set)
    skipped = collections.Counter()
    for layer in args.layers.split(','):
        for addr, p in sorted(read_manifest(layer).items()):
            name = p[6]
            if only is not None and name not in only and ('0x%08X' % addr) not in only:
                continue
            if p[1] == 'Planned':
                skipped['planned'] += 1
                continue
            batch = batches.get(p[2])
            if batch is None:
                batch = batches[p[2]] = Batch(os.path.join(sweep, p[2]))
            text = batch.functions.get(name)
            if text is None:
                skipped['no text'] += 1
                print('no text: %s in %s' % (name, p[2]))
                continue
            target = plan[addr]['new'] if addr in plan else None
            if not target:
                skipped['no plan'] += 1
                continue
            per_file[target].append((addr, name, text, batch))
            removed[layer].add(addr)
    created = []
    moved = 0
    for rel, rows in sorted(per_file.items()):
        path = os.path.join(GAME, rel)
        if not os.path.exists(path):
            folder = os.path.dirname(rel)
            chain = NAMESPACE_OF.get(folder) or rel.split('/')[0]
            raw = new_file_text(rel, chain, [])
            created.append(rel)
        else:
            raw = open(path, errors='replace').read()
        at, chain = insertion_point(raw)
        if at is None:
            raise SystemExit('%s: no namespace to add to' % rel)
        present, functions = declared_in(raw)
        statements, seen = [], set()
        bodies = []
        renamed = {}
        for addr, name, text, batch in sorted(rows):
            for names, body in batch.needs(text):
                key = tuple(names) or tuple(body)
                if key in seen:
                    continue
                seen.add(key)
                known = present.get(names[0]) if names else None
                known = known or (header_globals().get(names[0]) if names else None)
                if known:
                    # The file has this name: the same thing, or another's, and the draft's takes a name of its own.
                    if same_declaration(known, body[0]):
                        continue
                    renamed[names[0]] = names[0] + 'Word'
                statements += body
            body = '\n'.join(text)
            if name in functions and not name.endswith('_'):
                # The file calls it by a typed line of that name: the body takes the marker (READABLE.md, routes).
                body = re.sub(r'\b%s(\s*\()' % name, name + r'_\1', body, count=1)
                body = re.sub(r'(READABLE_\w+\()%s,' % name, r'\1sym::%s, %s_,' % (name, name), body)
            bodies.append(body)
        block = ''
        if statements:
            block += '\n    namespace\n    {\n' + '\n'.join(statements) + '\n    }\n'
        for b in bodies:
            block += '\n' + b + '\n'
        block = unqualify(block, chain)
        for old, new in renamed.items():
            block = re.sub(r'(?<!sym::)\b%s\b' % old, new, block)
        raw = raw[:at] + block + raw[at:]
        # What the batch included and the file does not; the declarations its header held.
        have = set(l.strip() for l in raw.split('\n') if l.startswith('#include'))
        own = set(re.findall(r'GAME_CALL\w*\((\w+)\)', raw))
        add = sorted(i for i in needed_includes(rows, block) if i not in have and not (own & game_calls(i)))
        extra_decls = []
        tables = {}
        for addr, name, text, batch in rows:
            used = set(IDENT.findall('\n'.join(text)))
            for line in batch.headers:
                inner = re.match(r'namespace [\w:]+ \{ (.*)\}$', line).group(1)
                for decl in re.findall(r'[^;]+\([^;]*\);', inner):
                    m = re.search(r'^\s*(.*?)\b(\w+)\(', decl)
                    if not m or m.group(2) == 'GAME_CALL':
                        continue
                    plain = m.group(2).rstrip('_')
                    if not {plain, m.group(2)} & used or re.search(r'\b%s\s*\(' % m.group(2), raw):
                        continue
                    space = re.match(r'namespace ([\w:]+)', line).group(1)
                    if re.search(r'\b%s\s*\(' % plain, raw) and (chain + '::').startswith(space + '::'):
                        # The file's own line for it takes the plain name: the call goes through the table.
                        tables[m.group(2)] = (m.group(1).strip(), plain)
                    elif line not in extra_decls:
                        extra_decls.append(line)
        insert_at = None
        for m in re.finditer(r'^#include[^\n]*\n', raw, re.M):
            insert_at = m.end() if '"sym/' not in m.group(0) else insert_at
        first = re.search(r'^#include "(ee|zge|aggressive|sce)/[^\n]*\n', raw, re.M) or re.search(r'^#include <cstddef>\n', raw, re.M)
        if add and first:
            raw = raw[:first.start()] + '\n'.join(add) + '\n' + ('' if first.group(0)[10] != '<' or 1 else '') + raw[first.start():]
        for name, (result, plain) in tables.items():
            raw = re.sub(r'game::call<&(?:[\w:]+::)?%s>\(\s*' % name, lambda m: 'ee::call<%s>(sym::%s, ' % (result, plain), raw)
            raw = raw.replace('sym::%s, )' % plain, 'sym::%s)' % plain)
        if extra_decls:
            m = list(re.finditer(r'^#include[^\n]*\n', raw, re.M))[-1]
            raw = raw[:m.end()] + '\n' + '\n'.join(extra_decls) + '\n' + raw[m.end():]
        with open(path, 'w', errors='replace') as f:
            f.write(raw)
        moved += len(rows)
    for layer, addrs in removed.items():
        lines = []
        for line in open(manifest_path(layer), errors='replace'):
            if not line.startswith('#') and line.strip() and int(line.split('\t')[0], 16) in addrs:
                continue
            lines.append(line)
        with open(manifest_path(layer), 'w') as f:
            f.writelines(lines)
    print('moved %d functions into %d files (%d new), left %s' % (moved, len(per_file), len(created), dict(skipped)))
    for rel in created:
        print('new:', rel)
    return 0


def function_span(raw, name):
    """(start, end) of the function registered as `name`: its comment lines, its body, the registration, a GAME_CALL after it."""
    # (the last one: an adopted function is at the end, and one draft has a hand-written function's name)
    found = list(re.finditer(r'^[ \t]*READABLE_\w+\((?:sym::%s, )?%s_?(?:, \w+)?\);?[^\n]*\n' % (name, name), raw, re.M)) or \
        list(re.finditer(r'^[ \t]*READABLE_\w+\((?:sym::%s, )%s_?,[^\n]*\n' % (name, name), raw, re.M))
    if not found:
        return None
    m = found[-1]
    end = m.end()
    tail = re.match(r'[ \t]*GAME_CALL\w*\(%s\);[^\n]*\n' % name, raw[end:])
    if tail:
        end += tail.end()
    # Up over the body: the closing brace of the line above, then back to its opening one at the same indent.
    before = raw[:m.start()].rstrip('\n')
    close = before.rfind('\n') + 1
    indent = re.match(r'[ \t]*', before[close:]).group(0)
    opening = before.rfind('\n' + indent + '{\n')
    if opening < 0:
        return None
    start = before.rfind('\n', 0, opening) + 1
    # The signature may run over lines; comments above it go with it.
    while True:
        above = raw.rfind('\n', 0, start - 1) + 1
        line = raw[above:start].strip()
        if line.startswith('//') or (line and not line.endswith((';', '}', '{')) and not line.startswith('#')):
            start = above
        else:
            break
    return start, end


def back(args):
    """The functions out of their files: their rows are in the base commit's manifest and stay generated."""
    names = [l.split('\t')[0].strip() for l in open(args.plan[1:])] if args.plan.startswith('@') else args.plan.split(',')
    per_file = collections.defaultdict(list)
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    missing = 0
    for name in names:
        path = os.path.join(GAME, plan[name])
        raw = open(path, errors='replace').read()
        span = function_span(raw, name)
        if span is None:
            print('no such function here: %s in %s' % (name, plan[name]))
            missing += 1
            continue
        with open(path, 'w', errors='replace') as f:
            f.write(raw[:span[0]] + raw[span[1]:].lstrip('\n'))
    return sync(args)


def sync(args):
    """A layer's manifest: the base commit's rows less those with committed source."""
    code = ''
    for root, _, files in os.walk(GAME):
        if os.path.basename(root) in ('sweep',):
            continue
        for f in files:
            if f.endswith('.cpp'):
                code += open(os.path.join(root, f), errors='replace').read()
    registered = set(m.group(1) for m in re.finditer(r'^\s*READABLE_(?:FUNCTION(?:_WAITS|_VU0)?)\((?:sym::(\w+),|(?:\w+)\s*,)', code, re.M) if m.group(1))
    registered |= set(m.group(1).rstrip('_') for m in re.finditer(r'^\s*READABLE_(?:FUNCTION(?:_WAITS|_VU0)?)\((\w+)\s*,', code, re.M))
    # By address: one draft has the name of a hand-written function at another address.
    table = {}
    for line in open(os.path.join(ztree.TOOLS, 'readable_symbols.tsv'), errors='replace'):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 3 and p[0] == 'function' and not (len(p) > 4 and p[4]):
            table[p[1]] = int(p[2], 16)
    addresses = set(table[n] for n in registered if n in table)
    for layer in LAYERS:
        rel = os.path.relpath(manifest_path(layer), ztree.TREE)
        text = subprocess.run(['git', 'show', args.base + ':' + rel], cwd=ztree.TREE, stdout=subprocess.PIPE, encoding='latin-1').stdout
        lines = [l + '\n' for l in text.split('\n')[:-1]]
        lines = [l for l in lines if l.startswith('#') or not l.strip() or int(l.split('\t')[0], 16) not in addresses]
        with open(manifest_path(layer), 'w', encoding='latin-1') as f:
            f.writelines(lines)
    return 0


INT = r'(?:uint32_t|int32_t)'
POINTER_FIELDS = {}


def pointer_field(record, field):
    """The pointee of a struct's `ee::Ptr<T> field;`, from the headers and sources' text."""
    if not POINTER_FIELDS:
        for layer in LAYERS:
            for root, _, files in os.walk(os.path.join(GAME, layer)):
                for f in files:
                    if f.endswith(('.h', '.cpp')):
                        raw = open(os.path.join(root, f), errors='replace').read()
                        for m in re.finditer(r'^\s*struct (\w+)\s*(?://[^\n]*)?\n\s*\{\n(.*?)^\s*\};', raw, re.M | re.S):
                            for g in re.finditer(r'^\s*ee::Ptr<([^<>]*(?:<[^<>]*>)?)>\s+(\w+)\s*;', m.group(2), re.M):
                                POINTER_FIELDS[(m.group(1), g.group(2))] = g.group(1)
    return POINTER_FIELDS.get((record.split('::')[-1], field))


def apply_spec(signature, body, rows):
    """Hand rows for one function: (parameter or local, its name, its type): a parameter nothing named, a struct nothing typed."""
    for old, new, kind in rows:
        target = '%s %s' % (kind, new) if kind else None
        if target:
            signature, n = re.subn(r'(?:const\s+)?[\w:<>]+(?:\s*\*)?\s+\b%s\b(?=\s*[,)])' % old, target, signature)
            if not n:
                body, n = re.subn(r'^(\s*)(?:const\s+)?%s\s+%s\s*=\s*(.*);' % (INT, old), lambda m: '%s%s %s(%s);' % (m.group(1), kind, new, m.group(2)), body, flags=re.M)
        body = re.sub(r'(?<![\w.:>])%s\b' % old, new, body)
        signature = re.sub(r'(?<![\w.:>])%s\b' % old, new, signature)
    return signature, body


def retype(signature, body, skip):
    """A variable every use of which is `ee::Ptr<S>(v)` (a null test besides) becomes `ee::Ptr<S> v`."""
    found = 0
    names = re.findall(r'\b%s\s+(\w+)(?=\s*[,)])' % INT, signature) + re.findall(r'^\s*(?:const\s+)?%s\s+(\w+)\s*(?:=|;)' % INT, body, re.M)
    for v in sorted(set(names)):
        if v in skip:
            continue
        wrapped = re.findall(r'ee::Ptr<([^<>()]*(?:<[^<>]*>)?)>\(%s\)' % v, body)
        wrapped += re.findall(r'\bee::at<[^;\n]*?>\(%s\s*\+\s*offsetof\(([\w:]+),' % v, body)
        kinds = set(wrapped)
        if len(kinds) != 1:
            continue
        kind = kinds.pop()
        if kind.startswith('const ') or kind in ('char', 'void'):
            continue
        rest = re.sub(r'ee::Ptr<[^<>()]*(?:<[^<>]*>)?>\(%s\)' % v, '', body)
        rest = re.sub(r'^\s*(?:const\s+)?%s\s+%s\s*(?:=[^;]*)?;' % (INT, v), '', rest, flags=re.M)
        rest = re.sub(r'\bee::at<[^;\n]*?>\(%s(?:\s*\+\s*[\w()&:, ]+?)?\)' % v, '', rest)
        rest = re.sub(r'(?<![\w.])%s\s*(?:==|!=)\s*0\b|\bif \(!?%s\)' % (v, v), '', rest)
        if re.search(r'(?<![\w.>:])%s\b(?!\s*:)' % v, rest):
            continue
        typed = 'ee::Ptr<%s> %s' % (kind, v)
        signature = re.sub(r'\b%s\s+%s(?=\s*[,)])' % (INT, v), typed, signature)
        body = re.sub(r'^(\s*)(?:const\s+)?%s\s+%s\s*=\s*(.*);' % (INT, v), lambda m: '%s%s(%s);' % (m.group(1), typed, m.group(2)), body, flags=re.M)
        body = re.sub(r'^(\s*)(?:const\s+)?%s\s+%s\s*;' % (INT, v), lambda m: '%s%s;' % (m.group(1), typed), body, flags=re.M)
        body = re.sub(r'ee::Ptr<%s>\(%s\)' % (re.escape(kind), v), v, body)
        body = re.sub(r'(?<=\bee::at<)([^;\n]*?>\()%s\b(?=\s*(?:\+|\)))' % v, r'\1%s.address' % v, body)
        body = re.sub(r'(?<![\w.>:])%s\s*(?:==|!=)\s*0\b' % v, lambda m: m.group(0).replace(v, v + '.address'), body)
        # What it points to at a pointer field is that field's type: a local of it reads as one.
        for m in list(re.finditer(r'^(\s*)(?:const\s+)?%s\s+(\w+)\s*=\s*\*ee::at<u?int32_t>\(%s\.address\s*\+\s*offsetof\(([\w:]+),\s*(\w+)\)\);' % (INT, v), body, re.M)):
            target = pointer_field(m.group(3), m.group(4))
            if target:
                body = body.replace(m.group(0), '%s%s<PLACEHOLDER>' % (m.group(1), ''), 1).replace('<PLACEHOLDER>', 'ee::Ptr<%s> %s(%s.address + 0);' % (target, m.group(2), v) if False else 'const uint32_t %s = %s->%s.address;' % (m.group(2), v, m.group(4)))
        body = re.sub(r'\bif \((!?)%s\)' % v, lambda m: 'if (%s%s.address)' % (m.group(1), v), body)
        found += 1
    return signature, body, found


ARRAY_READ = re.compile(r'\bee::at<(u?int(?:8|16|32)_t|ee::Float)>\(\((\w+) << (\d)\) \+ (\w+)\)')
ARRAY_BYTE = re.compile(r'\bee::at<(u?int8_t)>\((\w+) \+ (\w+)\)')
SIZES = {'int32_t': 2, 'uint32_t': 2, 'ee::Float': 2, 'int16_t': 1, 'uint16_t': 1, 'int8_t': 0, 'uint8_t': 0}


def arrays(body):
    """`*ee::at<T>((i << k) + base)`, k the size of a T, is `ee::Ptr<T>(base)[i]`."""
    def read(m):
        if SIZES[m.group(1)] != int(m.group(3)):
            return m.group(0)
        return 'ee::Ptr<%s>(%s + (%s << %s))' % (m.group(1), m.group(4), m.group(2), m.group(3))
    def element(m):
        if SIZES[m.group(1)] != int(m.group(3)):
            return m.group(0)
        return '&ee::Ptr<%s>(%s)[%s]' % (m.group(1), m.group(4), m.group(2))
    return re.sub(r'\*\s*&', '', ARRAY_READ.sub(element, body))


def typed(args):
    names = [l.strip() for l in open(args.plan[1:])] if args.plan.startswith('@') else args.plan.split(',')
    spec = collections.defaultdict(list)
    if args.spec:
        for line in open(args.spec):
            p = line.rstrip('\n').split('\t')
            if p[0] and not p[0].startswith('#'):
                spec[p[0]].append((p[1], p[2], p[3] if len(p) > 3 else ''))
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    done = 0
    for name in names:
        path = os.path.join(GAME, plan[name])
        raw = open(path, errors='replace').read()
        span = function_span(raw, name)
        if span is None:
            continue
        text = raw[span[0]:span[1]]
        at = text.find('\n    {\n')
        if at < 0:
            continue
        signature, body = text[:at], text[at:]
        signature, body = apply_spec(signature, body, spec.get(name, ()))
        body = arrays(body)
        for _ in range(3):
            signature, body, found = retype(signature, body, ())
            if not found:
                break
            done += found
        new = signature + body
        if new != text:
            with open(path, 'w', errors='replace') as f:
                f.write(raw[:span[0]] + new + raw[span[1]:])
    print('typed %d variables' % done)
    return 0


LOW = re.compile(r'^(\s*)// Note: its name is a low-confidence guess \(readable_names\.tsv: (?:[\w-]+: )?(.*)\)\s*$')
MACHINE_TRAIL = re.compile(r'\s*// (?:the original\'s (?:tail call|branch back)|a branch back)\s*$')
MACHINE_NOTE = re.compile(r'^\s*// Note: (?:Ghidra declares|returns void|reads v\d|reads a\d)')


def comment_from_note(line):
    """What the names round read in the function, as its comment; nothing that talks in offsets or registers."""
    m = LOW.match(line)
    if not m:
        return None
    text = re.sub(r';?\s*\bdead\s*$', '', m.group(2)).strip()
    text = re.sub(r'^[\w]+: (?=\()', '', text)
    text = re.sub(r'^\(\.\.\):\s*|^\(\)\s*(?:->|:)\s*', '', text)
    if not text or re.search(r'0x|\+\d|\bv0\b|\ba[0-3]\b|FUN_|fn_|\bjal\b|^empty$|^returns \d', text) or len(text) < 12:
        return ''
    text = text[0].upper() + text[1:] + ('.' if text[-1] not in '.)' else '')
    words = text.split(' ')
    out, cur = [], m.group(1) + '//'
    for w in words:
        if len(cur) + len(w) + 1 > 110:
            out.append(cur)
            cur = m.group(1) + '//'
        cur += ' ' + w
    return '\n'.join(out + [cur]) + '\n'


def tidy(args):
    """The drafter's notes about itself go: what the names round read stays as the comment; the anonymous blocks' lines
    nothing uses after the passes go too. Files are the ones the functions are in."""
    names = [l.strip() for l in open(args.plan[1:])] if args.plan.startswith('@') else args.plan.split(',')
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    files = collections.defaultdict(list)
    for n in names:
        files[plan[n]].append(n)
    for rel, group in sorted(files.items()):
        path = os.path.join(GAME, rel)
        raw = open(path, errors='replace').read()
        for name in group:
            span = function_span(raw, name)
            if not span:
                continue
            text = raw[span[0]:span[1]]
            lines = text.split('\n')
            kept = []
            for line in lines:
                if MACHINE_NOTE.match(line):
                    continue
                if LOW.match(line):
                    c = comment_from_note(line)
                    if c:
                        kept.append(c.rstrip('\n'))
                    continue
                kept.append(MACHINE_TRAIL.sub('', line))
            new = '\n'.join(l for l in kept if l.strip() != '// fp = sp (the frame pointer)')
            # A local named for its callee: what it holds is the call's result.
            for local in sorted(set(re.findall(r'^\s*(?:const\s+)?[\w:<>]+\s+(\w+_)\s*=', new, re.M))):
                new = re.sub(r'(?<![\w:&.])%s(?!\w)' % local, local[:-1] + 'Result', new)
            # `const int32_t x = f(); return x;` is `return f();`
            new = re.sub(r'^(\s*)const u?int32_t (\w+) = ([^;\n]*);\n\s*return \2;[ \t]*$', r'\1return \3;', new, flags=re.M)
            raw = raw[:span[0]] + new + raw[span[1]:]
        # Declarations carried with a draft that nothing reads now; a line the file already had is its owner's.
        base = subprocess.run(['git', 'show', 'HEAD:' + os.path.relpath(path, ztree.TREE)], cwd=ztree.TREE, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, encoding='latin-1').stdout
        owned = set(base.split('\n'))
        changed = True
        while changed:
            changed = False
            code = code_only(raw)
            for m in re.finditer(r'^        (?:inline ee::Global<[^;\n]*>|constexpr uint32_t)\s+(\w+)[^\n]*\n', raw, re.M):
                if m.group(0).rstrip('\n') not in owned and len(re.findall(r'\b%s\b' % m.group(1), code)) == 1:
                    raw = raw[:m.start()] + raw[m.end():]
                    changed = True
                    break
        raw = re.sub(r'\n    namespace\n    \{\n    \}\n', '\n', raw)
        raw = re.sub(r'\n    namespace // the game functions below call: ee/plain.h\n    \{\n    \}\n', '\n', raw)
        with open(path, 'w', errors='replace') as f:
            f.write(raw)
    return 0


ROUTE = re.compile(r'\bgame::(call|callAt|tailCall|tailCallAt)<&((?:\w+::)*)(\w+?)_?(?:, sym::(\w+))?>\(')


TABLE = re.compile(r'\bee::(call|tailCall)(<[\w:]+>)?\((k[A-Z]\w+)(, |\))')


def routed(args):
    """`game::callAt<&zge::poolRecord_, sym::poolRecord>(x)` in an adopted function, where a plain name is hidden or two bodies
    fold: `routed::poolRecord(x)`, one GAME_ROUTE line per name in the file (ee/route.h), the routing unchanged."""
    names = [l.strip() for l in open(args.plan[1:])]
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    files = collections.defaultdict(list)
    for n in names:
        files[plan[n]].append(n)
    total = 0
    for rel, group in sorted(files.items()):
        path = os.path.join(GAME, rel)
        raw = open(path, errors='replace').read()
        lines = {}
        first = None
        for name in group:
            span = function_span(raw, name)
            if not span:
                continue
            first = span[0] if first is None else min(first, span[0])
            text = raw[span[0]:span[1]]

            def one(m):
                short = m.group(3)
                form = m.group(1)
                alias = short + ('Tail' if form.startswith('tail') else '')
                template = m.group(0)[:-1]
                lines[alias] = '            GAME_ROUTE(%s, %s);' % (alias, template)
                return 'routed::%s(' % alias
            new, n = ROUTE.subn(one, text)

            def table(m):
                alias = m.group(3)[1].lower() + m.group(3)[2:] + ('Tail' if m.group(1) == 'tailCall' else '') + re.sub(r'\W', '', m.group(2) or '')
                lines[alias] = '            GAME_TABLE_ROUTE(%s, ee::%s%s, %s);' % (alias, 'game' if m.group(1) == 'call' else 'tailCall', m.group(2) or '', m.group(3))
                return 'routed::%s(%s' % (alias, '' if m.group(4) == ')' else '') + (')' if m.group(4) == ')' else '')
            new, k = TABLE.subn(table, new)
            n += k
            if n:
                total += n
                raw = raw[:span[0]] + new + raw[span[1]:]
                if span[0] < first:
                    first = span[0]
        if lines:
            at = raw.rfind('\n', 0, first) + 1
            block = '    namespace\n    {\n        namespace routed\n        {\n' + '\n'.join(lines[k] for k in sorted(lines)) + '\n        }\n    }\n\n'
            raw = raw[:at] + block + raw[at:]
            if '"ee/route.h"' not in raw:
                inc = re.search(r'^#include "ee/readable.h"\n', raw, re.M) or re.search(r'^#include "ee/abi.h"\n', raw, re.M) or re.search(r'^#include <cstddef>\n', raw, re.M)
                raw = raw[:inc.end()] + '#include "ee/route.h"\n' + raw[inc.end():]
            with open(path, 'w', errors='replace') as f:
                f.write(raw)
    print('routed %d calls' % total)
    return 0


RECORD_HEADERS = (('aggressive::Edit', 'aggressive/level/edit_records.h'), ('aggressive::Park', 'aggressive/level/edit_records.h'), ('aggressive::', 'aggressive/game_records.h'),
                  ('zge::', 'zge/engine_records.h'))
FIELD_READ = re.compile(r'(\*)?\bee::at<([\w:]+)>\(([\w.]+) \+ (k[A-Z]\w+)\)')


def layout_rows():
    """{constant: (struct, offset, field)} of readable_names_layout.tsv's confident rows, as the drafter names them."""
    rows = {}
    for line in open(os.path.join(ztree.TOOLS, 'readable_names_layout.tsv'), errors='replace'):
        p = line.rstrip('\n').split('\t')
        if line.startswith('#') or len(p) < 4 or not re.fullmatch(r'0x[0-9a-fA-F]+', p[1]) or p[3] == 'low':
            continue
        short = p[0].split('::')[-1]
        rows['k%s%s' % (short, p[2][0].upper() + p[2][1:])] = (p[0], int(p[1], 16), p[2])
    return rows


def known_structs():
    names = set()
    for layer in LAYERS + ('sce',):
        for root, _, files in os.walk(os.path.join(ztree.GAME, layer)):
            for f in files:
                if f.endswith('.h'):  # (one a source keeps to itself is in an anonymous namespace: no clash)
                    names |= set(re.findall(r'^\s*(?:struct|using) (\w+)\b[^;\n]*', open(os.path.join(root, f), errors='replace').read(), re.M))
    return names | {'Matrix', 'Point', 'Path', 'Search', 'Model'}


def records(args):
    """The layout table's structs no source defines, from what the adopted functions read: a header of them, the reads typed.
    A field read at two widths stays as it was, and is listed."""
    names = [l.strip() for l in open(args.plan[1:])]
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    table = layout_rows()
    exists = known_structs()
    uses = collections.defaultdict(collections.Counter)
    files = collections.defaultdict(list)
    for n in names:
        files[plan[n]].append(n)
    texts = {}
    for rel, group in files.items():
        path = os.path.join(GAME, rel)
        if not os.path.exists(path):
            continue
        raw = texts[rel] = open(path, errors='replace').read()
        for name in group:
            span = function_span(raw, name)
            if span:
                for m in FIELD_READ.finditer(raw[span[0]:span[1]]):
                    uses[m.group(4)][m.group(2)] += 1
    fields = collections.defaultdict(dict)
    width = {'int32_t': 4, 'uint32_t': 4, 'ee::Float': 4, 'int16_t': 2, 'uint16_t': 2, 'int8_t': 1, 'uint8_t': 1}
    mixed = []
    for constant, kinds in uses.items():
        if constant not in table or table[constant][0].split('::')[-1] in exists:
            continue
        record, offset, field = table[constant]
        if not record.startswith(('aggressive::', 'zge::')) or len({width.get(k, 0) for k in kinds}) != 1 or 0 in {width.get(k, 0) for k in kinds} or ('ee::Float' in kinds and len(kinds) > 1):
            mixed.append(constant)
            continue
        fields[record][constant] = (offset, field, kinds.most_common(1)[0][0])
    # Header text, per struct in offset order, padding between.
    per_header = collections.defaultdict(list)
    usable = {}
    for record, members in sorted(fields.items()):
        header = next(h for prefix, h in RECORD_HEADERS if record.startswith(prefix))
        space, short = record.rsplit('::', 1)
        body, at, asserts = [], 0, []
        for constant, (offset, field, kind) in sorted(members.items(), key=lambda kv: kv[1][0]):
            if offset < at or offset % width[kind]:
                mixed.append(constant)
                continue
            if offset > at:
                body.append('        uint8_t _%02x[0x%X];' % (at, offset - at))
            body.append('        %s %s;' % (kind, field))
            asserts.append('    static_assert(offsetof(%s, %s) == 0x%X);' % (short, field, offset))
            at = offset + width[kind]
            usable[constant] = (short, field, kind, record)
        per_header[header].append((space, '    struct %s\n    {\n%s\n    };\n%s\n' % (short, '\n'.join(body), '\n'.join(asserts))))
    for header, blocks in per_header.items():
        text = '#pragma once\n\n// Records the adopted functions read: their layout and names are the work database\'s (readable_names_layout.tsv);\n// each has only the fields code reads so far.\n\n#include "ee/ee.h"\n\n#include <cstddef>\n#include <cstdint>\n\n'
        for space in sorted(set(b[0] for b in blocks)):
            text += 'namespace %s\n{\n%s}\n\n' % (space, '\n'.join(b[1] for b in blocks if b[0] == space))
        with open(os.path.join(GAME, header), 'w') as f:
            f.write(text.rstrip('\n') + '\n')
    # The reads.
    count = 0
    for rel, group in files.items():
        raw = texts.get(rel)
        if raw is None:
            continue
        spaces = set()
        for name in group:
            span = function_span(raw, name)
            if not span:
                continue

            def one(m):
                nonlocal count
                star, kind, base, constant = m.groups()
                if constant not in usable or not star:
                    return m.group(0)
                short, field, wanted, record = usable[constant]
                spaces.add(record)
                count += 1
                base = base[:-len('.address')] if base.endswith('.address') else base
                text = 'ee::Ptr<%s>(%s)->%s' % (record.split('::', 1)[1] if False else record, base, field)
                return text if kind == wanted else 'static_cast<%s>(%s)' % (kind, text)
            new = raw[span[0]:span[1]]
            for _ in range(3):
                new = FIELD_READ.sub(one, new)
            # A write through a read's cast is a plain write.
            new = re.sub(r'^(\s*)static_cast<[\w:]+>\((ee::Ptr<[\w:]+>\(\w+\)->\w+)\) = ', r'\1\2 = ', new, flags=re.M)
            raw = raw[:span[0]] + new + raw[span[1]:]
        if spaces:
            headers = sorted({next(h for prefix, h in RECORD_HEADERS if r.startswith(prefix)) for r in spaces})
            inc = re.search(r'^#include "ee/readable.h"\n', raw, re.M) or re.search(r'^#include "ee/abi.h"\n', raw, re.M) or re.search(r'^#include <cstddef>\n', raw, re.M)
            add = ''.join('#include "%s"\n' % h for h in headers if '"%s"' % h not in raw)
            raw = raw[:inc.end()] + add + raw[inc.end():]
        with open(os.path.join(GAME, rel), 'w', errors='replace') as f:
            f.write(raw)
    print('records: %d structs, %d fields, %d reads typed; left out: %d' % (len(fields), len(usable), count, len(mixed)))
    print('left: ' + ' '.join(sorted(mixed)[:30]))
    return 0


def hand(args):
    """FILE: `=== name` lines, each followed by that function's hand-written text, comment and registration included."""
    plan = {}
    for line in open(args.placed):
        p = line.rstrip('\n').split('\t')
        plan[p[1]] = p[5]
    text = open(args.plan).read()
    done = 0
    for m in re.finditer(r'^=== (\w+)\n(.*?)(?=^=== |\Z)', text, re.M | re.S):
        name, new = m.group(1), m.group(2).rstrip('\n') + '\n'
        path = os.path.join(GAME, plan[name])
        raw = open(path, errors='replace').read()
        span = function_span(raw, name)
        if span is None:
            print('no such function: %s' % name)
            continue
        with open(path, 'w', errors='replace') as f:
            f.write(raw[:span[0]] + new + raw[span[1]:])
        done += 1
    print('hand-written: %d' % done)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('step', choices=('move', 'back', 'sync', 'typed', 'tidy', 'hand', 'routed', 'records'))
    ap.add_argument('plan', nargs='?')
    ap.add_argument('--spec')
    ap.add_argument('--placed', help='readable_place.py plan\'s OUT.tsv (back, typed, tidy, routed, records, hand read it)')
    ap.add_argument('--base', default='e4279ba9')
    ap.add_argument('--build', default=os.path.join(ztree.TREE, 'out', 'pass'))
    ap.add_argument('--only')
    ap.add_argument('--layers', default=','.join(LAYERS))
    a = ap.parse_args()
    if a.placed is None and a.step in ('back', 'typed', 'tidy', 'routed', 'records', 'hand'):
        ap.error('%s needs --placed (readable_place.py plan OUT.tsv)' % a.step)
    return {'move': move, 'back': back, 'sync': sync, 'typed': typed, 'tidy': tidy, 'hand': hand, 'routed': routed, 'records': records}[a.step](a)


if __name__ == '__main__':
    sys.exit(main())
