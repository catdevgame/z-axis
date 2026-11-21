#!/usr/bin/env python3
"""Guest addresses as names (docs/READABLE.md "Symbols").

Readable code names its callees, globals, data and resume points; a guest
address is a row of a table, never a number in the code:

    ee::call<uint32_t>(sym::memset, dst, byte, bytes);
    ee::Global<uint32_t, sym::g_scratchBuffer> g_scratchBuffer;
    namespace here = points::stringFindAny;  ee::block(here::loop);
    READABLE_FUNCTION(memoryFill, Verified);

The tables:

  tools/readable_symbols.tsv    committed so AI builds without the work
                                database; generated from it (`generate`):
                                names and addresses only, the same kind of
                                data as readable_names.tsv. Rows of the game
                                column `dm2` (`bmx`) are addresses of that
                                game's own: in its folder (games/dm2), which is
                                compiled in its own instance only, they are
                                plain sym::name and points::fn (a source there
                                includes "sym/dm2/..."); elsewhere
                                sym::dm2::name (sym::bmx::name). AI's rows are
                                the key of the work database, AI being the
                                baseline. A function only one engine version
                                has, in a shared file, is registered with
                                READABLE_FUNCTION_ONLY(game, fn, STATUS)
                                (ee/readable_waits.h): its symbol is that
                                game's row, its body is inline, and only that
                                game's instance binds it.
  tools/bind/<game>.tsv         generated from the work database and the two
                                ELFs (`bind --write`), and committed: where
                                the game has each of AI's functions and data,
                                so a commit builds with its own table on every
                                host. `bind --check` (lanecheck) says it is
                                what the database makes; with it
                                <game>.layout.tsv (`layouts`).
  <build>/symbols/<game>/sym/<source>.h
                                generated at build time (`headers`): the
                                constants one source file names, at that
                                game's addresses, written only when their
                                text changes, so a new symbol recompiles the
                                files that name it. Compile-time: no run-time
                                cost, AI's objects stay byte for byte.

Commands:

  generate [--workdb DB] [--drafts DIR] [--check]
        (with duckdb) AI's rows from the work database: its registrations,
        readable_names.tsv's rows and the sources' constants and globals name
        functions and data; a function's resume points are named from its
        machine code. Only points and strings that a source uses get a row.
  bind [--check | --write] [--game dm2 ...] [--workdb DB]
        (duckdb: run again with the scratch venv's python when this one has
        none) each game's table and layouts made from the work database, the
        ELFs and this tree, against the committed tools/bind/: --check exits 1
        when one differs (stale), --write writes the ones that differ.
  table --game dm2 [--workdb DB] --elf-ai ELF --elf-game ELF --out FILE
        (with duckdb: the scratch venv's python) instruction by instruction
        over the hash-identical pairs: which of them really are, and every
        callee and data address the pair proves.
  layouts --game dm2 --bind FILE --elf-game ELF --out tools/bind/dm2.layout.tsv
        the struct fields a function with a body there uses, at the offsets
        the game's machine code uses: static_asserts in that game's build.
  headers --out DIR --game ai|dm2 [--bind FILE] [--levels FILE]
  convert FILE...   literals to names (idempotent); what has no row is listed
  lint [--drafts DIR]  any address literal in code fails, and so does a FUN_
                    reference in a comment (reported apart)
  census [--json]   what is left, by kind and layer
  view FILE         the file as the old tools read it (literal_view)
  add KIND NAME ADDRESS [FUNCTION] [--game dm2]  a new symbol (what hand-written
                    code uses to name a new callee or resume point): one line
                    of the table written, the rest left where they are
  rename OLD NEW    a symbol's new name, in the table and the sources

Tools that scan the sources for addresses read them through literal_view(),
which writes the names back as the literals they stand for, so their regular
expressions, the digests and the marks' check see what they always saw.
"""
import argparse
import bisect
import collections
import difflib
import itertools
import json
import os
import re
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)

HERE = os.path.dirname(os.path.abspath(__file__))
# The tree these tools are in: its readable code is in engine/, ps2/ and games/, each source known by its key
# (zge/render/draw.cpp: zaxis_paths.key_of, zaxis_paths.readable)
TREE = zaxis_paths.TREE
key_of, readable = zaxis_paths.key_of, zaxis_paths.readable
TABLE = os.environ.get('READABLE_SYMBOLS_TABLE') or os.path.join(HERE, 'readable_symbols.tsv')
UNBOUND = 0xDEAD0000  # a symbol this game has no row for: no registered function may use it
LAYERS = ('ee', 'zge', 'aggressive', 'sce', 'dm2', 'bmx')
GAME_FOLDERS = ('dm2', 'bmx')   # a game's own folder: its code, in its instance only; plain names are its own rows
_FOLDER_INCLUDE = re.compile(r'^#include "sym/(%s)/' % '|'.join(GAME_FOLDERS), re.M)


def folder_game(text):
    """The game whose folder a source is in, by its own symbol header (#include "sym/dm2/..."), or None."""
    m = _FOLDER_INCLUDE.search(text)
    return m.group(1) if m else None


def folder_of(rel):
    """The game whose folder a path under src/game is in (dm2/rider/x.cpp: dm2), or None."""
    head = rel.replace(os.sep, '/').split('/')[0]
    return head if head in GAME_FOLDERS else None
EXTRA_ROWS = 'symbols-extra.tsv'  # a build's drafts' rows the committed table has not got yet
BOUND = 'bound.tsv'  # game/<game>/: AI's symbols a game has that no pair of its table names (Bind.read_bound)
IMAGE_LOW, IMAGE_HIGH = 0x00100000, 0x00320000  # the three ELFs' images, .bss included

# ---- the table --------------------------------------------------------------


class Table:
    """kind, name, address, function (a point's), game: every symbol. A row's game is empty for
    AI's; `dm2` for an address of DM2's own, seen by games/dm2's sources before AI's rows."""

    def __init__(self, path=TABLE):
        self.path = path
        self.syms = {}     # name -> (kind, address)
        self.points = {}   # (function, name) -> address
        self.by_address = {}   # (kind, address) -> name; points: (function, address) -> name
        self.games = {}    # game -> Table of that game's own rows
        self.game = ''
        if not path:
            return
        for p in [path] + ([os.environ['READABLE_SYMBOLS_EXTRA']] if os.environ.get('READABLE_SYMBOLS_EXTRA') else []):
            if not os.path.exists(p):
                continue
            for line in open(p, encoding='utf-8'):
                if not line.strip() or line.startswith('#'):
                    continue
                kind, name, address, function, game = (line.rstrip('\n').split('\t') + ['', ''])[:5]
                self.own(game).put(kind, name, int(address, 16), function)

    def own(self, game):
        if not game or game == 'ai':
            return self
        if game not in self.games:
            self.games[game] = Table(None)
            self.games[game].game = game
        return self.games[game]

    def put(self, kind, name, address, function=''):
        if kind == 'point':
            self.points[(function, name)] = address
            self.by_address[(function, address)] = name
        else:
            self.syms[name] = (kind, address)
            self.by_address.setdefault((kind, address), name)

    def rows(self):
        for table in [self] + [self.games[g] for g in sorted(self.games)]:
            for name, (kind, address) in table.syms.items():
                yield (kind, name, '0x%08X' % address, '', table.game)
            for (function, name), address in table.points.items():
                yield ('point', name, '0x%08X' % address, function, table.game)

    def save(self, path=None):
        rows = sorted(self.rows(), key=lambda r: (r[4], r[0] == 'point', r[3], r[2], r[1]))
        with open(path or self.path, 'w', encoding='utf-8') as f:
            f.write(HEADER)
            for r in rows:
                f.write('\t'.join(r) + '\n')


HEADER = ("# readable_symbols.py generate: kind, name, address, function (a point's), game (empty: AI). Names and "
          "addresses only; made from the work database, committed so AI builds without it.\n")
_table = None


def table():
    global _table
    if _table is None:
        _table = Table()
    return _table


# ---- what the old tools read -------------------------------------------------

_REG2 = re.compile(r'\bREADABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION)\(\s*(\w+)\s*,\s*(\w+)\s*\)')
_REG_AT = re.compile(r'\bREADABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)\(\s*sym::(\w+)\s*,')
_REG_IN = re.compile(r'\b(\w*READABLE_(?:FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)_IN)\(\s*(\w+)\s*,\s*sym::(?:(?:dm2|bmx)::)?(\w+)\s*,')
# A function only one engine version has, in a shared file (ee/readable_waits.h): (form, game, fn[, a status word
# read by nothing: the form before tools19]). Its symbol is the game's own row, its status the game's
# symbols_status.tsv row (only_status); literal_view gives it as that game's READABLE_FUNCTION_IN, as a game folder's.
_REG_ONLY = re.compile(r'\bREADABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION)_ONLY\(\s*(dm2|bmx)\s*,\s*(\w+)\s*(?:,\s*(\w+)\s*)?\)')
_only_statuses = {}
# A shared registration one game's instance does not make (ee/readable_waits.h): READABLE_EXCEPT(GAME, REGISTRATION),
# REGISTRATION any plain or _OWN form. That game's version is far: its own body, in game/GAME, registers at the address alone.
_REG_EXCEPT = re.compile(r'\bREADABLE_EXCEPT\(\s*(ai|dm2|bmx)\s*,')


def excepted_spans(code):
    """[(start, end, game)] of the READABLE_EXCEPT wrappers in code: the registrations between start and end are
    not made in game's instance."""
    out = []
    for m in _REG_EXCEPT.finditer(code):
        depth, k = 0, code.index('(', m.start())
        while k < len(code):
            depth += (code[k] == '(') - (code[k] == ')')
            if depth == 0:
                break
            k += 1
        out.append((m.start(), k + 1, m.group(1)))
    return out


def unwrap_except(text):
    """The text with each READABLE_EXCEPT wrapper blanked (its head and its closing parenthesis, lengths and lines
    kept): the registration inside as every build but that game's has it, for the tools that read AI's view."""
    spans = excepted_spans(text)
    if not spans:
        return text
    chars = list(text)
    for start, end, _ in spans:
        head = _REG_EXCEPT.match(text, start)
        for i in range(start, head.end()):
            chars[i] = ' '
        if end - 1 < len(chars) and chars[end - 1] == ')':
            chars[end - 1] = ' '
    return ''.join(chars)


def only_status(game, name):
    """The status of a function of GAME's instance: its row of game/GAME/symbols_status.tsv, Draft without one (what
    headers writes for sym::status::GAME::NAME)."""
    if game not in _only_statuses:
        path = readable(TREE, game, 'symbols_status.tsv')
        rows = {}
        if os.path.exists(path):
            for line in open(path, encoding='utf-8'):
                p = line.split()
                if len(p) >= 2 and not line.startswith('#'):
                    rows[p[0]] = p[1]
        _only_statuses[game] = rows
    return _only_statuses[game].get(name, 'Draft')
_SYM = re.compile(r'\bsym::(?!dm2::|bmx::|level::|status::)(\w+)')
_SYM_OWN = re.compile(r'\bsym::(dm2|bmx)::(\w+)')    # a game's own row named outside its folder
_POINT = re.compile(r'\bpoints::(?!dm2::)(\w+)::(\w+)')
_POINT_DM2 = re.compile(r'\bpoints::dm2::(\w+)::(\w+)')
_ALIAS = re.compile(r'^[ \t]*namespace here = points::(dm2::)?(\w+);[ \t]*\n', re.M)
_ALIAS_INLINE = re.compile(r' namespace here = points::(dm2::)?(\w+);')
_HERE = re.compile(r'\bhere::(\w+)')
_INCLUDE = re.compile(r'^#include "sym/[^"]*"[ \t]*\n', re.M)


# The bind tables literal_view reads a game folder's registrations of AI's names with (<dir>/<game>.tsv, with
# game/<game>/bound.tsv): None is this tree's own (BIND), '' none; readable_digest.py --bind-dir sets the build's
# (ZX_BIND_DIR, which game.cmake makes this tree's).
BIND = os.path.join(HERE, 'bind')
BIND_DIR = None
_binds = {}


def game_bind(game):
    """GAME's bind table (Bind), or None where there is none."""
    if game not in _binds:
        folder = BIND_DIR if BIND_DIR is not None else BIND
        path = os.path.join(folder, game + '.tsv') if folder else None
        _binds[game] = Bind(path) if path and os.path.exists(path) else None
    return _binds[game]


def literal_view(text):
    """The source as it read with literals: for tools that scan for addresses. Line numbers kept."""
    if 'sym::' not in text and 'here::' not in text and 'points::' not in text and 'READABLE_' not in text:
        return text
    text = unwrap_except(text)
    t = table()
    dm2 = t.own('dm2')
    # A game folder's source: its plain names are its own rows, its registrations that game's (_IN).
    game = folder_game(text)
    own = t.own(game) if game else t

    def plain_sym(name):
        return own.syms.get(name) or (t.syms.get(name) if own is not t else None)

    def reg_address(name):
        """The address a registration of name is the build's at: in a game folder, the game's own row, else AI's
        row where the game's bind table puts it, as its instance registers it (cmd_headers), else none (no bind
        table, or a name the game has not got: the registration is left as written); elsewhere AI's row. (A game
        folder's registration of an AI name was written at AI's address: 34 of BMX XXX's, its park editor's among
        them, sat at AI's addresses in the work database, tools32.)"""
        if not game:
            entry = t.syms.get(name)
            return entry[1] if entry else None
        if name in own.syms:
            return own.syms[name][1]
        bind = game_bind(game)
        return resolve(bind, t, name) if bind else None

    def reg(m):
        address = reg_address(m.group(2))
        if address is not None and game:
            return 'READABLE_%s_IN(%s, 0x%08X, %s, %s)' % (m.group(1), game, address, m.group(2), m.group(3))
        return 'READABLE_%s(0x%08X, %s, %s)' % (m.group(1), address, m.group(2), m.group(3)) if address is not None else m.group(0)

    def reg_only(m):
        entry = t.own(m.group(2)).syms.get(m.group(3))
        return 'READABLE_%s_IN(%s, 0x%08X, %s, %s)' % (m.group(1), m.group(2), entry[1], m.group(3),
                                                    only_status(m.group(2), m.group(3))) if entry else m.group(0)

    text = _REG_ONLY.sub(reg_only, text)
    text = _REG2.sub(reg, text)

    def reg_own(m):     # (a version's own body: AI's build registers the shared function as the plain form)
        entry = plain_sym(m.group(2))
        return 'READABLE_%s(0x%08X, %s, %s)' % (m.group(1), entry[1], m.group(2), m.group(3)) if entry else m.group(0)

    text = _REG_OWN.sub(reg_own, text)

    def reg_at(m):
        address = reg_address(m.group(2))
        if address is not None and game:
            return 'READABLE_%s_IN(%s, 0x%08X,' % (m.group(1), game, address)
        return 'READABLE_%s(0x%08X,' % (m.group(1), address) if address is not None else m.group(0)

    text = _REG_AT.sub(reg_at, text)

    def reg_in(m):
        named = re.search(r'sym::(dm2|bmx)::', m.group(0))
        entry = (t.own(named.group(1)) if named else t).syms.get(m.group(3))
        return '%s(%s, 0x%08X,' % (m.group(1), m.group(2), entry[1]) if entry else m.group(0)

    text = _REG_IN.sub(reg_in, text)
    text = _INCLUDE.sub('\n', text)

    def point(m):
        a = own.points.get((m.group(1), m.group(2)))
        return '0x%08X' % a if a is not None else m.group(0)

    def point_dm2(m):
        a = dm2.points.get((m.group(1), m.group(2)))
        return '0x%08X' % a if a is not None else m.group(0)

    text = _POINT.sub(point, text)
    text = _POINT_DM2.sub(point_dm2, text)
    # here:: is the alias before it.
    out = []
    pos = 0
    current = None
    events = sorted([(m.start(), m.end(), 'alias', m) for m in _ALIAS.finditer(text)] +
                    [(m.start(), m.end(), 'inline', m) for m in _ALIAS_INLINE.finditer(text)] +
                    [(m.start(), m.end(), 'here', m) for m in _HERE.finditer(text)], key=lambda e: e[:3])
    points = own.points
    for start, end, what, m in events:
        if start < pos:
            continue
        out.append(text[pos:start])
        if what == 'alias':
            current, points = m.group(2), (dm2 if m.group(1) else own).points
            out.append('\n')
        elif what == 'inline':
            current, points = m.group(2), (dm2 if m.group(1) else own).points
        else:
            a = points.get((current, m.group(1)))
            out.append('0x%08X' % a if a is not None else text[start:end])
        pos = end
    out.append(text[pos:])
    text = ''.join(out)

    def sym(m):
        entry = plain_sym(m.group(1))
        return '0x%08Xu' % entry[1] if entry else m.group(0)

    def sym_own(m):
        entry = t.own(m.group(1)).syms.get(m.group(2))
        return '0x%08Xu' % entry[1] if entry else m.group(0)

    return folded(_SYM_OWN.sub(sym_own, _SYM.sub(sym, text)))


# A per-version word (ee/readable_waits.h): READABLE_PARAM_ONLY(GAME, TYPE NAME = VALUE) or (GAME, EXPRESSION) at a
# call, _EXCEPT: one version's word, or every version's but one's; READABLE_RESULT_* a return type, READABLE_RETURN_*
# a returned value.
_VERSION_FORM = re.compile(r'\bREADABLE_(PARAM|RESULT|RETURN)_(ONLY|EXCEPT)\s*\(')
VERSION_GAMES = ('ai', 'dm2', 'bmx')


def version_view(text, game=None, keep_length=False):
    """The text as GAME's instance compiles its per-version parameters and arguments (None: AI's build): a form of
    this game's is `, WORD` (a parameter without the value its definition gives the other versions), another's is no
    text; a result form is its type or void, a return form its value or
    static_cast<void>(value). keep_length: each form blanked to its own length instead (newlines kept), its word where
    it is, so offsets and lines stay those of the text (void in a form's room)."""
    if not _VERSION_FORM.search(text):
        return text
    game = game or 'ai'
    out, at = [], 0
    for m in _VERSION_FORM.finditer(text):
        if m.start() < at:
            continue
        depth, k = 0, m.end() - 1
        while k < len(text):
            if text[k] == '(':
                depth += 1
            elif text[k] == ')':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        inner = text[m.end():k]
        comma = inner.find(',')
        named = inner[:comma].strip() if comma >= 0 else ''
        if named not in VERSION_GAMES:
            continue    # (not the form: left as written)
        word = inner[comma + 1:]
        what, kind = m.group(1), m.group(2)
        if what == 'PARAM':
            depth2, cut = 0, len(word)
            for i, c in enumerate(word):
                if c in '(<[{':
                    depth2 += 1
                elif c in ')>]}':
                    depth2 -= 1
                elif c == '=' and depth2 == 0 and word[i + 1:i + 2] != '=' and word[i - 1:i] not in '=!<>':
                    cut = i
                    break
            word = word[:cut].rstrip()
        has = (named == game) == (kind == 'ONLY')
        form = text[m.start():k + 1]
        lead = m.end() + comma + 1 - m.start()
        blank = ''.join(c if c == '\n' else ' ' for c in form)
        if what in ('RESULT', 'RETURN'):
            plain = word.strip() if has else ('void' if what == 'RESULT' else 'static_cast<void>(%s)' % word.strip())
            if keep_length and has:
                blank = blank[:lead] + word + blank[lead + len(word):]
            elif keep_length:
                blank = (plain + blank[len(plain):]) if len(plain) <= len(blank) else plain
            out.append(text[at:m.start()] + (blank if keep_length else plain))
        elif keep_length:
            if has:
                # (the word where it was written, a comma before it in the form's first column)
                blank = ',' + blank[1:lead] + word + blank[lead + len(word):]
            out.append(text[at:m.start()] + blank)
        else:
            out.append(text[at:m.start()] + (', ' + word.strip() if has else ''))
        at = k + 1
    out.append(text[at:])
    return ''.join(out)


