#!/usr/bin/env python3
"""Tests of the build's readable tools' source parsing (readable_digest, readable_symbols' Levels, readable_entries,
readable_locals_order, runtime_replaced) and of the frame table's locals (fncheck_traced_gen.load_locals).
The checks' own tests (fncheck_corpus, readable_args, readable_localscan) are the tooling's checks/checks_test.py.

    python3 -B runtime/tools/readable_tools_test.py [-v]
"""
import os
import sys
import unittest
import warnings

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shutil  # noqa: E402
import tempfile  # noqa: E402
import fncheck_traced_gen  # noqa: E402
import readable_digest  # noqa: E402
import readable_entries  # noqa: E402
import readable_locals_order  # noqa: E402
import readable_symbols  # noqa: E402
import runtime_replaced  # noqa: E402


class AddRow(unittest.TestCase):
    """readable_symbols.py add: one line written, the others where they are (the table is not in save's order)."""
    TABLE = ('# header\n'
             'function\tzeta\t0x00100100\t\t\n'
             'function\talpha\t0x00100200\t\t\n'      # (out of save's order, as a lane's hand-added row)
             'point\tafterCall\t0x00100108\tzeta\t\n'
             'function\tdm2Own\t0x00200000\t\tdm2\n'
             'function\tdm2Late\t0x00200400\t\tdm2\n')

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, 'table.tsv')
        with open(self.path, 'w') as f:
            f.write(self.TABLE)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def lines(self):
        with open(self.path) as f:
            return f.read().splitlines()

    def test_new_row_after_its_neighbour(self):
        self.assertEqual(readable_symbols.add_row(self.path, 'function', 'beta', 0x00100180), 'added')
        old = self.TABLE.splitlines()
        self.assertEqual(self.lines(), old[:2] + ['function\tbeta\t0x00100180\t\t'] + old[2:])

    def test_a_games_row_among_its_games(self):
        readable_symbols.add_row(self.path, 'function', 'dm2Mid', 0x00200100, '', 'dm2')
        self.assertEqual(self.lines()[5], 'function\tdm2Mid\t0x00200100\t\tdm2')
        readable_symbols.add_row(self.path, 'function', 'bmxFirst', 0x00300000, '', 'bmx')
        self.assertEqual(self.lines()[-1], 'function\tbmxFirst\t0x00300000\t\tbmx')
        self.assertEqual(len(self.lines()), len(self.TABLE.splitlines()) + 2)

    def test_a_point_with_its_function(self):
        readable_symbols.add_row(self.path, 'point', 'taken', 0x00100110, 'zeta')
        self.assertEqual(self.lines()[4], 'point\ttaken\t0x00100110\tzeta\t')

    def test_the_same_symbol_in_its_place(self):
        self.assertEqual(readable_symbols.add_row(self.path, 'function', 'alpha', 0x00100204), 'replaced')
        old = self.TABLE.splitlines()
        self.assertEqual(self.lines(), old[:2] + ['function\talpha\t0x00100204\t\t'] + old[3:])
        self.assertEqual(readable_symbols.add_row(self.path, 'function', 'alpha', 0x00100204), 'same')
        # (AI's alpha is not DM2's: a game's row of the name is its own)
        self.assertEqual(readable_symbols.add_row(self.path, 'function', 'alpha', 0x00200200, '', 'dm2'), 'added')


class Definitions(unittest.TestCase):
    def defs(self, code):
        return readable_digest.definitions(readable_digest.strip(code))

    def test_if_constexpr_is_a_step(self):
        # (d4scene3: DM2's setupSceneModel named matchSceneRoot through a definition named constexpr)
        d = self.defs('void f(int a)\n{\n    if constexpr (kIsBmx) {\n        g(a);\n    }\n}\n'
                      'void h() { if constexpr (!kIsDm2) { matchSceneRoot(); } else { g(1); } }\n')
        self.assertEqual(sorted(d), ['f', 'h'])
        self.assertNotIn('matchSceneRoot', ''.join(d['f']))

    def test_a_brace_in_a_literal_is_no_brace(self):
        # (r9bmxzgui2: screen_space.cpp's scanName compares with '{'; its definition ran to the end of the file and
        # the level walk charged refreshGuiScreenSpace guiNodeMatrix's g_textAlignment2)
        d = self.defs("int scan(char c) { return c != '{' && c != '\\'' ? 1 : 0; }\n"
                      'void after() { name(kAlignmentName); }\n'
                      'const char *open() { return "{{"; }\n'
                      "int sep() { return 1'000; }\n")
        self.assertEqual(sorted(d), ['after', 'open', 'scan', 'sep'])
        self.assertNotIn('kAlignmentName', ''.join(d['scan']))
        self.assertTrue(d['open'][0].endswith('}'))

    def test_keywords_are_no_definitions(self):
        d = self.defs('void f() { while (x) { y(); } switch (z) { case 1: break; } for (;;) { } }\n')
        self.assertEqual(sorted(d), ['f'])

    def test_qualifiers(self):
        d = self.defs('int Foo::bar(int a) const { return a; }\nauto baz() -> int { return 1; }\n')
        self.assertEqual(sorted(d), ['bar', 'baz'])


class AliasConstants(unittest.TestCase):
    """_ALIAS_K: a constant that names a symbol, as Levels and the headers read it (tools30: BMX's
    `constexpr ee::Ptr<Matrix> kComposedMatrix(sym::composedMatrix)`, unbound, was not seen)."""

    def found(self, text):
        return [m.groups() for m in readable_symbols._ALIAS_K.finditer(text)]

    def test_each_initialisation(self):
        self.assertEqual(self.found('constexpr uint32_t kA = sym::a;'), [('kA', None, 'a')])
        self.assertEqual(self.found('constexpr ee::Ptr<Matrix> kComposedMatrix(sym::composedMatrix);'),
                         [('kComposedMatrix', None, 'composedMatrix')])
        self.assertEqual(self.found('static const T kB{ sym::bmx::b };'), [('kB', 'bmx', 'b')])

    def test_a_call_is_no_alias(self):
        self.assertEqual(self.found('const int kC = f(sym::c);'), [])


class SkipGameSteps(unittest.TestCase):
    """The steps an `if constexpr` on the game flags leaves out for a game (item 9: d4render3)."""

    def kept(self, text, game, constants=None):
        return ' '.join(readable_symbols.skip_game_steps(text, game, constants).split())

    def test_negated_flag(self):
        t = 'f() { if constexpr (!game::readable::kIsDm2) if (*g_gsVideoMode == 3) room = 2; done(); }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { done(); }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { if (*g_gsVideoMode == 3) room = 2; done(); }')

    def test_an_inner_if_else_is_the_inner_ifs(self):
        t = 'f() { if constexpr (!game::readable::kIsDm2) if (x) y = g_a; else y = g_b; z = g_c; }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { z = g_c; }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { if (x) y = g_a; else y = g_b; z = g_c; }')

    def test_a_loop_step_ends_at_its_block(self):
        t = 'f() { if constexpr (game::readable::kIsDm2) for (i = 0; i < 3; ++i) { a(); } b = g_keep; }'
        self.assertEqual(self.kept(t, 'bmx'), 'f() { b = g_keep; }')

    def test_else_if_chain_and_other_flags(self):
        t = ('f() { if constexpr (game::readable::kIsDm2) { A(); } else if constexpr (game::readable::kIsBmx) { B(); } '
             'else { C(); } D(); }')
        self.assertEqual(self.kept(t, 'dm2'), 'f() { { A(); } D(); }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { { B(); } D(); }')

    def test_a_step_inside_a_kept_step(self):
        t = ('f() { if constexpr (!game::readable::kIsBmx) { if constexpr (game::readable::kIsDm2) { arrayFirst(); } '
             'else { walkStart(here::taken); } } }')
        self.assertEqual(self.kept(t, 'dm2'), 'f() { { { arrayFirst(); } } }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { }')

    def test_bare_flag_and_flag_constant(self):
        t = 'f() { using game::readable::kIsDm2; if constexpr (kIsDm2) a(); else b(); if constexpr (kKeeps) c(); }'
        self.assertEqual(self.kept(t, 'dm2', {'kKeeps': '!game::readable::kIsDm2'}), 'f() { using game::readable::kIsDm2; a(); }')
        self.assertEqual(self.kept(t, 'bmx', {'kKeeps': '!game::readable::kIsDm2'}), 'f() { using game::readable::kIsDm2; b(); c(); }')

    def test_the_rest_after_a_kept_return(self):
        # (item 10, d4scene4: DM2 objectNameHash never reaches objectLevelNode)
        t = ('uint32_t objectNameHash(int o) { if constexpr (game::readable::kIsDm2) return nodeNameHash(levelObjectOf(o)); '
             'const int node = objectLevelNode(o); return node; }')
        self.assertEqual(self.kept(t, 'dm2'), 'uint32_t objectNameHash(int o) { return nodeNameHash(levelObjectOf(o));}')
        self.assertIn('objectLevelNode', self.kept(t, 'bmx'))
        t = 'f() { if (a) { if constexpr (!game::readable::kIsBmx) { x(); return 1; } y(); } z(); }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { if (a) { { x(); return 1; }} z(); }')

    def test_a_conditional_expression_on_the_flags(self):
        # (item 10, d4scene4: DM2 nodeSceneObject never calls objectSceneKindOrNone)
        t = 'f() { const bool b = game::readable::kIsDm2 ? pooled : pooled && objectSceneKindOrNone(o) == 2; g(b); }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { const bool b = (pooled) ; g(b); }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { const bool b = (pooled && objectSceneKindOrNone(o) == 2) ; g(b); }')
        t = 'f() { y = max(kIsBmx ? 1 : 2, z); m = (!kIsDm2 && !kIsBmx) ? a() : b(); n = !kIsDm2 ? e(kIsBmx ? 3 : 4) : h(); }'
        self.assertEqual(self.kept(t, 'bmx'), 'f() { y = max( (1) , z); m = (b()) ; n = (e( (3) )) ; }')

    def test_a_condition_broken_over_lines(self):
        # (tools30, unbound1's finding: `kIsDm2\n    ? a : b` was no flags condition to _flags_value, both arms kept)
        t = 'f() { x = game::readable::kIsDm2\n        ? sym::dm2Only\n        : sym::shared; if constexpr (kIsBmx\n || kIsDm2) g(); }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { x = (sym::dm2Only) ; g(); }')
        self.assertEqual(self.kept(t, 'bmx'), 'f() { x = (sym::shared) ; g(); }')

    def test_other_conditional_expressions_are_left(self):
        t = 'f() { x = a ? b : c; w = q::r ? s::t : u::v; p = x && kIsDm2 ? 5 : 6; e = y == kIsDm2 ? 1 : 2; }'
        self.assertEqual(self.kept(t, 'dm2'), ' '.join(t.split()))

    def test_another_condition_is_left(self):
        t = 'f() { if constexpr (Build::kFloats) { a(); } else { b(); } }'
        self.assertEqual(self.kept(t, 'dm2'), 'f() { if constexpr (Build::kFloats) { a(); } else { b(); } }')


