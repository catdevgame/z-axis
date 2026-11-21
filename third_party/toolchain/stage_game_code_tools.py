#!/usr/bin/env python3
"""stage_game_code_tools.py OUT --llvm DIR --recompiler PATH --sse2neon PATH [--game ID=CONFIG,FUNCTIONS ...]:
the app's game-code-tools folder (collection/game_code.h: what the app makes each game's code with on the player's
machine), from the bundled compiler (--llvm: the unpacked archive of build-llvm.sh, with bin/ and lib/clang/; or a
compiler of the same layout for a test), this tree's recompiler and module headers, and each game's recompiler inputs.

  bin/clang++, bin/<linker>   the compiler and this host's linker (ld64.lld, lld-link, ld.lld), copied (or the llvm
                              program and its names); bin/ps2_recomp
  lib/clang/                  the compiler's headers
  sdk/                        the module's headers, module_entry.cpp, game_module_flags.txt, key.txt (their hash);
                              vu/: VU1's step and core (ps2/vu1), the ahead-of-time microprograms' sources'
  games/<id>/                 config.toml (its paths are the app's to set; its [vu1] table names the game's VU1
                              microprogram chain, from the symbol tables: tools/vu1_image.py) and ghidra-functions.csv
  toolchain.txt               llvm.lock's version and the compiler's own --version line
  LLVM-LICENSE.TXT            LLVM's licence (Apache-2.0 WITH LLVM-exception), the archive's LICENSE.TXT: it goes with
                              the compiler and linker wherever they are shipped (NOTICE 9)

--tree-games TREE: the three games' inputs from a z-axis tree's games (as recompile.sh makes them): AI's
games/aggressive/workspace/recomp/config.game.toml, DM2's and BMX XXX's config.ns.toml (the collection's namespaced
code) made by games/dm2/workspace/recomp/tools/make-config.py, and each one's ghidra-functions.csv.

--android READELF: the folder for the Android app, from build-llvm.sh's android-arm64 archive and an Android build of
the recompiler, made on another host (nothing staged is run): the programs as the APK's native libraries, native/
libclang.so, liblld.so and libps2_recomp.so (platform/android/build-apk.sh puts them in lib/arm64-v8a, the one folder Android
runs an app's files from, and the rest in its assets), no bin/; toolchain.txt names the archive's version and folder in
place of the compiler's --version. READELF (the NDK's llvm-readelf) checks the programs need the system's libraries only;
the llvm-strip beside it strips them.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(TREE, 'tools', 'setup'))
import vu1_image  # noqa: E402
EXE = '.exe' if os.name == 'nt' else ''
# What a game module's compile reads of the runtime (engine/game_module/game_module.cmake's include path, used): sdk/
# include/ is that path's folders in one (each file by the name a module's #include gives it), found in the tree's
# include roots in this order.
SDK_FILES = ['include/ps2_generated.h', 'include/ee_instructions.h', 'include/ps2_call_list.h',
             'include/runtime/guest_cpu.h', 'include/runtime/ps2_memory_map.h', 'include/runtime/ps2_address.h',
             'include/runtime/lane_watch.h', 'include/vu1/vu1_core.h']
SDK_ROOTS = ['ps2/include', 'ps2', 'engine']
# (sdk name, the tree's folder): every header of it (the game module's own headers, engine/game_module)
SDK_TREES = [('include/game_module', 'engine/game_module')]
# VU1's step, instructions and out-of-line pipelines, what the ahead-of-time microprograms are compiled against (sdk/vu).
SDK_VU = ['vu1_core.cpp', 'vu1_step.h', 'vu1_exec_lower.h', 'vu1_exec_upper.h', 'vu1_detail.h',
          'vu1_aot_program.h', 'vu1_aot_register.h']


def linker_names():
    if sys.platform == 'darwin':
        return ['ld64.lld']
    if os.name == 'nt':
        return ['lld-link']
    return ['ld.lld']


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def foreign_libraries(path):
    """The libraries a staged program on macOS loads from outside the system (/usr/lib, /System): what a player's Mac
    without the build machine's packages (Homebrew's) has not got. Other hosts: none checked here."""
    if sys.platform != 'darwin':
        return []
    out = subprocess.run(['otool', '-L', path], capture_output=True, text=True).stdout.splitlines()[1:]
    libs = [line.split()[0] for line in out if line.strip()]
    return [lib for lib in libs if not lib.startswith(('/usr/lib/', '/System/', '@rpath/', '@loader_path/',
                                                      '@executable_path/'))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--llvm', required=True)
    ap.add_argument('--recompiler', required=True)
    ap.add_argument('--sse2neon', help='sse2neon.h (the ARM hosts\' modules include it; none for x86-64)')
    ap.add_argument('--game', action='append', default=[], help='ID=CONFIG,FUNCTIONS')
    ap.add_argument('--tree-games', metavar='TREE', help='the three games\' inputs from this z-axis tree (above)')
    ap.add_argument('--android', metavar='READELF', help='the Android app\'s folder (above), checked with this llvm-readelf')
    ap.add_argument('--link', action='store_true', help='link the compiler and linker instead of copying them (a test with '
                    'a compiler that needs its own folder\'s libraries)')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    if a.android:
        stage_android_programs(a, out)
    else:
        stage_programs(a, out)
    stage_licence(a, out)
    with tempfile.TemporaryDirectory() as work:
        if a.tree_games:
            a.game += tree_games(os.path.abspath(a.tree_games), work)
        stage_rest(a, out)


def stage_licence(a, out):
    """LLVM's licence beside its programs (build-llvm.sh and build-llvm.ps1 put the release's llvm/LICENSE.TXT at the
    archive's top). A --link test's compiler may have none: nothing staged for a test is shipped."""
    src = os.path.join(a.llvm, 'LICENSE.TXT')
    if os.path.isfile(src):
        copy(src, os.path.join(out, 'LLVM-LICENSE.TXT'))
    elif not a.link:
        sys.exit('stage_game_code_tools.py: no LICENSE.TXT in %s (build-llvm.sh\'s archive has it): the compiler '
                 'cannot be shipped without its licence' % a.llvm)


# The games' ELFs (the configs' input, which the app sets to the player's disc's).
TREE_ELFS = {'ai': 'SLUS_203.27', 'dm2': 'SLUS_201.59', 'bmx': 'SLUS_204.15'}


def tree_games(tree, work):
    ai = os.path.join(tree, 'games', 'aggressive', 'workspace', 'recomp')
    games = ['ai=%s,%s' % (os.path.join(ai, 'config.game.toml'), os.path.join(ai, 'ghidra-functions.csv'))]
    for game in ['dm2', 'bmx']:
        recomp = os.path.join(tree, 'games', game, 'workspace', 'recomp')
        made = os.path.join(work, game, 'config.game.toml')
        os.makedirs(os.path.dirname(made))
        subprocess.run([sys.executable, '-I', os.path.join(tree, 'games', 'dm2', 'workspace', 'recomp', 'tools',
                                                           'make-config.py'),
                        os.path.join(recomp, 'config.toml'), made, '--namespace', game, '--input', TREE_ELFS[game],
                        '--output', 'generated', '--ghidra', 'ghidra-functions.csv'], check=True, stdout=subprocess.DEVNULL)
        games.append('%s=%s,%s' % (game, os.path.join(work, game, 'config.ns.toml'),
                                   os.path.join(recomp, 'ghidra-functions.csv')))
    return games


# The libraries an Android program may need: the system's (the NDK's stable ones); its C++ library is linked in.
ANDROID_SYSTEM = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so'}


def stage_android_programs(a, out):
    native = os.path.join(out, 'native')
    for name, src in [('clang', os.path.join(a.llvm, 'bin', 'clang')), ('lld', os.path.join(a.llvm, 'bin', 'lld')),
                      ('ps2_recomp', a.recompiler)]:
        if not os.path.exists(src):
            sys.exit('stage_game_code_tools.py: no %s' % src)
        dst = os.path.join(native, 'lib%s.so' % name)
        copy(os.path.realpath(src), dst)
        # (stripped, as the APK's runner is: the recompiler's build keeps its symbols)
        subprocess.run([os.path.join(os.path.dirname(a.android), 'llvm-strip'), '--strip-all', dst], check=True)
        info = subprocess.run([a.android, '--file-header', '--dynamic-table', dst], capture_output=True, text=True).stdout
        if 'AArch64' not in info:
            sys.exit('stage_game_code_tools.py: %s is not an AArch64 program' % src)
        needed = {line.split('[')[1].split(']')[0] for line in info.splitlines() if '(NEEDED)' in line}
        if needed - ANDROID_SYSTEM:
            sys.exit('stage_game_code_tools.py: %s needs %s, not on the device' % (src, ', '.join(sorted(needed - ANDROID_SYSTEM))))
    # (the compiler's headers: the archive's own lib/clang/<version>, which game_code.cpp gives it as -resource-dir)
    resources = glob.glob(os.path.join(a.llvm, 'lib', 'clang', '*', 'include'))
    if len(resources) != 1:
        sys.exit('stage_game_code_tools.py: %s has %d lib/clang/<version>/include' % (a.llvm, len(resources)))
    version = os.path.basename(os.path.dirname(resources[0]))
    shutil.copytree(resources[0], os.path.join(out, 'lib', 'clang', version, 'include'))


def stage_programs(a, out):
    # The compiler and linker: the llvm program's names (one file each: a copy keeps the folder movable).
    for name in ['clang++'] + linker_names():
        src = os.path.join(a.llvm, 'bin', name + EXE)
        if not os.path.exists(src):
            src = os.path.join(a.llvm, 'bin', 'llvm' + EXE)   # (the multicall program: it dispatches by its name)
        if not os.path.exists(src):
            sys.exit('stage_game_code_tools.py: no %s in %s' % (name, a.llvm))
        dst = os.path.join(out, 'bin', name + EXE)
        if a.link:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.symlink(os.path.realpath(src), dst)
        else:
            copy(os.path.realpath(src), dst)
    # (the compiler's own headers, as the source compiler finds them: copied beside the staged one, where it looks)
    resource = subprocess.run([os.path.join(a.llvm, 'bin', 'clang++' + EXE), '-print-resource-dir'],
                              capture_output=True, text=True).stdout.strip()
    version = os.path.basename(resource)
    shutil.copytree(os.path.join(resource, 'include'), os.path.join(out, 'lib', 'clang', version, 'include'))
    copy(a.recompiler, os.path.join(out, 'bin', 'ps2_recomp' + EXE))


def sdk_source(name):
    """The tree's file an sdk/include NAME is: the first of the include roots that has it."""
    rel = name[len('include/'):]
    for root in SDK_ROOTS:
        path = os.path.join(TREE, root, rel)
        if os.path.isfile(path):
            return path
    sys.exit('stage_game_code_tools.py: no %s in %s' % (rel, ', '.join(SDK_ROOTS)))


