#!/usr/bin/env python3
"""A game's recording corpus (DM2's, or BMX XXX's with --game bmx): the counterpart of AI's
readable_corpus.py (docs/READABLE.md, "How a function is proven"), over the pad script routes of
dm2routes.py or bmxroutes.py.

    dm2corpus.py cov ROUTE ...     a ZX_FNPROF profile of each route: <store>/cov/ROUTE.csv
    dm2corpus.py record ROUTE ...  each route recorded into <store>/<game>-v1/ROUTE/
    dm2corpus.py ship ROUTE ...    recorded folders to the Linux worker (reccorpus/<game>-v1), checked by size and
                                   file count, then dropped from the Mac
    dm2corpus.py status            routes done, sizes, here and on the Linux worker

Every run: readable off, unpaced, null GS, VU1 skipped, a scratch HOME, card and support folder, a
time limit and a memory limit, stopped by PID at the route's end (a dump at that script time is
the mark, deleted). Each function's caps are readable_corpus.py's: 200 calls, 8 a vsync; 50 and 1 for
functions with 10 or more call sites.

Options: --game dm2|bmx, --build DIR (the collection build, default RUNNER_BUILD), --store DIR (default
$ZAXIS_SCRATCH/reccorpus), --jobs N (at most 2), --max-secs S (default 1500), --max-rss-mb M
(default 8000), --gs (the renderers: the route draws with the CPU GS).
"""
import argparse
import importlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.dirname(ROOT))
import zpaths  # noqa: E402  (tooling/paths.env: the scratch folder, the Linux worker)
SCRATCH = zpaths.ZAXIS_SCRATCH
# A game's own: its routes, ELF, namespaced generated code (ELF and GENERATED in the environment name another place: the Linux worker's).
GAMES = {'dm2': {'routes': 'dm2routes', 'elf': os.path.join(ROOT, 'recomp', 'disc-root', 'SLUS_201.59'),
                 'generated': os.path.join(ROOT, 'recomp', 'generated-ns')},
         'bmx': {'routes': 'bmxroutes', 'elf': os.path.join(SCRATCH, 'zbind1', 'bmx', 'disc', 'SLUS_204.15'),
                 'generated': os.path.join(zpaths.PATHS['BMX_RECOMP'], 'generated-ns')}}
GAME = 'dm2'
routes = None
ELF = GENERATED = VERSION = SLATE_STORE = None
SLATE = zpaths.PATHS.get('LINUX_WORKER_HOST') or 'linux-worker'   # the Linux worker's ssh host (paths.env's names)
CAPS_LIGHT, CAPS_HEAVY, HEAVY_SITES = (200, 8), (50, 1), 10


def choose_game(name):
    global GAME, routes, ELF, GENERATED, VERSION, SLATE_STORE
    GAME = name
    routes = importlib.import_module(GAMES[name]['routes'])
    ELF = os.environ.get('ELF', GAMES[name]['elf'])
    GENERATED = os.environ.get('GENERATED', GAMES[name]['generated'])
    VERSION = name + '-v1'
    SLATE_STORE = (zpaths.PATHS.get('LINUX_WORKER_ROOT') or '/srv/z-axis-worker') + '/reccorpus/' + VERSION


def rss_mb(pid):
    p = subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)], capture_output=True, text=True)
    return int(p.stdout.strip() or 0) // 1024


def function_list(build, out):
    """Every registered function of the game, with its caps."""
    p = subprocess.run([os.path.join(build, 'runtime', 'zx-fncheck'), '--list'], capture_output=True, text=True)
    addresses = [int(m.group(1), 16) for m in re.finditer(r'^%s:0x([0-9a-f]+)\s' % GAME, p.stdout, re.M)]
    files = {}
    for name in os.listdir(GENERATED):
        m = re.search(r'_0x([0-9a-f]+)\.cpp$', name)
        if m:
            files[int(m.group(1), 16)] = name
    with open(out, 'w') as f:
        for a in addresses:
            sites = 0
            if a in files:
                sites = len(re.findall(r'// 0x[0-9a-f]+: 0x[0-9a-f]+\s+jalr?\s', open(os.path.join(GENERATED, files[a]), errors='replace').read()))
            caps = CAPS_HEAVY if sites >= HEAVY_SITES else CAPS_LIGHT
            f.write('0x%08x %d %d\n' % ((a,) + caps))
    return len(addresses)


