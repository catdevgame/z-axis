#!/usr/bin/env python3
"""readable_layering.py: how far the readable game is from one engine with three games on it, by kind and folder.

The Z-Axis Collection is one engine and three games (the user, 11-05: "Don't duplicate engine
code"). zge holds the engine once, whichever games use a function: a game that lacks one does not call it, and a
difference goes inside the one body (perGame, a struct's fields side by side in its header, if constexpr for a
step one game skips; no #if in a .cpp). A game folder holds its game's code; game code alone may have a second
copy (BMX XXX's park editor next to AI's). Whether a function is engine or game code is a question of evidence,
not of how many games have it: the originals were linked as the engine library (../ngengine/ZGE2..4) and then
the game's own sources (./src/com, ../GameTools), so where a function sits in each game's ELF, and where its
counterparts sit in the others', says which it is (readable_layering_games.tsv). What is counted, in every
folder but ee/ (the dispatch, which keys its tables by game):

  pp_game         #if / #ifdef / #elif groups whose condition names a game (ZX_GAME_DM2, ZX_GAME_BMX,
                  ZX_GAME_INSTANCE): one source that is two sources
  pp_lines        code lines such a group leaves out of at least one game (ee/game_id.h's games), in a .cpp
  layout_lines    the same in a header: mostly a struct's shape in one game
  one_game_lines  of pp_lines and layout_lines, the lines only one game compiles: its code in a shared file
  is_game         kIsDm2, kIsBmx, kInstanceGame, Game::X, games::x: a branch on the game in a body
  per_game        perGame(...) as a value in a body (a constant, a count, a flag bit)
  per_game_layout perGame(...) in a struct's shape: a static_assert, an offsetof/sizeof, an array bound
  per_game_word   READABLE_PARAM_ONLY/_EXCEPT(game, ...), READABLE_RESULT_* and READABLE_RETURN_*: a parameter one
                  version takes (or all but one) and the word a call passes for it, a result one version has
                  (ee/readable_waits.h's per-version signatures), each place written
  reg_in          READABLE_*_IN(game, ...) outside that game's folder: another game's function registered here
  own_body        READABLE_*_OWN(fn, status, game, own[, game2, own2]): a version's own body under a shared symbol, one
                  per own body (ee/readable_waits.h)
  up_ref          (zge) functions registered in aggressive/ only that a zge file names (a call, a symbol), once
                  per file and function: the engine reaching up into AI's folder. A name is the function C++'s
                  lookup finds for it (its qualifier, the namespaces it is written in, namespace aliases, using-
                  declarations and directives, a function declared nearer), so host/movies.h's playAgain called
                  as movies::playAgain() is not aggressive::playAgain; sym::x is AI's symbol x. What the engine
                  calls in the game is declared in zge/game.h and each game defines it (engineMain's
                  gameStartup): not counted. A field or variable of the same name (`uint32_t alwaysZero;`,
                  offsetof(T, alwaysZero)) is not a reference
  game_in_zge     (zge) functions the evidence calls game code: AI's game block, and no counterpart in another
                  game's engine block. They go to game/aggressive
  game_lines      their bodies' lines
  split_in_zge    (zge) functions whose evidence disagrees (AI's engine block, a counterpart in a game block, or
                  the other way round), not yet judged: readable_layering_judged.txt takes the verdict
  split_lines     their bodies' lines
  engine_in_game  (a game folder) functions the evidence calls engine code: in aggressive/, AI's engine block
                  and no counterpart in a game block; in dm2/ or bmx/, a function of that game's own (sym::dm2::x)
                  in its ELF's engine block. They go down into zge
  engine_lines    their bodies' lines
  split_in_game   (aggressive/) functions whose evidence disagrees, not yet judged
  split_game_lines their bodies' lines

The collection (the third layer: collection/, what spans the games: the app and its game picker, the shared
settings, the bottom screen, mods) is counted over the readable folders and the runtime's (ps2/, engine/'s
overrides and game_module, collection/) by the headers each file includes (a "..." include resolved against the file's folder and the
runtime's include folders), and by the collection's namespaces a file declares or names in its code (namespace
zx_app / zx_android, zx_app::, zx_android::, zx::app::, zx::collection::: a collection function declared
by hand and called is a reach as an include is): the engine and the games never include the collection, and the
collection reaches the engine and the games only through their interfaces:

  collection_in_engine   an engine file (engine/zge, engine/host, ps2/sce, ps2/ee; engine/overrides) including a collection
                         header
  collection_in_game     a game file (games/aggressive, dm2, bmx: their code and overrides) including one
  collection_in_runtime  the runtime (ps2/runtime, kernel, gs, vu1, include) including one: the runtime knows its games itself
                         (runtime/games.h) and a front end built in installs itself (runtime/front_end.h)
  collection_reach       a collection file including an engine's or a game's header that is not an interface
                         (COLLECTION_INTERFACES: zge/native/*.h, the engine's readers made for the host; zge/game.h).
                         What the collection needs of a game goes through the runtime's contracts (the settings
                         registry, runtime/panel_snapshot.h, ps2_host) or an interface listed there

    readable_layering.py                  the table, by folder (zge/x, aggressive/x, dm2/x, sce)
    readable_layering.py --json FILE      the counts as JSON
    readable_layering.py --check          exit 1 if any kind in any folder is above the baseline
    readable_layering.py --rebaseline     write the baseline (only ever down: it refuses a rise, except the
                                          one a new game in ee/game_id.h brings, once)
    readable_layering.py --list KIND      the places of one kind (file:line: what); layout_lines by struct: each
                                          struct's lines under a `file struct: N lines` head (a line outside a
                                          struct goes with the struct its check names)
    readable_layering.py games            write readable_layering_games.tsv from the work database
                                          (WORKDB_PY, tools/paths.env): every AI function's block in AI's
                                          ELF, and its counterpart's class and block in DM2's and BMX XXX's.
                                          A refresh can move counts either way: rebaseline after it, in the
                                          same commit, with the reason.
    --root DIR                            another z-axis tree (default this checkout)

Comments and strings are left out. Folders are the first two path parts of a readable source's key (zge/render: its
path in engine/, ps2/ or games/, tools/checks/zlayout.py; the collection's rows: a file's first two folders of the
tree, collection's one, as ps2/runtime, engine/overrides, games/dm2/overrides, collection). A function is found
by its registration's symbol in tools/readable_symbols.tsv (AI's address), so a rename keeps its evidence.
readable_layering_judged.txt holds the verdicts given one by one (name, engine or game, the evidence); a
verdict there wins over the blocks. readable_layering_systems.tsv names game code kept in zge by decision (the
park editor in zge/editor): a function there counts as engine when every game of its row runs that body (AI, and a
row of each other game's symbols_status.tsv), and the names its entries header declares (what it calls in each
game) are not up_ref; a function there only one game runs is still game_in_zge.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from readable_plaincalls import strip, folder  # noqa: E402  (the same reading of the sources)

import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
GAME = os.path.normpath(ztree.TREE)   # (the tree: its readable code by keys, ztree.walk)
BASELINE = os.path.join(HERE, 'readable_layering_baseline.json')
GAMES_TABLE = os.path.join(HERE, 'readable_layering_games.tsv')
SYMBOLS = os.path.join(ztree.TOOLS, 'readable_symbols.tsv')
JUDGED = os.path.join(HERE, 'readable_layering_judged.txt')
SYSTEMS = os.path.join(HERE, 'readable_layering_systems.tsv')
KINDS = ['pp_game', 'pp_lines', 'layout_lines', 'one_game_lines', 'is_game', 'per_game', 'per_game_layout', 'per_game_word', 'reg_in',
         'own_body', 'up_ref', 'game_in_zge', 'game_lines', 'split_in_zge', 'split_lines', 'engine_in_game', 'engine_lines',
         'split_in_game', 'split_game_lines', 'collection_in_engine', 'collection_in_game', 'collection_in_runtime',
         'collection_reach']
# The engine's and the games' headers the collection may include (by key; a folder with its '/'):
# the engine's readers made for the host, and what the engine asks of the game.
COLLECTION_INTERFACES = ('zge/native/', 'game.h')
SKIP_DIRS = {'ee'}
ENGINE = ('zge/',)
GAME_FOLDERS = ('aggressive/',)  # AI's own: DM2's and BMX XXX's folders register their games' functions
GAME_ENTRIES = 'zge/game.h'  # what the engine calls in the game: declared there, each game defines it
EVIDENCE = ('identical', 'close', 'drifted', 'callsite')  # a pair whose block counts (a weak one is a lead only)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def sources(root):
    for top, dirs, files in (w for t, _ in ztree.zlayout.READABLE if t not in SKIP_DIRS
                             for w in ztree.walk(root, t)):
        for f in sorted(files):
            if f.endswith(('.cpp', '.h')):
                p = os.path.join(top, f)
                yield ztree.key_of(p, root), p


def games_of(root):
    """The games of ee/game_id.h's enum, in order ("ai", "dm2", "bmx")."""
    text = open(ztree.readable(root, 'ee', 'game_id.h')).read()
    enum = re.search(r'enum class Game\b[^{]*\{([^}]*)\}', text).group(1)
    return [m.lower() for m in re.findall(r'\b([A-Z]\w*)\s*=', enum)]


