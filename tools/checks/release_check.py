#!/usr/bin/env python3
"""release_check.py: one package checked as players get it, in seconds, before anything else is done with it.

    release_check.py --mac-app APP | --mac-zip ZIP | --windows-zip ZIP | --windows-dir DIR | --apk APK
                     [--version X.Y.Z] [--tree DIR] [--build-path PATH ...] [--test-apk] [--game-data] [--llvm DIR]

The static checks of a release, shared by everything that makes or uses a package: the packaging scripts right after
they package (platform/mac/make-app.sh PUBLIC=1, platform/windows/make-windows.ps1, platform/android/build-apk.sh
RELEASE=1 or TEST=1), every test of a package as its first step (the first runs, the update tests, the Thor's install,
the placing of the user's copies) and the workshop's release.sh. A package it refuses is never tested, placed or
released: the build that made it is fixed and run again.

What it checks:
  every package: the version (--version, default the tree's VERSION.txt); every text file byte for byte as the tree
    has it (the licences: LICENSE, NOTICE, FFmpeg's LGPL 2.1 as FFMPEG-LICENSE.txt, Michroma's OFL; TREE_TEXT) or as
    made from it by a transform declared here (the Windows README from its template, the APK's game-code-tools as
    stage_game_code_tools.py and build-apk.sh make them, LLVM's files against --llvm, an unpacked llvm.lock archive;
    the packaging tools' plists and signature files), and no text file without one; no game files (a disc folder, disc images, ELFs,
    generated code; --game-data: a developer's package with the game files of their own disc); no path of the machine
    it was built on in its programs: a home folder, a folder of a checkout (tools, engine, ps2, games, collection,
    platform, third_party, third_party-cache, out, scratch ...) under a drive or a home, in ASCII or UTF-16, and each
    --build-path (the checkout, build and cache folders the builder used, which it passes) in any case and slash;
  --mac-app: the ad-hoc signature verifies (codesign --verify --deep --strict), no dylib bundled, every Mach-O and
    LSMinimumSystemVersion at macOS 15.0 at most, no quarantine, CFBundleShortVersionString;
  --mac-zip: no AppleDouble files, and unpacked both ways (ditto, as Finder does, and unzip) to an app that passes
    --mac-app;
  --windows-zip: its entries named with "/", one top folder, then unpacked to a folder that passes --windows-dir;
  --windows-dir: one program, a 64-bit Windows GUI .exe with no DLL beside it, its version resource's ProductVersion;
  --apk: signed (v2 or later) by the collection's release key alone (RELEASE_APK_CERT), not debuggable nor profileable,
    minSdk 33, versionName, arm64-v8a only with the runner and the game-code tools' libraries, the game-code tools
    with LLVM's licence, under GitHub's 2 GiB a file, no game code linked into the runner (generated or VU1
    ahead-of-time names); --test-apk: the test flavour (TEST=1: its own id, the debug key, debuggable), the rest the
    same. Needs the Android SDK's build-tools (ANDROID_HOME, else Homebrew's android-commandlinetools) and a Java
    (JAVA_HOME, else Homebrew's openjdk@17).

Output: a line per problem, then "release-check: pass <what>" or "release-check: FAIL <what> (N problems)".
Exit 0 pass, 1 problems, 2 usage or a package that cannot be read. Stdlib only (python3 -I); runs on the Mac, Linux
and Windows (the Mac's checks need the Mac's tools; the APK's, the SDK's).
"""
import argparse
import glob
import hashlib
import os
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(HERE))   # (the checkout this tools/ folder is in)

# The collection's release key's certificate (apk1, 2026-11-18): its SHA-256, as apksigner prints it.
RELEASE_APK_CERT = os.environ.get('RELEASE_APK_CERT',
                                  '4debecd347842ccd0d3682648b31e9717c23d3d9235809f0e6623dacb0fc3177')
MIN_MACOS = '15.0'
# Each licence a package carries, by its name in the package, and the tree's file it must be byte for byte.
LICENCES = [('LICENSE', 'LICENSE'), ('NOTICE', 'NOTICE'), ('FFMPEG-LICENSE.txt', 'third_party/licenses/FFmpeg-LGPL-2.1.txt')]
# A path of the machine a program was built on: a home folder, or a checkout's or a workshop's folder under a drive
# (MSVC's __FILE__ in a static FFmpeg named three: 1.3.1's dry run). Not any drive path: the games' own strings name
# folders of their developers' drives (BMX XXX's create-a-rider folder, AI's front end's art).
FOLDERS = r'(?:tools|tooling|engine|ps2|games|collection|platform|runtime|iop|third_party|third_party-cache|_deps|scratch|out)'
MACHINE_PATH = re.compile(
    r'[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/]'
    r'|/(?:Users|home)/[A-Za-z0-9_.-]+/'
    r'|[A-Za-z]:[\\/][ -~]{0,200}?[\\/]' + FOLDERS + r'[\\/]', re.I)
