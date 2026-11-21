#!/usr/bin/env python3
"""wlane_branches.py SRC OUT: a copy of a game's generated folder (recomp/generated-ns) for the lane differential's tool
build (ZX_LANE_WATCH_BUILD=ON, ZX_RUNNER_EXTRA_GENERATED_DIRS pointed at OUT): every conditional branch's direction and
every register jump's target go through the differential's hooks (runtime/lane_watch.h, ZX_LANE_DIFF):

    const bool branch_taken_0xPC = (COND);     ->  const bool branch_taken_0xPC = ZX_LANE_BRANCH(0xPCu, (COND));
    const uint32_t jumpTarget = GPR_U32(ctx, N);  ->  ... = ZX_LANE_JUMP(PC, GPR_U32(ctx, N));   (PC: the jump's)

Nothing else changes; outside a ZX_LANE_WATCH_BUILD build both macros are their argument. Files that would not change
are hard-linked, the rest written only when their text differs (a rebuild compiles only what changed). A build input
only: the copy is never committed. Prints the counts; exit 1 if a branch or jump form it does not know is found.
"""
import os
import re
import sys

BRANCH = re.compile(r'^(\s*)const bool (branch_taken_(0x[0-9a-f]+)) = (.*);\s*$')
JUMP = re.compile(r'^(\s*)const uint32_t jumpTarget = (GPR_U32\(ctx, \d+\));\s*$')
PC = re.compile(r'ctx->pc = (0x[0-9A-Fa-f]+)u;')


def convert(text):
    out, branches, jumps, odd = [], 0, 0, 0
    last_pc = None
    for line in text.split('\n'):
        m = PC.search(line)
        if m:
            last_pc = m.group(1)
        m = BRANCH.match(line)
        if m:
            out.append('%sconst bool %s = ZX_LANE_BRANCH(%su, (%s));' % (m.group(1), m.group(2), m.group(3), m.group(4)))
            branches += 1
            continue
        m = JUMP.match(line)
        if m and last_pc:
            out.append('%sconst uint32_t jumpTarget = ZX_LANE_JUMP(%su, %s);' % (m.group(1), last_pc, m.group(2)))
            jumps += 1
            continue
        if 'branch_taken_' in line and 'const bool' in line or ('jumpTarget =' in line and 'const' in line):
            odd += 1
        out.append(line)
    return '\n'.join(out), branches, jumps, odd


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = os.path.realpath(sys.argv[1]), sys.argv[2]
    total = [0, 0, 0, 0]
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for name in files:
            a, b = os.path.join(root, name), os.path.join(dst, rel, name)
            text = open(a, errors='surrogateescape').read() if name.endswith('.cpp') else None
            new, br, ju, odd = convert(text) if text is not None else (None, 0, 0, 0)
            total[0] += br
            total[1] += ju
            total[2] += odd
            if new is None or new == text:
                if not os.path.exists(b) or os.stat(b).st_ino != os.stat(a).st_ino:
                    if os.path.exists(b):
                        os.remove(b)
                    os.link(a, b)
                continue
            if not os.path.exists(b) or open(b, errors='surrogateescape').read() != new:
                if os.path.exists(b):
                    os.remove(b)
                with open(b, 'w', errors='surrogateescape') as f:
                    f.write(new)
            total[3] += 1
    print('wlane_branches: %d branches, %d jumps hooked in %d files; %d unknown forms' % tuple(total[:2] + [total[3], total[2]]))
    return 1 if total[2] else 0


if __name__ == '__main__':
    sys.exit(main())
