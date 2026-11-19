"""The top-level folders of a z-axis tree, old or new: tools17 renamed ps2xRuntime to runtime, ps2xTest to tests,
ps2xIOP to iop and recomp (the docs) to docs. A tool that reads a tree (a lane's branch from before, a commit a
worker builds) asks here which name that tree has. No side effects on import; paths.sh's zdir is the shell's.

    import zlayout
    zlayout.folder(tree, 'runtime')        # 'runtime', or 'ps2xRuntime' in a tree from before
    zlayout.path(tree, 'runtime', 'src', 'game')

A folder counts when it holds what that folder always has (MARKER): a checkout that moved between commits from
before and after the rename keeps the other name's ignored files (a runtime/tools/__pycache__ left in a slot that
then checked out an old commit), so the folder's existence alone does not tell. A tree with neither gets the new name. The build dirs kept the old names (<build>/ps2xRuntime/ps2EntryRunner,
<build>/ps2xTest/ps2x_tests) in both: they are not here.
"""
import os

# kind: (the name now, the name before tools17)
FOLDERS = {'runtime': ('runtime', 'ps2xRuntime'), 'tests': ('tests', 'ps2xTest'), 'iop': ('iop', 'ps2xIOP'),
           'docs': ('docs', 'recomp')}
# kind: a file every commit's folder of that kind has, old name or new
MARKER = {'runtime': 'CMakeLists.txt', 'tests': 'CMakeLists.txt', 'iop': 'CMakeLists.txt', 'docs': 'READABLE.md'}


def folder(tree, kind):
    new, old = FOLDERS[kind]
    has = lambda name: os.path.isfile(os.path.join(tree, name, MARKER[kind]))
    if has(new):
        return new
    if has(old):
        return old
    if not os.path.isdir(os.path.join(tree, new)) and os.path.isdir(os.path.join(tree, old)):
        return old
    return new


def path(tree, kind, *rest):
    return os.path.join(tree, folder(tree, kind), *rest)


def folder_at(repo, sha, kind):
    """folder() for a commit, not a checkout: the name that commit's tree has (git ls-tree)."""
    import subprocess
    new, old = FOLDERS[kind]
    names = subprocess.run(['git', '-C', repo, 'ls-tree', '--name-only', sha], capture_output=True, text=True).stdout.split()
    return old if new not in names and old in names else new