GAME_FILES = re.compile(r'(^|/)disc/|\.(iso|bin|cue|chd|cso|zso|elf|vu1code)$|(^|/)SLU[SE]_[0-9]{3}\.[0-9]{2}$'
                        r'|(^|/)generated(-ns|-vu1)?/', re.I)


class Check:
    def __init__(self, what, tree, version, build_paths, game_data, llvm=None):
        self.what, self.tree, self.version, self.game_data, self.llvm = what, tree, version, game_data, llvm
        self.problems = []
        # (each build path in both slashes; matched case-insensitively, as Windows' file names are)
        forms = set()
        for p in build_paths:
            p = p.rstrip('/\\')
            if len(p) >= 4:
                forms |= {p, p.replace('\\', '/'), p.replace('/', '\\')}
        self.build_paths = sorted(forms, key=len, reverse=True)

    def problem(self, text):
        print('release-check: problem: %s' % text)
        self.problems.append(text)

    def game_files(self, names, where):
        if self.game_data:
            return
        found = [n for n in names if GAME_FILES.search(n)]
        if found:
            self.problem('%s carries game files (%d, e.g. %s)' % (where, len(found), ', '.join(found[:3])))

    def machine_paths(self, path, label):
        """No path of the build machine in the program at PATH."""
        with open(path, 'rb') as f:
            self.machine_paths_in(f.read(), label)

    def machine_paths_in(self, data, label, text=False):
        """No path of the build machine in DATA (a program's, in ASCII or UTF-16; text: a text file's, as it is)."""
        texts = ((data.decode('utf-8'),) if text else
                 (data.decode('latin-1'), data.decode('utf-16-le', 'ignore'), data[1:].decode('utf-16-le', 'ignore')))
        found = []
        for t in texts:
            found += [m.group(0) for m in MACHINE_PATH.finditer(t)]
            low = t.lower()
            for p in self.build_paths:
                if p.lower() in low:
                    found.append(p)
        if found:
            self.problem('%s names %d paths of the machine it was built on, e.g. %s'
                         % (label, len(found), '; '.join(sorted(set(x[:140] for x in found))[:3])))

    def end(self):
        if self.problems:
            print('release-check: FAIL %s (%d problems)' % (self.what, len(self.problems)))
            return 1
        print('release-check: pass %s' % self.what)
        return 0


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, **kw)


def files_under(root):
    for d, _, fs in os.walk(root):
        for f in fs:
            yield os.path.join(d, f)


def rel_names(root):
    return [os.path.relpath(p, root).replace(os.sep, '/') for p in files_under(root)]


def folder_reader(root):
    """read(NAME[, LIMIT]): the bytes of the file NAME under root (its first LIMIT), or None."""
    def read(name, limit=-1):
        p = os.path.join(root, *name.split('/'))
        if os.path.islink(p) or not os.path.isfile(p):
            return None
        with open(p, 'rb') as f:
            return f.read(limit)
    return read


# ---- the packaged text files ------------------------------------------------------------------------------------
# Every text file a package carries is the tree's byte for byte (TREE_TEXT), or made from the tree by a transform
# declared here (GENERATED, the Windows README, the APK's game-code-tools); a text file with neither is a problem, so
# a new one is declared here as it is added. Line ends and encodings are in the bytes: a Windows checkout's autocrlf
# (1.3.1's Michroma-OFL.txt) or a build machine's folder in a generated file (1.3.1's DM2 and BMX XXX configs) shows.
# By package, each file by its name there (the Mac app's under the .app, the Windows folder's under it, the APK's
# entries) and the tree's file it is; every one of them must be there.
TREE_TEXT = {
    'mac': [('Contents/Resources/LICENSE', 'LICENSE'), ('Contents/Resources/NOTICE', 'NOTICE'),
            ('Contents/Resources/FFMPEG-LICENSE.txt', 'third_party/licenses/FFmpeg-LGPL-2.1.txt'),
            ('Contents/Resources/fonts/Michroma-OFL.txt', 'collection/resources/fonts/Michroma-OFL.txt')],
    'windows': [('LICENSE.txt', 'LICENSE'), ('NOTICE.txt', 'NOTICE'),
                ('FFMPEG-LICENSE.txt', 'third_party/licenses/FFmpeg-LGPL-2.1.txt'),
                ('fonts/Michroma-OFL.txt', 'collection/resources/fonts/Michroma-OFL.txt')],
    'apk': [('assets/LICENSE', 'LICENSE'), ('assets/NOTICE', 'NOTICE'),
            ('assets/FFMPEG-LICENSE.txt', 'third_party/licenses/FFmpeg-LGPL-2.1.txt'),
            ('assets/fonts/Michroma-OFL.txt', 'collection/resources/fonts/Michroma-OFL.txt')],
}
# Text the packaging tools make and other checks judge, by name (a pattern) and what it must be: the Mac app's
# Info.plist (make-app.sh: its version and minimum macOS, above) and its seal (codesign's, verified above), plists;
# the APK's v1 signature files (apksigner's, verified above), a JAR manifest and its signature file.
GENERATED = {'mac': [(r'Contents/Info\.plist', 'plist'), (r'Contents/_CodeSignature/CodeResources', 'plist')],
             'apk': [(r'META-INF/MANIFEST\.MF', b'Manifest-Version: '), (r'META-INF/[^/]+\.SF', b'Signature-Version: ')]}
