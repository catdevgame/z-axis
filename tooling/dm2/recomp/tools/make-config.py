#!/usr/bin/env python3
"""Derive config.game.toml from the Ghidra-exported config.toml (copy of the
Aggressive Inline repo's recomp/tools/make-config.py with DM2 additions).

The export stubs every function whose name the runtime knows. For pure EE
library code (no hardware access) the recompiled original is exact while the
runtime HLE is an approximation, and mixing the two is unsafe when they share
a data structure (libvifpk's sceVif1Packet). Names matched by UNSTUB_PREFIXES
are dropped from [general].stubs and listed in [general].recompile (PS2Recomp
branch aggressive-inline), which also overrides the recompiler's name-based
binding to runtime handlers.

usage: make-config.py [config.toml] [config.game.toml] [--namespace NAME] [--input ELF] [--output DIR]
                      [--ghidra CSV]

--namespace: the game's namespace in config.ns.toml (default dm2), which also picks the game's
ADDRESS_STUBS. --input, --output (the game build's folder, .../generated; config.ns.toml's is
.../generated-ns) and --ghidra (the Ghidra export's CSV) take the place of config.toml's paths, so
the export can name paths of the workspace and a tool give the real ones (bmx/recomp/recompile.py).
"""
import argparse, re
ap = argparse.ArgumentParser(usage=__doc__.split('usage: ')[1].split('\n\n')[0])
ap.add_argument('src', nargs='?', default='config.toml')
ap.add_argument('dst', nargs='?', default='config.game.toml')
ap.add_argument('--namespace', default='dm2')
ap.add_argument('--input')
ap.add_argument('--output')
ap.add_argument('--ghidra')
args = ap.parse_args()
src, dst = args.src, args.dst

UNSTUB_PREFIXES = (
    'sceVif1Pk',   # libvifpk packet builder (sceVif1Packet state shared with recompiled callers)
    'sceGifPk',    # libgifpk packet builder (same reason)
    'sceVu0',      # libvu0: pure COP2 macro code. The HLE versions differ from the
                   # originals (MulMatrix operand order reversed; ecossin is a
                   # register-ABI helper (vf6 in, vf4 out) that the HLE treats as a
                   # C function writing to a0), which broke all 3D transforms.
)
# Exact names: pure newlib string/memory routines. The recompiled originals
# are exact; the HLE versions are only approximations.
UNSTUB_NAMES = (
    'memcmp', 'memchr', 'memmove', 'strcasecmp', 'strncasecmp', 'strcat', 'strchr',
    'strcmp', 'strcpy', 'strlen', 'strncmp', 'strncpy', 'strrchr', 'strlwr', 'strupr',
    '__kernel_cosf', '__kernel_sinf', '__ieee754_rem_pio2f',
    # Dave Mirra: libgraph's double-buffer setup and swap. DM2 uses
    # sceGsSetDefDBuff/sceGsSwapDBuff (not the Dc variants Aggressive Inline
    # uses); the runtime's HLE of those reads the trailing arguments from the
    # stack (they are in t0-t2) and never sends the frame clear the game asks
    # for, so in a level the picture kept the loading screen behind the
    # geometry. The originals build the environments in memory and send them
    # with GIF DMA and GS register writes, which the runtime handles; the
    # parameters come from the HLE sceGsResetGraph through sceGsGetGParam
    # (0x1CC910, bound in dave_mirra_bmx2.cpp).
    'sceGsSetDefDBuff', 'sceGsSwapDBuff', 'sceGsSetDefDispEnv', 'sceGsPutDispEnv',
    'sceGszbufaddr', 'sceGsSetDefDrawEnv', 'sceGsSetDefClear', 'sceGsPutDrawEnv',
)

