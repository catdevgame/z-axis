#!/usr/bin/env python3
"""recompile.py: BMX XXX's recompiled code and VU images, made from this workspace's config.

    python3 -I recompile.py [--out RECOMP] [--elf ELF | --iso ISO] [--recompiler PS2_RECOMP] [--like DIR]

One step makes everything of BMX XXX's that comes from its ELF, into RECOMP (default paths.env's
BMX_RECOMP), as DM2's workspace has it in recomp/:
- config.game.toml and config.ns.toml: dm2/recomp/tools/make-config.py from config.toml (the Ghidra
  export) and ghidra-functions.csv here, namespace bmx, with the ELF's and RECOMP's paths;
- generated-ns: ps2_recomp's code from config.ns.toml (the collection's, namespace bmx), with the VU0
  upload beside it (vu0_upload.vif and vu0_upload.recomp: the tree's runtime/tools/vu0_upload.py, which
  game.cmake wants made for this very recompile);
- generated-vu1: the VU1 micro memory image (the tree's runtime/tools/vu1_image.py), which the build
  makes the VU1 ahead-of-time code from.
The ELF is ELF, else SLUS_204.15 from the disc image ISO (extracted into RECOMP/../extracted), else the one
extracted there before, else from GAME_DISCS' "BMX XXX (USA)/BMX XXX-ps2.iso", else BMX_DATA's disc copy; it
must be the USA release the config was made from (SHA-256 below). tooling/recompile.sh --bmx IMAGE (Windows:
recompile.ps1 -Bmx IMAGE) runs this with its recompiler. The recompiler is ps2_recomp of this tree's build (the
ps2_recomp target, docs/BRINGUP.md): --recompiler, PS2_RECOMP, else the first in the tree's out/.

Each folder is made aside and then put in place of the old one. A file the same as the one it replaces,
or as LIKE's (default: the old folder, else the copy builds took before it, BMX_DATA's
recomp), keeps that file's time, so a host that syncs by size and time (rcheck.py) and a build see no
change; the summary says how the new folder compares with that one.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLING = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, TOOLING)
import zpaths  # noqa: E402

ELF_NAME = 'SLUS_204.15'
ELF_SHA256 = '5f4033a8d0048f728b22197bb55291e34a042cc127dba4459f963883128d8f7f'   # BMX XXX (USA): the export's
GAME = 'bmx'
MAKE_CONFIG = os.path.join(TOOLING, 'dm2', 'recomp', 'tools', 'make-config.py')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def toml_path(path):
    """PATH as a TOML string's text: forward slashes (a Windows path's backslashes would be escapes)."""
    return path.replace(os.sep, '/')


def the_elf(args, recomp):
    """The release's ELF: given, extracted from the disc image, or BMX_DATA's disc copy."""
    extracted = os.path.join(os.path.dirname(recomp), 'extracted', ELF_NAME)
    iso = args.iso or os.path.join(zpaths.PATHS.get('GAME_DISCS', ''), 'BMX XXX (USA)', 'BMX XXX-ps2.iso')
    if args.iso and not os.path.isfile(args.iso):
        sys.exit('recompile: no disc image %s' % args.iso)
    if args.elf:
        elf = args.elf
    elif os.path.isfile(extracted) and not args.iso:
        elf = extracted
    elif os.path.isfile(iso):
        if os.path.exists(extracted):
            os.remove(extracted)
        os.makedirs(os.path.dirname(extracted), exist_ok=True)
        # (bsdtar reads ISO 9660; Windows' tar.exe is bsdtar)
        subprocess.run(['bsdtar' if shutil.which('bsdtar') else 'tar', '-xf', iso, '-C', os.path.dirname(extracted),
                        ELF_NAME], check=True)
        os.chmod(extracted, 0o644)
        elf = extracted
        say('extracted %s from %s' % (ELF_NAME, iso))
    else:
        elf = os.path.join(zpaths.PATHS.get('BMX_DATA', ''), 'disc', ELF_NAME)
    if not os.path.isfile(elf):
        sys.exit('recompile: no %s: give --elf, or --iso (the disc image; not at %s)' % (ELF_NAME, iso))
    if sha256(elf) != ELF_SHA256:
        sys.exit('recompile: %s is not the USA release the config was made from (SHA-256 %s)' % (elf, sha256(elf)))
    return elf


def the_recompiler(args, tree):
    if args.recompiler or os.environ.get('PS2_RECOMP'):
        return args.recompiler or os.environ['PS2_RECOMP']
    out = os.path.join(tree, 'out')
    for build in sorted(os.listdir(out)) if os.path.isdir(out) else []:
        for name in ('ps2_recomp', 'ps2_recomp.exe'):
            path = os.path.join(out, build, 'recompiler', 'ps2xRecomp', name)
            if os.path.isfile(path):
                return path
    sys.exit('recompile: no ps2_recomp: build the tree\'s ps2_recomp target (docs/BRINGUP.md) or give --recompiler')


def files(folder):
    """{relative path: absolute path} of FOLDER's files."""
    out = {}
    for top, _, names in os.walk(folder):
        for n in names:
            if n == '.DS_Store' or n.startswith('._'):
                continue
            p = os.path.join(top, n)
            out[os.path.relpath(p, folder)] = p
    return out


