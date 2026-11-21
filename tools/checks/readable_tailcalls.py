#!/usr/bin/env python3
"""readable_tailcalls.py: readable bodies that make a plain call where the original tail-calls.

Where the original ends a function with `j callee` (its tail call: the callee runs at the caller's own sp and its
return is the caller's), the readable body calls it as the original does, through game::tail (game::tail(fn, ...),
game::tailCall<&fn>, a tail:: wrapper on ee::tailCall for a guest callee). A plain call `callee(...)` runs the callee
below the caller's frame instead (kCalleeStackGap below a frameless caller's sp), so the callee's frame and the stale
stack it leaves are not the original's, and a game that copies stale stack into heap records differs in RAM
(verify2's finding; verify3 fixed ~74 such sites in DM2 by hand from a call-site diagnostic run, e029884f7, and
three were A/B culprits: updatePose, restoreSavedLevelObjects, runLevelContacts).

This is the static form of that diagnostic, every game, every body, no run. For each game (ai, dm2, bmx) and each
registered body whose C++ runs in that game's instance (readable_symbols.py Levels: level 2; a game folder's own
registrations; READABLE_FUNCTION_ONLY's in their game; an _OWN body in its game), the original's instructions
(the game's recompiled code, its `// 0x...: j func_X` and `jal func_X` comments) give the functions it tail-calls
(`j`, and how many `j` instructions go to each) and the ones it calls (`jal`). The body is read as that game's
instance compiles it (readable_symbols.skip_game_steps). A site is
  - a plain call anywhere in the body of a function the original only tail-calls (it `j`s to it and never `jal`s
    it): the original gives back that callee's result, the readable makes a call and gives it back itself;
  - a plain call on one of the body's return paths of a function the original `j`s to (and may `jal` elsewhere),
    while the body tail-calls it fewer times than the original's `j` instructions to it (each one past those is a
    site, in the body's order). A return path is every `return f(...);` (f's call alone, or in a cast), a call
    statement followed by `return;` (or by `break;` in a switch the function ends with), and the last statement of
    the body and of each if/else arm, block and switch the function ends with, at any depth (a loop's last
    statement is none). A call there of a C++ helper of the file (or of a header of its folder's first part) that
    is no game function is read through: the helper's own return paths, two deep (DM2's runUiScript ending in
    zgui's runScriptLines). frame1's callee_sp measure found 16 DM2 bodies so, this rule all 16.

What it does not see: a call in a helper the body reaches other than on a return path (only the registered body is
read); a call through a pointer or ee::game; which of a callee's several calls on return paths the original's `j`s
are, past their number; a registered function whose game has no recompiled code here (its generated folder is
missing: said, not counted). The reverse (a tail call of what the original only calls) is not counted: a body that
returns a jal'd call's value through game::tail can be right there, and verify2's tailcalls.py reads it from a run.

    readable_tailcalls.py               the sites, then the counts by game and folder
    readable_tailcalls.py --check       exit 1 if a game's folder has more sites than its baseline (those folders'
                                        sites listed)
    readable_tailcalls.py --rebaseline  write the baseline (only ever down: it refuses a rise)
    readable_tailcalls.py --json FILE   the counts as JSON
    readable_tailcalls.py --games dm2   only these games (ai,dm2,bmx)

Folders are the first two path parts under src/game, as readable_plaincalls.py's. The baseline is
readable_tailcalls_baseline.json next to this file, at main's counts when it was made: 153 (AI 45, DM2 58, BMX XXX 50),
the sites no run had shown (verify3's 74 are the difference between e029884f7^, 132 in DM2, and e029884f7, 58: every
one it fixed, and none it did not); 0 from b120 (tail2); with the return paths read (tools25), 34 at main fa93c7220
(AI 4, DM2 25, BMX XXX 5; frame1's head 06b86cb46: 13). About 15 s for the three games.
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py; puts its tools/readable on sys.path)
import readable_digest as rd  # noqa: E402
import readable_symbols as rs  # noqa: E402
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')

GAME = os.path.normpath(ztree.TREE)   # (its readable code: ztree.walk, key_of)
BASELINE = os.path.join(HERE, 'readable_tailcalls_baseline.json')
GAMES = ('ai', 'dm2', 'bmx')
JUMP = re.compile(r'//\s*0x([0-9a-fA-F]+):\s*0x[0-9a-fA-F]+\s+(j|jal)\s+func_([0-9A-Fa-f]+)')
FILE_ADDR = re.compile(r'_0x([0-9a-fA-F]+)\.cpp$')
# a call: a name (with its qualifiers) and its '(' ; a member call (.f, ->f) and an address (&f) are no plain call
CALL = re.compile(r'((?:\b\w+\s*::\s*)*)\b([A-Za-z_]\w*)\s*\(')
# qualifiers of a dispatch, not of a callee's namespace: what follows is no plain call of a game function
DISPATCH = ('tail', 'ee', 'game', 'std', 'sym', 'points')


def generated_dir(game):
    if game == 'ai':
        return os.path.join(zaxis_paths.get('AI_WORKSPACE'), 'recomp', 'generated')
    if game == 'dm2':
        return os.path.join(zaxis_paths.get('DM2_WORKSPACE'), 'recomp', 'generated-ns')
    recomp = zaxis_paths.get('BMX_RECOMP', required=False) or os.path.join(SCRATCH_ROOT, 'zbind1', 'bmx', 'recomp')
    return os.path.join(recomp, 'generated-ns')


class Original:
    """A game's recompiled functions: the targets each one `j`s to and `jal`s, read from its instruction comments."""

    def __init__(self, directory):
        self.directory = directory
        self.files = collections.defaultdict(list)
        for f in os.listdir(directory):
            m = FILE_ADDR.search(f)
            if m:
                self.files[int(m.group(1), 16)].append(f)
        self.cache = {}

    def has(self, address):
        return address in self.files

    def jumps(self, address):
        """(the targets it `j`s to, the ones it `jal`s, {target: how many `j` instructions go to it})."""
        if address not in self.cache:
            js, jals, sites = set(), set(), collections.defaultdict(set)
            for f in self.files.get(address, ()):
                for m in JUMP.finditer(open(os.path.join(self.directory, f), errors='replace').read()):
                    target = int(m.group(3), 16)
                    (js if m.group(2) == 'j' else jals).add(target)
                    if m.group(2) == 'j':
                        sites[target].add(int(m.group(1), 16))  # (by the instruction's address: a chunk twice is one)
            self.cache[address] = (js, jals, {t: len(a) for t, a in sites.items()})
        return self.cache[address]


