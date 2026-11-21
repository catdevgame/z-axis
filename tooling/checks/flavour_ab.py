#!/usr/bin/env python3
"""The inter-binary A/B: the app's flavour against the dev build, the same game on both, compared word for word.

  flavour_ab.py run --dir DIR --dev RUNNER --app RUNNER [--game ai|dm2|bmx] [--clock K] [--inputs a,b,...]
                    [--jobs N] [--secs S] [--keep]

The dev build (ZX_DEV_CHECKS ON, runtime/CMakeLists.txt) is what every check runs; the app's flavour (OFF: what
make-app.sh, make-windows.ps1 and the Android build ship) has none of the checkers' machinery and its hooks are
constant nulls, and the gate never runs it. Its proof is this: each of the game's A/B inputs (career_ab.py's
GATE_INPUTS, or the game's `gate` list) runs on both binaries with the defaults on both sides, at one clock (default
4096, the gate's), and career_ab.py compare sets them side by side: the state log at every tick, the scratchpad at
every vsync, every word of every RAM dump, and the hashes of what rendering is handed (VIF1, GIF, VIF0, the GS
register writes) at every vsync. No allow list: the two binaries run the same code on the same input, so nothing
may differ. One line per input; exit 0 only if every input passes. A pass deletes its dumps (--keep keeps them).

Run from the game's workspace, as career_ab.py gate is (AI's for ai, DM2's for dm2 and bmx). Each run is
career_ab.py run's: unpaced, the null GS, a scratch HOME of its own, at most --jobs at a time while memory allows,
with ZX_MC_FIXED_TIME=1 on both sides: the memory card's dates are otherwise the host's clock when a run writes
its card, which two runs started seconds apart see differently (AI's editor input: the card job's date, left in
its stack, 23:15:57 against 23:15:59), as any two runs of one binary would.

Not compared: the scratchpad (career_ab.py hashes it through ZX_FNCYCLES_WATCH, the cycle tracer's, which the
app's flavour does not have); what it holds reaches RAM, the streams and the fields, which are.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import career_ab  # noqa: E402  (its inputs, its runs and its compare)


def inputs_of(game, names):
    """[(input, ticks)] of the game's A/B, in its order (career_ab.py gate's, played routes resolved as it does)."""
    other = career_ab.GAMES.get(game)
    inputs = dict(other['gate'] if other else career_ab.GATE_INPUTS)
    order = names.split(',') if names else [n for n, _ in (other['gate'] if other else career_ab.GATE_INPUTS)]
    for name in order:
        if other and name not in inputs:  # a played route of tools/dm2routes.py: to its end (as gate does)
            script = subprocess.check_output([sys.executable, os.path.join(career_ab.dm2_repo(), 'tools', 'dm2routes.py'),
                                              'script', name], text=True)
            meta = os.path.join(career_ab.SCRATCH_ROOT, 'playdm2', 'sessions', name[len('play-'):], 'meta.json')
            inputs[name] = (json.load(open(meta))['end_tick'] if os.path.exists(meta)
                            else max(int(i.split('v')[0]) for i in script.split(',')) + 20000)
        if name not in inputs:
            raise SystemExit('flavour_ab: %s is not one of %s\'s A/B inputs' % (name, game))
    return [(n, inputs[n]) for n in order]


def run(args):
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'career_ab.py')
    root = os.path.abspath(args.dir)
    os.makedirs(root, exist_ok=True)
    inputs = inputs_of(args.game, args.inputs)
    os.environ['ZX_MC_FIXED_TIME'] = '1'  # (the card's dates: see the top)
    jobs = []
    for name, ticks in inputs:
        for side, runner in (('dev', args.dev), ('app', args.app)):
            jobs.append(('%s-%s' % (name, side), [sys.executable, '-I', tool, 'run', '%s-%s' % (name, side), '--dir', root,
                                                  '--input', name, '--to', str(ticks), '--runner', os.path.abspath(runner),
                                                  '--clock', str(args.clock), '--secs', str(args.secs), '--game', args.game]))
    career_ab.run_jobs(jobs, root, args.jobs)
    failed = []
    for name, ticks in inputs:
        a, b = os.path.join(root, name + '-dev'), os.path.join(root, name + '-app')
        cmd = [sys.executable, '-I', tool, 'compare', a, b, '--out', os.path.join(root, name + '.words.txt')]
        r = subprocess.run(cmd, capture_output=True, text=True)
        with open(os.path.join(root, name + '.compare.txt'), 'w') as f:
            f.write(r.stdout + r.stderr)
        line = re.search(r'^RESULT: (.*)$', r.stdout, re.M)
        fields = re.search(r'(\d+ of \d+) fields equal', r.stdout)
        tick = re.search(r'compared at every tick to tick (\d+)', r.stdout)
        print('%-15s %s: %s fields to tick %s; %s' % (name, 'PASS' if r.returncode == 0 else 'FAIL', fields.group(1) if fields else '?',
                                                    tick.group(1) if tick else '?', line.group(1) if line else 'no result'), flush=True)
        # A side that stops short of the input's end fails, as in career_ab.py gate: the tick run-game.sh stopped it at,
        # not the compare's last tick (the state words' last change). (int27gate: the app's DM2 editor-0 and BMX XXX
        # frontend ran to the 1,200 s limit, never stopped at their tick, and passed on what both had reached.)
        reached = []
        for side in ('dev', 'app'):
            try:
                stops = re.findall(r'^stopped at tick (\d+) ', open(os.path.join(root, '%s-%s.gate.log' % (name, side))).read(), re.M)
            except OSError:
                stops = []
            reached.append(int(stops[-1]) if stops else 0)
        short = min(reached) + 60 < ticks
        if short:
            print('%-15s FAIL: dev reached tick %d, app %d, the input goes to %d (a side stopped short: a hang, a crash or '
                  'the time limit)' % (name, reached[0], reached[1], ticks), flush=True)
        if r.returncode or short:
            failed.append(name)
        elif not args.keep:
            for path in glob.glob(os.path.join(a, 'dump-*.ram')) + glob.glob(os.path.join(b, 'dump-*.ram')):
                os.remove(path)
    print('flavour A/B (%s, the app\'s flavour against the dev build, defaults, clock %d): %d of %d inputs pass%s'
          % (args.game, args.clock, len(inputs) - len(failed), len(inputs), ': failed ' + ','.join(failed) if failed else ''),
          flush=True)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    r = sub.add_parser('run')
    r.add_argument('--dir', required=True)
    r.add_argument('--dev', required=True, help='the dev build\'s zx-runner')
    r.add_argument('--app', required=True, help='the app flavour\'s zx-runner (ZX_DEV_CHECKS OFF)')
    r.add_argument('--game', default='ai', choices=('ai',) + tuple(career_ab.GAMES))
    r.add_argument('--clock', type=int, default=career_ab.GATE_CLOCK)
    r.add_argument('--inputs')
    r.add_argument('--jobs', type=int, default=2)
    r.add_argument('--secs', type=int, default=1200)
    r.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