# ---------------------------------------------------------------- the preprocessor's game groups

GAME_MACRO = re.compile(r'\bZX_GAME_(?!DIR\b|SOURCES\b)\w+')


def macros_for(game):
    """The game macros a source of GAME's is compiled with (game.cmake: the instance's definitions)."""
    if game == 'ai':
        return {}
    return {'ZX_GAME_INSTANCE': game, 'ZX_GAME_' + game.upper(): '1'}


def evaluate(expr, game):
    """The value of a #if condition for GAME, or None when it names anything but game macros."""
    defined = macros_for(game)
    e = re.sub(r'\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)',
               lambda m: '1' if (m.group(1) or m.group(2)) in defined else '0', expr)
    if GAME_MACRO.search(expr) is None:
        return None

    def value(m):
        name = m.group(0)
        if name.startswith('ZX_GAME_'):
            v = defined.get(name, '0')
            return v if v.isdigit() else '1'
        raise KeyError(name)

    try:
        e = re.sub(r'\b[A-Za-z_]\w*\b', value, e)
    except KeyError:
        return None
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    try:
        return bool(eval(e, {'__builtins__': {}}))
    except Exception:
        return None


def game_lines(code, games):
    """[(line, the set of games compiling it)] of a stripped source, and the game groups' opening lines."""
    everyone = frozenset(games)
    stack = []  # [(active games, games taken by an earlier branch, whether the group names a game)]
    out, groups = [], []
    lines = code.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        n = i
        while line.endswith('\\') and i + 1 < len(lines):
            i += 1
            line = line[:-1] + lines[i]
        i += 1
        d = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', line)
        if d:
            kind, rest = d.group(1), d.group(2).strip()
            if kind in ('ifdef', 'ifndef'):
                rest = ('defined(%s)' if kind == 'ifdef' else '!defined(%s)') % rest.split()[0] if rest else '0'
                kind = 'if'
            if kind == 'if':
                vals = {g: evaluate(rest, g) for g in games}
                if all(v is not None for v in vals.values()):
                    active = frozenset(g for g in games if vals[g])
                    stack.append([active, set(active), True])
                    groups.append(n + 1)
                else:
                    stack.append([everyone, set(), False])
            elif kind == 'elif' and stack:
                top = stack[-1]
                vals = {g: evaluate(rest, g) for g in games}
                if top[2] and all(v is not None for v in vals.values()):
                    active = frozenset(g for g in games if vals[g] and g not in top[1])
                    top[0] = active
                    top[1] |= active
                elif not top[2]:
                    top[0] = everyone
            elif kind == 'else' and stack:
                top = stack[-1]
                if top[2]:
                    top[0] = everyone - frozenset(top[1])
                    top[1] = set(everyone)
            elif kind == 'endif' and stack:
                stack.pop()
            continue
        active = everyone
        for s in stack:
            active = active & s[0]
        out.append((n + 1, line, active))
    return out, groups