# The Windows README: the tree's template, its @APP_NAME@ (the package's folder) and @APP_VERSION@ filled in, lines
# CRLF (make-windows.ps1).
WINDOWS_README = ('README.txt', 'platform/windows/README.txt')
# The APK's game-code-tools (third_party/toolchain/stage_game_code_tools.py, read for its lists): sdk/ the tree's
# module headers and sources, its key.txt their hash; games/<id>/ each game's recompiler config (made from the tree's as
# staging makes it, with the VU1 chain block of tools/setup/vu1_image.py) and ghidra-functions.csv; toolchain.txt from
# llvm.lock; files.txt the folder's list and key (build-apk.sh); sse2neon.h the pinned release's; LLVM's headers and
# licence the llvm.lock archive's (--llvm: any host's unpacked archive, the headers are the same).
TOOLS = 'assets/game-code-tools/'
GAME_DIRS = {'ai': 'aggressive', 'dm2': 'dm2', 'bmx': 'bmx'}
SSE2NEON = ('https://github.com/DLTcollab/sse2neon/archive/refs/tags/v1.9.1.tar.gz',
            '78632498a57bf7e080e84cb8d74c47ce93b204cece77eab79de2908eb8bc92ed')   # (that release's sse2neon.h)
VU1_BLOCK = ('\n# The VU1 microprograms\' chain (%s, tools/setup/vu1_image.py): the recompiler writes the micro memory image '
             'it leaves.\n[vu1]\nmicroprogram_chain = "0x%08X"\nmicroprogram_chain_tte = %s\n')


def is_text(data):
    if b'\0' in data:
        return False
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return False
    return True


def how_differs(got, want):
    """'4488 bytes, 93 CRs; the tree's 4395, 0 CRs; first difference at byte 61'."""
    at = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
    bom = ', a BOM' if got.startswith(b'\xef\xbb\xbf') else ''
    return '%d bytes, %d CRs%s; the tree\'s %d, %d CRs; first difference at byte %d' % (
        len(got), got.count(b'\r'), bom, len(want), want.count(b'\r'), at)


class TextFiles:
    """One package's text files against the tree: compare(NAME, got, want, what) for each, then end()."""

    def __init__(self, c, where):
        self.c, self.where = c, where
        self.same = self.made = 0
        self.unchecked = []

    def compare(self, name, got, want, what, exact=None):
        """got: the package's bytes; want: the tree's (or made from it), or a str saying why there are none; exact:
        the tree's file itself (default: what says "the tree's ...")."""
        if isinstance(want, str):
            self.c.problem('%s: %s: %s' % (self.where, name, want))
        elif got != want:
            self.c.problem('%s: %s is not %s byte for byte (%s)' % (self.where, name, what, how_differs(got, want)))
        elif what.startswith('the tree\'s ') if exact is None else exact:
            self.same += 1
        else:
            self.made += 1

    def end(self):
        n = self.same + self.made
        note = (', not compared: %s' % '; '.join(self.unchecked)) if self.unchecked else ''
        print('release-check: %s: %d text files the tree\'s byte for byte, %d as declared (made from the tree, the '
              'pinned third parties\', the packaging tools\')%s' % (self.where, self.same, self.made, note))
        return n


