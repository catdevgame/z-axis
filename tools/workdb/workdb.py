#!/usr/bin/env python3
"""The work database: one DuckDB file with everything known about Aggressive Inline's PS2
functions across the nine builds of Z-Axis's engine (AI, Dave Mirra BMX 2 and BMX XXX on PS2,
Xbox and GameCube), for the readable rewrite's sweep and the readable passes (MATCH3.md
section 6).

    python -I tools/workdb.py build [--only LAYER,...] [--base-exports]
                                                            rebuild the database from its sources
                                                            (--base-exports: the Xbox and GameCube
                                                            exports as analysed, not the readable ones)
    python -I tools/workdb.py query "SQL" [--csv|--json]   read-only query
    python -I tools/workdb.py card ADDR|NAME [--build B] [--json] [--prefix '// '] [--width 100]
                                                            an AI function's card (a DM2 or BMX XXX name of a
                                                            function with no AI counterpart: that build's card;
                                                            --build: ADDR is that PS2 build's address)
    python -I tools/workdb.py tables                        tables and views with row counts
    python -I tools/workdb.py schema [TABLE]                columns
    python -I tools/workdb.py notes [TOPIC|ADDR|NAME] [--build B] [--json]
                                                            the knowledge notes of a topic or a function
                                                            (no argument: counts per topic)
    python -I tools/workdb.py emit FILE KIND key=value ...  append one sweep result to a JSONL file
                                                            (KIND note: a fact about a function, see NOTE_TOPICS)
    python -I tools/workdb.py check-results [FILE ...]      validate result files without a build

(`python` = $ZAXIS_SCRATCH/venv/bin/python, which has the duckdb module; the script
re-runs itself with it when started by another Python.)

The database is a cache: `build` makes it from the sources below, nothing edits it by hand, and
everyone but `build` opens it read-only. It is written to a temporary file and renamed over the
old one, so readers never see half a database; a lock file keeps two builds apart. Sweep jobs
and agents never write the database: they append results to their own JSONL files under
<workdb>/results/ (`emit`, or any writer of the same lines), and the next build ingests them.

Where things are (environment overrides): WORKDB_DIR ($ZAXIS_SCRATCH/workdb: the
database work.duckdb, results/, the new builds' Ghidra outputs), AI_SCRATCH
($ZAXIS_SCRATCH), AI_REPO (AI_WORKSPACE in tools/paths.env), DM2_REPO
(DM2_WORKSPACE), WORKDB_GAME_SRC (the readable sources: a z-axis checkout, its readable folders
(tools/checks/zlayout.py); point it at a worktree to index that one),
WORKDB_SOURCES (a JSON file of {source name: path} replacing any default path; `tables`
lists the names). A missing source is recorded in `sources` and its layer left empty.

Everything in the database is derived from the games' discs: it stays in scratch, never in git. Its notes, text
only, are exported to workdb_notes.jsonl beside this script (`export-notes`, the workshop's step), and `card` and
`notes` read that export when there is no database or no duckdb module (a public clone), or with WORKDB_PUBLIC=1:
tools/workdb_notes.py.

    python -I tools/workdb.py export-notes [--out FILE] [--rules FILE ...] [--check]
                                                            write the public notes export (--rules: public_scan's
                                                            private rules, default the workshop's; --check: exit 1
                                                            when FILE is not what the database makes)
"""
import collections, csv, datetime, fcntl, glob, json, os, re, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env)
VENV_PY = os.path.join(zpaths.ZAXIS_SCRATCH, 'venv', 'bin', 'python')
try:
    import duckdb
except ImportError:
    if os.path.exists(VENV_PY) and os.path.realpath(sys.prefix) != \
            os.path.realpath(os.path.dirname(os.path.dirname(VENV_PY))):
        os.execv(VENV_PY, [VENV_PY, '-I', os.path.abspath(__file__)] + sys.argv[1:])
    duckdb = None   # (card and notes read the public notes export, workdb_notes.py; the rest needs the module)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

HOME = os.path.expanduser('~')
SCRATCH = os.environ.get('AI_SCRATCH', zpaths.ZAXIS_SCRATCH)
WORKDB = os.environ.get('WORKDB_DIR', SCRATCH + '/workdb')
DB = os.environ.get('WORKDB', WORKDB + '/work.duckdb')
AI_REPO = os.environ.get('AI_REPO', zpaths.AI_WORKSPACE)
DM2_REPO = os.environ.get('DM2_REPO', zpaths.DM2_WORKSPACE)
ZAXIS = os.path.dirname(os.path.dirname(os.path.dirname(AI_REPO)))  # the z-axis checkout the AI workspace sits in
GAME_SRC = os.environ.get('WORKDB_GAME_SRC', ZAXIS)   # (a tree: its readable code by keys, zpaths.zlayout)
M3 = SCRATCH + '/match3'
VERSION = 4  # the schema; bump when a table changes shape (3: digests, coverage, readable_digests; 4: func_notes)

# ------------------------------------------------------------------ sources

SOURCES = {
    # PS2 builds: the ELF copies, Ghidra function maps and SCE scans of MATCH3.md
    'ai_ps2.exe': M3 + '/elf/SLUS_203.27',
    'ai_ps2.csv': AI_REPO + '/recomp/ghidra-functions.csv',
    'ai_ps2.sce': DM2_REPO + '/ghidra-ai/sce-db/sce-matches.csv',
    'dm2_ps2.exe': M3 + '/elf/SLUS_201.59',
    'dm2_ps2.csv': DM2_REPO + '/recomp/ghidra-functions.csv',
    'dm2_ps2.sce': DM2_REPO + '/ghidra/sce-db/sce-matches.csv',
    'bmx_ps2.exe': M3 + '/elf/SLUS_204.15',
    'bmx_ps2.csv': M3 + '/ghidra-bmxxxx/out/ghidra-functions.csv',
    'bmx_ps2.sce': M3 + '/ghidra-bmxxxx/sce-db/sce-matches.csv',
    # Xbox builds: XBE copies, xbe_pipeline.sh exports, xmatch.py pairings, __FILE__ uses
    'ai_xbox.exe': SCRATCH + '/aixbox/xbe/ai.xbe',
    'ai_xbox.jsonl': SCRATCH + '/aixbox/ghidra-aixbox/out/AiXbox-xbe.jsonl',
    'ai_xbox.pair': SCRATCH + '/aixbox/xmatch.json',
    'ai_xbox.paths': M3 + '/xbe/ai-paths.json',
    'dm2_xbox.exe': SCRATCH + '/dm2xbox-ghidra/xbe/dm2.xbe',
    'dm2_xbox.jsonl': SCRATCH + '/dm2xbox-ghidra/ghidra/out/Dm2Xbox-xbe.jsonl',
    'dm2_xbox.pair': WORKDB + '/dm2xbox/xmatch.json',
    'dm2_xbox.paths': M3 + '/xbe/dm2-paths.json',
    'bmx_xbox.exe': WORKDB + '/bmxxbox/xbe/bmxxxx.xbe',
    'bmx_xbox.jsonl': WORKDB + '/bmxxbox/ghidra/out/BmxXbox-xbe.jsonl',
    'bmx_xbox.pair': WORKDB + '/bmxxbox/xmatch.json',
    'bmx_xbox.paths': M3 + '/xbe/bmxxxx-paths.json',
    # GameCube builds: DOL copies, gc_pipeline.sh exports, gcmatch.py pairings
    'ai_gc.exe': WORKDB + '/gc/dol/ai.dol',
    'ai_gc.jsonl': WORKDB + '/gc/ghidra-ai/out/AiGc-gc.jsonl',
    'ai_gc.pair': WORKDB + '/gc/ai-gcmatch.json',
    'dm2_gc.exe': WORKDB + '/gc/dol/dm2.dol',
    'dm2_gc.jsonl': WORKDB + '/gc/ghidra-dm2/out/Dm2Gc-gc.jsonl',
    'dm2_gc.pair': WORKDB + '/gc/dm2-gcmatch.json',
    'bmx_gc.exe': WORKDB + '/gc/dol/bmx.dol',
    'bmx_gc.jsonl': WORKDB + '/gc/ghidra-bmx/out/BmxGc-gc.jsonl',
    'bmx_gc.pair': WORKDB + '/gc/bmx-gcmatch.json',
    # the PS2 three-way match and AI's module map
    'match3': M3 + '/match3.json',
    'modmap': M3 + '/modmap.json',
    # the readable sources and the recording corpus
    'game_src': GAME_SRC,
    # the readable functions' source digests (tools/readable/readable_digest.py, run on game_src)
    'readable_digest': os.environ.get('WORKDB_READABLE_DIGEST',
                                      os.path.join(GAME_SRC, 'tools', 'readable', 'readable_digest.py')),
    'corpus': SCRATCH + '/reccorpus',
    # a ZX_FNPROF profile (calls and EE self time per function; the tracking page sizes by it)
    'profile': AI_REPO + '/recomp/traces/live-fnprof.csv',
    # string tables (key = name hash) and where KSTR names are spelled out
    'strings.ai_ps2': AI_REPO + '/recomp/disc-root/ASSETS/STRING.EUS',
    'strings.dm2_ps2': DM2_REPO + '/recomp/disc-root/ASSETS/STRING.EUS',
    'strings.ai_xbox': AI_REPO + '/dist/.xbox-cache/media/assets/string.eus',
    'strings.dm2_xbox': AI_REPO + '/dist/.dm2-xbox-cache/media/assets/string.eus',
    'kstr.files': ';'.join([AI_REPO + '/dist/.xbox-cache/media/assets/menu/*.zsc',
                            AI_REPO + '/dist/.dm2-xbox-cache/media/assets/menu/*.zsc',
                            SCRATCH + '/gcdisc/**/*.zsc', SCRATCH + '/gcdisc/**/*.bni',
                            SCRATCH + '/gcdisc/**/*.log']),
    # the results of sweep jobs and agents
    'results': WORKDB + '/results',
}
# The Xbox and GameCube exports: the readable ones by default (tools/exports.py: the readable
# copy's ExportXbe, which writes the analysis name as "n", so every table reads as from the base
# file), `build --base-exports` or READABLE_EXPORTS=base for the files the analysis wrote.
import exports  # noqa: E402
OVERRIDDEN = set()


def use_exports(readable, warn=False):
    for b in exports.BUILDS:
        if 'export' in exports.BASE[b] and b + '.jsonl' not in OVERRIDDEN:
            SOURCES[b + '.jsonl'] = exports.path(b, 'export', readable, warn)


use_exports(exports.default_readable())
if os.environ.get('WORKDB_SOURCES'):
    _o = json.load(open(os.environ['WORKDB_SOURCES']))
    OVERRIDDEN.update(_o)
    SOURCES.update(_o)


def src(name):
    p = SOURCES.get(name)
    return p if p and os.path.exists(p) else None


# The nine builds. Layer bounds (engine start, game start, SDK start) for the PS2 builds:
# AI's from READABLE.md "Layout"; DM2's and BMX XXX's from where AI's engine, game and SDK
# functions' identical and close counterparts fall (MATCH3.md section 6).
BUILDS = [
    ('ai_ps2', 'ai', 'ps2', 'mips-r5900', 'SLUS_203.27', (0x100100, 0x171000, 0x2021E0)),
    ('dm2_ps2', 'dm2', 'ps2', 'mips-r5900', 'SLUS_201.59', (0x100100, 0x150000, 0x1CC800)),
    ('bmx_ps2', 'bmx', 'ps2', 'mips-r5900', 'SLUS_204.15', (0x100100, 0x170000, 0x21DAA8)),
    ('ai_xbox', 'ai', 'xbox', 'x86', 'default.xbe (InlineXBox)', None),
    ('dm2_xbox', 'dm2', 'xbox', 'x86', 'default.xbe', None),
    ('bmx_xbox', 'bmx', 'xbox', 'x86', 'default.xbe', None),
    ('ai_gc', 'ai', 'gamecube', 'ppc-gekko', 'main.dol (GILE51)', None),
    ('dm2_gc', 'dm2', 'gamecube', 'ppc-gekko', 'main.dol (GBXE51)', None),
    ('bmx_gc', 'bmx', 'gamecube', 'ppc-gekko', 'main.dol (GB3E51)', None),
]
BUILD = {b[0]: b for b in BUILDS}
# SCE scan hits that are not the library: the signature's hash matched one of the game's own functions.
# DM2's 0x1C2728 ("_InitSys", 9 instructions) calls DM2's game code and reads its data, between the level
# object types (EditObj_GapMarker, Car, Boat ... SpecialFX/zfxMgr.c) that run to the libraries at 0x1CC800
# (sceGsResetGraph); DM2's _InitSys is 0x128D88, the hash of AI's. Their layer is by bounds, unnamed.
SCE_FALSE = {'dm2_ps2': {0x1C2728}}
LABEL = {'ai_ps2': 'AI PS2', 'dm2_ps2': 'DM2 PS2', 'bmx_ps2': 'BMX PS2', 'ai_xbox': 'AI Xbox',
         'dm2_xbox': 'DM2 Xbox', 'bmx_xbox': 'BMX Xbox', 'ai_gc': 'AI GC', 'dm2_gc': 'DM2 GC',
         'bmx_gc': 'BMX GC'}
GAME_PS2 = {'ai': 'ai_ps2', 'dm2': 'dm2_ps2', 'bmx': 'bmx_ps2'}

# ------------------------------------------------------------------ schema

