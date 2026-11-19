"""Where the tools here find what is outside this repository: the game workspaces (tools and their
data), the work database's tool, the data folder. The paths are the tooling's (tooling/paths.env,
read by tooling/zpaths.py); a variable of the same name set in the environment wins. The tooling is
looked for in this checkout, then in its main checkout (a checkout from before the tooling was folded
in may have left its submodule empty). With no tooling, get() returns the environment's value or None,
so only a tool that needs the default (and was given no path) stops.
"""
import importlib.util
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_paths = None


def _main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return common[:-len('/.git')] if common.endswith('/.git') else None


def _load():
    global _paths
    if _paths is None:
        _paths = {}
        for root in (REPO, _main_checkout()):
            module = os.path.join(root or '', 'tooling', 'zpaths.py')
            if root and os.path.isfile(module):
                spec = importlib.util.spec_from_file_location('zpaths', module)
                zpaths = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(zpaths)
                _paths = dict(zpaths.PATHS)
                break
    return _paths


def get(name, required=True):
    """The path NAME (paths.env: AI_WORKSPACE, DM2_WORKSPACE, WORKDB_PY, ZAXIS_MAIN, ...)."""
    value = os.environ.get(name) or _load().get(name)
    if value is None and required:
        raise SystemExit('%s: no %s: set it (tooling/paths.env)' % (
            os.path.basename(sys.argv[0] or 'zaxis_paths'), name))
    return value