def text_files(c, kind, names, read, where, game_files=False, llvm=None):
    """Every text file of the package (names: its files; read(NAME): the bytes, or None) checked as above."""
    t = TextFiles(c, where)
    names = set(names)
    done = set()
    for name, tree_name in TREE_TEXT[kind]:
        done.add(name)
        got = read(name)
        if got is None:
            c.problem('%s has no %s (the tree\'s %s)' % (where, name, tree_name))
            continue
        with open(os.path.join(c.tree, tree_name), 'rb') as f:
            t.compare(name, got, f.read(), 'the tree\'s %s' % tree_name)
    for pattern, must in GENERATED.get(kind, []):
        for name in sorted(n for n in names if re.fullmatch(pattern, n)):
            done.add(name)
            data = read(name)
            if must == 'plist':
                try:
                    plistlib.loads(data)
                    t.made += 1
                except Exception as e:  # noqa: BLE001 (any parse error is the problem)
                    c.problem('%s: %s is not a plist (%s)' % (where, name, e))
            elif data.startswith(must) and is_text(data):
                t.made += 1
            else:
                c.problem('%s: %s does not start "%s"' % (where, name, must.decode().strip()))
    if kind == 'windows' and not game_files:
        name, tree_name = WINDOWS_README
        done.add(name)
        got = read(name)
        template = os.path.join(c.tree, tree_name)
        if got is None:
            c.problem('%s has no %s' % (where, name))
        elif not os.path.isfile(template):
            t.compare(name, got, 'not compared: the tree has no %s' % tree_name, '')
        else:
            with open(template, 'rb') as f:
                text = f.read().replace(b'\r\n', b'\n')
            text = text.replace(b'@APP_NAME@', where.encode()).replace(b'@APP_VERSION@', c.version.encode())
            t.compare(name, got, text.replace(b'\n', b'\r\n'), 'the tree\'s %s (its name and version filled in, CRLF)'
                      % tree_name, exact=False)
    if kind == 'apk':
        tools = sorted(n for n in names if n.startswith(TOOLS))
        done |= set(tools)
        if tools:
            game_code_tools(c, t, [n[len(TOOLS):] for n in tools], lambda n: read(TOOLS + n), llvm)
    for name in sorted(names - done):
        if game_files and kind != 'apk':
            continue   # (a developer's package: the disc's files are theirs)
        data = read(name, 65536)
        if data is not None and b"\0" not in data and is_text(read(name)):
            c.problem('%s: %s is a text file with no source declared (release_check.py TREE_TEXT)' % (where, name))
    # (and in every text file, whatever its source, no path of the build machine: 1.1.0 to 1.3.1's APKs named one in
    # two configs, which a declared file that is not compared byte for byte would not show)
    for name in sorted(done):
        data = read(name)
        if data is not None and is_text(data):
            c.machine_paths_in(data, '%s: %s' % (where, name), text=True)
    return t.end()


def stage_lists(tree):
    """stage_game_code_tools.py's lists of what it stages (read, not run)."""
    import ast
    with open(os.path.join(tree, 'third_party', 'toolchain', 'stage_game_code_tools.py')) as f:
        module = ast.parse(f.read())
    lists = {}
    for node in module.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                lists[node.targets[0].id] = ast.literal_eval(node.value)
            except ValueError:
                pass
    return lists


def made_config(tree, game, work):
    """The game's config.toml as staging makes it, without its VU1 block: AI's the tree's config.game.toml, DM2's and
    BMX XXX's the namespaced config make-config.py makes in their recomp folder; or a str saying why there is none."""
    recomp = os.path.join(tree, 'games', GAME_DIRS[game], 'workspace', 'recomp')
    if game == 'ai':
        with open(os.path.join(recomp, 'config.game.toml'), 'rb') as f:
            return f.read()
    elfs = stage_lists(tree).get('TREE_ELFS', {})
    made = os.path.join(work, game, 'config.game.toml')
    os.makedirs(os.path.dirname(made))
    r = run([sys.executable, '-I', os.path.join(tree, 'games', 'dm2', 'workspace', 'recomp', 'tools', 'make-config.py'),
             'config.toml', made, '--namespace', game, '--input', elfs.get(game, '?'), '--output', 'generated',
             '--ghidra', 'ghidra-functions.csv'], cwd=recomp)
    if r.returncode:
        return 'make-config.py failed: %s' % (r.stderr or r.stdout).strip()[:200]
    with open(os.path.join(work, game, 'config.ns.toml'), 'rb') as f:
        return f.read()


def vu1_blocks(tree):
    """Each game's VU1 chain block, as staging appends it (vu1_image.py of the tree, in a process of its own)."""
    r = run([sys.executable, '-I', '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import vu1_image as v; '
             'print(v.CHAIN, v.TTE); [print(g, v.chain_address(g)) for g in v.GAMES]',
             os.path.join(tree, 'tools', 'setup')])
    lines = r.stdout.split('\n')
    if r.returncode or not lines:
        return {}
    chain, tte = lines[0].split()
    return {g: (VU1_BLOCK % (chain, int(a), 'true' if tte == 'True' else 'false')).encode()
            for g, a in (line.split() for line in lines[1:] if line.strip())}