def folder(path):
    parts = ztree.key_of(path, GAME).replace(os.sep, '/').split('/')
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def definition(code, name):
    """[(start, end)] of every `name(...) [qualifiers] {...}` in the blanked code (offsets are the file's)."""
    out = []
    for m in re.finditer(r'\b%s\s*\(' % re.escape(name), code):
        p = m.end() - 1
        q = rd.match(code, p, '(', ')')
        if q < 0:
            continue
        t = re.match(r'\s*(?:const\s*|noexcept\s*|override\s*|final\s*|->\s*[\w:<>,\s*&]+?\s*)*\{', code[q:q + 200])
        if not t:
            continue
        b = q + t.end() - 1
        e = rd.match(code, b, '{', '}')
        if e > 0:
            out.append((m.start(), e))
    return out


def plain_calls(body):
    """[(offset, name)] of the plain calls in a body's text (its own name's first match is its head: left out)."""
    out = []
    for m in CALL.finditer(body):
        quals = [q.strip() for q in m.group(1).split('::') if q.strip()]
        if quals and quals[0] in DISPATCH:
            continue
        before = body[:m.start()].rstrip()
        if before.endswith(('.', '->', '&')) or m.group(2) in rd.KEYWORDS:
            continue
        out.append((m.start(2), m.group(2)))
    return out