# Functions the Ghidra export left unnamed (FUN_...) that the runtime replaces, stubbed by address
# ("NAME@ADDRESS": the runtime's handler NAME bound at ADDRESS, the generated function keeps its
# FUN_ name), per game. Each is the SDK function its name says: the bind table pairs it with AI's
# (runtime/tools/bind/<game>.tsv: high, identical), and AI's override binds the same handler at its
# address (game_overrides/aggressive_inline_slus20327.cpp kBindings; DM2's dave_mirra_bmx2.cpp too).
ADDRESS_STUBS = {
    'bmx': (
        # libmpeg's sceMpegIsEnd and sceMpegIsRefBuffEmpty (AI 0x207D78, 0x207D88): recompiled,
        # they read decoder state only the real decoder updates, and the runtime's MPEG HLE tracks
        # it instead; without them a movie never ends (AI's intro chain stopped after AKA).
        'sceMpegIsEnd@0x00222CD8',
        'sceMpegIsRefBuffEmpty@0x00222CE8',
        # libcdvd's sceCdDiskReady (AI 0x2144A0): recompiled, it binds SIF RPC 0x8000059A
        # (cdvdfsv, in the IOP BIOS), which iop/ does not provide, and waits there (AI hung at
        # "Initializing CD...").
        'sceCdDiskReady@0x0022C4E0',
    ),
}

text = open(src).read()
for key, value in (('input', args.input), ('output', args.output), ('ghidra_output', args.ghidra)):
    if value is not None:
        text, n = re.subn(r'^%s = ".*"$' % key, lambda m: '%s = "%s"' % (key, value), text, count=1, flags=re.M)
        if n != 1:
            raise SystemExit('make-config: %s has no %s line' % (src, key))
start = text.index('stubs = [')
end = text.index(']', start)
block = text[start:end]
kept, dropped = [], []
for line in block.splitlines()[1:]:
    m = re.match(r'\s*"([^"@]+)(@0x[0-9A-Fa-f]+)?",?\s*$', line)
    if not m:
        kept.append(line); continue
    unstub = m.group(1).startswith(UNSTUB_PREFIXES) or m.group(1) in UNSTUB_NAMES
    (dropped if unstub else kept).append(line)
def stub_address(line):
    m = re.search(r'@0x([0-9A-Fa-f]+)"', line)
    return int(m.group(1), 16) if m else -1
named = {stub_address(l) for l in kept}
for stub in ADDRESS_STUBS.get(args.namespace, ()):
    line = '  "%s",' % stub
    if stub_address(line) in named:
        raise SystemExit('make-config: %s: the export already stubs that address' % stub)
    kept.append(line)
if ADDRESS_STUBS.get(args.namespace):
    kept.sort(key=stub_address)
header = '# Generated by tools/make-config.py from %s; do not edit by hand.\n' % src
recompile = 'recompile = [\n' + ''.join('  "%s*",\n' % p for p in UNSTUB_PREFIXES) + ''.join('  "%s",\n' % n for n in UNSTUB_NAMES) + ']\n'
out = header + text[:start] + recompile + 'stubs = [\n' + '\n'.join(kept) + '\n' + text[end:]
open(dst, 'w').write(out)
# The same code namespaced, for one executable with other games (PS2Recomp
# [general] namespace; CMake PS2X_RUNNER_EXTRA_GENERATED_DIRS): config.ns.toml,
# output recomp/generated-ns.
# (output: ".../generated", or "generated" beside the config, as ps2_recomp reads a relative path)
ns = re.sub(r'^output = "(.*/)?generated"$', lambda m: 'output = "%sgenerated-ns"' % (m.group(1) or ''), out, count=1, flags=re.M)
ns = ns.replace('single_file_output = false\n', 'single_file_output = false\nnamespace = "%s"\n' % args.namespace, 1)
open(dst.replace('config.game.toml', 'config.ns.toml') if 'config.game.toml' in dst else dst + '.ns', 'w').write(ns)
print('dropped %d stubs: %s' % (len(dropped), ', '.join(l.strip().strip('",') for l in dropped)))