class LiteralViewRegistrations(unittest.TestCase):
    """literal_view writes a registration at the address its build registers it at: AI's row in a shared source; in a
    game folder the game's own row, else AI's name where the game's bind table puts it (BMX XXX's park editor, 34
    registrations, was written at AI's addresses), else as written."""

    def setUp(self):
        self.saved = readable_symbols._table, dict(readable_symbols._binds), readable_symbols.BIND_DIR
        t = readable_symbols.Table(None)
        t.put('function', 'shared', 0x1000)
        t.put('function', 'unbound', 0x1010)
        t.own('bmx').put('function', 'ownRow', 0x3000)
        readable_symbols._table = t
        readable_symbols._binds.clear()
        readable_symbols._binds['bmx'] = StubBind({0x1000: 0x2000})

    def tearDown(self):
        readable_symbols._table, binds, readable_symbols.BIND_DIR = self.saved
        readable_symbols._binds.clear()
        readable_symbols._binds.update(binds)

    def view(self, text, folder=True):
        head = '#include "sym/bmx/editor/x.cpp.h"\n' if folder else ''
        return readable_symbols.literal_view(head + text).split('\n')[-1]

    def test_a_shared_source_is_ais(self):
        self.assertEqual(self.view('READABLE_FUNCTION(shared, Verified);', folder=False),
                         'READABLE_FUNCTION(0x00001000, shared, Verified);')

    def test_a_game_folder_reads_its_own_row_then_the_bind(self):
        self.assertEqual(self.view('READABLE_FUNCTION(ownRow, Draft);'), 'READABLE_FUNCTION_IN(bmx, 0x00003000, ownRow, Draft);')
        self.assertEqual(self.view('READABLE_FUNCTION(shared, Verified);'),
                         'READABLE_FUNCTION_IN(bmx, 0x00002000, shared, Verified);')
        self.assertEqual(self.view('READABLE_FUNCTION_WAITS(sym::shared, body, Verified);'),
                         'READABLE_FUNCTION_WAITS_IN(bmx, 0x00002000, body, Verified);')

    def test_never_ais_address_in_a_game_folder(self):
        # (a name the bind has not got, or no bind table at all: left as written)
        self.assertEqual(self.view('READABLE_FUNCTION(unbound, Draft);'), 'READABLE_FUNCTION(unbound, Draft);')
        readable_symbols._binds['bmx'] = None
        self.assertEqual(self.view('READABLE_FUNCTION(shared, Verified);'), 'READABLE_FUNCTION(shared, Verified);')


class ExceptRegistrations(unittest.TestCase):
    """READABLE_EXCEPT(GAME, REGISTRATION) (ee/readable_waits.h): the registration is every build's but GAME's
    instance's, which registers nothing at the address; AI's view (literal_view) reads it as written inside."""

    def setUp(self):
        self.saved = readable_symbols._table
        t = readable_symbols.Table(None)
        t.put('function', 'shared', 0x1000)
        readable_symbols._table = t

    def tearDown(self):
        readable_symbols._table = self.saved

    SRC = 'int shared() { return 1; }\n    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(shared, Verified)); // (its own)\n'

    def test_kept_out_of_that_game_alone(self):
        names = lambda game: [r[0] for r in readable_symbols.registrations_in(self.SRC, game)]
        self.assertEqual(names(None), ['shared'])
        self.assertEqual(names('dm2'), ['shared'])
        self.assertEqual(names('bmx'), [])

    def test_an_own_form_kept_out_too(self):
        src = 'int shared() { return 1; }\n    READABLE_EXCEPT(bmx, READABLE_FUNCTION_OWN(shared, Verified, dm2, sharedByName));\n'
        rows = lambda game: [(r[0], r[1]) for r in readable_symbols.registrations_in(src, game)]
        self.assertEqual(rows(None), [('shared', 'shared')])
        self.assertEqual(rows('dm2'), [('shared', 'sharedByName')])
        self.assertEqual(rows('bmx'), [])

    def test_ais_view_is_the_registration(self):
        line = readable_symbols.literal_view(self.SRC).split('\n')[1]
        self.assertEqual(line.split(';')[0].strip(), 'READABLE_FUNCTION_WAITS(0x00001000, shared, Verified)')
        self.assertEqual(len(line), len(self.SRC.split('\n')[1]) + len('0x00001000, ') )


class StubBind:
    """A game's table for the tests: AI address -> the game's, and which AI functions are bodies (same code)."""
    ai = False

    def __init__(self, functions, bodies=(), data=None):
        self.functions, self.bodies, self.data = dict(functions), set(bodies), dict(data or {})

    def function(self, ai):
        return self.functions.get(ai)

    def body(self, ai):
        return (ai, ai + 4, self.functions[ai]) if ai in self.bodies else None

    def datum(self, ai):
        return self.data.get(ai)

    def point(self, ai):
        return None