HEAD = re.compile(r'(if|else|for|while|switch|do|return|case|default|break)\b')
LABEL = re.compile(r'\s*(?:case\b[^;{}]*?[^:]:(?!:)|default\s*:(?!:))')
# a tail call in the readable: game::tail(fn, ...), game::tail<T>(fn, ...), ee::tailCall<T>(sym::fn, ...),
# game::tailCall<&fn>(...), tailCallAt, and a tail:: wrapper (tail::heapFree(...)); group 1 or 2 or 3 names the callee
TAIL = re.compile(r'(?:\b(?:game|ee)\s*::\s*)?\b(?:tail|tailCall|tailCallAt)\s*(?:<\s*&\s*([\w:]+)\s*>|<[^<>()]*>)?\s*\('
                  r'(?:\s*&?\s*((?:\w+\s*::\s*)*\w+)\s*[,)])?|\btail\s*::\s*(\w+)\s*\(')


def statements(text, i, j):
    """[(start, end)] of the statements in text[i:j] at its own level (labels `case X:` left out of each)."""
    out = []
    while True:
        m = LABEL.match(text, i)
        while m and m.end() <= j:
            i = m.end()
            m = LABEL.match(text, i)
        while i < j and text[i].isspace():
            i += 1
        if i >= j:
            return out
        end = min(rs._statement_end(text, i), j)
        if end <= i:
            return out
        out.append((i, end))
        i = end


def tail_positions(text, i, j, tail, out, switch=False):
    """The statements of text[i:j], at every depth, after which the function returns with nothing more done: one
    followed by `return;` (or by `break;` in a switch in tail position), and the last of a block in tail position
    (the body's; an if/else arm's, a block's or a switch's in tail position). A loop's last statement is none (the loop
    goes on), its `f(); return;` is one."""
    sts = statements(text, i, j)
    for k, (a, b) in enumerate(sts):
        nxt = text[sts[k + 1][0]:sts[k + 1][1]].strip() if k + 1 < len(sts) else None
        here = bool((tail and nxt is None) or (nxt is not None and re.fullmatch(r'return\s*;', nxt)) or (
            switch and tail and nxt is not None and re.fullmatch(r'break\s*;', nxt)))
        st = text[a:b]
        if st.startswith('{'):
            tail_positions(text, a + 1, b - 1, here, out)
            continue
        h = HEAD.match(st)
        if h and h.group(1) in ('if', 'for', 'while', 'switch'):
            c = re.compile(r'%s\s*(?:constexpr\s*)?\(' % h.group(1)).match(text, a)
            close = rd.match(text, c.end() - 1, '(', ')') if c else -1
            if close < 0:
                continue
            if h.group(1) == 'switch':
                o = text.find('{', close, b)
                if o >= 0:
                    tail_positions(text, o + 1, b - 1, here, out, switch=True)
                continue
            body_end = min(rs._statement_end(text, close), b)
            arm = here and h.group(1) == 'if'
            tail_positions(text, close, body_end, arm, out)
            e = re.compile(r'\s*else\b').match(text, body_end) if h.group(1) == 'if' else None
            if e and e.end() < b:
                tail_positions(text, e.end(), b, arm, out)
            continue
        if h and h.group(1) == 'do':
            tail_positions(text, a + 2, rs._statement_end(text, a + 2), False, out)
            continue
        if h:
            continue        # (a return, a break, a label's word: no call made last there)
        if here:
            out.append((a, b))


def returned(text):
    """[(start, end)] of the expressions of the `return EXPR;` statements of a body, every one."""
    out = []
    for m in re.finditer(r'\breturn\b', text):
        end = rs._statement_end(text, m.start())
        expr = text[m.end():end].rstrip().rstrip(';')
        if expr.strip():
            out.append((m.end(), m.end() + len(expr)))
    return out