def game_code_tools(c, t, names, read, llvm):
    """The APK's game-code-tools: names under the folder, read(NAME) their bytes."""
    tree = c.tree
    lists = stage_lists(tree)
    want = {}   # name -> (bytes or a str why not, what)

    def tree_file(name, rel):
        p = os.path.join(tree, rel)
        if os.path.isfile(p):
            with open(p, 'rb') as f:
                want[name] = (f.read(), 'the tree\'s %s' % rel)
        else:
            want[name] = ('the tree has no %s' % rel, '')
    # sdk/ (stage_rest: SDK_FILES from the first include root that has it, SDK_TREES' headers and notes, SDK_VU,
    # module_entry.cpp and game_module_flags.txt)
    for f in lists.get('SDK_FILES', []):
        rel = f[len('include/'):]
        root = next((r for r in lists.get('SDK_ROOTS', []) if os.path.isfile(os.path.join(tree, r, rel))), None)
        tree_file('sdk/' + f, (root + '/' + rel) if root else f)
    for sdk_name, folder in lists.get('SDK_TREES', []):
        for top, _dirs, files in os.walk(os.path.join(tree, folder)):
            for n in files:
                if n.endswith(('.h', '.txt')) and n != 'game_module_flags.txt':
                    rel = os.path.relpath(os.path.join(top, n), tree).replace(os.sep, '/')
                    tree_file('sdk/%s/%s' % (sdk_name, rel[len(folder) + 1:]), rel)
    for n in lists.get('SDK_VU', []):
        tree_file('sdk/vu/' + n, 'ps2/vu1/' + n)
    tree_file('sdk/module_entry.cpp', 'engine/game_module/module_entry.cpp')
    tree_file('sdk/game_module_flags.txt', 'engine/game_module/game_module_flags.txt')
    with open(os.path.join(tree, 'CMakeLists.txt')) as f:
        pinned = SSE2NEON[0] in f.read()
    sse = read('sdk/include/sse2neon.h')
    if sse is not None:
        if not pinned:
            want['sdk/include/sse2neon.h'] = ('the tree pins another sse2neon than %s (release_check.py SSE2NEON)'
                                              % SSE2NEON[0], '')
        elif hashlib.sha256(sse).hexdigest() == SSE2NEON[1]:
            want['sdk/include/sse2neon.h'] = (sse, 'the pinned release\'s sse2neon.h')
        else:
            want['sdk/include/sse2neon.h'] = ('not the pinned release\'s sse2neon.h (SHA-256 %s, not %s)'
                                              % (hashlib.sha256(sse).hexdigest(), SSE2NEON[1]), '')
    key = hashlib.sha256()
    for name in sorted(n for n in names if n.startswith('sdk/') and n != 'sdk/key.txt'):
        key.update(name[4:].encode() + b'\0' + hashlib.sha256(read(name)).digest())
    want['sdk/key.txt'] = ((key.hexdigest() + '\n').encode(), 'its sdk files\' key (stage_game_code_tools.py)')
    # games/
    blocks = vu1_blocks(tree)
    with tempfile.TemporaryDirectory(prefix='release-check') as work:
        for game in sorted({n.split('/')[1] for n in names if n.startswith('games/')}):
            if game not in GAME_DIRS:
                continue
            tree_file('games/%s/ghidra-functions.csv' % game,
                      'games/%s/workspace/recomp/ghidra-functions.csv' % GAME_DIRS[game])
            base = made_config(tree, game, work)
            if isinstance(base, str):
                want['games/%s/config.toml' % game] = (base, '')
            else:
                want['games/%s/config.toml' % game] = (
                    base + blocks.get(game, b''), 'its config as staging makes it from the tree (with its VU1 block)')
    # toolchain.txt, files.txt
    with open(os.path.join(tree, 'third_party', 'toolchain', 'llvm.lock')) as f:
        lock = dict(line.split(' ', 1) for line in f.read().splitlines() if line and not line.startswith('#') and ' ' in line)
    version = lock.get('version', '?')
    archive = lock.get('archive-android-arm64', '?').split(' ')[0].replace('.tar.xz', '')
    want['toolchain.txt'] = (('llvm.lock %s\nclang version %s (%s)\n' % (version, version, archive)).encode(),
                             'llvm.lock\'s version and Android archive')
    listed = sorted((n for n in names if n != 'files.txt'), key=lambda n: n.encode())
    sums = ''.join('%s  %s\n' % (hashlib.sha256(read(n)).hexdigest(), n) for n in listed)
    want['files.txt'] = (('key %s\n' % hashlib.sha256(sums.encode()).hexdigest()[:16] + ''.join(n + '\n' for n in listed))
                         .encode(), 'the folder\'s list and key (build-apk.sh)')
    # LLVM's
    llvm_names = [n for n in names if n.startswith('lib/clang/') or n == 'LLVM-LICENSE.TXT']
    if llvm:
        for n in llvm_names:
            p = os.path.join(llvm, 'LICENSE.TXT' if n == 'LLVM-LICENSE.TXT' else n)
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    want[n] = (f.read(), 'LLVM %s\'s %s (the --llvm archive)' % (version, n))
            else:
                want[n] = ('the LLVM archive (--llvm %s) has no %s' % (llvm, n), '')
    elif llvm_names:
        t.unchecked.append('%d of LLVM %s\'s, with no --llvm archive' % (len(llvm_names), version))
    for n in names:
        if n in want:
            body, what = want[n]
            t.compare(TOOLS + n, read(n), body, what)
        elif n not in llvm_names:
            c.problem('%s: %s%s has no source declared (release_check.py game_code_tools)' % (t.where, TOOLS, n))
    for n in sorted(set(want) - set(names)):
        if not isinstance(want[n][0], str) and n != 'sdk/include/sse2neon.h':
            c.problem('%s has no %s%s (%s)' % (t.where, TOOLS, n, want[n][1]))