def stage_rest(a, out):
    # The module's headers and their key.
    sdk = os.path.join(out, 'sdk')
    files = [(f, sdk_source(f)) for f in SDK_FILES]
    for name, folder in SDK_TREES:
        for top, _dirs, names in os.walk(os.path.join(TREE, folder)):
            for n in sorted(names):
                if n.endswith(('.h', '.txt')) and n != 'game_module_flags.txt':     # (its headers and their notes)
                    p = os.path.join(top, n)
                    files.append((name + '/' + os.path.relpath(p, os.path.join(TREE, folder)).replace(os.sep, '/'), p))
    files += [('vu/' + n, os.path.join(TREE, 'ps2', 'vu1', n)) for n in SDK_VU]
    files += ([('include/sse2neon.h', a.sse2neon)] if a.sse2neon else []) + [
              ('module_entry.cpp', os.path.join(TREE, 'engine', 'game_module', 'module_entry.cpp')),
              ('game_module_flags.txt', os.path.join(TREE, 'engine', 'game_module', 'game_module_flags.txt'))]
    h = hashlib.sha256()
    for rel, src in sorted(files):
        copy(src, os.path.join(sdk, rel))
        h.update(rel.encode() + b'\0' + hashlib.sha256(open(src, 'rb').read()).digest())
    open(os.path.join(sdk, 'key.txt'), 'w').write(h.hexdigest() + '\n')
    for item in a.game:
        game, paths = item.split('=', 1)
        config, functions = paths.split(',', 1)
        copy(config, os.path.join(out, 'games', game, 'config.toml'))
        # (the renderer's VU1 microprograms: the recompiler writes the micro memory image their chain leaves)
        if game in vu1_image.GAMES:
            with open(os.path.join(out, 'games', game, 'config.toml'), 'a') as f:
                f.write('\n# The VU1 microprograms\' chain (%s, tools/setup/vu1_image.py): the recompiler writes the micro '
                        'memory image it leaves.\n[vu1]\nmicroprogram_chain = "0x%08X"\nmicroprogram_chain_tte = %s\n'
                        % (vu1_image.CHAIN, vu1_image.chain_address(game), 'true' if vu1_image.TTE else 'false'))
        copy(functions, os.path.join(out, 'games', game, 'ghidra-functions.csv'))
    # (every program the app runs takes only what the player's system has)
    for name in sorted(os.listdir(os.path.join(out, 'bin'))) if not a.android else []:
        foreign = foreign_libraries(os.path.join(out, 'bin', name))
        if foreign and not a.link:
            sys.exit('stage_game_code_tools.py: %s loads %s, not on a player\'s system' % (name, ', '.join(foreign)))
    lock = dict(line.split(' ', 1) for line in open(os.path.join(HERE, 'llvm.lock')).read().splitlines()
                if line and not line.startswith('#') and ' ' in line)
    if a.android:
        clang = 'clang version %s (%s)' % (lock.get('version', '?'), os.path.basename(os.path.normpath(a.llvm)))
    else:
        clang = subprocess.run([os.path.join(out, 'bin', 'clang++' + EXE), '--version'], capture_output=True,
                               text=True).stdout.splitlines()[0]
    open(os.path.join(out, 'toolchain.txt'), 'w').write('llvm.lock %s\n%s\n' % (lock.get('version', '?'), clang))
    total = sum(os.path.getsize(os.path.join(t, n)) for t, _d, ns in os.walk(out) for n in ns)
    print('stage_game_code_tools: %s, %.1f MB (%s; sdk key %s)' % (out, total / 1e6, clang, h.hexdigest()[:16]))


if __name__ == '__main__':
    main()