CAST = re.compile(r'\s*(?:(?:static_cast|reinterpret_cast|ee::bit_cast|std::bit_cast)\s*<[^;{}]*?>\s*\(|\(\s*(?:const\s+)?'
                  r'(?:\w+\s*::\s*)*\w+\s*(?:<[^;{}()]*>)?\s*\**\s*\)(?=\s*[\w(]))')
# a functional cast's type: int32_t(...), ee::Float(...), ee::Ptr<T>(...), bool(...)
TYPE_NAME = re.compile(r'(?:u?int(?:8|16|32|64)_t|float|double|bool|char|unsigned|Float|Ptr|Address|Word|Half)$')


def whole_call(text, a, b):
    """(qualifiers, name, offset) when text[a:b] is one call and nothing else (`f(x)`, `zge::f(x);`, the call in a
    cast: `static_cast<uint32_t>(f(x))`, `(int32_t)f(x)`, `ee::Float(f(x))`), else None."""
    while True:
        c = CAST.match(text, a)
        if not c:
            break
        if text[c.end() - 1] == '(':        # (a keyword cast: its parentheses hold the whole of it)
            close = rd.match(text, c.end() - 1, '(', ')')
            if close < 0 or text[close:b].strip() not in ('', ';'):
                return None
            a, b = c.end(), close - 1
        else:
            a = c.end()
    m = re.compile(r'\s*((?:\w+\s*::\s*)*)([A-Za-z_]\w*)\s*(<[^;{}()]*>)?\s*\(').match(text, a)
    if not m or m.group(2) in rd.KEYWORDS:
        return None
    close = rd.match(text, m.end() - 1, '(', ')')
    if close < 0 or text[close:b].strip() not in ('', ';'):
        return None
    if TYPE_NAME.match(m.group(2)):
        return whole_call(text, m.end(), close - 1)
    if m.group(3):
        return None         # (a template's call: game::tail<T>, a dispatch; read by tail_targets)
    return [q.strip() for q in m.group(1).split('::') if q.strip()], m.group(2), m.start(2)


def return_calls(body):
    """[(offset, name)] of the plain calls a body makes on a return path: `return f(...);` (in a cast or not), and a
    call statement after which the function returns (followed by `return;`, or the last of a block the function ends
    with)."""
    if '{' not in body:
        return []
    o = body.find('{')
    spans = returned(body)
    tail_positions(body, o + 1, len(body) - 1, True, spans)
    out = []
    for a, b in spans:
        c = whole_call(body, a, b)
        if c and not (c[0] and c[0][0] in DISPATCH):
            out.append((c[2], c[1]))
    return sorted(set(out))


def tail_targets(body):
    """{name: how many} of the callees a body tail-calls through game::tail, tailCall or a tail:: wrapper."""
    out = collections.Counter()
    for m in TAIL.finditer(body):
        name = m.group(1) or m.group(2) or m.group(3)
        if name:
            out[re.split(r'\s*::\s*', name)[-1]] += 1
    return out