def folded(text):
    """The forms a literal was written in, back as the number: %hi, uncached, a part, a number
    that only looked like an address."""
    number = r'0x([0-9A-Fa-f]+)u?'
    text = re.sub(r'\(%s \+ %s\)' % (number, number), lambda m: '0x%08Xu' % (int(m.group(1), 16) + int(m.group(2), 16)), text)
    text = re.sub(r'ee::high\(%s\)' % number, lambda m: '0x%08Xu' % high_half(int(m.group(1), 16)), text)
    text = re.sub(r'ee::uncached\(%s\)' % number, lambda m: '0x%08Xu' % (int(m.group(1), 16) | 0x20000000), text)
    text = re.sub(r'\(([123])([uUlL]*) << 20\)', lambda m: '0x%08X%s' % (int(m.group(1)) << 20, m.group(2)), text)
    return re.sub(r'\(0x([0-9A-Fa-f]+)([uUlL]*) << 16 \| 0x([0-9A-Fa-f]+)[uUlL]*\)',
                  lambda m: '0x%08X%s' % (int(m.group(1), 16) << 16 | int(m.group(3), 16), m.group(2)), text)


# ---- ELF and instructions ----------------------------------------------------


class Elf:
    def __init__(self, path):
        self.bytes = open(path, 'rb').read()
        b = self.bytes
        self.segments = []
        phoff, = struct.unpack_from('<I', b, 28)
        phentsize, phnum = struct.unpack_from('<HH', b, 42)
        for i in range(phnum):
            ty, off, vaddr, _, filesz, memsz = struct.unpack_from('<6I', b, phoff + i * phentsize)
            if ty == 1:
                self.segments.append((vaddr, off, filesz, memsz))
        shoff, = struct.unpack_from('<I', b, 32)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 46)
        names_off = struct.unpack_from('<I', b, shoff + shstrndx * shentsize + 16)[0]
        self.sections = {}
        for i in range(shnum):
            name, ty, _, addr, off, size = struct.unpack_from('<6I', b, shoff + i * shentsize)
            n = b[names_off + name:b.index(b'\0', names_off + name)].decode()
            self.sections[n] = (addr, off, size, ty)
        reginfo = self.sections.get('.reginfo')
        self.gp = struct.unpack_from('<I', b, reginfo[1] + 20)[0] if reginfo else 0
        if not self.gp:
            self.gp = self._gp_from_start()

    def _gp_from_start(self):
        # The start-up code's `la gp, _gp` (the ELF left .reginfo's word 0).
        addr = struct.unpack_from('<I', self.bytes, 24)[0]
        hi = {}
        for i in range(32):
            w = self.word(addr + i * 4)
            op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15:
                hi[rt] = imm << 16
            elif op in (9, 13) and rt == 28 and rs in hi:
                return (hi[rs] + (imm - 0x10000 if op == 9 and imm & 0x8000 else imm)) & 0xFFFFFFFF
        return 0

    def word(self, address):
        for vaddr, off, filesz, _ in self.segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from('<I', self.bytes, off + address - vaddr)[0]
        return None

    def data(self, address, size):
        for vaddr, off, filesz, _ in self.segments:
            if vaddr <= address and address + size <= vaddr + filesz:
                return self.bytes[off + address - vaddr:off + address - vaddr + size]
        return None

    def low(self):
        return min(s[0] for s in self.segments)

    def high(self):
        return max(s[0] + s[3] for s in self.segments)


LOADS_STORES = set(range(0x20, 0x40)) | {0x1A, 0x1B, 0x1E, 0x1F}
# What a call leaves a register holding is the callee's: past a jal or jalr (and its delay slot)
# only the callee-saved registers (s0-s7, gp, sp, fp) still hold a %hi; the others' go to `spent`,
# as any other write's (another arm the linear walk reads next may still use them: a `lui a0`
# in a branch's delay slot serves both arms' calls). Without this, a `lui v0` before a call made
# the same words after it an address: findObjects' `lw v1, 0xC(v0)` on the call's result voted
# 0x2A000C, and renderCountedFrame's `move s0, v0; sb zero, 8(s0)` voted AI's
# g_upperCaseLetters (0x240008) to another BMX address, which demoted nameHash.
KEPT_OVER_CALLS = frozenset(range(16, 24)) | {28, 29, 30}


def is_call(w):
    return w >> 26 == 3 or (w >> 26 == 0 and w & 0x3F == 9)


def clobbered_by_call(hi):
    return [r for r in hi if r not in KEPT_OVER_CALLS]
BRANCHES = {4, 5, 6, 7, 20, 21, 22, 23}


def signed(imm):
    return imm - 0x10000 if imm & 0x8000 else imm


def branch_target(word, pc):
    """(target, conditional) of a branch or jump at pc that stays by offset, else None."""
    op, rt = word >> 26, (word >> 16) & 31
    if op == 2:
        return ((pc & 0xF0000000) | ((word & 0x03FFFFFF) << 2), False)
    if op in BRANCHES or (op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19)) or (op == 0x11 and ((word >> 21) & 31) == 8):
        return (pc + 4 + (signed(word & 0xFFFF) << 2), True)
    return None