class Run:
    def __init__(self, args, route, kind):
        self.args, self.route, self.kind = args, route, kind

    def script_arg(self, work, script):
        """A played route's script is long: as a file (ZX_PAD_SCRIPT=@file) past what a variable holds."""
        if len(script) < 60000:
            return script
        path = os.path.join(work, 'script.txt')
        with open(path, 'w') as f:
            f.write(script)
        return '@' + path

    def go(self):
        a = self.args
        work = os.path.join(a.store, 'work', '%s-%s-%s' % (GAME, self.kind, self.route))
        shutil.rmtree(work, ignore_errors=True)
        for sub in ('home', 'cwd', 'app', 'shots', 'mc0', 'mc1', 'dump'):
            os.makedirs(os.path.join(work, sub))
        script = routes.script_of(self.route)
        end = max(int(i.split('v')[0]) for i in script.split(',')) + 300
        env = dict(os.environ)
        env.update({'ZX_UNPACED': '1', 'ZX_GS_NULL': '1', 'ZX_VU1_SKIP': '1', 'ZX_READABLE': 'off',
                    'ZX_DEV_UNLOCK_ALL': '1', 'ZX_DEBUG_UI': '0', 'ZX_SAVES': 'card', 'ZX_MC_FD_BASE0': '1', 'ZX_GS_BACKEND': 'cpu',
                    'HOME': os.path.join(work, 'home'), 'ZX_APP_SUPPORT_DIR': os.path.join(work, 'app'),
                    'ZX_APP_LOG_DIR': os.path.join(work, 'app'), 'ZX_SCREENSHOT_DIR': os.path.join(work, 'shots'),
                    'ZX_MC_ROOT': os.path.join(work, 'mc0'), 'ZX_PAD_SCRIPT_CLOCK': 'vsync',
                    'ZX_PAD_SCRIPT': self.script_arg(work, script), 'ZX_DUMP_WHEN': 'end:script>=%d' % end,
                    'ZX_DUMP_PREFIX': os.path.join(work, 'dump', 'd'),
                    'ZX_STATE_LOG': '0x%X,0x%X,0x%X,0x%X' % (routes.MODE, routes.SESSION, routes.LEVEL_FRAME, routes.RIDER_TABLE)})
        if len(script) > 100000:     # a played route: more than an environment string can hold
            path = os.path.join(work, 'script.txt')
            with open(path, 'w') as f:
                f.write(script)
            env['ZX_PAD_SCRIPT'] = '@' + path
        for k, v in routes.extra_env(self.route).items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = v
        if a.gs:
            env.pop('ZX_GS_NULL', None)
            env.pop('ZX_VU1_SKIP', None)
        if self.kind == 'cov':
            os.makedirs(os.path.join(a.store, 'cov'), exist_ok=True)
            result = os.path.join(a.store, 'cov', self.route + '.csv')
            env['ZX_FNPROF'] = result
        else:
            result = os.path.join(a.store, VERSION, self.route)
            shutil.rmtree(result, ignore_errors=True)
            os.makedirs(result)
            listing = os.path.join(work, 'functions.txt')
            function_list(a.build, listing)
            env.update({'ZX_FNREC': '@' + listing, 'ZX_FNREC_DIR': result, 'ZX_FNREC_ATOMIC': '1',
                        'ZX_FNREC_MAX': '1000', 'ZX_FNREC_PER_VSYNC': '8',
                        # (proof1) no call begun past the route's end, the end mark's guest moment: what the run
                        # does while it is being stopped depends on the host's timing, so a recording made on
                        # another host would differ in those last calls
                        'ZX_FNREC_UNTIL_SCRIPT': str(end)})
        runner = os.path.join(a.build, 'runtime', 'zx-runner')
        log = os.path.join(work, 'run.log')
        t0 = time.time()
        with open(log, 'wb') as f:
            p = subprocess.Popen([runner, ELF], env=env, cwd=os.path.join(work, 'cwd'), stdout=f, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        print('%s %s: pid %d, to script time %d' % (self.kind, self.route, p.pid, end), flush=True)
        why, peak = 'time limit', 0
        mark = os.path.join(work, 'dump', 'd-end.ram')
        while p.poll() is None and time.time() - t0 < a.max_secs:
            peak = max(peak, rss_mb(p.pid))
            if peak > a.max_rss_mb:
                why = 'memory limit'
                break
            if os.path.exists(mark):
                why = 'end of route'
                time.sleep(1)
                break
            time.sleep(2)
        rc = p.poll()
        if rc is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        else:
            why = 'exited %s' % rc
        if os.path.exists(mark):
            os.remove(mark)
        tail = open(log, 'rb').read()[-65536:].decode('latin-1')
        ticks = re.findall(r'\btick=(\d+)', tail)
        out = {'route': self.route, 'kind': self.kind, 'why': why, 'seconds': round(time.time() - t0), 'peak_mb': peak,
               'tick': int(ticks[-1]) if ticks else None, 'when': time.strftime('%Y-%m-%d %H:%M')}
        print(json.dumps(out), flush=True)
        with open(os.path.join(a.store, GAME + '-runs.jsonl'), 'a') as f:
            f.write(json.dumps(out) + '\n')


def many(args, kind):
    todo = list(args.routes)
    for r in todo:
        if r not in routes.ROUTES:
            raise SystemExit('unknown route ' + r)
    lock = threading.Lock()

    def worker():
        while True:
            with lock:
                if not todo:
                    return
                r = todo.pop(0)
            Run(args, r, kind).go()

    threads = [threading.Thread(target=worker) for _ in range(min(2, args.jobs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def folder_stat(path):
    n, size = 0, 0
    for name in os.listdir(path):
        if name.endswith('.fnrec'):
            n += 1
            size += os.path.getsize(os.path.join(path, name))
    return n, size


def replay_here(route, root):
    """With DM2_SHIP_REPLAY's file (lines: the tree, the zx-fncheck, the replay folder, the --only list) a semantic replay of
    the route before it leaves the Mac, with its memory capped."""
    plan = os.path.expanduser(os.environ.get('DM2_SHIP_REPLAY', os.path.join(SCRATCH, 'dm2corpus', 'replay-before-ship.txt')))
    if not os.path.exists(plan):
        return
    tree, fncheck, out, only = open(plan).read().split()
    link = os.path.join(out, 'in-' + route)
    os.makedirs(link, exist_ok=True)
    if not os.path.exists(os.path.join(link, route)):
        os.symlink(os.path.join(root, route), os.path.join(link, route))
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'checks', 'fncheck_corpus.py')
    subprocess.run([sys.executable, '-I', tool, 'run', '--fncheck', fncheck,
                    '--build', os.path.dirname(os.path.dirname(fncheck)), '--out', os.path.join(out, route), '--corpus', link, '--jobs', '2',
                    '--max-mem-gb', '4', '--semantic', '--no-cache', '--only', open(only).read().strip().replace('\n', ',')],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, ZAXIS_TREE=tree))


def ship(args):
    root = os.path.join(args.store, VERSION)
    for r in args.routes:
        local = os.path.join(root, r)
        n, size = folder_stat(local)
        replay_here(r, root)
        subprocess.run(['ssh', SLATE, 'mkdir -p %s/%s' % (SLATE_STORE, r)], check=True)
        subprocess.run(['nice', '-n', '19', 'rsync', '-a', '--partial', '--exclude=._*', '--rsync-path=nice -n 19 ionice -c3 rsync',
                        local + '/', '%s:%s/%s/' % (SLATE, SLATE_STORE, r)], check=True)
        q = subprocess.run(['ssh', SLATE, "cd %s/%s && find . -name '*.fnrec' | wc -l && find . -name '*.fnrec' -printf '%%s\\n' | "
                            "awk '{s+=$1} END {print s+0}'" % (SLATE_STORE, r)], capture_output=True, text=True, check=True)
        there = [int(x) for x in q.stdout.split()]
        if there != [n, size]:
            raise SystemExit('%s: the Linux worker has %s, the Mac %s: kept' % (r, there, [n, size]))
        shutil.rmtree(local)
        with open(os.path.join(root, 'shipped.tsv'), 'a') as f:
            f.write('%s\t%d\t%d\n' % (r, n, size))
        print('%s: %d files, %.2f GB on the Linux worker, dropped here' % (r, n, size / 1e9), flush=True)


def status(args):
    root = os.path.join(args.store, VERSION)
    here = sum(folder_stat(os.path.join(root, r))[1] for r in os.listdir(root) if os.path.isdir(os.path.join(root, r))) if os.path.isdir(root) else 0
    shipped = [line.split('\t') for line in open(os.path.join(root, 'shipped.tsv'))] if os.path.exists(os.path.join(root, 'shipped.tsv')) else []
    print('here %.2f GB; on the Linux worker %d routes, %.2f GB' % (here / 1e9, len(shipped), sum(int(x[2]) for x in shipped) / 1e9))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['cov', 'record', 'ship', 'status'])
    ap.add_argument('routes', nargs='*')
    ap.add_argument('--game', default='dm2', choices=sorted(GAMES))
    ap.add_argument('--build', default=os.environ.get('RUNNER_BUILD'))
    ap.add_argument('--store', default=os.path.join(SCRATCH, 'reccorpus'))
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--max-secs', type=int, default=1500)
    ap.add_argument('--max-rss-mb', type=int, default=8000)
    ap.add_argument('--gs', action='store_true')
    a = ap.parse_args()
    choose_game(a.game)
    if a.cmd in ('cov', 'record'):
        many(a, a.cmd)
    elif a.cmd == 'ship':
        ship(a)
    else:
        status(a)


if __name__ == '__main__':
    main()