def game_sites(game, t, sources, say):
    """[(folder, path, line, caller, caller address, callee, callee address)] of one game."""
    directory = generated_dir(game)
    if not os.path.isdir(directory):
        say('readable_tailcalls: %s: no recompiled code in %s: not counted' % (game, directory))
        return None
    original = Original(directory)
    if game == 'ai':
        bind = rs.Bind(None)
    else:
        table = os.path.join(zaxis_paths.get('GAME_BIND_DIR', required=False) or
                             os.path.join(SCRATCH_ROOT, 'workdb', 'bind'), game + '.tsv')
        if not os.path.exists(table):
            say('readable_tailcalls: %s: no bind table %s: not counted' % (game, table))
            return None
        bind = rs.Bind(table)
    levels = rs.Levels(bind, t, GAME, None if game == 'ai' else game)
    instance = None
    if game != 'ai':
        inst = ztree.readable(GAME, game, 'instance_sources.txt')
        instance = {l.strip() for l in open(inst) if l[:1].isalpha()} if os.path.exists(inst) else set()
    mine = t.own(game) if game != 'ai' else None

    def address_in(rel_game, name):
        """The game's address of a function named in a source (a game folder's: its own row first), or None."""
        if rel_game and name in mine.syms:
            entry = mine.syms[name]
            return entry[1] if entry[0] == 'function' else None
        name = levels.entry_of.get(name, name)     # (an _OWN body's name: its symbol)
        if name in levels.only:
            return levels.only[name]
        entry = t.syms.get(name)
        if not entry or entry[0] != 'function':
            return None
        return bind.function(entry[1])

    headers = {}

    def helper_bodies(code, start, end, top, name):
        """The texts of the definitions of a C++ helper (no function of the game) a body calls: the file's own
        (the body itself left out), else the headers' of its folder's first part."""
        found = [code[a:b] for a, b in definition(code, name) if not (a <= start < b)]
        if found:
            return found
        if top not in headers:
            headers[top] = []
            for h in sources:
                rh = ztree.key_of(h, GAME).replace(os.sep, '/')
                if rh.endswith('.h') and rh.split('/')[0] == top:
                    headers[top].append(rs.blank(open(h, encoding='utf-8', errors='replace').read()))
        return [hc[a:b] for hc in headers[top] if name in hc for a, b in definition(hc, name)]

    def path_calls(code, start, end, view, depth, top=None):
        """[(offset in the view, callee, the helper it is reached through or None)]: the calls on the body's return
        paths, and through a helper on one (the helper's own return paths, two helpers deep)."""
        out = []
        for at, callee in return_calls(view):
            if address_in(rel_game, callee) is not None:
                out.append((at, callee, None))
            elif depth < 2:
                for h in helper_bodies(code, start, end, top, callee):
                    hv = rs.skip_game_steps(h, game, levels.flag_constants)
                    out.extend((at, c, callee) for _, c, _ in path_calls(code, start, end, hv, depth + 1, top))
        return out

    sites = []
    for path in sources:
        rel = ztree.key_of(path, GAME).replace(os.sep, '/')
        rel_game = rs.folder_of(rel)
        if game == 'ai':
            if rel_game:
                continue
        elif rel_game != game and rel not in instance:
            continue
        text = open(path, encoding='utf-8', errors='replace').read()
        if 'READABLE_' not in text:
            continue
        code = rs.blank(text)
        regs = []
        for name, short, _, own in rs.registrations_in(rd.strip(text), None if game == 'ai' else game):
            if rel_game:
                address = address_in(rel_game, name)
            elif game == 'ai':
                entry = t.syms.get(name)
                address = entry[1] if entry and entry[0] == 'function' else None
            else:
                address = levels.address(name) if levels.level.get(name) == 2 else None
            regs.append((name, own or short, address))
        for m in rs._REG_ONLY.finditer(rd.strip(text)):
            if m.group(2) == game:
                regs.append((m.group(3), m.group(3), levels.only.get(m.group(3))))
        lines = None
        for name, body_name, address in regs:
            if address is None or not original.has(address):
                continue
            js, jals, j_sites = original.jumps(address)
            only_tails = js - jals - {address}
            both = (js & jals) - {address}
            if not js - {address}:
                continue
            for start, end in definition(code, body_name):
                body = code[start:end]
                view = rs.skip_game_steps(body, game, levels.flag_constants)
                seen = set()
                for at, callee in plain_calls(view[view.find('{'):]):
                    target = address_in(rel_game, callee)
                    if target is None or target not in only_tails or callee in seen:
                        continue
                    seen.add(callee)
                    if lines is None:
                        lines = [0] + [i + 1 for i, c in enumerate(text) if c == '\n']
                    k = re.search(r'\b%s\s*\(' % re.escape(callee), body[body.find('{'):])
                    offset = start + body.find('{') + (k.start() if k else 0)
                    line = sum(1 for x in lines if x <= offset)
                    sites.append((folder(path), rel, line, name, address, callee, target, 'only'))
                if not both and not only_tails:
                    continue
                # a callee the original both calls (jal) and tail-calls (j): a plain call on a return path, while the
                # body tail-calls it fewer times than the original's j sites, is one of those j (frame1's 16 DM2
                # bodies: the second disableContact of bailBodyRelease)
                tails = tail_targets(view)
                missing = collections.Counter()
                for at, callee, via in path_calls(code, start, end, view, 0, rel.split('/')[0]):
                    target = address_in(rel_game, callee)
                    if target not in both and (target not in only_tails or callee in seen):
                        continue    # (a plain call in the body of what the original only tail-calls: said above)
                    if missing[callee] >= j_sites.get(target, 0) - tails[callee]:
                        continue
                    missing[callee] += 1
                    if lines is None:
                        lines = [0] + [i + 1 for i, c in enumerate(text) if c == '\n']
                    offset = start + body_offset(body, view, at, via or callee)
                    line = sum(1 for x in lines if x <= offset)
                    sites.append((folder(path), rel, line, name, address, callee, target,
                                  ('both:' if target in both else 'only:') + (via or '')))
    return sites