def walk_pair(a, b, a_start, b_start, count, a_gp, b_gp, votes):
    """The pair's words in step. Everything but what a link changes must be equal; `votes`
    gets each (kind, a address, b address) the pair proves. False if they are not identical.
    The walk is linear, and the compiler hoists a `lui` over a branch, so a %lo can come after
    another arm's %lo took the register: what a register held stays in `spent`, for a word that
    only differs in its low half and so has to be an address."""
    a_hi, b_hi = {}, {}
    a_spent, b_spent = {}, {}
    local = []

    def forget(reg):
        if reg in a_hi and reg in b_hi:
            a_spent[reg], b_spent[reg] = a_hi[reg], b_hi[reg]
        a_hi.pop(reg, None)
        b_hi.pop(reg, None)

    call_at = None
    for i in range(count):
        if call_at is not None and i == call_at + 2:
            for reg in clobbered_by_call(a_hi):
                forget(reg)
            call_at = None
        pa, pb = a_start + i * 4, b_start + i * 4
        x, y = a.word(pa), b.word(pb)
        if x is None or y is None:
            return False
        if is_call(x):
            call_at = i
        op = x >> 26
        if op != y >> 26:
            return False
        rs, rt, rd = (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31
        if op == 3:  # jal
            local.append(('call', (x & 0x03FFFFFF) << 2, (y & 0x03FFFFFF) << 2))
            continue
        if op == 2:  # j
            ta, tb = (x & 0x03FFFFFF) << 2, (y & 0x03FFFFFF) << 2
            if a_start <= ta < a_start + count * 4:
                if ta - a_start != tb - b_start:
                    return False
            else:
                local.append(('tail', ta, tb))
            continue
        if op == 15:  # lui
            if (x ^ y) & 0xFFFF0000:
                return False
            a_hi[rt] = (x & 0xFFFF) << 16
            b_hi[rt] = (y & 0xFFFF) << 16
            continue
        address_form = op in LOADS_STORES or op in (9, 13)
        if address_form and (rs in a_hi and rs in b_hi or (x != y and rs in a_spent)):
            if (x ^ y) & 0xFFFF0000:
                return False
            high_a, high_b = (a_hi[rs], b_hi[rs]) if rs in a_hi and rs in b_hi else (a_spent[rs], b_spent[rs])
            lo_a, lo_b = (x & 0xFFFF, y & 0xFFFF) if op == 13 else (signed(x & 0xFFFF), signed(y & 0xFFFF))
            local.append(('data', (high_a + lo_a) & 0xFFFFFFFF, (high_b + lo_b) & 0xFFFFFFFF))
            if op in (9, 13) or (op in LOADS_STORES and op < 0x28):
                forget(rt)
            continue
        if address_form and rs == 28:  # gp-relative
            if (x ^ y) & 0xFFFF0000:
                return False
            local.append(('data', (a_gp + signed(x & 0xFFFF)) & 0xFFFFFFFF, (b_gp + signed(y & 0xFFFF)) & 0xFFFFFFFF))
            continue
        if x != y:
            return False
        if op == 0 and x & 0x3F in (0x21, 0x25, 0x2D) and rt == 0 and rs in a_hi and rs in b_hi:    # a move
            a_hi[rd], b_hi[rd] = a_hi[rs], b_hi[rs]
        elif op == 0:
            forget(rd)
        elif op in (8, 9, 10, 11, 12, 13, 14) or (op in LOADS_STORES and op < 0x28):
            forget(rt)
    votes.extend(local)
    return True


def side_addresses(elf, start, count, gp):
    """{word index: address} of the words that form an address on one side, by the same linear
    reading as walk_pair; the calls apart."""
    hi, spent, found, calls, keys = {}, {}, {}, {}, []
    call_at = None
    for i in range(count):
        if call_at is not None and i == call_at + 2:
            for reg in clobbered_by_call(hi):
                spent[reg] = hi.pop(reg)
            call_at = None
        w = elf.word(start + i * 4)
        if w is None:
            keys.append(None)
            continue
        if is_call(w):
            call_at = i
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        keys.append((op, w & 0x3F) if op in (0, 0x1C, 0x11, 0x12) else (op,))
        if op == 3:
            calls[i] = (w & 0x03FFFFFF) << 2
        elif op == 15:
            hi[rt] = (w & 0xFFFF) << 16
        elif (op in LOADS_STORES or op in (9, 13)) and (rs in hi or rs in spent):
            base = hi.get(rs, spent.get(rs))
            found[i] = (base + (w & 0xFFFF if op == 13 else signed(w & 0xFFFF))) & 0xFFFFFFFF
            if op in (9, 13) or op < 0x28:
                if rt in hi:
                    spent[rt] = hi.pop(rt)
        elif (op in LOADS_STORES or op in (9, 13)) and rs == 28:
            found[i] = (gp + signed(w & 0xFFFF)) & 0xFFFFFFFF
        elif op == 0 and w & 0x3F in (0x21, 0x25, 0x2D) and rt == 0 and rs in hi:
            hi[rd] = hi[rs]
        elif op == 0:
            if rd in hi:
                spent[rd] = hi.pop(rd)
        elif op in (8, 9, 10, 11, 12, 13, 14) or (op in LOADS_STORES and op < 0x28):
            if rt in hi:
                spent[rt] = hi.pop(rt)
    return keys, found, calls


def aligned_votes(a, b, row, ginsns):
    """A pair that is not word for word: what lines up. Instructions matched by kind (registers
    and numbers apart, a close pair's are often allocated otherwise) name each other's data and
    callees: weaker than a walk, so only for an address no walk named."""
    ka, fa, ca = side_addresses(a, row.ai, row.insns, a.gp)
    kb, fb, cb = side_addresses(b, row.game, ginsns, b.gp)
    votes = []
    for block in difflib.SequenceMatcher(None, ka, kb, autojunk=False).get_matching_blocks():
        if block.size < ALIGNED_RUN:
            continue
        for k in range(block.size):
            i, j = block.a + k, block.b + k
            if i in fa and j in fb:
                votes.append(('data', fa[i], fb[j]))
            if i in ca and j in cb:
                votes.append(('call', ca[i], cb[j]))
    return votes


# A run of matched instructions this long is the same code, not two prologues.
ALIGNED_RUN = 6


class BindRow:
    """A pair of the table. `votes` holds what its word-for-word walk says about callees and data."""

    def __init__(self, ai, end, game, conf, cls, method, insns):
        self.ai, self.end, self.game, self.conf, self.cls, self.method = ai, end, game, conf, cls, method
        self.insns = insns
        self.identical = False
        self.votes = []
        # A pair read in both games' machine code (workdb result kind `pair`, method `read`): the evidence of
        # record. It is sure: no vote moves it, and a pair that says otherwise is the one that is wrong.
        self.read = method == 'read' and conf == 'high'


# A word-identical pair this long is no accident. A shorter one can be any small wrapper: the
# walk ignores call targets, and data if the code around it is the same.
TRUSTED_WORDS = 12


def tally(rows):
    """What the pairs that still walk identical say: callee and data addresses, counted."""
    callees = collections.defaultdict(collections.Counter)
    data = collections.defaultdict(collections.Counter)
    for row in rows.values():
        if row.identical:
            for kind, x, y in row.votes:
                (data if kind == 'data' else callees)[x][y] += 1
    return callees, data


def settle_votes(rows, absent=frozenset()):
    """The table's one rule: a pair is what its neighbours in the call graph say it is.

    An identical pair proves the callees and data it names, but it is itself only proven by the
    words the link does not change, so two kinds of pair can be wrong, and the two show the same
    way: two pairs disagree on what an AI address is in the game.
    - A row found by position or strings, or a wrapper any other wrapper looks like, is wrong when
      identical bodies call something else: the callers name the true entry.
    - A pair that calls something else than a long identical pair (or than most callers) does not
      run the same code: it stays an entry, its body is not shared, and it proves nothing more.
    A pair read in the machine code is sure whatever its callers say, and so is a read that the game
    has no counterpart (`absent`: AI addresses): a call site that names one otherwise is the wrong pair
    (DM2's boxContainsBox, read at its own address, where one caller's walk named boxContainsPoint's
    twin; BMX XXX's nodeNameHash, read as absent, where one caller's walk named nodeName's body).
    Each demotion takes that pair's votes away, so this runs until nothing changes.
    Returns the demoted pairs with what contradicted them."""
    contradicted = []

    def demote(ai, why):
        row = rows[ai]
        row.identical = False
        contradicted.append((ai, why))

    def against(kind, ai, truth, why):
        """Demote every pair that says `ai` is something else. True if there was one."""
        found = False
        for voter in list(rows.values()):
            if voter.identical and any((k == 'data') == (kind == 'data') and x == ai and y != truth
                                       for k, x, y in voter.votes):
                demote(voter.ai, why)
                found = True
        return found

    def majority(seen):
        best, votes = seen.most_common(1)[0]
        return None if len(seen) > 1 and sorted(seen.values())[-2] == votes else best

    def one_step():
        callees, data = tally(rows)
        # Pairs that contradict something sure go first: their votes must not move a row.
        for ai, seen in sorted(data.items()):
            best = majority(seen)
            if len(seen) > 1 and best is not None and against('data', ai, best, 'data 0x%X is 0x%X' % (ai, best)):
                return True
        for ai, seen in sorted(callees.items()):
            if ai in absent and against('callee', ai, None, 'callee 0x%X has no counterpart (read)' % ai):
                return True
            row = rows.get(ai)
            if len(seen) > 1 or (row and row.game not in seen):
                sure = row and (row.read or row.identical and row.insns >= TRUSTED_WORDS or seen[row.game] >= max(seen.values()))
                if sure and against('callee', ai, row.game, 'callee 0x%X is 0x%X' % (ai, row.game)):
                    return True
        # Then the rows the callers correct.
        for ai, seen in sorted(callees.items()):
            row = rows.get(ai)
            if len(seen) > 1 or (row and row.game not in seen):
                truth = majority(seen)
                if truth is None or (row and row.read):
                    continue    # a tie: nothing says which (a read row: its voters went above)
                if row and row.game != truth:
                    row.game, row.conf, row.cls = truth, 'callsite', 'corrected'
                    row.method = 'calls:%d' % seen[truth]
                    row.identical = False
                    return True
                if against('callee', ai, truth, 'callee 0x%X is 0x%X' % (ai, truth)):
                    return True
        return False

    while one_step():
        pass
    return contradicted


def build_table(args):
    import duckdb
    db = duckdb.connect(args.workdb, read_only=True)
    a, b = Elf(args.elf_ai), Elf(args.elf_game)
    build = args.game if args.game.endswith('_ps2') else args.game + '_ps2'
    funcs = {r[0]: (r[1], r[2]) for r in db.execute(
        "SELECT addr, end_addr, insns FROM functions WHERE build='ai_ps2'").fetchall()}
    gfuncs = {r[0]: (r[1], r[2]) for r in db.execute(
        "SELECT addr, end_addr, insns FROM functions WHERE build=?", [build]).fetchall()}
    pairs = db.execute("SELECT ai_addr, addr, cls, confidence, method FROM counterparts WHERE build=? AND NOT coalesce(inlined, false)",
                       [build]).fetchall()
    # Reads that the game has no counterpart (b_addr 0): the newest read per AI function, as the
    # database's build takes them; neither a call site nor an alignment gives such a function a row.
    absent = {r[0] for r in db.execute(
        "SELECT addr, b_addr FROM (SELECT DISTINCT ON (addr) addr, b_addr FROM func_pairs "
        "WHERE build = 'ai_ps2' AND b_build = ? AND method = 'read' ORDER BY addr, ts DESC)", [build]).fetchall()
        if r[1] == 0}
    rows = {}
    demoted = 0
    for ai, g, cls, conf, method in pairs:
        if ai not in funcs or g not in gfuncs:
            continue
        end, insns = funcs[ai]
        gend, ginsns = gfuncs[g]
        row = BindRow(ai, end, g, conf, cls, method, insns)
        # (any pair: the database's classes hash the words, and a string's address is in the hash)
        if insns == ginsns and walk_pair(a, b, ai, g, insns, a.gp, b.gp, row.votes):
            row.identical = True
        elif cls == 'identical':
            demoted += 1
        rows[ai] = row
    contradicted = settle_votes(rows, absent)
    # Two globals in another order, and code that uses them in that order, walk word for word: the
    # walk pairs them by position. The close code around them says otherwise (BMX XXX's carrier and
    # attachment pools: freeCarriedAnimals frees them in the other order, and init, attach and detach
    # make and use each as AI's does). Where at least two close pairs align a datum to one address
    # and outnumber the identical pairs that name it otherwise, those pairs do not run the same
    # code: they stay entries, and prove nothing more (nor do their alignments count). Only where the
    # walk names that address as another datum: the two swapped.
    named_otherwise = set()
    while True:
        _, data = tally(rows)
        by_pair = collections.defaultdict(collections.Counter)  # AI datum -> {game address: close pairs}
        for row in rows.values():
            if not row.identical and row.ai not in named_otherwise and row.cls in ('close', 'drifted', 'identical', 'corrected') and row.game in gfuncs:
                for x, y in {(x, y) for kind, x, y in aligned_votes(a, b, row, gfuncs[row.game][1]) if kind == 'data' and x in data}:
                    by_pair[x][y] += 1
        found = []
        for x, seen in sorted(by_pair.items()):
            y, n = seen.most_common(1)[0]
            walkers = [r for r in rows.values() if r.identical and any(k == 'data' and vx == x for k, vx, _ in r.votes)]
            # (only a pair that names y as another datum: the two in another order)
            otherwise = [r for r in walkers if any(k == 'data' and vx == x and vy != y for k, vx, vy in r.votes)
                         and any(k == 'data' and vx != x and vy == y for k, vx, vy in r.votes)]
            if n >= 2 and list(seen.values()).count(n) == 1 and otherwise and n > len(walkers):
                found += otherwise
        if not found:
            break
        for row in {r.ai: r for r in found}.values():
            row.identical = False
            named_otherwise.add(row.ai)
            contradicted.append((row.ai, 'its data otherwise than the close pairs around it'))
        contradicted += settle_votes(rows, absent)
    # What the identical pairs prove.
    callees, data = tally(rows)
    extra = []
    conflicts = [('contradicted', ai, why) for ai, why in contradicted]
    for ai, seen in sorted(callees.items()):
        if len(seen) > 1:
            conflicts.append(('callee', ai, dict(seen)))
            continue
        g = next(iter(seen))
        if ai not in rows and ai not in absent and ai in funcs and g in gfuncs:
            extra.append(BindRow(ai, funcs[ai][0], g, 'callsite', 'unmatched', 'calls:%d' % seen[g], funcs[ai][1]))
    # What the close pairs say, for the addresses no walk named.
    aligned = collections.defaultdict(collections.Counter)
    close_calls = collections.defaultdict(collections.Counter)
    walked = collections.defaultdict(set)
    for ai, seen in data.items():
        for g in seen:
            walked[g].add(ai)
    for row in rows.values():
        if not row.identical and row.ai not in named_otherwise and row.cls in ('close', 'drifted', 'identical', 'corrected') and row.game in gfuncs:
            for kind, x, y in aligned_votes(a, b, row, gfuncs[row.game][1]):
                (close_calls if kind == 'call' else aligned)[x][y] += 1
    # Where the close pairs disagree on a datum, an alignment that makes it an address an identical walk names
    # as another AI datum's loses (AI's carriedAnimalCount paired with an animator function of BMX XXX's voted
    # g_animalCarrierTree to the animators' tree, so the tree had no row). Undisputed, it stands: a section's
    # first word can be two AI addresses' (the start-up code's bss start, AI's g_renderCamera, is BMX XXX's
    # g_displayStateReady).
    for x, seen in aligned.items():
        if len(seen) > 1:
            for y in [y for y in seen if y in walked and x not in walked[y]]:
                if len(seen) > 1:
                    del seen[y]
    taken = {r.game for r in rows.values()} | {r.game for r in extra}
    named = set(rows) | {r.ai for r in extra}
    corrected = 0
    for ai, seen in sorted(close_calls.items()):
        if ai in callees:
            continue    # an identical pair's walk names it: a close pair's alignment is weaker (freeMaterial)
        row = rows.get(ai)
        if row and not row.identical and row.conf in ('medium', 'low') and len(seen) == 1 and row.game not in seen:
            # (a row found by position or a guess, and close code that calls something else there)
            g = next(iter(seen))
            if g in gfuncs and g not in taken:
                row.game, row.conf, row.cls, row.method = g, 'aligned', 'corrected', 'aligned:%d' % seen[g]
                corrected += 1
                continue
        if ai not in named and ai not in absent and ai in funcs and len(seen) == 1:
            g = next(iter(seen))
            if g in gfuncs and g not in taken:
                extra.append(BindRow(ai, funcs[ai][0], g, 'aligned', 'unmatched', 'aligned:%d' % seen[g], funcs[ai][1]))
                taken.add(g)
    rows = [(('body' if r.identical else 'entry'), r.ai, r.end, r.game, r.conf, r.cls, r.method)
            for r in list(rows.values()) + extra]
    built = dict(db.execute("SELECT key, value FROM meta").fetchall())
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write('# readable_symbols.py table: %s against AI, from the work database built %s (workdb.py at %s) and the ELFs. '
                'Committed in tools/bind: `bind --check` says it is what the database makes.\n'
                % (args.game, built.get('built', '?'), built.get('workdb_commit') or '?'))
        f.write('#kind\tai\tai_end\tgame\tconfidence\tcls\tmethod\n')
        for kind, ai, end, g, conf, cls, method in sorted(rows, key=lambda r: r[1]):
            f.write('%s\t0x%08X\t0x%08X\t0x%08X\t%s\t%s\t%s\n' % (kind, ai, end, g, conf, cls, method))
        ndata = 0
        for ai, seen in sorted(data.items()):
            if not (a.low() <= ai <= a.high()):  # the last section's end is a symbol too (_end)
                continue
            g, n = seen.most_common(1)[0]
            same = ''
            x, y = a.data(ai, 4), b.data(g, 4)
            if x is not None and y is not None:
                same = 'same' if x == y else 'differs'
            f.write('data\t0x%08X\t\t0x%08X\t%s\t%s\tvotes:%d\n' % (ai, g, 'high' if len(seen) == 1 else 'conflict', same, n))
            ndata += 1
            if len(seen) > 1:
                conflicts.append(('data', ai, dict(seen)))
        for ai, seen in sorted(aligned.items()):
            if ai in data or not a.low() <= ai <= a.high() or len(seen) > 1:
                continue
            g, n = seen.most_common(1)[0]
            if a.low() <= ai < a.high() and not b.low() <= g <= b.high():
                continue
            x, y = a.data(ai, 4), b.data(g, 4)
            same = '' if x is None or y is None else ('same' if x == y else 'differs')
            f.write('data\t0x%08X\t\t0x%08X\taligned\t%s\tvotes:%d\n' % (ai, g, same, n))
            ndata += 1
    summary = collections.Counter(r[0] for r in rows)
    print('%s: %d body (identical, checked word by word), %d entry (%d from call sites only), %d demoted from identical, '
          '%d data addresses, %d conflicts' % (args.game, summary['body'], summary['entry'], len(extra), demoted, ndata, len(conflicts)))
    if args.conflicts:
        with open(args.conflicts, 'w') as f:
            for c in conflicts:
                f.write(repr(c) + '\n')


def pair_strength(row):
    """A function row of a game's table (kind, ai, ai_end, game, confidence, cls, method): its confidence's rank, a pair
    read in both games' code (method `read`) above any other of that confidence."""
    conf = {'high': 3, 'medium': 2, 'low': 1}.get(row[4] if len(row) > 4 else '', 0)
    return conf * 2 + (1 if len(row) > 6 and row[6] == 'read' else 0)


class Bind:
    """A game's table: AI's addresses to the game's."""

    def __init__(self, path=None):
        self.ai = path is None
        self.bodies = []     # sorted (ai, ai_end, game)
        self.entries = {}    # ai -> game
        self.strength = {}   # ai -> how well the table's pair of a function is proven (pair_strength)
        self.data = {}       # ai -> game (exact, no conflict)
        self.aligned_data = set()    # ai: data the table has only from a close pair's alignment (no walk)
        self.bound_points = {}   # ai -> game: bound.tsv's resume points (a close function's, read by hand)
        self.segments = []
        if self.ai:
            return
        rows = []
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            if p[0] == 'body':
                self.bodies.append((int(p[1], 16), int(p[2], 16), int(p[3], 16)))
                self.entries[int(p[1], 16)] = int(p[3], 16)
                self.strength[int(p[1], 16)] = pair_strength(p)
            elif p[0] == 'entry':
                self.entries[int(p[1], 16)] = int(p[3], 16)
                self.strength[int(p[1], 16)] = pair_strength(p)
            elif p[0] == 'data' and p[4] in ('high', 'aligned'):
                self.data[int(p[1], 16)] = int(p[3], 16)
                if p[4] == 'aligned':
                    self.aligned_data.add(int(p[1], 16))
                rows.append((int(p[1], 16), int(p[3], 16)))
        self.bodies.sort()
        self._starts = [r[0] for r in self.bodies]
        # Neighbouring proven addresses with one delta are one object.
        run = None
        for ai, g in sorted(rows):
            if run and g - ai == run[2] and ai - run[1] <= 0x100:
                run[1] = ai
            else:
                if run:
                    self.segments.append(tuple(run))
                run = [ai, ai, g - ai]
        if run:
            self.segments.append(tuple(run))
        self._seg = [s[0] for s in self.segments]
        self.read_bound(readable(TREE, os.path.splitext(os.path.basename(path))[0], BOUND))

    def read_bound(self, own):
        """game/<game>/bound.tsv: AI's symbols the game has where no pair of the table names them (a callee
        inlined into a drifted caller, a datum only close code uses, a close function's resume point), each read
        by hand from the game's own code: kind (function, data, point), AI's name (a point's as FUNCTION::NAME,
        points::FUNCTION::NAME in the sources), the game's address, the evidence. A row the table already names
        otherwise is an error: the table's walk is the stronger proof. (A datum the table has only from an alignment
        is not: the row replaces it.) A point's function must be bound (by the
        table or a function row), and the point lie after the function's start there."""
        if not os.path.exists(own):
            return
        t = table()
        rows = []
        for n, line in enumerate(open(own, encoding='utf-8'), 1):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            if len(p) < 4 or p[0] not in ('function', 'data', 'point') or not p[3].strip():
                raise SystemExit('%s:%d: kind (function, data, point), AI name, address, evidence' % (own, n))
            rows.append((n, p[0], p[1], int(p[2], 16)))
        for n, kind, name, address in sorted(rows, key=lambda r: r[1] == 'point'):    # (a point's function first)
            if kind == 'point':
                function, _, point = name.partition('::')
                ai = t.points.get((function, point))
                if ai is None:
                    raise SystemExit('%s:%d: %s is no AI point (FUNCTION::NAME, as points::FUNCTION::NAME)' % (own, n, name))
                entry = t.syms.get(function)
                start = self.function(entry[1]) if entry and entry[0] == 'function' else None
                if start is None:
                    raise SystemExit('%s:%d: %s: its function %s is not bound here' % (own, n, name, function))
                if address <= start or address & 3:
                    raise SystemExit('%s:%d: %s at 0x%08X: not a word after %s at 0x%08X' % (own, n, name, address, function, start))
                have = self.point(ai)
                if have is not None and have != address:
                    raise SystemExit('%s:%d: the table has %s at 0x%08X' % (own, n, name, have))
                self.bound_points[ai] = address
                continue
            entry = t.syms.get(name)
            if not entry or entry[0] != kind:
                raise SystemExit('%s:%d: %s is no AI %s' % (own, n, name, kind))
            have = self.function(entry[1]) if kind == 'function' else self.datum(entry[1])
            # A datum the table has only by alignment (a close pair's instructions lined up, no walk) is the weaker
            # proof: a row read in the game's code takes its place (DM2's g_textDeleteThread: endMovie's alignment
            # named the video decoder where the code loads the label). So is one it has no row for at all, only
            # its neighbours' common delta (a segment): BMX XXX's g_textOnSelect, OnStart and ShowDel sit in a run
            # of AI's zgui texts whose BMX run has words ZGE4 added between them, so the delta is wrong there.
            if kind == 'data' and (entry[1] in self.aligned_data or entry[1] not in self.data):
                have = None
            if have is not None and have != address:
                raise SystemExit('%s:%d: the table has %s at 0x%08X' % (own, n, name, have))
            (self.entries if kind == 'function' else self.data)[entry[1]] = address

    def body(self, ai):
        """(start, end, game start) of the body the AI address lies in, or None."""
        if self.ai:
            return None
        i = bisect.bisect_right(self._starts, ai) - 1
        if i >= 0 and self.bodies[i][0] <= ai < self.bodies[i][1]:
            return self.bodies[i]
        return None

    def function(self, ai):
        return ai if self.ai else self.entries.get(ai)

    def point(self, ai):
        if self.ai:
            return ai
        r = self.body(ai)
        return r[2] + ai - r[0] if r else self.bound_points.get(ai)

    def datum(self, ai):
        if self.ai:
            return ai
        if ai in self.data:
            return self.data[ai]
        i = bisect.bisect_right(self._seg, ai) - 1
        if i >= 0 and self.segments[i][0] <= ai <= self.segments[i][1]:
            return ai + self.segments[i][2]
        return None


# ---- the sources --------------------------------------------------------------


def blank(text):
    """Comments and string contents as spaces, same length."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
        elif text[i] in '"\'':
            if text[i] == "'" and i and text[i - 1].isalnum():  # 1'000
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != text[i] and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != '\n':
                out[k] = ' '
        i = j
    return ''.join(out)


def game_sources(root=TREE, layers=LAYERS):
    for layer in layers:
        for d, dirs, files in zaxis_paths.zlayout.readable_walk(root, layer):
            # (sorted: the host's directory order is no input, as NTFS's and APFS's differ)
            dirs[:] = sorted(x for x in dirs if x != 'sweep')
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    yield os.path.join(d, f)


def layer_of(path, root=TREE):
    rel = key_of(path, root)
    return rel.split(os.sep)[0]


MACHINE = re.compile(r'(?:FUN|fn|entry|LAB|thunk|sub|DAT|kFun|kFn|kData|kLab|kAddr|kAddress)_?[0-9A-Fa-f]{5,8}\b|(?:FUN|entry|LAB|thunk)_')


PUNCTUATION = {' ': 'Space', '<': 'Less', '>': 'Greater', '=': 'Equals', '!': 'Not', '$': 'Dollar', '|': 'Bar',
               '\\': 'Backslash', '.': 'Dot', ',': 'Comma', ':': 'Colon', '-': 'Minus', '+': 'Plus', '/': 'Slash',
               '%': 'Percent', '\r': 'Return', '\n': 'Newline', '\t': 'Tab', '*': 'Star', '?': 'Question', '#': 'Hash',
               '(': 'OpenParen', ')': 'CloseParen', '[': 'OpenBracket', ']': 'CloseBracket', '"': 'Quote', "'": 'Apostrophe',
               '_': 'Underscore', '&': 'Ampersand', '~': 'Tilde', '^': 'Caret', '@': 'At', ';': 'Semicolon'}


class Facts:
    """One ELF's functions and sections, for the converter and the generator."""

    def __init__(self, elf, csv, t):
        self.elf = Elf(elf) if isinstance(elf, str) else elf
        self.t = t
        self.starts, self.ends, self.names = [], {}, {}
        self._point_names, self._high_uses = {}, {}
        import csv as csvmod
        with open(csv, newline='') as f:
            rows = list(csvmod.reader(f))
        head = [h.strip().lower() for h in rows[0]]
        name_i = head.index('name')
        start_i = next(i for i, h in enumerate(head) if h in ('start', 'address', 'entry', 'start_address'))
        end_i = next((i for i, h in enumerate(head) if h in ('end', 'end_address')), None)
        size_i = next((i for i, h in enumerate(head) if h == 'size'), None)
        spans = {}
        for r in rows[1:]:
            if len(r) <= max(name_i, start_i):
                continue
            try:
                s = int(r[start_i], 16)
            except ValueError:
                continue
            if r[name_i].startswith('entry_'):  # the recompiler's entry blocks: inside a function
                continue
            e = int(r[end_i], 16) if end_i is not None else s + int(r[size_i], 0)
            spans[s] = e
            if not MACHINE.match(r[name_i]):
                self.names[s] = r[name_i]
        self.starts = sorted(spans)
        self.ends = spans

    def function_at(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and address < self.ends[self.starts[i]]:
            return self.starts[i]
        return None

    def kind(self, address):
        """'function', 'point' (inside one), 'data' or None."""
        if ('function', address) in self.t.by_address:
            return 'function'
        f = self.function_at(address)
        if f is not None:
            return 'function' if f == address else 'point'
        if ('data', address) in self.t.by_address:
            return 'data'
        s = self.elf.sections
        if '.bss' in s and address == s['.bss'][0] + s['.bss'][2]:     # _end
            return 'data'
        text = s.get('.text')
        for name in ('.data', '.rodata', '.lit4', '.sdata', '.sbss', '.bss', '.vutext', '.reginfo'):
            if name in s and s[name][0] <= address < s[name][0] + s[name][2]:
                return 'data'
        if text and text[0] <= address < text[0] + text[2]:
            return 'point'
        return None

    def text_name(self, address):
        """g_text<Words> for a C string in the ELF (readable_names.py's convention), or None."""
        data = self.elf.data(address, 64) or self.elf.data(address, 16)
        if not data or b'\0' not in data:
            return None
        word = struct.unpack_from('<I', data)[0]
        if IMAGE_LOW <= word < IMAGE_HIGH:   # a pointer that happens to print
            return None
        raw = data[:data.index(b'\0')]
        if not raw or not all(32 <= c < 127 or c in (9, 10, 13) for c in raw):
            return None
        text = raw.decode('ascii')
        if not re.search(r'[A-Za-z0-9]', text):
            names = [PUNCTUATION.get(c) for c in text]
            return 'g_text' + ''.join(names) if all(names) else None
        words = re.findall(r'[A-Za-z0-9]+', text)
        return 'g_text' + ''.join(w[0].upper() + w[1:] for w in words)[:40]

    def function_name(self, address):
        return self.t.by_address.get(('function', address)) or self.names.get(address)

    def high_uses(self, function):
        """Counter of the addresses a function builds from a `lui` and a low half."""
        if function not in self._high_uses:
            seen = collections.Counter()
            high = {}
            for pc in range(function, self.ends[function], 4):
                w = self.elf.word(pc)
                if w is None:
                    continue
                op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
                if op == 15:
                    high[rt] = (w & 0xFFFF) << 16
                elif (op in LOADS_STORES or op in (9, 13)) and rs in high:
                    low = w & 0xFFFF if op == 13 else signed(w & 0xFFFF)
                    seen[(high[rs] + low) & 0xFFFFFFFF] += 1
                    if op in (9, 13) or op < 0x28:
                        high.pop(rt, None)
                elif op in (8, 9, 10, 11, 12, 13, 14) or (op in LOADS_STORES and op < 0x28):
                    high.pop(rt, None)
                elif op == 0:
                    high.pop((w >> 11) & 31, None)
            self._high_uses[function] = seen
        return self._high_uses[function]

    def point_names(self, function):
        """{address: name} of every instruction of a function (point_names)."""
        if function not in self._point_names:
            end = self.ends[function]
            word = self.elf.word
            calls, branches = {}, {}
            for pc in range(function, end, 4):
                w = word(pc)
                if w is None:
                    continue
                if w >> 26 == 3:
                    calls[pc + 8] = (w & 0x03FFFFFF) << 2
                b = branch_target(w, pc)
                if b:
                    branches[pc] = b[0]
            self._point_names[function] = point_names(function, end, calls, branches, self.function_name)
        return self._point_names[function]


def point_names(start, end, calls, branches, callee_name):
    """Every instruction of a function, by what it is in the code: the return of a call, a loop's
    head, a join, a branch's target, what follows a branch, in address order; the nth of a kind
    has its number. A pure function of the code (and the callees' names), so a name does not
    depend on which points a source uses. `calls`: {return address: callee}; `branches`: {pc:
    target}. Role names: a person replaces them, or the marks leave the source."""
    sources = collections.Counter(branches.values())
    backward = {t for pc, t in branches.items() if t <= pc}
    used = collections.Counter()
    names = {}
    taken = set()
    for pc in range(start + 4, end + 4, 4):
        if pc in calls:
            callee = callee_name(calls[pc])
            role = 'after' + (callee[0].upper() + callee[1:] if callee else 'Call')
        elif pc in backward:
            role = 'loop'
        elif pc >= end - 12 and sources[pc]:
            role = 'done'
        elif sources[pc] > 1:
            role = 'join'
        elif sources[pc] == 1:
            role = 'taken'
        elif pc - 8 in branches:
            role = 'notTaken'
        else:
            role = 'next'
        used[role] += 1
        name = role if used[role] == 1 else '%s%d' % (role, used[role])
        while name in taken:    # (a callee's own name can end in a number)
            used[role] += 1
            name = '%s%d' % (role, used[role])
        taken.add(name)
        names[pc] = name
    return names


LITERAL = re.compile(r'\b0[xX]([0-9a-fA-F]{5,8})([uUlL]*)\b')
COMMENT_REF = re.compile(r'\b(?:FUN|DAT|LAB|fn)_(?:00)?[0-9A-Fa-f]{6}\b')
COMMENT_DATA_ONLY = re.compile(r'[ \t]*// (?:DAT|LAB)_[0-9A-Fa-f]{8}[ \t]*$', re.M)
COMMENT_HEX = re.compile(r'\b0x0*([12][0-9A-Fa-f]{5})\b')
COMMENT_ADDRESS = re.compile(r'\b(?:FUN|DAT|LAB|fn)_((?:00)?[0-9A-Fa-f]{6})\b')
MARKS = re.compile(r'(?:ee::)?(?:block|blockWhole|loopBack|loopBackWhole|afterCall|tableJump|selfCall)\([^;]*$')
GLOBAL_DECL = re.compile(r'(?:ee::)?Global<[^;]*,\s*$')
CONSTEXPR = re.compile(r'(?:\bconstexpr\s+[\w:<>\s*]+?|,\s*|^\s*)\b(k\w+)\s*=\s*$')
REG3 = re.compile(r'\bREADABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION)\(\s*0[xX]([0-9a-fA-F]+)\s*,\s*([\w:]+)\s*,\s*(\w+)\s*\)')
REG_PLANNED = re.compile(r'\bREADABLE_PLANNED\(\s*0[xX]([0-9a-fA-F]+)\s*,\s*"(\w+)"')
REG_IN3 = re.compile(r'\b(\w*READABLE_(?:FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)_IN)\(\s*(\w+)\s*,\s*0[xX]([0-9a-fA-F]+)\s*,\s*([\w:"]+)')
BITWISE_BEFORE = re.compile(r'(?:[^&|]&|[^|]\||\^|<<|>>|~)\s*$')
BITWISE_AFTER = re.compile(r'[uUlL]*\s*(?:&[^&]|\|[^|]|\^|<<|>>)')


def clash_free(t, name, kind, address):
    """name, or name with a suffix if another address has it."""
    if name not in t.syms or t.syms[name] == (kind, address):
        return name
    n = 2
    while '%s%d' % (name, n) in t.syms and t.syms['%s%d' % (name, n)] != (kind, address):
        n += 1
    return '%s%d' % (name, n)


def function_bodies(code):
    """[(start, end)] of the outermost function bodies of a source, sorted."""
    bodies = []
    stack = []
    start = 0
    for m in re.finditer(r'[{};]', code):
        c = m.group(0)
        if c == '{':
            header = code[start:m.start()]
            word = re.search(r'\b(namespace|struct|class|enum|union)\b', header)
            function = '(' in header and not word and not re.search(r'=\s*$', header.rstrip())
            stack.append((m.end(), function))
            start = m.end()
        elif c == '}':
            if stack:
                opened, function = stack.pop()
                if function and not any(f for _, f in stack):
                    bodies.append((opened, m.start()))
            start = m.end()
        else:
            start = m.end()
    return bodies


def body_of(bodies, starts, pos):
    i = bisect.bisect_right(starts, pos) - 1
    if i >= 0 and pos <= bodies[i][1]:
        return bodies[i][0]
    return None


class GeneratedCode:
    """What the facts need of an ELF, from the recompiled sources (the drafter's own reading of
    them): a build has no ELF to hand. `parsed(address)` as readable_draft.py's Workspace."""

    sections = {}

    def __init__(self, parsed, facts_of):
        self.parsed, self.facts_of = parsed, facts_of
        self.words = {}

    def word(self, address):
        if address not in self.words:
            function = self.facts_of().function_at(address)
            p = self.parsed(function) if function is not None else None
            for insn in (p.insns.values() if p else ()):
                self.words[insn.addr] = int(insn.raw, 16) if isinstance(insn.raw, str) else insn.raw
        return self.words.get(address)

    def data(self, address, size):
        return None


class DraftSymbols:
    """The sweep's generated batch files, written with symbols (readable_draft.py). A row the
    committed table has not got yet goes to the build's symbols-extra.tsv: `generate` commits it."""

    def __init__(self, parsed, csv, out_dir):
        self.t = table()
        self.committed = set(self.t.rows())
        self.path = os.path.join(out_dir, EXTRA_ROWS)
        code = GeneratedCode(parsed, lambda: self.facts)
        self.facts = Facts(code, csv, self.t)
        self.scope = Scope(self.facts, self.t)
        self.report = []

    def convert(self, text, rel, spans):
        inserted = []
        new = comment_names(convert_text(text, rel, self.scope, self.report, inserted=inserted), self.t)
        inserted.sort()

        def moved(line):       # (spans count lines from 1)
            return line + bisect.bisect_right(inserted, line - 1)

        return new, {a: (moved(first), moved(last)) for a, (first, last) in spans.items()}

    def save(self):
        rows = sorted(set(self.t.rows()) - self.committed)
        write_if_changed(self.path, ''.join('\t'.join(r) + '\n' for r in rows))
        for line in self.report:
            sys.stderr.write('readable_symbols: %s\n' % line)
        return len(rows)


class Scope:
    """The facts and table a literal of a source is read with: the game's own for games/dm2."""

    def __init__(self, facts, table):
        self.facts, self.t = facts, table


MARK_ARGUMENT = re.compile(r'\b(?:block|blockWhole|loopBack|loopBackWhole)\(|\{\s*31\s*,\s*$')


def number_form(m, value, round_number=False):
    """A number that only looks like an address, written so it does not: a generated draft
    has no one to say so. A round one that is not one of these three may be an address's high
    half: left for the lint."""
    suffix = m.group(2) or ''
    if value & 0xFFFFF == 0 and value >> 20 in (1, 2, 3):
        return '(%d%s << 20)' % (value >> 20, suffix)
    if round_number:
        return None
    return '(0x%X%s << 16 | 0x%X%s)' % (value >> 16, suffix, value & 0xFFFF, suffix)


def high_half(address):
    return (address + 0x8000) & 0xFFFF0000


def high_symbol(value, functions, code, scope):
    """The named datum a round number is the %hi of: what the source's functions build from it,
    most used first, then what the source names."""
    t, seen = scope.t, collections.Counter()
    for function in functions:
        if function in scope.facts.ends:
            for address, n in scope.facts.high_uses(function).items():
                if high_half(address) == value:
                    seen[address] += n
    for address, _ in sorted(seen.items(), key=lambda kv: (-kv[1], kv[0])):
        name = t.by_address.get(('function', address))
        if name:
            return name, address
        name = t.by_address.get(('data', address)) or scope.facts.text_name(address)
        if name:
            return clash_free(t, name, 'data', address), address
    for m in LITERAL.finditer(code):   # (what else the source names there)
        address = int(m.group(1), 16)
        name = t.by_address.get(('data', address))
        if name and address != value and high_half(address) == value:
            return name, address
    for name in sorted(set(_SYM.findall(code))):
        entry = t.syms.get(name)
        if entry and high_half(entry[1]) == value:
            return name, entry[1]
    return None, None


def literal_scope(rel, decl, address, ai, dm2):
    """AI's or DM2's: games/dm2's sources are DM2's own (its instance only); a DM2 address elsewhere is
    written sym::dm2::x."""
    if dm2 is None or not rel.startswith('dm2/'):
        return ai
    if folder_of(rel) == 'dm2':
        return dm2
    if not decl and ('data', address) in ai.t.by_address and ('data', address) not in dm2.t.by_address:
        return ai
    if decl and decl.startswith('kAi'):
        return ai
    if os.path.basename(rel).startswith('ai_') and not (decl and decl.startswith('kDm2')):
        return ai
    in_ai, in_dm2 = address in ai.facts.ends, address in dm2.facts.ends
    return ai if in_ai and not in_dm2 else dm2


def conditional_lines(text):
    """The lines (0-based) inside a preprocessor conditional; an include guard is not one."""
    held, depth = set(), 0
    lines = blank(text).split('\n')
    for n, line in enumerate(lines):
        directive = re.match(r'[ \t]*#[ \t]*(if|ifdef|ifndef|endif)\b', line)
        if directive and directive.group(1) == 'endif':
            depth -= 1
        elif directive:
            guard = re.match(r'[ \t]*#[ \t]*ifndef[ \t]+(\w+)', line)
            following = lines[n + 1] if n + 1 < len(lines) else ''
            if guard and depth == 0 and re.match(r'[ \t]*#[ \t]*define[ \t]+%s\b' % guard.group(1), following):
                continue
            depth += 1
        elif depth:
            held.add(n)
    return held


def conditional_includes(text):
    """A source's own table included under #if: every other platform has no symbols. (A layout
    header under ZX_GAME_INSTANCE is not one: it is that game's build's, on purpose.)"""
    held = conditional_lines(text)
    found = []
    for n, line in enumerate(text.split('\n')):
        if n in held and line.startswith('#include "sym/') and not line.endswith('.layout.h"'):
            found.append((n + 1, line + '\n'))
    return found


def own_table(result, rel, inserted=None):
    """The file's own table of constants: included after its last #include that every platform
    sees (one a conditional holds moves out, only that platform would have the symbols); a file
    that names no symbol has none."""
    include = '#include "sym/%s.h"\n' % rel.replace(os.sep, '/')
    names_one = re.search(r'\b(?:sym|points)::\w|\bREADABLE_(?:FUNCTION|PLANNED)\w*\(', blank(result))  # (a registration: its macro names the symbol)
    if not names_one or any(line == include for _, line in conditional_includes(result)):
        result = re.sub(r'^' + re.escape(include), '', result, flags=re.M)
    if include in result or not names_one:
        return result
    last = None
    held = conditional_lines(result)
    code = blank(result)
    for m in re.finditer(r'^#include [^\n]*\n', result, re.M):
        line = result.count('\n', 0, m.start())
        if line not in held and not re.search(r'\bnamespace\b', code[:m.start()]):     # (not one at the end of the file)
            last = m
    if last:
        result = result[:last.end()] + include + result[last.end():]
        if inserted is not None:   # (in the converted text's lines: back to the original's)
            before = result.count('\n', 0, last.end())
            aliases_before = sum(1 for line in inserted if line < before)
            inserted.append(before - aliases_before)
    else:
        once = re.search(r'^#pragma once\n', result, re.M)
        at = once.end() if once else 0
        result = result[:at] + '\n' + include + result[at:]
    return result


def convert_text(text, rel, ai, report, dm2=None, inserted=None):
    code = blank(text)
    edits = []
    # (a game folder's source names its game's rows plainly: sym::x, points::f, READABLE_FUNCTION)
    folder = dm2 is not None and folder_of(rel) == 'dm2'

    def sym_of(scope):
        return 'sym::dm2::' if scope is dm2 and not folder else 'sym::'

    def fn_symbol(scope, address, fallback=None):
        t = scope.t
        name = t.by_address.get(('function', address))
        if name:
            return name
        name = scope.facts.function_name(address) or fallback
        if not name:
            return None
        name = clash_free(t, name, 'function', address)
        t.put('function', name, address)
        return name

    for m in REG3.finditer(code):
        address = int(m.group(2), 16)
        short = m.group(3).split('::')[-1]
        scope = dm2 if folder else ai
        name = fn_symbol(scope, address, short)
        if name == short and '::' not in m.group(3):
            new = 'READABLE_%s(%s, %s)' % (m.group(1), short, m.group(4))
        else:
            new = 'READABLE_%s(sym::%s, %s, %s)' % (m.group(1), name, m.group(3), m.group(4))
        edits.append((m.start(), m.end(), new))
    for m in REG_PLANNED.finditer(code):
        name = fn_symbol(ai, int(m.group(1), 16), m.group(2))
        edits.append((m.start(1) - 2, m.end(1), 'sym::' + name))
    for m in REG_IN3.finditer(code):
        if m.group(2) == 'GAME':
            continue
        scope = dm2 if m.group(2) == 'dm2' and dm2 else ai
        short = m.group(4).strip('"').split('::')[-1]
        name = fn_symbol(scope, int(m.group(3), 16), short)
        edits.append((m.start(3) - 2, m.end(3), sym_of(scope) + name))
    registered = [int(m.group(2), 16) for m in REG3.finditer(code)] + [int(m.group(3), 16) for m in REG_IN3.finditer(code)
                                                                        if m.group(2) != 'GAME']
    covered = [(s, e) for s, e, _ in edits]
    covered_starts = sorted(s for s, _ in covered)
    covered_map = dict(covered)
    bodies = function_bodies(code)
    body_starts = [b[0] for b in bodies]
    aliases = {}    # body start -> function name
    # (a name of the source's own: the points are written out in full)
    here_taken = bool(re.search(r'\bhere\b', code)) and 'namespace here =' not in code

    def in_covered(pos):
        i = bisect.bisect_right(covered_starts, pos) - 1
        return i >= 0 and pos < covered_map[covered_starts[i]]

    for m in LITERAL.finditer(code):
        if in_covered(m.start()):
            continue
        address = int(m.group(1), 16)
        uncached = IMAGE_LOW <= address - 0x20000000 < IMAGE_HIGH
        if uncached:
            address -= 0x20000000
        elif not IMAGE_LOW <= address < IMAGE_HIGH:
            continue
        if NOT_ADDRESS in text[text.rfind('\n', 0, m.start()) + 1:text.find('\n', m.end())]:
            continue
        line_start = code.rfind('\n', 0, m.start()) + 1
        line_end = code.find('\n', m.end())
        pre = code[line_start:m.start()]
        where = '%s:%d' % (rel, code.count('\n', 0, m.start()) + 1)
        decl = CONSTEXPR.search(pre)
        glob = GLOBAL_DECL.search(pre)
        scope = literal_scope(rel, decl.group(1) if decl else None, address, ai, dm2)
        facts, t = scope.facts, scope.t
        kind = facts.kind(address)
        round_number = address & 0xFFFF == 0 and not glob and not MARK_ARGUMENT.search(pre)
        if uncached or (round_number and not BITWISE_BEFORE.search(pre) and not BITWISE_AFTER.match(code, m.end())):
            pass
        elif kind is None or BITWISE_BEFORE.search(pre) or BITWISE_AFTER.match(code, m.end()) or (kind == 'point' and address & 3):
            edits.append((m.start(), m.end(), number_form(m, address)))
            continue
        if round_number:
            name, datum = high_symbol(address, registered, code, scope)
            if not name:
                written = number_form(m, address, True)
                if written:
                    edits.append((m.start(), m.end(), written))
                else:
                    report.append('%s: 0x%X: round: a number or the high half of an address?' % (where, address))
                continue
            if ('function', datum) not in t.by_address:
                t.put('data', name, datum)
            written = '(%s%s + %s)' % ((sym_of(scope),) + tuple(name.split('+'))) if '+' in name else sym_of(scope) + name
            edits.append((m.start(), m.end(), 'ee::high(%s)' % written))
            continue
        if kind == 'function':
            fallback = None
            if decl:
                fallback = decl.group(1)[1].lower() + decl.group(1)[2:]
            else:
                # (a table of a function and its name)
                named_row = re.match(r'[uUlL]*\s*,\s*\{?\s*"(\w+)"', text[m.end():m.end() + 60])
                fallback = named_row.group(1) if named_row else None
                if not fallback:     # `0x... /* name */` and a shim's entry
                    written = re.match(r'[uUlL]*\s*/\*\s*(\w+)\s*\*/', text[m.end():m.end() + 60])
                    shim = re.match(r'[uUlL]*\s*,\s*&ee::entryDirect<&shim(\w+)>', text[m.end():m.end() + 80])
                    fallback = written.group(1) if written else lower_first('k' + shim.group(1)) if shim else None
            name = fn_symbol(scope, address, fallback)
            if not name:
                report.append('%s: 0x%X: a function with no name' % (where, address))
                continue
            edits.append((m.start(), m.end(), sym_of(scope) + name))
        elif kind == 'data':
            name = t.by_address.get(('data', address))
            if not name and glob:
                name = re.match(r'\s*(\w+)', code[code.find('>', m.end()) + 1:]).group(1)
            if not name and decl:
                name = decl.group(1)[1].lower() + decl.group(1)[2:]
            if not name:
                name = facts.text_name(address)
            if not name:
                report.append('%s: 0x%X: data with no name' % (where, address))
                continue
            if '+' in name:   # a part of a named object
                base, offset = name.split('+')
                written = '(%s%s + %s)' % (sym_of(scope), base, offset)
            else:
                name = clash_free(t, name, 'data', address)
                t.put('data', name, address)
                written = sym_of(scope) + name
            edits.append((m.start(), m.end(), 'ee::uncached(%s)' % written if uncached else written))
        else:
            function = facts.function_at(address)
            if function is None:
                report.append('%s: 0x%X: code outside every function' % (where, address))
                continue
            fname = fn_symbol(scope, function)
            if not fname:
                report.append('%s: 0x%X: in a function with no name' % (where, address))
                continue
            name = t.by_address.get((fname, address))
            if not name:
                name = facts.point_names(function).get(address)
                if not name:
                    report.append('%s: 0x%X: not an instruction of %s' % (where, address, fname))
                    continue
                t.put('point', name, address, fname)
            body = body_of(bodies, body_starts, m.start())
            qualified = ('dm2::' if scope is dm2 and not folder else '') + fname
            if not here_taken and body is not None and aliases.setdefault(body, qualified) == qualified:
                edits.append((m.start(), m.end(), 'here::' + name))
            else:
                edits.append((m.start(), m.end(), 'points::%s::%s' % (qualified, name)))
    for body, fname in aliases.items():
        line_end = code.find('\n', body)
        if code[body:line_end].strip():  # a body on one line
            edits.append((body, body, ' namespace here = points::%s;' % fname))
            continue
        next_line = text[line_end + 1:text.find('\n', line_end + 1)]
        indent = re.match(r'\s*', next_line).group(0)
        edits.append((line_end + 1, line_end + 1, '%snamespace here = points::%s;\n' % (indent, fname)))
        if inserted is not None:
            inserted.append(code.count('\n', 0, line_end + 1))
    if not edits:
        return own_table(text, rel, inserted)
    out = []
    pos = 0
    for start, end, new in sorted(edits):
        out.append(text[pos:start])
        out.append(new)
        pos = end
    out.append(text[pos:])
    return own_table(''.join(out), rel, inserted)


def comment_names(text, t, own=None):
    """A machine reference in a comment becomes the name: FUN_00171EC8 -> parseNumber. One with
    no row is left for the lint."""
    text = COMMENT_DATA_ONLY.sub('', text)     # (the drafter's `kName = ...; // DAT_00270000`: the row says it)
    code = blank(text)

    def name(m):
        if m.start() < len(code) and code[m.start()] != ' ':
            return m.group(0)
        address = int(m.group(1), 16)
        for table_ in ([own] if own else []) + [t]:
            found = table_.by_address.get(('function', address)) or table_.by_address.get(('data', address))
            if found:
                return found.split('+')[0]
        return m.group(0)

    text = COMMENT_ADDRESS.sub(name, text)
    if own is not None:      # (games/dm2's comments cite both games' addresses: left as they are)
        return text
    code = blank(text)

    def plain(m):
        if m.start() < len(code) and code[m.start()] != ' ':
            return m.group(0)
        address = int(m.group(1), 16)
        found = t.by_address.get(('function', address)) or t.by_address.get(('data', address))
        return found.split('+')[0] if found else m.group(0)

    return comment_doubles(COMMENT_HEX.sub(plain, text), t)


def comment_doubles(text, t):
    """What naming a reference leaves where the comment already had the name: `strcmp strcmp`,
    `vec3Length (vec3Length)`, `(copyStream, copyStream)`: the name once."""
    code = blank(text)
    known = set(t.syms)

    def in_comment(m):
        start = text.rfind('\n', 0, m.start()) + 1
        slash = text.find('//', start, m.start())
        return slash >= 0 and code[slash] == ' ' and m.group(1) in known and '{' not in text[slash:m.start()]

    for pattern, keep in ((r'\b(\w+) \(\1\)', r'\1'), (r'\b(\w+) \(\1, ', r'\1 ('), (r'\b(\w+),? \1\b', r'\1')):
        text = re.sub(pattern, lambda m: m.expand(keep) if in_comment(m) else m.group(0), text)
        code = blank(text)
    return text


def cmd_comments(args):
    t = table()
    for path in args.files:
        path = os.path.abspath(path)
        text = open(path, encoding='utf-8').read()
        new = comment_names(text, t, t.games.get('dm2') if os.sep + 'dm2' + os.sep in path else None)
        if new != text:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(new)
        for n, token in comment_references(new):
            print('%s:%d: %s' % (path, n, token))


def scopes(args, t):
    ai = Scope(Facts(args.elf, args.csv, t), t)
    dm2 = None
    if os.path.exists(args.elf_dm2) and os.path.exists(args.csv_dm2):
        own = t.own('dm2')
        dm2 = Scope(Facts(args.elf_dm2, args.csv_dm2, own), own)
    return ai, dm2


def cmd_convert(args):
    t = table()
    ai, dm2 = scopes(args, t)
    report = []
    root = os.path.abspath(args.root) if args.root else None
    for path in args.files:
        path = os.path.abspath(path)
        rel = os.path.relpath(path, root) if root else key_of(path)
        text = open(path, encoding='utf-8').read()
        new = convert_text(text, rel, ai, report, dm2)
        if new != text:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(new)
    t.save(args.out_table)
    for line in report:
        print(line)
    print('%d symbols, %d points; %d items left' % (len(t.syms), len(t.points), len(report)))


# ---- headers ------------------------------------------------------------------

_WORD = re.compile(r'[A-Za-z_]\w*')
# (`constexpr uint32_t k = sym::bmx::x;`: a game's own row, group 2, as sym::bmx::x written in place; it needs no bind.
# Initialised as `kX(sym::x)` or `kX{sym::x}` too: BMX's kComposedMatrix(sym::composedMatrix), unbound, went unseen)
_ALIAS_K = re.compile(r'\b(?:constexpr|const)\s+[\w:<>\s*]+?\b(k\w+)\s*(?:=\s*|[({]\s*)sym::(?:(dm2|bmx)::)?(\w+)')
_ALIAS_G = re.compile(r'Global<[^;]*?,\s*sym::(\w+)\s*>\s*(\w+)')
_GCALL = re.compile(r'\bgame::(?:call|tailCall|callAt|tailCallAt)<\s*&\s*([\w:]+)')
_REG_ANY = re.compile(r'\bREADABLE_(?:FUNCTION_WAITS|FUNCTION_VU0|FUNCTION)\(\s*(?:sym::(\w+)\s*,\s*)?([\w:]+)\s*,\s*(\w+)\s*\)')
# READABLE_FUNCTION[_WAITS|_VU0]_OWN(FN, STATUS, GAME, OWN[, GAME2, OWN2]) (ee/readable_waits.h): the shared symbol FN
# whose body in GAME's instance is OWN, a version's own implementation; AI's build and the other instances have FN's.
_REG_OWN = re.compile(r'\bREADABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION)_OWN\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)'
                      r'(?:\s*,\s*(\w+)\s*,\s*(\w+))?\s*\)')


def registrations_in(code, game=None):
    """[(symbol name, body name, status, own body or None)] of a source's symbol registrations as game's
    instance sees them (None: AI's build): the plain forms, and the _OWN form with that game's body."""
    return [row[1:] for row in registrations_at(code, game)]


def registrations_at(code, game=None):
    """registrations_in's rows, each after the position of its registration in code."""
    out = []
    # (a registration READABLE_EXCEPT keeps out of this game's instance is not this game's: None is AI's build)
    skipped = [(a, b) for a, b, g in excepted_spans(code) if g == (game or 'ai')]
    for m in _REG_ANY.finditer(code):
        if m.group(2).startswith('0x') or any(a <= m.start() < b for a, b in skipped):
            continue
        out.append((m.start(), m.group(1) or m.group(2).split('::')[-1], m.group(2).split('::')[-1], m.group(3), None))
    for m in _REG_OWN.finditer(code):
        if any(a <= m.start() < b for a, b in skipped):   # (an _OWN form READABLE_EXCEPT keeps out, as the plain ones)
            continue
        own = {m.group(4): m.group(5)}
        if m.group(6):
            own[m.group(6)] = m.group(7)
        body = own.get(game, m.group(2))
        out.append((m.start(), m.group(2), body, m.group(3), body if body != m.group(2) else None))
    return out


def write_if_changed(path, text):
    if os.path.exists(path) and open(path, encoding='utf-8').read() == text:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    return True


# ---- C++ name lookup (for Levels) -----------------------------------------------------------------------------
# Two namespaces can define functions with the same short name: zge::forEachChild (scene/transform.cpp) and
# zge::zgui::forEachChild (zgui/draw.cpp, sym forEachChild2). C++ decides which one a plain call reaches by looking
# the name up from the caller's scope outward, through the using-declarations and using-directives of each scope
# (the file's own and those of the headers it includes); a call's qualifier is looked up the same way. The levels
# used to key a call by its short name and take the last registration read, so the answer depended on the order the
# host listed the directories (winapp3: NTFS walks zgui after scene, so DM2's countModelNodes and addModelBone,
# whose pose.cpp calls zge::forEachChild, were level 1 on Windows only).

_NAMESPACE_OPEN = re.compile(r'\b(?:inline\s+)?namespace\s+((?:\w+\s*::\s*)*\w+)?\s*\{|\bextern\s+"C(?:\+\+)?"\s*\{')
_USING = re.compile(r'\busing\s+(namespace\s+)?(::\s*)?((?:\w+\s*::\s*)*\w+)\s*;')
_NAMESPACE_ALIAS = re.compile(r'\bnamespace\s+(\w+)\s*=\s*(::\s*)?((?:\w+\s*::\s*)*\w+)\s*;')
_QUOTED_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
_QUALIFIER_BEFORE = re.compile(r'(::\s*)?((?:\w+\s*::\s*)+)$')


def _qualified(text):
    return tuple(part.strip() for part in text.split('::') if part.strip())


# What namespace_scopes steps over (a preprocessor line with its continuations, a string or character literal; a
# quote after a letter or digit is a digit separator, 1'000) and the braces it counts.
_SCOPE_TOKEN = re.compile(r'^[ \t]*#(?:[^\n]*\\\n)*[^\n]*|"(?:\\.|[^"\\\n])*"?|(?<![0-9A-Za-z])\'(?:\\.|[^\'\\\n])*\'?|[{}]',
                          re.M)


def namespace_scopes(code):
    """[(position, namespaces, at namespace scope, in an unnamed namespace)], one from the start and one after each
    brace: the namespaces enclosing the code from that position on, whether it is at namespace scope (in no class,
    function or block), and whether an unnamed namespace encloses it. code is readable_digest.strip's (no
    comments). An unnamed namespace adds no name (C++'s implicit using-directive makes its names its enclosing
    namespace's, in its own translation unit only), nor does extern "C"; a brace in a literal or a preprocessor
    line is none."""
    opens, unnamed = {}, set()
    for m in _NAMESPACE_OPEN.finditer(code):
        opens[m.end() - 1] = _qualified(m.group(1) or '')
        if m.group(0).lstrip().startswith(('namespace', 'inline')) and not m.group(1):
            unnamed.add(m.end() - 1)
    stack, out, inside = [], [(0, (), True, False)], []
    for m in _SCOPE_TOKEN.finditer(code):
        c = m.group()
        if c == '{':
            stack.append(opens.get(m.start()))
            inside.append(m.start() in unnamed)
        elif c == '}' and stack:
            stack.pop()
            inside.pop()
        else:
            continue
        names = ()
        for part in stack:
            if part is None:
                break
            names += part
        out.append((m.end(), names, all(part is not None for part in stack), any(inside)))
    return out


def definition_spans(code, rd):
    """{name: [(start, end)]}: where each of readable_digest.definitions(code)'s texts is, in the same order."""
    spans = {}
    for m in rd.IDENT_CALL.finditer(code):
        name = m.group(1)
        if name in rd.KEYWORDS or name.isupper():
            continue
        q = rd.match(code, m.end() - 1, '(', ')')
        if q < 0:
            continue
        t = re.match(r'\s*(?:const\s*|noexcept\s*|override\s*|final\s*|->\s*[\w:<>,\s*&]+?\s*)*\{', code[q:q + 200])
        if not t:
            continue
        e = rd.match(code, q + t.end() - 1, '{', '}')
        if e < 0:
            continue
        spans.setdefault(name, []).append((m.start(), e))
    return spans


class _Visible:
    """The tree's functions and those of a translation unit's unnamed namespaces, as one set to look in."""

    def __init__(self, known, local):
        self.known, self.local = known, local

    def __contains__(self, name):
        return name in self.known or name in self.local


class NameLookup:
    """The functions the game tree's sources define at namespace scope, by qualified name, and C++'s lookup of a
    call's name among them (lookup)."""

    def __init__(self, codes, tree):
        """codes: {path: readable_digest.strip of its text}, every source and header of the tree."""
        import readable_digest as rd
        self.scopes, self.spans, self.qualifiers = {}, {}, {}
        self.known, self.namespaces = set(), {()}
        self.local = {}     # path -> the functions it defines in an unnamed namespace: its translation unit's only
        own_usings, self.block_usings, self.aliases = {}, {}, {}
        for p in sorted(codes):
            code = codes[p]
            scopes = namespace_scopes(code)
            self.scopes[p] = ([s[0] for s in scopes], scopes)
            for _, names, _, _ in scopes:
                self.namespaces.update(names[:k] for k in range(len(names) + 1))
            spans = self.spans[p] = definition_spans(code, rd)
            for name, where in spans.items():
                for start, _ in where:
                    names, at_namespace = self.scope_at(p, start)
                    unnamed = self.in_unnamed(p, start)
                    q = _QUALIFIER_BEFORE.search(code, max(0, start - 200), start) \
                        if code[max(0, start - 40):start].rstrip().endswith('::') else None
                    qualifier = _qualified(q.group(2)) if q else ()
                    base = () if q and q.group(1) else names
                    self.qualifiers[(p, start)] = base + qualifier
                    if at_namespace:
                        (self.local.setdefault(p, set()) if unnamed else self.known).add(base + qualifier + (name,))
                        self.namespaces.update((base + qualifier)[:k] for k in range(len(base + qualifier) + 1))
            for m in _USING.finditer(code):
                names, at_namespace = self.scope_at(p, m.start())
                row = (names, 'directive' if m.group(1) else 'declaration', _qualified(m.group(3)), bool(m.group(2)))
                if at_namespace:
                    own_usings.setdefault(p, []).append(row)
                else:
                    self.block_usings.setdefault(p, []).append((m.start(),) + row)
            for m in _NAMESPACE_ALIAS.finditer(code):
                names, _ = self.scope_at(p, m.start())
                self.aliases.setdefault(names + (m.group(1),), (_qualified(m.group(3)), bool(m.group(2)), names))
        # A source sees its own namespace-scope usings and those of every header it includes, directly or not.
        includes = {}
        for p, code in codes.items():
            found = []
            for m in _QUOTED_INCLUDE.finditer(code):
                for base in (os.path.dirname(p),) + tuple(os.path.join(tree, r) for r in zaxis_paths.zlayout.READABLE_ROOTS) + (tree,):
                    path = os.path.normpath(os.path.join(base, m.group(1)))
                    if path in codes:
                        found.append(path)
                        break
            includes[p] = found
        self.usings, self.visible = {}, {}
        for p in codes:
            seen, todo = set(), [p]
            while todo:
                q = todo.pop()
                if q not in seen:
                    seen.add(q)
                    todo += includes.get(q, [])
            self.usings[p] = sorted(set(row for q in seen for row in own_usings.get(q, [])))
            self.visible[p] = _Visible(self.known, set().union(*(self.local.get(q, ()) for q in seen)))

    def scope_at(self, p, pos):
        """(the namespaces enclosing pos in p, whether pos is at namespace scope)."""
        starts, scopes = self.scopes[p]
        _, names, at_namespace, _ = scopes[bisect.bisect_right(starts, pos) - 1]
        return names, at_namespace

    def in_unnamed(self, p, pos):
        starts, scopes = self.scopes[p]
        return scopes[bisect.bisect_right(starts, pos) - 1][3]

    def definition_scope(self, p, start):
        """The scope a definition's body looks names up from: its namespaces and its own qualifier (a member
        defined outside its class, `Node::visit(...) {`, looks in the class first)."""
        return self.qualifiers.get((p, start), self.scope_at(p, start)[0])

    def _namespace(self, p, scope, path, absolute, depth=0):
        """The namespace (or class) path names from scope: its first name looked up outward, through aliases."""
        if absolute:
            scope = ()
        for k in range(len(scope), -1, -1):
            if absolute and k < len(scope):
                break
            outer = scope[:k]
            alias = self.aliases.get(outer + path[:1])
            if alias and depth < 8:
                target = self._namespace(p, alias[2], alias[0], alias[1], depth + 1)
                if target is not None:
                    return target + path[1:]
            if outer + path in self.namespaces:
                return outer + path
        return None

    def lookup(self, p, scope, span, qualifier, short, absolute=False):
        """The qualified names of the functions defined in the tree that a call `qualifier::short(` in p, in a
        definition at span (start, end) whose scope is scope, reaches, as C++'s name lookup finds them; None when
        it finds none of them (a function defined outside the game tree, or one only argument-dependent lookup
        or a macro reaches). The scopes are searched from the innermost out and the first that has the name ends
        the search: a using-declaration declares the name in its own scope (a function's block first), and a
        using-directive's names count as declared in the nearest namespace that encloses both it and the
        namespace it names (`using namespace zge;` in aggressive: as zge's own, after aggressive's names)."""
        known = self.visible.get(p, self.known)
        block = [row[1:] for row in self.block_usings.get(p, []) if span[0] <= row[0] < span[1]]
        usings = self.usings.get(p, [])
        if qualifier:
            target = self._namespace(p, scope, qualifier, absolute)
            if target is None:
                return None
            if target + (short,) in known:
                return {target + (short,)}
            found = self._declared(p, [u for u in usings if u[0] == target], target, short)
            return found or None
        chain = [scope[:k] for k in range(len(scope), -1, -1)]
        nominated = []      # (the namespace its names count as declared in, the namespace a directive names)
        for rows, at in [(block, scope)] + [([u for u in usings if u[0] == names], names) for names in chain]:
            for _, kind, path, absolute_path in rows:
                if kind == 'directive':
                    target = self._namespace(p, at, path, absolute_path)
                    if target is not None:
                        common = 0
                        while common < min(len(at), len(target)) and at[common] == target[common]:
                            common += 1
                        nominated.append((at[:common], target))
        for names, rows, own in [(scope, block, False)] + [(names, [u for u in usings if u[0] == names], True)
                                                          for names in chain]:
            found = self._declared(p, rows, names, short)
            if own:
                if names + (short,) in known:
                    found.add(names + (short,))
                found |= {target + (short,) for at, target in nominated if at == names and target + (short,) in known}
            if found:
                return found
        return None

    def _declared(self, p, rows, names, short):
        """What the using-declarations among rows (at scope names) that declare short name."""
        known = self.visible.get(p, self.known)
        found = set()
        for _, kind, path, absolute in rows:
            if kind == 'declaration' and path[-1] == short:
                target = self._namespace(p, names, path[:-1], absolute) if len(path) > 1 else ()
                if target is not None and target + (short,) in known:
                    found.add(target + (short,))
        return found


class Levels:
    """Per registered function of a game: 2 its C++ body runs there, 1 only its address is
    known (game::call reaches the recompiled function), 0 no such function there."""

    def __init__(self, bind, t, tree, game=None):
        sys.path.insert(0, HERE)
        import readable_digest
        self.bind, self.t = bind, t
        self.tree = tree
        self.level, self.why = {}, {}
        # game/<game>/close.tsv: functions read and proven the same logic though their words differ.
        close_table = readable(tree, game or '', 'close.tsv')
        self.close = set()
        self.game = game
        if game and os.path.exists(close_table):
            self.close = {line.split('\t')[0] for line in open(close_table, encoding='utf-8') if line.strip() and not line.startswith('#')}
        self.aliases = {}
        self.files = {}
        self.own = {}       # name -> this game's own body (the _OWN form)
        # What a plain call of a registered body (an own body's too) reaches: its symbol, by the body's qualified name
        # (a tuple of names), and by its short name for a call C++'s lookup in the tree does not settle (NameLookup).
        self.entry_of = {}      # qualified body name -> symbol
        self.entries_named = {}  # short body name -> {symbols}
        self.regs_in = {}   # path -> {body name: symbol} of the functions its own source registers
        # plain_sources.txt: a source the build compiles from its entries' copy (readable_entries.py), where a call of
        # a registered function is its entry (game::detail::Route) and not its body inline
        plain_list = readable(tree, 'plain_sources.txt')
        self.plain_prefixes = tuple(line.strip() for line in open(plain_list, encoding='utf-8')
                                    if line[:1].isalpha()) if os.path.exists(plain_list) else ()
        files = list(game_sources(tree))
        raw = {p: open(p, encoding='utf-8', errors='replace').read() for p in files}
        for p, text in raw.items():     # (a constant's name means what its own file says: kNoText is two things)
            own = self.aliases.setdefault(p, {})
            for m in _ALIAS_K.finditer(text):
                own[m.group(1)] = None if m.group(2) else m.group(3)    # (None: a game's own row, not AI's name)
            for m in _ALIAS_G.finditer(text):
                own[m.group(2)] = m.group(1)
        self.flag_constants = flag_constants(readable_digest.strip(text) for text in raw.values())
        self.codes = {p: readable_digest.strip(text) for p, text in raw.items()}
        self.names = NameLookup(self.codes, tree)
        registered = []
        excepted = {}   # name -> file of the registrations READABLE_EXCEPT keeps out of this game's instance
        # The addresses this game's own folder registers (its own rows, else AI's names as the bind puts them, as the
        # headers write them): a function --try-close tries is never registered at one of them, since readable::find
        # takes an address's first registration (DM2 0x1845C0: zge's level 1 unregisterLevelObjectType, bound there,
        # ran in place of games/dm2's bmxobjctvGenerateHash until the bind was made again).
        self.folder_at = set()
        for p, text in raw.items():
            if not game or 'READABLE_' not in text or folder_of(key_of(p, tree)) != game:
                continue
            for name, _, _, _ in registrations_in(readable_digest.strip(text), game):
                own_row = t.own(game).syms.get(name)
                address = own_row[1] if own_row else resolve(bind, t, name)
                if address is not None:
                    self.folder_at.add(address)
        for p, text in raw.items():
            if 'READABLE_' not in text or folder_of(key_of(p, tree)):
                continue
            code = self.codes[p]
            defs = readable_digest.definitions(code)
            for a, b, g in excepted_spans(code):
                if g == (game or 'ai'):
                    for m in _REG_ANY.finditer(code, a, b):
                        excepted[m.group(1) or m.group(2).split('::')[-1]] = key_of(p, tree).replace(os.sep, '/')
                    for m in _REG_OWN.finditer(code, a, b):
                        excepted[m.group(2)] = key_of(p, tree).replace(os.sep, '/')
            for at, name, short, _, own in registrations_at(code, game):
                registered.append((name, short, defs, p))
                self.files[name] = key_of(p, tree).replace(os.sep, '/')
                self.add_entry(p, at, short, name)
                self.regs_in.setdefault(p, {})[short] = name
                if own:
                    self.own[name] = own
            for m in _REG_OWN.finditer(code):     # (every version's own body: readable_entries gives each an entry)
                for body in (m.group(2), m.group(5), m.group(7)):
                    if body:
                        self.add_entry(p, m.start(), body, m.group(2))
                        self.regs_in.setdefault(p, {})[body] = m.group(2)
        # First the ones that have no body here, then a fixed point over game::call.
        for name, _, _, _ in registered:
            entry = t.syms.get(name)
            if not entry:
                continue
            address = entry[1]
            self.level[name] = 0 if bind.function(address) is None else (
                2 if bind.ai or bind.body(address) or name in self.close or name in self.own else 1)
            if self.level[name] == 0:
                self.why[name] = 'no counterpart'
            elif self.level[name] == 1:
                self.why[name] = 'its code differs here'
            elif name in self.own:
                self.why[name] = 'own body ' + self.own[name]
        # A shared registration READABLE_EXCEPT keeps out of this game's instance: nothing here, the game's own folder
        # registers its version at the address.
        self.excepted = set()   # the names READABLE_EXCEPT keeps out of this instance (level 0)
        for name, rel in excepted.items():
            if name in t.syms and name not in self.level:
                self.level[name] = 0
                self.why[name] = 'not registered here: %s\'s own body is at the address (READABLE_EXCEPT)' % game
                self.files[name] = rel
                self.excepted.add(name)
        # A function only one engine version has (READABLE_FUNCTION_ONLY, a shared source): in that game's
        # instance, at the game's own address (its row: there is no AI function to bind), with its body unless
        # something it names is not bound here; in AI's build and the other instances it is not registered.
        self.only = {}  # name -> the game's address
        for p, text in raw.items():
            if '_ONLY(' not in text or folder_of(key_of(p, tree)) or not game:
                continue
            code = readable_digest.strip(text)
            found = [(m.group(3)) for m in _REG_ONLY.finditer(code) if m.group(2) == game]
            if not found:
                continue
            defs = readable_digest.definitions(code)
            for name in found:
                entry = t.own(game).syms.get(name)
                if not entry or entry[0] != 'function':
                    raise SystemExit('%s: READABLE_FUNCTION_ONLY(%s, %s): no function row of %s\'s (readable_symbols.py add '
                                     'function %s ADDRESS --game %s)' % (key_of(p, tree), game, name, game, name, game))
                if name in self.level:
                    raise SystemExit('%s: READABLE_FUNCTION_ONLY(%s, %s): the name is registered as a shared function too' % (
                        key_of(p, tree), game, name))
                self.only[name] = entry[1]
                self.level[name] = 2
                self.files[name] = key_of(p, tree).replace(os.sep, '/')
                registered.append((name, name, defs, p))
        changed = True
        while changed:
            changed = False
            for name, short, defs, path in registered:
                if self.level.get(name) != 2 or bind.ai:
                    continue
                problem = self.problem(short, defs, readable_digest, self.aliases[path], self.regs_in.get(path, {}),
                                       self.is_plain(path), path)
                if problem:
                    self.level[name] = 1
                    self.why[name] = problem
                    changed = True
        # An address can be two AI functions' (DM2 calls heapAllocate where AI calls its allocateSized):
        # both call it, but only the one whose body it is registers a body there.
        self.bodies_at = {self.address(name) for name, level in self.level.items() if level == 2}
        self.instance = None    # (headers --instance: the shared sources the game's instance compiles)
        self._weaker = None

    def callee_level(self, callee):
        """The level a plain or game call of callee reaches here: its own, except for a name READABLE_EXCEPT keeps out
        of this instance (level 0), whose call goes to what the game has at the address the bind puts it: the game's
        own body when its folder registers one there (level 2, as every folder registration), or an _ONLY function at
        that address (its level); else nothing readable is there and it stays 0 (facetree1: BMX XXX's testNodeFaces
        calls segmentFaceTree, whose BMX body is games/bmx's segmentFaceTreeUncut at 0x122528)."""
        level = self.level.get(callee, -1)
        if level != 0 or callee not in self.excepted:
            return level
        address = resolve(self.bind, self.t, callee)
        if address is None:
            return 0
        if address in self.folder_at:
            return 2
        at = [self.level.get(name, 0) for name, own in self.only.items() if own == address]
        return max(at) if at else 0

    def add_entry(self, p, at, body, symbol):
        """A registration in p at position at: body (its unqualified name, as the macro names it, looked up from
        there as C++ does) is symbol's."""
        names, _ = self.names.scope_at(p, at)
        found = self.names.lookup(p, names, (at, at), (), body)
        qualified = min(found) if found and len(found) == 1 else names + (body,)
        if self.entry_of.get(qualified, symbol) != symbol:
            raise SystemExit('%s: %s registers %s, and %s is registered as %s too' % (
                key_of(p, self.tree), '::'.join(qualified), symbol, '::'.join(qualified), self.entry_of[qualified]))
        self.entry_of[qualified] = symbol
        self.entries_named.setdefault(body, set()).add(symbol)

    def callees(self, path, span, call, short):
        """The symbols a call of short reaches, call its text up to the name (`zge::zgui::forEachChild`, or
        `forEachChild`) in the definition at span of path: what C++'s lookup finds there, if that is registered
        (none if it finds a function the tree defines but does not register); a call the lookup does not settle
        reaches every registered body of that name (each must be right here: the answer does not depend on which
        was read last)."""
        text = re.sub(r'\s+', '', call)
        found = None
        if path is not None and not text.startswith('>'):
            found = self.names.lookup(path, self.names.definition_scope(path, span[0]), span, _qualified(text)[:-1], short,
                                      text.startswith('::'))
        if found is None:
            return sorted(self.entries_named.get(short, ()))
        return sorted(self.entry_of[q] for q in found if q in self.entry_of)

    def weaker(self):
        """The level 1 functions not tried because another one the instance compiles is paired with the same address
        of this game by a better proven pair, or as well proven as theirs: only the best is tried (readable::find takes
        the first registration, so two tried bodies at one address left the choice to the link order: DM2 0x149E78,
        loadLevelPaths high and read against spawnLevelObjects medium by neighbour; d7scene's note names it
        loadLevelPaths), none when no pair is better than the others."""
        if self._weaker is None:
            by = {}
            for name, level in self.level.items():
                if level == 1 and name not in self.only and (self.instance is None or self.files.get(name) in self.instance):
                    by.setdefault(self.address(name), []).append(name)
            self._weaker = set()
            for names in by.values():
                if len(names) < 2:
                    continue
                strength = {n: getattr(self.bind, 'strength', {}).get(self.t.syms[n][1], 0) for n in names}
                best = max(strength.values())
                leaders = [n for n in names if strength[n] == best]
                self._weaker |= set(n for n in names if strength[n] < best or len(leaders) > 1)
        return self._weaker

    def tried(self, name):
        """Whether --try-close registers name's level 1 body at level 2 in this game's corpus build: not at an address
        another registration's body is at (a level 2 one's, this game's own folder's, or a level 1 one's whose pair the
        table proves better)."""
        if self.level.get(name) != 1:
            return False
        address = self.address(name)
        return address not in self.bodies_at and address not in self.folder_at and name not in self.weaker()

    def address(self, name):
        """The game's address of a registered function: its bound AI symbol's, or a game-only function's own."""
        return self.only[name] if name in self.only else resolve(self.bind, self.t, name)

    def runs_here(self, short, defs, rd, local):
        """The definitions of this file that run as short's own C++: its body and the helpers it reaches, not a
        registered function's body (a call of one is its entry) nor what only that body reaches."""
        runs, todo = set(), [short]
        while todo:
            n = todo.pop()
            if n in runs or n not in defs:
                continue
            runs.add(n)
            for d in defs[n]:
                for ident in set(rd.IDENT.findall(skip_game_steps(d, self.game, self.flag_constants))):
                    if ident in defs and ident not in runs and ident not in local:
                        todo.append(ident)
        return runs

    def is_plain(self, path):
        rel = key_of(path, self.tree).replace(os.sep, '/')
        return any(rel.startswith(prefix) for prefix in self.plain_prefixes)

    def problem(self, short, defs, rd, aliases, local=None, plain=False, path=None):
        """Why short's body cannot run here, or None. In a plain source what runs as its C++ is its body and the
        helpers it reaches (runs_here): a registered callee is reached at its address (its body where it is at
        level 2 here, and then its own check answers for it; else the recompiled original), so its body's needs
        are not this function's (BMX setGsTextureImage named g_gsVideoMode through buildGsTextureUpload, level 1
        there; DM2 releasePart releaseStream's points). In another source a same-file call runs the body inline:
        every definition it reaches is walked."""
        local = local or {}     # body name -> symbol of the functions this file registers
        # (names in sorted order: the reason given is the same in every run, whatever the string hashes)
        runs = self.runs_here(short, defs, rd, local)
        seen, todo = set(), [short]
        while todo:
            n = todo.pop()
            if n in seen or n not in defs:
                continue
            seen.add(n)
            if plain and n not in runs:
                continue
            spans = self.names.spans.get(path, {}).get(n, [])
            for k, d in enumerate(defs[n]):
                span = spans[k] if k < len(spans) else (0, 0)
                d = skip_game_steps(d, self.game, self.flag_constants)
                for ident in sorted(set(rd.IDENT.findall(d))):
                    ident = self.own.get(ident, ident)   # (a callee with an own body here: that body's needs, not the shared one's)
                    if ident in defs and ident not in seen:
                        todo.append(ident)
                current = None
                alias = re.search(r'namespace here = points::(\w+);', d)
                if alias:
                    current = alias.group(1)
                for m in _HERE.finditer(d):
                    if current and self.t.points.get((current, m.group(1))) is not None:
                        if self.bind.point(self.t.points[(current, m.group(1))]) is None:
                            return 'point %s::%s' % (current, m.group(1))
                for m in _POINT.finditer(d):
                    a = self.t.points.get((m.group(1), m.group(2)))
                    if a is not None and self.bind.point(a) is None:
                        return 'point %s::%s' % m.groups()
                names = set(_SYM.findall(d))
                for ident in sorted(set(rd.IDENT.findall(d))):
                    if aliases.get(ident):
                        names.add(aliases[ident])
                for name in sorted(names):
                    if resolve(self.bind, self.t, name) is None:
                        return 'symbol ' + name
                for m in _GCALL.finditer(d):
                    named = m.group(1).split('::')[-1].rstrip('_')    # GAME_CALL's NAME##_
                    # (a name the lookup does not find registered is checked as a symbol, as it was by its name alone)
                    for callee in self.callees(path, span, m.group(1).rstrip('_'), named) or [named]:
                        if self.callee_level(callee) == 0:
                            return 'callee ' + callee
                        if callee not in self.level and resolve(self.bind, self.t, callee) is None:
                            return 'callee ' + callee
                # A plain call of a registered function, in this file or another, is its entry: Route goes to what is
                # installed at the callee's address here (its body, its own body, or the recompiled original), so
                # the callee needs an address in this game (d4scene3: BMX's own setupSceneModelWithOptions called
                # nodeName, of geometry_nodes.cpp, which BMX's table had not bound; only the replay saw it). Checked
                # in the code that runs as this function's C++: its body and its helpers, not a registered callee's
                # body (that runs only where the callee is at level 2, and its own check covers it). A name this
                # file defines is the file's (a helper of the same name as another file's function is its own).
                if n not in runs:
                    continue
                for m in itertools.chain(_PLAIN_CALL.finditer(d), _TAIL_CALL.finditer(d)):
                    if m.re is _PLAIN_CALL and _DECLARED.search(d, 0, m.start()):
                        continue    # (`ee::Ptr<AnimatePlaneGroup> group(...)`: a local, not aggressive::group)
                    called = m.group(1)
                    if called == short:
                        continue
                    if called in defs:
                        callees = [local[called]] if called in local else []
                    else:
                        callees = self.callees(path, span, called_as(d, m), called)
                    for callee in callees:
                        if callee in self.only:
                            continue
                        if self.callee_level(callee) == 0 or resolve(self.bind, self.t, callee) is None:
                            return 'callee ' + callee
        return None


def called_as(d, m):
    """The name a _PLAIN_CALL or _TAIL_CALL match m in d calls, as written (`zge::zgui::forEachChild`), `::` first
    when it is qualified from the global namespace, `>::` first when by a template's (a member: not looked up)."""
    text = m.group(0)[:m.end(1) - m.start(0)]
    if m.re is _TAIL_CALL:
        text = text.split('(', 1)[1]
    before = d[:m.start() if m.re is _PLAIN_CALL else m.start() + m.group(0).index('(') + 1].rstrip()
    if before.endswith('::'):
        text = ('>::' if before[:-2].rstrip().endswith('>') else '::') + text
    return text.strip()


# A call by name (not a member's: obj.f( or p->f(), or a function handed to game::tail.
_PLAIN_CALL = re.compile(r'(?<![\w.>])(?:\w+::)*(\w+)\s*\(')
# Before a name, a type: the name is declared there (`T name(initialiser)`), not called. (A word that ends a
# statement's head, return or else, is no type.)
_DECLARED = re.compile(r'(?:(?<![\w:])(?!(?:return|else|case|throw|do|co_return|co_yield|co_await|not|and|or|new|delete)\b)'
                       r'[A-Za-z_]\w*|>)\s*$')
_TAIL_CALL = re.compile(r'\btail\(\s*(?:\w+::)*(\w+)')
_IF_CONSTEXPR = re.compile(r'\bif\s+constexpr\s*\(')
# A game flag: game::readable::kIsDm2, or the bare name a `using game::readable::kIsDm2;` brings in (no other
# kIsDm2 is declared).
_GAME_FLAG = re.compile(r'(?:\bgame::readable::|(?<![\w:]))kIs(Dm2|Bmx|Ai)\b')
# A constant made of the game flags alone (material.h: constexpr bool kMaterialTableKeepsSize = !game::readable::kIsDm2;).
_FLAG_CONSTANT = re.compile(r'\bconstexpr\s+bool\s+(k\w+)\s*=\s*([^;{}]+);')
_FLAGS_ONLY = re.compile(r'(?:\s|[()!]|&&|\|\||(?:\bgame::readable::|(?<![\w:]))kIs(?:Dm2|Bmx|Ai)\b)+')


def flag_constants(texts):
    """{name: condition} of the constants made of the game flags alone; a name declared twice otherwise is left out."""
    found = {}
    for text in texts:
        for m in _FLAG_CONSTANT.finditer(text):
            expr = ' '.join(m.group(2).split())
            found.setdefault(m.group(1), set()).add(expr if _FLAGS_ONLY.fullmatch(expr) else None)
    return {name: exprs.pop() for name, exprs in found.items() if len(exprs) == 1 and None not in exprs}


def skip_game_steps(text, game, constants=None):
    """The text without what this game's instance never runs, so that a symbol or callee named only there is not one
    the body needs: the branch an `if constexpr` on the game flags (game::readable::kIsDm2, kIsBmx, or a constant of
    flag_constants) leaves out, the rest of the block after a kept branch that returns (`if constexpr (kIsDm2)
    return ...;`: DM2 objectNameHash), and the other arm of a conditional expression on the flags (`kIsDm2 ? a : b`:
    DM2 nodeSceneObject). The arm kept is read the same way (an `else if constexpr` chain, a step inside a step);
    a condition that is not made of the game flags alone is left as it is, and its arms are read as any text."""
    if not game:
        return text
    constants = constants or {}
    # (a per-version parameter or argument another version's: no text in this game's instance, ee/readable_waits.h)
    return _skip_choices(_skip_steps(version_view(text, game), game, constants), game, constants)


def _flags_value(cond, game, constants):
    """True or False: a condition made of the game flags alone, for this game; None for any other."""
    if constants:
        cond = re.sub(r'(?<![\w:])k\w+\b', lambda c: '(%s)' % constants[c.group()] if c.group() in constants else c.group(), cond)
    expr = _GAME_FLAG.sub(lambda f: 'True' if f.group(1).lower() == game else 'False', cond)
    expr = expr.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ')
    if not re.fullmatch(r'(?:\s|[()]|True|False|and|or|not)+', expr):
        return None
    expr = ' '.join(expr.split())   # (one line: a condition broken before its `?`, `kIsDm2\n    ? a : b`, is a second
                                    # line to eval, an indent error, and was read as no flags condition: both arms kept)
    try:
        return bool(eval(expr, {'__builtins__': {}}, {}))  # (only True, False, and, or, not, parentheses)
    except SyntaxError:
        return None


def _returns(arm):
    """Whether a statement always returns: `return ...;`, or a block whose last statement does."""
    arm = arm.strip()
    if re.match(r'(?:return|throw)\b', arm):
        return True
    if not arm.startswith('{') or not arm.endswith('}'):
        return False
    inner, last, i = arm[1:-1], None, 0
    while inner[i:].strip():
        end = _statement_end(inner, i)
        if end <= i:
            break
        last, i = inner[i:end], end
    return last is not None and _returns(last)


def _block_end(text, at):
    """Where the block that `at` is in closes (its '}'), or the text's end."""
    depth = 0
    for i in range(at, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth < 0:
                return i
    return len(text)


def _skip_choices(text, game, constants):
    """`FLAGS ? a : b` as the arm this game takes (FLAGS: a flag term, !kIsDm2, game::readable::kIsBmx, a
    constant of flag_constants, or a parenthesised group of them, the whole of the condition)."""
    pos = 0
    while True:
        q = text.find('?', pos)
        if q < 0:
            return text
        pos = q + 1
        # the condition: back from '?' over a flags-only term or group
        j = q - 1
        while j >= 0 and text[j].isspace():
            j -= 1
        if j >= 0 and text[j] == ')':
            depth, k = 0, j
            while k >= 0:
                if text[k] == ')':
                    depth += 1
                elif text[k] == '(':
                    depth -= 1
                    if depth == 0:
                        break
                k -= 1
            start = k
        else:
            k = j
            while k >= 0 and (text[k].isalnum() or text[k] in '_:'):
                k -= 1
            start = k + 1
        while start > 0 and text[start - 1] in '! \t\n' and '!' in text[:start].rstrip()[-1:]:
            start = len(text[:start].rstrip()) - 1     # (a leading `!`, spaces between)
        if start < 0:
            continue
        # (the condition is the whole of it: an expression starts before it, after = ( , ? : { ; or return)
        before = text[:start].rstrip()
        if before and not (before[-1] in '=(,?:{;' or re.search(r'\breturn$', before)):
            continue
        if before.endswith(('==', '!=', '<=', '>=')):
            continue
        taken = _flags_value(text[start:q], game, constants)
        if taken is None:
            continue
        # the arms: to the ':' of this '?' (not '::'), then to the end of the expression
        depth, nested, k, colon = 0, 0, q + 1, None
        while k < len(text):
            c = text[k]
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
                if depth < 0:
                    break
            elif c == '?' and depth == 0:
                nested += 1
            elif c == ':' and depth == 0 and text[k + 1:k + 2] != ':' and text[k - 1:k] != ':':
                if nested == 0:
                    colon = k
                    break
                nested -= 1
            elif c == ';' and depth == 0:
                break
            k += 1
        if colon is None:
            continue
        depth, nested, k = 0, 0, colon + 1
        while k < len(text):
            c = text[k]
            if c in '([{':
                depth += 1
            elif c in ')]}':
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and c in ',;':
                break
            elif c == '?' and depth == 0:
                nested += 1
            elif c == ':' and depth == 0 and text[k + 1:k + 2] != ':' and text[k - 1:k] != ':':
                if nested == 0:
                    break
                nested -= 1
            k += 1
        arm = text[q + 1:colon] if taken else text[colon + 1:k]
        text = text[:start] + ' (' + arm.strip() + ') ' + text[k:]
        pos = start


def _skip_steps(text, game, constants):
    out, pos = [], 0
    while True:
        m = _IF_CONSTEXPR.search(text, pos)
        if not m:
            out.append(text[pos:])
            return ''.join(out)
        cond_start = m.end()
        cond_end = _matching(text, cond_start - 1, '(', ')')
        if cond_end is None:
            out.append(text[pos:cond_start])
            pos = cond_start
            continue
        taken = _flags_value(text[cond_start:cond_end], game, constants)
        if taken is None:
            out.append(text[pos:cond_end + 1])
            pos = cond_end + 1
            continue
        then_end = _statement_end(text, cond_end + 1)
        else_m = re.compile(r'\s*else\b').match(text, then_end)
        else_end = _statement_end(text, else_m.end()) if else_m else then_end
        out.append(text[pos:m.start()])
        kept = text[cond_end + 1:then_end] if taken else text[else_m.end():else_end] if else_m else ''
        out.append(_skip_steps(kept, game, constants))
        pos = else_end
        if _returns(kept):      # (what follows in the block this game never reaches)
            pos = _block_end(text, else_end)


def _matching(text, at, open_, close):
    depth = 0
    for i in range(at, len(text)):
        if text[i] == open_:
            depth += 1
        elif text[i] == close:
            depth -= 1
            if depth == 0:
                return i
    return None


_STATEMENT_HEAD = re.compile(r'(if|for|while|switch|do)\b')


def _statement_end(text, at):
    """Past the statement that starts at `at`: a block; if (constexpr) (...) STATEMENT [else STATEMENT]; for, while,
    switch (...) STATEMENT; do STATEMENT while (...); or one statement to its ';' at depth 0."""
    i = at
    while i < len(text) and text[i].isspace():
        i += 1
    if i < len(text) and text[i] == '{':
        end = _matching(text, i, '{', '}')
        return len(text) if end is None else end + 1
    head = _STATEMENT_HEAD.match(text, i)
    if head:
        j = head.end()
        if head.group(1) == 'do':
            body_end = _statement_end(text, j)
            k = text.find(';', body_end)     # (while (...);)
            return len(text) if k < 0 else k + 1
        c = re.compile(r'\s*(?:constexpr\s*)?\(').match(text, j) if head.group(1) == 'if' else re.compile(r'\s*\(').match(text, j)
        if c:
            close = _matching(text, c.end() - 1, '(', ')')
            if close is None:
                return len(text)
            body_end = _statement_end(text, close + 1)
            if head.group(1) == 'if':
                e = re.compile(r'\s*else\b').match(text, body_end)
                if e:
                    return _statement_end(text, e.end())
            return body_end
    depth = 0
    while i < len(text):
        c = text[i]
        if c in '({[':
            depth += 1
        elif c in ')}]':
            depth -= 1
            if depth < 0:
                return i        # (the enclosing block's end: a last statement with no ';')
        elif c == ';' and depth <= 0:
            return i + 1
        i += 1
    return len(text)


def resolve(bind, t, name):
    entry = t.syms.get(name)
    if not entry:
        return None
    kind, address = entry
    return bind.function(address) if kind == 'function' else bind.datum(address)


def status_rows(path):
    rows = {}
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'):
                rows[p[0]] = p[1]
    return rows


def cmd_headers(args):
    t = table()
    bind = Bind(args.bind)
    levels = Levels(bind, t, TREE, None if bind.ai else args.game)
    if getattr(args, 'instance', None):
        levels.instance = set(args.instance)
    statuses = status_rows(args.status)
    written = 0
    count = 0
    paths = [(p, TREE) for p in game_sources()]
    if args.drafts:
        for d, dirs, files in os.walk(args.drafts):
            dirs.sort()
            paths += [(os.path.join(d, f), args.drafts) for f in sorted(files) if f.endswith('.cpp')]
    for path, root in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        rel = (key_of(path) if root == TREE else os.path.relpath(path, root)).replace(os.sep, '/')
        if '#include "sym/' not in text:
            continue
        syms, pts = set(_SYM.findall(text)), set()
        for m in _POINT.finditer(text):
            pts.add(m.group(1))
        pts.update(re.findall(r'namespace here = points::(\w+);', text))
        regs = set()
        reg_status = {}
        for m in _REG_ANY.finditer(text):
            regs.add(m.group(1) or m.group(2).split('::')[-1])
            reg_status[m.group(1) or m.group(2).split('::')[-1]] = m.group(3)
        for m in _REG_OWN.finditer(text):
            regs.add(m.group(2))
            reg_status[m.group(2)] = m.group(3)
        syms |= regs
        # (game, fn) of the functions only one engine version has: an inline body, or AI's build would emit it
        only = [(m.group(2), m.group(3)) for m in _REG_ONLY.finditer(text)]
        for g, name in only:
            if not re.search(r'\binline\b[^;{}()]*\b%s\s*\(' % name, text):
                raise SystemExit('%s: READABLE_FUNCTION_ONLY(%s, %s): define %s inline in this file, so AI\'s build, which '
                                 'does not register it, emits nothing of it' % (rel, g, name, name))
        own = t.own('dm2')
        # A game folder's source (its instance only): its plain names are its game's own rows, at their own
        # addresses (no bind), else AI's names bound as in a shared source; a function it registers has a body
        # there, at the status it is written with.
        game = folder_of(rel) if root == TREE else None
        mine = t.own(game) if game else None
        own_syms = set(_SYM_OWN.findall(text)) | set(only)
        own_pts = {m.group(1) for m in _POINT_DM2.finditer(text)} | set(re.findall(r'namespace here = points::dm2::(\w+);', text))
        lines = ['#pragma once', '// Generated by tools/readable_symbols.py headers: the symbols %s names, %s.' % (
                 rel, "at AI's addresses" if bind.ai else 'at this game\'s addresses'), '#include <cstdint>']
        if regs or only:
            lines.append('#include "ee/readable.h"')
        for name in sorted(syms):
            # (in a game folder: the game's own row first, else AI's name where the bind puts it, as in a shared source)
            if game and name in mine.syms:
                value = mine.syms[name][1]
            elif name in t.syms:
                value = resolve(bind, t, name)
            else:
                raise SystemExit('%s: sym::%s is in no row of readable_symbols.tsv (readable_symbols.py add%s)' % (
                    rel, name, ' --game ' + game if game else ''))
            lines.append('#ifndef SYM_%s' % name)
            lines.append('#define SYM_%s' % name)
            lines.append('namespace sym { inline constexpr uint32_t %s = 0x%08Xu; }' % (name, value if value is not None else UNBOUND))
            lines.append('#endif')
            if game and name in regs and args.game == game:     # (bind or not: the folder's own addresses)
                lines += ['#ifndef SYM_LEVEL_%s' % name, '#define SYM_LEVEL_%s' % name,
                          'namespace sym::level { inline constexpr int %s = 2; }' % name,
                          'namespace sym::status { inline constexpr auto %s = ::game::readable::Status::%s; }' % (
                              name, reg_status[name]), '#endif']
            elif name in regs and not bind.ai and not game:
                # (apart: a header this source includes may have named the symbol already)
                lines.append('#ifndef SYM_LEVEL_%s' % name)
                lines.append('#define SYM_LEVEL_%s' % name)
                level = levels.level.get(name, 0)
                # --try-close: a corpus build's level 1 bodies are built and registered too (Draft, never on
                # by default), so a replay can say which of them do what this game's code does.
                tried = args.try_close and levels.tried(name)
                lines.append('namespace sym::level { inline constexpr int %s = %d; }' % (name, 2 if tried else level))
                lines.append('namespace sym::status { inline constexpr auto %s = ::game::readable::Status::%s; }' % (
                    name, statuses.get(name, 'Draft')))
                lines.append('#endif')
            count += 1
        for g, name in sorted(own_syms):      # a game's own: its name can be AI's too
            if name not in t.own(g).syms:
                raise SystemExit('%s: sym::%s::%s is in no row of readable_symbols.tsv (readable_symbols.py add --game %s)' % (
                    rel, g, name, g))
            lines.append('#ifndef SYM_%s_%s' % (g, name))
            lines.append('#define SYM_%s_%s' % (g, name))
            lines.append('namespace sym::%s { inline constexpr uint32_t %s = 0x%08Xu; }' % (g, name, t.own(g).syms[name][1]))
            lines.append('#endif')
            count += 1
        for g, name in sorted(only):
            if bind.ai:
                continue    # (AI's build registers none: the macro names no level)
            # its level in its game's instance (2, or 1 when something it names is not bound there); 0 in another
            level = levels.level.get(name, 0) if args.game == g else 0
            tried = level == 1 and args.try_close and levels.tried(name)
            lines += ['#ifndef SYM_LEVEL_%s_%s' % (g, name), '#define SYM_LEVEL_%s_%s' % (g, name),
                      'namespace sym::level::%s { inline constexpr int %s = %d; }' % (g, name, 2 if tried else level),
                      'namespace sym::status::%s { inline constexpr auto %s = ::game::readable::Status::%s; }' % (
                          g, name, statuses.get(name, 'Draft') if args.game == g else 'Draft'), '#endif']
        for function in sorted(pts):
            rows = sorted((a, n) for (f, n), a in (mine or t).points.items() if f == function)
            lines.append('#ifndef POINTS_%s' % function)
            lines.append('#define POINTS_%s' % function)
            for a, n in rows:
                value = a if game else bind.point(a)
                # (an unbound point keeps its own number: a function's resume points are case labels)
                lines.append('namespace points::%s { inline constexpr uint32_t %s = 0x%08Xu; }' % (
                    function, n, value if value is not None else UNBOUND + (a & 0xFFFFFF)))
                count += 1
            lines.append('#endif')
        for function in sorted(own_pts):
            lines.append('#ifndef POINTS_dm2_%s' % function)
            lines.append('#define POINTS_dm2_%s' % function)
            for a, n in sorted((a, n) for (f, n), a in own.points.items() if f == function):
                lines.append('namespace points::dm2::%s { inline constexpr uint32_t %s = 0x%08Xu; }' % (function, n, a))
                count += 1
            lines.append('#endif')
        if write_if_changed(os.path.join(args.out, 'sym', rel + '.h'), '\n'.join(lines) + '\n'):
            written += 1
    # The layout table (`layouts`): checked where the structs are complete, at the end of the source.
    layout = os.path.splitext(args.bind)[0] + '.layout.tsv' if args.bind else None
    if layout and os.path.exists(layout):
        per = collections.defaultdict(list)
        for line in open(layout, encoding='utf-8'):
            rel, struct, field, offset, name = line.rstrip('\n').split('\t')
            per[rel].append('static_assert(offsetof(%s, %s) == %s); // %s' % (struct, field, offset, name))
        for rel, lines in per.items():
            head = '// Generated by tools/readable_symbols.py: the fields %s uses, at the offsets this game\'s machine code uses.' % rel
            write_if_changed(os.path.join(args.out, 'sym', rel + '.layout.h'), '\n'.join([head] + lines) + '\n')
        for rel in args.instance or ():     # (a source can have no field proven in this game: the include stays good)
            if rel not in per:
                write_if_changed(os.path.join(args.out, 'sym', rel + '.layout.h'), '// Generated by tools/readable_symbols.py: no field of %s proven in this game.\n' % rel)
    # What cmake needs: every registration, by game (`game address`).
    if bind.ai:
        own_rows = set()
        for path in game_sources():
            view = literal_view(open(path, encoding='utf-8', errors='replace').read())
            for m in re.finditer(r'^\s*READABLE_(?:FUNCTION|FUNCTION_WAITS|FUNCTION_VU0|PLANNED)\( *(0x[0-9A-Fa-f]+)', view, re.M):
                own_rows.add('ai\t0x%x\n' % int(m.group(1), 16))
            for m in re.finditer(r'READABLE_(?:FUNCTION|FUNCTION_WAITS|FUNCTION_VU0|PLANNED)_IN\( *(\w+) *, *(0x[0-9A-Fa-f]+)', view):
                own_rows.add('%s\t0x%x\n' % (m.group(1), int(m.group(2), 16)))
        write_if_changed(os.path.join(args.out, 'registered.tsv'), ''.join(sorted(own_rows)))
    # What cmake needs: this game's registrations.
    rows = []
    for name, level in sorted(levels.level.items()):
        if args.instance and levels.files.get(name) not in args.instance:
            continue
        address = levels.address(name)
        rows.append('%s\t0x%08X\t%d\t%s\n' % (name, address or 0, level, levels.why.get(name, '')))
    write_if_changed(os.path.join(args.out, 'levels.tsv'), ''.join(rows))
    print('%s: %d headers written, %d constants; %s' % (
        'ai' if bind.ai else args.game, written, count,
        dict(collections.Counter(levels.level.values()))))


# ---- generate -----------------------------------------------------------------

KEYWORD = {'new', 'delete', 'default', 'switch', 'case', 'register', 'class', 'template', 'this', 'operator',
           'union', 'struct', 'enum', 'for', 'while', 'do', 'if', 'else', 'return', 'break', 'continue', 'goto'}


def lower_first(constant):
    return constant[1].lower() + constant[2:]


def usable(name):
    return bool(name) and re.fullmatch(r'[A-Za-z_]\w*', name) is not None and not MACHINE.match(name) and name not in KEYWORD


def rows_from_workdb(db):
    """(functions, data): {address: [names, best first]} from the database."""
    registered = dict(db.execute("SELECT addr, name FROM readable").fetchall())
    names_table = db.execute("SELECT addr, name, kind, confidence FROM names "
                             "WHERE build = 'ai_ps2' AND source = 'readable_names'").fetchall()
    best = dict(db.execute("SELECT addr, name FROM best_names WHERE build = 'ai_ps2'").fetchall())
    symbols = db.execute("SELECT addr, name, kind FROM readable_symbols WHERE file NOT LIKE 'dm2/%' "
                         "ORDER BY file, line").fetchall()
    listed = db.execute("SELECT addr, name FROM functions WHERE build = 'ai_ps2' ORDER BY addr").fetchall()
    functions = [r[0] for r in listed]
    named = {a: n for a, n, k, c in names_table if k == 'function'}
    constants = collections.defaultdict(list)
    data = collections.defaultdict(list)
    for a, n, k in symbols:
        if k == 'global':
            data[a].append((0, n))
        elif n.startswith('k') and len(n) > 1 and n[1].isupper():
            constants[a].append(lower_first(n))
            data[a].append((2, lower_first(n)))
    for a, n, k, c in names_table:
        if k == 'global':
            data[a].append((3 if c == 'low' else 1, n))
        elif k in ('part', 'datum'):
            data[a].append((-1, n))
    fn = {}
    for a in functions:
        fn[a] = [n.split('::')[-1] for n in (registered.get(a), named.get(a)) if n] + constants[a] + (
            [best[a]] if a in best else [])
    for a, n in listed:
        fn[a].append(n)
    for a, n in named.items():      # (a function Ghidra's list runs into the one before)
        fn.setdefault(a, [n])
    return fn, {a: [n for _, n in sorted(v, key=lambda x: x[0])] for a, v in data.items()}


def uses(text, t):
    """(names of the sym:: rows, addresses of the points) a source names."""
    syms = {n for n in _SYM.findall(text) if n in t.syms}
    for m in _REG_ANY.finditer(text):
        name = m.group(1) or m.group(2).split('::')[-1]
        if name in t.syms:
            syms.add(name)
    for m in _REG_OWN.finditer(text):
        if m.group(2) in t.syms:
            syms.add(m.group(2))
    points = {t.points[k] for k in _POINT.findall(text) if k in t.points}
    current = None
    events = sorted([(m.start(), m.group(1), True) for m in re.finditer(r'namespace here = points::((?:dm2::)?\w+);', text)] +
                    [(m.start(), m.group(1), False) for m in _HERE.finditer(text)])
    for _, value, alias in events:
        if alias:
            current = None if value.startswith('dm2::') else value
        elif (current, value) in t.points:
            points.add(t.points[(current, value)])
    return syms, points


def cmd_generate(args):
    import duckdb
    if args.drafts:         # (what the drafter named and the table has not got yet)
        os.environ['READABLE_SYMBOLS_EXTRA'] = os.path.join(args.drafts, EXTRA_ROWS)
    old = table()
    db = duckdb.connect(args.workdb, read_only=True)
    new = Table(None)
    facts = Facts(args.elf, args.csv, new)   # (callees are named as the new table names them)
    fn, data = rows_from_workdb(db)
    paths = list(game_sources())
    if args.drafts:
        for d, dirs, files in os.walk(args.drafts):
            dirs.sort()
            paths += [os.path.join(d, f) for f in sorted(files) if f.endswith('.cpp')]
    used_names, used_points = set(), set()
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        if folder_game(text):   # (its plain names are its own rows, which are kept whole below)
            continue
        names, points = uses(text, old)
        used_names |= names
        used_points |= points
    # A name a source uses stays (`rename` changes one): the rest is the database's.
    for name in sorted(used_names):
        new.put(old.syms[name][0], name, old.syms[name][1])
    for address in sorted(fn):
        names = [n for n in fn[address] if usable(n)]
        if names and ('function', address) not in new.by_address:
            new.put('function', clash_free(new, names[0], 'function', address), address)
    for address in sorted(data):
        parts = [n for n in data[address] if '+' in n]
        if parts:                       # readable_names.tsv's `name+offset`: part of a named object
            new.put('data', parts[0], address)
            continue
        names = [n for n in data[address] if usable(n)]
        if not names or not address & 0xFFFF:
            continue
        kind = facts.kind(address)
        if kind == 'data' and ('data', address) not in new.by_address:
            new.put('data', clash_free(new, names[0], 'data', address), address)
        elif kind == 'point' and facts.function_at(address) is None and ('function', address) not in new.by_address:
            new.put('function', clash_free(new, names[0], 'function', address), address)   # (code no function list has)
    known = {(n, a) for a, ns in fn.items() for n in ns} | {(n, a) for a, ns in data.items() for n in ns}
    strings, pending = 0, []
    for name in sorted(used_names):
        if (name, old.syms[name][1]) in known:
            continue
        if old.syms[name][0] == 'data' and facts.text_name(old.syms[name][1]):
            strings += 1
        else:
            pending.append(name)
    for name, (kind, address) in new.syms.items():
        if '+' in name:     # a part's base must be where the row says
            base, offset = name.split('+')
            if base not in new.syms or new.syms[base][1] + int(offset, 16) != address:
                raise SystemExit('generate: %s is not at 0x%X' % (name, address))
    for address in sorted(used_points):
        function = facts.function_at(address)
        fname = new.by_address.get(('function', function))
        name = facts.point_names(function).get(address) if fname else None
        if name:
            new.put('point', name, address, fname)
    # DM2's rows: games/dm2's own constants (`convert`), and the database's names of its functions.
    for game, rows in old.games.items():
        new.games[game] = rows
    own = new.own('dm2')
    dm2_names = db.execute("SELECT addr, name FROM best_names WHERE build = 'dm2_ps2' UNION ALL "
                           "SELECT addr, name FROM func_map WHERE build = 'dm2_ps2' UNION ALL "
                           "SELECT c.addr, b.name FROM counterparts c JOIN best_names b ON b.build = 'ai_ps2' AND b.addr = c.ai_addr "
                           "WHERE c.build = 'dm2_ps2'").fetchall()
    for address, name in sorted(dm2_names, key=lambda r: r[0]):
        name = name.split('::')[-1] if name else name
        if usable(name) and ('function', address) not in own.by_address:
            own.put('function', clash_free(own, name, 'function', address), address)
    if args.check:
        have, want = set(old.rows()), set(new.rows())
        for r in sorted(have - want)[:10]:
            print('  table:    ', r)
        for r in sorted(want - have)[:10]:
            print('  generated:', r)
        print('generate: %d rows only in the table, %d only generated, of %d' % (len(have - want), len(want - have), len(want)))
        return 1 if have != want else 0
    new.path = args.out or TABLE
    new.save()
    print('generate: %d functions and data (%d strings), %d points, %d of DM2; %d the work database does not know yet' % (
        len(new.syms), strings, len(new.points), sum(len(g.syms) + len(g.points) for g in new.games.values()), len(pending)))
    for name in sorted(pending)[:40]:
        print('  not in the database yet:', name)
    return 0


# ---- layouts ------------------------------------------------------------------

_MEMBER = re.compile(r'^\s*[\w:<>\s*]+?\b(\w+)(?:\[[^\]]*\])?\s*;\s*//\s*\+(0x[0-9A-Fa-f]+)\b', re.M)
_STRUCT = re.compile(r'\bstruct\s+(\w+)\s*\{')
_FIELD_USE = re.compile(r'(?:->|\.)\s*(\w+)\b')


def struct_fields(text):
    """{struct: {field: offset}} from a source's `field; // +0xNN` lines."""
    sys.path.insert(0, HERE)
    import readable_digest
    code = blank(text)
    out = {}
    for m in _STRUCT.finditer(code):
        end = readable_digest.match(code, m.end() - 1, '{', '}')
        if end < 0:
            continue
        body = text[m.end():end]
        fields = {f.group(1): int(f.group(2), 16) for f in _MEMBER.finditer(body)}
        if fields:
            out[m.group(1)] = fields
    return out


def displacements(elf, start, count):
    """The displacements a function's own loads and stores use off a register (not gp, not a
    lui's low half), from its machine code."""
    hi = set()
    seen = set()
    for i in range(count):
        w = elf.word(start + i * 4)
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15:
            hi.add(rt)
            continue
        if op in LOADS_STORES and rs not in hi and rs != 28 and rs != 29:
            seen.add(signed(w & 0xFFFF))
        if (op in (9, 13) or op in LOADS_STORES and op < 0x28) and rt in hi:
            hi.discard(rt)
    return seen


def cmd_layouts(args):
    """A field is the same in a game when a function that runs there uses it: the row's offset
    comes from the game's own machine code, so a struct changed for one game fails the
    static_assert in the other game's build."""
    sys.path.insert(0, HERE)
    import readable_digest
    t = table()
    bind = Bind(args.bind)
    levels = Levels(bind, t, TREE)
    elf = Elf(args.elf_game)
    insns = {g: (end - ai) // 4 for ai, end, g in bind.bodies}
    rows = []
    counts = collections.Counter()
    headers = {}    # (each header's structs read once: a later header's struct of the same name wins, as in one text)
    for path in game_sources():
        text = open(path, encoding='utf-8', errors='replace').read()
        if '#include "sym/' not in text or path.endswith('.h'):
            continue
        rel = key_of(path).replace(os.sep, '/')
        included = {}
        for h in re.findall(r'^#include "([^"]+)"', text, re.M):
            for base in (os.path.dirname(path),) + tuple(os.path.join(TREE, r) for r in zaxis_paths.zlayout.READABLE_ROOTS):
                header = os.path.join(base, h)
                if os.path.exists(header):
                    if header not in headers:
                        headers[header] = struct_fields(open(header, encoding='utf-8', errors='replace').read())
                    included.update(headers[header])
                    break
        fields = struct_fields(text)
        for name, value in included.items():
            fields.setdefault(name, value)
        namespaced = {}
        ns = 'zge' if rel.startswith('zge/') else 'sce'
        for struct, members in fields.items():
            namespaced[struct] = members
        code = readable_digest.strip(text)
        defs = readable_digest.definitions(code)
        checks = {}
        for name, short, _, _ in registrations_in(code, args.game):
            if levels.level.get(name) != 2 or name not in t.syms:
                continue
            address = bind.function(t.syms[name][1])
            own = displacements(elf, address, insns.get(address, 0))
            for d in defs.get(short, []):
                used = set(_FIELD_USE.findall(d))
                for struct, members in namespaced.items():
                    for field, offset in members.items():
                        if field in used:
                            if offset in own:
                                checks[(struct, field, offset)] = name
                            else:
                                counts['not seen in the machine code'] += 1
        for (struct, field, offset), name in sorted(checks.items()):
            rows.append('%s\t%s::%s\t%s\t0x%X\t%s\n' % (rel, ns, struct, field, offset, name))
            counts['proven'] += 1
    write_if_changed(args.out, ''.join(rows))
    print('%s: %s' % (args.game, dict(counts)))


# ---- bind: the committed tables ---------------------------------------------------

# Each game's ELF (paths.env's workspaces): the table's and the layouts' other input beside the work database.
GAME_ELVES = {'dm2': ('DM2_WORKSPACE', 'recomp/disc-root/SLUS_201.59'), 'bmx': ('BMX_DATA', 'disc/SLUS_204.15')}
SCRATCH = zaxis_paths.get('ZAXIS_SCRATCH', required=False) or ''   # (paths.env's; none on a worker with no tooling)
VENV_PY = os.path.join(SCRATCH, 'venv', 'bin', 'python')     # (the scratch venv: duckdb)


def table_rows(path):
    """A table's rows, its comment lines (which name the database build it was made from) left out; None: no file."""
    if not os.path.exists(path):
        return None
    return [line for line in open(path, encoding='utf-8') if not line.startswith('#')]


def cmd_bind(args):
    """Each game's table and layouts are a function of the commit: made from the work database, the two ELFs and this
    tree's sources, they must be the committed tools/bind/ files, which every build of the commit uses (game.cmake). A
    work database built since with other pairs, or a source change of a level-2 function's fields, makes them stale."""
    try:
        import duckdb  # noqa: F401
    except ImportError:
        if os.path.exists(VENV_PY) and os.path.realpath(sys.prefix) != os.path.realpath(os.path.dirname(os.path.dirname(VENV_PY))):
            os.execv(VENV_PY, [VENV_PY, '-I', os.path.abspath(__file__)] + sys.argv[1:])
        sys.exit('bind: needs the duckdb module (%s)' % VENV_PY)
    import shutil
    import tempfile
    if not os.path.exists(args.workdb):
        sys.exit('bind: no work database %s' % args.workdb)
    out = args.dir or BIND
    work = tempfile.mkdtemp(prefix='readable_bind')
    stale = []
    try:
        for game in args.game or list(GAME_ELVES):
            root, rel = GAME_ELVES[game]
            elf = dict(x.split('=', 1) for x in args.elf_game or []).get(game) or os.path.join(zaxis_paths.get(root, required=False) or '', rel)
            if not os.path.exists(elf):
                sys.exit('bind: no %s ELF %s' % (game, elf))
            made = os.path.join(work, game + '.tsv')
            build_table(argparse.Namespace(game=game, workdb=args.workdb, elf_ai=args.elf_ai, elf_game=elf, out=made,
                                            conflicts=None))
            cmd_layouts(argparse.Namespace(out=os.path.join(work, game + '.layout.tsv'), game=game, bind=made,
                                            elf_game=elf))
            for name in (game + '.tsv', game + '.layout.tsv'):
                committed, fresh = os.path.join(out, name), os.path.join(work, name)
                have, want = table_rows(committed), table_rows(fresh)
                if have == want:
                    print('bind: %s is what the database makes (%d rows)' % (name, len(want)))
                    continue
                diff = [l for l in difflib.unified_diff(have or [], want, n=0) if l[:1] in '+-' and l[:3] not in ('+++', '---')]
                if args.write:
                    os.makedirs(out, exist_ok=True)
                    shutil.copyfile(fresh, committed)
                    print('bind: %s written (%d lines changed)' % (name, len(diff)))
                    continue
                stale.append(name)
                print('bind: %s is stale: %s' % (name, 'not committed' if have is None else
                                                 '%d lines differ from what the database makes' % len(diff)))
                for line in diff[:args.show]:
                    print('  ' + line.rstrip('\n'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if stale:
        root = os.path.dirname(os.path.dirname(HERE))
        print('bind: STALE %s: `readable_symbols.py bind --write`, then commit %s' % (
            ', '.join(stale), os.path.relpath(out, root) if out.startswith(root + os.sep) else out))
        return 1
    return 0


# ---- lint ---------------------------------------------------------------------

MACHINE_NAME = re.compile(r'\b(?:kFun|kFn|fn_|FUN_|kData|DAT_|LAB_)(?:00)?[0-9A-Fa-f]{6}\b')
NOT_ADDRESS = 'not an address'


def is_image(value):
    return IMAGE_LOW <= value < IMAGE_HIGH or IMAGE_LOW <= value - 0x20000000 < IMAGE_HIGH


def literals(text):
    """(line, token, value) of each address literal or machine name in the code of a source:
    comments and strings are not code."""
    code = blank(text)
    lines = text.split('\n')
    found = []
    for n, line in enumerate(code.split('\n')):
        if NOT_ADDRESS in lines[n]:
            continue
        for m in re.finditer(r'\b(0[xX][0-9a-fA-F]+|[1-9][0-9]{6,8})[uUlL]*\b', line):
            value = int(m.group(1), 0)
            if is_image(value):
                found.append((n + 1, m.group(0), value))
        for m in MACHINE_NAME.finditer(line):
            found.append((n + 1, m.group(0), 0))
    return found


def comment_references(text):
    """(line, token) of each FUN_-style reference in a comment."""
    code = blank(text)
    found = []
    for n, (line, kept) in enumerate(zip(text.split('\n'), code.split('\n'))):
        for m in COMMENT_REF.finditer(line):
            if m.start() < len(kept) and kept[m.start()] == ' ':
                found.append((n + 1, m.group(0)))
    return found


def lint_paths(drafts=None):
    for path in game_sources():
        yield path, key_of(path).replace(os.sep, '/')
    if drafts:
        for d, dirs, files in os.walk(drafts):
            dirs.sort()
            for f in sorted(files):
                if f.endswith('.cpp'):
                    path = os.path.join(d, f)
                    yield path, 'drafts/' + os.path.relpath(path, drafts).replace(os.sep, '/')


def cmd_lint(args):
    code, comments, conditional, files = 0, 0, 0, 0
    for path, rel in lint_paths(args.drafts):
        text = open(path, encoding='utf-8', errors='replace').read()
        found, refs = literals(text), comment_references(text)
        held = conditional_includes(text)
        for n, line in held:
            print('%s:%d: %s under a preprocessor conditional: readable_symbols.py convert moves it out' % (rel, n, line.strip()))
        code += len(found)
        conditional += len(held)
        comments += len(refs)
        files += bool(found or refs or held)
        for n, token, _ in found if not args.quiet else []:
            print('%s:%d: %s: an address in code: name it (readable_symbols.py add, docs/READABLE.md "Symbols")' % (rel, n, token))
        for n, token in refs if not (args.quiet or args.quiet_comments) else []:
            print('%s:%d: %s: a machine reference in a comment: write the function\'s name' % (rel, n, token))
    print('lint: %d literals in code, %d machine references in comments, %d conditional includes, in %d files' % (code, comments, conditional, files))
    return 1 if code or conditional or (comments and not args.quiet_comments) else 0


def cmd_census(args):
    facts = Facts(args.elf, args.csv, table())
    per = collections.Counter()
    kinds = collections.defaultdict(set)
    named = collections.Counter()
    context = collections.Counter()
    files = list(game_sources())
    if args.drafts:
        for d, dirs, fs in os.walk(args.drafts):
            dirs.sort()
            files += [os.path.join(d, f) for f in sorted(fs) if f.endswith('.cpp')]
    for path in files:
        layer = layer_of(path) if zaxis_paths.is_readable(path) else 'drafts:' + os.path.relpath(path, args.drafts).split(os.sep)[0]
        text = open(path, encoding='utf-8', errors='replace').read()
        code = blank(text)
        for m in LITERAL.finditer(code):
            address = int(m.group(1), 16)
            if not IMAGE_LOW <= address < IMAGE_HIGH:
                continue
            line_start = code.rfind('\n', 0, m.start()) + 1
            pre = code[line_start:m.start()]
            kind = facts.kind(address) or 'not in the ELF'
            if re.search(r'READABLE_\w+\(\s*$', pre):
                use = 'registration'
            elif GLOBAL_DECL.search(pre):
                use = 'Global<>'
            elif MARKS.search(pre):
                use = 'mark'
            elif 'calleeSaves' in pre or 'passRegisters' in pre:
                use = 'return address'
            elif CONSTEXPR.search(pre):
                use = 'constant'
            else:
                use = 'in code'
            per[(layer, kind, use)] += 1
            kinds[kind].add(address)
            if kind == 'function' and not facts.function_name(address):
                named['function without a name'] += 1
    for (layer, kind, use), n in sorted(per.items()):
        print('%-16s %-16s %-16s %6d' % (layer, kind, use, n))
    print({k: len(v) for k, v in kinds.items()}, dict(named))
    if args.json:
        json.dump({'|'.join(k): v for k, v in per.items()}, open(args.json, 'w'))


def cmd_rename(args):
    """A symbol's new name, in the table and wherever a source says sym::OLD or points::OLD."""
    t = table()
    renamed = set()
    for tbl in [t] + list(t.games.values()):
        if args.old in tbl.syms or any(function == args.old for function, _ in tbl.points):
            renamed.add(tbl.game)
        if args.old in tbl.syms:
            kind, address = tbl.syms.pop(args.old)
            tbl.by_address.pop((kind, address), None)
            tbl.put(kind, args.new, address)
        for (function, name), address in list(tbl.points.items()):
            if function == args.old:
                del tbl.points[(function, name)]
                del tbl.by_address[(function, address)]
                tbl.put('point', name, address, args.new)
    t.save()
    pattern = re.compile(r'\b((?:sym|points)(?:::dm2)?)::%s\b' % re.escape(args.old))
    changed = 0
    for path in game_sources():
        text = open(path, encoding='utf-8').read()
        # (a plain name is the folder's game's row in a game folder, AI's elsewhere; sym::dm2:: is DM2's)
        plain = folder_of(key_of(path)) or ''

        def one(m):
            game = 'dm2' if m.group(1).endswith('::dm2') else plain
            return '%s::%s' % (m.group(1), args.new) if game in renamed else m.group(0)
        new = pattern.sub(one, text)
        if new != text:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(new)
            changed += 1
    print('%s is %s now, in %d sources' % (args.old, args.new, changed))


def row_key(row):
    """A table row's place in Table.save's order: game, points last, a point's function, address, name."""
    kind, name, address, function, game = row
    return (game, kind == 'point', function, int(address, 16), name)


def add_row(path, kind, name, address, function='', game=''):
    """One row of the table at path written, every other line left as it is: the row of the same symbol (a name of
    the game's, a point by its function and name) in its place, else a new line after the last row of its game and
    kind (point or not) that save's order puts before it (before the first of them if none does; at the end if the
    game has no such row yet). The file stays in the order it is in (rows added by hand, out of save's order, stay):
    a lane's add diffs one line. Returns 'same', 'replaced' or 'added'."""
    function = function if kind == 'point' else ''
    game = '' if game in ('', 'ai') else game
    new = (kind, name, '0x%08X' % address, function, game)
    lines = open(path, encoding='utf-8').read().splitlines(keepends=True)
    rows = []      # (line index, row)
    for i, line in enumerate(lines):
        if not line.strip() or line.startswith('#'):
            continue
        rows.append((i, tuple((line.rstrip('\n').split('\t') + ['', ''])[:5])))
    text = '\t'.join(new) + '\n'

    def same(r):
        if r[4] != game or (r[0] == 'point') != (kind == 'point'):
            return False
        return (r[3], r[1]) == (function, name) if kind == 'point' else r[1] == name
    for i, r in rows:
        if same(r):
            if lines[i] == text:
                return 'same'
            lines[i] = text
            break
    else:
        cls = [(i, r) for i, r in rows if r[4] == game and (r[0] == 'point') == (kind == 'point')]
        before = [i for i, r in cls if row_key(r) <= row_key(new)]
        at = max(before) + 1 if before else (cls[0][0] if cls else len(lines))
        if at == len(lines) and lines and not lines[-1].endswith('\n'):
            lines[-1] += '\n'
        lines.insert(at, text)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(''.join(lines))
    return 'replaced' if any(same(r) for _, r in rows) else 'added'


def cmd_add(args):
    t = table()
    print('%s: %s' % (os.path.relpath(t.path), add_row(t.path, args.kind, args.name, int(args.address, 16),
                                                       args.function or '', args.game)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='command', required=True)
    ai, dm2 = zaxis_paths.get('AI_WORKSPACE', required=False) or '', zaxis_paths.get('DM2_WORKSPACE', required=False) or ''
    defaults = {'elf': os.path.join(ai, 'recomp/disc-root/SLUS_203.27'),
                'csv': os.path.join(ai, 'recomp/ghidra-functions.csv'),
                'elf_dm2': os.path.join(dm2, 'recomp/disc-root/SLUS_201.59'),
                'csv_dm2': os.path.join(dm2, 'recomp/ghidra-functions.csv')}
    q = s.add_parser('table')
    q.add_argument('--game', required=True)
    q.add_argument('--workdb', default=os.path.join(SCRATCH, 'workdb', 'work.duckdb'))
    q.add_argument('--elf-ai', default=defaults['elf'])
    q.add_argument('--elf-game', required=True)
    q.add_argument('--out', required=True)
    q.add_argument('--conflicts')
    q.set_defaults(fn=build_table)
    q = s.add_parser('bind')
    w = q.add_mutually_exclusive_group()
    w.add_argument('--check', action='store_true', help='exit 1 when a committed table differs (the default)')
    w.add_argument('--write', action='store_true', help='write the tables that differ into tools/bind')
    q.add_argument('--game', action='append', choices=sorted(GAME_ELVES))
    q.add_argument('--workdb', default=os.environ.get('WORKDB') or os.path.join(SCRATCH, 'workdb', 'work.duckdb'))
    q.add_argument('--elf-ai', default=defaults['elf'])
    q.add_argument('--elf-game', action='append', help='GAME=ELF (default paths.env\'s workspaces)')
    q.add_argument('--dir', help='the tables to check or write (default: this tree\'s tools/bind)')
    q.add_argument('--show', type=int, default=20, help='the lines of a stale table\'s difference shown')
    q.set_defaults(fn=cmd_bind)
    q = s.add_parser('headers')
    q.add_argument('--out', required=True)
    q.add_argument('--game', default='ai')
    q.add_argument('--bind')
    q.add_argument('--status')
    q.add_argument('--drafts', help='the sweep\'s generated batch files, written with symbols')
    q.add_argument('--instance', nargs='*', help='the sources this game has an instance of: levels.tsv lists theirs')
    q.add_argument('--try-close', action='store_true', help='level 1 bodies registered too, for a replay to judge')
    q.set_defaults(fn=cmd_headers)
    q = s.add_parser('layouts')
    q.add_argument('--out', required=True)
    q.add_argument('--game', required=True)
    q.add_argument('--bind', required=True)
    q.add_argument('--elf-game', required=True)
    q.set_defaults(fn=cmd_layouts)
    q = s.add_parser('convert')
    q.add_argument('files', nargs='+')
    q.add_argument('--root')
    q.add_argument('--elf', default=defaults['elf'])
    q.add_argument('--csv', default=defaults['csv'])
    q.add_argument('--elf-dm2', default=defaults['elf_dm2'])
    q.add_argument('--csv-dm2', default=defaults['csv_dm2'])
    q.add_argument('--out-table')
    q.set_defaults(fn=cmd_convert)
    q = s.add_parser('comments')
    q.add_argument('files', nargs='+')
    q.set_defaults(fn=cmd_comments)
    q = s.add_parser('generate')
    q.add_argument('--workdb', default=os.path.join(SCRATCH, 'workdb', 'work.duckdb'))
    q.add_argument('--elf', default=defaults['elf'])
    q.add_argument('--csv', default=defaults['csv'])
    q.add_argument('--drafts', help='the sweep\'s generated batch files')
    q.add_argument('--out')
    q.add_argument('--check', action='store_true', help='the committed table is what the database gives')
    q.set_defaults(fn=cmd_generate)
    q = s.add_parser('lint')
    q.add_argument('--drafts')
    q.add_argument('--quiet', action='store_true')
    q.add_argument('--quiet-comments', action='store_true', help='count comments, do not fail on them (until they are done)')
    q.set_defaults(fn=cmd_lint)
    q = s.add_parser('census')
    q.add_argument('--elf', default=defaults['elf'])
    q.add_argument('--csv', default=defaults['csv'])
    q.add_argument('--drafts')
    q.add_argument('--json')
    q.set_defaults(fn=cmd_census)
    q = s.add_parser('view')
    q.add_argument('file')
    q.set_defaults(fn=lambda a: sys.stdout.write(literal_view(open(a.file, encoding='utf-8').read())))
    q = s.add_parser('rename')
    q.add_argument('old')
    q.add_argument('new')
    q.set_defaults(fn=cmd_rename)
    q = s.add_parser('add')
    q.add_argument('kind', choices=['function', 'data', 'point'])
    q.add_argument('name')
    q.add_argument('address')
    q.add_argument('function', nargs='?')
    q.add_argument('--game', default='')
    q.set_defaults(fn=cmd_add)
    args = p.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == '__main__':
    main()