# ---------------------------------------------------------------- registrations and their bodies

REG = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?\s*\(([^;]*?)\)\s*;')
REG_IN = re.compile(r'\bREADABLE_(?:FUNCTION(?:_WAITS|_VU0)?|PLANNED)_IN\s*\(\s*(\w+)\s*,([^;]*?)\)\s*;')
REG_OWN = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?_OWN\s*\(([^;]*?)\)\s*;')


def registrations(code):
    """[(offset, symbol name, C++ name)] of a source's AI registrations."""
    out = []
    for m in REG.finditer(code):
        args = [a.strip() for a in m.group(1).split(',')]
        if len(args) == 2:
            out.append((m.start(), args[0].split('::')[-1], args[0]))
        elif len(args) >= 3:
            sym = args[0][5:] if args[0].startswith('sym::') else None
            if sym is None:
                continue  # an address register (READABLE_FUNCTION_AT's literal form): no symbol
            out.append((m.start(), sym, args[1]))
    return out


def body_lines(code, fn, before):
    """The lines of FN's definition nearest before offset BEFORE (0 when not found in this file)."""
    name = re.escape(fn.split('::')[-1])
    best = 0
    for m in re.finditer(r'(?<![\w.>])(?:\w+::)*%s\s*\(' % name, code[:before]):
        j, depth = m.end(), 1
        while j < len(code) and depth:
            depth += {'(': 1, ')': -1}.get(code[j], 0)
            j += 1
        t = re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>*& ]+?)?\s*\{', code[j:])
        if not t:
            continue
        k, depth = j + t.end(), 1
        while k < len(code) and depth:
            depth += {'{': 1, '}': -1}.get(code[k], 0)
            k += 1
        start = code.rfind('\n', 0, m.start()) + 1
        best = code.count('\n', start, k) + 1
    return best


def symbol_addresses(path, game=''):
    """{name: address} of a game's functions in readable_symbols.tsv (game '': AI's)."""
    out = {}
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 3 and f[0] == 'function' and (f[4] if len(f) >= 5 else '') == game:
            out[f[1]] = int(f[2], 16)
    return out


TABLE_ENGINE_ENDS = {}  # {game: where its ELF's engine library ends}, from the games table's header


def games_table(path):
    """{AI address: (name, AI's block, {game: (class, block)})} from readable_layering_games.tsv."""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8'):
        if line.startswith('#engine_end'):
            for kv in line.split()[1:]:
                build, end = kv.split('=')
                TABLE_ENGINE_ENDS[build.split('_')[0]] = int(end, 16)
        if line.startswith('#'):
            continue
        f = line.rstrip('\n').split('\t')
        out[int(f[0], 16)] = (f[1], f[2], {'dm2': (f[3], f[4]), 'bmx': (f[5], f[6])})
    return out


def verdict(row):
    """'engine', 'game', 'sdk' or 'split': AI's block and the blocks of the counterparts that count."""
    blocks = {row[1]} | {block for cls, block in row[2].values() if cls in EVIDENCE}
    return blocks.pop() if len(blocks) == 1 else 'split'


def systems(path=SYSTEMS):
    """[(folder, games, entries header)] of readable_layering_systems.tsv: game code kept in zge by decision."""
    out = []
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            if line[:1].isalpha() and len(f) >= 3:
                out.append((f[0].rstrip('/') + '/', f[1].split(','), f[2]))
    return out


def runs_in(root, game):
    """The shared functions GAME's instance runs: the names of game/<game>/symbols_status.tsv."""
    path = ztree.readable(root, game, 'symbols_status.tsv')
    if not os.path.exists(path):
        return set()
    return {line.split('\t')[0] for line in open(path, encoding='utf-8') if line[:1].isalpha() or line[:1] == '_'}


def judged(path=JUDGED):
    """{name: 'engine' or 'game'} from readable_layering_judged.txt."""
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            if line[:1].isalpha() and len(f) >= 3 and f[1] in ('engine', 'game'):
                out[f[0]] = f[1]
    return out


# ---------------------------------------------------------------- the count

# A name declared as a field or variable: a type's last token before it (a word, `>`, `*`, `&`), `;` after it.
FIELD_DECLARATION = re.compile(r'\s*(?:\[[^\]]*\]\s*)?;')
FIELD_TYPE = re.compile(r'(?:\b(?!(?:return|goto|case|else|delete|throw)\b)[A-Za-z_]\w*|[>*&])\s+\Z')
DECLARATION_NEXT = re.compile(r'\s*\(')  # (after a type: a function declared or defined)

IS_GAME = re.compile(r'\bkIs(?:Ai|Dm2|Bmx)\b|\bkInstanceGame\b|\bGame::(?:Ai|Dm2|Bmx)\b|\bgames::(?:ai|dm2|bmx)\b')
PER_GAME = re.compile(r'\bperGame\s*\(')
PER_GAME_WORD = re.compile(r'\bREADABLE_(?:PARAM|RESULT|RETURN)_(?:ONLY|EXCEPT)\s*\(\s*(?:ai|dm2|bmx)\s*,')


# ---------------------------------------------------------------- names by namespace (up_ref)
#
# A name is the function it names by C++'s lookup, not by its last word: zge/host's movies::playAgain() (an alias of
# ::game::host::movies) is host/movies.h's playAgain, not aggressive::playAgain (tools37). A source is read once, in
# order, with the scopes open at each name: its namespaces (a nested one, an anonymous one), the blocks in them, and
# what each declares for lookup there: using-declarations, using-directives, namespace aliases. A header's
# namespace-level ones count in every file (whoever includes it), so an alias a header gives is seen where it is used.

# A name with its qualifier (::a::b::c), not a member's (x.c, p->c) nor a qualifier's tail; or a brace.
NAME_TOKEN = re.compile(r'(?<![\w.])(?<!->)(?<!::)(::\s*)?(\w+(?:\s*::\s*\w+)*)|[{}]')
NAMESPACE_HEAD = re.compile(r'(?:inline\s+)?namespace\s*((?:::\s*)?\w+(?:\s*::\s*(?:inline\s+)?\w+)*)?\s*'
                            r'(?:(\{)|=\s*(::\s*)?(\w+(?:\s*::\s*\w+)*)\s*;)')
USING = re.compile(r'using\s+(namespace\s+)?(::\s*)?(\w+(?:\s*::\s*\w+)*)\s*;')


def name_parts(text):
    return tuple(p for p in re.split(r'\s*::\s*', text.strip()) if p and p != 'inline')


class Scope:
    """An open scope: a namespace (PATH its full name, () the global one; an anonymous one its parent's) or a block
    (a function's, a struct's, extern "C"'s), with what it declares for lookup."""
    __slots__ = ('path', 'is_namespace', 'usings', 'directives', 'aliases')

    def __init__(self, path, is_namespace):
        self.path, self.is_namespace = path, is_namespace
        self.usings, self.directives, self.aliases = {}, [], {}


def relative(path, target, absolute):
    """The full names a qualified TARGET written in namespace PATH may mean, the nearest first."""
    if absolute:
        return [target]
    return [path[:k] + target for k in range(len(path), -1, -1)]


def scan_names(code, names=None):
    """Walk CODE (stripped) in order: [(match, scopes)] for each name whose last part is in NAMES (all when None), with
    the scopes open there (outermost first; the list a snapshot), and the namespace-level declarations found:
    {namespace path: Scope}."""
    stack = [Scope((), True)]
    found, declared = [], {}
    pos = 0
    while True:
        m = NAME_TOKEN.search(code, pos)
        if m is None:
            break
        pos = m.end()
        text = m.group(0)
        if text == '{':
            stack.append(Scope(stack[-1].path, False))
            continue
        if text == '}':
            if len(stack) > 1:
                stack.pop()
            continue
        word = m.group(2)
        if word in ('namespace', 'inline'):
            h = NAMESPACE_HEAD.match(code, m.start())
            if h:
                pos = h.end()
                if h.group(2):      # namespace a::b { (or namespace {)
                    parts = name_parts(h.group(1) or '')
                    stack.append(Scope(stack[-1].path + parts, True))
                elif h.group(1):    # namespace a = b::c;
                    top = stack[-1]
                    top.aliases[name_parts(h.group(1))[-1]] = relative(top.path, name_parts(h.group(4)), bool(h.group(3)))
                    if top.is_namespace:
                        declared.setdefault(top.path, Scope(top.path, True)).aliases.update(
                            {name_parts(h.group(1))[-1]: top.aliases[name_parts(h.group(1))[-1]]})
            continue
        if word == 'using':
            u = USING.match(code, m.start())
            if u:
                pos = u.end()
                top = stack[-1]
                target = relative(top.path, name_parts(u.group(3)), bool(u.group(2)))
                shared = declared.setdefault(top.path, Scope(top.path, True)) if top.is_namespace else None
                for scope in [top] + ([shared] if shared else []):
                    if u.group(1):
                        scope.directives.extend(target)
                    else:
                        scope.usings.setdefault(name_parts(u.group(3))[-1], []).extend(target)
            continue
        if names is None or word.rsplit(':', 1)[-1].strip().rstrip('_') in names:
            found.append((m, list(stack)))
    return found, declared


def namespaces_at(code, offsets):
    """The namespace path open at each offset of OFFSETS in CODE (stripped)."""
    found, _ = scan_names(code)
    at = [(m.start(), stack[-1].path) for m, stack in found]
    out, i, path = [], 0, ()
    for offset in sorted(offsets):
        while i < len(at) and at[i][0] <= offset:
            path = at[i][1]
            i += 1
        out.append((offset, path))
    paths = dict(out)
    return [paths[o] for o in offsets]


def lookup(stack, shared, absolute, quals, name):
    """The full names a name may mean where STACK is open, in the order C++ looks (the nearest first): QUALS its
    qualifier's parts, ABSOLUTE for ::a::b. SHARED: the headers' namespace-level declarations by namespace."""
    if absolute:
        return [quals + (name,)]
    out = []
    for scope in reversed(stack):
        scopes = [scope] + ([shared[scope.path]] if scope.is_namespace and scope.path in shared else [])
        if quals:
            alias = next((s.aliases[quals[0]] for s in scopes if quals[0] in s.aliases), None)
            if alias is not None:
                return out + [p + quals[1:] + (name,) for p in alias]
        else:
            for s in scopes:
                out += s.usings.get(name, [])
        if scope.is_namespace:
            out.append(scope.path + quals + (name,))
        for s in scopes:
            out += [d + quals + (name,) for d in s.directives]
    return out


def function_key(parts):
    """A function's full name as the sets hold it (a C++ name's trailing _ dropped: abs_ is abs)."""
    return parts[:-1] + (parts[-1].rstrip('_'),)


def layout_use(code, at):
    """Whether the perGame call at AT sits in a struct's shape: a static_assert, offsetof/sizeof, an array bound."""
    start = max(code.rfind(';', 0, at), code.rfind('{', 0, at), code.rfind('}', 0, at)) + 1
    stmt = code[start:at]
    if re.search(r'\bstatic_assert\b|\boffsetof\b|\bsizeof\b|\balignof\b', stmt):
        return True
    return stmt.count('[') > stmt.count(']')


# ---------------------------------------------------------------- the collection layer

INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
COLLECTION_NAMESPACE = re.compile(r'\bnamespace\s+(?:zx_app|zx_android)\b|\b(?:zx_app|zx_android|zx::app|zx::collection)::')


# The host's code the collection counts run over (what runtime/src and runtime/include were before layout1), by its
# folder in the tree: (folder, layer). A game's folder counts its code and overrides (not its tools/, docs/ or
# workspace/); engine/game_module's sources are the game modules', no layer (its headers are the runtime's).
HOST_FOLDERS = (('collection', 'collection'), ('platform', 'collection'),
                ('engine/zge', 'engine'), ('engine/host', 'engine'), ('ps2/sce', 'engine'), ('ps2/ee', 'engine'),
                ('engine/overrides', 'engine'), ('games', 'game'),
                ('ps2/runtime', 'runtime'), ('ps2/kernel', 'runtime'), ('ps2/gs', 'runtime'), ('ps2/vu1', 'runtime'),
                ('ps2/include', 'runtime'), ('engine/game_module', 'runtime'))
OVERRIDE_PREFIXES = ('aggressive_inline_', 'dave_mirra_', 'bmx_xxx_', 'zge_')
# Where an include of the host's code is found (the build's include roots)
HOST_INCLUDE_ROOTS = ('ps2/include', 'ps2', 'ps2/runtime', 'ps2/kernel', 'engine', 'games', '')


def runtime_layer(rel):
    """The layer of a file by its path in the tree: collection, engine, game, runtime; None for the tools, generated
    code, a game's tools and anything else not of the four."""
    parts = rel.split('/')
    if parts[0] == 'games' and len(parts) > 2 and parts[2] in ztree.zlayout.GAME_NOT_CODE and parts[2] != 'overrides':
        return None
    if rel.startswith('engine/game_module/') and not rel.endswith('.h'):
        return None
    if '/overrides/' in rel and not os.path.basename(rel).startswith(OVERRIDE_PREFIXES):
        return None     # (as before: an override counted by its game's prefix and an underscore, dave_mirra.h not)
    for folder_, layer in HOST_FOLDERS:
        if rel.startswith(folder_ + '/'):
            return layer
    return None


def runtime_folder(rel, root):
    """The row a host file counts in: a readable source by its folder as the table has them, the rest by its first two
    folders (collection's: one)."""
    key = ztree.zlayout.readable_key(root, os.path.join(root, rel))
    if key is not None:
        return folder(key)
    parts = rel.split('/')
    if parts[0] == 'collection':
        return 'collection'
    if parts[0] == 'games' and len(parts) > 3:
        return '/'.join(parts[:3])
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def collection_counts(root, places=None):
    """The four collection kinds over the host's code (HOST_FOLDERS; root is the tree): rows by folder."""
    include_dirs = [os.path.join(root, d) for d in HOST_INCLUDE_ROOTS]
    out = {}

    def add(rel, kind, line, what):
        row = out.setdefault(runtime_folder(rel, root), {k: 0 for k in KINDS})
        row[kind] += 1
        if places is not None:
            places.setdefault(kind, []).append('%s:%d: %s' % (rel, line, what))

    files = []
    for top in sorted({f.split('/')[0] for f, _ in HOST_FOLDERS}):
        for d, dirs, names in os.walk(os.path.join(root, top)):
            dirs[:] = sorted(x for x in dirs if x != 'generated')
            for f in sorted(names):
                if f.endswith(('.cpp', '.h', '.mm')):
                    p = os.path.join(d, f)
                    files.append((os.path.relpath(p, root).replace(os.sep, '/'), p))
    for rel, p in files:
        layer = runtime_layer(rel)
        if layer is None:
            continue
        code = read(p)  # (the includes' names are quoted: not strip)
        if layer in ('engine', 'game', 'runtime'):
            # The collection's namespaces in the code (a collection function declared by hand and called).
            for n, text in enumerate(strip(code).split('\n'), 1):
                for m in COLLECTION_NAMESPACE.finditer(text):
                    add(rel, 'collection_in_' + layer, n, m.group(0))
        for m in INCLUDE.finditer(code):
            name = m.group(1)
            target = None
            for base in [os.path.dirname(p)] + include_dirs:
                candidate = os.path.normpath(os.path.join(base, name))
                if os.path.isfile(candidate):
                    target = os.path.relpath(candidate, root).replace(os.sep, '/')
                    break
            if target is None:
                continue
            target_layer = runtime_layer(target)
            line = code.count('\n', 0, m.start()) + 1
            if target_layer == 'collection' and layer in ('engine', 'game', 'runtime'):
                add(rel, 'collection_in_' + layer, line, name)
            elif layer == 'collection' and target_layer in ('engine', 'game'):
                under_game = ztree.zlayout.readable_key(root, os.path.join(root, target)) or target
                if not under_game.startswith(COLLECTION_INTERFACES):
                    add(rel, 'collection_reach', line, name)
    return out


def count(root=GAME, symbols=SYMBOLS, table_path=GAMES_TABLE, places=None):
    games = games_of(root)
    texts = {rel: strip(read(p)) for rel, p in sources(root)}
    addresses = symbol_addresses(symbols) if os.path.exists(symbols) else {}
    table = games_table(table_path)
    verdicts = judged()
    own_addresses = {}
    out = {}

    def add(rel, kind, n=1, line=None, what=''):
        if n:
            row = out.setdefault(folder(rel), {k: 0 for k in KINDS})
            row[kind] += n
            if places is not None and line is not None:
                places.setdefault(kind, []).append('%s:%d: %s' % (rel, line, what))

    # The functions registered in AI's folder by full name (and symbol), less those the engine registers and the
    # entries the engine declares for each game to define (zge/game.h's, a decided system's entries header).
    game_fns, engine_fns, game_syms, engine_syms, sym_fn = set(), set(), set(), set(), {}
    for rel, code in texts.items():
        regs = registrations(code)
        if regs and rel.startswith(GAME_FOLDERS + ENGINE + ('sce/',)):
            game = rel.startswith(GAME_FOLDERS)
            for (at, sym, fn), path in zip(regs, namespaces_at(code, [r[0] for r in regs])):
                key = function_key(relative(path, name_parts(fn), fn.strip().startswith('::'))[0])
                (game_fns if game else engine_fns).add(key)
                (game_syms if game else engine_syms).add(sym)
                if game:
                    sym_fn.setdefault(sym, key)
    decided = systems()
    entries = set()
    for header in [GAME_ENTRIES] + [s[2] for s in decided]:
        code = texts.get(header, '')
        decls = list(re.finditer(r'\b(\w+)\s*\([^;{}]*\)\s*;', code))
        for m, path in zip(decls, namespaces_at(code, [m.start(1) for m in decls])):
            entries.add(function_key(path + (m.group(1),)))
    shared_runs = {g: runs_in(root, g) for s in decided for g in s[1] if g != 'ai'}
    up_fns = game_fns - engine_fns - entries
    up_syms = game_syms - engine_syms - {e[-1] for e in entries}
    known = game_fns | engine_fns | entries
    wanted = {f[-1] for f in up_fns} | {s.rstrip('_') for s in up_syms}
    # Every source read once for those names: the headers' namespace-level usings and aliases (by namespace), and the
    # functions declared at namespace level anywhere (a nearer one of the same name hides AI's from lookup).
    shared, declared, scans = {}, set(), {}
    for rel, code in (texts.items() if wanted else ()):
        # (a member named in offsetof(Type, member) is a field, not the function of that name)
        scan = re.sub(r'\boffsetof\s*\([^()]*\)', lambda m: ' ' * len(m.group(0)), code)
        found, levels = scan_names(scan, wanted)
        if rel.startswith(ENGINE):
            scans[rel] = (scan, found)
        if rel.endswith('.h'):
            for path, scope in levels.items():
                into = shared.setdefault(path, Scope(path, True))
                into.directives += scope.directives
                for k, v in scope.usings.items():
                    into.usings.setdefault(k, []).extend(v)
                for k, v in scope.aliases.items():
                    into.aliases.setdefault(k, v)
        for m, stack in found:
            if stack[-1].is_namespace and DECLARATION_NEXT.match(scan, m.end()) and \
                    FIELD_TYPE.search(scan[max(0, m.start() - 200):m.start()]):
                declared.add(function_key(relative(stack[-1].path, name_parts(m.group(2)), bool(m.group(1)))[0]))
    known |= declared
    for rel, code in texts.items():
        if rel in scans:
            scan, found = scans[rel]
            seen = {}
            for m, stack in found:
                parts = name_parts(m.group(2))
                quals, name = parts[:-1], parts[-1]
                if quals and quals[-1] == 'sym':        # sym::x: AI's symbol x (a game's own is sym::dm2::x)
                    hit = sym_fn.get(name) if name in up_syms else None
                else:
                    hit = next((function_key(c) for c in lookup(stack, shared, bool(m.group(1)), quals, name)
                                if function_key(c) in known), None)
                    hit = hit if hit in up_fns else None
                if hit is None:
                    continue
                if FIELD_DECLARATION.match(code, m.end()) and FIELD_TYPE.search(code[max(0, m.start() - 200):m.start()]):
                    continue  # a field or variable of that name (`uint32_t alwaysZero;`), not the function
                seen.setdefault('::'.join(hit), code.count('\n', 0, m.start()) + 1)
            for name, line in sorted(seen.items()):
                add(rel, 'up_ref', 1, line, name)
        lines, groups = game_lines(code, games)
        for g in groups:
            add(rel, 'pp_game', 1, g, code.split('\n')[g - 1].strip())
        for n, text, active in lines:
            if text.strip() and len(active) < len(games):
                kind = 'layout_lines' if rel.endswith('.h') else 'pp_lines'
                add(rel, kind, 1, n, ','.join(sorted(active)) + ': ' + text.strip())
                if len(active) == 1:
                    add(rel, 'one_game_lines', 1, n, next(iter(active)) + ': ' + text.strip())
        for m in IS_GAME.finditer(code):
            add(rel, 'is_game', 1, code.count('\n', 0, m.start()) + 1, m.group(0))
        for m in PER_GAME.finditer(code):
            if re.search(r'\bconstexpr\s+T\s*$', code[:m.start()]):
                continue  # its definition
            kind = 'per_game_layout' if layout_use(code, m.start()) else 'per_game'
            add(rel, kind, 1, code.count('\n', 0, m.start()) + 1, code.split('\n')[code.count('\n', 0, m.start())].strip())
        for m in PER_GAME_WORD.finditer(code):
            add(rel, 'per_game_word', 1, code.count('\n', 0, m.start()) + 1, code.split('\n')[code.count('\n', 0, m.start())].strip())
        for m in REG_IN.finditer(code):
            if not rel.startswith(m.group(1) + '/'):
                add(rel, 'reg_in', 1, code.count('\n', 0, m.start()) + 1, ' '.join(m.group(0).split()))
        for m in REG_OWN.finditer(code):     # (one per own body: FN, STATUS, GAME, OWN[, GAME2, OWN2])
            bodies = max(1, (len([a for a in m.group(1).split(',') if a.strip()]) - 2) // 2)
            add(rel, 'own_body', bodies, code.count('\n', 0, m.start()) + 1, ' '.join(m.group(0).split()))
        # A game's own functions: READABLE_*_IN(g, sym::g::x, ...), or in its folder (its instance only) the plain
        # READABLE_FUNCTION(x, ...) / (sym::x, fn, ...), whose names are that game's rows.
        own_regs = [(m.start(), m.group(1), re.match(r'\s*sym::%s::(\w+)' % m.group(1), m.group(2))) for m in REG_IN.finditer(code)]
        own_regs = [(at, g, s.group(1)) for at, g, s in own_regs if s]
        folder_game = rel.split('/')[0]
        if folder_game in ('dm2', 'bmx'):
            own_regs += [(at, folder_game, sym) for at, sym, _ in registrations(code)]
        for at, g, name in own_regs:
            if rel.startswith(g + '/') and g in TABLE_ENGINE_ENDS:
                if g not in own_addresses:
                    own_addresses[g] = symbol_addresses(symbols, g) if os.path.exists(symbols) else {}
                address = own_addresses[g].get(name)
                if address is not None and address < TABLE_ENGINE_ENDS[g]:
                    add(rel, 'engine_in_game', 1, code.count('\n', 0, at) + 1, '%s (%s engine block)' % (name, g))
        engine = rel.startswith(ENGINE)
        own = rel.startswith(GAME_FOLDERS)
        if not (engine or own) or not table:
            continue
        for at, sym, fn in registrations(code):
            address = addresses.get(sym)
            row = table.get(address) if address is not None else None
            if row is None:
                continue
            v = verdicts.get(sym) or verdict(row)
            system = next((s for s in decided if rel.startswith(s[0])), None)
            if engine and system and all(g == 'ai' or sym in shared_runs[g] for g in system[1]):
                v = 'engine'    # (game code every game of the system runs as this one body: engine by decision)
            line = code.count('\n', 0, at) + 1
            what = '%s (ai %s; %s)' % (sym, row[1], ', '.join('%s %s %s' % (g, c, b) for g, (c, b) in row[2].items()))
            kind = {(True, 'game'): 'game_in_zge', (True, 'split'): 'split_in_zge',
                    (False, 'engine'): 'engine_in_game', (False, 'split'): 'split_in_game'}.get((engine, v))
            if kind:
                add(rel, kind, 1, line, what)
                add(rel, {'game_in_zge': 'game_lines', 'split_in_zge': 'split_lines', 'engine_in_game': 'engine_lines',
                          'split_in_game': 'split_game_lines'}[kind], body_lines(code, fn, at))
    for f, row in collection_counts(root, places).items():
        here = out.setdefault(f, {k: 0 for k in KINDS})
        for k in ('collection_in_engine', 'collection_in_game', 'collection_in_runtime', 'collection_reach'):
            here[k] += row[k]
    return {'games': games, 'folders': out}


STRUCT_HEAD = re.compile(r'^\s*(?:struct|union|class)\s+(\w+)[^;]*$')
NAMED_STRUCT = re.compile(r'\b(?:offsetof|sizeof)\s*\(\s*(\w+)')


def struct_of_lines(code):
    """{line: the struct it is in} of a stripped header: a line in a struct's braces is its outermost struct's; a
    line outside goes with the struct its offsetof/sizeof names, else with none (a type, an accessor)."""
    out, stack, pending = {}, [], None
    for n, line in enumerate(code.split('\n'), 1):
        m = STRUCT_HEAD.match(line)
        if m and not stack:
            pending = m.group(1)
        if stack:
            out[n] = stack[0][0]
        else:
            named = NAMED_STRUCT.search(line)
            out[n] = named.group(1) if named else pending
        for c in line:
            if c == '{':
                if not stack and pending:
                    stack.append([pending, 1])
                    pending = None
                elif stack:
                    stack[-1][1] += 1
            elif c == '}' and stack:
                stack[-1][1] -= 1
                if stack[-1][1] == 0:
                    stack.pop()
    return out


def show_by_struct(root, rows):
    """--list layout_lines: the places grouped by file and struct, in file order."""
    groups, structs = {}, {}
    for p in rows:
        rel, line, what = p.split(':', 2)
        if rel not in structs:
            structs[rel] = struct_of_lines(strip(open(ztree.readable(root, rel) or os.path.join(root, rel), encoding='utf-8',
                                                  errors='replace').read()))
        key = (rel, structs[rel].get(int(line)) or '(outside a struct)')
        groups.setdefault(key, []).append(p)
    for (rel, struct), lines in groups.items():
        print('%s %s: %d lines' % (rel, struct, len(lines)))
        for p in lines:
            print('  ' + p)


def totals(folders):
    t = {k: 0 for k in KINDS}
    for row in folders.values():
        for k in KINDS:
            t[k] += row.get(k, 0)
    return t


def show(result):
    folders = result['folders']
    heads = ['pp_game', 'pp_lines', 'layout', 'one_game', 'is_game', 'per_game', 'pg_layout', 'pg_word', 'reg_in', 'own', 'up_ref',
             'game', 'lines', 'split', 'lines', 'engine', 'lines', 'split_g', 'lines', 'c_engine', 'c_game', 'c_runtime', 'c_reach']
    width = max([len(f) for f in folders] + [6])
    print('games: %s' % ' '.join(result['games']))
    print('%-*s %s' % (width, 'folder', ' '.join('%9s' % h for h in heads)))
    for f in sorted(folders):
        if any(folders[f].values()):
            print('%-*s %s' % (width, f, ' '.join('%9d' % folders[f][k] for k in KINDS)))
    t = totals(folders)
    print('%-*s %s' % (width, 'total', ' '.join('%9d' % t[k] for k in KINDS)))


def rises(folders, base):
    out = []
    for f, row in folders.items():
        for k in KINDS:
            was = base.get(f, {}).get(k, 0)
            if row[k] > was:
                out.append('%s %s: %d, baseline %d' % (f, k, row[k], was))
    return out


# Where each ELF's engine library ends and the game's own sources begin (the work database's file anchors:
# the source paths in each ELF's strings, ../ngengine/ZGE* below, ./src/com and ../GameTools above). AI: the
# last ngengine anchor is zgui.c's in zguiCommands, the first function after it gameStartup (main's, in
# aggressive/game_frame.cpp); DM2: zgui.c's anchor, then src/com/action.c's at the next function but one;
# BMX XXX: zgui.c's, then src/com/bmx.c's right after gameStartup's counterpart. The SDK's functions are the
# ones each build's functions table calls sdk (SCE-scanned names, crt0, the libraries).
ENGINE_ENDS = {'ai_ps2': 0x185D20, 'dm2_ps2': 0x163280, 'bmx_ps2': 0x190340}  # not an address: tool data


def cmd_games(out_path):
    """readable_layering_games.tsv from the work database."""
    import zaxis_paths
    workdb = zaxis_paths.get('WORKDB_PY')

    def query(sql):
        out = subprocess.run([sys.executable, '-I', workdb, 'query', sql, '--csv'], capture_output=True, text=True,
                             check=True).stdout.strip().split('\n')[1:]
        return [r.split(',') for r in out if r]

    sdk = {(b, int(a)) for b, a in query("select build, addr from functions where layer = 'sdk' "
                                          "and build in ('ai_ps2', 'dm2_ps2', 'bmx_ps2')")}

    def block(build, address):
        if address is None:
            return '-'
        if (build, address) in sdk:
            return 'sdk'
        return 'engine' if address < ENGINE_ENDS[build] else 'game'

    names = {int(a, 16): n for a, n in query("select address, name from card")}
    pairs = {}
    for ai, build, addr, cls in query("select ai_addr, build, addr, cls from counterparts "
                                      "where build in ('dm2_ps2', 'bmx_ps2')"):
        pairs[(int(ai), build)] = (cls, int(addr))
    # A function the matcher paired with nothing that a caller identical in that game calls at the same
    # offset there is in that game all the same: class 'callsite', at the address called.
    lifted = 0
    for ai, build, addr in query("select distinct a.callee, p.build, g.callee from calls a "
                                 "join counterparts p on p.ai_addr = a.caller and p.cls = 'identical' "
                                 "and p.build in ('dm2_ps2', 'bmx_ps2') "
                                 "join calls g on g.build = p.build and g.caller = p.addr and g.call_offset = a.call_offset "
                                 "where a.build = 'ai_ps2'"):
        if (int(ai), build) not in pairs:
            pairs[(int(ai), build)] = ('callsite', int(addr))
            lifted += 1
    with open(out_path, 'w') as f:
        f.write('# readable_layering.py games, from the work database: every AI function, the block of its ELF it '
                'sits in (engine: the ngengine library; game: ./src/com and ../GameTools; sdk), and its counterpart '
                'in DM2 and BMX XXX: class (identical, close, drifted, weak, none: docs/MATCH3.md; callsite: '
                'unpaired, but a caller identical there calls it) and the block it sits in there. Names, classes and '
                'blocks only.\n')
        f.write('#engine_end\t%s\n' % '\t'.join('%s=0x%06X' % kv for kv in sorted(ENGINE_ENDS.items())))
        f.write('#address\tname\tai\tdm2\tdm2_block\tbmx\tbmx_block\n')
        rows = 0
        for ai in sorted(names):
            ai_block = block('ai_ps2', ai)
            cols = []
            for build in ('dm2_ps2', 'bmx_ps2'):
                cls, addr = pairs.get((ai, build), ('none', None))
                cols += [cls, block(build, addr)]
            f.write('0x%08X\t%s\t%s\t%s\n' % (ai, names[ai], ai_block, '\t'.join(cols)))
            rows += 1
    print('readable_layering: %d functions written to %s (%d classes from call sites)' % (rows, out_path, lifted))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', choices=['games'])
    ap.add_argument('--root', default=GAME, help='the z-axis tree to count')
    ap.add_argument('--symbols', default=None, help='readable_symbols.tsv (default: the root tree\'s tools/readable)')
    ap.add_argument('--games-table', default=GAMES_TABLE)
    ap.add_argument('--json', metavar='FILE')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--rebaseline', action='store_true')
    ap.add_argument('--baseline', default=BASELINE)
    ap.add_argument('--list', metavar='KIND', choices=KINDS)
    a = ap.parse_args()
    if a.command == 'games':
        return cmd_games(a.games_table)
    symbols = a.symbols or os.path.join(a.root, 'tools', 'readable', 'readable_symbols.tsv')
    places = {} if a.list else None
    result = count(a.root, symbols, a.games_table, places)
    folders = result['folders']
    if a.list:
        rows = (places or {}).get(a.list, [])
        if a.list == 'layout_lines':
            show_by_struct(a.root, rows)
        else:
            for p in rows:
                print(p)
        print('readable_layering: %s %d' % (a.list, totals(folders)[a.list]))
        return 0
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'games': result['games'], 'folders': folders, 'total': totals(folders)}, f, indent=1,
                      sort_keys=True)
    base = json.load(open(a.baseline)) if os.path.exists(a.baseline) else None
    new_games = [g for g in result['games'] if base is not None and g not in base.get('games', [])]
    if a.check:
        if base is None:
            print('readable_layering: no baseline (%s)' % a.baseline)
            return 2
        bad = rises(folders, base.get('folders', {}))
        t, b = totals(folders), totals(base.get('folders', {}))
        print('readable_layering: %s' % ', '.join('%s %d (%+d)' % (k, t[k], t[k] - b[k]) for k in KINDS))
        for line in bad:
            print('  rose: ' + line)
        if bad and new_games:
            print('readable_layering: a new game (%s): its own differences go in with it, once (--rebaseline)'
                  % ' '.join(new_games))
        return 1 if bad else 0
    if a.rebaseline:
        bad = rises(folders, base.get('folders', {})) if base is not None else []
        if bad and not new_games:
            for line in bad:
                print('  rose: ' + line)
            print('readable_layering: not rebaselined: counts only go down')
            return 1
        with open(a.baseline, 'w') as f:
            json.dump({'games': result['games'], 'folders': folders}, f, indent=1, sort_keys=True)
            f.write('\n')
        print('readable_layering: baseline written (%s)' % ', '.join('%s %d' % kv for kv in totals(folders).items()))
        return 0
    if not a.json:
        show(result)
    return 0


if __name__ == '__main__':
    sys.exit(main())
