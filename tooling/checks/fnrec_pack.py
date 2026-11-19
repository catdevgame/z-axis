#!/usr/bin/env python3
"""Compresses a recording corpus in place, folder by folder (the compressed form: runtime/src/game/ee/fnrec_file.h,
fnrec_io.py pack). Every plain .fnrec of a route folder is compressed into a temporary file beside it
(._packing.<name>, which no corpus tool lists and no sync sends), decompressed again and compared with the
original byte for byte, given the original's modification time, and only then put in its place (one rename: a
reader sees the plain file or the compressed one, never half of either). The file's name, its plain size (the
head names it) and its mtime stay, so the verdict cache keys it as before (fncheck_corpus.py file_id) and the
syncs (corpus-sync.sh, wk.py manifest) see the same file.

    fnrec_pack.py CORPUS [CORPUS ...] [--jobs N] [--dry-run] [--folders A,B] [--level 9]

CORPUS is a corpus folder (its route folders are converted one at a time) or a route folder. A folder that a
running job names (a ps2xFnCheck, fncheck_corpus.py, the recorder, a sync's wk.py: by its command line naming the
folder or its corpus) is left for later in the pass, and the pass ends with the folders still busy listed. Safe to
stop at any point and run again: compressed files are skipped, leftover temporaries removed. Prints each folder's
size before and after and the corpus' totals. The ps2xFnCheck that replays a converted corpus must read the
compressed form (rzstd1 on): an older one stops at a converted file ("not a fncheck recording").
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnrec_io  # noqa: E402

WINDOWS = sys.platform == 'win32'
TEMP_PREFIX = '._packing.'
FNREC = re.compile(r'^(?:[a-z0-9]+-)?[0-9a-f]{8}\.fnrec$')
# The processes that read or write a corpus (by name, lower case, without .exe).
JOB_NAMES = ('ps2xfncheck', 'ps2entryrunner')
JOB_SCRIPTS = ('fncheck_corpus.py', 'wk.py', 'dm2corpus.py', 'readable_corpus.py', 'replay-gate', 'corpus-sync')
try:   # fncheck_corpus.py's corpus without --corpus: $ZAXIS_SCRATCH/reccorpus/v5 (the tooling's paths.env)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
DEFAULT_CORPUS = os.path.join(SCRATCH_ROOT, 'reccorpus', 'v5')


def log(msg):
    print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def lower_priority():
    if WINDOWS:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL
    else:
        os.nice(19)


def command_lines():
    """Every other process's command line, for the job processes and scripts above."""
    out = []
    if WINDOWS:
        ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne %d } | "
              "Select-Object Name,CommandLine | ConvertTo-Json -Compress" % os.getpid())
        p = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True, timeout=120)
        rows = json.loads(p.stdout or '[]')
        rows = rows if isinstance(rows, list) else [rows]
        out = [(r.get('Name') or '', r.get('CommandLine') or '') for r in rows]
    elif os.path.isdir('/proc'):
        for pid in os.listdir('/proc'):
            if not pid.isdigit() or int(pid) == os.getpid():
                continue
            try:
                raw = open('/proc/%s/cmdline' % pid, 'rb').read()
            except OSError:
                continue
            args = raw.decode('utf-8', 'replace').split('\0')
            out.append((os.path.basename(args[0]) if args else '', ' '.join(args)))
    else:
        p = subprocess.run(['ps', '-axww', '-o', 'pid=,command='], capture_output=True, text=True)
        for line in p.stdout.splitlines():
            pid, _, cmd = line.strip().partition(' ')
            if pid.isdigit() and int(pid) != os.getpid():
                out.append((os.path.basename(cmd.split(' ')[0]), cmd))
    jobs = []
    for name, cmd in out:
        n = name.lower()
        n = n[:-4] if n.endswith('.exe') else n
        if n in JOB_NAMES or any(s in cmd for s in JOB_SCRIPTS):
            jobs.append(cmd)
    return jobs