A = 'UBIGINT'
SCHEMA = collections.OrderedDict([
    ('meta', [('key', 'VARCHAR'), ('value', 'VARCHAR')]),
    ('sources', [('name', 'VARCHAR'), ('path', 'VARCHAR'), ('present', 'BOOLEAN'),
                 ('bytes', 'UBIGINT'), ('mtime', 'TIMESTAMP')]),
    ('builds', [('build', 'VARCHAR'), ('game', 'VARCHAR'), ('platform', 'VARCHAR'),
                ('cpu', 'VARCHAR'), ('executable', 'VARCHAR'), ('loaded', 'BOOLEAN'),
                ('functions', 'INTEGER'), ('paired_to', 'VARCHAR'), ('pairs', 'INTEGER'),
                ('engine_start', A), ('game_start', A), ('sdk_start', A), ('note', 'VARCHAR')]),
    # layer 1: functions, calls, data
    ('functions', [('build', 'VARCHAR'), ('addr', A), ('end_addr', A), ('bytes', 'INTEGER'),
                   ('insns', 'INTEGER'), ('name', 'VARCHAR'), ('named', 'BOOLEAN'),
                   ('layer', 'VARCHAR'), ('section', 'VARCHAR'), ('hash', 'VARCHAR'),
                   ('n_calls', 'INTEGER'), ('n_strings', 'INTEGER'), ('floats', 'DOUBLE[]'),
                   ('ints', 'UBIGINT[]'), ('disps', 'INTEGER[]')]),
    ('calls', [('build', 'VARCHAR'), ('caller', A), ('call_offset', 'INTEGER'), ('callee', A)]),
    ('func_strings', [('build', 'VARCHAR'), ('func', A), ('addr', A), ('text', 'VARCHAR'),
                      ('is_path', 'BOOLEAN')]),
    ('func_globals', [('build', 'VARCHAR'), ('func', A), ('addr', A)]),
    ('file_anchors', [('build', 'VARCHAR'), ('func', A), ('path', 'VARCHAR'), ('file', 'VARCHAR'),
                      ('source', 'VARCHAR')]),
    ('modules', [('build', 'VARCHAR'), ('func', A), ('module', 'VARCHAR'), ('path', 'VARCHAR'),
                 ('how', 'VARCHAR'), ('block', 'INTEGER'), ('candidates', 'VARCHAR[]')]),
    ('blocks', [('build', 'VARCHAR'), ('block', 'INTEGER'), ('first_addr', A), ('last_addr', A),
                ('functions', 'INTEGER'), ('words', 'INTEGER'), ('layer', 'VARCHAR'),
                ('modules', 'VARCHAR'), ('readable', 'VARCHAR[]'), ('folders', 'VARCHAR')]),
    # matches between builds (direct links) and AI's counterparts in every build (chained)
    ('matches', [('a_build', 'VARCHAR'), ('a_addr', A), ('b_build', 'VARCHAR'), ('b_addr', A),
                 ('method', 'VARCHAR'), ('cls', 'VARCHAR'), ('confidence', 'VARCHAR'),
                 ('score', 'DOUBLE'), ('votes', 'DOUBLE'), ('detail', 'VARCHAR'),
                 ('triangle', 'BOOLEAN'), ('source', 'VARCHAR')]),
    ('inlined', [('a_build', 'VARCHAR'), ('a_addr', A), ('b_build', 'VARCHAR'), ('host', A),
                 ('callers', 'INTEGER'), ('evidence', 'VARCHAR'), ('source', 'VARCHAR')]),
    ('counterparts', [('ai_addr', A), ('build', 'VARCHAR'), ('addr', A), ('confidence', 'VARCHAR'),
                      ('cls', 'VARCHAR'), ('method', 'VARCHAR'), ('via', 'VARCHAR'),
                      ('score', 'DOUBLE'), ('inlined', 'BOOLEAN')]),
    # the readable sources
    ('readable', [('addr', A), ('name', 'VARCHAR'), ('kind', 'VARCHAR'), ('status', 'VARCHAR'),
                  ('file', 'VARCHAR'), ('line', 'INTEGER'), ('waits', 'BOOLEAN'),
                  ('vu0', 'BOOLEAN'), ('doc', 'VARCHAR')]),
    ('readable_files', [('file', 'VARCHAR'), ('lines', 'INTEGER')]),
    # each registration's current source digest (readable_digest.py: its definition and the same
    # file's functions it calls), which a zx-fncheck pass names as impl
    ('readable_digests', [('game', 'VARCHAR'), ('addr', A), ('name', 'VARCHAR'), ('status', 'VARCHAR'),
                          ('file', 'VARCHAR'), ('digest', 'VARCHAR')]),
    ('progress', [('merged', 'VARCHAR'), ('commit', 'VARCHAR'), ('verified', 'INTEGER')]),
    ('profile', [('addr', A), ('calls', 'BIGINT'), ('inclusive_ns', 'BIGINT'), ('self_ns', 'BIGINT')]),
    ('readable_symbols', [('addr', A), ('name', 'VARCHAR'), ('kind', 'VARCHAR'),
                          ('type', 'VARCHAR'), ('file', 'VARCHAR'), ('line', 'INTEGER')]),
    # the recording corpus
    ('corpus_routes', [('version', 'INTEGER'), ('route', 'VARCHAR'), ('description', 'VARCHAR'),
                       ('functions', 'INTEGER'), ('calls', 'BIGINT'), ('bytes', 'BIGINT'),
                       ('when_made', 'VARCHAR')]),
    ('corpus', [('version', 'INTEGER'), ('route', 'VARCHAR'), ('addr', A), ('name', 'VARCHAR'),
                ('calls', 'INTEGER'), ('callee_calls', 'BIGINT'), ('mb', 'DOUBLE'),
                ('vsync_first', 'INTEGER'), ('vsync_last', 'INTEGER'), ('cut_short', 'BOOLEAN')]),
    ('corpus_uncovered', [('version', 'INTEGER'), ('addr', A), ('name', 'VARCHAR'),
                          ('status', 'VARCHAR')]),
    # strings, tuning, name candidates
    ('tuning', [('addr', A), ('category', 'VARCHAR'), ('name', 'VARCHAR'), ('value', 'DOUBLE'),
                ('max_value', 'DOUBLE'), ('min_value', 'DOUBLE'), ('step', 'DOUBLE')]),
    ('string_keys', [('build', 'VARCHAR'), ('key', A), ('name', 'VARCHAR'), ('text', 'VARCHAR')]),
    ('key_names', [('key', A), ('name', 'VARCHAR'), ('source', 'VARCHAR')]),
    ('names', [('build', 'VARCHAR'), ('addr', A), ('name', 'VARCHAR'), ('source', 'VARCHAR'),
               ('kind', 'VARCHAR'), ('confidence', 'VARCHAR'), ('evidence', 'VARCHAR')]),
    ('best_names', [('build', 'VARCHAR'), ('addr', A), ('name', 'VARCHAR'), ('source', 'VARCHAR')]),
    # layer 2: the sweep's results (filled from results/*.jsonl)
    ('sweep_draft', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'),
                     ('agent', 'VARCHAR'), ('result', 'VARCHAR'), ('reason', 'VARCHAR'),
                     ('file', 'VARCHAR'), ('lines', 'INTEGER'), ('gotos', 'INTEGER'),
                     ('name', 'VARCHAR'), ('source_file', 'VARCHAR'), ('line', 'INTEGER'),
                     ('extra', 'VARCHAR')]),
    ('sweep_build', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'),
                     ('agent', 'VARCHAR'), ('result', 'VARCHAR'), ('error', 'VARCHAR'),
                     ('build_dir', 'VARCHAR'), ('source_file', 'VARCHAR'), ('line', 'INTEGER'),
                     ('extra', 'VARCHAR')]),
    ('sweep_check', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'),
                     ('agent', 'VARCHAR'), ('route', 'VARCHAR'), ('result', 'VARCHAR'),
                     ('calls', 'INTEGER'), ('failed', 'INTEGER'), ('first_failure', 'VARCHAR'),
                     ('cycles_equal', 'BOOLEAN'), ('game', 'VARCHAR'),
                     # what the check was made against (zx-fncheck's result line): the readable
                     # function's source digest, the generated original's, the recording's recorder
                     # version and corpus folder, the checker executable's
                     ('impl', 'VARCHAR'), ('orig', 'VARCHAR'), ('rec', 'VARCHAR'), ('checker', 'VARCHAR'),
                     ('source_file', 'VARCHAR'), ('line', 'INTEGER'), ('extra', 'VARCHAR')]),
    # path coverage of the recordings: the original's basic blocks the replayed calls ran, summed
    # over a corpus (tools/checks/fncheck_corpus.py emit)
    ('sweep_coverage', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'),
                        ('agent', 'VARCHAR'), ('game', 'VARCHAR'), ('blocks', 'INTEGER'), ('hit', 'INTEGER'),
                        ('calls', 'INTEGER'), ('files', 'INTEGER'), ('uncovered', 'VARCHAR'),
                        ('corpus', 'VARCHAR'), ('orig', 'VARCHAR'), ('checker', 'VARCHAR'),
                        ('source_file', 'VARCHAR'), ('line', 'INTEGER'), ('extra', 'VARCHAR')]),
    ('sweep_failure', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'),
                       ('ts', 'TIMESTAMP'), ('agent', 'VARCHAR'), ('stage', 'VARCHAR'),
                       ('failure_class', 'VARCHAR'), ('detail', 'VARCHAR'),
                       ('source_file', 'VARCHAR'), ('line', 'INTEGER'), ('extra', 'VARCHAR')]),
    ('sweep_status', [('run', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'),
                      ('agent', 'VARCHAR'), ('status', 'VARCHAR'), ('registered', 'BOOLEAN'),
                      ('note', 'VARCHAR'), ('source_file', 'VARCHAR'), ('line', 'INTEGER'),
                      ('extra', 'VARCHAR')]),
    # functions of the other builds mapped by hand (a port's study: DM2's rider code), keyed by
    # their own build and address, and pairs between builds the match does not make (DM2 -> BMX XXX)
    ('func_map', [('run', 'VARCHAR'), ('build', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'),
                  ('ts', 'TIMESTAMP'), ('agent', 'VARCHAR'), ('name', 'VARCHAR'), ('role', 'VARCHAR'),
                  ('port', 'VARCHAR'), ('confidence', 'VARCHAR'), ('module', 'VARCHAR'),
                  ('covered', 'VARCHAR'), ('size', 'INTEGER'), ('callers', 'INTEGER'),
                  ('callees', 'INTEGER'), ('note', 'VARCHAR'), ('counterparts', 'VARCHAR'),
                  ('source_file', 'VARCHAR'), ('line', 'INTEGER'), ('extra', 'VARCHAR')]),
    ('func_pairs', [('run', 'VARCHAR'), ('build', 'VARCHAR'), ('addr', A), ('b_build', 'VARCHAR'),
                    ('b_addr', A), ('generator', 'VARCHAR'), ('ts', 'TIMESTAMP'), ('agent', 'VARCHAR'),
                    ('method', 'VARCHAR'), ('cls', 'VARCHAR'), ('confidence', 'VARCHAR'),
                    ('ratio', 'DOUBLE'), ('source_file', 'VARCHAR'), ('line', 'INTEGER'),
                    ('extra', 'VARCHAR')]),
    # knowledge notes: what a function does, what was cut, bugs kept, how it is reached, layout
    # evidence, why it stays Draft (kind `note`; several per function, shown on its card)
    ('func_notes', [('run', 'VARCHAR'), ('build', 'VARCHAR'), ('addr', A), ('generator', 'VARCHAR'),
                    ('ts', 'TIMESTAMP'), ('agent', 'VARCHAR'), ('topic', 'VARCHAR'),
                    ('text', 'VARCHAR'), ('source', 'VARCHAR'), ('source_file', 'VARCHAR'),
                    ('line', 'INTEGER'), ('extra', 'VARCHAR')]),
    ('ingest_files', [('path', 'VARCHAR'), ('lines', 'INTEGER'), ('accepted', 'INTEGER'),
                      ('rejected', 'INTEGER'), ('mtime', 'TIMESTAMP')]),
    ('ingest_errors', [('path', 'VARCHAR'), ('line', 'INTEGER'), ('error', 'VARCHAR'),
                       ('text', 'VARCHAR')]),
])

BUILD_NAMES = ('ai_ps2', 'dm2_ps2', 'bmx_ps2', 'ai_xbox', 'dm2_xbox', 'bmx_xbox', 'ai_gc', 'dm2_gc', 'bmx_gc')

# The result kinds: their table, the fields a line must have besides kind/addr/run/generator/ts,
# and the values `result`/`status`/`stage` may take (anything else is rejected with a reason).
RESULT_KINDS = {
    'draft': ('sweep_draft', {'result': ('drafted', 'failed', 'skipped')}),
    'build': ('sweep_build', {'result': ('ok', 'compile_error', 'link_error', 'set_aside')}),
    'check': ('sweep_check', {'result': ('pass', 'fail', 'no_recording', 'crash', 'timeout',
                                         'skipped'), 'route': None}),
    'failure': ('sweep_failure', {'stage': ('draft', 'build', 'check', 'register'),
                                  'failure_class': None}),
    'status': ('sweep_status', {'status': ('Draft', 'Tested', 'Verified', 'Planned')}),
    'coverage': ('sweep_coverage', {'blocks': None, 'hit': None}),
    # keyed by `build` + `addr` (any build, not only AI's PS2): a mapped function, a pair
    'map': ('func_map', {'build': BUILD_NAMES, 'role': None}),
    'pair': ('func_pairs', {'build': BUILD_NAMES, 'b_build': BUILD_NAMES, 'b_addr': None,
                            'confidence': ('high', 'medium', 'low')}),
}
# Knowledge notes (kind `note`): `build` is optional (default ai_ps2), `topic` one of these.
# purpose: what the function does. cut: a feature the PS2 original has that the port dropped, or
# the original lacks. bug-kept: an original bug the readable code keeps. trigger: what calls it
# or what makes it run. route: how a recording or a play session reaches it. layout: field,
# struct or table evidence. evidence: how a fact was established (a check, a trace, a string).
# caveat: a trap when changing it. history: a past change worth knowing (a fix, a rename).
NOTE_TOPICS = ('purpose', 'cut', 'bug-kept', 'trigger', 'route', 'layout', 'evidence', 'caveat',
               'history')
RESULT_KINDS['note'] = ('func_notes', {'topic': NOTE_TOPICS, 'text': None})
RESULT_COMMON = ('run', 'generator', 'ts')
CARD_NOTES = 12   # notes shown on a card

CONF_RANK = {'high': 3, 'medium': 2, 'low': 1}
CLS_CONF = {'identical': 'high', 'close': 'high', 'drifted': 'medium', 'weak': 'low'}


def h(a):
    return None if a is None else '0x%08X' % a


def parse_addr(s):
    if isinstance(s, int):
        return s
    s = str(s).strip()
    if s.lower().startswith('fun_'):
        s = s[4:]
    return int(s, 16)


# ------------------------------------------------------------------ the writer

class Writer:
    """Rows into NDJSON files per table, loaded into DuckDB with the schema's types."""

    def __init__(self, tmp):
        self.tmp = tmp
        self.files = {}
        self.counts = collections.Counter()

    def add(self, table, row):
        f = self.files.get(table)
        if f is None:
            f = self.files[table] = open(os.path.join(self.tmp, table + '.jsonl'), 'w')
        cols = SCHEMA[table]
        if isinstance(row, dict):
            row = [row.get(c) for c, _ in cols]
        assert len(row) == len(cols), (table, row)
        f.write(json.dumps(row, default=_json_default) + '\n')
        self.counts[table] += 1

    def load(self, con):
        for t, cols in SCHEMA.items():
            con.execute('CREATE TABLE %s (%s)' % (t, ', '.join('%s %s' % c for c in cols)))
        for t, f in self.files.items():
            f.close()
            cols = SCHEMA[t]
            # each line is a JSON array; read it as one column and unpack by position
            p = os.path.join(self.tmp, t + '.jsonl')
            sel = ', '.join(_extract(i, ty) for i, (c, ty) in enumerate(cols))
            con.execute("INSERT INTO %s SELECT %s FROM read_json('%s', format='newline_delimited', "
                        "columns={'j': 'JSON'}, records=false)" % (t, sel, p.replace("'", "''")))


def _extract(i, ty):
    if ty.endswith('[]'):
        return "CAST(j->%d AS %s)" % (i, ty)
    if ty == 'VARCHAR':
        return "j->>%d" % i
    if ty == 'TIMESTAMP':
        return "TRY_CAST(j->>%d AS TIMESTAMP)" % i
    return "CAST(j->>%d AS %s)" % (i, ty)


def _json_default(o):
    if isinstance(o, (set, frozenset)):
        return sorted(o)
    if isinstance(o, datetime.datetime):
        return o.isoformat(sep=' ')
    raise TypeError(type(o))


# ------------------------------------------------------------------ build steps

def log(*a):
    print('[%s]' % datetime.datetime.now().strftime('%H:%M:%S'), *a, file=sys.stderr, flush=True)


def is_named(n):
    return bool(n) and not n.split('::')[-1].startswith(
        ('FUN_', 'thunk_FUN_', 'LAB_', 'entry_', 'switchD', 'caseD', 'SUB_'))


def load_ps2(W, ctx):
    """The three PS2 builds: functions, calls, strings, globals, floats, offsets."""
    import fmatch3, xmatch
    for b, game, plat, cpu, exe, bounds in BUILDS:
        if plat != 'ps2':
            continue
        e, c, s = src(b + '.exe'), src(b + '.csv'), src(b + '.sce')
        if not (e and c and s):
            ctx['missing'][b] = 'ELF, function CSV or SCE scan missing'
            continue
        fmatch3.AI_ENGINE = (bounds[0], bounds[1])
        fmatch3.AI_SDK_START = bounds[2]
        P, PS = xmatch.ps2_side(e, c, s)
        sce = {}
        for r in csv.DictReader(open(s)):
            sce[int(r['Address'], 16)] = (r['Name'], r['Library'])
        ctx['sides'][b] = PS
        for a in SCE_FALSE.get(b, ()):
            sce.pop(a, None)
            P.names.pop(a, None)
            if a in PS:
                PS[a]['layer'] = fmatch3.layer_of(a, set())
                PS[a]['name'] = None
        ctx['data'][b] = P.elf.data
        ctx['sce'][b] = sce
        ctx['elf'][b] = P.elf
        for a, f in PS.items():
            nm = P.names.get(a) or 'FUN_%08x' % a
            emit_function(W, b, f, nm, '.text', f.get('hash'), c32=set(f['glob']))
        log(b, len(PS), 'functions')
    fmatch3.AI_ENGINE = (0x100100, 0x171000)
    fmatch3.AI_SDK_START = 0x2021E0


