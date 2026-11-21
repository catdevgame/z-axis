#!/usr/bin/env python3
"""Places the swept rows in named subsystem files: no row stays under sweep/ or in a batch's file.

  readable_place.py plan OUT.tsv [--game GAME_DIR] [--db WORKDB]     every row, its file, how it was placed, the evidence
  readable_place.py summary OUT.tsv                                  rows, lines and how, per subsystem and per file
  readable_place.py manifest OUT.tsv [--game GAME_DIR]               the manifests' file and namespace columns from the plan

A plan row: address, name, status, old file, new file, how, evidence. The evidence, best first:

  source    the workdb's module (a source path in the strings, a block of a path) mapped to our folder
  between   both address neighbours are in one authored file: the linker keeps a source file together
  calls     most of its callers and callees that have a place are in one file
  near      the nearest authored function before it, in the same address block
  name      a word of its name is a file's name in its layer
  region    the nearest authored function of its layer before it, within 40: weak, a person checks
  family    FAMILIES below: the name's family, over any evidence weaker than `between`
  layer     nothing better: the layer's own unplaced file, flagged for a person

A row that reaches `layer` is not placed well. The manifest step changes names, never code:
every draft keeps its generated text, and a row is committed as source only once it passes
readable_lint.py (never bulk-commit generated output).
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Original source modules (workdb `modules`, `file_anchors`) to our files.
MODULE_FILES = {
    'zglTerrainRender': 'zge/render/terrain.cpp', 'ps2Movie': 'zge/movie/ps2/pss_player.cpp', 'Ps2MemCard': 'zge/platform/memory_card/save_jobs.cpp',
    'zprofile': 'zge/debug/profile.cpp', 'inlineFx': 'aggressive/fx/inline_fx.cpp', 'zgui': 'zge/zgui/zgui.cpp',
    'ps2Chan': 'zge/sound/channels.cpp', 'zfile': 'zge/assets/file.cpp', 'zobjModel': 'zge/scene/model_objects.cpp',
    'zobj': 'zge/scene/objects.cpp', 'zdebug': 'zge/debug/debug.cpp', 'zsaveapi': 'zge/platform/save_api.cpp',
    'zobjParticleEmitter': 'zge/render/particle_emitters.cpp', 'zscnpts': 'zge/scene/scene_points.cpp',
    'zfxmgr': 'zge/render/fx_manager.cpp', 'zfxMgr': 'aggressive/fx/fx_manager.cpp', 'zTimer': 'zge/platform/timers.cpp',
    'inlineTextureFX': 'zge/render/texture_effects.cpp', 'zobjPEmit': 'zge/render/particle_emitters.cpp',
    'editToolSet': 'aggressive/level/edit_tools.cpp',
}
SDK_FILES = {'libc': 'sce/libc.cpp', 'libm': 'sce/libm.cpp', 'libgcc': 'sce/libgcc.cpp', 'libvu0': 'sce/vu0.cpp', 'libpkt': 'sce/vifpk.cpp',
             'libkernl': 'sce/libkernl.cpp', 'libmpeg': 'sce/libmpeg.cpp', 'libipu': 'sce/libipu.cpp', 'libdev': 'sce/libdev.cpp',
             'libpad': 'sce/libpad.cpp', 'libmc': 'sce/libmc.cpp', 'libgraph': 'sce/libgraph.cpp', 'libcdvd': 'sce/libcdvd.cpp',
             'libdma': 'sce/libdma.cpp', 'libsdr': 'sce/libsdr.cpp', 'libpc': 'sce/libpc.cpp', 'libhttps': 'sce/libhttps.cpp',
             'libsk': 'sce/libsk.cpp'}
# Name families: what a row's name says beats where its neighbours happen to be. A row whose evidence is only a word of its
# name or an address neighbour takes the family's file. First match wins; `source` and `between` evidence stands unless forced.
EDIT = 'aggressive/level/'
FAMILIES = [(re.compile(p), f) for p, f in (
    (r'(?i)pisser', EDIT + 'edit_tool_pisser.cpp'), (r'(?i)crane', EDIT + 'edit_tool_crane.cpp'), (r'(?i)vacuum', EDIT + 'edit_tool_vacuum.cpp'),
    (r'^(terrainTool|\w*EditTerrain)', EDIT + 'edit_tool_terrain.cpp'), (r'(?i)testRunTool|EditTestRun', EDIT + 'edit_tool_test_run.cpp'),
    (r'^railTool|EditRailTool', EDIT + 'edit_tool_rail.cpp'), (r'^objectTool|EditObjectTool', EDIT + 'edit_tool_object.cpp'),
    (r'EditStartTool|^startTool', EDIT + 'edit_tool_start.cpp'), (r'ToolSet|^selectFirstTool|^selectNextTool|^selectPreviousTool|^makeThemeTools|Tool$', 'aggressive/level/edit_tools.cpp'),
    (r'(?i)editCore', EDIT + 'edit_core.cpp'), (r'(?i)editObject', EDIT + 'edit_object.cpp'), (r'(?i)tower', EDIT + 'edit_tower.cpp'),
    (r'^parkSave', EDIT + 'park_save.cpp'), (r'ParkBuffer|parkBuffer', EDIT + 'park_buffer.cpp'), (r'(?i)competition', EDIT + 'competition.cpp'),
    (r'ScriptTimer', 'aggressive/script/timers.cpp'), (r'CarriedAnimal|carriedAnimal|animalCarrier|carrierAnimal|detachAnimal', 'zge/scene/carried_objects.cpp'),
    (r'CardGame|RandomCard|^collectCard|^resetCards', 'aggressive/card_game.cpp'),
    (r'^pcfs', 'zge/assets/pcfs.cpp'), (r'^lz|^copyByteRun|^decodeFileBlock|^decompress', 'zge/assets/decompress.cpp'),
    (r'SolverMatri|solverMatri', 'zge/collision/solver_matrix.cpp'),
    (r'(?i)ffd', 'zge/scene/ffd.cpp'), (r'Globals|^globals', 'aggressive/globals_file.cpp'), (r'(?i)zfileLibrary', 'aggressive/zfile_library.cpp'),
    (r'^(?:newTask|insertTask|deleteTask|initTaskNodePools|destroyTaskPools|markTaskFlag)', 'zge/platform/tasks.cpp'),
    (r'^(?:startEngine|shutdownEngine|runEngineHooks|engineNoop|engineStopHook|engineStartHook|initRenderStages|runEngineHooksAlias|passThroughHook)', 'zge/platform/engine.cpp'),
    (r'(?i)primitiveRenderer|^drawPrimitiveStrips|^drawTexturedPrimitives|^freePrimitiveMaterials|^createPrimitiveMaterials', 'zge/render/primitives.cpp'),
    (r'(?i)bezier', 'zge/math/curve.cpp'), (r'^(?:initMath|shutdownMath|randomNumber)$', 'zge/math/random.cpp'),
    (r'^memory(?:Compare|Set)', 'zge/text/cstring.cpp'), (r'^list[A-Z]|^initList$', 'zge/container/list.cpp'),
    (r'CursorArray|PointerArray|pointerArray|^array(?:First|Next)$|^firstCursorItem$|^current$', 'zge/container/array.cpp'),
    (r'SearchTree|searchTree', 'zge/container/tree.cpp'), (r'^(?:new|free|spawn|draw)?(?:Spark|Billboard|Poly)|^bounceSparks|^spawnSplashTemplate|^sparkType|^billboardType', 'zge/render/particle_instances.cpp'),
    (r'(?i)music', 'zge/sound/music.cpp'), (r'(?i)loadedTexture', 'zge/render/loaded_textures.cpp'), (r'^free(?:All)?SoundRecords?$', 'zge/sound/effects.cpp'),
    (r'(?i)boneChain', 'zge/scene/bone_chain.cpp'), (r'^(?:sumObjectSaveSize|saveObjectState|objectVelocity)$', 'zge/scene/objects.cpp'),
    (r'WorldMesh|^matchWorldName', 'zge/scene/world_mesh.cpp'), (r'(?i)lightObject', 'zge/scene/light_objects.cpp'),
    (r'AnimObj|^attachScriptObject$|^returnOneHandler$|^freeComponentAnimator$|^playAnimSoundEvent$|^animObjectVelocity$', 'aggressive/script/anim_objects.cpp'),
    (r'^rwf|DvdDevice|DvdReader|^(?:init|shutdown)FileDevicesFlag$|^registerFileDevices$', 'zge/movie/file_devices.cpp'),
    (r'^setParkSaveNameAndAuthor|^freeParkName|^loadParkSaveIcon', EDIT + 'park_save.cpp'), (r'^inlineMemCard', 'aggressive/game_save.cpp'),
    (r'^detachAllAnimals$|^sendAttachmentMessage$', 'zge/scene/carried_objects.cpp'), (r'^(?:bodyBox|pushBodyFromPoint)$', 'aggressive/level/dynamics_objects.cpp'),
    (r'(?i)breakable', 'aggressive/level/breakables.cpp'), (r'^(?:firstBreakable|setBreakable|breakableWhole|nextBreakable)$', 'aggressive/level/breakables.cpp'),
    (r'^(?:placeEditPiece|readEditPiece|destroyEditPiece|buildParkLevel|openParkEditor)$', EDIT + 'park_editor.cpp'), (r'^matchPictureByHash|[Pp]icture', 'zge/render/pictures.cpp'),
    (r'^(?:newMaterial|releaseMaterial|freeMaterial|dropMaterialReference|setMaterialTexture|queueMaterialTexture|addMaterialTexture|updateMaterialAlphaFlag|shutdownMaterialPool|initTextures|shutdownTextures)$', 'zge/render/materials.cpp'),
    (r'^transformVecByRows$', 'zge/math/matrix.cpp'), (r'^scriptStubZero$', 'aggressive/script/vm.cpp'),
)]
BATCH = re.compile(r'(^|/)(sweep/|blocks_|hooked_)')


def manifests(game):
    rows = []
    for layer in ('zge', 'aggressive', 'sce'):
        path = ztree.zlayout.readable_path(game, layer + '/sweep/manifest.txt')
        for line in open(path, errors='replace'):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            rows.append({'addr': int(p[0], 16), 'status': p[1], 'file': p[2], 'flags': p[5], 'name': p[6], 'layer': layer})
    return rows


def plan(args):
    import duckdb
    con = duckdb.connect(args.db, read_only=True)
    game = os.path.abspath(args.game)
    rows = manifests(game)
    swept = {r['addr'] for r in rows}
    authored = dict(con.execute("SELECT addr, file FROM readable WHERE file NOT LIKE '%sweep%' AND kind <> 'PLANNED'").fetchall())
    authored = {a: f for a, f in authored.items() if a not in swept}
    addresses = sorted(authored)
    import bisect
    modules = {a: (m, p, h, b) for a, m, p, h, b in con.execute(
        "SELECT func, module, path, how, block FROM modules WHERE build = 'ai_ps2'").fetchall()}
    order = [a for (a,) in con.execute("SELECT addr FROM functions WHERE build = 'ai_ps2' ORDER BY addr").fetchall()]
    position = {a: i for i, a in enumerate(order)}
    calls = collections.defaultdict(collections.Counter)
    for caller, callee in con.execute("SELECT caller, callee FROM calls WHERE build = 'ai_ps2'").fetchall():
        if callee in authored:
            calls[caller][authored[callee]] += 1
        if caller in authored:
            calls[callee][authored[caller]] += 1
    out = []
    placed = {}
    stems = collections.defaultdict(set)
    for f in set(authored.values()):
        for w in re.findall(r'[a-z]+', os.path.splitext(os.path.basename(f))[0]):
            if len(w) > 3:
                stems[w].add(f)
    for r in sorted(rows, key=lambda r: r['addr']):
        a = r['addr']
        module, path, how, block = modules.get(a, ('', '', '', None))
        module, path, how = module or '', path or '', how or ''
        new, why, evidence = None, None, ''
        if module.startswith('sdk:') and module[4:] in SDK_FILES:
            new, why, evidence = SDK_FILES[module[4:]], 'source', module
        elif module in MODULE_FILES and how in ('file', 'block', 'between'):
            new, why, evidence = MODULE_FILES[module], 'source', '%s (%s)' % (path, how)
        if not new:
            i = bisect.bisect_left(addresses, a)
            before = authored[addresses[i - 1]] if i else None
            after = authored[addresses[i]] if i < len(addresses) else None
            if before and before == after:
                new, why, evidence = before, 'between', '%x .. %x' % (addresses[i - 1], addresses[i])
            elif calls[a] and calls[a].most_common(1)[0][1] >= 2 and calls[a].most_common(1)[0][1] * 2 > sum(calls[a].values()):
                f, n = calls[a].most_common(1)[0]
                new, why, evidence = f, 'calls', '%d of %d' % (n, sum(calls[a].values()))
            elif before and position.get(a, 0) - position.get(addresses[i - 1], -99) <= 12 and before.startswith(r['layer']):
                new, why, evidence = before, 'near', '%x, %d functions back' % (addresses[i - 1], position.get(a, 0) - position.get(addresses[i - 1], 0))
        if not new:
            # A word of its name is a file's: emitter, chunk, follower, card, sound ...
            words = [w.lower() for w in re.findall(r'[A-Z]?[a-z]+|[A-Z]+(?![a-z])', r['name']) if len(w) > 3]
            hits = collections.Counter()
            for w in words:
                for stem, files in stems.items():
                    if w == stem or (len(w) > 4 and stem.startswith(w)) or (len(stem) > 4 and w.startswith(stem)):
                        for f in files:
                            if f.startswith(r['layer']):
                                hits[f] += 1
            if hits and hits.most_common(1)[0][1] >= 1:
                new, why, evidence = hits.most_common(1)[0][0], 'name', ' '.join(words)
        if not new:
            i = bisect.bisect_left(addresses, a)
            for j in range(i - 1, max(i - 40, -1), -1):
                if authored[addresses[j]].startswith(r['layer']):
                    new, why, evidence = authored[addresses[j]], 'region', '%x' % addresses[j]
                    break
        if not new:
            new, why, evidence = r['layer'] + '/unplaced.cpp', 'layer', ''
        if why not in ('source', 'between') or not new.startswith(r['layer']):
            for pattern, target in FAMILIES:
                if pattern.search(r['name']):
                    new, why, evidence = target, 'family', pattern.pattern
                    break
        placed[a] = new
        out.append((a, r['name'], r['status'], r['flags'], r['file'], new, why, evidence))
    with open(args.out, 'w') as f:
        f.write('address\tname\tstatus\tflags\told\tnew\thow\tevidence\n')
        for row in out:
            f.write('0x%08X\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' % row)
    return summary(args)


def summary(args):
    rows = [l.rstrip('\n').split('\t') for l in open(args.out)][1:]
    print('%d rows' % len(rows))
    print('how: ' + ', '.join('%s %d' % kv for kv in collections.Counter(r[6] for r in rows).most_common()))
    print('still named for a batch: %d' % sum(1 for r in rows if BATCH.search(r[5])))
    folders = collections.Counter(os.path.dirname(r[5]) for r in rows)
    for k, n in folders.most_common():
        print('%6d %s' % (n, k))
    print('files %d, new files %d' % (len(set(r[5] for r in rows)), sum(1 for f in set(r[5] for r in rows) if not os.path.exists(ztree.zlayout.readable_path(args.game, f) or os.path.join(args.game, f)))))
    return 0


def manifest(args):
    rows = {int(r[0], 16): r for r in (l.rstrip('\n').split('\t') for l in open(args.out)) if r[0].startswith('0x')}
    game = os.path.abspath(args.game)
    for layer in ('zge', 'aggressive', 'sce'):
        path = ztree.zlayout.readable_path(game, layer + '/sweep/manifest.txt')
        lines = []
        for line in open(path, errors='replace'):
            if not line.startswith('#') and line.strip():
                p = line.rstrip('\n').split('\t')
                new = rows[int(p[0], 16)][5]
                p[2] = new
                p[3] = layer + '::' + re.sub(r'\W', '_', os.path.splitext(os.path.basename(new))[0])
                line = '\t'.join(p) + '\n'
            lines.append(line)
        with open(path, 'w') as f:
            f.writelines(lines)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('step', choices=('plan', 'summary', 'manifest'))
    ap.add_argument('out')
    ap.add_argument('--game', default=None, help='the z-axis tree (default: ztree.py\'s)')
    sys.path.insert(0, os.path.dirname(HERE))
    import zpaths  # (tools/paths.env: the scratch folder)
    ap.add_argument('--db', default=os.path.join(zpaths.ZAXIS_SCRATCH, 'workdb', 'work.duckdb'))
    a = ap.parse_args()
    if a.game is None:
        sys.path.insert(0, HERE)
        import ztree
        a.game = ztree.TREE
    return {'plan': plan, 'summary': summary, 'manifest': manifest}[a.step](a)


if __name__ == '__main__':
    sys.exit(main())
