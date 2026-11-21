#!/usr/bin/env python3
"""zguard: the shared tools stop before they write into the z-axis main checkout.

The main checkout ($ZAXIS_MAIN) is the orchestrator's: only its merge and batch scripts (round-merge.sh, batchS.sh,
merge-check.sh, push-batch.sh) work there, and they say so with ZAXIS_IN_MAIN=1 in their environment, which every
tool they start inherits. Any other caller whose working directory, or an output path it names, resolves inside the
main checkout is stopped with a message before anything is written (tools36: three lanes in one week wrote there
from the Bash tool's start folder after a cd that failed: an ai/ folder, a doc commit attempt, a playbmx/ folder).

Inside the main checkout means under $ZAXIS_MAIN after symlinks (os.path.realpath), except
  - the lanes' worktrees ($ZAXIS_WORKTREES, wt/): a lane's own folder;
  - for the working directory only, the main checkout's tools/ and games/ (the scripts cd into a game's tools and
    workspace, games/aggressive/tools and games/dm2/workspace, by design: gate.sh, logiccheck); an output path there
    still counts. The workspaces' data folders
    are links into the data folder (ZAXIS_DATA, data-links.txt), so an output under one resolves outside and is fine.
A host with no main checkout (a worker's copy, the Linux worker, the Windows worker: no $ZAXIS_MAIN/.git) has nothing to guard.

    import zguard; zguard.refuse('replay.py', [out_dir])        # Python (tools/ on sys.path)
    zguard TOOL [PATH ...]                                       # shell, after sourcing paths.sh
    python3 -I zguard.py TOOL [--no-cwd] [PATH ...]             # the same, exit 2 with the message
    python3 -I zguard.py --check PATH                            # exit 0 outside, 1 inside (no message)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOW = 'ZAXIS_IN_MAIN'


def _paths():
    sys.path.insert(0, HERE)
    try:
        import zpaths
    finally:
        sys.path.pop(0)
    return zpaths.PATHS


def _real(path):
    return os.path.realpath(os.path.abspath(os.path.expanduser(path)))


def _under(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def main_checkout():
    """The main checkout's real path, or None when this host has none."""
    main = os.environ.get('ZAXIS_MAIN') or _paths().get('ZAXIS_MAIN')
    if not main or not os.path.isdir(os.path.join(main, '.git')):
        return None
    return _real(main)


def inside(path, cwd=False):
    """Whether PATH resolves inside the main checkout (not a lane's worktree; with cwd, not its tools/ either)."""
    main = main_checkout()
    if not main or not path:
        return False
    p = _real(path)
    if not _under(p, main):
        return False
    worktrees = os.environ.get('ZAXIS_WORKTREES') or _paths().get('ZAXIS_WORKTREES') or os.path.join(main, 'wt')
    if _under(p, _real(worktrees)):
        return False
    if cwd and any(_under(p, os.path.join(main, d)) for d in ('tools', 'games')):
        return False
    return True


def problems(outputs=(), cwd=True):
    """[(what, path)] of what resolves inside the main checkout: the working directory (cwd) and OUTPUTS."""
    if os.environ.get(ALLOW) == '1':
        return []
    found = []
    if cwd:
        try:
            here = os.getcwd()
        except OSError:
            here = None
        if here and inside(here, cwd=True):
            found.append(('the working directory', here))
    for p in outputs:
        if p and inside(p):
            found.append(('the path', p))
    return found


def message(tool, found):
    main = main_checkout()
    lines = ['%s: refused: %s %s is inside the main checkout %s.' % (tool, what, path, main) for what, path in found]
    lines.append('%s: only the orchestrator\'s merge and batch scripts work there (ZAXIS_IN_MAIN=1). Run from your '
                 'worktree (%s/NAME) or your scratch, with absolute paths; nothing was written.'
                 % (tool, os.path.join(main or '$ZAXIS_MAIN', 'wt')))
    return '\n'.join(lines)


def refuse(tool, outputs=(), cwd=True):
    """Stop (exit 2, the message on stderr) when the working directory or one of OUTPUTS is inside the main
    checkout, unless the caller is the orchestrator's merge and batch scripts (ZAXIS_IN_MAIN=1)."""
    found = problems(outputs, cwd)
    if found:
        sys.stderr.write(message(tool, found) + '\n')
        sys.stderr.flush()
        raise SystemExit(2)


def _cli(argv):
    if len(argv) == 2 and argv[0] == '--check':
        return 1 if inside(argv[1]) else 0
    if not argv:
        sys.stderr.write(__doc__)
        return 2
    tool, rest = argv[0], argv[1:]
    cwd = '--no-cwd' not in rest
    refuse(tool, [p for p in rest if p != '--no-cwd'], cwd=cwd)
    return 0


if __name__ == '__main__':
    sys.exit(_cli(sys.argv[1:]))