def norm(path):
    p = os.path.abspath(path).replace('\\', '/').rstrip('/')
    return p.lower() if WINDOWS else p


def busy_folders(corpus, jobs):
    """(the whole corpus busy, {route folder names a job names})."""
    root = norm(corpus)
    whole, named = False, set()
    for cmd in jobs:
        c = cmd.replace('\\', '/')
        c = c.lower() if WINDOWS else c
        if 'fncheck_corpus.py' in c and '--corpus' not in c and norm(DEFAULT_CORPUS) == root:
            whole = True
        at = c.find(root)
        while at >= 0:
            rest = c[at + len(root):]
            if rest[:1] == '/' and len(rest) > 1:
                named.add(re.split(r'[/"\' ]', rest[1:])[0])
            elif rest[:1] in ('', ' ', '"', "'", ','):
                whole = True
            at = c.find(root, at + 1)
    return whole, named


def same_bytes(plain, packed):
    """True if packed decompresses to exactly plain's bytes."""
    with open(plain, 'rb') as a, fnrec_io.open_plain(packed) as b:
        while True:
            x = a.read(8 << 20)
            y = b.read(len(x)) if x else b.read(1)
            if x != y:
                return False
            if not x:
                return True


def pack_one(path, level):
    """(plain bytes, packed bytes, what happened): 'packed', 'already', or an error."""
    st = os.stat(path)
    head = fnrec_io.head(path)
    if head['packed']:
        return head['plain_bytes'] or st.st_size, st.st_size, 'already'
    folder, name = os.path.split(path)
    temp = os.path.join(folder, TEMP_PREFIX + name)
    try:
        plain, packed = fnrec_io.pack(path, temp, level=level)
        if plain != st.st_size or not same_bytes(path, temp):
            raise IOError('the compressed copy does not give the file back')
        os.utime(temp, ns=(st.st_atime_ns, st.st_mtime_ns))
        now = os.stat(path)
        if (now.st_size, now.st_mtime_ns) != (st.st_size, st.st_mtime_ns):
            raise IOError('changed while it was compressed')
        os.replace(temp, path)
        return plain, packed, 'packed'
    except (OSError, ValueError) as e:
        try:
            os.remove(temp)
        except OSError:
            pass
        return st.st_size, st.st_size, 'error: %s' % e


def route_folders(corpus):
    """[(route name, folder)]: the corpus' route folders, or the folder itself when it holds recordings."""
    names = os.listdir(corpus)
    if any(FNREC.match(n) for n in names):
        return [(os.path.basename(corpus.rstrip('/\\')), corpus)]
    return [(n, os.path.join(corpus, n)) for n in sorted(names)
            if os.path.isdir(os.path.join(corpus, n)) and n not in ('work', '.incoming')]


def convert_folder(folder, a, pool):
    for n in os.listdir(folder):
        if n.startswith(TEMP_PREFIX):
            os.remove(os.path.join(folder, n))   # (a stopped run's)
    files = [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if FNREC.match(n)]
    if a.dry_run:
        rows = [(fnrec_io.key_size(p), os.path.getsize(p), 'already' if fnrec_io.head(p)['packed'] else 'plain')
                for p in files]
    else:
        files.sort(key=lambda p: -os.path.getsize(p))   # (the big ones first: the threads end together)
        rows = list(pool.map(lambda p: pack_one(p, a.level), files))
    before = sum(r[0] for r in rows)   # (the plain bytes)
    after = sum(os.path.getsize(p) for p in files)
    errors = [(p, r[2]) for p, r in zip(files, rows) if r[2].startswith('error')]
    return {'files': len(files), 'packed': sum(1 for r in rows if r[2] == 'packed'),
            'already': sum(1 for r in rows if r[2] == 'already'), 'plain': sum(1 for r in rows if r[2] == 'plain'),
            'before': before, 'after': after, 'errors': errors}