# ---- the Mac app ------------------------------------------------------------------------------------------------
def mac_app(c, app):
    where = os.path.basename(app)
    if not (os.path.isdir(app) and app.endswith('.app')):
        c.problem('%s is not an app bundle' % app)
        return
    r = run(['codesign', '--verify', '--deep', '--strict', app])
    if r.returncode:
        c.problem('%s: the signature does not verify (%s)' % (where, (r.stderr or r.stdout).strip()[:200]))
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    if info.get('LSMinimumSystemVersion') != MIN_MACOS:
        c.problem('%s asks for macOS %s (LSMinimumSystemVersion), not %s' % (where, info.get('LSMinimumSystemVersion'), MIN_MACOS))
    if info.get('CFBundleShortVersionString') != c.version:
        c.problem('%s is version %s, not %s' % (where, info.get('CFBundleShortVersionString'), c.version))
    fw = os.path.join(app, 'Contents', 'Frameworks')
    if os.path.isdir(fw) and os.listdir(fw):
        c.problem('%s bundles %s' % (where, ' '.join(sorted(os.listdir(fw)))))
    for p in files_under(app):
        if os.path.islink(p) or not os.stat(p).st_mode & stat.S_IXUSR:
            continue
        with open(p, 'rb') as f:
            magic = f.read(4)
        if magic not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xce\xfa\xed\xfe'):
            continue
        out = run(['vtool', '-show-build', p]).stdout
        m = re.search(r'minos\s+(\S+)', out)
        if m and tuple(int(x) for x in m.group(1).split('.')) > tuple(int(x) for x in MIN_MACOS.split('.')):
            c.problem('%s needs macOS %s' % (os.path.relpath(p, app), m.group(1)))
        c.machine_paths(p, os.path.relpath(p, app))
    res = os.path.join(app, 'Contents', 'Resources')
    c.game_files(['Resources/' + n for n in rel_names(res)] if os.path.isdir(res) else [], where)
    if 'com.apple.quarantine' in run(['xattr', '-lr', app]).stdout:
        c.problem('%s is quarantined' % where)
    names = [n for n in rel_names(app) if not os.path.islink(os.path.join(app, n))]
    text_files(c, 'mac', names, folder_reader(app), where, c.game_data)


def mac_zip(c, path):
    names = zipfile.ZipFile(path).namelist()
    if any(re.search(r'(^|/)\._|__MACOSX', n) for n in names):
        c.problem('%s holds AppleDouble files' % os.path.basename(path))
    with tempfile.TemporaryDirectory(prefix='release-check') as work:
        for how, argv in (('ditto', ['ditto', '-x', '-k', path]), ('unzip', ['unzip', '-q', path, '-d'])):
            d = os.path.join(work, how)
            os.makedirs(d)
            r = run(argv + [d])
            apps = glob.glob(os.path.join(d, '*.app'))
            if r.returncode or len(apps) != 1:
                c.problem('%s does not unpack (%s) to one app: %s' % (os.path.basename(path), how, (r.stderr or '').strip()[:200]))
                continue
            mac_app(c, apps[0])