def emit_function(W, b, f, name, section, hsh, c32=()):
    a = f['a']
    W.add('functions', [b, a, f['e'], f['e'] - a, f['n'], name, is_named(name), f['layer'],
                        section, hsh, len(f['calls']), len(f['strs']),
                        sorted(v for v in f['floats'] if v == v and abs(v) != float('inf')),
                        sorted(f['ints']), sorted(f['disps'])])
    mul = 4 if BUILD[b][2] == 'ps2' else 1
    for off, t in f['calls']:
        W.add('calls', [b, a, off * mul, t])
    for sa, st in sorted(f['stra'].items()):
        W.add('func_strings', [b, a, sa, st, bool(PATH_RE().match(st))])
    strs = set(f['stra'])
    flts = set(f['fla'])
    for g in sorted(set(f['glob'])):
        if g not in strs and g not in flts:
            W.add('func_globals', [b, a, g])
    for sa, st in sorted(f['stra'].items()):
        if PATH_RE().match(st):
            path = st.replace('\\', '/')
            W.add('file_anchors', [b, a, path, path.rsplit('/', 1)[-1], 'string'])


def PATH_RE():
    import xmatch
    return xmatch.PATH_RE


def load_other(W, ctx):
    """The Xbox and GameCube builds and their pairings with their PS2 builds."""
    import xmatch, gcmatch
    for b, game, plat, cpu, exe, bounds in BUILDS:
        if plat == 'ps2':
            continue
        e, j = src(b + '.exe'), src(b + '.jsonl')
        if not (e and j):
            ctx['missing'][b] = 'no Ghidra export (%s)' % SOURCES.get(b + '.jsonl')
            continue
        if plat == 'xbox':
            X, XS = xmatch.xbox_side(e, j)
            ctx['data'][b] = X.d
            for f in XS.values():
                f['c32'] = set(f['ints']) | set(f['glob'])
        else:
            X, XS = gcmatch.gc_side(e, j)
            ctx['data'][b] = X.d
        ctx['sides'][b] = XS
        pair = src(b + '.pair')
        lay = {}
        ps2 = GAME_PS2[game]
        if pair:
            P = json.load(open(pair))
            ctx['pairs'][b] = P
            for p, r in P['pairs'].items():
                W.add('matches', [ps2, int(p, 16), b, int(r['x'], 16), r['how'], r['conf'],
                                  r['conf'], r['score'], _num(r.get('votes')),
                                  json.dumps(r.get('detail')), None,
                                  os.path.basename(os.path.dirname(pair)) + '/' + os.path.basename(pair)])
                lay[int(r['x'], 16)] = r['layer']
            for p, r in P['inlined'].items():
                for host in r['hosts']:
                    W.add('inlined', [ps2, int(p, 16), b, int(host, 16), r['callers'],
                                      r['evidence'], os.path.basename(pair)])
            libstart = int(P.get('xbox_lib_start', '0xFFFFFFFF'), 16)
        else:
            ctx['missing'][b] = 'not paired (%s missing)' % SOURCES.get(b + '.pair')
            libstart = 1 << 32
        for a, f in XS.items():
            if a in lay:
                f['layer'] = lay[a]
            elif plat == 'xbox' and (f['sec'] != '.text' or a >= libstart or is_named(f['name'])):
                f['layer'] = 'sdk'
            else:
                f['layer'] = 'unknown'
            emit_function(W, b, f, f['name'], f['sec'], None, f.get('c32', ()))
        if plat == 'xbox' and src(b + '.paths'):
            starts = sorted(XS)
            import bisect
            for r in json.load(open(src(b + '.paths'))):
                path = r['path'].replace('\\', '/')
                for u in r['uses']:
                    u = int(u, 16)
                    i = bisect.bisect_right(starts, u) - 1
                    if i >= 0 and u < XS[starts[i]]['e']:
                        W.add('file_anchors', [b, starts[i], path, path.rsplit('/', 1)[-1],
                                               'xbe_paths'])
        log(b, len(XS), 'functions,', len(P['pairs']) if pair else 0, 'pairs')


def _num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load_match3(W, ctx):
    """AI against DM2 and BMX XXX on PS2 (fmatch3.py), and AI's module map (modmap.py)."""
    m = src('match3')
    if not m:
        ctx['missing']['match3'] = 'no match3.json'
        return
    M = json.load(open(m))
    for a, r in M['functions'].items():
        for g, b in (('dm2', 'dm2_ps2'), ('bmx', 'bmx_ps2')):
            x = r.get(g)
            if not x or not x.get('addr'):
                continue
            W.add('matches', ['ai_ps2', int(a, 16), b, int(x['addr'], 16), x.get('method'),
                              x['cls'], CLS_CONF.get(x['cls'], 'low'), x.get('ratio'),
                              None, json.dumps(dict(ops=x.get('ops'),
                                                    callees_consistent=x.get('callees_consistent'))),
                              r.get('triangle'), 'match3.json'])
            ctx['ai_layer_of'].setdefault(b, {}).setdefault(int(x['addr'], 16), r['layer'])
    mm = src('modmap')
    if not mm:
        return
    MM = json.load(open(mm))
    for a, r in MM['functions'].items():
        W.add('modules', ['ai_ps2', int(a, 16), r.get('module'), r.get('path'), r.get('how'),
                          r.get('block'), r.get('candidates')])
    for k, r in MM['block_ranges'].items():
        W.add('blocks', ['ai_ps2', int(k), int(r['first'], 16), int(r['last'], 16), r['functions'],
                         r['words'], r['layer'], json.dumps(r.get('modules')),
                         r.get('readable'), json.dumps(r.get('folders'))])
    ctx['modmap'] = MM


REG_RE = re.compile(r'READABLE_(FUNCTION(?:_WAITS|_VU0)*|PLANNED)\(\s*(0x[0-9A-Fa-f]+)\s*,'
                    r'\s*([^,)]+?)\s*(?:,\s*(\w+))?\s*\)')
# Another game's registrations (READABLE_FUNCTION_IN, _WAITS_IN, _VU0_IN, READABLE_PLANNED_IN: GAME first,
# ee/game_id.h; DM2's rider port): named in that game's build (<game>_ps2), not in AI's readable table.
REG_IN_RE = re.compile(r'READABLE_(FUNCTION(?:_WAITS|_VU0)*|PLANNED)_IN\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,'
                       r'\s*([^,)]+?)\s*(?:,\s*(\w+))?\s*\)')
GLOBAL_RE = re.compile(r'ee::Global<\s*(.+?)\s*,\s*(0x[0-9A-Fa-f]+)u?\s*(?:([-+])\s*(0x[0-9A-Fa-f]+)u?\s*)?>'
                       r'\s*(\w+)')
CONST_RE = re.compile(r'\b(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)u?\s*[;,]\s*(?://\s*(.*))?')
DOC_RE = re.compile(r'//\s*([A-Za-z_]\w*)\s*[:,]\s*(.*)')


def load_readable(W, ctx):
    root = src('game_src')
    if not root:
        ctx['missing']['readable'] = 'no readable sources at %s' % GAME_SRC
        return
    # Sources name their addresses (tools/readable/readable_symbols.py): read them with the literals written back.
    sys.path.insert(0, os.path.join(root, 'tools', 'readable'))
    import readable_symbols
    readable_symbols.TABLE = os.path.join(root, 'tools', 'readable', 'readable_symbols.tsv')
    docs = {}
    regs = {}
    regs_in = {}
    for dp, _, files in zpaths.zlayout.readable_walk(root):
        if '/drafts' in dp.replace(root, ''):
            continue
        for fn in sorted(files):
            if not fn.endswith(('.cpp', '.h')):
                continue
            p = os.path.join(dp, fn)
            rel = zpaths.zlayout.readable_key(root, p).replace('/', os.sep)   # (its key: zge/render/draw.cpp)
            lines = readable_symbols.literal_view(open(p, encoding='utf-8', errors='replace').read()).split('\n')
            W.add('readable_files', [rel, len(lines) - 1])
            for i, ln in enumerate(lines, 1):
                s = ln.strip()
                m = DOC_RE.match(s)
                if m:
                    t = m.group(2).strip()
                    # the comment goes on over the next comment lines
                    k = i
                    while k < len(lines) and lines[k].strip().startswith('//') and \
                            not DOC_RE.search(lines[k]) and len(t) < 300:
                        t += ' ' + lines[k].strip()[2:].strip()
                        k += 1
                    docs.setdefault(m.group(1), (t[:400], rel))
                if s.startswith(('//', '#', '*')):
                    continue
                m = REG_RE.search(s)
                if m:
                    kind, addr, name, status = m.groups()
                    a = int(addr, 16)
                    regs[a] = [a, name.strip('"& '), kind, status or 'Planned', rel, i,
                               'WAITS' in kind, 'VU0' in kind]
                m = REG_IN_RE.search(s)
                if m and m.group(2) != 'GAME':
                    kind, game, addr, name, status = m.groups()
                    a = int(addr, 16)
                    regs_in[(game, a)] = [a, name.strip('"& '), kind, status or 'Planned', rel, i,
                                          'WAITS' in kind, 'VU0' in kind]
                for m in GLOBAL_RE.finditer(s):
                    ty, base, sign, off, name = m.groups()
                    v = int(base, 16) + (int(off, 16) * (1 if sign == '+' else -1) if off else 0)
                    W.add('readable_symbols', [v, name, 'global', ty, rel, i])
                if 'constexpr' in s or re.match(r'k[A-Z]\w*\s*=', s):
                    for m in CONST_RE.finditer(s):
                        W.add('readable_symbols', [int(m.group(2), 16), m.group(1), 'constant',
                                                   m.group(3), rel, i])
    # The sweep's generated drafts (<layer>/sweep/manifest.txt in a readable folder, the batch files are made
    # at build time): a manifest row is the registration (address, status, file, namespace, macro, flags,
    # name, ...); a hand-written registration of the same address wins.
    for layer, folder in zpaths.zlayout.readable_dirs(root):
        mp = os.path.join(folder, 'sweep', 'manifest.txt')
        if not os.path.isfile(mp):
            continue
        for i, ln in enumerate(open(mp, encoding='utf-8', errors='replace'), 1):
            c = ln.rstrip('\n').split('\t')
            if ln.startswith('#') or len(c) < 7 or not c[0].startswith('0x'):
                continue
            a = int(c[0], 16)
            if a in regs:
                continue
            kind = 'PLANNED' if c[1] == 'Planned' else (c[4][len('READABLE_'):] if c[4].startswith('READABLE_')
                                                         else 'FUNCTION')
            regs[a] = [a, c[6] if c[6] != '-' else 'fn_%X' % a, kind, c[1], c[2], i, 'WAITS' in kind, 'VU0' in kind]
    # A function's comment starts with its name.
    docs = {a: docs[r[1].split('::')[-1]] for a, r in regs.items() if r[1].split('::')[-1] in docs}
    for a, r in regs.items():
        d = docs.get(a)
        W.add('readable', r + [d[0] if d else None])
    ctx['readable'] = regs
    ctx['readable_in'] = regs_in
    ctx['docs'] = docs
    log('readable', len(regs), 'registrations,', W.counts['readable_symbols'], 'symbols')
    load_readable_digests(W, ctx, root)


def load_readable_digests(W, ctx, root):
    tool = src('readable_digest')
    if not tool:
        ctx['missing']['readable_digests'] = 'no readable_digest.py at %s' % SOURCES.get('readable_digest')
        return
    p = subprocess.run([sys.executable, '-I', tool, '--json', '--tree', root],
                       capture_output=True, text=True)
    if p.returncode:
        ctx['missing']['readable_digests'] = 'readable_digest.py failed: %s' % p.stderr.strip()[-300:]
        return
    for r in json.loads(p.stdout or '[]'):
        key = zpaths.zlayout.readable_key(root, r['file'])     # (a file by its key, as before layout1)
        W.add('readable_digests', [r['game'], int(r['addr'], 16), r['name'], r['status'],
                                   key.replace('/', os.sep) if key else r['file'], r['digest']])
    log('readable digests', W.counts['readable_digests'])


def load_corpus(W, ctx):
    root = src('corpus')
    if not root:
        ctx['missing']['corpus'] = 'no recording corpus'
        return
    for idx in sorted(glob.glob(os.path.join(root, 'v*', 'index.json'))):
        try:
            d = json.load(open(idx))
        except ValueError as e:
            ctx['missing']['corpus ' + idx] = 'unreadable: %s' % e
            continue
        v = d.get('version') or int(re.sub(r'\D', '', os.path.basename(os.path.dirname(idx))))
        for route, r in d['routes'].items():
            W.add('corpus_routes', [v, route, r.get('description'), len(r['functions']),
                                    r.get('calls'), r.get('bytes'), d.get('when')])
            for a, f in r['functions'].items():
                vs = f.get('vsyncs') or [None, None]
                W.add('corpus', [v, route, int(a, 16), f.get('name'), f.get('calls'),
                                 f.get('callee_calls'), f.get('mb'), vs[0], vs[-1],
                                 f.get('cut_short')])
        for u in d.get('uncovered', []):
            W.add('corpus_uncovered', [v, int(u['address'], 16), u.get('name'), u.get('status')])
    log('corpus', W.counts['corpus'], 'function-route rows')


def load_history(W, ctx):
    """The profile (by function start) and the progress line: Verified counts that main's
    PS2Recomp bump commits report ("N of M ... on")."""
    p = src('profile')
    if p:
        for r in csv.DictReader(open(p)):
            W.add('profile', [int(r['address'], 16), int(r['calls']), int(r.get('inclusive_ns') or 0),
                              int(r['self_ns'])])
    try:
        out = subprocess.run(['git', '-C', ZAXIS, 'log', '--reverse', '--date=format:%Y-%m-%d %H:%M',
                              '--format=%ad|%h|%s', 'main'], capture_output=True, text=True).stdout
    except OSError:
        out = ''
    for ln in out.splitlines():
        d, c, msg = ln.split('|', 2)
        m = re.search(r'\b(\d+) of (\d+) (?:readable functions )?on\b', msg) or \
            re.search(r'\b(\d+) verified and on by default', msg)
        if m and re.match(r'(Bump PS2Recomp|Batch [0-9])', msg):  # the batch marks (push-batch.sh)
            W.add('progress', [d, c, int(m.group(1))])


def name_hash(name):
    """The engine's name hash (AI 0x148888, DM2 0x133F18): case-insensitive ELF hash
    (tools/modding/dm2_level.py name_hash)."""
    hh = 0
    for ch in name.encode('latin1'):
        c = ch - 32 if 97 <= ch <= 122 else ch
        hh = ((hh << 4) + c) & 0xFFFFFFFF
        high = hh & 0xF0000000
        if high:
            hh ^= high >> 24
        hh &= ~high & 0xFFFFFFFF
    return hh


KSTR_RE = re.compile(rb'KSTR_[A-Za-z0-9_]+')


def load_strings(W, ctx):
    """String tables (key = name hash of KSTR_*), the KSTR names the builds and UI files spell
    out, and AI's tuning table."""
    names = {}
    for b, data in ctx['data'].items():
        for m in KSTR_RE.finditer(data):
            names.setdefault(m.group().decode(), b)
    pats = (SOURCES.get('kstr.files') or '').split(';')
    for pat in pats:
        for p in glob.glob(pat, recursive=True):
            if os.path.getsize(p) > 64 << 20:
                continue
            for m in KSTR_RE.finditer(open(p, 'rb').read()):
                names.setdefault(m.group().decode(), os.path.basename(p))
    keyname = {}
    for n, where in sorted(names.items()):
        k = name_hash(n)
        keyname.setdefault(k, n)
        W.add('key_names', [k, n, where])
    texts = {}
    for b in ('ai_ps2', 'dm2_ps2', 'ai_xbox', 'dm2_xbox'):
        p = src('strings.' + b)
        if not p:
            continue
        d = open(p, 'rb').read()
        size, count = struct.unpack_from('<II', d, 0)
        for i in range(count):
            k, off = struct.unpack_from('<II', d, 8 + 8 * i)
            t = None
            if off:
                e = d.find(b'\0', off)
                t = d[off:e].decode('latin1')
            W.add('string_keys', [b, k, keyname.get(k), t])
            if t is not None:
                texts.setdefault(k, t)
    ctx['keys'] = (keyname, texts)
    # AI's tuning table (aggressive/tuning.h): 32 categories at 0x24AD28 of {entries, count,
    # name[32]}, entries of {name[32], value, max, min, step}
    E = ctx['elf'].get('ai_ps2')
    tun = {}
    if E:
        for i in range(32):
            c = E.read(0x24AD28 + 0x28 * i, 0x28)
            if not c or len(c) < 0x28:
                break
            ptr, cnt = struct.unpack_from('<II', c, 0)
            if not ptr:
                break
            cat = c[8:40].split(b'\0')[0].decode('latin1')
            for k in range(cnt):
                ea = ptr + 0x30 * k
                e = E.read(ea, 0x30)
                if not e or len(e) < 0x30:
                    break
                nm = e[:32].split(b'\0')[0].decode('latin1')
                v, mx, mn, st = struct.unpack_from('<4f', e, 32)
                W.add('tuning', [ea, cat, nm, v, mx, mn, st])
                tun[ea] = (cat, nm, v)
                ctx.setdefault('tuning_cats', {}).setdefault(0x24AD28 + 0x28 * i, []).append(ea)
    ctx['tuning'] = tun
    log('strings:', len(names), 'KSTR names,', W.counts['string_keys'], 'table keys,',
        len(tun), 'tuning entries')