def disk_bytes(folder):
    return sum(os.path.getsize(os.path.join(folder, n)) for n in os.listdir(folder)
               if os.path.isfile(os.path.join(folder, n)))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('corpus', nargs='+')
    ap.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 2) // 2)))
    ap.add_argument('--level', type=int, default=fnrec_io.PACK_LEVEL)
    ap.add_argument('--folders', help='only these route folders (commas)')
    ap.add_argument('--dry-run', action='store_true', help='count what would be compressed, change nothing')
    ap.add_argument('--retry-busy', type=float, default=0, metavar='MINUTES',
                    help='wait up to this long for busy folders at the end of the pass (default: list them, stop)')
    a = ap.parse_args(argv)
    if fnrec_io.library() is None and not a.dry_run:
        sys.exit('fnrec_pack: no zstd library (Python 3.14 compression.zstd, or the zstandard module)')
    lower_priority()
    only = set(a.folders.split(',')) if a.folders else None
    log('fnrec_pack: zstd %s via %s, level %d, %d threads' % (
        (fnrec_io.library() or ('-', '-'))[1], (fnrec_io.library() or ('-', '-'))[0], a.level, a.jobs))
    status = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for corpus in a.corpus:
            corpus = os.path.abspath(corpus)
            t0 = time.time()
            todo = [(r, f) for r, f in route_folders(corpus) if only is None or r in only]
            disk0 = sum(disk_bytes(f) for _, f in todo)
            total = {'files': 0, 'packed': 0, 'already': 0, 'plain': 0, 'before': 0, 'after': 0}
            deadline = time.time() + a.retry_busy * 60
            while todo:
                waiting = []
                jobs = command_lines()
                for route, folder in todo:
                    whole, named = busy_folders(corpus, jobs)
                    if whole or route in named or busy_folders(folder, jobs)[0]:
                        waiting.append((route, folder))
                        continue
                    t = time.time()
                    r = convert_folder(folder, a, pool)
                    for k in total:
                        total[k] += r[k]
                    if a.dry_run:
                        log('%s: %d files: %d plain to compress, %d compressed already; %.1f MB of plain recordings' % (
                            route, r['files'], r['plain'], r['already'], r['before'] / 1e6))
                    else:
                        log('%s: %d files (%d compressed now, %d before), %.1f MB -> %.1f MB (%.1fx) in %.0f s%s' % (
                            route, r['files'], r['packed'], r['already'], r['before'] / 1e6, r['after'] / 1e6,
                            r['before'] / max(1, r['after']), time.time() - t,
                            '; %d errors: %s' % (len(r['errors']), r['errors'][:3]) if r['errors'] else ''))
                    if r['errors']:
                        status = 1
                    jobs = command_lines()
                if waiting and time.time() < deadline:
                    log('%d folders a running job reads: again in a minute' % len(waiting))
                    time.sleep(60)
                elif waiting:
                    log('left for a later run (a running job reads them): %s' % ', '.join(r for r, _ in waiting))
                    status = status or 3
                    waiting = []
                todo = waiting
            disk1 = sum(disk_bytes(f) for r, f in route_folders(corpus) if only is None or r in only)
            if a.dry_run:
                log('%s: dry run: %d recordings in %d folders, %d plain to compress, %d compressed already, %.2f GB on '
                    'disk' % (corpus, total['files'], len(route_folders(corpus)), total['plain'], total['already'],
                              disk0 / 1e9))
                continue
            log('%s: %d recordings, %.2f GB of plain recordings in %.2f GB; the folders %.2f GB before, %.2f GB after '
                '(%.1fx) in %.0f s' % (corpus, total['files'], total['before'] / 1e9, total['after'] / 1e9, disk0 / 1e9,
                                       disk1 / 1e9, disk0 / max(1, disk1), time.time() - t0))
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