# ---- the Windows zip ------------------------------------------------------------------------------------------------
def pe_kind(path):
    """'gui x64', or what else the program is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    if head[:2] != b'MZ':
        return 'not a PE file'
    off = struct.unpack_from('<I', head, 0x3c)[0]
    if head[off:off + 4] != b'PE\0\0':
        return 'not a PE file'
    machine = struct.unpack_from('<H', head, off + 4)[0]
    magic, = struct.unpack_from('<H', head, off + 24)
    subsystem, = struct.unpack_from('<H', head, off + 24 + 68)
    kind = ('gui' if subsystem == 2 else 'console' if subsystem == 3 else 'subsystem %d' % subsystem)
    arch = 'x64' if machine == 0x8664 and magic == 0x20b else 'machine 0x%x' % machine
    return '%s %s' % (kind, arch)


def product_version(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.search('ProductVersion\0'.encode('utf-16-le') + rb'(?:\0\0)?((?:[^\0]\0)+)\0\0', data)
    return m.group(1).decode('utf-16-le') if m else 'none'


def windows_dir(c, root):
    where = os.path.basename(root.rstrip('/\\'))
    names = rel_names(root)
    exes = [n for n in names if n.lower().endswith('.exe')]
    if len(exes) != 1:
        c.problem('%s holds %d programs, not one' % (where, len(exes)))
    for n in exes:
        p = os.path.join(root, n)
        kind = pe_kind(p)
        if kind != 'gui x64':
            c.problem('%s is not a 64-bit Windows GUI program (%s)' % (n, kind))
        v = product_version(p)
        if v != c.version:
            c.problem('%s\'s version resource says ProductVersion %s, not %s' % (n, v, c.version))
        c.machine_paths(p, n)
    dlls = [n for n in names if n.lower().endswith('.dll')]
    if dlls:
        c.problem('%s carries DLLs: %s' % (where, ' '.join(dlls[:5])))
    c.game_files(names, where)
    text_files(c, 'windows', names, folder_reader(root), where, c.game_data)


def windows_zip(c, path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    where = os.path.basename(path)
    if any('\\' in n for n in names):
        c.problem('%s names its folders with backslashes (Compress-Archive\'s), not /' % where)
    tops = sorted({n.replace('\\', '/').split('/')[0] for n in names})
    if len(tops) != 1:
        c.problem('%s has %d top folders (%s), not the app\'s one' % (where, len(tops), ' '.join(tops[:4])))
    c.game_files(names, where)
    with tempfile.TemporaryDirectory(prefix='release-check') as work:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            if name.endswith('/') or name.startswith('/') or '..' in name.split('/'):
                continue
            dest = os.path.join(work, *name.split('/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(info) as src, open(dest, 'wb') as out:
                shutil.copyfileobj(src, out)
        for top in tops:
            if os.path.isdir(os.path.join(work, top)):
                windows_dir(c, os.path.join(work, top))


# ---- the APK ------------------------------------------------------------------------------------------------
def android_tools():
    brew = ''
    if shutil.which('brew'):
        brew = run(['brew', '--prefix']).stdout.strip()
    sdk = os.environ.get('ANDROID_HOME') or os.path.join(brew, 'share', 'android-commandlinetools')
    env = dict(os.environ)
    env.setdefault('JAVA_HOME', os.path.join(brew, 'opt', 'openjdk@17'))
    bts = sorted(glob.glob(os.path.join(sdk, 'build-tools', '*')),
                 key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r'[.-]', os.path.basename(p))])
    for bt in reversed(bts):
        signer = os.path.join(bt, 'apksigner' + ('.bat' if os.name == 'nt' else ''))
        aapt2 = os.path.join(bt, 'aapt2' + ('.exe' if os.name == 'nt' else ''))
        if os.path.exists(signer) and os.path.exists(aapt2):
            return signer, aapt2, env
    return None, None, env


def apk(c, path, test):
    where = os.path.basename(path)
    signer, aapt2, env = android_tools()
    if not signer:
        print('release-check: no Android build-tools (ANDROID_HOME)')
        sys.exit(2)
    sign = run([signer, 'verify', '--verbose', '--print-certs', path], env=env)
    text = sign.stdout + sign.stderr
    if sign.returncode:
        c.problem('%s: apksigner does not verify it' % where)
    if not re.search(r'Verified using v(2|3) scheme \(APK Signature Scheme v(2|3)\): true', text):
        c.problem('%s has no v2/v3 signature' % where)
    certs = sorted(set(re.findall(r'[Ss]igner.* certificate SHA-256 digest: ([0-9a-f]+)', text)))
    badging = run([aapt2, 'dump', 'badging', path], env=env).stdout
    manifest = run([aapt2, 'dump', 'xmltree', '--file', 'AndroidManifest.xml', path], env=env).stdout
    package = (re.search(r"^package: name='([^']*)'", badging, re.M) or [None, ''])[1]
    if test:
        if not package.endswith('.test'):
            c.problem('%s is not the test flavour (package %s)' % (where, package))
    else:
        if not ('Number of signers: 1' in text and certs == [RELEASE_APK_CERT]):
            c.problem('%s is signed by %s, not the release key alone (%s)' % (where, ' '.join(certs) or 'nothing', RELEASE_APK_CERT))
        if re.search(r'^application-debuggable', badging, re.M):
            c.problem('%s is debuggable' % where)
        if 'E: profileable' in manifest:
            c.problem('%s is profileable' % where)
    if (re.search(r"^minSdkVersion:'(\d+)'", badging, re.M) or [None, ''])[1] != '33':
        c.problem('%s\'s minSdk is not 33' % where)
    vname = (re.search(r"versionName='([^']*)'", badging) or [None, ''])[1]
    if vname != c.version:
        c.problem('%s\'s versionName is %s, not %s' % (where, vname, c.version))
    z = zipfile.ZipFile(path)
    names = z.namelist()
    libs = sorted(n for n in names if n.startswith('lib/'))
    want = ['lib/arm64-v8a/libclang.so', 'lib/arm64-v8a/liblld.so', 'lib/arm64-v8a/libps2_recomp.so',
            'lib/arm64-v8a/libzx-runner.so']
    if libs != want:
        c.problem('%s\'s native libraries are %s, not the runner and the game code\'s tools' % (where, ' '.join(libs)))
    if 'assets/game-code-tools/files.txt' not in names:
        c.problem('%s has no game-code-tools (it cannot make the game code)' % where)
    if 'assets/game-code-tools/LLVM-LICENSE.TXT' not in names:
        c.problem('%s ships LLVM\'s clang and lld without LLVM-LICENSE.TXT' % where)

    entries = set(names)

    def read(name, limit=-1):
        if name not in entries:
            return None
        with z.open(name) as f:
            return f.read(limit)
    text_files(c, 'apk', [n for n in names if not n.endswith('/')], read, where, c.game_data, c.llvm)
    c.game_files(names, where)
    size = os.path.getsize(path)
    if size >= 2 ** 31:
        c.problem('%s is %d bytes, over GitHub\'s 2 GiB a file' % (where, size))
    with tempfile.TemporaryDirectory(prefix='release-check') as work:
        for lib in libs:
            dest = os.path.join(work, os.path.basename(lib))
            with z.open(lib) as src, open(dest, 'wb') as out:
                shutil.copyfileobj(src, out)
        runner = os.path.join(work, 'libzx-runner.so')
        if os.path.exists(runner):
            c.machine_paths(runner, 'libzx-runner.so')
            # (no game code in the runner: a build with the games' modules linked in names their generated
            # functions, entry_<address>_0x<address> and FUN_<address>_0x<address>, and its VU1 microprograms
            # compiled ahead of time, VU1AotProgram<hash>; the device makes each module apart)
            with open(runner, 'rb') as f:
                data = f.read()
            n = len(re.findall(rb'(?:entry|FUN)_[0-9a-f]{8}_0x[0-9a-f]+|VU1AotProgramILm[0-9]', data))
            if n:
                c.problem('libzx-runner.so links game code (%d generated or VU1 ahead-of-time names)' % n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    kind = ap.add_mutually_exclusive_group(required=True)
    kind.add_argument('--mac-app')
    kind.add_argument('--mac-zip')
    kind.add_argument('--windows-zip')
    kind.add_argument('--windows-dir')
    kind.add_argument('--apk')
    ap.add_argument('--version', help='the version it must be (default: the tree\'s VERSION.txt)')
    ap.add_argument('--tree', default=TREE, help='the checkout whose licences and version it must carry')
    ap.add_argument('--build-path', action='append', default=[], help='a folder the build used (repeatable)')
    ap.add_argument('--test-apk', action='store_true', help='the APK is the test flavour (TEST=1)')
    ap.add_argument('--game-data', action='store_true', help='a developer\'s package with game files')
    ap.add_argument('--llvm', help='an unpacked LLVM archive of llvm.lock\'s version (any host\'s): the APK\'s LLVM '
                    'headers and licence compared with it')
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.tree, 'LICENSE')):
        print('release-check: no tree at %s (--tree: the checkout the package was made from)' % a.tree)
        return 2
    version = a.version
    if not version:
        with open(os.path.join(a.tree, 'VERSION.txt')) as f:
            version = f.readline().strip()
    target = a.mac_app or a.mac_zip or a.windows_zip or a.windows_dir or a.apk
    target = os.path.abspath(target)
    if not os.path.exists(target):
        print('release-check: no %s' % target)
        return 2
    # (the builder's own folders are build paths too: the checkout and this machine's home)
    paths = list(a.build_path) + [os.path.abspath(a.tree), os.path.expanduser('~')]
    c = Check(os.path.basename(target), a.tree, version, paths, a.game_data, a.llvm and os.path.abspath(a.llvm))
    try:
        if a.mac_app:
            mac_app(c, target)
        elif a.mac_zip:
            mac_zip(c, target)
        elif a.windows_zip:
            windows_zip(c, target)
        elif a.windows_dir:
            windows_dir(c, target)
        else:
            apk(c, target, a.test_apk)
    except (OSError, zipfile.BadZipFile, plistlib.InvalidFileException, KeyError) as e:
        c.problem('%s cannot be read: %s' % (os.path.basename(target), e))
    return c.end()


if __name__ == '__main__':
    sys.exit(main())