class Levels(unittest.TestCase):
    """Levels over a tree of made-up sources: AI's symbols at 0x1000 + 0x10 n, the game's at 0x2000 + 0x10 n."""

    def setUp(self):
        warnings.simplefilter('ignore', ResourceWarning)    # (Levels reads the sources with open().read())
        self.tree = tempfile.mkdtemp(prefix='readable_tools_test')
        os.makedirs(os.path.join(self.tree, 'zge'))

    def tearDown(self):
        shutil.rmtree(self.tree)

    def levels(self, sources, game, bound, unbound_data=(), close=()):
        """sources {file: text}; bound: functions the game has with the same code (the rest of those named in
        `names` are unbound); returns {name: (level, why)}."""
        t = readable_symbols.Table(None)
        names = sorted(set(readable_digest.IDENT.findall(' '.join(sources.values()))))
        functions, bodies, data = {}, set(), {}
        for i, n in enumerate(names):
            if n.startswith('g_'):
                t.put('data', n, 0x1000 + 0x10 * i)
                if n not in unbound_data:
                    data[0x1000 + 0x10 * i] = 0x2000 + 0x10 * i
            else:
                t.put('function', n, 0x1000 + 0x10 * i)
                if n in bound:
                    functions[0x1000 + 0x10 * i] = 0x2000 + 0x10 * i
                    bodies.add(0x1000 + 0x10 * i)
        for f, text in sources.items():
            with open(os.path.join(self.tree, 'zge', f), 'w') as o:
                o.write(text)
        if close:
            os.makedirs(os.path.join(self.tree, game), exist_ok=True)
            with open(os.path.join(self.tree, game, 'close.tsv'), 'w') as o:
                o.write(''.join('%s\tread\n' % n for n in close))
        lv = readable_symbols.Levels(StubBind(functions, bodies, data), t, self.tree, game)
        return {n: (lv.level[n], lv.why.get(n, '')) for n in lv.level}

    def test_plain_callee_in_another_file_needs_an_address(self):
        # (d4scene3: BMX's own setupSceneModelWithOptions called nodeName of geometry_nodes.cpp, unbound in BMX)
        src = {'pose.cpp': 'void setupSceneModelWithOptions(int n) { zge::nodeName(n); }\n'
                           'void setupSceneModel(int n) { }\n'
                           'READABLE_FUNCTION_WAITS_OWN(setupSceneModel, Verified, bmx, setupSceneModelWithOptions);\n',
               'nodes.cpp': 'int nodeName(int n) { return n; }\nREADABLE_FUNCTION(nodeName, Verified);\n'}
        lv = self.levels(src, 'bmx', bound={'setupSceneModel'})
        self.assertEqual(lv['setupSceneModel'], (1, 'callee nodeName'))
        lv = self.levels(src, 'bmx', bound={'setupSceneModel', 'nodeName'})
        self.assertEqual(lv['setupSceneModel'][0], 2)

    def test_a_level_one_callee_is_reached_at_its_address(self):
        # (its body differs here: the call goes to the recompiled original, as the original's jal does)
        src = {'a.cpp': 'int caller(int n) { return callee(n); }\nREADABLE_FUNCTION(caller, Verified);\n',
               'b.cpp': 'int callee(int n) { return n; }\nREADABLE_FUNCTION(callee, Verified);\n'}
        t = readable_symbols.Table(None)
        t.put('function', 'caller', 0x1000)
        t.put('function', 'callee', 0x1010)
        for f, text in src.items():
            with open(os.path.join(self.tree, 'zge', f), 'w') as o:
                o.write(text)
        lv = readable_symbols.Levels(StubBind({0x1000: 0x2000, 0x1010: 0x2010}, {0x1000}), t, self.tree, 'dm2')
        self.assertEqual((lv.level['caller'], lv.level['callee']), (2, 1))

    def test_a_helper_calling_an_unbound_function(self):
        src = {'a.cpp': 'namespace { int helper(int n) { return heapTake(n); } }\n'
                        'int caller(int n) { return helper(n); }\nREADABLE_FUNCTION(caller, Verified);\n',
               'heaps.cpp': 'int heapTake(int n) { return n; }\nREADABLE_FUNCTION(heapTake, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'caller', 'helper'})
        self.assertEqual(lv['caller'], (1, 'callee heapTake'))

    def test_a_registered_callee_body_is_not_the_callers(self):
        # (readHlibChunk -> newUiLibrary -> heapTake: newUiLibrary's own level answers for its body)
        src = {'a.cpp': 'int newUiLibrary(int n) { return heapTake(n); }\nREADABLE_FUNCTION(newUiLibrary, Verified);\n'
                        'int readHlibChunk(int n) { return newUiLibrary(n); }\nREADABLE_FUNCTION(readHlibChunk, Verified);\n',
               'heaps.cpp': 'int heapTake(int n) { return n; }\nREADABLE_FUNCTION(heapTake, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'newUiLibrary', 'readHlibChunk'})
        self.assertEqual(lv['newUiLibrary'], (1, 'callee heapTake'))
        self.assertEqual(lv['readHlibChunk'][0], 2)

    def test_a_constant_naming_a_games_own_row(self):
        # (d5scene: `constexpr uint32_t k = sym::bmx::x;` was read as AI's symbol `bmx`, level 1)
        for game in ('dm2', 'bmx'):
            src = {'a.cpp': 'constexpr uint32_t kTimer = sym::%s::g_ownTimer;\n'
                            'constexpr uint32_t kShared = sym::g_shared;\n'
                            'int f(int n) { return ee::load(kTimer) + ee::load(kShared) + n; }\n'
                            'READABLE_FUNCTION(f, Verified);\n' % game}
            self.assertEqual(self.levels(src, game, bound={'f'})['f'], (2, ''))
            # (an AI name through a constant still needs its row here)
            self.assertEqual(self.levels(src, game, bound={'f'}, unbound_data={'g_shared'})['f'], (1, 'symbol g_shared'))

    def test_a_local_of_a_functions_name_is_no_call(self):
        src = {'a.cpp': 'int addPlane(int n) { ee::Ptr<Plane> group(n); return group->x; }\n'
                        'READABLE_FUNCTION(addPlane, Verified);\n',
               'events.cpp': 'int group(int n) { return n; }\nREADABLE_FUNCTION(group, Verified);\n'}
        lv = self.levels(src, 'bmx', bound={'addPlane'})
        self.assertEqual(lv['addPlane'][0], 2)

    def test_a_file_s_own_helper_of_another_files_name(self):
        src = {'a.cpp': 'namespace { int objectLevelNode(int n) { return n; } }\n'
                        'int f(int n) { return objectLevelNode(n); }\nREADABLE_FUNCTION(f, Verified);\n',
               'objects.cpp': 'int objectLevelNode(int n) { return n; }\nREADABLE_FUNCTION(objectLevelNode, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'f'})
        self.assertEqual(lv['f'][0], 2)

    def test_steps_a_game_skips(self):
        src = {'a.cpp': 'int f(int n) {\n    using game::readable::kIsDm2;\n    if constexpr (!kIsDm2)\n        reset();\n'
                        '    if constexpr (kKeepsSize) { freeSized(n); } else { heapFree(n); }\n    return n; }\n'
                        'READABLE_FUNCTION(f, Verified);\n',
               'b.cpp': 'constexpr bool kKeepsSize = !game::readable::kIsDm2;\n'
                        'void reset() { }\nREADABLE_FUNCTION(reset, Verified);\n'
                        'void freeSized(int n) { }\nREADABLE_FUNCTION(freeSized, Verified);\n'
                        'void heapFree(int n) { }\nREADABLE_FUNCTION(heapFree, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'f', 'heapFree'})
        self.assertEqual(lv['f'][0], 2)
        lv = self.levels(src, 'bmx', bound={'f', 'heapFree'})
        self.assertEqual(lv['f'][0], 1)

    def test_a_plain_source_reaches_a_registered_callee_at_its_address(self):
        # (BMX setGsTextureImage named g_gsVideoMode through buildGsTextureUpload, level 1 there)
        src = {'gs.cpp': 'int build(int t) { return *sym::g_videoMode + t; }\nREADABLE_FUNCTION(build, Verified);\n'
                         'int setImage(int t) { return build(t); }\nREADABLE_FUNCTION(setImage, Verified);\n'}
        lv = self.levels(src, 'bmx', bound={'build', 'setImage'}, unbound_data={'g_videoMode'})
        self.assertEqual(lv['setImage'], (1, 'symbol g_videoMode'))   # (not a plain source: build's body runs inline)
        with open(os.path.join(self.tree, 'plain_sources.txt'), 'w') as f:
            f.write('# plain\nzge/\n')
        lv = self.levels(src, 'bmx', bound={'build', 'setImage'}, unbound_data={'g_videoMode'})
        self.assertEqual(lv['build'], (1, 'symbol g_videoMode'))
        self.assertEqual(lv['setImage'][0], 2)

    def test_a_plain_source_still_walks_its_helpers(self):
        with open(os.path.join(self.tree, 'plain_sources.txt'), 'w') as f:
            f.write('zge/\n')
        src = {'a.cpp': 'namespace { int helper(int n) { return *sym::g_gone + n; } }\n'
                        'int f(int n) { return helper(n); }\nREADABLE_FUNCTION(f, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'f'}, unbound_data={'g_gone'})
        self.assertEqual(lv['f'], (1, 'symbol g_gone'))

    def test_own_bodies_of_two_versions(self):
        src = {'draw.cpp': 'int drawScene(int a) { return *sym::g_onlyZge3 + a; }\n'
                           'int bySector(int a) { return *sym::g_zge2 + a; }\nint byTrees(int a) { return *sym::g_zge4 + a; }\n'
                           'READABLE_FUNCTION_WAITS_OWN(drawScene, Verified, dm2, bySector, bmx, byTrees);\n'
                           'int drawFrame(int a) { return drawScene(a); }\nREADABLE_FUNCTION(drawFrame, Verified);\n'}
        for game, theirs in (('dm2', 'g_zge4'), ('bmx', 'g_zge2')):
            lv = self.levels(src, game, bound={'drawScene', 'drawFrame'}, unbound_data={'g_onlyZge3', theirs})
            self.assertEqual(lv['drawScene'], (2, 'own body ' + ('bySector' if game == 'dm2' else 'byTrees')))
            self.assertEqual(lv['drawFrame'][0], 2)
        lv = self.levels(src, 'dm2', bound={'drawScene', 'drawFrame'}, unbound_data={'g_zge2'})
        self.assertEqual(lv['drawScene'], (1, 'symbol g_zge2'))

    def test_a_callee_with_an_own_body_is_walked_through_it(self):
        # (item 2, fixed by e0bc317f: DM2 removeLevelObjects reaches removeLevelObject's own body, not the shared one)
        src = {'objects.cpp': 'int removeLevelObject(int o) { return *sym::g_onlyZge3 + o; }\n'
                              'int unregisterLevelObject(int o) { return o; }\n'
                              'READABLE_FUNCTION_WAITS_OWN(removeLevelObject, Verified, dm2, unregisterLevelObject);\n'
                              'int removeLevelObjects() { return removeLevelObject(1); }\n'
                              'READABLE_FUNCTION_WAITS(removeLevelObjects, Verified);\n'}
        lv = self.levels(src, 'dm2', bound={'removeLevelObject', 'removeLevelObjects'}, unbound_data={'g_onlyZge3'})
        self.assertEqual(lv['removeLevelObjects'][0], 2)
        lv = self.levels(src, 'bmx', bound={'removeLevelObject', 'removeLevelObjects'}, unbound_data={'g_onlyZge3'})
        self.assertEqual(lv['removeLevelObjects'], (1, 'symbol g_onlyZge3'))

    def test_try_close_skips_an_address_the_game_folder_registers(self):
        # (DM2 0x1845C0: zge's unregisterLevelObjectType, level 1 and bound at the address of game/dm2's own
        # bmxobjctvGenerateHash, was registered there by --try-close; readable::find takes the first registration)
        src = {'zge/objects.cpp': 'int unregisterLevelObjectType(int n) { return n; }\n'
                                  'READABLE_FUNCTION(unregisterLevelObjectType, Draft);\n'
                                  'int removeLevelObjectType(int n) { return n; }\n'
                                  'READABLE_FUNCTION(removeLevelObjectType, Draft);\n',
               'dm2/game/goals.cpp': 'int bmxobjctvGenerateHash(int n) { return n; }\n'
                                     'READABLE_FUNCTION(bmxobjctvGenerateHash, Draft);\n'}
        for f, text in src.items():
            os.makedirs(os.path.dirname(os.path.join(self.tree, f)), exist_ok=True)
            with open(os.path.join(self.tree, f), 'w') as o:
                o.write(text)
        t = readable_symbols.Table(None)
        t.put('function', 'unregisterLevelObjectType', 0x1000)
        t.put('function', 'removeLevelObjectType', 0x1010)
        t.own('dm2').put('function', 'bmxobjctvGenerateHash', 0x2000)
        # (both AI functions bound, their code different here: level 1)
        lv = readable_symbols.Levels(StubBind({0x1000: 0x2000, 0x1010: 0x2010}), t, self.tree, 'dm2')
        self.assertEqual((lv.level['unregisterLevelObjectType'], lv.level['removeLevelObjectType']), (1, 1))
        self.assertEqual(lv.folder_at, {0x2000})
        self.assertFalse(lv.tried('unregisterLevelObjectType'))     # (the folder's function is at 0x2000)
        self.assertTrue(lv.tried('removeLevelObjectType'))
        # (BMX's instance has no game/dm2: there the address is free)
        lv = readable_symbols.Levels(StubBind({0x1000: 0x2000, 0x1010: 0x2010}), t, self.tree, 'bmx')
        self.assertTrue(lv.tried('unregisterLevelObjectType'))



    def test_a_caller_of_an_excepted_function_reaches_the_games_own_body(self):
        # (facetree1: BMX XXX's testNodeFaces calls segmentFaceTree, which READABLE_EXCEPT keeps out of BMX's
        # instance; game/bmx's segmentFaceTreeUncut is registered at the address the bind puts it: the caller stays 2.
        # With no own body there the call reaches nothing readable, and the caller drops to 1 as before.)
        shared = {'plain_sources.txt': 'zge/\n',
                  'zge/faces.cpp': 'int segmentFaceTree(int n) { return n; }\n'
                                   'inline int segmentFaceTreeByCorners(int n) { return n; }\n'
                                   'READABLE_EXCEPT(bmx, READABLE_FUNCTION_VU0_OWN(segmentFaceTree, Verified, dm2, '
                                   'segmentFaceTreeByCorners));\n'
                                   'int testNodeFaces(int n) { return segmentFaceTree(n); }\n'
                                   'READABLE_FUNCTION(testNodeFaces, Verified);\n'}
        own = {'bmx/collision/face_tree.cpp': 'int segmentFaceTreeUncut(int n) { return n + 1; }\n'
                                             'READABLE_FUNCTION_VU0(segmentFaceTreeUncut, Draft);\n'}

        def levels(sources):
            shutil.rmtree(self.tree)
            for f, text in sources.items():
                os.makedirs(os.path.dirname(os.path.join(self.tree, f)), exist_ok=True)
                with open(os.path.join(self.tree, f), 'w') as o:
                    o.write(text)
            t = readable_symbols.Table(None)
            t.put('function', 'segmentFaceTree', 0x1000)
            t.put('function', 'testNodeFaces', 0x1010)
            t.own('bmx').put('function', 'segmentFaceTreeUncut', 0x2000)
            lv = readable_symbols.Levels(StubBind({0x1000: 0x2000, 0x1010: 0x2010}, {0x1010}), t, self.tree, 'bmx')
            return {n: (lv.level[n], lv.why.get(n, '')) for n in lv.level}

        lv = levels(dict(shared, **own))
        self.assertEqual(lv['segmentFaceTree'][0], 0)
        self.assertEqual(lv['testNodeFaces'], (2, ''))
        lv = levels(shared)
        self.assertEqual(lv['testNodeFaces'], (1, 'callee segmentFaceTree'))

    def test_try_close_takes_the_best_pair_of_one_address(self):
        # (DM2 0x149E78: loadLevelPaths high and read, spawnLevelObjects medium by neighbour, both tried before)
        src = {'zge/follower.cpp': 'int loadLevelPaths(int n) { return n; }\nREADABLE_FUNCTION(loadLevelPaths, Draft);\n',
               'zge/objects.cpp': 'int spawnLevelObjects(int n) { return n; }\nREADABLE_FUNCTION(spawnLevelObjects, Draft);\n'}
        for f, text in src.items():
            with open(os.path.join(self.tree, f), 'w') as o:
                o.write(text)
        t = readable_symbols.Table(None)
        t.put('function', 'loadLevelPaths', 0x1000)
        t.put('function', 'spawnLevelObjects', 0x1010)
        bind = StubBind({0x1000: 0x2000, 0x1010: 0x2000})
        lv = readable_symbols.Levels(bind, t, self.tree, 'dm2')
        self.assertEqual((lv.tried('loadLevelPaths'), lv.tried('spawnLevelObjects')), (False, False))   # (as well proven)
        bind.strength = {0x1000: readable_symbols.pair_strength(['entry', '', '', '', 'high', 'close', 'read']),
                         0x1010: readable_symbols.pair_strength(['entry', '', '', '', 'medium', 'drifted', 'neighbour'])}
        lv = readable_symbols.Levels(bind, t, self.tree, 'dm2')
        self.assertEqual((lv.tried('loadLevelPaths'), lv.tried('spawnLevelObjects')), (True, False))
        # (one the instance does not compile is no rival)
        bind.strength = {}
        lv = readable_symbols.Levels(bind, t, self.tree, 'dm2')
        lv.instance = {'zge/follower.cpp'}
        self.assertTrue(lv.tried('loadLevelPaths'))

    # Two registered bodies with one short name (zge::forEachChild, scene's, and zge::zgui::forEachChild, sym
    # forEachChild2): a plain call reaches the one C++'s lookup finds from the caller's scope. Keyed by the short
    # name, the last one read won, which depended on the host's walk order (winapp3: NTFS walks zgui after scene,
    # so DM2's countModelNodes went to level 1 on Windows only); sorted walks alone would give zgui's everywhere.
    TWO_WALKS = {
        'scene/transform.cpp': 'namespace zge\n{\n    int forEachChild(int node) { return node; }\n'
                               '    READABLE_FUNCTION(forEachChild, Verified);\n}\n',
        'zgui/draw.cpp': 'namespace zge::zgui\n{\n    void forEachChild(int node) { }\n'
                         '    READABLE_FUNCTION(sym::forEachChild2, forEachChild, Verified);\n}\n',
    }

    def walk_levels(self, sources, bound, order):
        t = readable_symbols.Table(None)
        names = ['forEachChild', 'forEachChild2'] + sorted(n for n in bound if n not in ('forEachChild', 'forEachChild2'))
        functions = {}
        for i, n in enumerate(names):
            t.put('function', n, 0x1000 + 0x10 * i)
            if n in bound:
                functions[0x1000 + 0x10 * i] = 0x2000 + 0x10 * i
        for f, text in dict(self.TWO_WALKS, **sources).items():
            os.makedirs(os.path.dirname(os.path.join(self.tree, 'zge', f)), exist_ok=True)
            with open(os.path.join(self.tree, 'zge', f), 'w') as o:
                o.write(text)
        walk = os.walk

        def ordered(top, *a, **k):     # (the host's order: a walk that lists the directories either way)
            for d, dirs, files in walk(top, *a, **k):
                dirs.sort(reverse=order == 'reversed')
                yield d, dirs, files
        readable_symbols.os.walk = ordered
        try:
            lv = readable_symbols.Levels(StubBind(functions, functions), t, self.tree, 'dm2')
        finally:
            readable_symbols.os.walk = walk
        return {n: (lv.level[n], lv.why.get(n, '')) for n in lv.level}

    def test_a_plain_call_reaches_its_own_namespace(self):
        src = {'scene/pose.cpp': 'namespace zge\n{\n    int countModelNodes(int node) { return forEachChild(node); }\n'
                                 '    READABLE_FUNCTION(countModelNodes, Verified);\n}\n',
               'zgui/nodes.cpp': 'namespace zge::zgui\n{\n    void destroyNode(int node) { forEachChild(node); }\n'
                                 '    READABLE_FUNCTION(destroyNode, Verified);\n}\n'}
        for order in ('sorted', 'reversed'):
            lv = self.walk_levels(src, {'forEachChild', 'countModelNodes', 'destroyNode'}, order)
            self.assertEqual(lv['countModelNodes'], (2, ''), order)
            self.assertEqual(lv['destroyNode'], (1, 'callee forEachChild2'), order)

    def test_a_qualified_call_and_a_using_declaration(self):
        src = {'aggressive/menu.cpp': 'namespace aggressive\n{\n    using zge::zgui::forEachChild;\n'
                                      '    void closeMenu(int node) { forEachChild(node); }\n'
                                      '    READABLE_FUNCTION(closeMenu, Verified);\n}\n',
               'scene/remove.cpp': 'namespace zge\n{\n    void removeWidgets(int node) { zgui::forEachChild(node); }\n'
                                   '    READABLE_FUNCTION(removeWidgets, Verified);\n'
                                   '    void removeNodes(int node) { ::zge::forEachChild(node); }\n'
                                   '    READABLE_FUNCTION(removeNodes, Verified);\n}\n'}
        for order in ('sorted', 'reversed'):
            lv = self.walk_levels(src, {'forEachChild', 'closeMenu', 'removeWidgets', 'removeNodes'}, order)
            self.assertEqual(lv['closeMenu'], (1, 'callee forEachChild2'), order)
            self.assertEqual(lv['removeWidgets'], (1, 'callee forEachChild2'), order)
            self.assertEqual(lv['removeNodes'], (2, ''), order)

    def test_a_function_of_the_callers_namespace_hides_an_outer_one(self):
        # (aggressive::forEachChild, registered nowhere, is what the call reaches: neither registered body is charged)
        src = {'aggressive/walk.cpp': 'namespace aggressive\n{\n    void forEachChild(int node) { }\n}\n',
               'aggressive/menu.cpp': 'namespace aggressive\n{\n    using namespace zge;\n'
                                      '    void closeMenu(int node) { forEachChild(node); }\n'
                                      '    READABLE_FUNCTION(closeMenu, Verified);\n}\n'}
        lv = self.walk_levels(src, {'closeMenu'}, 'sorted')
        self.assertEqual(lv['closeMenu'], (2, ''))

    def test_an_unnamed_namespace_is_its_own_files(self):
        # (aggressive/camera.cpp's unnamed setCameraFarPlane is no name camera_far_clip.cpp sees: zge's is its callee)
        src = {'aggressive/walk.cpp': 'namespace aggressive\n{\n    namespace\n    {\n        void forEachChild(int node) { }\n'
                                      '    }\n    void walkNodes(int node) { forEachChild(node); }\n'
                                      '    READABLE_FUNCTION(walkNodes, Verified);\n}\n',
               'aggressive/menu.cpp': 'namespace aggressive\n{\n    using namespace zge;\n'
                                      '    void closeMenu(int node) { forEachChild(node); }\n'
                                      '    READABLE_FUNCTION(closeMenu, Verified);\n}\n'}
        lv = self.walk_levels(src, {'closeMenu', 'walkNodes'}, 'sorted')
        self.assertEqual(lv['walkNodes'], (2, ''))
        self.assertEqual(lv['closeMenu'], (1, 'callee forEachChild'))

    def test_namespace_scopes(self):
        code = ('namespace a::b\n{\n    namespace\n    {\n        int x;\n    }\n    struct S { int f() { return 0; } };\n}\n'
                '#define M { namespace q {\nextern "C" { int y; }\n')
        scopes = readable_symbols.namespace_scopes(code)
        at = lambda text: max((s for s in scopes if s[0] <= code.index(text)), key=lambda s: s[0])[1:3]
        self.assertEqual(at('int x'), (('a', 'b'), True))
        self.assertEqual(at('return 0'), (('a', 'b'), False))
        self.assertEqual(at('int y'), ((), True))
        unnamed = lambda text: max((s for s in scopes if s[0] <= code.index(text)), key=lambda s: s[0])[3]
        self.assertEqual((unnamed('int x'), unnamed('return 0')), (True, False))

class BoundPoints(unittest.TestCase):
    """bound.tsv's point rows: a close function's resume point, read by hand (BMX waitForFrameReady::loop)."""

    def setUp(self):
        warnings.simplefilter('ignore', ResourceWarning)
        self.dir = tempfile.mkdtemp(prefix='readable_tools_test')
        self.saved = readable_symbols._table
        t = readable_symbols.Table(None)
        t.put('function', 'waitForFrameReady', 0x101438)
        t.put('point', 'loop', 0x101448, 'waitForFrameReady')
        t.put('function', 'same', 0x102000)
        t.put('point', 'again', 0x102010, 'same')
        t.put('function', 'unbound', 0x103000)
        t.put('point', 'loop', 0x103010, 'unbound')
        readable_symbols._table = t
        with open(os.path.join(self.dir, 'zz.tsv'), 'w') as f:    # (the bind table: same's body identical, wait close)
            f.write('body\t0x102000\t0x102020\t0x202000\nentry\t0x101438\t0x101500\t0x201438\n')
        self.bind = readable_symbols.Bind(os.path.join(self.dir, 'zz.tsv'))

    def tearDown(self):
        readable_symbols._table = self.saved
        shutil.rmtree(self.dir)

    def bound(self, rows):
        path = os.path.join(self.dir, 'bound.tsv')
        with open(path, 'w') as f:
            f.write('# kind, AI name, address, evidence\n' + ''.join('\t'.join(r) + '\n' for r in rows))
        self.bind.read_bound(path)

    def test_a_point_row(self):
        self.assertIsNone(self.bind.point(0x101448))
        self.bound([('point', 'waitForFrameReady::loop', '0x00201450', 'the wait loop\'s first word, read')])
        self.assertEqual(self.bind.point(0x101448), 0x201450)
        self.assertEqual(self.bind.point(0x102010), 0x202010)     # (a body's points as before)

    def test_bad_point_rows(self):
        for row in [('point', 'waitForFrameReady::nowhere', '0x00201450', 'x'),   # no such AI point
                    ('point', 'unbound::loop', '0x00203010', 'x'),                 # its function unbound
                    ('point', 'waitForFrameReady::loop', '0x00201400', 'x'),       # before the function
                    ('point', 'waitForFrameReady::loop', '0x00201451', 'x'),       # not a word
                    ('point', 'same::again', '0x00202014', 'x'),                   # the table says 0x202010
                    ('point', 'waitForFrameReady::loop', '0x00201450', '')]:       # no evidence
            with self.assertRaises(SystemExit, msg=repr(row)):
                self.bound([row])


class BoundData(unittest.TestCase):
    """bound.tsv's data rows against the table's: a datum the table has only from a close pair's alignment gives way
    to a row read in the game's code (DM2's g_textDeleteThread); one a walk names still stops the row."""

    def setUp(self):
        warnings.simplefilter('ignore', ResourceWarning)
        self.dir = tempfile.mkdtemp(prefix='readable_tools_test')
        self.saved = readable_symbols._table
        t = readable_symbols.Table(None)
        t.put('data', 'g_walked', 0x273000)
        t.put('data', 'g_aligned', 0x273730)
        t.put('data', 'g_unnamed', 0x274000)
        readable_symbols._table = t
        with open(os.path.join(self.dir, 'zz.tsv'), 'w') as f:
            f.write('data\t0x00273000\t\t0x00255000\thigh\tsame\tvotes:3\n'
                    'data\t0x00273730\t\t0x00206A78\taligned\tdiffers\tvotes:1\n')
        self.bind = readable_symbols.Bind(os.path.join(self.dir, 'zz.tsv'))

    def tearDown(self):
        readable_symbols._table = self.saved
        shutil.rmtree(self.dir)

    def bound(self, rows):
        path = os.path.join(self.dir, 'bound.tsv')
        with open(path, 'w') as f:
            f.write('# kind, AI name, address, evidence\n' + ''.join('\t'.join(r) + '\n' for r in rows))
        self.bind.read_bound(path)

    def test_a_row_replaces_an_alignment(self):
        self.assertEqual(self.bind.datum(0x273730), 0x206A78)
        self.bound([('data', 'g_aligned', '0x00255520', 'the label the code loads, read')])
        self.assertEqual(self.bind.datum(0x273730), 0x255520)
        self.assertEqual(self.bind.datum(0x273000), 0x255000)     # (a walked datum as before)

    def test_a_row_names_an_unbound_datum(self):
        self.bound([('data', 'g_unnamed', '0x00256000', 'read')])
        self.assertEqual(self.bind.datum(0x274000), 0x256000)

    def test_a_row_replaces_a_segment_inference(self):
        # g_inferred has no row: two walked rows either side with one delta (0x255000 - 0x273000) make a segment
        # that covers it.
        readable_symbols._table.put('data', 'g_inferred', 0x273008)
        with open(os.path.join(self.dir, 'zz.tsv'), 'a') as f:
            f.write('data\t0x00273010\t\t0x00255010\thigh\tsame\tvotes:2\n')
        self.bind = readable_symbols.Bind(os.path.join(self.dir, 'zz.tsv'))
        self.assertEqual(self.bind.datum(0x273008), 0x255008)
        self.bound([('data', 'g_inferred', '0x00255100', 'the text the code loads, read')])
        self.assertEqual(self.bind.datum(0x273008), 0x255100)
        self.assertEqual(self.bind.datum(0x273000), 0x255000)     # (the walked row as before)

    def test_a_walked_datum_stops_a_row(self):
        with self.assertRaises(SystemExit):
            self.bound([('data', 'g_walked', '0x00255004', 'read')])
        self.bound([('data', 'g_walked', '0x00255000', 'the same address: no error')])


class Entries(unittest.TestCase):
    def test_only_form(self):
        src = ('    inline ee::Float lengthSquared(const Vec3 *v)\n    {\n        return v->x * v->x;\n    }\n'
               '    READABLE_FUNCTION_ONLY(dm2, lengthSquared, Draft);\n')
        out, names = readable_entries.rewrite(src, 'vector.cpp')
        self.assertEqual(names, ['lengthSquared'])
        self.assertEqual(out.count('\n'), src.count('\n'))      # (the lines stay the source's)
        self.assertIn('inline ee::Float lengthSquared_(const Vec3 *v)', out)
        # (the entry: an external function of DM2's instance only, declared as a header declares it)
        self.assertIn('    ee::Float lengthSquared(const Vec3 *v); inline ee::Float lengthSquared_', out)
        self.assertIn(' READABLE_ONLY_DEFINE(dm2, ee::Float lengthSquared(const Vec3 *v) { return ::game::detail::Route'
                      '<&lengthSquared_>::template at<sym::dm2::lengthSquared, ::game::readable::kInstanceGame>(v); })', out)
        self.assertIn('READABLE_FUNCTION_ONLY_AT(dm2, lengthSquared, lengthSquared_, Draft);', out)

    def test_only_form_waits(self):
        src = 'inline int f(int a) { return a; }\nREADABLE_FUNCTION_WAITS_ONLY(bmx, f, Draft);\n'
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertIn('READABLE_FUNCTION_WAITS_ONLY_AT(bmx, f, f_, Draft);', out)
        self.assertIn('READABLE_ONLY_DEFINE(bmx, int f(int a) { return ::game::detail::Route<&f_>::template at<sym::bmx::f, '
                      '::game::readable::kInstanceGame>(a); })', out)

    def test_only_form_is_inline(self):
        with self.assertRaises(readable_entries.Fail):
            readable_entries.rewrite('int f(int a) { return a; }\nREADABLE_FUNCTION_ONLY(dm2, f, Draft);\n', 'x.cpp')
        with self.assertRaises(readable_entries.Fail):
            readable_entries.rewrite('inline int f(int a) { return a; }\nREADABLE_FUNCTION_ONLY(ai, f, Draft);\n', 'x.cpp')

    def test_a_registration_before_its_definition(self):
        # (item 13, d4collision4's query.cpp: the copy named collisionQueryUncached_ before its definition)
        src = ('int h(int a);\nint hOwn(int a, int b = 2);\n'
               'int h(int a) { return a; }\nREADABLE_FUNCTION_OWN(h, Verified, dm2, hOwn);\n'
               'int hOwn(int a, int b) { return a + b; }\n')
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertEqual(out.count('\n'), src.count('\n'))
        line = out.splitlines()[3]
        self.assertTrue(line.startswith('READABLE_OWN_INLINE(dm2) int hOwn_(int a, int b); READABLE_FUNCTION_OWN_AT(h, h_, Verified, dm2, hOwn_);'), line)
        self.assertNotIn('int h_(int a); READABLE', out)      # (h is defined before its registration: as before)

    def test_own_bodies_of_two_versions(self):
        # (item 14, d4render4: drawScene has ZGE2's own body and ZGE4's)
        src = ('int drawScene(int a) { return a; }\nint bySector(int a) { return a + 1; }\nint byTrees(int a) { return a + 2; }\n'
               'READABLE_FUNCTION_WAITS_OWN(drawScene, Verified, dm2, bySector, bmx, byTrees);\n')
        out, names = readable_entries.rewrite(src, 'x.cpp')
        self.assertEqual(sorted(names), ['bySector', 'byTrees', 'drawScene'])
        self.assertIn('READABLE_FUNCTION_WAITS_OWN_AT(drawScene, drawScene_, Verified, dm2, bySector_, bmx, byTrees_);', out)
        for n in ('drawScene', 'bySector', 'byTrees'):      # (each an entry at the shared symbol)
            self.assertIn('Route<&%s_>::template at<sym::drawScene, ::game::readable::kInstanceGame>(a)' % n, out)

    def test_own_bodies_are_their_games_alone(self):
        # (tools4 item 2: an own body is inline outside its game's instance, so another build emits nothing of it, and
        # its entry is its game's)
        src = ('int drawScene(int a) { return a; }\nint bySector(int a) { return a + 1; }\nint byTrees(int a) { return a + 2; }\n'
               'READABLE_FUNCTION_WAITS_OWN(drawScene, Verified, dm2, bySector, bmx, byTrees);\n')
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertEqual(out.count('\n'), src.count('\n'))
        self.assertIn('int drawScene(int a); int drawScene_(int a) { return a; } int drawScene(int a) {', out)
        self.assertIn('int bySector(int a); READABLE_OWN_INLINE(dm2) int bySector_(int a) { return a + 1; } READABLE_ONLY_DEFINE(dm2, int '
                      'bySector(int a) { return ::game::detail::Route<&bySector_>', out)
        self.assertIn('int byTrees(int a); READABLE_OWN_INLINE(bmx) int byTrees_(int a) { return a + 2; } READABLE_ONLY_DEFINE(bmx, int byTrees(', out)
        with self.assertRaises(readable_entries.Fail):
            readable_entries.rewrite('int h(int a) { return a; }\nint g(int a) { return a; }\n'
                                     'READABLE_FUNCTION_OWN(h, Verified, ai, g);\n', 'x.cpp')

    def test_plain_and_own_forms_as_before(self):
        src = ('int g(int a) { return a; }\nREADABLE_FUNCTION(g, Verified);\n'
               'int h(int a) { return a; }\nint hOwn(int a) { return a; }\nREADABLE_FUNCTION_OWN(h, Verified, dm2, hOwn);\n')
        out, names = readable_entries.rewrite(src, 'x.cpp')
        self.assertEqual(sorted(names), ['g', 'h', 'hOwn'])
        self.assertIn('READABLE_FUNCTION(sym::g, g_, Verified);', out)
        self.assertIn('READABLE_FUNCTION_OWN_AT(h, h_, Verified, dm2, hOwn_);', out)
        self.assertIn('Route<&g_>::template at<sym::g, ::game::readable::kInstanceGame>(a)', out)


class EntryReadsPerGame(unittest.TestCase):
    """readable_entry_reads.tsv's game column: a game's row is its instance's or its own body's alone."""
    ROWS = {('f', None): [18], ('f', 'dm2'): [19], ('g', None): [18], ('h', 'dm2'): [19], ('k', 'bmx'): [20],
            ('two', None): [18, 19], ('two', 'bmx'): [18, 21]}

    def setUp(self):
        self.saved = readable_entries._entry_reads
        readable_entries._entry_reads = dict(self.ROWS)

    def tearDown(self):
        readable_entries._entry_reads = self.saved

    BODY = '{ const ee::LeftByCaller<int32_t> w; return a + w; }'

    def test_the_table_reads_both_forms(self):
        readable_entries._entry_reads = None
        rows = readable_entries.entry_reads()
        self.assertEqual(rows[('readHmtlChunk', None)], [18])
        self.assertEqual(rows[('readHmtlChunk', 'dm2')], [19])
        self.assertEqual(rows[('finishUiImageLoad', None)], [19])
        self.assertNotIn(('saveFloatRegisters', None), rows)    # (a range: readable_pass.py's row)

    def test_a_row_without_a_game_is_a_number_as_before(self):
        out, _ = readable_entries.rewrite('int g(int a) %s\nREADABLE_FUNCTION(g, Draft);\n' % self.BODY, 'x.cpp')
        self.assertIn('ee::LeftByCaller<int32_t, 18> w;', out)

    def test_a_shared_body_takes_its_instance_s_row(self):
        out, _ = readable_entries.rewrite('int f(int a) %s\nREADABLE_FUNCTION(f, Draft);\n' % self.BODY, 'x.cpp')
        self.assertIn('ee::LeftByCaller<int32_t, (::game::readable::kInstanceGame == ::game::games::dm2 ? 19 : 18)> w;', out)

    def test_an_own_body_takes_its_game_s_row_of_the_function(self):
        src = ('int h(int a) { return a; }\nint hOwn(int a) %s\nREADABLE_FUNCTION_OWN(h, Draft, dm2, hOwn);\n' % self.BODY)
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertIn('ee::LeftByCaller<int32_t, 19> w;', out)

    def test_an_only_function_takes_its_game_s_row(self):
        out, _ = readable_entries.rewrite('inline int k(int a) %s\nREADABLE_FUNCTION_ONLY(bmx, k, Draft);\n' % self.BODY, 'x.cpp')
        self.assertIn('ee::LeftByCaller<int32_t, 20> w;', out)

    def test_only_the_differing_place_is_a_choice(self):
        src = ('int two(int a) { const ee::LeftByCaller<int32_t> w; const ee::LeftByCaller<int32_t> v; return a + w + v; }\n'
               'READABLE_FUNCTION(two, Draft);\n')
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertIn('ee::LeftByCaller<int32_t, 18> w;', out)
        self.assertIn('ee::LeftByCaller<int32_t, (::game::readable::kInstanceGame == ::game::games::ai ? 19 : '
                      '::game::readable::kInstanceGame == ::game::games::dm2 ? 19 : 21)> v;', out)

    def test_a_game_with_no_row_stops_the_build(self):
        # h has DM2's row alone: a shared body runs in AI's and BMX's instances too; another game's own body has none.
        for src in ('int h(int a) %s\nREADABLE_FUNCTION(h, Draft);\n' % self.BODY,
                    'int h(int a) { return a; }\nint hOwn(int a) %s\nREADABLE_FUNCTION_OWN(h, Draft, bmx, hOwn);\n' % self.BODY,
                    'inline int k(int a) %s\nREADABLE_FUNCTION_ONLY(dm2, k, Draft);\n' % self.BODY):
            with self.assertRaises(readable_entries.Fail, msg=src):
                readable_entries.rewrite(src, 'x.cpp')


class PerVersionSignatures(unittest.TestCase):
    """READABLE_PARAM_ONLY / _EXCEPT in parameter lists and calls, READABLE_RESULT_* and READABLE_RETURN_*
    (ee/readable_waits.h): one body, each version's words (tools4: ZGE4's colour shift, ZGE2's cloneScene)."""

    SRC = ('namespace zge\n{\n'
           '    void colourWord(uint32_t packet, uint32_t vertex READABLE_PARAM_ONLY(bmx, uint32_t colourShift = 1))\n'
           '    {\n        put(packet, vertex >> colourShift);\n    }\n'
           '    READABLE_FUNCTION(colourWord, Verified);\n'
           '    uint32_t clone(uint32_t scene READABLE_PARAM_EXCEPT(dm2, uint32_t flags = kKeepsRegion), uint32_t more = 3)\n'
           '    {\n        return scene | flags | more;\n    }\n'
           '    READABLE_FUNCTION_WAITS(clone, Verified);\n}\n')

    def test_entries_copy(self):
        out, names = readable_entries.rewrite(self.SRC, 'x.cpp')
        self.assertEqual(sorted(names), ['clone', 'colourWord'])
        self.assertEqual(out.count('\n'), self.SRC.count('\n'))
        # the body: the form without its value, and the value a constant at its top for the versions without it
        self.assertIn('void colourWord_(uint32_t packet, uint32_t vertex READABLE_PARAM_ONLY(bmx, uint32_t colourShift))\n'
                      '    { READABLE_PARAM_ONLY_VALUE(bmx, uint32_t colourShift = 1)\n', out)
        self.assertIn('{ READABLE_PARAM_EXCEPT_VALUE(dm2, uint32_t flags = kKeepsRegion)\n', out)
        # the entry: the same signature, the word passed on as the _ARG form
        self.assertIn(' void colourWord(uint32_t packet, uint32_t vertex READABLE_PARAM_ONLY(bmx, uint32_t colourShift)) '
                      '{ return ::game::detail::Route<&colourWord_>::template at<sym::colourWord, '
                      '::game::readable::kInstanceGame>(packet, vertex READABLE_PARAM_ONLY(bmx, colourShift)); }', out)
        self.assertIn('(scene READABLE_PARAM_EXCEPT(dm2, flags), more); }', out)
        # the declaration before the body: no value either (a default argument keeps its place)
        self.assertIn('uint32_t clone(uint32_t scene READABLE_PARAM_EXCEPT(dm2, uint32_t flags), uint32_t more = 3); ', out)
        self.assertNotIn('colourShift = 1))', out)

    def test_an_unregistered_definition_and_a_declaration(self):
        src = ('static void helper(int a READABLE_PARAM_ONLY(dm2, int b = 2)) { use(a, b); }\n'
               'void declared(int a READABLE_PARAM_ONLY(dm2, int b));\n')
        out, _ = readable_entries.rewrite(src, 'x.cpp')
        self.assertIn('helper(int a READABLE_PARAM_ONLY(dm2, int b)) { READABLE_PARAM_ONLY_VALUE(dm2, int b = 2) use', out)
        self.assertIn('void declared(int a READABLE_PARAM_ONLY(dm2, int b));', out)

    def test_what_stops(self):
        for src in ('void f(int a READABLE_PARAM_ONLY(dm2, int b)) { }\n',                 # a definition with no value
                    'void f(int a READABLE_PARAM_ONLY(dm2, int b = 1));\n',               # a declaration with one
                    'void f(READABLE_PARAM_ONLY(dm2, int b = 1)) { }\nREADABLE_FUNCTION(f, Verified);\n',  # first
                    'void f(int a READABLE_PARAM_ONLY(ps2, int b = 1)) { }\n'):            # no such game
            with self.assertRaises(readable_entries.Fail, msg=src):
                readable_entries.rewrite(src, 'x.cpp')

    def test_version_view(self):
        t = ('void f(uint32_t v READABLE_PARAM_ONLY(bmx, uint32_t shift = 1)) { g(v READABLE_PARAM_ONLY(bmx, shift)); '
             'h(s READABLE_PARAM_EXCEPT(dm2, kKeep)); }')
        view = readable_symbols.version_view
        self.assertEqual(view(t), 'void f(uint32_t v ) { g(v ); h(s , kKeep); }')
        self.assertEqual(view(t, 'dm2'), 'void f(uint32_t v ) { g(v ); h(s ); }')
        self.assertEqual(view(t, 'bmx'), 'void f(uint32_t v , uint32_t shift) { g(v , shift); h(s , kKeep); }')
        for g in (None, 'dm2', 'bmx'):      # (offsets and lines kept)
            kept = view(t, g, keep_length=True)
            self.assertEqual(len(kept), len(t))
            self.assertEqual(''.join(kept.split()), ''.join(view(t, g).split()))

    def test_order_and_result_forms(self):
        # (main 11-08, d5zgui's cases: BMX setTagWord returns nothing; another order after the first parameter: the
        # name in each place for its versions, no value; a first parameter one version has not is an own body's)
        src = ('uint32_t openFile(uint32_t file READABLE_PARAM_ONLY(bmx, uint32_t formTag), uint32_t bytes\n'
               '                  READABLE_PARAM_EXCEPT(bmx, uint32_t formTag))\n'
               '{\n    return open(file, formTag, bytes);\n}\nREADABLE_FUNCTION(openFile, Verified);\n'
               'READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagWord(uint32_t record, uint32_t value)\n'
               '{\n    return READABLE_RETURN_EXCEPT(bmx, store(record, value));\n}\nREADABLE_FUNCTION(setTagWord, Verified);\n')
        out, names = readable_entries.rewrite(src, 'x.cpp')
        self.assertEqual(out.count('\n'), src.count('\n'))
        self.assertNotIn('_VALUE(bmx, uint32_t formTag', out)
        self.assertIn('(file READABLE_PARAM_ONLY(bmx, formTag), bytes READABLE_PARAM_EXCEPT(bmx, formTag)); }', out)
        self.assertIn(' READABLE_RESULT_EXCEPT(bmx, uint32_t) setTagWord(uint32_t record, uint32_t value) { return '
                      '::game::detail::Route<&setTagWord_>', out)
        view = readable_symbols.version_view
        ret = 'READABLE_RESULT_EXCEPT(bmx, uint32_t) f() { return READABLE_RETURN_EXCEPT(bmx, g()); }'
        self.assertEqual(view(ret), 'uint32_t f() { return g(); }')
        self.assertEqual(view(ret, 'bmx'), 'void f() { return static_cast<void>(g()); }')
        for g in (None, 'dm2', 'bmx'):
            self.assertEqual(len(view(ret, g, keep_length=True)), len(ret))
        for bad in ('void f(READABLE_PARAM_ONLY(bmx, uint32_t b = 1) uint32_t a) { }\n',     # nothing before it
                    'void f(uint32_t a READABLE_PARAM_ONLY(bmx, uint32_t b = 1), uint32_t c READABLE_PARAM_EXCEPT('
                    'bmx, uint32_t b)) { }\n'):                                                # a value for b
            with self.assertRaises(readable_entries.Fail, msg=bad):
                readable_entries.rewrite(bad + 'READABLE_FUNCTION(f, Verified);\n', 'x.cpp')

    def test_levels_read_this_versions_words(self):
        # (a symbol named only in another version's argument is not one this version's body needs)
        t = 'f() { g(a READABLE_PARAM_ONLY(bmx, sym::onlyBmx)); h(b READABLE_PARAM_EXCEPT(dm2, sym::notDm2)); }'
        kept = ' '.join(readable_symbols.skip_game_steps(t, 'dm2').split())
        self.assertNotIn('onlyBmx', kept)
        self.assertNotIn('notDm2', kept)
        self.assertIn('sym::onlyBmx', readable_symbols.skip_game_steps(t, 'bmx'))


class LocalsOrder(unittest.TestCase):
    """readable_locals_order.py: a row's ordinal is ee::frameLocal's, the count of Locals alive where its Local is
    made (BMX XXX setPathManState, tested2: rows in the source's order put position on rootPosition's place)."""
    BODY = (
        'namespace zge\n{\n'
        '    namespace\n    {\n'
        '        void helper()\n        {\n            ee::Local<int32_t> inHelper;\n            use(inHelper);\n        }\n'
        '    }\n'
        '    ee::Ptr<Motion> setState(int32_t state)\n    {\n'
        '        switch (state)\n        {\n'
        '        default:\n        {\n            ee::Local<char[0x40]> text;\n            return {};\n        }\n'
        '        }\n'
        '        if (state)\n        {\n            ee::Local<Vec4> rootPosition;\n            ee::Local<Vec4> position;\n'
        '            struct Kept { int32_t a; };\n'
        '            const auto later = [&]() { ee::Local<uint32_t> inLambda; use(inLambda); };\n'
        '            later();\n        }\n'
        '        if constexpr (game::readable::kIsDm2)\n        {\n            ee::Local<uint32_t> dm2Only;\n        }\n'
        '        else\n        {\n            ee::Local<uint32_t> notDm2;\n            helper();\n        }\n'
        '        return {};\n    }\n'
        '    READABLE_FUNCTION(setState, Verified);\n}\n')

    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, 'runtime', 'src', 'game', 'zge'))
        os.makedirs(os.path.join(self.root, 'runtime', 'tools'))
        with open(os.path.join(self.root, 'runtime', 'src', 'game', 'zge', 'state.cpp'), 'w') as f:
            f.write(self.BODY)

    def tearDown(self):
        shutil.rmtree(self.root)

    def rows(self, game, rows):
        table = {0: 'readable_locals.tsv', 1: 'readable_locals_dm2.tsv', 2: 'readable_locals_bmx.tsv'}[game]
        with open(os.path.join(self.root, 'runtime', 'tools', table), 'w') as f:
            f.write('# header\n' + ''.join('%d\t0x00100000\tsetState\t%d\t%s\t0x%02X\n' % (game, o, l, x)
                                          for o, l, x in rows))
        return readable_locals_order.check(self.root)

    def test_ordinals_by_the_blocks(self):
        tree = readable_locals_order.Tree(self.root)
        body, _ = tree.body_of('setState', 'bmx')
        made = sorted((o, l) for l, o, _, _, _ in tree.made(body, 'bmx'))
        # (text and rootPosition both first; the lambda's from where it is called; notDm2 first again, the helper's
        # after it)
        self.assertEqual(made, sorted([(0, 'text'), (0, 'rootPosition'), (1, 'position'), (2, 'inLambda'),
                                               (0, 'notDm2'), (1, 'inHelper')]))
        dm2 = sorted((o, l) for l, o, _, _, _ in tree.made(body, 'dm2'))
        self.assertIn((0, 'dm2Only'), dm2)
        self.assertNotIn((0, 'notDm2'), dm2)

    def test_source_order_rows_fail(self):
        problems = self.rows(2, [(0, 'text', 0), (1, 'rootPosition', 0), (2, 'position', 0x10)])
        self.assertTrue(any('puts rootPosition at ordinal 1; the function makes it at 0' in p for p in problems), problems)
        self.assertTrue(any('position (' in p and 'needs a row of its own' in p for p in problems), problems)

    def test_each_local_its_row(self):
        self.assertEqual(self.rows(2, [(0, 'text', 0), (0, 'rootPosition', 0), (0, 'notDm2', 0),
                                       (1, 'position', 0x10), (1, 'inHelper', 0x10), (2, 'inLambda', 0x20)]), [])
        problems = self.rows(2, [(0, 'rootPosition', 0), (0, 'notDm2', 0), (1, 'position', 0x10),
                                 (1, 'inHelper', 0x10)])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn('text (', problems[0])
        # (the lambda's Local, made from where it is called, takes a row like the body's own)
        problems = self.rows(2, [(0, 'text', 0), (0, 'rootPosition', 0), (0, 'notDm2', 0), (1, 'position', 0x10),
                                 (1, 'inHelper', 0x10), (2, 'gone', 0x20)])
        self.assertTrue(any('names gone' in p for p in problems) and any('inLambda (' in p for p in problems), problems)

    def test_a_game_block_is_that_games_alone(self):
        self.assertEqual(self.rows(1, [(0, 'text', 0), (0, 'rootPosition', 0), (0, 'dm2Only', 0),
                                       (1, 'position', 0x10), (2, 'inLambda', 0x20)]), [])

    def test_a_type_with_a_local_base(self):
        # (zge/collision/closest.cpp: `struct VectorOnStack : ee::Local<Vec4>`, `using SquaredVector = VectorOnStack;`,
        # each declaration by either name a Local; locals1 dropped closestPointOnTriangle's five rows as making none)
        with open(os.path.join(self.root, 'runtime', 'src', 'game', 'zge', 'closest.h'), 'w') as f:
            f.write('namespace zge\n{\n    struct VectorOnStack : ee::Local<Vec4>\n    {\n'
                    '        VectorOnStack(const Vec4 &value) { **this = value; }\n    };\n'
                    '    using SquaredVector = VectorOnStack;\n}\n')
        with open(os.path.join(self.root, 'runtime', 'src', 'game', 'zge', 'closest.cpp'), 'w') as f:
            f.write('namespace zge\n{\n    void closest(const Vec4 *v0, const Vec4 *v1, Vec4 *out)\n    {\n'
                    '        const SquaredVector edge0 = *v1 - *v0;\n        if (out)\n        {\n'
                    '            const VectorOnStack along = edge0 * 2.0f;\n            *out = *along;\n        }\n'
                    '    }\n    READABLE_FUNCTION(closest, Verified);\n}\n')
        tree = readable_locals_order.Tree(self.root)
        self.assertEqual(sorted(tree.derived), ['SquaredVector', 'VectorOnStack'])
        body, _ = tree.body_of('closest', 'ai')
        self.assertEqual(sorted((o, l) for l, o, _, _, _ in tree.made(body, 'ai')), [(0, 'edge0'), (1, 'along')])
        path = os.path.join(self.root, 'runtime', 'tools', 'readable_locals.tsv')
        with open(path, 'w') as f:
            f.write('0\t0x00100000\tclosest\t0\tedge0\t0x10\n0\t0x00100000\tclosest\t1\talong\t0x00\n')
        self.assertEqual(readable_locals_order.check(self.root), [])
        with open(path, 'w') as f:
            f.write('0\t0x00100000\tclosest\t0\tedge0\t0x10\n0\t0x00100000\tclosest\t2\talong\t0x00\n')
        problems = readable_locals_order.check(self.root)
        self.assertTrue(any('puts along at ordinal 2; the function makes it at 1' in p for p in problems), problems)

    def test_one_place_an_ordinal(self):
        path = os.path.join(self.root, 'runtime', 'tools', 'readable_locals.tsv')
        with open(path, 'w') as f:
            f.write('0\t0x00100000\tf\t0\ta\t0x00\n0\t0x00100000\tf\t0\tb\t0x00\n0\t0x00100000\tf\t1\tc\t0x10\n')
        self.assertEqual(fncheck_traced_gen.load_locals(path), {(0, 0x00100000): [0, 0x10]})
        with open(path, 'w') as f:
            f.write('0\t0x00100000\tf\t0\ta\t0x00\n0\t0x00100000\tf\t0\tb\t0x20\n')
        with self.assertRaises(SystemExit):
            fncheck_traced_gen.load_locals(path)


class CommittedBindTables(unittest.TestCase):
    """The bind tables are the commit's (tools/bind, tools39): literal_view reads this tree's with no --bind-dir, never a
    shared folder, and `bind --check` compares a table's rows, not the comment naming the database build it came from."""

    def setUp(self):
        self.saved = dict(readable_symbols._binds), readable_symbols.BIND_DIR
        self.dir = tempfile.mkdtemp(prefix='readable_tools_test')

    def tearDown(self):
        binds, readable_symbols.BIND_DIR = self.saved
        readable_symbols._binds.clear()
        readable_symbols._binds.update(binds)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_default_is_this_trees(self):
        self.assertEqual(readable_symbols.BIND, os.path.join(os.path.dirname(os.path.abspath(readable_symbols.__file__)), 'bind'))
        for game in readable_symbols.GAME_ELVES:
            self.assertTrue(os.path.exists(os.path.join(readable_symbols.BIND, game + '.tsv')), game)
            self.assertTrue(os.path.exists(os.path.join(readable_symbols.BIND, game + '.layout.tsv')), game)
        readable_symbols.BIND_DIR = None
        readable_symbols._binds.clear()
        self.assertTrue(readable_symbols.game_bind('bmx').entries)
        readable_symbols.BIND_DIR = ''
        readable_symbols._binds.clear()
        self.assertIsNone(readable_symbols.game_bind('bmx'))

    def test_rows_without_the_build_line(self):
        a, b = os.path.join(self.dir, 'a.tsv'), os.path.join(self.dir, 'b.tsv')
        row = 'body\t0x00100008\t0x001000B4\t0x00100008\thigh\tidentical\texact\n'
        open(a, 'w').write('# from the build of 11-10\n#kind\tai\n' + row)
        open(b, 'w').write('# from the build of 11-17\n#kind\tai\n' + row)
        self.assertEqual(readable_symbols.table_rows(a), readable_symbols.table_rows(b))
        open(b, 'a').write(row.replace('0x00100008\thigh', '0x00100010\thigh'))
        self.assertNotEqual(readable_symbols.table_rows(a), readable_symbols.table_rows(b))
        self.assertIsNone(readable_symbols.table_rows(os.path.join(self.dir, 'none.tsv')))


class RuntimeReplaced(unittest.TestCase):
    """runtime_replaced.py: a stub-only body and a kernel call wrapper are the runtime's; a wrapper with an
    instruction of the game's besides is not."""
    HEAD = '#include "ps2_runtime.h"\n\nvoid %s(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {\n'

    def wrapper(self, name, start, number, extra=''):
        return (self.HEAD % name + '    switch (ctx->pc) {\n        default: break;\n    }\n'
                '    // 0x%x: 0x2403%04x  addiu       $v1, $zero, 0x%X\n'
                '    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), %d));\n' % (start, number, number, number) + extra +
                '    // 0x%x: 0xc  syscall     0\n    ctx->pc = 0x%xu;\nruntime->handleSyscall(rdram, ctx, 0x0u);\n'
                '    // 0x%x: 0x3e00008  jr          $ra\n    {\n        const uint32_t jumpTarget = GPR_U32(ctx, 31);\n'
                '        ctx->pc = jumpTarget;\n        return;\n    }\n}\n' % (start + 4, start + 8, start + 8))

    def test_forms(self):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root)
        files = {
            'sceDmaSend_0x229cf8.cpp': self.HEAD % 'sceDmaSend_0x229cf8' +
            '    ctx->pc = getRegU32(ctx, 31);\n    ps2_stubs::sceDmaSend(rdram, ctx, runtime);\n}\n',
            'RFU116_SetSyscall_0x231298.cpp': self.wrapper('RFU116_SetSyscall_0x231298', 0x231298, 0x74),
            'FUN_00231400_0x231400.cpp': self.wrapper('FUN_00231400_0x231400', 0x231400, 0x5B,
                                                      '    // 0x23140c: 0x8c820000  lw          $v0, 0x0($a0)\n'),
        }
        for name, text in files.items():
            with open(os.path.join(root, name), 'w') as f:
                f.write(text)
        self.assertEqual(runtime_replaced.replaced(root),
                         [(0x229CF8, 'ps2_stubs::sceDmaSend'), (0x231298, 'syscall 0x74')])


if __name__ == '__main__':
    unittest.main()