def load_names(W, ctx):
    """Name candidates per function and build, with their source."""
    import xmatch
    keyname, texts = ctx.get('keys', ({}, {}))
    tun = ctx.get('tuning', {})
    tun_starts = sorted(tun)
    import bisect
    best = {}

    def cand(b, a, name, source, kind, conf, ev=None):
        W.add('names', [b, a, name, source, kind, conf, ev])

    regs = ctx.get('readable', {})
    for a, r in regs.items():
        cand('ai_ps2', a, r[1], 'readable', 'function', 'high', '%s, %s:%d' % (r[3], r[4], r[5]))
    # readable_names.tsv (tools/readable): where a function, a global or a resume point got its name.
    table = os.path.join(GAME_SRC, 'tools', 'readable', 'readable_names.tsv')
    if os.path.exists(table):
        for line in open(table, encoding='utf-8'):
            c = line.rstrip('\n').split('\t')
            if len(c) >= 4 and c[0] in ('function', 'global', 'datum', 'part', 'point') and c[1].startswith('0x'):
                cand('ai_ps2', int(c[1], 16), c[2], 'readable_names', c[0], c[3], c[4] if len(c) > 4 else None)
    for (game, a), r in ctx.get('readable_in', {}).items():
        cand(game + '_ps2', a, r[1], 'readable', 'function', 'high', '%s, %s:%d' % (r[3], r[4], r[5]))
    # The other games' own rows of readable_symbols.tsv (tools/readable: kind, name, address, function, game):
    # the names their sources call their own functions, data and resume points by, BMX XXX's editor functions
    # with no AI counterpart among them. (AI's rows are made from this database's own names: not read back.)
    table = os.path.join(GAME_SRC, 'tools', 'readable', 'readable_symbols.tsv')
    own = collections.defaultdict(set)
    if os.path.exists(table):
        for line in open(table, encoding='utf-8'):
            kind, name, addr, fn, game = (line.rstrip('\n').split('\t') + [''] * 5)[:5]
            if line.startswith('#') or not game or game + '_ps2' not in BUILD or not addr.startswith('0x'):
                continue
            k = {'function': 'function', 'data': 'datum', 'point': 'point'}.get(kind)
            if k:
                own[game].add(name)
                cand(game + '_ps2', int(addr, 16), name, 'readable_symbols', k, 'high',
                     'readable_symbols.tsv' + (' (in %s)' % fn if fn else ''))
    ctx['own_symbols'] = own
    for a, (t, rel) in ctx.get('docs', {}).items():
        if a not in regs:
            cand('ai_ps2', a, None, 'readable_doc', 'comment', 'medium', '%s: %s' % (rel, t[:200]))
    for b, sce in ctx['sce'].items():
        for a, (n, lib) in sce.items():
            cand(b, a, n, 'sce', 'function', 'high', lib)
    for b, S in ctx['sides'].items():
        for a, f in S.items():
            n = f.get('name')
            if is_named(n) and not (b in ctx['sce'] and a in ctx['sce'][b]):
                src_ = 'ghidra' if BUILD[b][2] == 'ps2' else (
                    'xdk' if '::' in n else 'function_id')
                cand(b, a, n, src_, 'function', 'high', None)
            # identifiers in its messages (xmatch.py names_from_xbox's rule, every build)
            for sa, st in sorted(f['stra'].items()):
                if xmatch.PATH_RE.match(st) or ' ' not in st and len(st) > 40:
                    continue
                for m in xmatch.NAME_RE.finditer(st):
                    nm, tail = m.group(1), m.group(2)
                    if len(nm) < 6 or nm.isupper() or nm.startswith('XBOX'):
                        continue
                    kind = 'function' if tail in ('(', ':') and m.start() < 3 else 'identifier'
                    cand(b, a, nm, 'message', kind, 'medium' if kind == 'function' else 'low',
                         st[:120])
            for sa, st in sorted(f['stra'].items()):
                if xmatch.PATH_RE.match(st):
                    base = st.replace('\\', '/').rsplit('/', 1)[-1]
                    cand(b, a, base.rsplit('.', 1)[0], 'file', 'module', 'high', st)
            # string keys: a name hash as a constant (PS2), a "$KSTR_x" string (Xbox, GC)
            for k in sorted(set(f.get('c32', ())) | set(f['ints'])):
                if k in keyname or (k in texts and k > 0xFFFF):
                    cand(b, a, keyname.get(k), 'string_key', 'string_key', 'medium',
                         '%s%s' % ('#%08X ' % k if k not in keyname else '',
                                   json.dumps(texts.get(k))[:100] if k in texts else ''))
            for st in f['strs']:
                for m in KSTR_RE.finditer(st.encode('latin1', 'replace')):
                    n = m.group().decode()
                    t = texts.get(name_hash(n))
                    cand(b, a, n, 'string_key', 'string_key', 'medium',
                         json.dumps(t)[:100] if t else None)
            if b == 'ai_ps2' and tun:
                hit = set()
                for g in f['glob']:
                    i = bisect.bisect_right(tun_starts, g) - 1
                    if i >= 0 and g < tun_starts[i] + 0x30:
                        hit.add(tun_starts[i])
                for ea in sorted(hit):
                    cat, nm, v = tun[ea]
                    cand(b, a, '%s/%s' % (cat, nm), 'tuning', 'tuning', 'high',
                         'entry 0x%08X, value %g' % (ea, v))
                # the game reads most entries through the category: its entries pointer
                # (0x24AD28 + 0x28 k) and then +0x30 j + 0x20 (value), +0x24 max, +0x28 min
                cats = [g for g in set(f['glob']) if g in ctx.get('tuning_cats', {})]
                for g in sorted(cats):
                    es = ctx['tuning_cats'][g]
                    for d in sorted(f['disps']):
                        j, fld = divmod(d, 0x30)
                        if fld in (0x20, 0x24, 0x28, 0x2C) and j < len(es):
                            cat, nm, v = tun[es[j]]
                            cand(b, a, '%s/%s' % (cat, nm), 'tuning', 'tuning',
                                 'medium' if len(cats) == 1 else 'low',
                                 'category entries pointer 0x%08X, offset +0x%X (%s), value %g'
                                 % (g, d, {0x20: 'value', 0x24: 'max', 0x28: 'min',
                                           0x2C: 'step'}[fld], v))
    MM = ctx.get('modmap')
    if MM:
        for a, r in MM['functions'].items():
            if r.get('module'):
                conf = {'file': 'high', 'sdk': 'high', 'block': 'medium'}.get(r['how'], 'low')
                cand('ai_ps2', int(a, 16), r['module'], 'modmap', 'module', conf,
                     '%s (%s)' % (r.get('path') or '', r['how']))
            elif r.get('candidates'):
                cand('ai_ps2', int(a, 16), None, 'modmap', 'module', 'low',
                     'between %s; candidates %s' % (r.get('between'), ', '.join(r['candidates'][:8])))
    log('names', W.counts['names'], 'candidates')


def ingest_results(W, ctx):
    d = src('results')
    if not d:
        return
    for p in sorted(glob.glob(os.path.join(d, '**', '*.jsonl'), recursive=True)):
        n = ok = bad = 0
        for i, ln in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
            ln = ln.strip()
            if not ln:
                continue
            n += 1
            row, err = result_row(ln, p, i)
            if err:
                bad += 1
                W.add('ingest_errors', [p, i, err, ln[:500]])
            else:
                ok += 1
                W.add(row[0], row[1])
        W.add('ingest_files', [p, n, ok, bad, datetime.datetime.fromtimestamp(os.path.getmtime(p))])
    log('results', W.counts['ingest_files'], 'files,', sum(W.counts[t] for t in SCHEMA
                                                          if t.startswith('sweep_')), 'sweep rows,',
        W.counts['func_map'], 'mapped functions,', W.counts['func_pairs'], 'pairs')


def result_row(ln, path, line):
    """(table, row dict) for one JSONL result line, or (None, error)."""
    try:
        r = json.loads(ln)
    except ValueError as e:
        return None, 'not JSON: %s' % e
    if not isinstance(r, dict):
        return None, 'not an object'
    kind = r.get('kind')
    if kind not in RESULT_KINDS:
        return None, 'kind %r is not one of %s' % (kind, ', '.join(RESULT_KINDS))
    table, rules = RESULT_KINDS[kind]
    try:
        addr = parse_addr(r.get('addr'))
    except (TypeError, ValueError):
        return None, 'addr %r is not an address' % r.get('addr')
    for k in RESULT_COMMON:
        if not r.get(k):
            return None, 'missing %s' % k
    for k, allowed in rules.items():
        if r.get(k) in (None, ''):
            return None, 'missing %s' % k
        if allowed and r[k] not in allowed:
            return None, '%s %r is not one of %s' % (k, r[k], ', '.join(allowed))
    cols = [c for c, _ in SCHEMA[table]]
    row = {c: r.get(c) for c in cols if c in r}
    row['addr'] = addr
    if table == 'func_notes':
        if row.get('build') in (None, ''):
            row['build'] = 'ai_ps2'
        if row['build'] not in BUILD_NAMES:
            return None, 'build %r is not one of %s' % (row['build'], ', '.join(BUILD_NAMES))
    for c, ty in SCHEMA[table]:
        if ty == A and c != 'addr' and row.get(c) is not None:
            try:
                row[c] = parse_addr(row[c])
            except (TypeError, ValueError):
                return None, '%s %r is not an address' % (c, row[c])
    row['source_file'] = path
    row['line'] = line
    known = set(cols) | {'kind'}
    extra = {k: v for k, v in r.items() if k not in known}
    row['extra'] = json.dumps(extra) if extra else None
    for c, ty in SCHEMA[table]:
        v = row.get(c)
        if v is None:
            continue
        if ty == 'INTEGER' and not isinstance(v, int):
            try:
                row[c] = int(v)
            except (TypeError, ValueError):
                return None, '%s %r is not an integer' % (c, v)
        if ty == 'DOUBLE' and not isinstance(v, (int, float)):
            return None, '%s %r is not a number' % (c, v)
        if ty == 'BOOLEAN' and not isinstance(v, bool):
            return None, '%s %r is not true/false' % (c, v)
        if ty == 'VARCHAR' and not isinstance(v, str):
            row[c] = json.dumps(v)
    return (table, row), None


def finish(con, ctx):
    """Derived tables and the views."""
    # layers of the other builds: their AI counterpart's where the match has one
    for b, m in ctx['ai_layer_of'].items():
        con.execute('CREATE TEMP TABLE lay AS SELECT * FROM (VALUES %s) t(addr, layer)' %
                    ','.join("(%d, '%s')" % (a, l) for a, l in m.items()))
        con.execute("UPDATE functions SET layer = lay.layer FROM lay WHERE functions.build = ? "
                    "AND functions.addr = lay.addr", [b])
        con.execute('DROP TABLE lay')
    # Pairs read in the machine code (result kind `pair`, build ai_ps2, method `read`): the newest one per
    # AI function and game replaces what the match found; b_addr 0 says the game has no counterpart
    con.execute("""
        CREATE TEMP TABLE read_pairs AS
        SELECT DISTINCT ON (addr, b_build) addr, b_build, b_addr, cls, run
        FROM func_pairs WHERE build = 'ai_ps2' AND b_build IN ('dm2_ps2', 'bmx_ps2') AND method = 'read'
        ORDER BY addr, b_build, ts DESC""")
    con.execute("""DELETE FROM matches WHERE a_build = 'ai_ps2' AND EXISTS (SELECT 1 FROM read_pairs r
                   WHERE r.addr = matches.a_addr AND r.b_build = matches.b_build)""")
    con.execute("""
        INSERT INTO matches
        SELECT 'ai_ps2', addr, b_build, b_addr, 'read', coalesce(cls, 'close'), 'high', NULL, NULL, NULL,
               NULL, 'results:' || run
        FROM read_pairs WHERE b_addr <> 0""")
    # AI's counterparts in every build: direct, or through the other game's PS2 build
    con.execute("""
        INSERT INTO counterparts
        SELECT a_addr, b_build, b_addr, confidence, cls, method, NULL, score, false
        FROM matches WHERE a_build = 'ai_ps2'""")
    con.execute("""
        INSERT INTO counterparts
        SELECT m1.a_addr, m2.b_build, m2.b_addr,
               CASE WHEN least(r1.r, r2.r) = 3 THEN 'high' WHEN least(r1.r, r2.r) = 2 THEN 'medium'
                    ELSE 'low' END,
               m2.cls, m2.method, m1.b_build, m2.score, false
        FROM matches m1 JOIN matches m2 ON m2.a_build = m1.b_build AND m2.a_addr = m1.b_addr
        JOIN conf_rank r1 ON r1.c = m1.confidence JOIN conf_rank r2 ON r2.c = m2.confidence
        WHERE m1.a_build = 'ai_ps2' AND m1.b_build IN ('dm2_ps2', 'bmx_ps2')""")
    con.execute("""
        INSERT INTO counterparts
        SELECT i.a_addr, i.b_build, i.host, 'inlined', NULL, i.evidence, NULL, NULL, true
        FROM inlined i WHERE i.a_build = 'ai_ps2'
          AND NOT EXISTS (SELECT 1 FROM counterparts c WHERE c.ai_addr = i.a_addr
                          AND c.build = i.b_build AND NOT c.inlined)""")
    con.execute("""
        INSERT INTO counterparts
        SELECT m1.a_addr, i.b_build, i.host, 'inlined', NULL, i.evidence, m1.b_build, NULL, true
        FROM matches m1 JOIN inlined i ON i.a_build = m1.b_build AND i.a_addr = m1.b_addr
        WHERE m1.a_build = 'ai_ps2' AND m1.b_build IN ('dm2_ps2', 'bmx_ps2')
          AND NOT EXISTS (SELECT 1 FROM counterparts c WHERE c.ai_addr = m1.a_addr
                          AND c.build = i.b_build AND NOT c.inlined)""")
    # best name per function: AI's own, then its counterparts' (other builds get AI's)
    con.execute("""
        INSERT INTO best_names
        SELECT 'ai_ps2', addr, name, source FROM (
          SELECT addr, name, source, row_number() OVER (PARTITION BY addr ORDER BY
            CASE source WHEN 'readable' THEN 0 WHEN 'sce' THEN 1 WHEN 'ghidra' THEN 2 ELSE 3 END) k
          FROM names WHERE build = 'ai_ps2' AND name IS NOT NULL
            AND source IN ('readable', 'sce', 'ghidra')
          UNION ALL
          SELECT s.addr, s.name, 'readable_symbol', row_number() OVER (PARTITION BY s.addr) + 10
          FROM readable_symbols s JOIN functions f ON f.build = 'ai_ps2' AND f.addr = s.addr
          WHERE s.kind = 'constant') WHERE k = 1 OR (k = 11 AND addr NOT IN
            (SELECT addr FROM names WHERE build = 'ai_ps2' AND source IN ('readable', 'sce', 'ghidra')))""")
    con.execute("""
        INSERT INTO best_names
        SELECT DISTINCT ON (f.build, f.addr) f.build, f.addr, coalesce(n.name, an.name),
               CASE WHEN n.name IS NOT NULL THEN n.source ELSE 'ai:' || an.source END
        FROM functions f
        LEFT JOIN (SELECT build, addr, name, source FROM names
                   WHERE source IN ('sce', 'ghidra', 'xdk', 'function_id')) n
               ON n.build = f.build AND n.addr = f.addr
        LEFT JOIN counterparts c ON c.build = f.build AND c.addr = f.addr AND NOT c.inlined
        LEFT JOIN best_names an ON an.build = 'ai_ps2' AND an.addr = c.ai_addr
        WHERE f.build <> 'ai_ps2' AND coalesce(n.name, an.name) IS NOT NULL
        ORDER BY f.build, f.addr, n.name NULLS LAST""")
    con.execute("""UPDATE builds SET pairs = (SELECT count(*) FROM matches m WHERE m.a_build = 'ai_ps2'
                   AND m.b_build = builds.build) WHERE build IN ('dm2_ps2', 'bmx_ps2')""")
    # DM2 -> BMX XXX PS2 (tools/dm2bmx.py, result kind `pair`): the newest run's pairs as direct
    # links, after AI's counterparts so none of AI's chains goes through them
    con.execute("""
        INSERT INTO matches
        SELECT build, addr, b_build, b_addr, method, cls, confidence, ratio, NULL,
               json_object('ops', extra->'ops', 'callees_consistent', extra->'callees_consistent'),
               CASE extra->>'triangle' WHEN 'agree' THEN true WHEN 'disagree' THEN false END,
               'results:' || run
        FROM func_pairs WHERE build = 'dm2_ps2' AND b_build = 'bmx_ps2'
          AND run = (SELECT run FROM func_pairs WHERE build = 'dm2_ps2' AND b_build = 'bmx_ps2'
                     ORDER BY ts DESC LIMIT 1)""")
    for v in VIEWS:
        con.execute(v)


