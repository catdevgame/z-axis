"""The top-level folders of a z-axis tree: runtime, tests, iop and docs, named in one place for the tools that read
a tree (a lane's branch, a commit a worker builds). No side effects on import; paths.sh's zdir is the shell's.

    import zlayout
    zlayout.folder(tree, 'runtime')        # 'runtime'
    zlayout.path(tree, 'runtime', 'src', 'game')

A tree from before tools17 (its runtime, tests, ps2xIOP and recomp folders) is not read since naming1.
"""
import os

# kind: the folder's names (one: the tuple stays for the callers that go through every name)
FOLDERS = {'runtime': ('runtime',), 'tests': ('tests',), 'iop': ('iop',), 'docs': ('docs',)}


def folder(tree, kind):
    return FOLDERS[kind][0]


def path(tree, kind, *rest):
    return os.path.join(tree, folder(tree, kind), *rest)


def folder_at(repo, sha, kind):
    """folder() for a commit, not a checkout."""
    return FOLDERS[kind][0]