def body_offset(body, view, at, name):
    """Where in the body the call of `name` at `at` in the game's view of it is: the same place when the view is the
    body, else the call of that name nearest the same share of the way through (the view only drops text)."""
    if len(view) == len(body):
        return at
    k = [x.start() for x in re.finditer(r'\b%s\s*[(<]' % re.escape(name), body)]
    if not k:
        return 0
    guess = at * len(body) / max(1, len(view))
    return min(k, key=lambda x: abs(x - guess))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--check', action='store_true')
    p.add_argument('--rebaseline', action='store_true')
    p.add_argument('--json')
    p.add_argument('--games', default=','.join(GAMES))
    p.add_argument('--quiet', action='store_true', help='the counts only, not every site')
    a = p.parse_args()
    t = rs.table()
    sources = sorted(rs.game_sources(GAME))
    counts = {}
    total = 0
    missing = []
    for game in a.games.split(','):
        sites = game_sites(game, t, sources, print)
        if sites is None:
            missing.append(game)
            continue
        c = collections.Counter(s[0] for s in sites)
        counts[game] = dict(sorted(c.items()))
        total += len(sites)
        if not a.quiet:
            base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
            for f, rel, line, caller, ca, callee, ta, kind in sorted(sites):
                if a.check and c[f] <= base.get(game, {}).get(f, 0):
                    continue    # (--check: the sites of a folder above its baseline only)
                via = kind.partition(':')[2]
                print('%s %s:%d: %s (0x%06X) calls %s (0x%06X)%s, which the original %s: game::tail' % (
                    game, rel, line, caller, ca, callee, ta, ' through %s' % via if via else '',
                    'tail-calls (j)' if kind.startswith('only') else 'tail-calls (j) there and calls (jal) elsewhere'))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(counts, f, indent=1, sort_keys=True)
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    rises = []
    for game, c in counts.items():
        for fold, n in c.items():
            b = base.get(game, {}).get(fold, 0)
            if n > b:
                rises.append('%s %s %d (baseline %d)' % (game, fold, n, b))
    if a.rebaseline:
        if rises and os.path.exists(BASELINE):    # (the first baseline is the tree's counts)
            print('readable_tailcalls: not rebaselined, counts above the baseline: ' + '; '.join(rises))
            return 1
        if missing:
            print('readable_tailcalls: not rebaselined, games not counted: ' + ','.join(missing))
            return 1
        with open(BASELINE, 'w') as f:
            json.dump(counts, f, indent=1, sort_keys=True)
            f.write('\n')
        print('readable_tailcalls: baseline written (%d sites)' % total)
        return 0
    print('readable_tailcalls: %d plain calls of what the original tail-calls (%s)%s' % (
        total, ', '.join('%s %d' % (g, sum(c.values())) for g, c in counts.items()),
        '; rose above the baseline: ' + '; '.join(rises) if rises else ''))
    return 1 if a.check and rises else 0


if __name__ == '__main__':
    sys.exit(main())