def same(a, b):
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def adopt_times(made, like):
    """MADE's files the same as LIKE's take LIKE's times; how MADE compares with LIKE."""
    mine, theirs = files(made), files(like) if like and os.path.isdir(like) else {}
    kept, differ = 0, []
    for rel, path in mine.items():
        other = theirs.get(rel)
        if other and same(path, other):
            st = os.stat(other)
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
            kept += 1
        elif other:
            differ.append(rel)
    only_mine = sorted(set(mine) - set(theirs))
    only_theirs = sorted(set(theirs) - set(mine))
    return len(mine), kept, sorted(differ), only_mine, only_theirs


def put_in_place(made, final):
    old = final + '.old'
    if os.path.exists(old):
        shutil.rmtree(old)
    if os.path.exists(final):
        os.replace(final, old)
    os.replace(made, final)
    if os.path.exists(old):
        shutil.rmtree(old)


def say(text):
    print('recompile: ' + text, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', help='the recomp folder (default paths.env BMX_RECOMP)')
    ap.add_argument('--elf')
    ap.add_argument('--iso')
    ap.add_argument('--recompiler')
    ap.add_argument('--like', help='the recomp folder to compare with and take the times of (default: the old one, '
                                   'else BMX_DATA/recomp)')
    args = ap.parse_args()
    recomp = os.path.abspath(args.out or zpaths.PATHS['BMX_RECOMP'])
    tree = zpaths.ZAXIS
    tools = os.path.join(tree, 'runtime', 'tools')
    for tool in ('vu0_upload.py', 'vu1_image.py'):
        if not os.path.isfile(os.path.join(tools, tool)):
            sys.exit('recompile: the tree %s has no runtime/tools/%s (a tree from before bmxcfg1)' % (tree, tool))
    elf = the_elf(args, recomp)
    recompiler = the_recompiler(args, tree)
    os.makedirs(recomp, exist_ok=True)
    like = args.like
    if like is None:
        like = recomp if os.path.isfile(os.path.join(recomp, 'generated-ns', 'register_functions.cpp')) else \
            os.path.join(zpaths.PATHS.get('BMX_DATA', ''), 'recomp')

    # The configs (make-config.py names its source as given: config.toml).
    subprocess.run([sys.executable, '-I', MAKE_CONFIG, 'config.toml', os.path.join(recomp, 'config.game.toml'),
                    '--namespace', GAME, '--input', toml_path(elf), '--output', toml_path(os.path.join(recomp, 'generated')),
                    '--ghidra', toml_path(os.path.join(HERE, 'ghidra-functions.csv'))], cwd=HERE, check=True)
    aside = tempfile.mkdtemp(prefix='making-', dir=recomp)
    try:
        code, vu1 = os.path.join(aside, 'generated-ns'), os.path.join(aside, 'generated-vu1')
        ns = open(os.path.join(recomp, 'config.ns.toml')).read()
        final_output = 'output = "%s"' % toml_path(os.path.join(recomp, 'generated-ns'))
        if final_output not in ns:
            sys.exit('recompile: config.ns.toml has no %s' % final_output)
        run_config = os.path.join(aside, 'config.ns.toml')
        with open(run_config, 'w') as f:
            f.write(ns.replace(final_output, 'output = "%s"' % toml_path(code)))
        say('%s %s' % (recompiler, run_config))
        with open(os.path.join(aside, 'ps2_recomp.log'), 'w') as log:
            rc = subprocess.run([recompiler, run_config], stdout=log, stderr=subprocess.STDOUT).returncode
        if rc != 0 or not os.path.isfile(os.path.join(code, 'register_functions.cpp')):
            sys.exit('recompile: ps2_recomp failed (%d): %s' % (rc, os.path.join(aside, 'ps2_recomp.log')))
        subprocess.run([sys.executable, '-I', os.path.join(tools, 'vu0_upload.py'), '--game', GAME, '--elf', elf,
                        '--out', os.path.join(code, 'vu0_upload.vif')], check=True)
        subprocess.run([sys.executable, '-I', os.path.join(tools, 'vu1_image.py'), '--game', GAME, '--elf', elf,
                        '--out-dir', vu1], check=True)
        bad = False
        for folder in ('generated-ns', 'generated-vu1'):
            made = os.path.join(aside, folder)
            count, kept, differ, only_mine, only_theirs = adopt_times(made, os.path.join(like, folder))
            say('%s: %d files; against %s: %d the same (their times kept), %d differ%s, %d new%s, %d not made%s' % (
                folder, count, os.path.join(like, folder), kept, len(differ), (' ' + ' '.join(differ[:8])) if differ else '',
                len(only_mine), (' ' + ' '.join(only_mine[:8])) if only_mine else '',
                len(only_theirs), (' ' + ' '.join(only_theirs[:8])) if only_theirs else ''))
            bad = bad or bool(differ or only_mine) and folder == 'generated-ns'
            put_in_place(made, os.path.join(recomp, folder))
        say('made %s from %s' % (recomp, elf))
        if bad and args.like:
            sys.exit(1)
    finally:
        shutil.rmtree(aside, ignore_errors=True)


if __name__ == '__main__':
    main()