VIEWS = [
    "CREATE MACRO hexa(a) AS printf('0x%08X', a)",
    "CREATE MACRO addr_of(s) AS CAST(CASE WHEN lower(s) LIKE '0x%' THEN s ELSE '0x' || s END AS UBIGINT)",
    # every AI function with its status, coverage and counterparts at a glance
    """CREATE VIEW ai_functions AS
       SELECT f.addr, hexa(f.addr) AS address, coalesce(bn.name, f.name) AS name, f.layer, f.insns,
              m.module, m.how AS module_how, m.block, r.status, r.file,
              (SELECT count(DISTINCT route) FROM corpus c WHERE c.addr = f.addr
                 AND c.version = (SELECT max(version) FROM corpus)) AS routes,
              (SELECT count(*) FROM counterparts c WHERE c.ai_addr = f.addr AND NOT c.inlined
                 AND c.confidence IN ('high', 'medium')) AS counterparts_hm
       FROM functions f
       LEFT JOIN best_names bn ON bn.build = 'ai_ps2' AND bn.addr = f.addr
       LEFT JOIN modules m ON m.build = 'ai_ps2' AND m.func = f.addr
       LEFT JOIN readable r ON r.addr = f.addr
       WHERE f.build = 'ai_ps2'""",
    # one row per AI function and build: the counterpart, its confidence and how it was found
    """CREATE VIEW counterparts_wide AS
       SELECT f.addr, hexa(f.addr) AS address, f.layer,
         max(CASE WHEN c.build = 'dm2_ps2' THEN hexa(c.addr) || ' ' || c.confidence END) AS dm2_ps2,
         max(CASE WHEN c.build = 'bmx_ps2' THEN hexa(c.addr) || ' ' || c.confidence END) AS bmx_ps2,
         max(CASE WHEN c.build = 'ai_xbox' THEN hexa(c.addr) || ' ' || c.confidence END) AS ai_xbox,
         max(CASE WHEN c.build = 'dm2_xbox' THEN hexa(c.addr) || ' ' || c.confidence END) AS dm2_xbox,
         max(CASE WHEN c.build = 'bmx_xbox' THEN hexa(c.addr) || ' ' || c.confidence END) AS bmx_xbox,
         max(CASE WHEN c.build = 'ai_gc' THEN hexa(c.addr) || ' ' || c.confidence END) AS ai_gc,
         max(CASE WHEN c.build = 'dm2_gc' THEN hexa(c.addr) || ' ' || c.confidence END) AS dm2_gc,
         max(CASE WHEN c.build = 'bmx_gc' THEN hexa(c.addr) || ' ' || c.confidence END) AS bmx_gc
       FROM functions f LEFT JOIN counterparts c ON c.ai_addr = f.addr
       WHERE f.build = 'ai_ps2' GROUP BY f.addr, f.layer""",
    # the per-function card as one row (workdb.py card prints the long form)
    """CREATE VIEW card AS
       SELECT a.*, (SELECT list(struct_pack(build := c.build, addr := hexa(c.addr),
                                 confidence := c.confidence, via := c.via, method := c.method)
                                 ORDER BY c.build)
                    FROM counterparts c WHERE c.ai_addr = a.addr) AS counterparts,
              (SELECT list(DISTINCT n.name || ' (' || n.source || ')')
                 FROM names n WHERE n.build = 'ai_ps2' AND n.addr = a.addr AND n.name IS NOT NULL)
                 AS names_ai,
              (SELECT list(DISTINCT n.name || ' (' || c.build || ' ' || n.source || ')')
                 FROM counterparts c JOIN names n ON n.build = c.build AND n.addr = c.addr
                 WHERE c.ai_addr = a.addr AND n.name IS NOT NULL) AS names_elsewhere,
              (SELECT list(DISTINCT s.text) FROM func_strings s
                 WHERE s.build = 'ai_ps2' AND s.func = a.addr) AS strings,
              (SELECT list(c.route || ':' || c.calls ORDER BY c.route) FROM corpus c
                 WHERE c.addr = a.addr AND c.version = (SELECT max(version) FROM corpus)) AS corpus
       FROM ai_functions a""",
    # name candidates for AI functions from every build (through the counterparts)
    """CREATE VIEW ai_names AS
       SELECT n.addr AS ai_addr, 'ai_ps2' AS build, n.addr, n.name, n.source, n.kind,
              n.confidence, NULL AS pair_confidence, n.evidence
       FROM names n WHERE n.build = 'ai_ps2'
       UNION ALL
       SELECT c.ai_addr, n.build, n.addr, n.name, n.source, n.kind, n.confidence,
              c.confidence, n.evidence
       FROM counterparts c JOIN names n ON n.build = c.build AND n.addr = c.addr""",
    # functions no route of the newest corpus reaches (registered or not)
    """CREATE VIEW uncovered AS
       SELECT a.addr, a.address, a.name, a.layer, a.insns, a.status, a.module
       FROM ai_functions a
       WHERE a.layer <> 'sdk' AND NOT EXISTS (SELECT 1 FROM corpus c WHERE c.addr = a.addr
             AND c.version = (SELECT max(version) FROM corpus))""",
    # how much of each layer has a counterpart in each build, by confidence (and by size)
    """CREATE VIEW carry_by_layer AS
       WITH best AS (
         SELECT ai_addr, build, max(CASE confidence WHEN 'high' THEN 4 WHEN 'medium' THEN 3
                                     WHEN 'low' THEN 2 WHEN 'inlined' THEN 1 END) AS r
         FROM counterparts GROUP BY ai_addr, build),
       b AS (SELECT DISTINCT build FROM counterparts)
       SELECT f.layer, b.build, count(*) AS functions,
              count(*) FILTER (WHERE best.r = 4) AS high,
              count(*) FILTER (WHERE best.r = 3) AS medium,
              count(*) FILTER (WHERE best.r = 2) AS low,
              count(*) FILTER (WHERE best.r = 1) AS inlined,
              count(*) FILTER (WHERE best.r IS NULL) AS none,
              round(100.0 * sum(f.insns) FILTER (WHERE best.r >= 3) / sum(f.insns), 1)
                AS pct_size_high_medium,
              round(100.0 * sum(f.insns) FILTER (WHERE best.r >= 2) / sum(f.insns), 1)
                AS pct_size_paired
       FROM functions f CROSS JOIN b
       LEFT JOIN best ON best.ai_addr = f.addr AND best.build = b.build
       WHERE f.build = 'ai_ps2'
       GROUP BY f.layer, b.build ORDER BY f.layer, b.build""",
    # the same for the readable registrations, by status
    """CREATE VIEW carry_by_status AS
       WITH best AS (
         SELECT ai_addr, build, max(CASE confidence WHEN 'high' THEN 4 WHEN 'medium' THEN 3
                                     WHEN 'low' THEN 2 WHEN 'inlined' THEN 1 END) AS r
         FROM counterparts GROUP BY ai_addr, build),
       b AS (SELECT DISTINCT build FROM counterparts)
       SELECT coalesce(r.status, 'none') AS status, b.build, count(*) AS functions,
              count(*) FILTER (WHERE best.r = 4) AS high,
              count(*) FILTER (WHERE best.r = 3) AS medium,
              count(*) FILTER (WHERE best.r = 2) AS low,
              count(*) FILTER (WHERE best.r = 1) AS inlined,
              count(*) FILTER (WHERE best.r IS NULL) AS none
       FROM functions f CROSS JOIN b
       LEFT JOIN readable r ON r.addr = f.addr
       LEFT JOIN best ON best.ai_addr = f.addr AND best.build = b.build
       WHERE f.build = 'ai_ps2' AND f.layer <> 'sdk'
       GROUP BY 1, b.build ORDER BY 1, b.build""",
    # the sweep: the newest result of each kind per function
    """CREATE VIEW sweep_latest AS
       WITH d AS (SELECT DISTINCT ON (addr) addr, run, generator, result, reason, ts
                  FROM sweep_draft ORDER BY addr, ts DESC),
            bu AS (SELECT DISTINCT ON (addr) addr, run, result, error, ts
                   FROM sweep_build ORDER BY addr, ts DESC),
            ck AS (SELECT addr, run, count(*) FILTER (WHERE result = 'pass') AS pass,
                          count(*) FILTER (WHERE result = 'fail') AS fail,
                          count(*) FILTER (WHERE result NOT IN ('pass', 'fail')) AS other,
                          max(ts) AS ts
                   FROM (SELECT DISTINCT ON (addr, route) * FROM sweep_check
                         ORDER BY addr, route, ts DESC) GROUP BY addr, run),
            fa AS (SELECT DISTINCT ON (addr) addr, stage, failure_class, detail, ts
                   FROM sweep_failure ORDER BY addr, ts DESC),
            st AS (SELECT DISTINCT ON (addr) addr, status, ts FROM sweep_status
                   ORDER BY addr, ts DESC)
       SELECT a.addr, a.address, a.name, a.layer, a.status AS registered_status,
              d.result AS draft, d.generator, bu.result AS build, ck.pass, ck.fail, ck.other,
              fa.stage AS failure_stage, fa.failure_class, fa.detail AS failure_detail,
              st.status AS sweep_status,
              greatest(d.ts, bu.ts, ck.ts, fa.ts, st.ts) AS last_ts
       FROM ai_functions a LEFT JOIN d USING (addr) LEFT JOIN bu USING (addr)
       LEFT JOIN ck USING (addr) LEFT JOIN fa USING (addr) LEFT JOIN st USING (addr)
       WHERE coalesce(d.addr, bu.addr, ck.addr, fa.addr, st.addr) IS NOT NULL""",
    # mapped functions of any build: the newest line per function, its pairs made outside the match
    """CREATE VIEW func_map_latest AS
       SELECT m.build, m.addr, hexa(m.addr) AS address, m.name, m.role, m.port, m.confidence, m.module,
              m.covered, m.size, m.callers, m.callees, m.note, m.counterparts, m.run, m.ts
       FROM (SELECT DISTINCT ON (build, addr) * FROM func_map ORDER BY build, addr, ts DESC) m""",
    """CREATE VIEW func_pairs_latest AS
       SELECT DISTINCT ON (build, addr, b_build) build, addr, hexa(addr) AS address, b_build, b_addr,
              hexa(b_addr) AS b_address, method, cls, confidence, ratio, run, ts
       FROM func_pairs ORDER BY build, addr, b_build, ts DESC""",
    # the engine's lineage: one row per engine function across ZGE2 (DM2 PS2), ZGE3 (AI PS2) and ZGE4
    # (BMX XXX PS2), from the direct links (AI -> DM2, AI -> BMX XXX, DM2 -> BMX XXX; a version the
    # direct link misses is taken through the third game's link). Engine: below the engine's end
    # (DM2 0x163280, AI 0x185D20, BMX XXX 0x190340: the source-path anchors' cut, zgui.c the last
    # module) and not sdk, or an AI function whose readable file is in zge/. Each step's class is
    # identical when the masked bodies are equal (so through a third game too), else the direct
    # pair's class (close, drifted, weak), NULL with no direct pair. "Not in an ELF" means that
    # build links no counterpart (BMX XXX's link drops unreferenced functions), not that its
    # source lost it.
    """CREATE VIEW engine_lineage AS
       WITH
       ai_eng AS (
         SELECT f.addr, r.file FROM functions f LEFT JOIN readable r ON r.addr = f.addr
         WHERE f.build = 'ai_ps2' AND f.layer <> 'sdk' AND (f.addr < addr_of('0x185D20') OR r.file LIKE 'zge/%')),
       ad AS (SELECT a_addr, b_addr, cls FROM matches WHERE a_build = 'ai_ps2' AND b_build = 'dm2_ps2'),
       ax AS (SELECT a_addr, b_addr, cls FROM matches WHERE a_build = 'ai_ps2' AND b_build = 'bmx_ps2'),
       dx AS (SELECT a_addr, b_addr, cls FROM matches WHERE a_build = 'dm2_ps2' AND b_build = 'bmx_ps2'),
       r3 AS (
         SELECT coalesce(ad.b_addr, dxr.a_addr) AS zge2, e.addr AS zge3,
                coalesce(ax.b_addr, CASE WHEN dxd.b_addr NOT IN (SELECT b_addr FROM ax) THEN dxd.b_addr END) AS zge4,
                ad.cls AS c23, ax.cls AS c34, e.file
         FROM ai_eng e LEFT JOIN ad ON ad.a_addr = e.addr LEFT JOIN ax ON ax.a_addr = e.addr
         LEFT JOIN dx dxd ON dxd.a_addr = ad.b_addr
         LEFT JOIN dx dxr ON ad.b_addr IS NULL AND dxr.b_addr = ax.b_addr
                          AND dxr.a_addr NOT IN (SELECT b_addr FROM ad)),
       r2 AS (
         SELECT f.addr AS zge2, NULL::UBIGINT AS zge3, dx.b_addr AS zge4, NULL AS c23, NULL AS c34, NULL AS file
         FROM functions f LEFT JOIN dx ON dx.a_addr = f.addr
         WHERE f.build = 'dm2_ps2' AND f.layer <> 'sdk' AND f.addr < addr_of('0x163280')
           AND f.addr NOT IN (SELECT b_addr FROM ad) AND f.addr NOT IN (SELECT zge2 FROM r3 WHERE zge2 IS NOT NULL)
           AND (dx.b_addr IS NULL OR dx.b_addr NOT IN (SELECT zge4 FROM r3 WHERE zge4 IS NOT NULL))),
       r4 AS (
         SELECT NULL::UBIGINT AS zge2, NULL::UBIGINT AS zge3, f.addr AS zge4, NULL AS c23, NULL AS c34, NULL AS file
         FROM functions f WHERE f.build = 'bmx_ps2' AND f.layer <> 'sdk' AND f.addr < addr_of('0x190340')
           AND f.addr NOT IN (SELECT b_addr FROM ax) AND f.addr NOT IN (SELECT b_addr FROM dx)
           AND f.addr NOT IN (SELECT zge4 FROM r3 WHERE zge4 IS NOT NULL)),
       al AS (SELECT * FROM r3 UNION ALL SELECT * FROM r2 UNION ALL SELECT * FROM r4),
       g AS (
         SELECT al.*, d.hash AS h2, a.hash AS h3, b.hash AS h4, d.insns AS n2, a.insns AS n3, b.insns AS n4,
                dx.cls AS c24, coalesce(na.name, nd.name, nb.name) AS name
         FROM al LEFT JOIN functions d ON d.build = 'dm2_ps2' AND d.addr = al.zge2
         LEFT JOIN functions a ON a.build = 'ai_ps2' AND a.addr = al.zge3
         LEFT JOIN functions b ON b.build = 'bmx_ps2' AND b.addr = al.zge4
         LEFT JOIN dx ON dx.a_addr = al.zge2 AND dx.b_addr = al.zge4
         LEFT JOIN best_names na ON na.build = 'ai_ps2' AND na.addr = al.zge3
         LEFT JOIN best_names nd ON nd.build = 'dm2_ps2' AND nd.addr = al.zge2
         LEFT JOIN best_names nb ON nb.build = 'bmx_ps2' AND nb.addr = al.zge4)
       SELECT zge2, zge3, zge4, hexa(zge2) AS zge2_address, hexa(zge3) AS zge3_address,
         hexa(zge4) AS zge4_address, name,
         CASE WHEN h2 = h3 THEN 'identical' ELSE c23 END AS zge2_zge3,
         CASE WHEN h3 = h4 THEN 'identical' ELSE c34 END AS zge3_zge4,
         CASE WHEN h2 = h4 THEN 'identical' ELSE c24 END AS zge2_zge4,
         CASE
           WHEN zge2 IS NOT NULL AND zge3 IS NOT NULL AND zge4 IS NOT NULL THEN
             CASE WHEN h2 = h3 AND h3 = h4 THEN 'the same in all three'
                  WHEN h3 = h4 THEN 'changed in ZGE3, kept in ZGE4'
                  WHEN h2 = h3 THEN 'kept in ZGE3, changed in ZGE4'
                  WHEN h2 = h4 THEN 'changed in ZGE3, ZGE4 has the ZGE2 body'
                  ELSE 'changed in ZGE3 and in ZGE4' END
           WHEN zge2 IS NOT NULL AND zge3 IS NOT NULL THEN
             CASE WHEN h2 = h3 THEN 'kept in ZGE3, not in ZGE4''s ELF'
                  ELSE 'changed in ZGE3, not in ZGE4''s ELF' END
           WHEN zge3 IS NOT NULL AND zge4 IS NOT NULL THEN
             CASE WHEN h3 = h4 THEN 'new in ZGE3, kept in ZGE4' ELSE 'new in ZGE3, changed in ZGE4' END
           WHEN zge2 IS NOT NULL AND zge4 IS NOT NULL THEN
             CASE WHEN h2 = h4 THEN 'not in ZGE3''s ELF, kept in ZGE4'
                  ELSE 'not in ZGE3''s ELF, changed in ZGE4' END
           WHEN zge3 IS NOT NULL THEN 'ZGE3 only'
           WHEN zge2 IS NOT NULL THEN 'ZGE2 only'
           ELSE 'ZGE4 only' END AS lineage,
         n2 AS zge2_insns, n3 AS zge3_insns, n4 AS zge4_insns, file
       FROM g""",
    # path coverage: the newest coverage line per function, with its share
    """CREATE VIEW coverage_latest AS
       SELECT c.game, c.addr, hexa(c.addr) AS address, a.name, a.layer, a.status, c.blocks, c.hit,
              CASE WHEN c.blocks > 0 THEN round(c.hit / c.blocks, 4) END AS share, c.calls, c.files,
              c.uncovered, c.corpus, c.run, c.ts
       FROM (SELECT DISTINCT ON (coalesce(game, 'ai'), addr) *, coalesce(game, 'ai') AS g FROM sweep_coverage
             ORDER BY coalesce(game, 'ai'), addr, ts DESC) c
       LEFT JOIN ai_functions a ON a.addr = c.addr AND c.g = 'ai'""",
    # Verified registrations whose current source digest has no passing check (a pass counts
    # only for the code it was made against): never checked with a digest, or edited since
    """CREATE VIEW stale_verified AS
       WITH p AS (SELECT coalesce(game, 'ai') AS game, addr, impl, max(ts) AS ts, count(*) AS passes FROM sweep_check
                  WHERE result = 'pass' AND impl IS NOT NULL
                  GROUP BY 1, addr, impl),
            last AS (SELECT DISTINCT ON (game, addr) game, addr, impl AS last_impl, ts AS last_pass FROM p
                     ORDER BY game, addr, ts DESC)
       SELECT d.game, d.addr, hexa(d.addr) AS address, d.name, d.file, d.digest,
              last.last_impl, last.last_pass,
              CASE WHEN last.addr IS NULL THEN 'no pass with a digest' ELSE 'edited since its last pass' END
                AS reason
       FROM readable_digests d LEFT JOIN last ON last.addr = d.addr AND last.game = d.game
       WHERE d.status = 'Verified'
         AND NOT EXISTS (SELECT 1 FROM p WHERE p.addr = d.addr AND p.game = d.game AND p.impl = d.digest)""",
    # failures grouped by class (the sweep's loop: fix the generator for the biggest group)
    """CREATE VIEW sweep_failure_classes AS
       SELECT stage, failure_class, count(DISTINCT addr) AS functions, min(detail) AS example
       FROM (SELECT DISTINCT ON (addr) * FROM sweep_failure ORDER BY addr, ts DESC)
       GROUP BY stage, failure_class ORDER BY functions DESC""",
]


