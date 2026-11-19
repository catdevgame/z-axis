#!/usr/bin/env python3
"""stage_game_code_tools.py OUT --llvm DIR --recompiler PATH --sse2neon PATH [--game ID=CONFIG,FUNCTIONS ...]:
the app's game-code-tools folder (collection/game_code.h: what the app makes each game's code with on the player's
machine), from the bundled compiler (--llvm: the unpacked archive of build-llvm.sh, with bin/ and lib/clang/; or a
compiler of the same layout for a test), this tree's recompiler and module headers, and each game's recompiler inputs.

  bin/clang++, bin/<linker>   the compiler and this host's linker (ld64.lld, lld-link, ld.lld), copied (or the llvm
                              program and its names); bin/ps2_recomp
  lib/clang/                  the compiler's headers
  sdk/                        the module's headers, module_entry.cpp, game_module_flags.txt, key.txt (their hash);
                              vu/: VU1's step and core (src/lib/vu), the ahead-of-time microprograms' sources'
  games/<id>/                 config.toml (its paths are the app's to set; its [vu1] table names the game's VU1
                              microprogram chain, from the symbol tables: tools/vu1_image.py) and ghidra-functions.csv
  toolchain.txt               llvm.lock's version and the compiler's own --version line
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(RUNTIME, 'tools'))
import vu1_image  # noqa: E402
EXE = '.exe' if os.name == 'nt' else ''
# What a game module's compile reads of the runtime (runtime/cmake/game_module.cmake's include path, used).
SDK_FILES = ['include/ps2_generated.h', 'include/ps2_runtime_macros.h', 'include/ps2_call_list.h',
             'include/runtime/guest_cpu.h', 'include/runtime/ps2_memory_map.h', 'include/runtime/ps2_address.h',
             'include/runtime/lane_watch.h', 'include/runtime/ps2_vu1_core.h']
SDK_TREES = ['include/game_module']
# VU1's step, instructions and out-of-line pipelines, what the ahead-of-time microprograms are compiled against (sdk/vu).
SDK_VU = ['ps2_vu1_core.cpp', 'ps2_vu1_step.h', 'ps2_vu1_exec_lower.h', 'ps2_vu1_exec_upper.h', 'ps2_vu1_detail.h',
          'ps2_vu1_aot.h', 'ps2_vu1_aot_register.h']


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
    ap.add_argument('--link', action='store_true', help='link the compiler and linker instead of copying them (a test with '
                    'a compiler that needs its own folder\'s libraries)')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
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
    # The module's headers and their key.
    sdk = os.path.join(out, 'sdk')
    files = [(f, os.path.join(RUNTIME, f)) for f in SDK_FILES]
    for tree in SDK_TREES:
        for top, _dirs, names in os.walk(os.path.join(RUNTIME, tree)):
            for n in sorted(names):
                p = os.path.join(top, n)
                files.append((os.path.relpath(p, RUNTIME), p))
    files += [('vu/' + n, os.path.join(RUNTIME, 'src', 'lib', 'vu', n)) for n in SDK_VU]
    files += ([('include/sse2neon.h', a.sse2neon)] if a.sse2neon else []) + [
              ('module_entry.cpp', os.path.join(RUNTIME, 'src', 'game_module', 'module_entry.cpp')),
              ('game_module_flags.txt', os.path.join(RUNTIME, 'cmake', 'game_module_flags.txt'))]
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
                f.write('\n# The VU1 microprograms\' chain (%s, runtime/tools/vu1_image.py): the recompiler writes the micro '
                        'memory image it leaves.\n[vu1]\nmicroprogram_chain = "0x%08X"\nmicroprogram_chain_tte = %s\n'
                        % (vu1_image.CHAIN, vu1_image.chain_address(game), 'true' if vu1_image.TTE else 'false'))
        copy(functions, os.path.join(out, 'games', game, 'ghidra-functions.csv'))
    # (every program the app runs takes only what the player's system has)
    for name in sorted(os.listdir(os.path.join(out, 'bin'))):
        foreign = foreign_libraries(os.path.join(out, 'bin', name))
        if foreign and not a.link:
            sys.exit('stage_game_code_tools.py: %s loads %s, not on a player\'s system' % (name, ', '.join(foreign)))
    lock = dict(line.split(' ', 1) for line in open(os.path.join(HERE, 'llvm.lock')).read().splitlines()
                if line and not line.startswith('#') and ' ' in line)
    clang = subprocess.run([os.path.join(out, 'bin', 'clang++' + EXE), '--version'], capture_output=True,
                           text=True).stdout.splitlines()[0]
    open(os.path.join(out, 'toolchain.txt'), 'w').write('llvm.lock %s\n%s\n' % (lock.get('version', '?'), clang))
    total = sum(os.path.getsize(os.path.join(t, n)) for t, _d, ns in os.walk(out) for n in ns)
    print('stage_game_code_tools: %s, %.1f MB (%s; sdk key %s)' % (out, total / 1e6, clang, h.hexdigest()[:16]))


if __name__ == '__main__':
    main()
