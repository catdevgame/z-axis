#!/usr/bin/env python3
"""A job's game runners as one pool (remote/mac/gate.sh): every part of a gate takes a runner from it for each game run,
so the parts share the job's runners and end together instead of each holding a fixed few (int27gate: three runners
held by flavour-dm2 for one run while the others sat idle).

The pool is a folder of lock files, runner0 .. runner<budget-1> (flock: a runner is taken while its file is locked by
a live process, so a killed run gives it back); budget is the number in BUDGET_FILE (rcheck writes the job's: 3, or 6
while it holds a second Mac slot), read at every take, so a shrunk budget hands out no more of the upper ones (a run
holding one finishes). Nothing here starts or stops a game: the callers do (career_ab.py run_jobs, gate.sh).

    runner_pool.py run N -- CMD ARGS...   take N runners (waiting for them), then exec CMD holding them
    runner_pool.py want                   runners held, plus those waited for (wait.<pid>, want.<pid> files of live
                                          processes): what the job could use now

Environment: RUNNER_POOL (the folder), RUNNER_POOL_BUDGET (the budget file). Without RUNNER_POOL, run execs at once
and career_ab.py runs as it always has.
"""
import fcntl
import os
import sys
import time

DEFAULT_BUDGET = 3


class Pool:
    def __init__(self, folder, budget_file=None):
        self.folder, self.budget_file = folder, budget_file
        os.makedirs(folder, exist_ok=True)

    @classmethod
    def from_env(cls):
        folder = os.environ.get('RUNNER_POOL')
        return cls(folder, os.environ.get('RUNNER_POOL_BUDGET')) if folder else None

    def budget(self):
        try:
            n = int(open(self.budget_file).read().strip()) if self.budget_file else DEFAULT_BUDGET
        except (OSError, ValueError):
            n = DEFAULT_BUDGET
        return n if n > 0 else DEFAULT_BUDGET

    def take(self):
        """A free runner (its open, locked file: closing it gives the runner back), or None."""
        for k in range(self.budget()):
            f = open(os.path.join(self.folder, 'runner%d' % k), 'a+')
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return f
            except OSError:
                f.close()
        return None

    def held(self):
        """How many runners are taken now (any process's)."""
        n = 0
        for name in os.listdir(self.folder):
            if not name.startswith('runner'):
                continue
            with open(os.path.join(self.folder, name), 'a+') as f:
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    fcntl.flock(f, fcntl.LOCK_UN)
                except OSError:
                    n += 1
        return n

    def note(self, kind, count):
        """wait.<pid> / want.<pid>: runners this process waits for (0: the file goes)."""
        path = os.path.join(self.folder, '%s.%d' % (kind, os.getpid()))
        if count:
            with open(path + '.tmp', 'w') as f:
                f.write('%d\n' % count)
            os.replace(path + '.tmp', path)
        elif os.path.exists(path):
            os.remove(path)

    def waited(self):
        n = 0
        for name in os.listdir(self.folder):
            kind, _, pid = name.partition('.')
            if kind not in ('wait', 'want') or not pid.isdigit():
                continue
            try:
                os.kill(int(pid), 0)
            except OSError:
                continue
            try:
                n += int(open(os.path.join(self.folder, name)).read().strip() or 0)
            except (OSError, ValueError):
                pass
        return n


def run(n, cmd):
    pool = Pool.from_env()
    if pool is not None:
        held = []
        pool.note('wait', n)
        while len(held) < n:   # (one at a time: the only caller needing two is seqcheck, so no two wait on each other)
            f = pool.take()
            if f is None:
                time.sleep(2)
                continue
            held.append(f)
            pool.note('wait', n - len(held))
        for f in held:
            os.set_inheritable(f.fileno(), True)   # (the locks stay with the command: given back when it exits)
    os.execvp(cmd[0], cmd)


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'run' and a[1].isdigit() and a[2] == '--' and len(a) > 3:
        run(int(a[1]), a[3:])
    elif a == ['want']:
        pool = Pool.from_env()
        print(pool.held() + pool.waited() if pool else 0)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