def build(argv):
    os.makedirs(WORKDB, exist_ok=True)
    lock = open(os.path.join(WORKDB, '.build.lock'), 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit('workdb: another build is running')
    mine = []
    try:
        _build(argv, mine)
    except BaseException:
        # leave no half-made files behind (this build holds the lock: they are its own)
        import shutil
        for p in mine:
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            elif os.path.exists(p):
                os.remove(p)
        raise


def _build(argv, mine):
    readable = exports.default_readable()
    if '--base-exports' in argv:
        argv.remove('--base-exports')
        readable = False
    use_exports(readable, warn=True)
    os.makedirs(os.path.join(WORKDB, 'results'), exist_ok=True)
    t0 = datetime.datetime.now()
    tmp = tempfile.mkdtemp(prefix='build-', dir=WORKDB)
    mine.append(tmp)
    W = Writer(tmp)
    ctx = dict(missing={}, sides={}, data={}, sce={}, elf={}, pairs={}, ai_layer_of={})
    for n, p in sorted(SOURCES.items()):
        for q in (p.split(';') if n == 'kstr.files' else [p]):
            ok = os.path.exists(q)
            st = os.stat(q) if ok else None
            W.add('sources', [n, q, ok, st.st_size if st else None,
                              datetime.datetime.fromtimestamp(st.st_mtime) if st else None])
    load_ps2(W, ctx)
    load_match3(W, ctx)
    load_other(W, ctx)
    load_readable(W, ctx)
    load_corpus(W, ctx)
    load_history(W, ctx)
    load_strings(W, ctx)
    load_names(W, ctx)
    ingest_results(W, ctx)
    for b, game, plat, cpu, exe, bounds in BUILDS:
        S = ctx['sides'].get(b)
        P = ctx['pairs'].get(b)
        to = GAME_PS2[game] if plat != 'ps2' else ('ai_ps2' if b != 'ai_ps2' else None)
        W.add('builds', [b, game, plat, cpu, exe, S is not None, len(S) if S else 0,
                         to if (P or plat == 'ps2' and b != 'ai_ps2') else None,
                         len(P['pairs']) if P else None,
                         bounds[0] if bounds else None, bounds[1] if bounds else None,
                         bounds[2] if bounds else None, ctx['missing'].get(b)])
    out = DB + '.tmp-%d' % os.getpid()
    mine.append(out)
    if os.path.exists(out):
        os.remove(out)
    con = duckdb.connect(out)
    con.execute("SET memory_limit = '4GB'")
    log('loading into', out)
    W.load(con)
    con.execute("CREATE TEMP TABLE conf_rank AS SELECT * FROM (VALUES ('high', 3), ('medium', 2), "
                "('low', 1)) t(c, r)")
    finish(con, ctx)
    rev = ''
    try:
        rev = subprocess.run(['git', '-C', GAME_SRC, 'log', '-1', '--format=%h %s'],
                             capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    here = ''
    try:
        here = subprocess.run(['git', '-C', HERE, 'log', '-1', '--format=%h'],
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    for k, v in (('schema_version', VERSION), ('built', t0.strftime('%Y-%m-%d %H:%M:%S')),
                 ('build_seconds', round((datetime.datetime.now() - t0).total_seconds(), 1)),
                 ('game_src', GAME_SRC), ('game_src_commit', rev), ('workdb_commit', here),
                 ('missing', json.dumps(ctx['missing'])),
                 ('exports', 'readable' if readable else 'base')):
        con.execute('INSERT INTO meta VALUES (?, ?)', [k, str(v)])
    con.execute('CHECKPOINT')
    con.close()
    os.replace(out, DB)
    for f in os.listdir(tmp):
        os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    log('done:', DB, '(%.0f s)' % (datetime.datetime.now() - t0).total_seconds())
    for k, v in ctx['missing'].items():
        log('missing:', k, '-', v)


# ------------------------------------------------------------------ readers

def connect():
    if duckdb is None:
        sys.exit('workdb: needs the duckdb module (%s); card and notes read the public notes export without it' % VENV_PY)
    if not os.path.exists(DB):
        sys.exit('workdb: no database at %s (run: workdb.py build)' % DB)
    return duckdb.connect(DB, read_only=True)


def cmd_query(argv):
    fmt = 'table'
    for f in ('--csv', '--json'):
        if f in argv:
            argv.remove(f)
            fmt = f[2:]
    if not argv:
        sys.exit('usage: workdb.py query "SQL" [--csv|--json]')
    con = connect()
    cur = con.execute(hex_literals(' '.join(argv)))
    cols = [d[0] for d in cur.description]
    rows = cur.fetchall()
    if fmt == 'json':
        print(json.dumps([dict(zip(cols, r)) for r in rows], default=str, indent=1))
    elif fmt == 'csv':
        w = csv.writer(sys.stdout)
        w.writerow(cols)
        w.writerows(rows)
    else:
        print_table(cols, rows)


def hex_literals(sql):
    """0x1A2B outside quotes -> its decimal value (DuckDB has no hex literals)."""
    parts = re.split(r"('(?:[^']|'')*')", sql)
    return ''.join(p if i % 2 else re.sub(r'\b0[xX]([0-9A-Fa-f]+)\b',
                                          lambda m: str(int(m.group(1), 16)), p)
                   for i, p in enumerate(parts))


def print_table(cols, rows, maxw=60):
    def cell(v):
        s = '' if v is None else str(v)
        return s if len(s) <= maxw else s[:maxw - 3] + '...'
    cs = [[cell(v) for v in r] for r in rows]
    ws = [max([len(c)] + [len(r[i]) for r in cs]) for i, c in enumerate(cols)]
    print('  '.join(c.ljust(w) for c, w in zip(cols, ws)).rstrip())
    print('  '.join('-' * w for w in ws))
    for r in cs:
        print('  '.join(v.ljust(w) for v, w in zip(r, ws)).rstrip())
    print('(%d rows)' % len(rows))


def cmd_tables(argv):
    con = connect()
    for (t, ty) in con.execute("SELECT table_name, table_type FROM information_schema.tables "
                               "ORDER BY table_type, table_name").fetchall():
        n = con.execute('SELECT count(*) FROM "%s"' % t).fetchone()[0]
        print('%-6s %-24s %9d' % ('view' if ty == 'VIEW' else 'table', t, n))
    print()
    for k, v in con.execute('SELECT key, value FROM meta').fetchall():
        print('%-16s %s' % (k, v))


def cmd_schema(argv):
    con = connect()
    q = "SELECT table_name, column_name, data_type FROM information_schema.columns"
    rows = con.execute(q + (" WHERE table_name = ?" if argv else "") +
                       " ORDER BY table_name, ordinal_position", argv[:1]).fetchall()
    last = None
    for t, c, ty in rows:
        if t != last:
            print(t)
            last = t
        print('    %-20s %s' % (c, ty))


def card_key(con, key, build=None):
    """(build, addr) of the function a card is about: an address (AI's; BUILD's with --build), a name AI's
    function or one of its counterparts has, else a name only another PS2 build's function has (BMX XXX's own
    editor functions: no AI counterpart). A function with an AI counterpart is AI's card, which shows it."""
    b = build or 'ai_ps2'
    a, row = None, None
    try:
        a = parse_addr(key)
        row = con.execute("SELECT build, addr FROM functions WHERE build = ? AND addr = ?", [b, a]).fetchone()
    except ValueError:
        pass
    if not row and a is None:
        if b == 'ai_ps2':
            # (a function's name: a global's or a resume point's is not a card)
            row = con.execute("SELECT n.build, n.addr FROM names n JOIN functions f ON f.build = n.build AND "
                              "f.addr = n.addr WHERE n.build = 'ai_ps2' AND n.name = ? "
                              "ORDER BY n.source = 'readable' DESC LIMIT 1", [key]).fetchone()
            if not row and not build:
                row = con.execute("SELECT 'ai_ps2', n.ai_addr FROM ai_names n JOIN functions f ON "
                                  "f.build = 'ai_ps2' AND f.addr = n.ai_addr WHERE n.name = ? LIMIT 1",
                                  [key]).fetchone()
        if not row:
            # (another PS2 build's own name: its registration first, then its symbol table's row)
            row = con.execute("""SELECT n.build, n.addr FROM names n JOIN functions f ON f.build = n.build
                                 AND f.addr = n.addr WHERE n.name = ? AND n.kind = 'function'
                                 AND n.build IN ('dm2_ps2', 'bmx_ps2') AND (? IS NULL OR n.build = ?)
                                 ORDER BY n.source = 'readable' DESC, n.build DESC LIMIT 1""",
                              [key, build, build]).fetchone()
    if not row and a is not None:
        # inside a function
        row = con.execute("SELECT build, addr FROM functions WHERE build = ? AND addr <= ? AND end_addr > ? "
                          "ORDER BY addr DESC LIMIT 1", [b, a, a]).fetchone()
    if row and row[0] != 'ai_ps2':
        ai = con.execute("SELECT ai_addr FROM counterparts WHERE build = ? AND addr = ? AND NOT inlined LIMIT 1",
                         list(row)).fetchone()
        if ai:
            row = ('ai_ps2', ai[0])
    return tuple(row) if row else None


def card_data(con, key, build=None):
    """Everything about one AI function, as a dict (or about another PS2 build's function that has no AI
    counterpart: other_card_data)."""
    row = card_key(con, key, build)
    if not row:
        return None
    if row[0] != 'ai_ps2':
        return other_card_data(con, *row)
    a = row[1]

    def q(sql, args=()):
        cur = con.execute(sql, list(args))
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]

    d = q("""SELECT f.*, bn.name AS best_name, m.module, m.path AS module_path, m.how AS module_how,
                    m.block, m.candidates, r.name AS readable_name, r.status, r.file, r.line, r.kind,
                    r.doc
             FROM functions f
             LEFT JOIN best_names bn ON bn.build = f.build AND bn.addr = f.addr
             LEFT JOIN modules m ON m.build = f.build AND m.func = f.addr
             LEFT JOIN readable r ON r.addr = f.addr
             WHERE f.build = 'ai_ps2' AND f.addr = ?""", [a])[0]
    if d['readable_name'] is None:
        doc = q("SELECT evidence FROM names WHERE build = 'ai_ps2' AND addr = ? AND "
                "source = 'readable_doc'", [a])
        d['doc'] = doc[0]['evidence'] if doc else None
    if d['block'] is not None:
        blk = q("SELECT * FROM blocks WHERE build = 'ai_ps2' AND block = ?", [d['block']])
        d['block_info'] = blk[0] if blk else None
    d['counterparts'] = q("""
        SELECT c.*, f.name AS fname, f.insns, f.disps, f.floats
        FROM counterparts c LEFT JOIN functions f ON f.build = c.build AND f.addr = c.addr
        WHERE c.ai_addr = ? ORDER BY c.build, c.inlined, c.addr""", [a])
    d['names'] = q("""SELECT * FROM ai_names WHERE ai_addr = ? AND (name IS NOT NULL OR
                       source IN ('readable_doc', 'string_key'))
                       ORDER BY build <> 'ai_ps2', source, name""", [a])
    builds = ['ai_ps2'] + sorted({c['build'] for c in d['counterparts']})
    strs = []
    for c in [dict(build='ai_ps2', addr=a)] + [c for c in d['counterparts'] if not c['inlined']]:
        for s in q("SELECT text, is_path FROM func_strings WHERE build = ? AND func = ?",
                   [c['build'], c['addr']]):
            strs.append((c['build'], s['text'], s['is_path']))
    d['strings'] = strs
    d['files'] = q("""SELECT DISTINCT fa.build, fa.path, fa.source FROM file_anchors fa
                      WHERE (fa.build, fa.func) IN (SELECT 'ai_ps2', ?::UBIGINT UNION ALL
                             SELECT build, addr FROM counterparts WHERE ai_addr = ? AND NOT inlined)
                      ORDER BY 1, 2""", [a, a])
    d['corpus'] = q("""SELECT version, route, calls, callee_calls, vsync_first, vsync_last, cut_short
                       FROM corpus WHERE addr = ? AND version = (SELECT max(version) FROM corpus)
                       ORDER BY route""", [a])
    d['corpus_versions'] = [r['version'] for r in q(
        "SELECT DISTINCT version FROM corpus WHERE addr = ? ORDER BY 1", [a])]
    d['callers'] = q("""SELECT DISTINCT c.caller AS addr, coalesce(bn.name, f.name) AS name
                        FROM calls c JOIN functions f ON f.build = c.build AND f.addr = c.caller
                        LEFT JOIN best_names bn ON bn.build = c.build AND bn.addr = c.caller
                        WHERE c.build = 'ai_ps2' AND c.callee = ? ORDER BY 1""", [a])
    d['callees'] = q("""SELECT c.callee AS addr, coalesce(bn.name, f.name) AS name, min(c.call_offset) o
                        FROM calls c LEFT JOIN functions f ON f.build = c.build AND f.addr = c.callee
                        LEFT JOIN best_names bn ON bn.build = c.build AND bn.addr = c.callee
                        WHERE c.build = 'ai_ps2' AND c.caller = ? GROUP BY 1, 2 ORDER BY o""", [a])
    d['globals'] = q("""SELECT g.addr, (SELECT string_agg(DISTINCT s.name, '/') FROM readable_symbols s
                                WHERE s.addr = g.addr AND s.kind = 'global') AS name
                        FROM (SELECT DISTINCT addr FROM func_globals WHERE build = 'ai_ps2'
                              AND func = ?) g ORDER BY 1""", [a])
    d['tuning'] = [n for n in d['names'] if n['source'] == 'tuning' and n['build'] == 'ai_ps2']
    d['sweep'] = q("SELECT * FROM sweep_latest WHERE addr = ?", [a])
    # (a database built before schema 3 has neither view)
    views = {r[0] for r in con.execute("SELECT view_name FROM duckdb_views()").fetchall()}
    d['paths'] = q("SELECT blocks, hit, calls, uncovered, run FROM coverage_latest "
                   "WHERE game = 'ai' AND addr = ?", [a]) if 'coverage_latest' in views else []
    # (stale_verified has a game column from the DM2/BMX gate on; a database built before it has AI's rows alone)
    stale_cols = {r[0] for r in con.execute("SELECT column_name FROM duckdb_columns() WHERE table_name = 'stale_verified'").fetchall()}
    d['stale'] = q("SELECT reason FROM stale_verified WHERE addr = ?" + (" AND game = 'ai'" if 'game' in stale_cols else ""),
                   [a]) if 'stale_verified' in views else []
    d['lineage'] = q("SELECT * FROM engine_lineage WHERE zge3 = ?", [a]) if 'engine_lineage' in views else []
    d['notes'] = fetch_notes(con, a, [(c['build'], c['addr']) for c in d['counterparts'] if not c['inlined']])
    d['built'] = con.execute("SELECT value FROM meta WHERE key = 'built'").fetchone()[0]
    d['build'] = 'ai_ps2'
    return d


def other_card_data(con, b, a):
    """Everything about function `a` of PS2 build `b` (DM2 or BMX XXX) with no AI counterpart, as card_data's
    dict: its own name, registration and symbol-table row, its calls, strings and notes, the matches the other
    builds have of it. What the database has of AI's functions alone (module, block, corpus, sweep) is empty."""

    def q(sql, args=()):
        cur = con.execute(sql, list(args))
        cols = [x[0] for x in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]

    game = BUILD[b][1]
    d = q("""SELECT f.*, bn.name AS best_name FROM functions f
             LEFT JOIN best_names bn ON bn.build = f.build AND bn.addr = f.addr
             WHERE f.build = ? AND f.addr = ?""", [b, a])[0]
    d.update(build=b, module=None, module_path=None, module_how=None, block=None, candidates=None,
             readable_name=None, status=None, file=None, line=None, kind=None, doc=None, corpus=[],
             corpus_versions=[], sweep=[], tuning=[])
    d['names'] = q("""SELECT ? AS ai_addr, * FROM names WHERE build = ? AND addr = ? AND name IS NOT NULL
                      ORDER BY source, name""", [a, b, a])
    reg = [n for n in d['names'] if n['source'] == 'readable']
    m = re.match(r'(\w+), (.+):(\d+)$', reg[0]['evidence'] or '') if reg else None
    if m:
        d.update(readable_name=reg[0]['name'], status=m.group(1), file=m.group(2), line=int(m.group(3)))
    own = [n for n in d['names'] if n['source'] in ('readable', 'readable_symbols') and n['kind'] == 'function']
    if own and not d['best_name']:
        d['best_name'] = own[0]['name']
    # the other builds' matches of it (the counterparts are AI's: this function has none)
    d['counterparts'] = q("""
        SELECT m.b_build AS build, m.b_addr AS addr, m.confidence, m.cls, m.method, NULL AS via, m.score,
               false AS inlined, f.name AS fname, f.insns, f.disps, f.floats
        FROM matches m LEFT JOIN functions f ON f.build = m.b_build AND f.addr = m.b_addr
        WHERE m.a_build = ? AND m.a_addr = ?
        UNION ALL
        SELECT m.a_build, m.a_addr, m.confidence, m.cls, m.method, NULL, m.score, false, f.name, f.insns,
               f.disps, f.floats
        FROM matches m LEFT JOIN functions f ON f.build = m.a_build AND f.addr = m.a_addr
        WHERE m.b_build = ? AND m.b_addr = ? ORDER BY 1, 2""", [b, a, b, a])
    d['strings'] = [(c['build'], s['text'], s['is_path']) for c in [dict(build=b, addr=a)] + d['counterparts']
                    for s in q("SELECT text, is_path FROM func_strings WHERE build = ? AND func = ?",
                               [c['build'], c['addr']])]
    d['files'] = q("SELECT DISTINCT build, path, source FROM file_anchors WHERE build = ? AND func = ? "
                   "ORDER BY 1, 2", [b, a])
    d['callers'] = q("""SELECT DISTINCT c.caller AS addr, coalesce(bn.name, f.name) AS name
                        FROM calls c JOIN functions f ON f.build = c.build AND f.addr = c.caller
                        LEFT JOIN best_names bn ON bn.build = c.build AND bn.addr = c.caller
                        WHERE c.build = ? AND c.callee = ? ORDER BY 1""", [b, a])
    d['callees'] = q("""SELECT c.callee AS addr, coalesce(bn.name, f.name) AS name, min(c.call_offset) o
                        FROM calls c LEFT JOIN functions f ON f.build = c.build AND f.addr = c.callee
                        LEFT JOIN best_names bn ON bn.build = c.build AND bn.addr = c.callee
                        WHERE c.build = ? AND c.caller = ? GROUP BY 1, 2 ORDER BY o""", [b, a])
    # (a callee or caller with no name of its own: the build's own symbol-table name)
    for c in d['callers'] + d['callees']:
        if not is_named(c['name']):
            n = con.execute("SELECT name FROM names WHERE build = ? AND addr = ? AND kind = 'function' AND "
                            "source IN ('readable', 'readable_symbols') LIMIT 1", [b, c['addr']]).fetchone()
            c['name'] = n[0] if n else c['name']
    d['globals'] = q("""SELECT g.addr, (SELECT string_agg(DISTINCT n.name, '/') FROM names n WHERE n.build = ?
                                AND n.addr = g.addr AND n.kind = 'datum') AS name
                        FROM (SELECT DISTINCT addr FROM func_globals WHERE build = ? AND func = ?) g
                        ORDER BY 1""", [b, b, a])
    views = {r[0] for r in con.execute("SELECT view_name FROM duckdb_views()").fetchall()}
    d['paths'] = q("SELECT blocks, hit, calls, uncovered, run FROM coverage_latest WHERE game = ? AND addr = ?",
                   [game, a]) if 'coverage_latest' in views else []
    stale_cols = {r[0] for r in con.execute("SELECT column_name FROM duckdb_columns() "
                                            "WHERE table_name = 'stale_verified'").fetchall()}
    d['stale'] = q("SELECT reason FROM stale_verified WHERE addr = ? AND game = ?", [a, game]) \
        if 'stale_verified' in views and 'game' in stale_cols else []
    col = {'dm2': 'zge2', 'bmx': 'zge4'}[game]
    d['lineage'] = q("SELECT * FROM engine_lineage WHERE %s = ?" % col, [a]) if 'engine_lineage' in views else []
    d['notes'] = fetch_notes(con, a, [(c['build'], c['addr']) for c in d['counterparts']], build=b)
    d['built'] = con.execute("SELECT value FROM meta WHERE key = 'built'").fetchone()[0]
    return d


def fetch_notes(con, a, others=(), build='ai_ps2'):
    """The notes of AI function `a` (BUILD's: other_card_data) and, labelled by build, of its counterparts, one
    per distinct (build, topic, text), in topic order, oldest first."""
    if not con.execute("SELECT count(*) FROM information_schema.tables WHERE table_name = 'func_notes'"
                       ).fetchone()[0]:
        return []     # a database built before schema 4
    keys = [(build, a)] + list(others)
    cond = ' OR '.join('(build = ? AND addr = ?)' for _ in keys)
    args = [x for k in keys for x in k]
    cur = con.execute("""SELECT build, addr, topic, text, agent, source, ts FROM (
            SELECT DISTINCT ON (build, addr, topic, text) * FROM func_notes WHERE %s
            ORDER BY build, addr, topic, text, ts) ORDER BY build <> ?, build, addr,
            list_position(?, topic), ts""" % cond, args + [build, list(NOTE_TOPICS)])
    cols = [x[0] for x in cur.description]
    return [dict(zip(cols, r)) for r in cur.fetchall()]


def fmt_card(d, prefix='// ', width=100):
    out = []
    W = width - len(prefix)

    def line(s, indent=''):
        import textwrap
        for i, t in enumerate(textwrap.wrap(s, W, initial_indent='' if not out or not indent else '',
                                            subsequent_indent=indent + '  ',
                                            break_long_words=True, break_on_hyphens=False) or ['']):
            out.append(prefix + t)

    a = d['addr']
    me = d.get('build', 'ai_ps2')
    nm = d['readable_name'] or d['best_name'] or d['name']
    line('workdb card: %s %s %s (tools/workdb.py card, database built %s)' % (LABEL[me], h(a), nm, d['built']))
    if me != 'ai_ps2':
        line('no AI counterpart: %s\'s own function' % LABEL[me])
    s = '%s, %d instructions (%d bytes)' % (d['layer'], d['insns'], d['bytes'])
    if d['module']:
        s += ', module %s (%s%s)' % (d['module'], d['module_how'],
                                    ', ' + d['module_path'] if d['module_path'] else '')
    elif d.get('candidates'):
        s += ', module unplaced (candidates %s)' % ', '.join(d['candidates'][:6])
    bi = d.get('block_info')
    if bi:
        s += '; block %d: %s-%s, %d functions' % (d['block'], h(bi['first_addr']), h(bi['last_addr']),
                                                 bi['functions'])
        if bi.get('readable'):
            s += ' (readable: %s)' % ', '.join(bi['readable'][:6])
    line(s)
    if d['readable_name']:
        line('readable: %s, %s (%s:%d)' % (d['readable_name'], d['status'], d['file'], d['line']))
    if d.get('doc'):
        line('doc: %s' % d['doc'])
    if me != 'ai_ps2':
        pass        # (the corpus table is AI's recordings)
    elif d['corpus']:
        line('corpus v%d: %s' % (d['corpus'][0]['version'], ', '.join(
            '%s %d%s' % (c['route'], c['calls'], ' (cut short)' if c['cut_short'] else '')
            for c in d['corpus'])))
    else:
        line('corpus: no route reaches it%s' % (
            ' (recorded in v%s)' % ','.join(map(str, d['corpus_versions'])) if d['corpus_versions'] else
            '' if d['readable_name'] else ' (not registered, so not recorded)'))
    if d['sweep']:
        s = d['sweep'][0]
        line('sweep: draft %s, build %s, checks %s pass / %s fail%s' % (
            s['draft'], s['build'], s['pass'], s['fail'],
            ', failure %s/%s' % (s['failure_stage'], s['failure_class']) if s['failure_class'] else ''))
    if d.get('paths'):
        p = d['paths'][0]
        unc = json.loads(p['uncovered']) if p['uncovered'] else []
        line('path coverage: %d of %d blocks run by %d recorded calls%s' % (
            p['hit'], p['blocks'], p['calls'], '; uncovered ' + ' '.join(unc[:10]) + (' ...' if len(unc) > 10 else '')
            if unc else ''))
    if d.get('stale'):
        line('stale: Verified, but %s (stale_verified)' % d['stale'][0]['reason'])
    if d.get('lineage'):
        g = d['lineage'][0]
        line('engine lineage: %s (ZGE2 DM2 %s, %s to ZGE3; ZGE4 BMX %s, %s from ZGE3; engine_lineage)' % (
            g['lineage'], h(g['zge2']) if g['zge2'] is not None else '-', g['zge2_zge3'] or '-',
            h(g['zge4']) if g['zge4'] is not None else '-', g['zge3_zge4'] or '-'))
    line('counterparts (confidence: high and medium dependable, low a lead; MATCH3.md):' if me == 'ai_ps2' else
         'matches (the other builds\' pairs of it; confidence as for counterparts, MATCH3.md):')
    by = collections.defaultdict(list)
    for c in d['counterparts']:
        by[c['build']].append(c)
    for b in [x for x in ('ai_ps2', 'dm2_ps2', 'bmx_ps2', 'ai_xbox', 'dm2_xbox', 'bmx_xbox', 'ai_gc', 'dm2_gc',
                          'bmx_gc') if x != me and (x != 'ai_ps2' or me != 'ai_ps2')]:
        cs = by.get(b)
        if not cs:
            line('  %-8s -' % LABEL[b])
            continue
        main = [c for c in cs if not c['inlined']]
        inl = [c for c in cs if c['inlined']]
        parts = []
        for c in main:
            p = '%s %s' % (h(c['addr']), c['confidence'])
            if c['cls'] and c['cls'] != c['confidence']:
                p += ' %s' % c['cls']
            p += ' (%s%s%s)' % (c['method'], ', via ' + LABEL[c['via']] if c['via'] else '',
                                ', score %.2f' % c['score'] if c['score'] is not None else '')
            if c['insns']:
                p += ', %d insns' % c['insns']
            parts.append(p)
        if inl:
            parts.append('inlined into %s (%s%s)' % (', '.join(h(c['addr']) for c in inl),
                                                    inl[0]['method'],
                                                    ', via ' + LABEL[inl[0]['via']] if inl[0]['via'] else ''))
        line('  %-8s %s' % (LABEL[b], '; '.join(parts)), '          ')
    # names, one entry per name and source with the builds that give it
    grp = collections.OrderedDict()
    for n in d['names']:
        if n['source'] in ('readable_doc', 'readable', 'string_key', 'tuning') or not n['name']:
            continue
        kind = n['kind'] if n['kind'] not in ('function',) else ''
        g = grp.setdefault((n['name'], n['source'], kind), dict(builds=[], ev=None))
        if LABEL[n['build']] not in g['builds']:
            g['builds'].append(LABEL[n['build']])
        if n['source'] == 'message' and n['evidence'] and not g['ev']:
            g['ev'] = n['evidence']
    nl = []
    for (name, source, kind), g in grp.items():
        t = '%s (%s%s: %s)' % (name, source, ' ' + kind if kind else '', ', '.join(g['builds']))
        if g['ev']:
            t += ' "%s"' % g['ev'].strip()[:60]
        nl.append(t)
    if nl:
        line('names: ' + '; '.join(nl[:16]) + ('; ...' if len(nl) > 16 else ''), '  ')
    keys = []
    for n in d['names']:
        if n['source'] == 'string_key':
            k = (n['name'], n['evidence'])
            if k not in keys:
                keys.append(k)
    if keys:
        line('string keys: ' + '; '.join('%s %s' % (k or '?', e or '') for k, e in keys[:10]), '  ')
    if d['tuning']:
        line('tuning: ' + '; '.join('%s (%s)' % (n['name'], n['evidence']) for n in d['tuning']), '  ')
    if d['files']:
        line('files: ' + '; '.join('%s (%s)' % (f['path'], LABEL[f['build']]) for f in d['files']), '  ')
    if d['strings']:
        st = collections.OrderedDict()
        for b, t, isp in d['strings']:
            if isp:
                continue
            st.setdefault(t, []).append(LABEL[b])
        if st:
            line('strings: ' + '; '.join('%s (%s)' % (json.dumps(t)[:80], ', '.join(sorted(set(bs))))
                                         for t, bs in list(st.items())[:14]), '  ')
    fl = d['floats']
    if fl:
        line('floats: ' + ', '.join('%g' % v for v in fl[:24]), '  ')
    offs = [(LABEL[me], d['disps'])] + [(LABEL[c['build']], c['disps']) for c in d['counterparts']
                                       if not c['inlined'] and c['disps']]
    if any(o for _, o in offs):
        line('struct offsets:', '')
        for lb, o in offs:
            if o:
                line('  %-8s %s' % (lb, ' '.join('+0x%X' % v for v in o[:28]) +
                                    (' ...' if len(o) > 28 else '')), '          ')
    if d['globals']:
        gs = [('%s %s' % (h(g['addr']), g['name'])) if g['name'] else h(g['addr'])
              for g in d['globals']]
        line('globals: ' + ', '.join(gs[:20]) + (' ...' if len(gs) > 20 else ''), '  ')
    if d['callers']:
        line('callers (%d): ' % len(d['callers']) + ', '.join(
            '%s %s' % (h(c['addr']), c['name']) for c in d['callers'][:12]) +
            (' ...' if len(d['callers']) > 12 else ''), '  ')
    if d['callees']:
        line('callees (%d, in call order): ' % len(d['callees']) + ', '.join(
            '%s %s' % (h(c['addr']), c['name']) for c in d['callees'][:16]) +
            (' ...' if len(d['callees']) > 16 else ''), '  ')
    if d.get('notes'):
        # at most CARD_NOTES on the card, topic by topic; `workdb.py notes ADDR` has them all
        line('notes (%d; workdb.py notes ADDR for all):' % len(d['notes']))
        for n in d['notes'][:CARD_NOTES]:
            who = '' if n['build'] == me else '%s %s: ' % (LABEL[n['build']], h(n['addr']))
            t = n['text'] if len(n['text']) <= 500 else n['text'][:497] + '...'
            line('  [%s] %s%s' % (n['topic'], who, t), '    ')
        if len(d['notes']) > CARD_NOTES:
            line('  ... %d more' % (len(d['notes']) - CARD_NOTES))
    return '\n'.join(out)


def cmd_card(argv):
    js = '--json' in argv
    if js:
        argv.remove('--json')
    prefix, width = '// ', 100
    if '--prefix' in argv:
        i = argv.index('--prefix')
        prefix = argv[i + 1]
        del argv[i:i + 2]
    if '--width' in argv:
        i = argv.index('--width')
        width = int(argv[i + 1])
        del argv[i:i + 2]
    build = None
    if '--build' in argv:
        i = argv.index('--build')
        build = argv[i + 1]
        del argv[i:i + 2]
        if build not in ('ai_ps2', 'dm2_ps2', 'bmx_ps2'):
            sys.exit('workdb: card --build is a PS2 build: ai_ps2, dm2_ps2 or bmx_ps2')
    if not argv:
        sys.exit('usage: workdb.py card ADDR|NAME ... [--build ai_ps2|dm2_ps2|bmx_ps2] [--json] [--prefix "// "] '
                 '[--width 100]')
    con = connect()
    res = []
    rc = 0
    for k in argv:
        d = card_data(con, k, build)
        if d is None:
            print('workdb: no %s function %s' % (LABEL[build] if build else 'AI, DM2 or BMX PS2', k), file=sys.stderr)
            rc = 1
            continue
        if js:
            res.append(d)
        else:
            print(fmt_card(d, prefix, width))
    if js:
        print(json.dumps(res if len(res) != 1 else res[0], default=str, indent=1))
    sys.exit(rc)


def cmd_notes(argv):
    """notes [TOPIC|ADDR|NAME] [--build B] [--json]: the knowledge notes of a topic or a function."""
    js = '--json' in argv
    if js:
        argv.remove('--json')
    build = None
    if '--build' in argv:
        i = argv.index('--build')
        build = argv[i + 1]
        del argv[i:i + 2]
    con = connect()
    if not con.execute("SELECT count(*) FROM information_schema.tables WHERE table_name = 'func_notes'"
                       ).fetchone()[0]:
        sys.exit('workdb: the database has no func_notes (built before schema 4; run: workdb.py build)')
    where, args = ['TRUE'], []
    if build:
        where.append('build = ?')
        args.append(build)
    if not argv:
        rows = con.execute("SELECT topic, count(*) n, count(DISTINCT (build, addr)) fns FROM "
                           "(SELECT DISTINCT build, addr, topic, text FROM func_notes WHERE %s) "
                           "GROUP BY topic ORDER BY list_position(?, topic)" % ' AND '.join(where),
                           args + [list(NOTE_TOPICS)]).fetchall()
        print_table(['topic', 'notes', 'functions'], rows)
        return
    key = ' '.join(argv)
    if key in NOTE_TOPICS:
        where.append('topic = ?')
        args.append(key)
    else:
        a = None
        try:
            a = parse_addr(key)
        except ValueError:
            r = con.execute("SELECT addr FROM names WHERE build = 'ai_ps2' AND name = ? "
                            "ORDER BY source = 'readable' DESC LIMIT 1", [key]).fetchone()
            r = r or con.execute("SELECT ai_addr FROM ai_names WHERE name = ? LIMIT 1", [key]).fetchone()
            if not r:
                # another PS2 build's own function (no AI counterpart): its notes are that build's
                k = card_key(con, key, build)
                if k:
                    r = (k[1],)
                    if k[0] != 'ai_ps2' and not build:
                        build = k[0]
                        where.append('build = ?')
                        args.append(build)
            if not r:
                sys.exit('workdb: %r is neither a topic (%s) nor a known function' %
                         (key, ', '.join(NOTE_TOPICS)))
            a = r[0]
        # an address inside a function counts as the function
        r = con.execute("SELECT addr FROM functions WHERE build = ? AND addr <= ? AND end_addr > ? "
                        "ORDER BY addr DESC LIMIT 1", [build or 'ai_ps2', a, a]).fetchone()
        where.append('addr = ?')
        args.append(r[0] if r else a)
    cur = con.execute("""SELECT n.build, n.addr, coalesce(bn.name, '') AS name, n.topic, n.text,
                              n.agent, n.source
                       FROM (SELECT DISTINCT ON (build, addr, topic, text) * FROM func_notes
                             WHERE %s ORDER BY build, addr, topic, text, ts) n
                       LEFT JOIN best_names bn ON bn.build = n.build AND bn.addr = n.addr
                       ORDER BY n.build, n.addr, list_position(?, n.topic)""" % ' AND '.join(where),
                      args + [list(NOTE_TOPICS)])
    cols = [x[0] for x in cur.description]
    rows = cur.fetchall()
    if js:
        print(json.dumps([dict(zip(cols, r)) for r in rows], default=str, indent=1))
        return
    last = None
    for b, a, nm, topic, text, agent, source in rows:
        if (b, a) != last:
            print('%s %s %s' % (LABEL[b], h(a), nm))
            last = (b, a)
        print('  [%s] %s%s' % (topic, text, ' (%s)' % source if source else ''))
    print('%d notes' % len(rows))


def cmd_emit(argv):
    """emit FILE KIND key=value ...: one result line (ts filled in, values JSON when they parse)."""
    if len(argv) < 2:
        sys.exit('usage: workdb.py emit FILE KIND addr=0x... run=... generator=... key=value ...')
    path, kind = argv[0], argv[1]
    r = {'kind': kind}
    for kv in argv[2:]:
        k, _, v = kv.partition('=')
        try:
            r[k] = json.loads(v)
        except ValueError:
            r[k] = v
    r.setdefault('ts', datetime.datetime.now().isoformat(timespec='seconds'))
    if isinstance(r.get('addr'), int):
        r['addr'] = h(r['addr'])
    ln = json.dumps(r)
    row, err = result_row(ln, path, 0)
    if err:
        sys.exit('workdb: not written: ' + err)
    append_result(path, r)


def append_result(path, record):
    """Append one result record (a dict with kind, addr, run, generator, ts and the kind's
    fields) to a JSONL file; whole lines only, so concurrent writers to one file do not
    interleave (each job should still have its own file)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'a') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.write(json.dumps(record) + '\n')


def cmd_check_results(argv):
    files = argv or sorted(glob.glob(os.path.join(WORKDB, 'results', '**', '*.jsonl'), recursive=True))
    bad = 0
    for p in files:
        for i, ln in enumerate(open(p), 1):
            if ln.strip():
                row, err = result_row(ln.strip(), p, i)
                if err:
                    bad += 1
                    print('%s:%d: %s' % (p, i, err))
    print('%d files, %d bad lines' % (len(files), bad))
    sys.exit(1 if bad else 0)


def public_mode():
    """card and notes from the public notes export: no duckdb module, no database, or WORKDB_PUBLIC=1."""
    return os.environ.get('WORKDB_PUBLIC') == '1' or duckdb is None or not os.path.exists(DB)


def cmd_export_notes(argv):
    """export-notes [--out FILE] [--rules FILE ...] [--check]: the notes, text only, as workdb_notes.jsonl."""
    import workdb_notes as wn
    out, rules, check = wn.EXPORT, [], '--check' in argv
    if check:
        argv.remove('--check')
    while argv:
        if argv[0] == '--out' and len(argv) > 1:
            out = os.path.abspath(argv[1])
        elif argv[0] == '--rules' and len(argv) > 1:
            rules.append(argv[1])
        else:
            sys.exit('usage: workdb.py export-notes [--out FILE] [--rules FILE ...] [--check]')
        del argv[:2]
    if not rules:
        # the workshop's own names (public_scan.py --rules): a public checkout has none, and exports nothing
        p = os.path.join(zpaths.ZAXIS_WORKSHOP or '', 'merge', 'public_scan_private.tsv')
        if not zpaths.ZAXIS_WORKSHOP or not os.path.exists(p):
            sys.exit('workdb: export-notes needs public_scan\'s private rules (--rules FILE): the workshop\'s')
        rules = [p]
    sys.path.insert(0, os.path.join(wn.TOOLING, 'checks'))
    import public_scan as ps
    con = connect()
    if not con.execute("SELECT count(*) FROM information_schema.tables WHERE table_name = 'func_notes'"
                       ).fetchone()[0]:
        sys.exit('workdb: the database has no func_notes (built before schema 4; run: workdb.py build)')
    # one note per (build, addr, topic, text), its oldest, as fetch_notes; the name the card shows; AI's counterpart
    cur = con.execute("""
        WITH n0 AS (SELECT DISTINCT ON (build, addr, topic, text) build, addr, topic, text, ts FROM func_notes
                    WHERE build IN ('ai_ps2', 'dm2_ps2', 'bmx_ps2') ORDER BY build, addr, topic, text, ts),
        -- the function the database puts each note in, as card_key: the one starting there, else the one it is inside
        n AS (SELECT n0.*, coalesce((SELECT f.addr FROM functions f WHERE f.build = n0.build AND f.addr = n0.addr),
                                    (SELECT max(f.addr) FROM functions f WHERE f.build = n0.build
                                     AND f.addr <= n0.addr AND f.end_addr > n0.addr)) AS func_addr FROM n0)
        SELECT n.build, n.addr, n.func_addr, n.topic, n.text,
               coalesce(CASE WHEN n.build = 'ai_ps2' THEN (SELECT r.name FROM readable r WHERE r.addr = n.func_addr
                                                           LIMIT 1) END,
                        (SELECT x.name FROM names x WHERE x.build = n.build AND x.addr = n.func_addr
                         AND x.source = 'readable' AND x.kind = 'function' LIMIT 1), bn.name, f.name) AS name
        FROM n LEFT JOIN best_names bn ON bn.build = n.build AND bn.addr = n.func_addr
        LEFT JOIN functions f ON f.build = n.build AND f.addr = n.func_addr
        ORDER BY list_position(['ai_ps2', 'dm2_ps2', 'bmx_ps2'], n.build), n.addr,
                 list_position(?, n.topic), n.ts, n.text""", [list(NOTE_TOPICS)])
    cols = [x[0] for x in cur.description]
    rows = [dict(zip(cols, r)) for r in cur.fetchall()]
    # a DM2 or BMX XXX function's AI counterparts: the one card_key turns its card into first (its query), then
    # every other AI function whose card shows its notes (fetch_notes: each counterpart's)
    ais = {}
    for b, a in sorted({(r['build'], r['func_addr']) for r in rows
                        if r['build'] != 'ai_ps2' and r['func_addr'] is not None}):
        first = con.execute("SELECT ai_addr FROM counterparts WHERE build = ? AND addr = ? AND NOT inlined LIMIT 1",
                            [b, a]).fetchone()
        if first:
            rest = [x[0] for x in con.execute("SELECT DISTINCT ai_addr FROM counterparts WHERE build = ? AND addr = ? "
                                              "AND NOT inlined ORDER BY 1", [b, a]).fetchall()]
            ais[(b, a)] = [first[0]] + [x for x in rest if x != first[0]]
    for r in rows:
        r['ai_addrs'] = ais.get((r['build'], r['func_addr']), [])
    built = con.execute("SELECT value FROM meta WHERE key = 'built'").fetchone()[0]
    meta = dict(exported=datetime.date.today().isoformat(), database_built=built)
    target = out
    if check:
        out = out + '.check'
    stats = wn.write_export(rows, meta, ps.load_rules(rules),
                            ps.load_allow(os.path.join(wn.TOOLING, 'checks', 'public_scan_allow.tsv')), out,
                            rel=os.path.relpath(target, wn.ROOT))
    if check:
        def body(p):
            return open(p, encoding='utf-8').read().split('\n', 1)[-1] if os.path.exists(p) else None
        same = body(out) == body(target)
        os.remove(out)
        print('workdb: export-notes --check: %s' % ('up to date' if same else '%s is not what the database makes'
                                                    % target))
        sys.exit(0 if same else 1)
    print('workdb: export-notes: %s: %s' % (out, ', '.join('%s %d' % kv for kv in sorted(stats.items()))))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help', 'help'):
        sys.exit(__doc__)
    cmd, argv = sys.argv[1], sys.argv[2:]
    if cmd in ('card', 'notes') and public_mode():
        import workdb_notes
        return {'card': workdb_notes.cmd_card, 'notes': workdb_notes.cmd_notes}[cmd](argv)
    if cmd == 'export-notes':
        return cmd_export_notes(argv)
    f = {'build': build, 'query': cmd_query, 'card': cmd_card, 'tables': cmd_tables,
         'schema': cmd_schema, 'emit': cmd_emit, 'notes': cmd_notes, 'check-results': cmd_check_results}.get(cmd)
    if not f:
        sys.exit(__doc__)
    f(argv)


if __name__ == '__main__':
    main()
