"""The z-axis layout for Python tools: paths.env read as paths.sh reads it.

    import zpaths                  # with this folder on sys.path
    zpaths.AI_WORKSPACE, zpaths.get('MERGE_OUT')
    zpaths.folder(tree, 'runtime') # a z-axis tree's folder, old or new (checks/zlayout.py)

A name set in the environment wins; ZAXIS_MAIN comes from git (the main checkout of the z-axis
checkout this tooling sits in) before paths.env's fallback. ZAXIS_TOOLING is this folder, ZAXIS the
checkout it is in, ZAXIS_WORKSHOP the workshop's folder (workshop/ beside this tooling, else the main
checkout's; None in a public checkout).
"""
import os
import re
import subprocess

ZAXIS_TOOLING = os.environ.get('ZAXIS_TOOLING') or os.path.dirname(os.path.abspath(__file__))
ZAXIS = os.environ.get('ZAXIS') or os.path.dirname(ZAXIS_TOOLING)


def _main_checkout():
    try:
        common = subprocess.run(['git', '-C', ZAXIS, 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return common[:-len('/.git')] if common.endswith('/.git') else None


def _workshop():
    """The workshop's folder (merge, remote, status): $ZAXIS_WORKSHOP, this checkout's workshop/, else the main
    checkout's; None when there is none (a public checkout)."""
    if os.environ.get('ZAXIS_WORKSHOP'):
        return os.environ['ZAXIS_WORKSHOP']
    for root in (ZAXIS, os.environ.get('ZAXIS_MAIN') or _main_checkout()):
        if root and os.path.isdir(os.path.join(root, 'workshop', 'merge')):
            return os.path.join(root, 'workshop')
    return None


ZAXIS_WORKSHOP = _workshop()


def _user_home():
    """paths.env's $HOME: the user's own home (the password database), or ZAXIS_HOME, never the environment's HOME, so
    a tool run with a scratch HOME (a gate's runner, a test) still finds the data (tools28: BMX XXX did not boot)."""
    if os.environ.get('ZAXIS_HOME'):
        return os.environ['ZAXIS_HOME']
    try:
        import pwd
        return pwd.getpwuid(os.getuid()).pw_dir
    except (ImportError, KeyError):
        return os.path.expanduser('~')


USER_HOME = _user_home()

# Test runs make no sound (mute1, as paths.sh): every runner this process starts opens SDL's dummy audio device. A run
# that has to be heard says ZAXIS_AUDIO=on itself.
os.environ.setdefault('ZAXIS_AUDIO', 'off')


def load(path=os.path.join(ZAXIS_TOOLING, 'paths.env'),
         workshop=os.path.join(ZAXIS_WORKSHOP or ZAXIS_TOOLING, 'merge', 'paths.env')):
    """{NAME: path} for every name in paths.env, the environment first, then the workshop's layout (workshop/merge/paths.env,
    when there), then paths.env's defaults: the first value of a name wins, as paths.sh."""
    names = {'HOME': USER_HOME, 'ZAXIS_TOOLING': ZAXIS_TOOLING, 'ZAXIS': ZAXIS, 'ZAXIS_WORKSHOP': ZAXIS_WORKSHOP or ''}
    found = {'ZAXIS_MAIN': os.environ.get('ZAXIS_MAIN') or _main_checkout()}
    out = {}
    lines = open(workshop).read().splitlines() if os.path.isfile(workshop) else []
    for line in lines + open(path).read().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, value = line.split('=', 1)
        if name in out:
            continue
        value = os.environ.get(name) or found.get(name) or \
            re.sub(r'\$\{?(\w+)\}?', lambda m: names.get(m[1], os.environ.get(m[1], m[0])), value)
        names[name] = out[name] = value
    return out


def bind_dir(tree):
    """The bind tables a build of TREE uses: its own runtime/tools/bind (committed since tools39), else the shared
    GAME_BIND_LEGACY of a tree from before; None for no tree."""
    if not tree:
        return None
    own = os.path.join(tree, 'runtime', 'tools', 'bind')
    return own if os.path.isfile(os.path.join(own, 'dm2.tsv')) else PATHS.get('GAME_BIND_LEGACY')


def _tree():
    """The z-axis tree in use, as checks/ztree.py finds it: $ZAXIS_TREE, the checkout the working folder is in, the
    main checkout."""
    if os.environ.get('ZAXIS_TREE'):
        return os.environ['ZAXIS_TREE']
    here = os.getcwd()
    while True:
        if os.path.exists(os.path.join(here, '.git')) and (os.path.isdir(os.path.join(here, 'runtime', 'src', 'game'))
                                                          or os.path.isdir(os.path.join(here, 'ps2xRuntime', 'src', 'game'))):
            return here
        up = os.path.dirname(here)
        if up == here:
            return PATHS.get('ZAXIS_MAIN')
        here = up


PATHS = load()
# GAME_BIND_DIR: the environment's, else the tree in use's tables (bind_dir), never a shared folder for a tree that has
# its own: a commit builds and is read with its own tables.
PATHS['GAME_BIND_DIR'] = os.environ.get('GAME_BIND_DIR') or bind_dir(_tree()) or PATHS.get('GAME_BIND_LEGACY')
globals().update(PATHS)
# The workshop's own names (workshop/merge/paths.env), in the environment of every process this one starts, unless already set:
# a tool of another tooling (an older one, which reads paths.env alone) then finds the same folders (paths.sh exports
# every name already).
_WORKSHOP = os.path.join(ZAXIS_WORKSHOP or ZAXIS_TOOLING, 'merge', 'paths.env')
if os.path.isfile(_WORKSHOP):
    for _line in open(_WORKSHOP).read().splitlines():
        _name = _line.strip().split('=', 1)[0]
        if _line.strip() and not _line.lstrip().startswith('#') and _name in PATHS:
            os.environ.setdefault(_name, PATHS[_name])
# The tooling in use, for every process this one starts: a z-axis tool (runtime/tools/zaxis_paths.py, which loads the
# main checkout's tooling/zpaths.py) and a script of this tooling then read this paths.env, so a lane's tooling copy
# runs its own workspaces' scripts (tools34), as paths.sh's export does for the shell.
os.environ.setdefault('ZAXIS_TOOLING', ZAXIS_TOOLING)
if ZAXIS_WORKSHOP:
    os.environ.setdefault('ZAXIS_WORKSHOP', ZAXIS_WORKSHOP)


def _link_data(tooling=ZAXIS_TOOLING):
    """The data links (data-links.txt) of the tooling in use, made when its first is missing and the data is here, as
    paths.sh does: a lane's tooling copy (a git clone has none) reads and writes the same data as main's. Only missing
    links to data that exists (setup.sh does the rest)."""
    marker = os.path.join(tooling, 'ai', 'recomp', 'generated')
    data = {'ai': PATHS.get('AI_DATA'), 'dm2': PATHS.get('DM2_DATA')}
    if os.path.islink(marker) or not data['ai'] or not os.path.exists(os.path.join(data['ai'], 'recomp', 'generated')):
        return
    try:
        lines = open(os.path.join(tooling, 'data-links.txt')).read().splitlines()
    except OSError:
        return
    for line in lines:
        parts = line.split(None, 1)
        if len(parts) != 2 or parts[0] not in data or not data[parts[0]]:
            continue
        link, target = os.path.join(tooling, *parts), os.path.join(data[parts[0]], parts[1])
        if not os.path.isdir(os.path.join(tooling, parts[0], 'tools')) or os.path.lexists(link) \
                or not os.path.exists(target):
            continue
        try:
            os.makedirs(os.path.dirname(link), exist_ok=True)
            os.symlink(target, link)
        except OSError:     # (another process made it first, or a host without symlinks)
            pass


_link_data()


def generated(folder):
    """A game's generated folder as this tooling's builds use it: its real path with -GENERATED_SET after it
    (paths.env), or the folder itself when the set is empty (zgen in paths.sh)."""
    tag = os.environ.get('GENERATED_SET', PATHS.get('GENERATED_SET', ''))
    return os.path.realpath(folder) + '-' + tag if tag else folder


def get(name):
    return PATHS[name]


def _zlayout():
    import importlib.util
    spec = importlib.util.spec_from_file_location('zlayout', os.path.join(ZAXIS_TOOLING, 'checks', 'zlayout.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# A z-axis tree's top-level folders, old or new (checks/zlayout.py): zpaths.folder(tree, 'runtime') is 'runtime',
# or 'ps2xRuntime' in a tree from before tools17; zpaths.tree_path(tree, 'runtime', 'src', 'game').
zlayout = _zlayout()
folder = zlayout.folder
tree_path = zlayout.path
