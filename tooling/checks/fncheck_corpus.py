#!/usr/bin/env python3
"""ps2xFnCheck over the whole recording corpus (docs/READABLE.md, "How a
function is proven"): every recording of every route, verdicts,
coverage summed per function, and the work database's result lines.

    fncheck_corpus.py run --fncheck BUILD/ps2xRuntime/ps2xFnCheck --out DIR
                          [--corpus DIR ...] [--jobs N|auto] [--only ADDR,...|@FILE] [--chunk N]
                          [--incremental] [--extras-changed] [--extras-for ADDR,...|@FILE]
                          [--verify SHARE] [--cache DIR] [--no-cache] [--build DIR] [--max-mem-gb GB]
                          [--semantic] [--frame-index DIR]
                          (each ps2xFnCheck in a process group of its own, killed whole on a timeout,
                          Ctrl-C or above --max-mem-gb, default 4: then its file is a fault with phase
                          `memory` and the chunk goes on; --jobs auto counts free memory at the cap
                          per job and the cores the load leaves; on Windows a job object instead of the
                          group)
    fncheck_corpus.py same DIR_A DIR_B [--ignore checker]   (the lines themselves, as sets)
    fncheck_corpus.py cache-import DIR --build BUILD [--map-corpus FROM=TO] | --digests FILE
                          (a run's lines into the cache; --map-corpus: another host's run of the same commit,
                          its files named as this host's copy of the corpus has them)
    fncheck_corpus.py digests --build BUILD --out FILE      (a build's digests, for a later import)
    fncheck_corpus.py cache-gc [--days 14]
    fncheck_corpus.py prune-plan DIR [--out PLAN.json]      (a dry run of pruning by file)
    fncheck_corpus.py verdicts DIR                  (per function: pass, fail, none)
    fncheck_corpus.py compare DIR_A DIR_B [--ignore-new] [--no-worse] [--semantic] [--replayed]   (verdicts per recording file;
                                    --replayed: only the files B has, A's others not run: B an --only run)
    fncheck_corpus.py coverage DIR [DIR ...] [--status Verified] [--worst 20] [--json]
                          [--covered-out FILE] [--route-prefix P]
    fncheck_corpus.py emit DIR --run NAME [--results DIR] [--registry FILE]
    fncheck_corpus.py generate --fncheck BUILD/ps2xRuntime/ps2xFnCheck --out DIR FN[:N] ...
    fncheck_corpus.py markers DIR [DIR ...]          (--markers lines summed over the files)
    fncheck_corpus.py safepoints DIR [DIR ...] [--all] (--safepoints lines per function)
    fncheck_corpus.py saves DIR [DIR ...] [--only ADDR,...|@FILE] [--held]
                          (per function: calls whose callee gets another callee-saved
                          register value than the original held: missing ee::calleeSaves)

`run` replays every *.fnrec under the corpus' route folders (default
$ZAXIS_SCRATCH/reccorpus/v5; more folders with --corpus, a DM2
recording folder among them) with `--cycles`, one process per chunk of a
route's files, and keeps every ps2xFnCheck line for tools ([fncheck:result],
[fncheck:coverage], [fncheck:fault], [fncheck:build], [fncheck:markers]) in DIR/lines.jsonl, one
JSON object per line with the route. A memory fault ends a process: the
file is recorded as a fault and the files after it go on in another.

The verdict cache (round corpusfast): `run` keeps every file's lines in an
SQLite cache (default $ZAXIS_SCRATCH/reccorpus/fncache), keyed by the
recording (path, size, mtime), the function's readable source and every
readable definition it can run (across files: through a project header both
files include that declares it; game::call's targets are answered from the
recording and not followed), the rest of those files and their headers (the
build's dependency log), the traced original (traced_<hex>.cpp and its blocks
file), the checker without the readable functions (fncheck_main, the runtime
and ee/*.cpp, every header the traced originals include and the objects of its
link that a replay runs (not the header units, built first and linked into
nothing, nor GS, audio, IOP or SDL code), the compile commands, the compiler)
and the flags; per part of a file's lines (core:
result, coverage, fault; safepoints; markers). `--incremental` replays only
the files with a part not cached and copies the rest (the result lines with
this binary's checker= digest), so lines.jsonl is what a full run writes
(sorted by route, file and kind). `--verify` (default 0.01) replays that share
of the cached files too and fails the run if one differs (a missed input).
Another game's recordings (dm2-, bmx-) are keyed in that game's instance
(ps2_game_readable_<game>: the shared engine sources and the game's folder,
compiled with its symbol headers, which the bind tables make): the same
closure over those objects, the headers' chunks the names reach (a symbol's
address, level and status), and the registrations as the binary lists them
(--list: the game's address, Planned or a body). The base also has the tables
ps2xFnCheck is built with (fncheck_build: stack arguments, frames) and the
recompiled code's headers. An address registered more than once keys the
registrations' order (readable::find runs the first), and a run with one of the
checker's switches set (ENV_KEYED: PS2X_READABLE_FRAMES and the like) keys it in
every file (tools40).
The keys are a host's own (the base has the compiler and the compile commands, a file's key its path), but the
verdicts are not: the Linux worker's checker and the Windows worker's one gave the same result lines on every one of
DM2's 113,808 files (batch 108; only faults' phase texts differ, the host's exit code), so `cache-import
--map-corpus` fills one host's cache from the other's full run of the same commit (the Windows worker's from the Linux worker's
batch fill, tools8). Windows keeps a game's cache as the Linux worker does (worker\\cache\\fncache-<game>): the paths in
files and commands are folded and written alike for every slot's build dir and checkout, the side files locked
with msvcrt.
`--extras-changed` takes a function's safepoints and markers lines from the
cache when its source and original are unchanged even if the checker changed
(the core lines replayed); `--extras-for` names functions always checked with
them (promotion candidates). The binary must be the build's (byte-identical)
and newer than its sources.

`run --semantic` (or `--fncheck-arg --semantic`) is the gate: ps2xFnCheck
--semantic in place of --cycles, with no --markers or --safepoints. A call
passes on what it does (results, RAM, calls and their arguments, args_left,
entry_left, the VU0 lanes later code reads); each line says mode=semantic, and
readable_bad still counts the readable side alone, original_bad the original's.

`run --frame-index DIR` (tools23) makes the folders' index of leftover reads first: every
recording's original replayed alone (ps2xFnCheck --frame-reads-only --frame-reads-out DIR), each
call's reads of its own frame before it writes them, with their values; then the replay itself with
--frame-reads-in DIR, where residue a readable version leaves in its frame unlike the original
that the folder's next leftover read finds fails (frame_residue_read). A file's cache key holds
its folder's index (its digest), since the verdict depends on the folder's other recordings. The
index covers every folder, whatever --incremental replays; with the cache it is kept per folder
(CACHE/frame-index) under the folder's recordings, their generated originals and the checker's
base, so only a folder whose key changed is replayed for it.

`run` exits 1 when the readable side failed or faulted on any recording, 2 when a cached file differs from its
replay, 0 otherwise: the original's own failures are said, not in the status (replay_status). A job's non-zero
exit is its failure: rcheck then makes no compare and sets no reference.

`compare` matches two runs file by file: the same functions must pass and
fail (original replay and readable version), with the same counts.
--ignore-new leaves out failures of the checks added in round `coverage`
(f0 of an integer function, stack arguments): their own report. `--no-worse`
(the merge gate): only a pass in A that is not one in B fails it, so a replay
under --cycles can be the reference for a semantic one; a verdict that got
better, or a count that moved with no verdict moving, is listed and passes. A
line of B made under --cycles fails it when --semantic is given.

`coverage` sums the blocks hit over every recording of a function (the
union of the block sets each file hit, over every DIR given) and gives
blocks total, hit, the share and the uncovered blocks' addresses.
--route-prefix keeps only routes whose name starts with one of P (comma
separated; `-P` leaves them out). --covered-out writes the blocks hit, a line
a function (`0x00125be0 0x125be0,0x125c00,...`), the seed the recorder's
PS2X_FNREC_COVERED reads (only calls that run a block not in it are kept with
PS2X_FNREC_NEW_BLOCKS=1).

`generate` runs ps2xFnCheck --generate (the pure vector routines checked on
generated inputs, default 20000 calls) and keeps its [fncheck:coverage] lines
in DIR/lines.jsonl (route `generate`), so `coverage DIR_A DIR` counts the
paths those calls checked with the recordings'.

`emit` writes the work database's lines (tooling/dm2/MATCH3.md section 6):
a `check` line per function and route with the digests the pass was made
against, and a `coverage` line per function over the corpus.
"""
import argparse
import atexit
import collections
import concurrent.futures
import copy
import ctypes
import datetime
try:
    import fcntl
except ImportError:   # Windows (remote1): msvcrt's byte-range lock in its place (lock_file, below)
    fcntl = None
    import msvcrt
import hashlib
import json
import shutil
import os
import posixpath
import re
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zlayout  # noqa: E402  (a tree's folders, old or new)
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)
import fnrec_io  # noqa: E402  (a recording plain or compressed: its plain size)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')

WINDOWS = sys.platform == 'win32'   # job objects instead of process groups (remote1, below)
CORPUS = os.path.join(SCRATCH_ROOT, 'reccorpus', 'v5')
RESULTS = os.path.join(SCRATCH_ROOT, 'workdb', 'results')
LINE = re.compile(r'^\[fncheck:(result|coverage|fault|build|markers|safepoints|callblocks|upper)\] (.*)$', re.M)
FIELD = re.compile(r'(\w+)=("(?:[^"]*)"|\S+)')


def fields(text):
    return {k: (v[1:-1] if v.startswith('"') else v) for k, v in FIELD.findall(text)}


def log(msg):
    print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def corpus_files(folders, only=None):
    """[(route, path)] of every recording: each folder's route subfolders, or
    the folder itself when it holds recordings."""
    out = []
    for top in folders:
        top = os.path.abspath(os.path.expanduser(top))
        direct = [n for n in os.listdir(top) if n.endswith('.fnrec')]
        # A folder of recordings is named by its last three path parts
        # (dm2rec0/runs/rec1/rec: runs/rec1/rec).
        subs = [('/'.join(top.replace(os.sep, '/').rstrip('/').split('/')[-3:]), top)] if direct else [
            (r, os.path.join(top, r)) for r in sorted(os.listdir(top))
            if os.path.isdir(os.path.join(top, r)) and r != 'work']
        for route, folder in subs:
            for name in sorted(os.listdir(folder)):
                m = re.match(r'^(?:([a-z0-9]+)-)?([0-9a-f]{8})\.fnrec$', name)
                if not m:
                    continue
                if only and int(m.group(2), 16) not in only:
                    continue
                path = os.path.join(folder, name)
                # (Windows: '/' separators, which its file APIs take; ps2xFnCheck's
                # result lines would print a '\\' as "'", and run_chunk matches
                # the files it gave by their printed names)
                out.append((route, path.replace('\\', '/') if WINDOWS else path))
    return out


TIME_L = '/usr/bin/time' if os.path.exists('/usr/bin/time') and sys.platform == 'darwin' else None
MAXRSS = re.compile(r'^\s*(\d+)\s+maximum resident set size', re.M)

# ---- the children: process groups and the memory cap -------------------------
#
# Each ps2xFnCheck runs in a session (process group) of its own, with
# /usr/bin/time around it when stats are on; a timeout, the memory cap, an
# error or Ctrl-C kills the whole group, never only `time` (the 2026-10-26
# kernel panic: five orphaned ps2xFnCheck processes, 45 to 113 GB, on a 48 GB
# Mac). The group's memory (macOS: every member's phys_footprint, resident and
# compressed, as memguard.py reads it; Linux: VmRSS) is polled twice a second;
# above MAX_MEM_BYTES (--max-mem-gb, default 4) the group is killed and the
# file it was in is a fault with phase `memory`, and the rest of the chunk goes
# on, as after a timeout.

MAX_MEM_BYTES = 4 << 30
POLL_SECONDS = 0.5
ACTIVE_GROUPS = set()          # the process groups of running children
ACTIVE_LOCK = threading.Lock()
STOPPING = threading.Event()   # set on Ctrl-C or an error: no new children

if sys.platform == 'darwin':
    _libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib')

    def footprint(pid):
        """A process's phys_footprint in bytes (proc_pid_rusage, RUSAGE_INFO_V2), or 0."""
        buf = ctypes.create_string_buffer(512)
        if _libc.proc_pid_rusage(pid, 2, buf) != 0:
            return 0
        return int.from_bytes(buf.raw[72:80], 'little')
else:
    def footprint(pid):
        try:
            for line in open('/proc/%d/status' % pid):
                if line.startswith('VmRSS:'):
                    return int(line.split()[1]) * 1024
        except OSError:
            pass
        return 0


def group_members(pgid):
    """The pids of process group pgid (ps), the leader first."""
    try:
        out = subprocess.run(['ps', '-axo', 'pid=,pgid='], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return [pgid]
    pids = [int(a) for a, b in (l.split() for l in out.splitlines() if len(l.split()) == 2) if int(b) == pgid]
    return sorted(pids, key=lambda x: x != pgid)


def kill_group(pgid):
    if WINDOWS:
        return _win_kill_group(pgid)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def kill_all_children():
    STOPPING.set()
    with ACTIVE_LOCK:
        groups = list(ACTIVE_GROUPS)
    for g in groups:
        kill_group(g)


atexit.register(kill_all_children)


def run_child(cmd, timeout):
    """Runs cmd in its own process group: (stdout, stderr, status, why, peak bytes).
    status None and why 'timeout' or 'memory' when the group was killed for it."""
    if WINDOWS:
        return _win_run_child(cmd, timeout)
    with tempfile.TemporaryFile() as fo, tempfile.TemporaryFile() as fe:
        proc = subprocess.Popen(cmd, stdout=fo, stderr=fe, stdin=subprocess.DEVNULL, start_new_session=True)
        with ACTIVE_LOCK:
            ACTIVE_GROUPS.add(proc.pid)
        why, peak, status = None, 0, None
        # time.monotonic: on macOS it stops while the Mac sleeps (drafter24:
        # the clock ran on through a clamshell sleep and timed chunks out).
        t0 = time.monotonic()
        try:
            while True:
                try:
                    status = proc.wait(POLL_SECONDS)
                    break
                except subprocess.TimeoutExpired:
                    pass
                if STOPPING.is_set():
                    why = 'stopped'
                    break
                if time.monotonic() - t0 > timeout:
                    why = 'timeout'
                    break
                used = sum(footprint(pid) for pid in group_members(proc.pid))
                peak = max(peak, used)
                if MAX_MEM_BYTES and used > MAX_MEM_BYTES:
                    why = 'memory'
                    break
        finally:
            if status is None:
                kill_group(proc.pid)
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass
            else:
                kill_group(proc.pid)  # anything the leader left behind
            with ACTIVE_LOCK:
                ACTIVE_GROUPS.discard(proc.pid)
        fo.seek(0)
        fe.seek(0)
        out = fo.read().decode('utf-8', 'replace')
        err = fe.read().decode('utf-8', 'replace')
    return out, err, (None if why else status), why, peak


# ---- Windows: job objects instead of process groups (remote1) ----------------
#
# No process groups, killpg or /usr/bin/time there. Each ps2xFnCheck goes into
# a job object of its own (whatever it starts joins it), killed whole with
# TerminateJobObject on a timeout, the memory cap, an error or Ctrl-C, and by
# Windows itself when this process ends first (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE:
# no orphan outlives the run, whatever kills it). The memory polled is the
# members' private bytes (PrivateUsage, what ps2xFnCheck's own --max-mem-mb
# reads there). The job also has a hard commit limit at twice the cap: a
# runaway faster than the poll has an allocation refused there instead of the
# desktop's memory taken, and its file is a fault with phase `memory` as well.
# The peak resident set (the child's PeakWorkingSetSize) is added to its stderr
# text in /usr/bin/time -l's form, for run_chunk's maxrss_mb.

if WINDOWS:
    from ctypes import wintypes as _wt
    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _k32.CreateJobObjectW.restype = _wt.HANDLE
    _k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, _wt.LPCWSTR]
    _k32.OpenProcess.restype = _wt.HANDLE
    _k32.OpenProcess.argtypes = [_wt.DWORD, _wt.BOOL, _wt.DWORD]
    _k32.SetInformationJobObject.argtypes = [_wt.HANDLE, ctypes.c_int, ctypes.c_void_p, _wt.DWORD]
    _k32.QueryInformationJobObject.argtypes = [_wt.HANDLE, ctypes.c_int, ctypes.c_void_p, _wt.DWORD,
                                               ctypes.c_void_p]
    _k32.AssignProcessToJobObject.argtypes = [_wt.HANDLE, _wt.HANDLE]
    _k32.TerminateJobObject.argtypes = [_wt.HANDLE, _wt.UINT]
    _k32.CloseHandle.argtypes = [_wt.HANDLE]
    _k32.K32GetProcessMemoryInfo.argtypes = [_wt.HANDLE, ctypes.c_void_p, _wt.DWORD]
    _k32.GetSystemTimes.argtypes = [ctypes.c_void_p] * 3
    _k32.GlobalMemoryStatusEx.argtypes = [ctypes.c_void_p]

    class _BasicLimit(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                    ('LimitFlags', _wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', _wt.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', _wt.DWORD), ('SchedulingClass', _wt.DWORD)]

    class _ExtendedLimit(ctypes.Structure):
        _fields_ = [('BasicLimitInformation', _BasicLimit), ('IoInfo', ctypes.c_ulonglong * 6),
                    ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

    class _MemoryCounters(ctypes.Structure):   # PROCESS_MEMORY_COUNTERS_EX
        _fields_ = [('cb', _wt.DWORD), ('PageFaultCount', _wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
            'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage',
            'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage',
            'PrivateUsage')]

    class _MemoryStatus(ctypes.Structure):     # MEMORYSTATUSEX
        _fields_ = [('dwLength', _wt.DWORD), ('dwMemoryLoad', _wt.DWORD)] + [(n, ctypes.c_ulonglong) for n in (
            'ullTotalPhys', 'ullAvailPhys', 'ullTotalPageFile', 'ullAvailPageFile', 'ullTotalVirtual',
            'ullAvailVirtual', 'ullAvailExtendedVirtual')]

    _QUERY, _VM_READ, _SET_QUOTA, _TERMINATE = 0x1000, 0x0010, 0x0100, 0x0001
    _WIN_JOBS = {}                 # the leader's pid -> its _WinJob (beside ACTIVE_GROUPS)

    def _memory_counters(handle):
        c = _MemoryCounters()
        c.cb = ctypes.sizeof(c)
        return c if _k32.K32GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb) else None

    def footprint(pid):
        """A process's private bytes (PrivateUsage), or 0."""
        h = _k32.OpenProcess(_QUERY | _VM_READ, False, pid)
        if not h:
            return 0
        try:
            c = _memory_counters(h)
            return c.PrivateUsage if c else 0
        finally:
            _k32.CloseHandle(h)

    class _WinJob:
        """A job object for one child and whatever it starts."""

        def __init__(self, hard_limit):
            self.lock = threading.Lock()
            self.h = _k32.CreateJobObjectW(None, None)
            if not self.h:
                raise OSError(ctypes.get_last_error(), 'CreateJobObject failed')
            info = _ExtendedLimit()
            info.BasicLimitInformation.LimitFlags = 0x2000 | (0x200 if hard_limit else 0)  # KILL_ON_JOB_CLOSE, JOB_MEMORY
            info.JobMemoryLimit = hard_limit
            _k32.SetInformationJobObject(self.h, 9, ctypes.byref(info), ctypes.sizeof(info))
            self.child = None
            self.pid = None
            self.joined = False

        def add(self, pid):
            """The child into the job (if that fails, taskkill /T is the way to kill its tree)."""
            self.pid = pid
            self.child = _k32.OpenProcess(_QUERY | _VM_READ | _SET_QUOTA | _TERMINATE, False, pid)
            self.joined = bool(self.child and _k32.AssignProcessToJobObject(self.h, self.child))

        def pids(self):
            n = 256

            class _Ids(ctypes.Structure):  # JOBOBJECT_BASIC_PROCESS_ID_LIST
                _fields_ = [('assigned', _wt.DWORD), ('listed', _wt.DWORD), ('ids', ctypes.c_size_t * n)]
            ids = _Ids()
            with self.lock:
                if not self.h or not self.joined or not _k32.QueryInformationJobObject(
                        self.h, 3, ctypes.byref(ids), ctypes.sizeof(ids), None):
                    return [self.pid] if self.pid else []
            return [int(ids.ids[i]) for i in range(min(ids.listed, n))]

        def peaks(self):
            """(the child's peak working set, the job's peak commit) in bytes."""
            info = _ExtendedLimit()
            with self.lock:
                ws = 0
                if self.child:
                    c = _memory_counters(self.child)
                    ws = c.PeakWorkingSetSize if c else 0
                if self.h and _k32.QueryInformationJobObject(self.h, 9, ctypes.byref(info), ctypes.sizeof(info), None):
                    return ws, info.PeakJobMemoryUsed
            return ws, 0

        def kill(self):
            with self.lock:
                if not self.h:
                    return
                if self.joined:
                    _k32.TerminateJobObject(self.h, 137)   # (not 0 or 1, ps2xFnCheck's own exits)
                    return
            if self.pid:
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.pid)], capture_output=True)

        def close(self):
            with self.lock:
                if self.child:
                    _k32.CloseHandle(self.child)
                if self.h:
                    _k32.CloseHandle(self.h)
                self.child = self.h = None


def _win_kill_group(pgid):
    job = _WIN_JOBS.get(pgid)
    if job is not None:
        job.kill()


def _win_run_child(cmd, timeout):
    """run_child on Windows: the child in a job object (above)."""
    hard = 2 * MAX_MEM_BYTES if MAX_MEM_BYTES else 0
    with tempfile.TemporaryFile() as fo, tempfile.TemporaryFile() as fe:
        proc = subprocess.Popen(cmd, stdout=fo, stderr=fe, stdin=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        job = _WinJob(hard)
        job.add(proc.pid)
        with ACTIVE_LOCK:
            ACTIVE_GROUPS.add(proc.pid)
            _WIN_JOBS[proc.pid] = job
        why, peak, status = None, 0, None
        t0 = time.monotonic()
        try:
            while True:
                try:
                    status = proc.wait(POLL_SECONDS)
                    break
                except subprocess.TimeoutExpired:
                    pass
                if STOPPING.is_set():
                    why = 'stopped'
                    break
                if time.monotonic() - t0 > timeout:
                    why = 'timeout'
                    break
                used = sum(footprint(pid) for pid in job.pids())
                peak = max(peak, used)
                if MAX_MEM_BYTES and used > MAX_MEM_BYTES:
                    why = 'memory'
                    break
        finally:
            job.kill()  # the child on a kill, else anything it left behind
            if status is None:
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass
            peak_ws, peak_commit = job.peaks()
            with ACTIVE_LOCK:
                ACTIVE_GROUPS.discard(proc.pid)
                _WIN_JOBS.pop(proc.pid, None)
            job.close()
        if why is None and status not in (0, 1) and hard and peak_commit >= hard - (64 << 20):
            why = 'memory'   # the job's commit limit refused an allocation
            peak = max(peak, peak_commit)
        fo.seek(0)
        fe.seek(0)
        out = fo.read().decode('utf-8', 'replace')
        err = fe.read().decode('utf-8', 'replace')
        if peak_ws:
            err += '\n%20d  maximum resident set size\n' % peak_ws
    return out, err, (None if why else status), why, peak


def _win_auto_jobs(cap):
    """auto_jobs on Windows: the free share from GlobalMemoryStatusEx, the load
    from the processors' busy share over a second (GetSystemTimes)."""
    ms = _MemoryStatus()
    ms.dwLength = ctypes.sizeof(ms)
    if not _k32.GlobalMemoryStatusEx(ctypes.byref(ms)) or not ms.ullTotalPhys:
        return 1

    def times():
        t = [ctypes.c_ulonglong() for _ in range(3)]   # idle, kernel (with idle), user
        _k32.GetSystemTimes(*[ctypes.byref(x) for x in t])
        return [x.value for x in t]
    a = times()
    time.sleep(1.0)
    b = times()
    idle, total = b[0] - a[0], (b[1] - a[1]) + (b[2] - a[2])
    cores = os.cpu_count() or 1
    load = cores * (1.0 - idle / float(total)) if total else 0.0
    free, total_mem = ms.ullAvailPhys / float(ms.ullTotalPhys), ms.ullTotalPhys
    per_job = max(MAX_MEM_BYTES, PEAK_MB << 20)
    by_memory = int((free - FREE_FLOOR) * total_mem / per_job)
    by_cores = int(cores - load)
    return max(0, min(cap, by_memory, max(by_cores, 1 if by_memory > 0 else 0)))


# Windows (remote1): the Windows worker's corpus is on a hard disk, where 16
# ps2xFnCheck processes reading their files in small pieces at once made the
# replay seek-bound (7.5 MB/s read, 10 % CPU, 24 chunks in 12 minutes). So a
# chunk's files are first read into the file cache in 8 MB pieces, one chunk
# at a time across the jobs (a chunk's files lie together on the disk, as the
# corpus sync wrote them), and ps2xFnCheck then reads them from memory.
PREFETCH = WINDOWS
PREFETCH_LOCK = threading.Lock()


def prefetch(paths):
    buf = bytearray(8 << 20)
    with PREFETCH_LOCK:
        for p in paths:
            try:
                with open(p, 'rb', buffering=0) as f:
                    while f.readinto(buf):
                        pass
            except OSError:
                pass


_OWN_CAP = {}
_OWN_CAP_LOCK = threading.Lock()


def own_cap_flags(fncheck):
    """ps2xFnCheck's own memory cap (--max-mem-mb, round drafter23), set a
    little under the group's (MAX_MEM_BYTES), so the child ends itself as a
    fault with its phase before the poll here has to kill it; [] for a binary
    without the option (an older build)."""
    if not MAX_MEM_BYTES:
        return []
    # (looked for once, by one thread, through a map of the file: every job's thread read the whole binary at the
    # start, 14 x 840 MB for DM2's checker on the Linux worker, an 8.6 GB spike in its scope; tools7)
    with _OWN_CAP_LOCK:
        if fncheck not in _OWN_CAP:
            try:
                import mmap
                with open(fncheck, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                    _OWN_CAP[fncheck] = m.find(b'--max-mem-mb') >= 0
            except (OSError, ValueError):
                _OWN_CAP[fncheck] = False
    if not _OWN_CAP[fncheck]:
        return []
    return ['--max-mem-mb', str(max(64, int(MAX_MEM_BYTES * 0.9) >> 20))]


def run_chunk(fncheck, route, paths, timeout, extra, stats=None):
    """Every line for tools from the chunk's files (restarting after a fault,
    a timeout or the memory cap). stats (a list) gets a record per process:
    files, seconds, peak RSS (MB, /usr/bin/time -l on macOS), the polled peak
    footprint, exit status, why it was killed."""
    lines = []
    todo = sorted(paths)
    if PREFETCH:
        prefetch(todo)
    while todo and not STOPPING.is_set():
        cmd = [fncheck] + CORE_FLAGS + own_cap_flags(fncheck) + extra + todo
        if TIME_L and stats is not None:
            cmd = [TIME_L, '-l'] + cmd
        t0 = time.time()
        out, err, status, why, peak = run_child(cmd, timeout)
        if stats is not None:
            m = MAXRSS.findall(err or '')
            stats.append({'route': route, 'files': len(todo), 'first': os.path.basename(todo[0]),
                          'seconds': round(time.time() - t0, 2), 'status': status, 'args': extra,
                          'maxrss_mb': round(int(m[-1]) / 1048576.0, 1) if m else None,
                          'peak_mb': round(peak / 1048576.0, 1), 'killed': why})
        done = set()
        fault = None
        for kind, text in LINE.findall(out):
            f = fields(text)
            f['kind'] = kind
            f['route'] = route
            lines.append(f)
            if kind == 'result':
                done.add(f.get('file'))
            elif kind == 'fault':
                fault = f.get('file')
        if status == 0 or status == 1 or why == 'stopped':
            break
        # A fault (exit 3), a crash, a timeout or the memory cap: the file it
        # stopped in, then the rest.
        rest = [p for p in todo if p not in done]
        if not rest:
            break
        stopped = fault if fault in rest and not why else rest[0]
        if fault != stopped or why:
            phase = ('memory' if why == 'memory' else 'timeout' if why == 'timeout' else 'exit %s' % status)
            entry = {'kind': 'fault', 'route': route, 'file': stopped, 'phase': phase}
            # (an abort says why on stderr only: DM2 testNodeFaces' "[readable] ee::Local 0 of 0x0011a870 does
            # not fit its frame" was a bare exit -6 on all 51 recordings)
            said = last_words(err) if not why else None
            if said:
                entry['message'] = said
            if why == 'memory':
                entry['peak_mb'] = str(round(peak / 1048576.0))
                log('memory cap: %s killed at %.0f MB in %s' % (os.path.basename(fncheck), peak / 1048576.0, stopped))
            lines.append(entry)
        todo = rest[rest.index(stopped) + 1:]
    return lines


# ---- Incremental replay: the verdict cache ---------------------------------
#
# A recording file's lines depend on: the file itself; the function's readable
# source and every readable definition it can reach by name (any file of the
# build's readable sources, the sweep's drafts included), with the rest of
# those files (registrations, file-level data) and the headers they include
# (the build's own dependency log); the traced original (its traced_<hex>.cpp
# and blocks file); the checker without the readable functions (fncheck_main,
# the runtime, every header the traced originals and the runtime include, the
# compile commands, the compiler); and the check's flags. Each is a digest
# (Tree below); a cache entry is keyed by all of them, per part of a file's
# lines: core (result, coverage, fault: --cycles and the other options),
# safepoints (--safepoints A B), markers (--markers). A file that faulted is
# kept whole under the full flags (part `all`).

HERE = os.path.dirname(os.path.abspath(__file__))
# The runtime code a replay runs (Tree.verdict_input).
RUNTIME_SOURCES = {'src/lib/ps2_memory.cpp', 'src/lib/ps2_runtime.cpp', 'src/lib/ps2_thread_cpu.cpp'}
RUNTIME_HEADERS = set()
sys.dont_write_bytecode = True   # its imports (readable_digest, readable_corpus) leave no __pycache__
CACHE = os.path.join(SCRATCH_ROOT, 'reccorpus', 'fncache')


def last_words(err):
    """The runtime's last word before a crash (ee/abi.cpp's frame checks abort after one), or None: stderr's last
    line, past /usr/bin/time -l's (indented numbers), if it is a `[readable]` one. A crash's fault keeps it as
    `message`."""
    lines = [l for l in (err or '').splitlines() if l.strip() and not re.match(r'\s+\d', l)]
    return lines[-1].strip() if lines and lines[-1].startswith('[readable] ') else None


CORE_FLAGS = ['--cycles', '--show', '1']   # (run --semantic: SEMANTIC_FLAGS in its place)
SEMANTIC_FLAGS = ['--semantic', '--show', '1']
SYSTEM = ('/Library/Developer/', '/Applications/Xcode', '/usr/include/')   # (see Tree.is_system)
ON_WINDOWS = sys.platform == 'win32'
# The checker's target by its CMake name (a phony for ps2xRuntime/ps2xFnCheck,
# .exe on Windows): the same inputs and commands on every platform.
FNCHECK_TARGET = 'ps2xFnCheck'

# The replay's environment (tools40). ps2xFnCheck inherits the run's, and the switches the checker's own code reads
# from it change how a readable call runs or is judged, which no source, header or flag of the key says: a run with
# PS2X_READABLE_FRAMES=0 exported was served the lines of a run with frames on. A switch set (not empty) is in
# every key of the run as `env:NAME=VALUE` (unset, the keys are as before); run.json keeps them for cache-import.
# A name the key's sources read (a "PS2X_..." or "ZAXIS_..." literal in a base or readable file, Tree.env_names)
# that neither table below names is keyed too, and the log says so: unclear is in.
ENV_KEYED = {
    'PS2X_READABLE_FRAMES': 'ee::framesOn: the frame model a readable call runs in (0: none)',
    'PS2X_READABLE_HELD': 'held::wanted: the held-register table on or off',
    'PS2X_READABLE_HELD_CODE': 'held.cpp codeMode: the held programs interpreted, compiled or checked both ways',
    'PS2X_READABLE_HELD_RANGE': 'held.cpp: the held table only in (or out of) these functions',
    'PS2X_READABLE_HELD_ENTRY_LOADS': 'held.cpp loadsOn: the entry loads of the held table',
    'PS2X_READABLE_HELD_SUCCESSORS': 'held.cpp: =check checks the table\'s successors (aborts on a bad one)',
    'PS2X_READABLE_HELD_CODE_ROUNDS': 'held.cpp sweepCode: rounds of made-up states each program runs (fails on one)',
    'PS2X_READABLE_SAFEPOINTS': 'ee::safePointsOn: safe points, so the fibers a readable call runs on',
    'PS2X_READABLE_FIBER_CALLS': 'suspend.cpp fiberCallsOn: guest calls on a fiber of their own',
    'PS2X_READABLE_RETURNS': 'ee::returnsOn: guest calls return through the published ra, or the runtime\'s slots',
    'PS2X_READABLE_LEAN': 'abi.cpp readLeanParts (ps2x::boot::text): the proof scaffolding\'s parts off',
    'PS2X_READABLE_LEAN_RANGE': 'abi.cpp leanInRange: lean only in these functions',
    'PS2X_STACK_POISON': 'abi.cpp poisonDeadStack: a dead frame filled with 0x5A (what a residue read finds)',
    'PS2X_FNCHECK_PROBE_VERIFY': 'fncheck_main lookAtCallee: what a probe left in RAM put back (what the replay reads)',
    'PS2X_JOB_RETURNS': 'zge saveJobReturnsForRecording: a readable function\'s own branch',
    'PS2X_NATIVE_COLLISION': 'zge/native collision_data.cpp: the native collision a readable caller can reach',
    'PS2X_NATIVE_COLLISION_CHECK': 'zge/native collision_check.cpp: the native collision checked against the game\'s',
    'PS2X_NATIVE_COLLISION_POISON': 'zge/native collision_data.cpp: the game\'s collision memory poisoned',
    'PS2X_NATIVE_MESHES_CHECK': 'zge/native geo_meshes.cpp: the native meshes checked against the game\'s',
    'PS2X_NATIVE_TEXTURES': 'zge/native native_textures.cpp: native textures in place of the game\'s',
}
_GAME_RUN = 'read by game::readable::install (readable.cpp), which a game run calls and ps2xFnCheck never does'
_HOST = 'read by PS2Runtime::initialize or the host (window, GS, VU1, IOP, pacing), which ps2xFnCheck never runs'
_SAID = 'a trace or count on stderr: no line the run keeps ([fncheck:...] on stdout)'
_PLACE = 'where a host file or folder is (a job\'s scratch HOME): a replay opens none, its memory is the recording\'s'
ENV_NOT_KEYED = dict(
    [(n, _GAME_RUN) for n in (
        'PS2X_READABLE', 'PS2X_FNREC', 'PS2X_FNREC_ATOMIC', 'PS2X_FNREC_COVERED', 'PS2X_FNREC_DIR', 'PS2X_FNREC_EFFECTS',
        'PS2X_FNREC_FROM', 'PS2X_FNREC_MAX', 'PS2X_FNREC_MAX_MB', 'PS2X_FNREC_MIN_CALLS', 'PS2X_FNREC_NEW_BLOCKS',
        'PS2X_FNREC_PER_VSYNC', 'PS2X_FNREC_UNWOUND', 'PS2X_FNREC_WAITS', 'PS2X_GUEST_CALL', 'PS2X_GUEST_STRINGS',
        'PS2X_DIFFCALL', 'PS2X_DIFFCALL_BANK', 'PS2X_DIFFCALL_CASE_MB', 'PS2X_DIFFCALL_CASE_MS', 'PS2X_DIFFCALL_DEBUG',
        'PS2X_DIFFCALL_DIR', 'PS2X_DIFFCALL_PATH_KEEP', 'PS2X_DIFFCALL_PATHS', 'PS2X_DIFFCALL_WAIT',
        'PS2X_STACK_POISON_STORES', 'PS2X_STACK_POISON_FRAMES', 'PS2X_FNPROF', 'PS2X_FNPROF_FROM', 'PS2X_FNPROF_FOR',
        'PS2X_FNCYCLES', 'PS2X_FNCYCLES_ARGS', 'PS2X_FNCYCLES_FROM', 'PS2X_FNCYCLES_VU0', 'PS2X_FNCYCLES_WATCH',
        'PS2X_TAILCALL_STATS')] +
    [(n, _HOST) for n in (
        'PS2X_WIDESCREEN', 'PS2X_ASPECT', 'PS2X_MOTION_BLUR', 'PS2X_DRAW_DISTANCE', 'PS2X_HEADLESS', 'PS2X_GS_BACKEND',
        'PS2X_GS_SCALE', 'PS2X_GS_DEFER', 'PS2X_GS_DUMP', 'PS2X_GS_DUMP_COUNT', 'PS2X_GS_DUMP_FRAME', 'PS2X_SHOT_AT',
        'PS2X_DUMP_AT_FRAME', 'PS2X_DUMP_PREFIX', 'PS2X_DUMP_RAM_ON_EXIT', 'PS2X_VU1_ASYNC', 'PS2X_VU1_DUMP',
        'PS2X_VU1_DUMP_AT', 'PS2X_VU1_DUMP_MATCH', 'PS2X_VU1_RECORD', 'PS2X_VU1_RECORD_FROM', 'PS2X_VU1_RECORD_RUNS',
        'PS2X_VU1_RECORD_VSYNCS', 'PS2X_VU1_SKIP', 'PS2X_VU1_STATS', 'PS2X_VIF1_PREFETCH', 'PS2X_VIF1_SHADOW_CHECK',
        'PS2X_SOUND_PCM', 'PS2X_LOAD_ALL_IOP_MODULES', 'PS2X_VSYNC_PHASE', 'PS2X_THREAD_PLACEMENT', 'PS2X_PERF_LOG',
        'PS2X_PERF_FRAMES', 'PS2X_WATCH32', 'ZAXIS_FRAME_QUEUE')] +
    [(n, _SAID) for n in (
        'PS2X_FNCHECK_PROBES', 'PS2X_SAFEPOINT_TRACE', 'PS2X_READABLE_HELD_TRACE', 'PS2X_FNREC_WAITS_DEBUG',
        'PS2X_COLLISION_TRACE', 'PS2X_COLLISION_TIME', 'PS2X_COMPARE_TOTALS', 'PS2X_NATIVE_COLLISION_DEBUG',
        'PS2X_NATIVE_MESHES_DEBUG', 'PS2X_TRACE_CALLS', 'PS2X_TRACE_DMA', 'PS2X_TRACE_DRAW_RECORDS',
        'PS2X_TRACE_NATIVE_TEXTURES', 'PS2X_TRACE_PACKETS', 'PS2X_TRACE_RPC', 'PS2X_TRACE_SAVES', 'PS2X_TRACE_THREADS',
        'PS2X_TRACE_VU1', 'PS2X_TRACE_WIDESCREEN')] +
    [(n, _PLACE) for n in (
        'PS2X_SAVES_DIR', 'PS2X_SAVE_PROFILE', 'PS2X_MODS_DIR', 'PS2X_XBOX_PACK', 'PS2X_XBOX_MOVIES',
        'PS2X_APP_SUPPORT_DIR', 'PS2X_APP_LOG_DIR', 'PS2X_SCREENSHOT_DIR')])   # (job.ps1 sets the last three)
ENV_NAME = re.compile(rb'"((?:PS2X|ZAXIS)_[A-Z0-9_]+)"')


def replay_env(names=()):
    """(the run's keyed switches as ['NAME=VALUE', ...], the unclassified names among them): ENV_KEYED's and the
    sources' other names (names) that ENV_NOT_KEYED leaves out, each set and not empty in this environment."""
    # (a name ending in _DIR is a place, as _PLACE's: a job's own folder would make every key the job's alone)
    wanted = set(ENV_KEYED) | set(n for n in names if not n.endswith('_DIR')) - set(ENV_NOT_KEYED)
    out, unclassified = [], []
    for name in sorted(wanted):
        value = os.environ.get(name)
        if value:
            out.append('%s=%s' % (name, value))
            if name not in ENV_KEYED:
                unclassified.append(name)
    return out, unclassified


def norm_path(path):
    """A path as Tree keeps it: absolute, '/' separators, on Windows case
    folded (its file names and drive letters are case-blind), so the digests'
    names and the prefix tests are the same on every platform (remote1: the
    keys read paths as POSIX ones)."""
    path = os.path.normpath(path)
    if ON_WINDOWS:
        path = os.path.normcase(path)
    return path.replace('\\', '/')


def listed_path(home, path):
    """A file --list names, as norm_path keeps it: a registration names its file from the repository's top
    (runtime/src/..., ee/readable.h's sourcePath, the same on every host), a binary from before by its absolute path;
    the first is the built tree's (home's)."""
    if home and not os.path.isabs(path) and not re.match(r'^[A-Za-z]:[/\\]', path):
        path = os.path.join(home, path)
    return norm_path(path)


def _ci(names):
    """A name, tuple or set of names as the labels are compared: case folded on Windows (norm_path), as they are
    elsewhere."""
    if not ON_WINDOWS:
        return names
    if isinstance(names, str):
        return names.lower()
    return type(names)(n.lower() for n in names)
# The main checkout's builds (out/...), which the coordinator's merges rebuild.
MAIN_BUILDS = None if ON_WINDOWS or not zaxis_paths.get('ZAXIS_MAIN', required=False) else \
    norm_path(os.path.join(zaxis_paths.get('ZAXIS_MAIN'), 'out')) + '/'
CODE_EXT = ('.h', '.hpp', '.hh', '.c', '.cc', '.cpp', '.inc', '.inl', '.ipp')
DEF_TAIL = re.compile(r'\s*(?:const\s*|noexcept\s*|override\s*|final\s*|->\s*[\w:<>,\s*&]+?\s*)*'
                      r'(?::\s*[\w:]+\s*[({][^;{}]*[)}](?:\s*,\s*[\w:]+\s*[({][^;{}]*[)}])*\s*)?\{')
ANY_REG = re.compile(r'\bREADABLE_[A-Z0-9_]+\(\s*(?:(\w+)\s*,\s*)?(0x[0-9A-Fa-f]+)\s*,[^\n]*')
# What a definition can run itself: names in call position (`f(`, `f<T>(`) and
# functions whose address it takes (`&f`), but not game::call's and
# game::tailCall's template arguments (answered from the recording).
CALLED = re.compile(r'\b([A-Za-z_]\w*)\s*(?:<[^<>;(){}]*>\s*)?\(|(?<![<,]\s)(?<![<,])&\s*(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)')
REPLAYED = re.compile(r'\b(?:call|tailCall|callAt|tailCallAt)\s*<\s*&\s*[\w:]+')
FNREC = re.compile(r'^(?:([a-z0-9]+)-)?([0-9a-f]{8})\.fnrec$')


def lock_file(f):
    """An exclusive lock on an open file until unlock_file: flock, or on Windows msvcrt's lock of its first byte
    (LK_LOCK gives up after ten seconds: asked again until it is had; the Windows worker's slots share a game's cache
    folder as the Linux worker's do, tools8)."""
    if fcntl is not None:
        fcntl.flock(f, fcntl.LOCK_EX)
        return
    f.seek(0)   # (the lock is on the file's first byte, whatever each process's position)
    while True:
        try:
            msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
            return
        except OSError:
            time.sleep(0.2)


def unlock_file(f):
    if fcntl is not None:
        fcntl.flock(f, fcntl.LOCK_UN)
    else:
        try:
            f.flush()
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        except OSError:
            pass


def sha(*parts):
    h = hashlib.sha256()
    for x in parts:
        h.update(x if isinstance(x, bytes) else str(x).encode('utf-8', 'replace'))
        h.update(b'\0')
    return h.hexdigest()[:24]


def split_flags(extra):
    """(core options, {part: options}) of a run's ps2xFnCheck options."""
    core, parts, i = [], {}, 0
    while i < len(extra):
        if extra[i] == '--safepoints':
            parts['safepoints'] = extra[i:i + 3]
            i += 3
        elif extra[i] == '--markers':
            parts['markers'] = ['--markers']
            i += 1
        else:
            core.append(extra[i])
            i += 1
    return CORE_FLAGS + core, parts


def part_of(kind):
    return kind if kind in ('safepoints', 'markers') else 'core'


def brace_kinds(code):
    """[(position of `{`, its match, kind)]: kind ns (a named namespace), anon
    (an unnamed one), class (struct, class, union) or code."""
    out, stack, start = [], [], 0
    for m in re.finditer(r'[{};]', code):
        c = m.group()
        if c == '{':
            head = code[start:m.start()]
            if re.search(r'\bextern\s*""\s*$', head):
                kind = 'ns'
            elif re.search(r'\bnamespace\s*$', head):
                kind = 'anon'
            elif re.search(r'\bnamespace\s+[\w:]+\s*$', head):
                kind = 'ns'
            elif re.search(r'\b(?:struct|class|union)\b[^()]*$', head) and not re.search(r'\benum\b', head):
                kind = 'class'
            else:
                kind = 'code'
            stack.append((m.start(), kind))
        elif c == '}' and stack:
            b, kind = stack.pop()
            out.append((b, m.start(), kind))
        start = m.end()
    return out


def scope_at(kinds, pos):
    """The kinds of the braces around pos."""
    return [k for b, e, k in kinds if b < pos < e]


# Header declarations (drafter24): a function's key holds the declarations of
# its headers it can see, not the whole headers, so a new field in skater.h
# misses the functions that name Player (or reach it), not every function of
# the 80 files that include skater.h. A header is split into its top-level
# declarations (namespaces and extern "C" blocks entered); each is keyed by the
# names it declares: a type by its tag (an enum by its enumerators too) and the
# declarators after its body, a function by its name (and its class), an
# operator, a variable or a typedef by every name of its head, a macro
# invocation (an upper-case name in call position) by every name in it, a
# #define by the macro's name. Chunks keyed by nothing (#if lines, `using
# namespace`, anything not recognised) are always in. A function's
# declarations: those keyed by a name its closure's definitions and its files'
# residues use, then by any name an included declaration uses, to a fixpoint.
# Wrongly left out is the risk, so anything unclear is in (fewer hits only).

CXX_WORDS = set("""alignas alignof and asm auto bool break case catch char char8_t char16_t char32_t class const
    consteval constexpr constinit const_cast continue decltype default delete do double dynamic_cast else enum
    explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not
    nullptr operator or private protected public register reinterpret_cast requires return short signed sizeof
    static static_assert static_cast struct switch template this thread_local throw true try typedef typeid
    typename union unsigned using virtual void volatile wchar_t while final override __attribute__
    __restrict __restrict__ __inline __forceinline __declspec""".split())
KEYS_VERSION = 'decl7'      # in every function's digest: a change of the scheme is a new key (decl5: game views;
                            # decl6: paths in files and commands written alike in every worktree and Linux worker slot).
                            # A game's _OWN body in its function's key (tools7) is no new scheme: only the functions
                            # with an own body got a key part ("own OWN" and its definition), so they miss once and
                            # every other function's decl6 line still serves. decl7 (tools12): the frame table's
                            # locals' places out of the base, each function's in its key (Tree.locals_part): every key
                            # is new once, then a lane's locals rows miss only the functions they place.
IDS = re.compile(r'\b[A-Za-z_]\w*\b')
# A frame table row (ee/abi.h FrameRow): game, address, size, first save, saves, first local, locals.
FRAME_ROW = re.compile(r'\{(\d+)u, (0x[0-9a-fA-F]+)u, (0x[0-9a-fA-F]+)u, (\d+)u, (\d+)u, (\d+)u, (\d+)u\}')
GAME_NAMES = {0: 'ai', 1: 'dm2', 2: 'bmx'}    # (ee/game_id.h)


def ids_of(text):
    return IDS.findall(text)


def skeleton(text):
    """(text with every top-level {...} emptied, the bodies)."""
    out, bodies, depth, start = [], [], 0, 0
    for i, c in enumerate(text):
        if c == '{':
            if depth == 0:
                out.append(text[start:i + 1])
                start = i + 1
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                bodies.append(text[start:i])
                start = i
    out.append(text[start:])
    return ''.join(out), bodies


def drop_template_head(skel):
    m = re.match(r'\s*template\s*<', skel)
    if not m:
        return skel
    depth = 0
    for i in range(m.end() - 1, len(skel)):
        if skel[i] == '<':
            depth += 1
        elif skel[i] == '>':
            depth -= 1
            if depth == 0:
                return skel[i + 1:]
    return skel


TYPE_HEAD = re.compile(r'\s*(?:typedef\s+)?(?:(?:const|volatile|static|inline|constexpr|extern)\s+)*'
                       r'(struct|class|union|enum)\b(?:\s+(?:class|struct)\b)?'
                       r'(?:\s*(?:alignas|__attribute__|__declspec)\s*\((?:[^()]|\([^()]*\))*\))*\s*(\w+)?')
FN_BODY = re.compile(r'\)\s*(?:const\s*|noexcept\s*|override\s*|final\s*|&\s*|->\s*[\w:<>,\s*&]+?\s*)*'
                     r'(?::\s*[^{};]*)?$')


def drop_angles(text):
    """text without its template argument lists (<...>, nested)."""
    prev = None
    while prev != text:
        prev = text
        text = re.sub(r'<[^<>;{}()]*>', ' ', text)
    return text


def declarators(head):
    """The names a variable declaration declares: per comma-separated
    declarator, the name before its initialiser, array bounds or bit width."""
    head = drop_angles(head)
    out, depth, start = [], 0, 0
    parts = []
    for i, c in enumerate(head):
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == ',' and depth == 0:
            parts.append(head[start:i])
            start = i + 1
    parts.append(head[start:])
    for part in parts:
        part = re.split(r'[=\[;{]|(?<!:):(?!:)|\(\s*\)', part, maxsplit=1)[0] if not re.search(r'\(\s*\*', part) else part
        fp = re.search(r'\(\s*[*&]\s*(?:const\s*)?([A-Za-z_]\w*)\s*\)', part)   # a function pointer
        if fp:
            out.append(fp.group(1))
            continue
        names = [w for w in ids_of(part) if w not in CXX_WORDS]
        if names:
            out.append(names[-1])
    return set(out)


def chunk_keys(text):
    """The names a top-level declaration declares: a set, None (always in) or
    () (never: a static_assert changes no behaviour)."""
    skel, bodies = skeleton(text)
    skel = drop_template_head(skel)
    head = skel.split('{', 1)[0]
    words = set(w for w in ids_of(skel) if w not in CXX_WORDS)
    if re.match(r'\s*static_assert\b', skel):
        return ()
    if re.match(r'\s*using\s+namespace\b', skel) or re.match(r'\s*namespace\s+\w+\s*=', skel):
        return None
    m = TYPE_HEAD.match(skel)
    if m and '{' in skel and m.end() <= len(head) and not FN_BODY.search(head):
        keys = set()
        if m.group(2) and m.group(2) not in CXX_WORDS:
            keys.add(m.group(2))
        if bodies and m.group(1) == 'enum':
            for item in bodies[0].split(','):
                e = re.match(r'\s*([A-Za-z_]\w*)', item)
                if e:
                    keys.add(e.group(1))
        keys |= set(w for w in ids_of(skel.rsplit('}', 1)[-1]) if w not in CXX_WORDS)
        if skel.lstrip().startswith('typedef'):
            keys |= words
        return keys or None
    if m and '{' not in skel and '(' not in skel:
        return words or None      # a forward declaration
    if 'operator' in ids_of(head):
        return words or None
    u = re.match(r'\s*using\s+([A-Za-z_]\w*)\s*=', skel)
    if u:
        return {u.group(1)}
    u = re.match(r'\s*using\s+[\w:]*::\s*([A-Za-z_]\w*)\s*;', skel)
    if u:
        return {u.group(1)}
    if re.match(r'\s*typedef\b', skel):
        return declarators(re.sub(r'^\s*typedef\b', '', skel.rsplit('}', 1)[-1] if '}' in skel else skel)) or words or None
    calls = [c for c in re.finditer(r'([A-Za-z_]\w*)\s*\(', head)
             if c.group(1) not in CXX_WORDS and not c.group(1).startswith('__')]
    if calls:
        name = calls[0].group(1)
        before = head[:calls[0].start()]
        if '=' in before:
            return declarators(head) or words or None  # a variable initialised by a call
        if name.isupper():
            # a macro invocation: it may declare anything it is given
            return set(w for w in ids_of(text) if w not in CXX_WORDS) or None
        keys = {name}
        q = re.search(r'(\w+)\s*::\s*$', before)
        if q:
            keys.add(q.group(1))
        return keys
    return declarators(head) or words or None  # a variable


def match_brace(text, i):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def split_declarations(text, out):
    """The top-level declarations of a stripped header without its
    preprocessor lines, into out; False if its braces do not balance."""
    depth, start, j, n = 0, 0, 0, len(text)
    while j < n:
        c = text[j]
        if c == '{':
            if depth == 0:
                head = text[start:j]
                if re.search(r'\bnamespace(?:\s+[\w:]+)?\s*$', head) or re.search(r'\bextern\s*""\s*$', head):
                    e = match_brace(text, j)
                    if e < 0 or not split_declarations(text[j + 1:e - 1], out):
                        return False
                    j = start = e
                    continue
            depth += 1
        elif c == '}':
            depth -= 1
            if depth < 0:
                return False
            if depth == 0:
                k = j + 1
                while k < n and text[k] in ' \t\r\n':
                    k += 1
                head = drop_template_head(skeleton(text[start:j + 1])[0]).split('{', 1)[0]
                is_fn = (FN_BODY.search(head) and not TYPE_HEAD.match(head + ' ').group(1) if TYPE_HEAD.match(head + ' ')
                         else FN_BODY.search(head)) and '=' not in head.split('(')[0]
                if is_fn or k >= n or text[k] == ';':
                    end = k + 1 if k < n and text[k] == ';' else j + 1
                    if text[start:end].strip():
                        out.append(text[start:end])
                    j = start = end
                    continue
        elif c == ';' and depth == 0:
            if text[start:j + 1].strip():
                out.append(text[start:j + 1])
            start = j + 1
        j += 1
    if depth != 0:
        return False
    if text[start:].strip():
        out.append(text[start:])
    return True


# A registration's status (Draft, Tested, Verified, Planned) is not in the
# keys (decl4): it changes no replay, only the lines' status= field, which a
# cached line gets from the current registration (Keys.restatus). A
# promotion round's status flips then miss nothing.
STATUS_WORD = re.compile(r'\b(?:Verified|Tested|Draft|Planned)\b')
# READABLE_FUNCTION[_WAITS|_VU0]_OWN[_AT](...): the shared function FN whose body in GAME's instance is OWN (one pair or
# two): NAME, FN only in the _AT form, then STATUS, then GAME, OWN[, GAME2, OWN2].
OWN_REG = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?_OWN(_AT)?\s*\(([^;()]*)\)\s*;')
REGISTRATION = re.compile(r'\bREADABLE_[A-Z0-9_]+\s*\(([^;]*)\)\s*;?\s*$')
REG_WORDS = {'Verified', 'Tested', 'Draft', 'Planned', 'ai', 'dm2', 'bmx', 'sweep'}


def header_chunks(code):
    """[(keys, digest, names used)] of a stripped header: keys a frozenset, or
    None for a chunk always in. A header whose braces do not balance is one
    chunk, always in."""
    lines, rest, chunks, i = code.split('\n'), [], [], 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith('#'):
            full = line
            while full.rstrip().endswith('\\') and i + 1 < len(lines):
                i += 1
                full = full.rstrip()[:-1] + ' ' + lines[i]
            d = re.match(r'\s*#\s*define\s+(\w+)', full)
            if re.match(r'\s*#\s*(?:include|pragma\s+once)\b', full):
                chunks.append((None, full, frozenset()))   # names nothing (the file is its own header)
            else:
                chunks.append(({d.group(1)} if d else None, full, None))
            rest.append('')
        else:
            rest.append(line)
        i += 1
    pieces = []
    if not split_declarations('\n'.join(rest), pieces):
        text = ' '.join(code.split())
        return [(None, sha(text), frozenset(ids_of(text)))]
    for piece in pieces:
        reg = REGISTRATION.search(piece)
        if reg:
            # A readable registration (READABLE_FUNCTION(addr, name, status)
            # and its kin; a definition's leftover return type may precede
            # it): keyed by the names it registers, using none, so one draft's
            # registration does not pull in every other one through
            # READABLE_FUNCTION and its status word.
            names = set(w for w in ids_of(reg.group(1)) if w not in REG_WORDS)
            chunks.append((names or None, STATUS_WORD.sub('S', piece), frozenset()))
            continue
        keys = chunk_keys(piece)
        if keys != ():
            chunks.append((keys, piece, None))
    out = []
    for keys, piece, used in chunks:
        text = ' '.join(piece.split())
        out.append((frozenset(keys) if keys is not None else None, sha(text),
                    used if used is not None else frozenset(ids_of(text))))
    return out


def generated_dirs(cmake_cache):
    """The recompiled code's folders a build was configured with (PS2X_RUNNER_GENERATED_DIR, then
    PS2X_RUNNER_EXTRA_GENERATED_DIRS), as norm_path makes them: their files are labelled G<n>/."""
    out = []
    try:
        for line in open(cmake_cache, errors='replace'):
            m = re.match(r'^PS2X_RUNNER_(?:EXTRA_)?GENERATED_DIRS?:[^=]*=(.*)$', line.rstrip('\n'))
            if m:
                out += [norm_path(x) for x in m.group(1).split(';') if x.strip()]
    except OSError:
        pass
    return [x for x in out if x not in ('.', '')]


def linked_folders(cmake_cache):
    """[(alias, real)] for the folders a build's recompiled-code paths (PS2X_RUNNER_*GENERATED*, the VU1 code, the
    function list) reach through a link above their own last part, longest first: a Linux worker slot's sN/recomp is a link to the shared recomp/, so slot N's build commands name
    sN/recomp/generated where slot 1's name recomp/generated, and the base digest differed by slot (tools7: slot 7's
    replays missed every line slot 1's had cached). The commands are keyed with the real folder. A path whose own
    last part is the link (the Mac's recomp/generated) is left as it is, and so are its keys."""
    out = set()
    if ON_WINDOWS:
        return []
    try:
        lines = open(cmake_cache, errors='replace').read().splitlines()
    except OSError:
        return []
    for line in lines:
        m = re.match(r'^PS2X_(?:RUNNER_\w*GENERATED\w*|VU1_AOT_DIR|GHIDRA_FUNCTIONS_CSV):(?:PATH|FILEPATH|STRING)=(.*)$', line)
        for v in (m.group(1).split(';') if m else []):
            if not v.startswith('/'):
                continue
            d = os.path.dirname(os.path.abspath(v))
            while d not in ('/', ''):
                if os.path.islink(d):
                    out.add((d, os.path.realpath(d)))
                    break
                d = os.path.dirname(d)
    return sorted(out, key=lambda x: -len(x[0]))


def named_source(obj, deps):
    """The dependency the object's path names (CMakeFiles/<target>.dir/<source>.o), or None."""
    m = re.search(r'(?:^|/)CMakeFiles/[^/]+\.dir/(.+)\.(?:o|obj)$', obj.replace('\\', '/'))
    if m:
        tail = m.group(1)
        while tail.startswith('__/'):
            tail = tail[3:]
        tail = '/' + (tail.lower() if ON_WINDOWS else tail)
        for d in deps:
            if (d.lower() if ON_WINDOWS else d).endswith(tail):
                return d
    return None


def object_source(obj, deps):
    """The source an object was compiled from, among its dependencies: the one its path names
    (CMakeFiles/<target>.dir/<source>.o). Not the first dependency: on Linux the precompiled header's
    own source (cmake_pch.hxx.cxx) comes first in the record of every object that uses it."""
    m = re.search(r'(?:^|/)CMakeFiles/[^/]+\.dir/(.+)\.(?:o|obj)$', obj.replace('\\', '/'))
    if m:
        tail = m.group(1)
        while tail.startswith('__/'):
            tail = tail[3:]   # (CMake's name for a '..' in a source outside the target's folder)
        tail = '/' + (tail.lower() if ON_WINDOWS else tail)
        for d in deps:
            if (d.lower() if ON_WINDOWS else d).endswith(tail):
                return d
    for d in deps:
        if d.endswith(('.cpp', '.cc', '.c', '.cxx')) and '/cmake_pch' not in d:
            return d
    return deps[0]


LINE_DIRECTIVE = re.compile(r'^\s*#\s*line\s+\d+\s+"([^"]+)"')
LISTED = re.compile(r'^(?:([a-z0-9]+):)?0x([0-9a-f]{8})\s+(\w+)\s+(\S+)\s+(.*)$')
# A game's symbol header's level line (readable_symbols.py headers): sym::level[::game] { ... NAME = N; }
SYM_LEVEL = re.compile(r'namespace sym::level(?:::(\w+))? \{ inline constexpr int (\w+) = (\d+); \}')


def key_strip(text):
    """readable_digest.strip for the keys: comments out, and a string's or a character's contents kept as a
    digest ("0<hex>": no braces for the definitions' matching, no identifier), so a changed message or name in a
    literal is a changed function (readable_digest.strip keeps them as written, braces and words included)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append('%s0%s%s' % (c, sha(text[i + 1:j]) if j > i + 1 else '', c))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def ninja_key(path):
    """A path as ninja's logs name it (its canonical form: '/' separators)."""
    return path.replace('\\', '/')


def read_ninja_deps(path):
    """{output: [input, ...]} of a ninja dependency log (.ninja_deps, versions 3
    and 4), read only: records up to the first one that is cut short or not
    whole (a build appending meanwhile), the last record of an output winning,
    as ninja's own loader keeps them, but without truncating the file
    (Tree.deps). {} when there is none."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return {}
    sig = b'# ninjadeps\n'
    if not data.startswith(sig) or len(data) < len(sig) + 4:
        return {}
    version = struct.unpack_from('<i', data, len(sig))[0]
    if version not in (3, 4):
        return {}
    head = 12 if version == 4 else 8   # output id, mtime (64-bit from version 4)
    pos, names, deps = len(sig) + 4, [], {}
    while pos + 4 <= len(data):
        size = struct.unpack_from('<I', data, pos)[0]
        is_deps, size = size >> 31, size & 0x7FFFFFFF
        if size < 4 or pos + 4 + size > len(data):
            break   # cut short: a record still being written
        rec = data[pos + 4:pos + 4 + size]
        pos += 4 + size
        if is_deps:
            if size < head or (size - head) % 4:
                break
            out_id = struct.unpack_from('<i', rec, 0)[0]
            deps[out_id] = struct.unpack_from('<%di' % ((size - head) // 4), rec, head)
        else:
            # The path, NUL-padded to 4 bytes, then its checksum: ~id.
            if (~struct.unpack_from('<I', rec, size - 4)[0]) & 0xFFFFFFFF != len(names):
                break
            names.append(rec[:size - 4].rstrip(b'\0').decode('utf-8', 'replace'))
    out = {}
    for out_id, ids in deps.items():
        if 0 <= out_id < len(names) and all(0 <= i < len(names) for i in ids):
            out[ninja_key(names[out_id])] = [names[i] for i in ids]
    return out


class Tree:
    """The digests a build's lines depend on (the block comment above)."""

    def __init__(self, build, cache_dir, fncheck=None):
        sys.path.insert(0, HERE)
        import readable_digest as rd
        self.rd = rd
        self._deps_log = None
        self._unpath_re = None
        self.build = norm_path(os.path.abspath(build))
        if MAIN_BUILDS and self.build.startswith(MAIN_BUILDS):
            # (read only here, but the merges rebuild those: use your worktree's)
            log('note: %s is the main checkout\'s build, which the merges rebuild: point --fncheck/--build '
                'at your worktree\'s build' % self.build)
        self.fncheck = os.path.abspath(fncheck or built_checker(self.build))
        home = None
        for line in open(os.path.join(self.build, 'CMakeCache.txt'), errors='replace'):
            if line.startswith('CMAKE_HOME_DIRECTORY:'):
                home = line.split('=', 1)[1].strip()
            elif line.startswith('CMAKE_CXX_COMPILER:'):
                self.compiler = line.split('=', 1)[1].strip()
        self.home = norm_path(home) if home else home
        # (the built tree's runtime folder: runtime, or ps2xRuntime in a tree from before tools17's renames; the build
        # dir's own folders kept the old names in both, B/ps2xRuntime/...)
        self.rt = zlayout.folder(home, 'runtime') if home else 'runtime'
        self.generated = generated_dirs(os.path.join(self.build, 'CMakeCache.txt'))
        self.links = linked_folders(os.path.join(self.build, 'CMakeCache.txt'))
        os.makedirs(cache_dir, exist_ok=True)
        # (one file a key scheme: a file's digest is made as the scheme reads it, key_strip since decl5, and a
        # digest remembered from another scheme by size and mtime would be that scheme's)
        self.digest_cache_path = os.path.join(cache_dir, 'file-digests-%s.json' % KEYS_VERSION)
        try:
            self.fcache = json.load(open(self.digest_cache_path))
        except (OSError, ValueError):
            self.fcache = {}
        t0 = time.time()
        self.lv = rd.readable_symbols.literal_view   # (a game's instance: its own symbol headers, Tree.view)
        self.views = {}
        self.listed = None
        self._objects()
        self._sources(self.readable_objs)
        self._base()
        self.save_digests()
        self.seconds = round(time.time() - t0, 1)
        self._impl = {}
        self.closure = {}
        os.makedirs(cache_dir, exist_ok=True)

    def save_digests(self):
        # (several runs share a cache folder, one per Linux worker slot, each with its own paths: the file read again under
        # a lock and this run's digests merged into it, so a run that saved since this one loaded keeps its own;
        # then a file of this process's own, moved over it. Every entry is checked by size and mtime when used.)
        with open(self.digest_cache_path + '.lock', 'a+') as lock:
            lock_file(lock)
            try:
                merged = json_or_empty(self.digest_cache_path)
                merged.update(self.fcache)
                write_replace(self.digest_cache_path, merged)
            finally:
                unlock_file(lock)

    def compiled_sources(self):
        """{object: the source its compile command compiles} of the build (ninja -t compdb), as the deps name files."""
        out = {}
        try:
            for e in json.loads(self.ninja('-t', 'compdb')):
                if e.get('output') and e.get('file'):
                    out[e['output'].replace('\\', '/')] = self.abs(os.path.join(e.get('directory') or self.build, e['file']))
        except (subprocess.CalledProcessError, ValueError, OSError):
            pass
        return out

    def ninja(self, *args):
        return subprocess.run(['ninja', '-C', self.build] + list(args), capture_output=True, text=True,
                              check=True).stdout

    def label(self, path):
        """A file as the digests name it: relative to the source tree (H/) or
        the build (B/), so two worktrees' or builds' keys agree (drafter23:
        absolute paths made every worktree miss)."""
        if '/_deps/' in path:
            return 'D/' + path.split('/_deps/', 1)[1]   # the fetched dependencies, wherever fetched
        for i, g in enumerate(self.generated):
            if path.startswith(g + '/'):
                return 'G%d/%s' % (i, path[len(g) + 1:])   # the recompiled code (PS2X_RUNNER_*GENERATED_DIR*)
        if path.startswith(self.build + '/'):
            return 'B/' + path[len(self.build) + 1:]
        if self.home and path.startswith(self.home + '/'):
            return 'H/' + path[len(self.home) + 1:]
        return path

    def verdict_input(self, path):
        """Whether a file of the checker's own build can change a replay's
        lines: the checker and the readable model (src/game), every
        runtime header (the traced originals' instruction macros, the
        runtime's types), and the runtime code a replay runs (memory, the
        runtime's registers and COP0, the syscalls a function's own code
        makes). Not the rest of the runtime (GS, audio, IOP, movies, SDL),
        which ps2xFnCheck links but no replay runs (drafter23: any merge that
        touched one of those 3,449 files missed the whole cache)."""
        # (Windows: the labels are case folded, as norm_path makes the paths; the names here likewise, tools8: the Windows
        # worker's base had 4 files of the Linux worker's 132, so a runtime change there would have missed no cached line)
        name = self.label(path).lower() if ON_WINDOWS else self.label(path)
        if name.startswith('G') and name.endswith(('.h', '.hpp')):
            return True   # the recompiled code's headers the traced originals include (a recompiler change)
        if name.startswith(_ci('B/ps2xRuntime/fncheck_build/')):
            # the tables ps2xFnCheck and the readable functions are built with (fncheck_traced_gen.py: the callees'
            # stack arguments, the frames; from the generated code and readable_locals*, readable_args_dead*.tsv),
            # not the digests' table, which only names impl=
            return not name.endswith('/readable_digests.cpp')
        if not name.startswith(_ci('H/%s/' % self.rt)):
            return False
        rel = name[len('H/%s/' % self.rt):]
        if rel.endswith(('.h', '.hpp', '.inl', '.inc')):
            return rel.startswith(_ci(('include/', 'src/game/', 'src/lib/Kernel/Syscalls/'))) or rel in _ci(RUNTIME_HEADERS)
        return rel.startswith(_ci(('src/game/', 'src/lib/Kernel/Syscalls/'))) or rel in _ci(RUNTIME_SOURCES)

    def source_folder(self, sub):
        """The tree's folder of a build subfolder: the build dirs kept the old names (ps2xRuntime, ps2xTest,
        ps2xIOP) when tools17 renamed the tree's (runtime, tests, iop)."""
        head, _, rest = sub.replace('\\', '/').partition('/')
        for kind, (new, old) in zlayout.FOLDERS.items():
            if head.lower() == old.lower():
                return '/'.join(x for x in (zlayout.folder(self.home, kind), rest) if x)
        return sub

    def abs(self, path):
        return norm_path(path if os.path.isabs(path) else os.path.join(self.build, path))

    def is_system(self, path):
        """The compiler's and the SDK's own files (outside the tree, the build
        and the fetched dependencies): pinned by the compiler's version and
        the compile commands in the base, not digested one by one. On the Mac
        exactly the SDK and the toolchain (SYSTEM); portable."""
        return not (path.startswith(self.build + '/') or (self.home and path.startswith(self.home + '/')) or
                    '/_deps/' in path or any(path.startswith(g + '/') for g in self.generated))

    def deps(self, objects):
        """{object: [dependency, ...]} from the build's dependency log, read
        here, not with `ninja -t deps`: ninja's loader truncates a log whose
        last record is half written, which is how it looks while a build of
        that directory appends to it, and the build's next records then
        follow a cut one, so ninja drops every record from there (the
        coordinator, drafter25: the damaged out/build-ai log at the start of
        every merge build; drafter22 saw 310 of 360 objects without records).
        Nothing here writes the build's files."""
        if self._deps_log is None:
            self._deps_log = read_ninja_deps(os.path.join(self.build, '.ninja_deps'))
        log = self._deps_log
        out = {}
        for o in objects:
            found = log.get(ninja_key(o))
            out[o] = [self.abs(x) for x in found] if found else []
        return out

    def scan_deps(self, objects):
        """{object: [dependency, ...]} from the compiler's -M scan of each object's
        own compile command (the source first, as ninja's deps log has it)."""
        cmds = {}
        for i in range(0, len(objects), 200):
            for line in self.ninja('-t', 'commands', '-s', *objects[i:i + 200]).split('\n'):
                args = shlex.split(line) if line.strip() else []
                if '-o' not in args or '-c' not in args:
                    continue
                obj = args[args.index('-o') + 1]
                if args and os.path.basename(args[0]) == 'ccache':
                    args = args[1:]
                out, skip = [], 0
                for k, a in enumerate(args):
                    if skip:
                        skip -= 1
                        continue
                    if a == '-o' or a in ('-MT', '-MF', '-MQ'):
                        skip = 1
                        continue
                    if a in ('-MD', '-MMD'):
                        continue
                    out.append(a)
                cmds[obj] = out + ['-M']

        def one(item):
            obj, args = item
            try:
                p = subprocess.run(args, capture_output=True, text=True, cwd=self.build, timeout=300)
            except (OSError, subprocess.TimeoutExpired):
                return obj, []
            if p.returncode != 0:
                return obj, []
            text = p.stdout.replace('\\\n', ' ')
            deps = text.split(':', 1)[1].split() if ':' in text else []
            return obj, [self.abs(d) for d in deps]

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            return dict(pool.map(one, sorted(cmds.items())))

    def _objects(self):
        objs = []
        for line in self.ninja('-t', 'targets', 'all').split('\n'):
            t = line.split(':')[0]
            if t.endswith('.o') or t.endswith('.obj'):
                objs.append(t)
        inputs = set(self.abs(x) for x in self.ninja('-t', 'inputs', FNCHECK_TARGET).split('\n') if x)
        # (only those the checker links: on Windows fiber_posix.cpp is a target
        # that is never built, so it has no dependency record)
        self.readable_objs = [o for o in objs if '/ps2_game_readable.dir/' in o and self.abs(o) in inputs]
        # Another game's instance (game.cmake: ps2_game_readable_<game>, the shared engine compiled again with that
        # game's symbol headers, and its own folder): its readable files, keyed as that game's (Tree.view).
        self.game_objs = collections.defaultdict(list)
        for o in objs:
            m = re.search(r'/ps2_game_readable_([a-z0-9]+)\.dir/', o.replace('\\', '/'))
            if m and self.abs(o) in inputs:
                self.game_objs[m.group(1)].append(o)
        in_games = set(o for v in self.game_objs.values() for o in v)
        traced = sorted(o for o in objs if '/ps2_game_traced' in o)
        # (the checker's other objects: those its link takes, not every input ninja lists for it. `ninja -t inputs`
        # follows order-only edges too: the header units (game_headers.cmake: ps2_game_headers, one unit per
        # readable header, an order-only dependency of the readable objects) are built before them and linked into
        # nothing, and through them every readable header was whole in the base, so a change to any one moved every
        # key of every game: pace1, b127 and b128's whole-corpus replays. A readable header is in a function's key
        # as the chunks its closure reaches (header_part).)
        linked = self.linked_objects()
        others = [o for o in objs if o not in self.readable_objs and o not in in_games and '/ps2_game_traced' not in o
                  and self.abs(o) in inputs and (linked is None or self.abs(o) in linked) and 'readable_digests.cpp' not in o]
        sample = traced[::max(1, len(traced) // 30)] + traced[-1:]
        d = self.deps(self.readable_objs + sorted(in_games) + others + sample)
        self.readable_deps = {o: d.get(o, []) for o in self.readable_objs + sorted(in_games)}
        # (clang-cl's /showIncludes, the Windows worker's deps: an object's record names its includes, never its own source,
        # so object_source took its first header, ee/abi.h, for 201 of AI's 393 readable objects, every plain source's
        # copy (readable_entries/), and _sources counted them as the engine's: their bodies were in no key and a changed
        # body was served its old lines (tools28: read3's registerMotionBlurEffect from int22's, impl df1e9db3 for
        # 08cd30d0). The source from the object's compile command (ninja -t compdb), first in its record.)
        lacking = [o for o, ds in self.readable_deps.items() if ds and named_source(o, ds) is None]
        if lacking:
            own = self.compiled_sources()
            for o in lacking:
                src = own.get(o.replace('\\', '/'))
                if src:
                    self.readable_deps[o] = [src] + self.readable_deps[o]
        self.traced_headers = set()
        for o in sample:
            self.traced_headers |= set(x for x in d.get(o, []) if '/fncheck_traced' not in x)
        self.base_files = set(self.traced_headers)
        # The checker's own objects bring their includes into the base only when a replay runs their code: their
        # source is a verdict input (fncheck_main, ee/*.cpp, the replay's runtime code), or unknown or the build's
        # own (a unity or precompiled-header source: in, as anything unclear). The rest of the runtime (GS, audio,
        # IOP, movies, SDL, the panel) is linked but never run by a replay, its sources already out of the base:
        # a header only they include is code only they contain (an inline is compiled where it is included), so
        # it cannot change a replay's lines either (pace1: every present1 or panel header change, include/runtime/
        # gs/*.h, panel/*.h, moved every key of every game). A header both kinds include stays in.
        run = self.run_objects(others, d)
        self.base_dropped = set()
        for o in others:
            if o in run:
                self.base_files |= set(d.get(o, []))
        for o in others:
            if o not in run:
                self.base_dropped |= set(x for x in d.get(o, []) if x not in self.base_files)
        # (the tables ps2xFnCheck is built with: readable_frames.cpp is compiled into the readable objects)
        fb = os.path.join(self.build, 'ps2xRuntime', 'fncheck_build')
        self.base_files |= set(norm_path(os.path.join(fb, n)) for n in (os.listdir(fb) if os.path.isdir(fb) else ())
                               if n.endswith('.cpp') and n != 'readable_digests.cpp')
        missing = [o for o in self.readable_deps if not self.readable_deps[o]]
        if missing:
            # The build's deps log has no record for them: a .ninja_deps cut short
            # (ninja drops every record after a damaged one; its loader truncates a
            # half-written last record, so a `ninja -t deps` run while that build
            # was writing the log damages it: out/build-ai, 310 of 360). Their
            # includes from the compiler instead (its -M scan of the object's own
            # command), so the cache works on any build.
            log('%d readable objects have no record in %s/.ninja_deps: scanning their includes with the compiler'
                % (len(missing), self.build))
            scanned = self.scan_deps(missing)
            for o in missing:
                self.readable_deps[o] = scanned.get(o, [])
            still = [o for o in missing if not self.readable_deps[o]]
            # An object whose source includes nothing in this build (on
            # Windows fiber_posix.cpp, all #if'd out: clang-cl names no
            # include, so ninja keeps no record): its source alone.
            for o in list(still):
                m = re.match(r'^(.*?)/CMakeFiles/[^/]+\.dir/(.+?)\.(?:o|obj)$', o.replace('\\', '/'))
                src = os.path.join(self.home, self.source_folder(m.group(1)), m.group(2)) if m and self.home else None
                if src and os.path.exists(src):
                    self.readable_deps[o] = [norm_path(src)]
                    still.remove(o)
            if still:
                raise SystemExit('no dependency log for %d readable objects, and the compiler scan failed for them '
                                 '(%s ...): build %s first' % (len(still), still[0], self.build))
        # The binary must be newer than every source it was built from.
        mt = os.stat(self.fncheck).st_mtime
        newer = [f for f in set().union(self.base_files, *self.readable_deps.values())
                 if not self.is_system(f) and os.path.exists(f) and os.stat(f).st_mtime > mt]
        if newer:
            raise SystemExit('%s is older than %d of its sources (%s ...): build it first' % (
                self.fncheck, len(newer), newer[0]))

    def linked_objects(self):
        """The objects the checker's link takes (absolute paths): its link edge's explicit and implicit inputs, and
        those of the archives among them, to any depth; not its order-only inputs (built first, linked into nothing).
        None when ninja cannot say (every input then, as before)."""
        out, seen = set(), set()
        todo = [os.path.relpath(self.fncheck, self.build).replace('\\', '/')]
        try:
            while todo:
                target = todo.pop()
                if target in seen:
                    continue
                seen.add(target)
                text = self.ninja('-t', 'query', target)
                inputs = re.search(r'^  input: [^\n]*\n((?:    [^\n]*\n)*)', text, re.M)
                if not inputs:
                    return None
                for line in inputs.group(1).splitlines():
                    words = line.split()
                    if not words or words[0] == '||':
                        continue
                    path = words[-1]
                    if path.endswith(('.o', '.obj')):
                        out.add(self.abs(path))
                    elif path.endswith(('.a', '.lib')):
                        todo.append(path)
        except (subprocess.CalledProcessError, OSError):
            return None
        return out

    def run_objects(self, objects, deps):
        """The objects of the checker's link (not the readable or traced ones) whose code a replay can run: their
        source is a verdict input, or not a source of the tree (the build's generated, unity or precompiled-header
        sources), or not known. The source from the object's record, else (clang-cl's /showIncludes names none) from
        its compile command."""
        own = None
        out = set()
        for o in objects:
            src = named_source(o, deps.get(o, []))
            if src is None:
                if own is None:
                    own = self.compiled_sources()
                src = own.get(o.replace('\\', '/'))
            if src is None or not self.label(src).startswith('H/') or self.verdict_input(src):
                out.add(o)
        return out

    def unpath(self, text):
        """A code file's text as the keys read it: the build's, the tree's and the linked folders' paths in it written
        alike in every worktree and Linux worker slot (a generated readable_entries file's #line names its source by its
        absolute path, so the Linux worker's slot 7 missed every line slot 1 had cached; tools7)."""
        for alias, real in self.links:
            text = text.replace(alias + '/', real + '/')
        if ON_WINDOWS:
            # (the Windows worker: a path written with either separator and in any case, as norm_path folds them; the
            # slots' build dirs out\slotN-GAME and checkouts wt\slotN share a game's cache by this, tools8)
            if self._unpath_re is None:
                pats = [(self.build, '$B/')] + ([(self.home, '$H/')] if self.home else [])
                self._unpath_re = [(re.compile(re.escape(p).replace('/', r'[/\\]') + r'[/\\]', re.I), name)
                                   for p, name in pats]
            for rx, name in self._unpath_re:
                text = rx.sub(name, text)
            return text
        text = text.replace(self.build + '/', '$B/')
        if self.home:
            text = text.replace(self.home + '/', '$H/')
        return text

    def code_text(self, path):
        return self.unpath(open(path, encoding='utf-8', errors='replace').read())

    def file_digest(self, path):
        try:
            st = os.stat(path)
        except OSError:
            return 'missing'
        c = self.fcache.get(path)
        if c and c[0] == st.st_size and c[1] == st.st_mtime_ns:
            return c[2]
        data = open(path, 'rb').read()
        if path.endswith(CODE_EXT):
            text = self.unpath(data.decode('utf-8', 'replace'))
            dg = sha(self.rd.normal(key_strip(text)))
        else:
            dg = sha(data)
        self.fcache[path] = [st.st_size, st.st_mtime_ns, dg]
        return dg

    def _sources(self, objs, game='ai'):
        """Per readable file: its definitions, their callees, the rest of the
        file and its headers. A definition in another file is reachable only
        through a project header both files include that names it (C++ needs
        the declaration) or a declaration in the caller's own file; the
        engine's support code (ee/*.cpp: the checker's registry, fibers,
        traces) is part of the base. AI's registrations from the sources
        (literal_view: AI's addresses); another game's from the binary
        (Tree.view)."""
        rd = self.rd
        self.defs = collections.defaultdict(list)       # (file, name) -> [digest of a definition]
        self.same = collections.defaultdict(set)        # (file, name) -> identifiers (readable_digest's closure)
        self.cross = collections.defaultdict(set)       # (file, name) -> names it calls or takes the address of
        self.where = collections.defaultdict(set)       # name -> files defining it that others can call
        self.qual = {}                                  # (file, name) -> class or namespace it is defined as a member of
        self.regs = collections.defaultdict(list)       # (game, addr) -> [(file, fn name or None, macro text)]
        self.context = {}                               # file -> digest of its residue
        self.status = {}                                # (game, addr) -> its registration's status (lower case)
        self.hchunks = {}                               # header -> [(keys or None, digest, names used)]
        self.by_key = collections.defaultdict(list)     # name -> [(header, chunk index)]
        self.headers = {}                               # file -> its project headers
        self.residue_ids = {}                           # file -> identifiers outside its definitions
        hids = {}
        ee_dir = norm_path(self.home + '/' + self.rt + '/src/game/ee') + '/'   # (case folded on Windows, as src is)
        self.sources = {}                               # the source a registration names (__FILE__) -> the file compiled
        for o in objs:
            deps = self.readable_deps[o]
            src = object_source(o, deps)
            heads = set(f for f in deps if f != src and not self.is_system(f))
            if src.startswith(ee_dir):
                self.base_files |= set(deps)
                continue
            text = self.code_text(src)
            # (a plain source's copy names its source in its first line, as the registrations' __FILE__ does)
            named = LINE_DIRECTIVE.match(text[:4096])
            # (code_text wrote the tree's path as $H/ in that line: the registrations' __FILE__ is the real path, whose
            # label is H/. Looked up as "$H/..." since decl6 (63c5f26b), no game's registration found its plain
            # source's copy, so no body of a plain source was in a DM2 or BMX key: eng1's drawSky came from the cache
            # with ZGE2's step changed; tools7b)
            named = named and re.sub(r'^\$([HB])/', lambda m: (self.home if m.group(1) == 'H' else self.build) + '/', named.group(1))
            self.sources[self.label(norm_path(named)) if named else src] = src
            raw = self.lv(text)
            code = key_strip(raw)
            kinds = brace_kinds(code)
            spans = []
            for m in rd.IDENT_CALL.finditer(code):
                name = m.group(1)
                if name in rd.KEYWORDS or name.isupper():
                    continue
                q = rd.match(code, m.end() - 1, '(', ')')
                if q < 0:
                    continue
                t = DEF_TAIL.match(code, q)
                if not t or t.end() - q > 400:
                    continue
                e = rd.match(code, t.end() - 1, '{', '}')
                if e < 0:
                    continue
                text = rd.normal(code[m.start():e])
                self.defs[(src, name)].append(sha(text))
                self.same[(src, name)] |= set(rd.IDENT.findall(text))
                self.cross[(src, name)] |= set(x or y for x, y in CALLED.findall(REPLAYED.sub(' ', text)))
                # Callable from another file: not a member of a class of this
                # file, not static, not in an unnamed namespace.
                scope = scope_at(kinds, m.start())
                lead = code[max(code.rfind(';', 0, m.start()), code.rfind('}', 0, m.start()),
                                code.rfind('{', 0, m.start())) + 1:m.start()]
                if not ('class' in scope or 'anon' in scope or 'code' in scope or re.search(r'\bstatic\b', lead)):
                    self.where[name].add(src)
                    q = re.search(r'(\w+)\s*::\s*$', lead)
                    if q:
                        self.qual[(src, name)] = q.group(1)
                spans.append((m.start(), e))
            spans.sort()
            rest, at = [], 0
            for b, e in spans:
                if b >= at:
                    rest.append(code[at:b])
                    at = e
            rest.append(code[at:])
            residue = rd.normal(''.join(rest))
            self.residue_ids[src] = set(rd.IDENT.findall(residue))
            self.headers[src] = heads
            for h in heads:
                if h not in hids and h.endswith(CODE_EXT):
                    try:
                        hids[h] = set(rd.IDENT.findall(key_strip(self.lv(self.code_text(h)))))
                    except OSError:
                        hids[h] = set()
            self.context[src] = sha(residue)
            for h in heads:
                self.header_index(h)
            # The file's own declarations outside its definitions (its
            # registrations, constants, tables, local types), split and keyed
            # as a header's are (decl2: a status flip or a new constant in a
            # sweep batch file missed every draft of the file).
            self.residue_index(src, ''.join(rest))
            if game != 'ai':
                continue
            for m in ANY_REG.finditer(raw):
                game = m.group(1) or 'ai'
                self.regs[(game, int(m.group(2), 16))].append((src, None, STATUS_WORD.sub('S', rd.normal(m.group(0)))))
            for m in rd.REG.finditer(raw):
                self.status[('ai', int(m.group(2), 16))] = m.group(4).lower()
            for m in rd.REG_IN.finditer(raw):
                self.status[(m.group(1), int(m.group(2), 16))] = m.group(4).lower()
            for m in rd.REG.finditer(raw):
                self.regs[('ai', int(m.group(2), 16))].append((src, m.group(3).split('::')[-1], ''))
            for m in rd.REG_IN.finditer(raw):
                self.regs[(m.group(1), int(m.group(2), 16))].append((src, m.group(3).split('::')[-1], ''))
        self.declared = collections.defaultdict(set)    # name -> project headers declaring it (`name(`)
        self.hids = hids
        for h in hids:
            try:
                text = key_strip(self.lv(self.code_text(h)))
            except OSError:
                continue
            for x in set(rd.IDENT_CALL.findall(text)):
                if x in self.where:
                    self.declared[x].add(h)
        if game != 'ai':
            self._listed_registrations(game)
        else:
            self._listed_bodies()

    def listed_registrations(self):
        """[(game, addr, status, name, file, notes)] of every registration in the binary (ps2xFnCheck --list):
        what a game's instance registers at that game's addresses, at the level and status its symbol headers
        (from the bind table) give, which the sources alone do not say."""
        if self.listed is None:
            try:
                p = subprocess.run([self.fncheck, '--list'], capture_output=True, text=True, timeout=600)
            except (OSError, subprocess.TimeoutExpired) as e:
                raise SystemExit('run: %s --list failed (%s): the cache cannot key another game\'s functions' % (self.fncheck, e))
            if p.returncode != 0:
                raise SystemExit('run: %s --list exited %d: the cache cannot key another game\'s functions'
                                 % (self.fncheck, p.returncode))
            self.listed = []
            # (game, addr) -> its registrations as --list gives them, in the registry's order: readable::find takes
            # an address's first, so that is the body its replay runs (a game folder's own body, an _OWN form, a
            # shared function the bind puts at the same address). Tree.closure_digest keys the order of an address
            # registered more than once.
            self.listed_order = collections.defaultdict(list)
            for line in p.stdout.splitlines():
                m = LISTED.match(line)
                if m:
                    rest = m.group(5).split('  (', 1)
                    row = (m.group(1) or 'ai', int(m.group(2), 16), m.group(3), m.group(4).split('::')[-1],
                           rest[0].strip(), '(' + rest[1] if len(rest) > 1 else '')
                    self.listed.append(row)
                    self.listed_order[row[:2]].append('%s@%s' % (row[3], self.label(listed_path(self.home, row[4]))))
        return self.listed

    def _listed_registrations(self, game):
        """A game view's registrations: each of the game's rows in --list, its body the definition of that name
        (or the plain copy's NAME_) in the file the registration names; a Planned one runs no C++ (level 1, or
        recorded only): its row alone. The level, name and notes (callees that wait, no traced original) are in
        the key, the status is not (Keys.restatus), as AI's."""
        for g, addr, status, name, file, notes in self.listed_registrations():
            if g != game:
                continue
            src = self.sources.get(self.label(listed_path(self.home, file)))
            # (Planned is a status, not "no body": a corpus build's --try-close registers a level 1 body at level 2
            # with its status row's status, Planned for DM2's zfloatToText and vectorToText, and the replay runs it;
            # tools6: their cached faults outlived Node::valueAs's DM2 layout. The level its symbol header gives.)
            planned = status == 'planned' and (src is None or self.instance_level(game, src, name) in (0, 1))
            text = '%s:%s %s %s' % (game, name, 'planned' if planned else 'body', notes)
            self.regs[(g, addr)].append((src or self.label(listed_path(self.home, file)), None, text))
            # (an address listed twice: the first registration is the one the replay checks and its lines name,
            # DM2's allocateSized (Planned) before heapAllocate, BMX's pointerArrayCount before cursorArrayCapacity)
            if (g, addr) not in self.status:
                self.status[(g, addr)] = status
            if src and not planned:
                own = self.own_bodies(src).get((name, game))
                if own:
                    # (the game's own body: its definition in src, else the files defining it that others can call)
                    files = [src] if (src, own) in self.defs or (src, own + '_') in self.defs else \
                        sorted(f for f in self.where.get(own, ()) if (f, own) in self.defs)
                    self.regs[(g, addr)].append((src, None, '%s:%s own %s' % (game, name, own)))
                    for f in files:
                        for n in (own, own + '_'):      # (the plain copy's entry and its body OWN_)
                            if (f, n) in self.defs:
                                self.regs[(g, addr)].append((f, n, ''))
                    if not files:
                        # (no definition found: the source file whole, so a change to it is a change of the key)
                        self.regs[(g, addr)].append((src, None, 'own %s unfound %s' % (own, self.file_digest(src))))
                    continue
                for n in (name, name + '_'):
                    if (src, n) in self.defs:
                        self.regs[(g, addr)].append((src, n, ''))

    def own_bodies(self, src):
        """{(NAME, game): OWN} of the _OWN registrations in src (OWN_REG): the body game's instance runs for NAME. The
        --list row names NAME, whose own definition is AI's body; the game's is OWN (tools7: d7zgui's DM2 replay at
        fda2ff5b served defineListedMenu's and defineListedTreeView's changed bodies from the cache, the key having
        AI's defineGuiMenuProperties in their place)."""
        if not hasattr(self, '_own'):
            self._own = {}
        if src not in self._own:
            out = {}
            try:
                code = key_strip(self.code_text(src))
            except OSError:
                code = ''
            for m in OWN_REG.finditer(code):
                args = [a.strip().split('::')[-1] for a in m.group(2).split(',')]
                name, pairs = (args[0], args[3:]) if m.group(1) else (args[0], args[2:])
                for i in range(0, len(pairs) - 1, 2):
                    out[(name, pairs[i])] = pairs[i + 1]
            self._own[src] = out
        return self._own[src]

    def instance_level(self, game, src, name):
        """The level game's instance registers name at in src: what the symbol headers src includes from
        B/symbols/<game>/ say (sym::level, or sym::level::<game> for an _ONLY function; 2 a body, 1 an entry, 0 none),
        the highest if several do; None if none says (then its body is in the key: unclear is in)."""
        root = norm_path(os.path.join(self.build, 'ps2xRuntime', 'symbols', game)) + '/'
        if not hasattr(self, '_levels'):
            self._levels = {}
        found = None
        for h in self.headers.get(src, ()):
            if not h.startswith(root):
                continue
            if h not in self._levels:
                lv = {}
                try:
                    for m in SYM_LEVEL.finditer(open(h, encoding='utf-8', errors='replace').read()):
                        if m.group(1) in (None, game):
                            lv[m.group(2)] = max(lv.get(m.group(2), 0), int(m.group(3)))
                except OSError:
                    pass
                self._levels[h] = lv
            if name in self._levels[h]:
                found = max(found or 0, self._levels[h][name])
        return found

    def _listed_bodies(self):
        """AI's registrations the sources' forms do not name a body for (the _OWN form, a plain copy's
        renamed one): the body --list names, in the file it names."""
        for g, addr, status, name, file, notes in self.listed_registrations():
            if g != 'ai' or status == 'planned' or any(r[1] for r in self.regs.get((g, addr), ())):
                continue
            src = self.sources.get(self.label(listed_path(self.home, file)))
            for n in (name, name + '_'):
                if src and (src, n) in self.defs:
                    self.regs[(g, addr)].append((src, n, 'ai:%s body %s' % (n, notes)))

    def view(self, game):
        """The tree as game's instance compiles it (ps2_game_readable_<game>: the shared sources and the game's own
        folder, with the symbol headers of B/symbols/<game>/ that its bind table makes): the same digests over those
        objects, their sources read as they are (the symbols are the headers' chunks in the key, not AI's
        addresses), the registrations from the binary. AI's, or a game this build has no instance of: the tree."""
        if game == 'ai' or game not in self.game_objs:
            return self
        v = self.views.get(game)
        if v is None:
            t0 = time.time()
            self.listed_registrations()   # (once, shared by the views)
            v = copy.copy(self)
            v.lv = lambda text: text
            v.views, v.game_objs, v._impl, v.closure = {}, {}, {}, {}
            v._sources(self.game_objs[game], game)
            self.status.update(v.status)
            self.views[game] = v
            self.save_digests()
            log('%s\'s instance: %d readable files, %d registrations (%.1f s)' % (
                game, len(self.game_objs[game]), sum(1 for k in v.regs if k[0] == game), time.time() - t0))
        return v

    def _base(self):
        files = sorted(f for f in self.base_files if not self.is_system(f) and self.verdict_input(f))
        self.frame_locals = None
        parts = sorted('%s=%s' % (self.label(f), self.frames_digest(f) if self.is_frame_table(f) else self.file_digest(f))
                       for f in files)
        cmds = self.commands()
        try:
            cc = subprocess.run([self.compiler, '--version'], capture_output=True, text=True).stdout
        except OSError:
            cc = self.compiler
        self.base = sha(cc, *(sorted(cmds) + parts))
        self.base_count = len(files)

    def env_names(self):
        """The environment's names the key's sources read (ENV_NAME: a "PS2X_..." or "ZAXIS_..." literal in a base
        file or a readable object's source or header, any game's): replay_env keys those no table classes."""
        if getattr(self, '_env_names', None) is None:
            names = set()
            files = set(self.base_files).union(*self.readable_deps.values())
            for f in files:
                if self.is_system(f) or not f.endswith(CODE_EXT):
                    continue
                try:
                    data = open(f, 'rb').read()
                except OSError:
                    continue
                if b'"PS2X_' in data or b'"ZAXIS_' in data:
                    names |= set(n.decode() for n in ENV_NAME.findall(data))
            self._env_names = names
        return self._env_names

    def is_frame_table(self, path):
        return self.label(path) == _ci('B/ps2xRuntime/fncheck_build/readable_frames.cpp')

    def frames_digest(self, path):
        """The frame table's part of the base (fncheck_traced_gen.py frame_table: each scanned function's frame size,
        register saves and call sites, from the generated code), without its locals' places (readable_locals*.tsv):
        those are each function's own, in the keys of the functions whose bodies run with that frame (locals_part;
        tools12: every lane's locals row made a new base, so every lane replay missed the whole cache). The places
        by function into self.frame_locals; a table this does not read is whole in the base."""
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            return 'missing'

        def table(name):
            return re.search(r'\b%s\[\]\s*=\s*\{(.*?)\};' % name, text, re.S)
        m, r = table('kLocals'), table('kRows')
        places, rows = m and m.group(1), r and r.group(1)
        found = rows and FRAME_ROW.findall(rows)
        if places is None or not found:
            return self.file_digest(path)
        places = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+|\b0\b', places)]
        out = {}
        for row in found:
            first, count = int(row[5]), int(row[6])
            if count:
                if first + count > len(places):
                    return self.file_digest(path)
                out[(GAME_NAMES.get(int(row[0]), row[0]), int(row[1], 16))] = tuple(places[first:first + count])
        self.frame_locals = out
        # (the rest as written, the places and each row's index into them out)
        shape = FRAME_ROW.sub(lambda r: '{%s}' % ', '.join(r.groups()[:5]), text[:m.start(1)] + text[m.end(1):])
        return 'frames:' + sha(re.sub(r'\A(?:\s*//[^\n]*\n)+', '', shape))

    def locals_part(self, game, addr, seen):
        """The locals' places (the frame table's, from readable_locals*.tsv) of the functions whose bodies can run in a
        replay of game's addr: its own and those of the registered bodies its closure reaches (a body's ee::Local
        places are its frame row's: ee/abi.cpp, frameLayout of its registration). [] without a frame table."""
        if self.frame_locals is None:
            return []
        if getattr(self, '_reg_addrs', None) is None or self._reg_addrs[0] != game:
            by = collections.defaultdict(set)
            for (g, a), regs in self.regs.items():
                if g == game:
                    for r in regs:
                        if r[1]:
                            by[(r[0], r[1])].add(a)
            self._reg_addrs = (game, by)
        addrs = {addr}
        for node in seen:
            addrs |= self._reg_addrs[1].get(node, set())
        return ['locals %x=%s' % (a, ','.join('%x' % p for p in self.frame_locals[(game, a)]))
                for a in sorted(addrs) if (game, a) in self.frame_locals]

    def commands(self):
        """ps2xFnCheck's build commands, paths and file lists made alike."""
        cmds = set()
        for line in self.ninja('-t', 'commands', FNCHECK_TARGET).split('\n'):
            # (the paths of the tree and the build named alike in every worktree;
            # the fetched dependencies wherever they are: the main checkout's
            # build-ai takes them from out/build-macos/_deps, a worktree's
            # build from its own, and the base differed for that alone)
            if 'readable_digest.py' in line:
                continue  # writes readable_digests.cpp, which no replay reads
            # (a command CMake's Ninja generator put in a script because it is long, on Windows: `<output>-<hash>.bat
            # <the script's digest>`. That digest is of the script's text, which names this build's and tree's paths,
            # so two slots' builds of one commit had two bases: DM2's and BMX XXX's symbol headers, with their
            # --instance lists, tools41. The script's text in its place, made alike below as any other line.)
            m = re.match(r'(\S+\.bat) [0-9a-f]{16}$', line.strip())
            if m:
                try:
                    with open(os.path.join(self.build, m.group(1)), errors='replace') as script:
                        line = ' '.join([m.group(1)] + [x.strip() for x in script.read().splitlines() if x.strip()])
                except OSError:
                    pass
            line = re.sub(r'(?:/[^\s]*?)?/_deps/', '/$D/', line)
            for alias, real in self.links:
                line = line.replace(alias + '/', real + '/')
            # (a path with a '..' in it as the file it names: a build dir configured before a CMakeLists change
            # keeps PS2X_GHIDRA_FUNCTIONS_CSV as recomp/generated/../ghidra-functions.csv, a new one has
            # recomp/ghidra-functions.csv, and the Linux worker's slots differed by that alone; tools7)
            line = re.sub(r'\S*/\.\./\S*', lambda m: posixpath.normpath(m.group(0)), line)
            if ON_WINDOWS:
                line = line.replace('\\', '/')
                low = line.lower()   # (case-blind: as norm_path)
                for path, name in ((self.build, '$B'), (self.home, '$H')):
                    while path and path in low:
                        i = low.index(path)
                        line, low = line[:i] + name + line[i + len(path):], low[:i] + name + low[i + len(path):]
            line = line.replace(self.build, '$B')  # (first: the build is often inside the tree)
            if self.home:
                line = line.replace(self.home, '$H')
            line = re.sub(r'(-o|-c|-MF|-MT|--list|--out-dir|--spans|--stack-args|--cpp)\s+\S+', r'\1 _', line)
            # (a plain copy is compiled with -iquote <its source's folder>: a readable file in a new folder was a
            # new command, so a new base, and the lane missed the batch's whole fill (b7game 1ef62506, four new
            # game/bmx folders: 0 of 64,432 files from the Linux worker's cache; tools8). The folder is the file's own, and
            # what it includes from there is in its closure (the dependency log).)
            line = re.sub(r'(-iquote)\s*\S+', r'\1 _', line)
            if ON_WINDOWS:
                # (clang-cl has no -iquote: CMake gives the plain copy's folder as an -I below src/game)
                line = re.sub(r'(?<!\S)-I\$H/(?:ps2x)?runtime/src/game/\S+', '-I_', line, flags=re.I)   # (either name: tools17)
            if ON_WINDOWS:
                # (clang-cl's /Fo, /Fd, /Fe: as -o on the Mac)
                line = re.sub(r'(?<!\S)([/-]F[ode])\S+', r'\1_', line)
            line = re.sub(r'\S+\.(?:cpp|c|o|d|stamp|list|a|obj|lib|pdb)\b', '_', line)
            # A list of files (the link line's objects) as one: a new readable
            # file is not a new checker (drafter24: every added file missed all)
            cmds.add(re.sub(r'_(?:\s+_)+', '_...', line))
        return cmds

    def header_index(self, h):
        """The header's declarations (header_chunks), indexed by their keys."""
        if h in self.hchunks:
            return
        dg = self.file_digest(h)
        if h.endswith(CODE_EXT):
            try:
                code = key_strip(self.lv(self.code_text(h)))
            except OSError:
                code = None
            chunks = header_chunks(code) if code is not None else [(None, dg, frozenset())]
        else:
            chunks = [(None, dg, frozenset())]   # not code: whole, always in
        self.hchunks[h] = chunks
        for i, (keys, _, _) in enumerate(chunks):
            for k in keys or ():
                self.by_key[k].append((h, i))

    def residue_index(self, src, code):
        """A readable file's residue as declarations, under ('R', src)."""
        key = ('R', src)
        chunks = header_chunks(code)
        self.hchunks[key] = chunks
        for i, (keys, _, _) in enumerate(chunks):
            for k in keys or ():
                self.by_key[k].append((key, i))

    def header_part(self, files, names):
        """The digests of the declarations of the files' residues and headers
        that names (and what those declarations name in turn) can see."""
        heads = set(('R', f) for f in files if ('R', f) in self.hchunks)
        for f in files:
            heads |= self.headers.get(f, set())
        seen, todo, out = set(names), list(names), set()

        def take(h, i):
            if (h, i) in out:
                return
            out.add((h, i))
            for x in self.hchunks[h][i][2]:
                if x not in seen:
                    seen.add(x)
                    todo.append(x)
        for h in heads:
            for i, (keys, _, _) in enumerate(self.hchunks[h]):
                if keys is None:
                    take(h, i)
        while todo:
            for h, i in self.by_key.get(todo.pop(), ()):
                if h in heads:
                    take(h, i)
        return sorted('%s:%s' % ('R:' + self.label(h[1]) if isinstance(h, tuple) else self.label(h),
                                 self.hchunks[h][i][1]) for h, i in out)

    def reachable(self, src, name):
        """The files whose definitions of name a definition in src can run."""
        out = set()
        if (src, name) in self.defs:
            out.add(src)
        for f in self.where.get(name, ()):
            if f == src:
                continue
            shared = self.declared[name] & self.headers[src] & self.headers[f]
            q = self.qual.get((f, name))
            if q:
                shared = set(h for h in shared if q in self.hids.get(h, ()))
            if shared or name in self.residue_ids[src]:
                out.add(f)
        return out

    def impl(self, game, addr):
        return self.view(game).closure_digest(game, addr)

    def closure_digest(self, game, addr):
        k = (game, addr)
        if k in self._impl:
            return self._impl[k]
        regs = self.regs.get(k, [])
        files = set(r[0] for r in regs)
        seen, todo = set(), [(r[0], r[1]) for r in regs if r[1]]
        while todo:
            node = todo.pop()
            if node in seen or node not in self.defs:
                continue
            seen.add(node)
            src = node[0]
            for n in self.same[node]:
                if (src, n) in self.defs and (src, n) not in seen:
                    todo.append((src, n))
            for n in self.cross[node]:
                for f in self.reachable(src, n):
                    if (f, n) not in seen:
                        todo.append((f, n))
        defs, names = [], set()
        for node in sorted(seen):
            files.add(node[0])
            defs += ['%s:%s=%s' % (self.label(node[0]), node[1], d) for d in self.defs[node]]
            names |= self.same[node]
        for r in regs:
            if r[1]:
                names.add(r[1])     # (its registration names it)
        # (an address registered more than once: which registration is first, as --list has them; the parts above
        # are sorted, and two registrations whose order alone changed ran the other body under the same key, tools40.
        # An address registered once has no such part: its key is as before)
        order = getattr(self, 'listed_order', {}).get(k, ())
        order = ['|order'] + list(order) if len(order) > 1 else []
        v = sha(*([KEYS_VERSION] + sorted(r[2] for r in regs) + ['|'] + sorted(defs) + ['|'] +
                  self.header_part(files, names) + ['|'] + self.locals_part(game, addr, seen) + order))
        self._impl[k] = v
        self.closure[k] = (seen, files)
        return v

    def orig(self, game, addr):
        folder = 'fncheck_traced' if game == 'ai' else 'fncheck_traced_' + game
        traced = os.path.join(self.build, 'ps2xRuntime', folder, 'traced_%x.cpp' % addr)
        try:
            text = open(traced, 'rb').read()
        except OSError:
            return 'none'
        m = re.search(rb'#include "(blocks/[^"]+)"', text)
        blocks = open(os.path.join(os.path.dirname(traced), m.group(1).decode()), 'rb').read() if m else b''
        return sha(text, blocks)


class VerdictCache:
    """Lines per (file, part, key) in an SQLite file (WAL: several runs may
    share it)."""

    def __init__(self, folder):
        import sqlite3
        import zlib
        self.zlib = zlib
        os.makedirs(folder, exist_ok=True)
        self.db = sqlite3.connect(os.path.join(folder, 'verdicts.sqlite'), timeout=600, check_same_thread=False)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('CREATE TABLE IF NOT EXISTS lines (key TEXT PRIMARY KEY, relaxed TEXT, file TEXT, '
                        'part TEXT, data BLOB, created REAL, used REAL)')
        self.db.execute('CREATE INDEX IF NOT EXISTS lines_relaxed ON lines (relaxed)')
        self.db.commit()
        self.hits = []
        global KEY_SIZES
        KEY_SIZES = KeySizes(self.db)

    def get(self, key):
        r = self.db.execute('SELECT data FROM lines WHERE key=?', (key,)).fetchone()
        if r:
            self.hits.append(key)
        return json.loads(self.zlib.decompress(r[0])) if r else None

    def get_relaxed(self, relaxed):
        r = self.db.execute('SELECT data, key FROM lines WHERE relaxed=? ORDER BY created DESC LIMIT 1',
                            (relaxed,)).fetchone()
        if r:
            self.hits.append(r[1])
        return json.loads(self.zlib.decompress(r[0])) if r else None

    def gc(self, days):
        """Entries no run used for days: deleted."""
        n = self.db.execute('DELETE FROM lines WHERE used < ?', (time.time() - days * 86400,)).rowcount
        self.db.commit()
        self.db.execute('VACUUM')
        return n

    def put_many(self, rows):
        KEY_SIZES.save()
        now = time.time()
        self.db.executemany('INSERT OR REPLACE INTO lines VALUES (?,?,?,?,?,?,?)',
                            [(k, rel, f, p, self.zlib.compress(json.dumps(ls, sort_keys=True).encode(), 6), now, now)
                             for k, rel, f, p, ls in rows])
        self.db.commit()

    def touch(self, keys):
        KEY_SIZES.save()
        now = time.time()
        self.db.executemany('UPDATE lines SET used=? WHERE key=?', [(now, k) for k in keys])
        self.db.commit()


class KeySizes:
    """Each recording's fnrec_io.key_size, kept in the verdict cache's database by the file's size and mtime: the
    size is read from a file's head, and reading every head took 92 s for BMX XXX's 109,304 files on the Windows
    worker (their stat 11 s), each run."""

    def __init__(self, db):
        self.db = db
        self.lock = threading.Lock()
        db.execute('CREATE TABLE IF NOT EXISTS key_sizes (path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, '
                   'key_size INTEGER)')
        db.commit()
        self.known = {p: (s, m, k) for p, s, m, k in db.execute('SELECT path, size, mtime, key_size FROM key_sizes')}
        self.new = {}

    def get(self, path, st):
        k = self.known.get(path)
        if k and k[0] == st.st_size and k[1] == st.st_mtime_ns:
            return k[2]
        size = fnrec_io.key_size(path, st)
        with self.lock:
            self.known[path] = self.new[path] = (st.st_size, st.st_mtime_ns, size)
        return size

    def save(self):
        with self.lock:
            rows, self.new = self.new, {}
        if rows:
            self.db.executemany('INSERT OR REPLACE INTO key_sizes VALUES (?,?,?,?)',
                                [(p,) + v for p, v in rows.items()])
            self.db.commit()


KEY_SIZES = None   # (the run's verdict cache's: VerdictCache)


def recording_size(path):
    """What the replay's order and choices go by: a recording's plain size from the verdict cache's key sizes (as
    before the corpus was compressed), or without a cache its size on disk (a head read per file was minutes of a
    --no-cache run's start on the Windows worker's hard disk)."""
    return KEY_SIZES.get(path, os.stat(path)) if KEY_SIZES else os.path.getsize(path)


def file_id(path):
    """A recording's key: its path, plain size (fnrec_io.key_size: a file compressed in place, its mtime kept by
    fnrec_pack.py, keys as it did plain) and mtime."""
    st = os.stat(path)
    return '%s|%d|%d' % (path, KEY_SIZES.get(path, st) if KEY_SIZES else fnrec_io.key_size(path, st), st.st_mtime_ns)


class Snapshot:
    """A tree's digests saved by `digests` (a build's state kept for a later
    cache-import, after its sources moved on)."""

    def __init__(self, path):
        d = json.load(open(path))
        self.base = d['base']
        self.fns = {tuple(k.split(':')[0:1]) + (int(k.split(':')[1], 16),): v for k, v in d['functions'].items()}

    def impl(self, game, addr):
        return self.fns[(game, addr)][0]

    def orig(self, game, addr):
        return self.fns[(game, addr)][1]


def frame_index_name(path):
    """A recording's folder in the index of leftover reads (ps2xFnCheck frameIndexName): <corpus>__<folder>."""
    folder = os.path.dirname(os.path.abspath(path))
    return os.path.basename(os.path.dirname(folder)) + '__' + os.path.basename(folder)


def make_frame_index(a, files, tree=None):
    """run --frame-index: the index of every folder's leftover reads (each file's original alone), and
    {folder's index name: digest of its reads}. With the verdict cache's tree, a folder's index is kept in
    CACHE/frame-index under the folder's recordings (file_id) and their functions' generated originals and the
    checker's base: a folder whose key is there is not replayed (a regenerated original or a new recording
    makes it again)."""
    index = os.path.abspath(a.frame_index)
    if os.path.isdir(index):
        for n in os.listdir(index):
            if n.endswith('.reads'):
                os.remove(os.path.join(index, n))
    os.makedirs(index, exist_ok=True)
    t0 = time.time()
    by_route = collections.defaultdict(list)
    for route, path in files:
        by_route[route].append(path)
    store = os.path.join(a.cache, 'frame-index') if tree is not None else None
    folder_key, todo, kept = {}, {}, 0
    for route, paths in by_route.items():
        name = frame_index_name(paths[0])
        if store is None:
            todo[route] = paths
            continue
        parts = [tree.base]
        for p in sorted(paths):
            m = FNREC.match(os.path.basename(p))
            parts += [file_id(p), tree.orig(m.group(1) or 'ai', int(m.group(2), 16))]
        folder_key[route] = (name, sha(*parts))
        got = os.path.join(store, folder_key[route][1] + '.reads')
        if os.path.exists(got):
            shutil.copyfile(got, os.path.join(index, name + '.cache.reads'))
            kept += 1
        else:
            todo[route] = paths
    chunks = [(route, paths[i:i + a.chunk]) for route, paths in todo.items() for i in range(0, len(paths), a.chunk)]
    chunks.sort(key=lambda c: -sum(recording_size(p) for p in c[1]))
    flags = ['--frame-reads-out', index]
    faults = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futs = [pool.submit(run_chunk, a.fncheck, route, paths, a.timeout, flags) for route, paths in chunks]
        for fut in concurrent.futures.as_completed(futs):
            faults += sum(1 for f in fut.result() if f['kind'] == 'fault')
    data = collections.defaultdict(list)
    size = 0
    for n in sorted(os.listdir(index)):
        if n.endswith('.reads'):
            with open(os.path.join(index, n), 'rb') as f:
                lines = f.read().splitlines()
            data[n.rsplit('.', 2)[0]] += lines
            size += sum(len(x) + 1 for x in lines)
    digests = dict((name, sha(*sorted(lines))) for name, lines in data.items())
    if store is not None and not STOPPING.is_set():
        os.makedirs(store, exist_ok=True)
        for route in todo:
            name, key = folder_key[route]
            path = os.path.join(store, key + '.reads')
            with open(path + '.%d.tmp' % os.getpid(), 'wb') as f:
                f.write(b''.join(x + b'\n' for x in sorted(data.get(name, []))))
            os.replace(path + '.%d.tmp' % os.getpid(), path)
    log('frame index: %d folders (%d from the cache), %.1f MB, %d faults, in %.0f s -> %s' % (
        len(by_route), kept, size / 1048576.0, faults, time.time() - t0, index))
    return digests


class Keys:
    """Cache keys of one run: per file and part, exact and relaxed (without
    the checker base: for --extras-changed)."""

    def __init__(self, tree, extra, frame_index=None, env=None):
        self.tree = tree
        self.core, self.parts = split_flags(extra)
        # (the replay's switches, ENV_KEYED: this environment's, or the run's own for cache-import; none set, the
        # flags and so the keys are as before)
        if env is None:
            env, unclassified = replay_env(tree.env_names() if hasattr(tree, 'env_names') else ())
            if env:
                log('keys: the replay\'s environment %s%s' % (' '.join(env), ' (%s: in no table of the environment, '
                    'keyed as unclear)' % ', '.join(unclassified) if unclassified else ''))
        self.env = list(env)
        tail = ['env:' + e for e in self.env]
        self.core = self.core + tail
        self.full = ' '.join(CORE_FLAGS + extra + tail)
        self.fn = {}
        self.frame_index = frame_index   # run --frame-index: {folder's index name: digest}

    def function(self, path):
        m = FNREC.match(os.path.basename(path))
        k = (m.group(1) or 'ai', int(m.group(2), 16))
        if k not in self.fn:
            self.fn[k] = (self.tree.impl(*k), self.tree.orig(*k))
        return k, self.fn[k]


    def restatus(self, path, lines):
        """Cached lines with the function's current status (the key leaves it out)."""
        status = getattr(self.tree, 'status', {}).get(self.function(path)[0])
        if not status or lines is None:
            return lines
        return [dict(l, status=status) if 'status' in l and l['status'] != status else l for l in lines]

    def of(self, path):
        """{part: (exact, relaxed)} with part `all` for the whole file."""
        fid = file_id(path)
        _, (impl, orig) = self.function(path)
        if self.frame_index is not None:
            fid = (fid, self.frame_index.get(frame_index_name(path), '-'))
        out = {'all': (sha(fid, impl, orig, self.tree.base, 'all', self.full), None),
               'core': (sha(fid, impl, orig, self.tree.base, 'core', ' '.join(self.core)), None)}
        for part, flags in self.parts.items():
            f = ' '.join(self.core + flags)
            out[part] = (sha(fid, impl, orig, self.tree.base, part, f), sha(fid, impl, orig, part, f))
        return out


def cmd_digests(a):
    """Write the tree's digests for every function of the corpus (cache-import --digests)."""
    tree = Tree(a.build, a.cache, a.fncheck)
    fns = {}
    for route, path in corpus_files(a.corpus or [CORPUS]):
        m = FNREC.match(os.path.basename(path))
        k = (m.group(1) or 'ai', int(m.group(2), 16))
        if k not in fns:
            fns[k] = (tree.impl(*k), tree.orig(*k))
    json.dump({'base': tree.base, 'build': tree.build, 'functions': {'%s:0x%08x' % k: v for k, v in sorted(fns.items())}},
              open(a.out, 'w'), indent=0)
    log('%d functions\' digests (base %s) -> %s' % (len(fns), tree.base, a.out))


def is_timeout(f):
    """A fault line of a timeout (the runner's or ps2xFnCheck's own)."""
    return f.get('kind') == 'fault' and (f.get('phase') == 'timeout' or
                                         bool(re.search(r'\(timeout\b', f.get('phase') or '')))


def split_lines(lines):
    """{file: {part: [line, ...]}} and the files that faulted."""
    per = collections.defaultdict(lambda: collections.defaultdict(list))
    faulted = set()
    for f in lines:
        per[f['file']][part_of(f['kind'])].append(f)
        if f['kind'] == 'fault':
            faulted.add(f['file'])
    return per, faulted


# Result fields that measure the host's time, not what the replay found (tools20's probes: callee_ms, callee_index_ms):
# two replays of one file differ there on any host, so the cache's check of cached lines (--verify) leaves them out.
# With them in, every checked file of a run made with tools20's checker was a mismatch: read1's DM2 run at b121, 860 of
# 1,292 checked files "differ from their replay", all of them in these two fields alone (tools23).
HOST_TIME_FIELDS = ('callee_ms', 'callee_index_ms')


def cache_rows(keys, path, parts, faulted, wanted):
    """The cache rows of one replayed file (none for a timeout or the memory
    cap, the runner's or ps2xFnCheck's own: they depend on the machine and the
    cap, not only on the inputs; a runaway is the inputs' and is kept)."""
    allp = [l for p in parts.values() for l in p]
    if any(l.get('kind') == 'fault' and (l.get('phase') in ('timeout', 'memory') or
                                         re.search(r'\((timeout|memory)\b', l.get('phase') or ''))
           for l in allp):
        return []
    k = keys.of(path)
    if path in faulted:
        return [(k['all'][0], None, path, 'all', allp)] if set(wanted) == set(['core'] + list(keys.parts)) else []
    return [(k[p][0], k[p][1], path, p, parts.get(p, [])) for p in wanted]


def map_corpus(lines, spec):
    """lines with each file's corpus folder FROM written as TO (--map-corpus FROM=TO, '/' separators, FROM compared
    case-blind): a run made on another host over its copy of the corpus, imported into this host's cache under
    this host's recordings (the verdicts are the host's own: the Linux worker's checker and the Windows worker's one
    agreed on all 113,808 of DM2's files at batch 108, tools8). Files outside FROM are left out."""
    if not spec:
        return lines
    src, _, dst = spec.partition('=')
    src = src.replace('\\', '/').rstrip('/') + '/'
    dst = dst.replace('\\', '/').rstrip('/') + '/'
    out = []
    for f in lines:
        q = f.get('file', '').replace('\\', '/')
        if q.lower().startswith(src.lower()):
            out.append(dict(f, file=dst + q[len(src):]))
    return out


def cmd_cache_import(a):
    """Fill the cache from a full run's lines (run with this build's ps2xFnCheck, or another host's of the same
    commit with --map-corpus: its files named as this host's copy of the corpus has them)."""
    run = json.load(open(os.path.join(a.dir, 'run.json')))
    if a.digests:
        tree = Snapshot(a.digests)
    else:
        fncheck = a.fncheck if a.fncheck is not None else run['fncheck'] if not a.map_corpus else None
        tree = Tree(a.build, a.cache, fncheck)
        log('tree digests in %.1f s: base %s (%d files)' % (tree.seconds, tree.base, tree.base_count))
    lines = map_corpus(load(a.dir), a.map_corpus)
    if run.get('mode', 'semantic' if any(l.get('mode') == 'semantic' for l in lines if l.get('kind') == 'result') else 'cycles') == 'semantic':
        CORE_FLAGS[:] = SEMANTIC_FLAGS   # (a semantic run's keys, as `run --semantic` makes them; its args name no mode)
    keys = Keys(tree, run['args'], env=run.get('env', []))   # (the run's switches, not this process's)
    per, faulted = split_lines(lines)
    cache = VerdictCache(a.cache)
    wanted = ['core'] + list(keys.parts)
    # A file is imported only when this build registers its function as the run's lines name it (the coverage
    # lines' name against --list's): a run made with other build inputs (the bind table rebuilt since: bmx 0x219e88
    # freeDmmRegion at batch 109's fill, Planned bmx_219e88 two hours later) would put its lines under the keys of
    # a function that is no longer that one. A function the run names nowhere (every file a fault) is left to the
    # first replay. The build inputs should still be the run's (rcheck --import-cache --bind DIR).
    named, renamed = {}, set()
    if not a.digests:
        for l in lines:
            if l.get('kind') == 'coverage' and l.get('name') and l.get('address'):
                named[(l.get('game', 'ai'), int(l['address'], 16))] = l['name'].split('::')[-1]
        now = {}
        for g, addr, status, name, file, notes in tree.listed_registrations():
            now.setdefault((g, addr), name)
    rows, missing, skipped = [], 0, 0
    for path, parts in per.items():
        if not os.path.exists(path):
            missing += 1   # (a recording this host's corpus does not have yet)
            continue
        if not a.digests:
            k = keys.function(path)[0]
            if k not in named or named[k] != now.get(k):
                skipped += 1
                if k in named:
                    renamed.add('%s:0x%08x %s -> %s' % (k[0], k[1], named[k], now.get(k)))
                continue
        rows += cache_rows(keys, path, parts, faulted, wanted)
        if len(rows) > 5000:
            cache.put_many(rows)
            rows = []
    cache.put_many(rows)
    log('%d files imported into %s%s%s' % (len(per) - missing - skipped, a.cache,
                                           ', %d not in this corpus' % missing if missing else '',
                                           ', %d left to a replay (their function unnamed in the run, or registered '
                                           'under another name here: %d functions%s)' % (
                                               skipped, len(renamed), ': ' + '; '.join(sorted(renamed)[:8]) if renamed else '')
                                           if skipped else ''))


KIND_ORDER = {'result': 0, 'coverage': 1, 'fault': 2, 'safepoints': 3, 'markers': 4, 'build': 5}


def line_order(f):
    return (f.get('route', ''), f.get('file', ''), KIND_ORDER.get(f.get('kind'), 9), json.dumps(f, sort_keys=True))


def address_list(spec):
    """ADDR,... or @FILE (an address a line, the first word) as {(game, addr)}."""
    out = set()
    if not spec:
        return out
    words = []
    for item in spec.split(','):
        if item.startswith('@'):
            words += [l.split()[0] for l in open(os.path.expanduser(item[1:])) if l.strip() and not l.startswith('#')]
        elif item:
            words.append(item)
    for w in words:
        game, _, addr = w.rpartition(':')
        out.add((game or 'ai', int(addr, 16)))
    return out


def json_or_empty(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_replace(path, obj, **kw):
    """obj as JSON into path through a file of this process's own, moved over it: a reader never sees half a file."""
    tmp = '%s.%d.tmp' % (path, os.getpid())
    with open(tmp, 'w') as f:
        json.dump(obj, f, **kw)
    for attempt in range(50):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            # (Windows: a reader of the other slot has the file open for a moment; the move is asked again)
            if attempt == 49:
                raise
            time.sleep(0.1)


def checker_of(cache_dir, fncheck, lines):
    """The checker digest ps2xFnCheck prints (a hash of its bytes): from this
    run's result lines, else remembered per binary in the cache folder."""
    path = os.path.join(cache_dir, 'checkers.json')
    st = os.stat(fncheck)
    ident = '%s|%d|%d' % (os.path.abspath(fncheck), st.st_size, st.st_mtime_ns)
    for f in lines:
        if f.get('kind') == 'result' and f.get('checker'):
            if json_or_empty(path).get(ident) != f['checker']:
                # (under a lock, read again: the other slots' runs write their own binaries' digests here too)
                with open(path + '.lock', 'a+') as lock:
                    lock_file(lock)
                    try:
                        known = json_or_empty(path)
                        known[ident] = f['checker']
                        write_replace(path, known, indent=0)
                    finally:
                        unlock_file(lock)
            return f['checker']
    return json_or_empty(path).get(ident)


def built_checker(build):
    """The build's ps2xFnCheck (.exe on Windows)."""
    return os.path.join(build, 'ps2xRuntime', 'ps2xFnCheck' + ('.exe' if ON_WINDOWS else ''))


def same_build(fncheck, build):
    import filecmp
    built = built_checker(build)
    return os.path.abspath(fncheck) == os.path.abspath(built) or filecmp.cmp(fncheck, built, shallow=False)


PEAK_MB = 350          # ps2xFnCheck's peak resident memory a process (round corpusfast: 100 to 303 MB)
FREE_FLOOR = 0.30      # the share of memory left free: nothing more starts below 30% (the Mac's limit)


def auto_jobs(cap):
    """Processes that fit in the memory above the free floor (macOS
    memory_pressure's free share) at the memory cap each (a child may grow to
    it before it is killed), and in the cores the load leaves, at most cap."""
    if WINDOWS:
        return _win_auto_jobs(cap)
    try:
        out = subprocess.run(['memory_pressure'], capture_output=True, text=True).stdout
        free = int(re.search(r'free percentage: (\d+)%', out).group(1)) / 100.0
        total = int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout)
    except (OSError, AttributeError, ValueError):
        return 1
    per_job = max(MAX_MEM_BYTES, PEAK_MB << 20)
    by_memory = int((free - FREE_FLOOR) * total / per_job)
    cores = os.cpu_count() or 1
    try:
        load = os.getloadavg()[0]
    except OSError:
        load = 0.0
    by_cores = int(cores - load)
    return max(0, min(cap, by_memory, max(by_cores, 1 if by_memory > 0 else 0)))


# ---- the app's level of a line's function -----------------------------------
#
# A corpus build of DM2 or BMX XXX registers more bodies than the app runs: its symbol headers --try-close
# build a level 1 function's body (its code differs in that game: the app knows only its address) at level 2
# and Draft, so the replay can judge it. A result line's app_level is the level the app (the collection's
# build, no --try-close) registers the replayed function at: 2 its C++ body runs, 1 only its address is known,
# 0 not there. It is readable_symbols.py's Levels for the game, a shared source's only when the source is in
# the game's instance_sources.txt (as promote.py's app_levels), and 2 for a registration in the game's own
# folder; AI's are 2. The registration is the one the replay checks, the address's first in ps2xFnCheck
# --list. A census counts a verdict for the app only at app_level 2 (cov2 and cov4 counted BMX's
# freeAnimateRecord, level 1, as a Draft with a recording). Not in the verdict cache's lines: added as the
# run writes lines.jsonl, so the cache and its checks are as before.
def app_levels(fncheck, build):
    """{(game, addr): app level} of every registration the binary lists, or None (the log says why)."""
    try:
        home = None
        bind_dir = None
        for line in open(os.path.join(build, 'CMakeCache.txt'), errors='replace'):
            if line.startswith('CMAKE_HOME_DIRECTORY:'):
                home = line.split('=', 1)[1].strip()
            elif line.startswith('PS2X_BIND_DIR:'):
                bind_dir = line.split('=', 1)[1].strip()
        if not home:
            raise RuntimeError('no CMAKE_HOME_DIRECTORY in %s' % build)
        rt = os.path.join(home, zlayout.folder(home, 'runtime'))
        p = subprocess.run([fncheck, '--list'], capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            raise RuntimeError('%s --list exited %d' % (fncheck, p.returncode))
        first = {}
        for line in p.stdout.splitlines():
            m = LISTED.match(line)
            if m:
                first.setdefault((m.group(1) or 'ai', int(m.group(2), 16)),
                                 (m.group(4).split('::')[-1], m.group(5).split('  (', 1)[0].strip()))
        games = sorted(set(g for g, _ in first) - {'ai'})
        levels = {}
        if games:
            if not bind_dir:
                raise RuntimeError('no PS2X_BIND_DIR in %s' % build)
            sys.path.insert(0, os.path.join(rt, 'tools'))
            import readable_symbols as rs
            game_dir = os.path.join(rt, 'src', 'game')
            for g in games:
                lv = rs.Levels(rs.Bind(os.path.join(bind_dir, g + '.tsv')), rs.table(), game_dir, g)
                inst = os.path.join(game_dir, g, 'instance_sources.txt')
                instance = {l.strip() for l in open(inst) if l[:1].isalpha()} if os.path.exists(inst) else set()
                levels[g] = (lv, instance, norm_path(os.path.join(game_dir, g)) + '/')
        out = {}
        for (g, addr), (name, file) in first.items():
            if g == 'ai':
                out[(g, addr)] = 2
                continue
            lv, instance, own = levels[g]
            if listed_path(home, file).startswith(own):
                out[(g, addr)] = 2
            else:
                out[(g, addr)] = lv.level.get(name, 0) if lv.files.get(name) in instance else 0
        return out
    except Exception as e:  # (the levels are a census's aid, never a verdict: a run goes on without them)
        log('note: no app_level in the lines (%s: %s)' % (type(e).__name__, e))
        return None


def cmd_run(a):
    global MAX_MEM_BYTES
    MAX_MEM_BYTES = int(a.max_mem_gb * (1 << 30)) if a.max_mem_gb > 0 else 0
    for sig in [x for x in (signal.SIGTERM, signal.SIGINT, getattr(signal, 'SIGHUP', None)) if x is not None]:
        signal.signal(sig, lambda signum, _: (kill_all_children(), sys.exit(128 + signum)))
    try:
        return run_corpus(a)
    except BaseException:
        kill_all_children()  # Ctrl-C or an error: no child outlives the run
        raise


def run_corpus(a):
    # (Windows: Tree keeps its paths as norm_path makes them, '/' and case
    # folded, and reads the dependency log itself, so its keys are made as on
    # the Mac's; remote1 refused the cache there until a desktop test.)
    if a.jobs == 'auto':
        a.jobs = auto_jobs(a.max_jobs)
        if a.jobs < 1:
            raise SystemExit('run: free memory under %d %%: wait' % (FREE_FLOOR * 100))
        log('jobs auto: %d' % a.jobs)
    else:
        a.jobs = int(a.jobs)
    # (ADDR,... or @FILE: rcheck's jobs pass a long list by file, past what a Windows command line takes)
    only = set(addr for _, addr in address_list(a.only)) if a.only else None
    files = corpus_files(a.corpus or [CORPUS], only)
    os.makedirs(a.out, exist_ok=True)
    extra = []
    for opt in a.fncheck_arg or []:
        extra += opt.split()
    if a.semantic or '--semantic' in extra:
        extra = [x for x in extra if x != '--semantic']
        if set(extra) & {'--cycles', '--markers', '--safepoints'}:
            raise SystemExit('run: --semantic is the verdict without --cycles, --markers and --safepoints')
        CORE_FLAGS[:] = SEMANTIC_FLAGS
    t0 = time.time()
    use_cache = not a.no_cache
    cached = {}                     # file -> its lines from the cache
    need = {}                       # file -> parts to replay
    verify = set()
    keys = cache = tree = None
    if a.incremental or a.extras_changed:
        use_cache = True
    if use_cache:
        build = a.build or os.path.dirname(os.path.dirname(os.path.abspath(a.fncheck)))
        if not os.path.exists(os.path.join(build, 'CMakeCache.txt')):
            raise SystemExit('run: the cache needs the build ps2xFnCheck was built in (--build DIR, or --no-cache)')
        if not same_build(a.fncheck, build):
            raise SystemExit('run: %s is not the build\'s ps2xFnCheck (%s)' % (a.fncheck, build))
        tree = Tree(build, a.cache, a.fncheck)
    frame_index = None
    if a.frame_index:
        frame_index = make_frame_index(a, files, tree)
        extra += ['--frame-reads-in', os.path.abspath(a.frame_index)]
    core, extras = split_flags(extra)
    all_parts = ['core'] + list(extras)
    if use_cache:
        keys = Keys(tree, extra, frame_index)
        cache = VerdictCache(a.cache)
        log('tree digests in %.1f s (base %s, %d files)' % (tree.seconds, tree.base, tree.base_count))
        listed = address_list(a.extras_for)
        hits = collections.Counter()
        for route, path in files:
            k = keys.of(path)
            got = keys.restatus(path, cache.get(k['all'][0]) if a.incremental else None)
            if got is not None:
                cached[path] = got
                hits['all'] += 1
                continue
            lines, missing = [], []
            for part in all_parts:
                exact, relaxed = k[part]
                got = keys.restatus(path, cache.get(exact) if a.incremental else None)
                how = part
                if got is None and part != 'core' and a.extras_changed and keys.function(path)[0] not in listed:
                    got = keys.restatus(path, cache.get_relaxed(relaxed))
                    how = 'relaxed ' + part
                if got is None:
                    missing.append(part)
                else:
                    hits[how] += 1
                    lines += got
            if missing:
                need[path] = missing
                if len(missing) < len(all_parts):
                    cached[path] = lines          # the parts it has (the replay gives the rest)
            else:
                cached[path] = lines
        full = [p for p in cached if p not in need]
        if full and a.verify > 0:
            import random
            n = max(1, int(round(len(full) * a.verify)))
            verify = set(random.Random(a.seed).sample(sorted(full), min(n, len(full))))
            for p in verify:
                need[p] = list(all_parts)
        cache.touch(cache.hits)
        log('cache: %d files whole from the cache, %d to replay (%d of them a check of cached lines); hits %s' % (
            len(full) - len(verify), len(need), len(verify), dict(hits)))
    else:
        for route, path in files:
            need[path] = list(all_parts)
    route_of = dict((p, r) for r, p in files)
    groups = collections.defaultdict(list)
    for path in sorted(need):
        groups[(route_of[path], tuple(need[path]))].append(path)
    chunks = []
    for (route, parts), paths in groups.items():
        flags = core[len(CORE_FLAGS):] + [x for p in parts if p != 'core' for x in extras[p]]
        for i in range(0, len(paths), a.chunk):
            chunks.append((route, paths[i:i + a.chunk], flags, parts))
    # Longest first (the corpus' biggest routes), so the last chunks are short.
    chunks.sort(key=lambda c: -sum(recording_size(p) for p in c[1]))
    log('%d recordings in %d routes, %d to replay in %d chunks, %d jobs: %s' % (
        len(files), len(set(r for r, _ in files)), len(need), len(chunks), a.jobs, a.fncheck))
    out_path = os.path.join(a.out, 'lines.jsonl')
    replayed = []
    timed = []
    mismatches = []
    unstable = []
    # The writer makes its own folder, where it opens it, and says loudly when it had to: the Windows worker's
    # 1115-024539-editor1-bmx found the --out folder, made at the run's start, gone 155 s later (its lines.jsonl.part
    # could not be opened). What removed it is not known (tools31: not the unit tests nor this run, each reproduced
    # beside a sibling job's folder): a WARNING line, which job.ps1 and job.sh put in the job's status, names it.
    if not os.path.isdir(a.out):
        log('WARNING: %s, made at the run\'s start (%.0f s ago), was removed by something else: made again' % (
            a.out, time.time() - t0))
    os.makedirs(a.out, exist_ok=True)
    with open(out_path + '.part', 'w') as out, concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        stats_out = open(os.path.join(a.out, 'chunks.jsonl'), 'w')
        futs = {}
        for route, paths, flags, parts in chunks:
            st = []
            futs[pool.submit(run_chunk, a.fncheck, route, paths, a.timeout, flags, st)] = (st, paths, parts)
        for n, fut in enumerate(concurrent.futures.as_completed(futs)):
            st, paths, parts = futs[fut]
            got = fut.result()
            for line in got:
                out.write(json.dumps(line, sort_keys=True) + '\n')
            out.flush()
            replayed += got
            for rec in st:
                stats_out.write(json.dumps(rec, sort_keys=True) + '\n')
            stats_out.flush()
            if cache is not None:
                per, faulted = split_lines(got)
                rows = []
                for path in paths:
                    pl = per.get(path, {})
                    if path in verify:
                        old = cached.get(path, [])
                        new = [l for p in all_parts for l in pl.get(p, [])]
                        # (a fault line is the fault: its phase and crash details are the host's (an abort exits
                        # -6 on Linux, 0xC0000409 on Windows; a host crash's address), so a cache filled from
                        # another host's lines (cache-import --map-corpus, tools8) is checked on the verdicts)
                        strip_c = lambda ls, drop: sorted(json.dumps(dict((k, v) for k, v in l.items()
                                                                          if k not in drop and (l.get('kind') != 'fault' or k in ('kind', 'file', 'route'))),
                                                                     sort_keys=True) for l in ls)
                        if strip_c(old, ('checker',) + HOST_TIME_FIELDS) != strip_c(new, ('checker',) + HOST_TIME_FIELDS):
                            # Only the first differences' text: a replay that is not
                            # reproducible (a host address in a register), not a key.
                            texts = ('checker',) + HOST_TIME_FIELDS + ('first_original', 'first_readable', 'first_regs_left', 'first_args_left', 'first_entry_left', 'first_stale_left', 'first_handed_left',
                                     'first_saves', 'first_held', 'first_vu0_left')
                            # A function with no readable side (Planned) whose original's own replay differs: the
                            # recording, the traced original and the checker are its whole key, so the replay is
                            # not reproducible (DM2's sceDmaSync polls the DMA status), not a key that misses.
                            no_readable = all(l.get('readable_ok', '0') == '0' and l.get('readable_bad', '0') == '0'
                                              for l in old + new if l.get('kind') == 'result')
                            (unstable if strip_c(old, texts) == strip_c(new, texts) or no_readable
                             else mismatches).append(path)
                    rows += cache_rows(keys, path, pl, faulted, list(parts))
                cache.put_many(rows)
            if (n + 1) % 25 == 0:
                log('%d of %d chunks (%.0f s)' % (n + 1, len(chunks), time.time() - t0))
            for f in got:
                if is_timeout(f):
                    timed.append((route_of[f['file']], f['file'], parts))
    # A timeout depends on the machine (load, a sleep), not only the inputs:
    # each file that timed out is replayed once more alone, after the rest
    # (drafter24: batch 4's five, all passing alone); one that times out again
    # stays a fault with phase "... (timeout)" and is listed in run.json.
    retried, still = [], []
    for route, path, parts in timed:
        if STOPPING.is_set():
            break
        flags = core[len(CORE_FLAGS):] + [x for p in parts if p != 'core' for x in extras[p]]
        got = run_chunk(a.fncheck, route, [path], a.timeout, flags)
        if any(is_timeout(f) for f in got):
            still.append(path)
            continue
        retried.append(path)
        replayed = [f for f in replayed if f.get('file') != path] + got
        if cache is not None:
            per, faulted = split_lines(got)
            cache.put_many(cache_rows(keys, path, per.get(path, {}), faulted, list(parts)))
    if timed:
        log('timeouts: %d files timed out, replayed again alone: %d now replay to the end, %d time out again%s' % (
            len(timed), len(retried), len(still), (' (%s)' % ', '.join(still[:5])) if still else ''))
    # Every file's lines: replayed, else from the cache with this binary's
    # checker digest; a file replayed for some parts keeps the others' cached lines.
    by_file = collections.defaultdict(list)
    for f in replayed:
        by_file[f.get('file')].append(f)
    checker = None
    if cached:
        checker = checker_of(a.cache, a.fncheck, replayed)
        if checker is None:
            smallest = min(cached, key=recording_size)
            checker = checker_of(a.cache, a.fncheck, run_chunk(a.fncheck, route_of[smallest], [smallest], a.timeout, []))
    elif use_cache:
        checker_of(a.cache, a.fncheck, replayed)

    def from_cache(ls, skip=()):
        return [dict(l, checker=checker) if l.get('kind') == 'result' and checker else l
                for l in ls if part_of(l['kind']) not in skip]
    lines = []
    for route, path in files:
        if path in need:
            ls = by_file.get(path, [])
            if path not in verify and not any(l['kind'] == 'fault' for l in ls):
                ls = ls + from_cache(cached.get(path, []), set(need[path]) | {'core'})
        else:
            ls = from_cache(cached[path])
        lines += ls
    lines.sort(key=line_order)
    if any(l['kind'] == 'result' for l in lines):
        level = app_levels(a.fncheck, a.build or os.path.dirname(os.path.dirname(os.path.abspath(a.fncheck)))) or {}
        lines = [dict(l, app_level=level[(l.get('game') or 'ai', int(l['address'], 16))])
                 if l['kind'] == 'result' and 'address' in l and (l.get('game') or 'ai', int(l['address'], 16)) in level
                 else l for l in lines]
    with open(out_path + '.tmp', 'w') as out:
        for l in lines:
            out.write(json.dumps(l, sort_keys=True) + '\n')
    os.replace(out_path + '.tmp', out_path)
    os.remove(out_path + '.part')
    info = {'fncheck': a.fncheck, 'corpus': a.corpus or [CORPUS], 'files': len(files), 'seconds': round(time.time() - t0),
            'jobs': a.jobs, 'args': extra, 'replayed': len(need), 'from_cache': len(files) - len(need),
            'incremental': bool(a.incremental), 'extras_changed': bool(a.extras_changed),
            'mode': 'semantic' if CORE_FLAGS == SEMANTIC_FLAGS else 'cycles',
            'verify': {'files': len(verify), 'mismatches': mismatches, 'unstable': unstable},
            'timeouts': {'retried': retried, 'still': still}}
    # (the switches the run's replays had, keyed by cache-import: a run without the cache has no tree, so
    # ENV_KEYED's alone)
    info['env'] = keys.env if keys is not None else replay_env()[0]
    if keys is not None:
        info['base'] = keys.tree.base
    json.dump(info, open(os.path.join(a.out, 'run.json'), 'w'), indent=1)
    log('done: %d recordings (%d replayed) in %.0f s -> %s' % (len(files), len(need), time.time() - t0, out_path))
    if a.emit:
        # The work database's check and coverage results for this replay
        # (cmd_emit), so the sweep stages and path coverage follow the real
        # state (the coordinator, drafter25: corpus replays never reported).
        emit_results(a.out, a.emit, a.results)
    if unstable:
        log('note: %d cached files replay with other first-difference text (not reproducible: %s)' % (
            len(unstable), ', '.join(unstable[:5])))
    if mismatches:
        log('WARNING: %d cached files differ from their replay (the cache key misses an input): %s' % (
            len(mismatches), ', '.join(mismatches[:5])))
        return 2
    return replay_status(lines)


def replay_status(lines):
    """run's exit status: 1 if the readable side failed or faulted on any recording. The original's own failures are
    counted and said, not in the status: every full replay has some (61 files and 90 faults at batch 52), and a job's
    non-zero exit takes it out of the compare and the reference."""
    bad = [l for l in lines if l['kind'] == 'result' and int(l['readable_bad'])]
    original = sum(1 for l in lines if l['kind'] == 'result' and int(l['original_bad']))
    faults = [l for l in lines if l['kind'] == 'fault']
    readable_faults = [l for l in faults if not l.get('phase', '').startswith('recompiled original')]
    log('readable side: %d files fail, %d faults; original replay: %d files fail, %d faults' % (
        len(bad), len(readable_faults), original, len(faults) - len(readable_faults)))
    for l in (bad + readable_faults)[:10]:
        log('  %s %s' % (l['kind'], l['file']))
    return 1 if bad or readable_faults else 0


def cmd_cache_gc(a):
    n = VerdictCache(a.cache).gc(a.days)
    log('%d cache entries unused for %g days deleted from %s' % (n, a.days, a.cache))


def cmd_same(a):
    """Two runs' lines.jsonl as sets of lines: identical, or the differences."""
    drop = set((a.ignore or '').split(',')) - {''}
    roots = cross_roots(a.a, a.b)

    def lines(d):
        return collections.Counter(json.dumps(dict((k, v) for k, v in f.items() if k not in drop), sort_keys=True)
                                   for f in relabel(load(d), roots.get(d)))
    A, B = lines(a.a), lines(a.b)
    only_a, only_b = A - B, B - A
    print('lines: %d and %d; only in A %d, only in B %d' % (sum(A.values()), sum(B.values()),
                                                           sum(only_a.values()), sum(only_b.values())))
    for l in list(only_a)[:a.show]:
        print('A ' + l[:400])
    for l in list(only_b)[:a.show]:
        print('B ' + l[:400])
    return 0 if not only_a and not only_b else 1


def load(d):
    return [json.loads(l) for l in open(os.path.join(d, 'lines.jsonl'))]


def corpus_roots(d):
    """A run's corpus folders as `same` and `compare` match them ('/'
    separated, lower case), or None (no run.json, or a folder not absolute)."""
    try:
        roots = json.load(open(os.path.join(d, 'run.json'))).get('corpus') or []
    except (OSError, ValueError):
        return None
    out = [os.path.expanduser(r).replace('\\', '/').rstrip('/').lower() + '/' for r in roots]
    return out if out and all(re.match(r'^(/|[a-z]:/)', r) for r in out) else None


def cross_roots(da, db):
    """{run folder: its corpus folders} when two runs' folders differ (both
    known), else {}: the files keep their paths."""
    ra, rb = corpus_roots(da), corpus_roots(db)
    return {da: ra, db: rb} if ra and rb and ra != rb else {}


def relabel(lines, roots):
    """lines with each file named by its corpus folder's place in the run's
    list and its path below it ('0:career/00101dc8.fnrec'): two runs over
    copies of one corpus in other folders (the Mac's and the Windows worker's,
    remote1) compare file by file. Only when the runs' folders differ; a file
    outside them keeps its path."""
    if not roots:
        return lines
    for f in lines:
        q = f.get('file', '').replace('\\', '/')
        for i, r in enumerate(roots):
            if q.lower().startswith(r):
                f['file'] = '%d:%s' % (i, q[len(r):])
                break
    return lines


def key_of(f):
    return (f.get('game', 'ai'), int(f['address'], 16))


def old_counts(f):
    """(readable_ok, readable_bad) by the checks before round `coverage`: the
    calls only the new checks fail (new_bad) counted as passing."""
    n = int(f.get('new_bad', 0))
    return int(f['readable_ok']) + n, int(f['readable_bad']) - n


def file_verdict(f, ignore_new=False):
    """(original, readable) for one result line: ok, bad or none."""
    orig = 'bad' if int(f['original_bad']) else ('ok' if int(f['recorded']) else 'none')
    if f.get('readable') != '1':
        return orig, '-'
    bad = old_counts(f)[1] if ignore_new else int(f['readable_bad'])
    return orig, ('bad' if bad else ('ok' if int(f['recorded']) - int(f['skipped']) else 'none'))


def verdict_rank(v):
    """How a file's verdict stands for --no-worse: a fault or a failing side is worst."""
    if v[0] == 'fault' or 'bad' in v[:2]:
        return 0
    return 1 if 'none' in v[:2] else 2


def cmd_compare(a):
    roots = cross_roots(a.a, a.b)
    A, B = relabel(load(a.a), roots.get(a.a)), relabel(load(a.b), roots.get(a.b))
    if a.semantic:
        modes = collections.Counter(f.get('mode', 'old') for f in B if f['kind'] == 'result')
        if set(modes) - {'semantic'}:
            print('NOT a semantic run: %s: %s' % (a.b, dict(modes)))
            return 1

    def index(lines):
        r = {}
        for f in lines:
            if f['kind'] == 'result':
                r[f['file']] = f
            elif f['kind'] == 'fault':
                r.setdefault(f['file'], {'fault': f.get('phase', ''), 'file': f['file']})
        return r
    ia, ib = index(A), index(B)
    same = differ = not_run = 0
    flips = collections.Counter()
    better = collections.Counter()
    shown = 0
    for path in sorted(set(ia) | set(ib)):
        fa, fb = ia.get(path), ib.get(path)
        if fb is None and a.replayed:
            not_run += 1    # (--replayed: B replayed some files only, an --only run; A's others were not run)
            continue
        if fa is None or fb is None:
            differ += 1
            print('only in %s: %s' % ('A' if fb is None else 'B', path))
            continue
        if 'fault' in fa or 'fault' in fb:
            va = ('fault',) if 'fault' in fa else file_verdict(fa)
            vb = ('fault',) if 'fault' in fb else file_verdict(fb, a.ignore_new)
        else:
            va = file_verdict(fa) + (fa['recorded'], fa['original_ok'], fa['readable_ok'], fa['skipped'])
            vb = file_verdict(fb, a.ignore_new) + (fb['recorded'], fb['original_ok'],
                                                    str(old_counts(fb)[0]) if a.ignore_new else fb['readable_ok'],
                                                    fb['skipped'])
        if va == vb:
            same += 1
        elif a.no_worse and verdict_rank(va) <= verdict_rank(vb):
            better[(va[:2], vb[:2])] += 1
        else:
            differ += 1
            flips[(va[:2], vb[:2])] += 1
            if shown < a.show:
                shown += 1
                print('differs: %s\n  A %s\n  B %s' % (path, va, vb))
    print('files compared: %d identical verdicts, %d differ%s' % (
        same, differ, '; not run: %d of A\'s files (B replayed %d)' % (not_run, len(ib)) if a.replayed else ''))
    for (va, vb), n in flips.most_common():
        print('  %s -> %s: %d' % (va, vb, n))
    if a.no_worse:
        print('no worse: %d more files moved without a pass lost' % sum(better.values()))
        for (va, vb), n in better.most_common():
            print('  %s -> %s: %d' % (va, vb, n))
    return 0 if differ == 0 else 1


def per_function(lines, ignore_new=False):
    """{(game, address): {'routes': {route: verdict}, ...}}"""
    out = collections.defaultdict(lambda: {'routes': {}, 'calls': 0, 'failed': 0, 'files': 0})
    for f in lines:
        if f['kind'] not in ('result', 'fault'):
            continue
        if f['kind'] == 'fault':
            m = re.search(r'(?:([a-z0-9]+)-)?([0-9a-f]{8})\.fnrec$', f['file'])
            k = (m.group(1) or 'ai', int(m.group(2), 16))
            out[k]['routes'][f['route']] = 'fault'
            continue
        k = key_of(f)
        o, r = file_verdict(f, ignore_new)
        e = out[k]
        e['files'] += 1
        e['calls'] += int(f['recorded'])
        e['failed'] += int(f['original_bad']) + int(f['readable_bad'])
        v = 'fail' if 'bad' in (o, r) else ('pass' if r == 'ok' else ('original_ok' if o == 'ok' else 'none'))
        e['routes'][f['route']] = v
        for d in ('impl', 'orig', 'rec', 'checker', 'status', 'name'):
            if d in f:
                e[d] = f[d]
    return out


def cmd_verdicts(a):
    pf = per_function(load(a.dir))
    c = collections.Counter()
    for k, e in sorted(pf.items()):
        vs = set(e['routes'].values())
        v = 'fail' if vs & {'fail', 'fault'} else ('pass' if 'pass' in vs else 'none')
        c[v] += 1
        if a.list:
            print('%s:0x%08x %s %s' % (k[0], k[1], v, ' '.join('%s=%s' % x for x in sorted(e['routes'].items()))))
    print(dict(c))


def coverage(lines):
    """{(game, address): {'blocks': [...], 'hit': set, 'calls': n, 'files': n}}"""
    cov = {}
    for f in lines:
        if f['kind'] != 'coverage':
            continue
        k = key_of(f)
        e = cov.setdefault(k, {'blocks': None, 'hit': collections.Counter(), 'calls': 0, 'files': 0,
                               'name': f.get('name'), 'status': (f.get('status') or '').capitalize()})
        blocks = [int(x, 16) for x in f['blocks'].split(',')] if f.get('blocks') not in (None, '', '-') else []
        if e['blocks'] is None:
            e['blocks'] = blocks
        e['calls'] += int(f.get('calls', 0))
        e['files'] += 1
        for item in (f.get('hits') or '').split(','):
            if item and item != '-':
                addr, n = item.split(':')
                e['hit'][int(addr, 16)] += int(n)
    for e in cov.values():
        e['blocks'] = e['blocks'] or []
    return cov


def addresses(text):
    return {int(x, 16) for x in (text or '').split(',') if x and x != '-'}


def cmd_markers(a):
    """Per function over every file of every DIR: the entry blocks some file
    reached unmarked and no file marked, the loop heads some call branched
    back to without an ee::loopBack in any file (a file's own line lists only
    what its calls did)."""
    per = {}
    for d in a.dir:
        for f in load(d):
            if f['kind'] != 'markers':
                continue
            e = per.setdefault(key_of(f), {'name': f.get('name'), 'status': f.get('status'), 'files': 0,
                                           'blocks': set(), 'loops': set(), 'marked': set(), 'loops_marked': set()})
            e['files'] += 1
            e['blocks'] |= addresses(f.get('unmarked_blocks'))
            e['loops'] |= addresses(f.get('unmarked_loops'))
            e['marked'] |= addresses(f.get('marked_blocks_at'))
            e['loops_marked'] |= addresses(f.get('marked_loops_at'))
    n = collections.Counter()
    for k, e in sorted(per.items()):
        blocks = sorted(e['blocks'] - e['marked'])
        loops = sorted(e['loops'] - e['loops_marked'])
        if not blocks and not loops:
            continue
        n['blocks' if blocks else 'loops only'] += 1
        print('%s:0x%08x %s %s files=%d unmarked_blocks=%s unmarked_loops=%s' % (
            k[0], k[1], e['name'], e['status'], e['files'], ','.join('0x%x' % x for x in blocks) or '-',
            ','.join('0x%x' % x for x in loops) or '-'))
    print('functions with unmarked entry blocks: %d, with only loop heads left: %d (of %d with markers lines)' % (
        n['blocks'], n['loops only'], len(per)))


def cmd_safepoints(a):
    """Per function: the --safepoints lines summed (points tried, differing,
    safe-point counts differing, the first difference)."""
    per = {}
    for d in a.dir:
        for f in load(d):
            if f['kind'] != 'safepoints':
                continue
            e = per.setdefault(key_of(f), {'name': f.get('name'), 'status': f.get('status'), 'calls': 0, 'points': 0,
                                           'differ': 0, 'count_differ': 0, 'original_bad': 0, 'first': None})
            for c in ('calls', 'points', 'differ', 'count_differ', 'original_bad'):
                e[c] += int(f.get(c, 0))
            if not e['first'] and f.get('first') not in (None, '-'):
                e['first'] = f['first']
    bad = 0
    for k, e in sorted(per.items()):
        if a.all or e['differ'] or e['count_differ'] or e['original_bad']:
            bad += bool(e['differ'] or e['count_differ'])
            print('%s:0x%08x %s %s calls=%d points=%d differ=%d count_differ=%d original_bad=%d first=%s' % (
                k[0], k[1], e['name'], e['status'], e['calls'], e['points'], e['differ'], e['count_differ'],
                e['original_bad'], e['first'] or '-'))
    print('%d functions, %d with a difference; %d points tried, %d differ' % (
        len(per), bad, sum(e['points'] for e in per.values()), sum(e['differ'] for e in per.values())))


def cmd_saves(a):
    """Per function: the calls whose callee gets another value of a
    callee-saved register than the original held there (ps2xFnCheck's
    saves_differ: one the callee's prologue saves, a missing or wrong
    ee::calleeSaves; held_differ: any other), with the first of each."""
    only = address_list(a.only) if a.only else None
    per = {}
    for d in a.dir:
        for f in load(d):
            if f['kind'] != 'result' or f.get('readable') != '1':
                continue
            k = key_of(f)
            if only is not None and k not in only:
                continue
            e = per.setdefault(k, {'status': f.get('status'), 'recorded': 0, 'saves': 0, 'held': 0,
                                   'first_saves': None, 'first_held': None})
            e['recorded'] += int(f.get('recorded', 0))
            for c, first in (('saves', 'first_saves'), ('held', 'first_held')):
                e[c] += int(f.get(c + '_differ', 0))
                if not e[first] and f.get(first):
                    e[first] = f[first]
    n = 0
    for k, e in sorted(per.items()):
        if e['saves'] or (a.held and e['held']):
            n += 1
            print('%s:0x%08x %s recorded=%d saves_differ=%d held_differ=%d first_saves=%s first_held=%s' % (
                k[0], k[1], e['status'], e['recorded'], e['saves'], e['held'], e['first_saves'] or '-',
                e['first_held'] or '-'))
    print('%d functions, %d with a callee-saved register a callee saves given another value (%d calls)' % (
        len(per), sum(1 for e in per.values() if e['saves']), sum(e['saves'] for e in per.values())))


def cmd_args(a):
    """Per function: the calls that leave out a register their callee reads (ps2xFnCheck's
    args_left: with another value than the original's; direct_left: of a game::call), and that
    rely on an entry value left in the context (entry_left: a failure when a game::call names the
    function, direct_called=1), and a direct callee's leftover (ee::leftByCall) a callee reads
    (stale_left, a note). With --handed the upper bound through unnamed callees as well."""
    only = address_list(a.only) if a.only else None
    fields = ('args_left', 'direct_left', 'entry_left', 'stale_left') + (('handed_left',) if a.handed else ())
    per = {}
    for d in a.dir:
        for f in load(d):
            if f['kind'] != 'result' or f.get('readable') != '1':
                continue
            k = key_of(f)
            if only is not None and k not in only:
                continue
            e = per.setdefault(k, {'status': f.get('status'), 'direct': f.get('direct_called', '0'), 'recorded': 0})
            e['recorded'] += int(f.get('recorded', 0))
            for c in fields:
                e[c] = e.get(c, 0) + int(f.get(c, 0))
                if not e.get('first_' + c) and f.get('first_' + c):
                    e['first_' + c] = f['first_' + c]
    n = 0
    for k, e in sorted(per.items()):
        if any(e.get(c) for c in fields):
            n += 1
            print('%s:0x%08x %s direct_called=%s recorded=%d %s' % (k[0], k[1], e['status'], e['direct'], e['recorded'], ' '.join(
                '%s=%d first=%s' % (c, e[c], e.get('first_' + c, '-')) for c in fields if e.get(c))))
    print('%d functions, %d leave out a register their callee reads' % (len(per), n))


def cmd_vu0(a):
    """Per function not registered _VU0: the calls that leave a VU0 lane or Q unlike the
    original (ps2xFnCheck's vu0_left), every lane seen and the first. A note: a lane nothing reads
    does no harm, but a later VMULx.xyz keeps a stale lane and an SQC2 stores it (abdiff1)."""
    only = address_list(a.only) if a.only else None
    per = {}
    for d in a.dir:
        for f in load(d):
            if f['kind'] != 'result' or f.get('readable') != '1':
                continue
            k = key_of(f)
            if only is not None and k not in only:
                continue
            e = per.setdefault(k, {'status': f.get('status'), 'recorded': 0, 'left': 0, 'lanes': set(), 'first': None})
            e['recorded'] += int(f.get('recorded', 0))
            e['left'] += int(f.get('vu0_left', 0))
            e['lanes'] |= {x for x in f.get('vu0_lanes', '').strip('"').split(',') if x}
            if not e['first'] and f.get('first_vu0_left'):
                e['first'] = f['first_vu0_left']
    rows = [(k, e) for k, e in per.items() if e['left']]
    for k, e in sorted(rows, key=lambda kv: -kv[1]['left']):
        print('%s:0x%08x %s recorded=%d vu0_left=%d lanes=%s first=%s' % (
            k[0], k[1], e['status'], e['recorded'], e['left'], ','.join(sorted(e['lanes'])), e['first'] or '-'))
    print('%d functions, %d leave VU0 unlike the original (%d of %d calls)' % (
        len(per), len(rows), sum(e['left'] for _, e in rows), sum(e['recorded'] for e in per.values())))


def route_kept(route, prefixes):
    if not prefixes:
        return True
    keep = [x for x in prefixes.split(',') if x and not x.startswith('-')]
    drop = [x[1:] for x in prefixes.split(',') if x.startswith('-')]
    if any(route.startswith(x) for x in drop):
        return False
    return not keep or any(route.startswith(x) for x in keep)


def cmd_coverage(a):
    lines = [f for d in a.dir for f in load(d) if route_kept(f.get('route', ''), a.route_prefix)]
    cov = coverage(lines)
    if a.covered_out:
        with open(a.covered_out, 'w') as out:
            for k, e in sorted(cov.items()):
                if k[0] not in ('ai', None, ''):
                    continue
                hit = [b for b in e['blocks'] if e['hit'][b]]
                out.write('0x%08x %s\n' % (k[1], ','.join('0x%x' % b for b in hit) or '-'))
    rows = []
    for k, e in cov.items():
        if a.status and e['status'] not in a.status.split(','):
            continue
        total = len(e['blocks'])
        hit = sum(1 for b in e['blocks'] if e['hit'][b])
        rows.append({'game': k[0], 'addr': '0x%08X' % k[1], 'name': e['name'], 'status': e['status'],
                     'blocks': total, 'hit': hit, 'share': round(hit / total, 4) if total else None,
                     'calls': e['calls'], 'files': e['files'],
                     'uncovered': ['0x%X' % b for b in e['blocks'] if not e['hit'][b]]})
    if a.json:
        json.dump(rows, sys.stdout, indent=0)
        return
    rows.sort(key=lambda r: (r['share'] if r['share'] is not None else 2, -r['blocks']))
    shares = [r['share'] for r in rows if r['share'] is not None]
    tb, th = sum(r['blocks'] for r in rows), sum(r['hit'] for r in rows)
    print('%d functions, blocks %d, hit %d (%.1f %%)' % (len(rows), tb, th, 100.0 * th / tb if tb else 0))
    bins = collections.Counter()
    for s in shares:
        bins['100 %' if s == 1 else '90-99 %' if s >= 0.9 else '75-89 %' if s >= 0.75 else '50-74 %' if s >= 0.5
             else '25-49 %' if s >= 0.25 else '0-24 %'] += 1
    for b in ('100 %', '90-99 %', '75-89 %', '50-74 %', '25-49 %', '0-24 %'):
        print('  %-8s %d' % (b, bins[b]))
    if shares:
        s = sorted(shares)
        print('  median %.1f %%, mean %.1f %%' % (100 * s[len(s) // 2], 100 * sum(s) / len(s)))
    print('worst %d:' % a.worst)
    for r in rows[:a.worst]:
        print('  %s %s %-32s %3d of %3d blocks (%5.1f %%), %d calls; uncovered %s' % (
            r['addr'], r['status'], (r['name'] or '')[:32], r['hit'], r['blocks'], 100 * (r['share'] or 0), r['calls'],
            ' '.join(r['uncovered'][:12]) + (' ...' if len(r['uncovered']) > 12 else '')))


def cmd_prune_plan(a):
    """A dry run of the corpus' pruning by file: per function, the recordings
    to keep so that every function's verdict, the blocks the corpus covers,
    the markers' and safe points' findings and (with --situations) one
    recording of every game situation the function was recorded in stay; the
    sizes before and after, checked by running this tool's own summaries on
    the kept lines. Nothing is moved or deleted."""
    sys.path.insert(0, HERE)
    import readable_corpus
    lines = [f for d in a.dir for f in load(d)]
    per = collections.defaultdict(lambda: collections.defaultdict(list))   # function -> file -> lines
    route_of = {}
    for f in lines:
        m = re.search(r'(?:([a-z0-9]+)-)?([0-9a-f]{8})\.fnrec$', f['file'])
        per[(m.group(1) or 'ai', int(m.group(2), 16))][f['file']].append(f)
        route_of[f['file']] = f['route']

    def elements(ls):
        """What a file adds: blocks hit, markers' blocks and loops (reached,
        marked), as (kind, address); bad: it fails, faults or has a safe-point
        difference."""
        out, bad = set(), False
        for f in ls:
            k = f['kind']
            if k == 'coverage':
                for item in (f.get('hits') or '').split(','):
                    if item and item != '-':
                        out.add(('block', int(item.split(':')[0], 16)))
            elif k == 'markers':
                for field in ('unmarked_blocks', 'unmarked_loops', 'marked_blocks_at', 'marked_loops_at'):
                    out |= set((field, x) for x in addresses(f.get(field)))
            elif k == 'fault':
                bad = True
            elif k == 'result':
                o, r = file_verdict(f)
                bad = bad or 'bad' in (o, r)
            elif k == 'safepoints':
                bad = bad or bool(int(f.get('differ', 0)) or int(f.get('count_differ', 0)) or int(f.get('original_bad', 0)))
        return out, bad
    variants = {'blocks': False, 'situations': True}
    report = {}
    keep_sets = {}
    for name, by_situation in variants.items():
        keep = set()
        for fn, files in per.items():
            info = {p: elements(ls) for p, ls in files.items()}
            kept = set(p for p, (_, bad) in info.items() if bad)
            have = set().union(*[info[p][0] for p in kept]) if kept else set()
            want = set().union(*[e for e, _ in info.values()])
            # Greedy cover: the file adding most, then the smaller.
            while have != want:
                best = max((p for p in files if p not in kept),
                           key=lambda p: (len(info[p][0] - have), -recording_size(p)))
                kept.add(best)
                have |= info[best][0]
            if by_situation:
                seen = set(readable_corpus.situation(route_of[p]) for p in kept)
                for p in sorted(files, key=lambda p: (-len(info[p][0]), recording_size(p))):
                    sit = readable_corpus.situation(route_of[p])
                    if sit not in seen:
                        kept.add(p)
                        seen.add(sit)
            if not kept and files:
                kept.add(min(files, key=recording_size))
            keep |= kept
        keep_sets[name] = keep
        kept_lines = [f for f in lines if f['file'] in keep]
        sizes = dict((p, recording_size(p)) for p in route_of)
        # The checks: verdicts per function, coverage, markers, safe points.
        def summary(ls):
            pf = per_function(ls)
            verd = {}
            for k, e in pf.items():
                vs = set(e['routes'].values())
                verd[k] = 'fail' if vs & {'fail', 'fault'} else ('pass' if 'pass' in vs else 'none')
            cov = dict((k, frozenset(b for b in e['blocks'] if e['hit'][b])) for k, e in coverage(ls).items())
            mk = collections.defaultdict(lambda: [set(), set(), set(), set()])
            sp = collections.defaultdict(int)
            for f in ls:
                if f['kind'] == 'markers':
                    e = mk[key_of(f)]
                    for i, field in enumerate(('unmarked_blocks', 'unmarked_loops', 'marked_blocks_at', 'marked_loops_at')):
                        e[i] |= addresses(f.get(field))
                elif f['kind'] == 'safepoints' and (int(f.get('differ', 0)) or int(f.get('count_differ', 0))):
                    sp[key_of(f)] += 1
            mk = dict((k, (frozenset(e[0] - e[2]), frozenset(e[1] - e[3]))) for k, e in mk.items())
            return verd, cov, mk, set(sp)
        full, part = summary(lines), summary(kept_lines)
        same = [x == y for x, y in zip(full, part)]
        report[name] = {'files': len(keep), 'files_before': len(route_of),
                        'gb': round(sum(sizes[p] for p in keep) / 1e9, 2),
                        'gb_before': round(sum(sizes.values()) / 1e9, 2),
                        'same': dict(zip(('verdicts', 'coverage', 'markers', 'safepoints'), same)),
                        'routes': dict(sorted(collections.Counter(route_of[p] for p in keep).items()))}
        print('%-10s keep %d of %d files, %.2f of %.2f GB; same verdicts %s, coverage %s, markers %s, safe points %s' % (
            name, len(keep), len(route_of), report[name]['gb'], report[name]['gb_before'], *same))
    if a.out:
        json.dump({'dirs': a.dir, 'report': report,
                   'keep': dict((n, sorted(k)) for n, k in keep_sets.items())}, open(a.out, 'w'), indent=0)
        print('plan -> %s' % a.out)


def cmd_generate(a):
    os.makedirs(a.out, exist_ok=True)
    out = open(os.path.join(a.out, 'lines.jsonl'), 'w')
    bad = 0
    for item in a.functions:
        fn, _, n = item.partition(':')
        stdout, stderr, status, why, _ = run_child([a.fncheck, '--generate', fn, n or '20000'], 3600)
        p = argparse.Namespace(stdout=stdout, stderr=stderr + (' (killed: %s)' % why if why else ''),
                               returncode=1 if status is None else status)
        for kind, rest in LINE.findall(p.stdout):
            f = fields(rest)
            f.update(kind=kind, route='generate')
            out.write(json.dumps(f) + '\n')
        summary = [l for l in p.stdout.split('\n') if 'generated calls identical' in l or ' differ: ' in l]
        log('\n'.join(summary) or '%s: no result (%s)' % (fn, p.stderr.strip()[:200]))
        bad += p.returncode != 0
    return 1 if bad else 0


def cmd_emit(a):
    emit_results(a.dir, a.run, a.results)


WORKDB = zaxis_paths.get('WORKDB_PY', required=False) or ''


def emit_results(run_dir, run_name, results):
    """RESULTS/RUN_NAME/check.jsonl and coverage.jsonl from a replay's lines (appended), then
    `workdb.py check-results` on them when the work database's tool is there."""
    a = argparse.Namespace(dir=run_dir, run=run_name, results=results)
    lines = load(a.dir)
    pf = per_function(lines)
    cov = coverage(lines)
    run = json.load(open(os.path.join(a.dir, 'run.json')))
    out = os.path.join(os.path.expanduser(a.results), a.run)
    os.makedirs(out, exist_ok=True)
    ts = datetime.datetime.now().isoformat(timespec='seconds')
    gen = 'fncheck_corpus.py'

    def write(kind, recs):
        with open(os.path.join(out, kind + '.jsonl'), 'a+') as f:
            lock_file(f)
            try:
                f.seek(0, os.SEEK_END)
                for r in recs:
                    f.write(json.dumps(r, sort_keys=True) + '\n')
            finally:
                unlock_file(f)
    checks, covs = [], []
    by_file = collections.defaultdict(list)
    for f in lines:
        if f['kind'] == 'result':
            by_file[(key_of(f), f['route'])].append(f)
    for (k, route), fs in sorted(by_file.items()):
        f = fs[0]
        o, r = file_verdict(f)
        if f.get('readable') != '1':
            continue  # Planned: no readable version, nothing passed
        result = 'fail' if 'bad' in (o, r) else ('pass' if r == 'ok' else 'no_recording')
        rec = {'kind': 'check', 'addr': '0x%08X' % k[1], 'run': a.run, 'generator': gen, 'ts': ts,
               'route': route, 'result': result, 'calls': int(f['recorded']),
               'failed': int(f['original_bad']) + int(f['readable_bad']),
               'cycles_equal': int(f.get('cycles_differ', 0)) == 0}
        if k[0] != 'ai':
            rec['game'] = k[0]
        if result == 'fail':
            rec['first_failure'] = (f.get('first_readable') or f.get('first_original') or '')[:300]
        for d in ('impl', 'orig', 'rec', 'checker'):
            if f.get(d):
                rec[d] = f[d]
        checks.append(rec)
    for k, e in sorted(cov.items()):
        total = len(e['blocks'])
        hit = sum(1 for b in e['blocks'] if e['hit'][b])
        rec = {'kind': 'coverage', 'addr': '0x%08X' % k[1], 'run': a.run, 'generator': gen, 'ts': ts,
               'blocks': total, 'hit': hit, 'calls': e['calls'], 'files': e['files'],
               'uncovered': ['0x%08X' % b for b in e['blocks'] if not e['hit'][b]],
               'corpus': ','.join(run['corpus'])}
        if k[0] != 'ai':
            rec['game'] = k[0]
        p = pf.get(k)
        if p and p.get('orig'):
            rec['orig'] = p['orig']
        if p and p.get('checker'):
            rec['checker'] = p['checker']
        covs.append(rec)
    write('check', checks)
    write('coverage', covs)
    print('%d check lines, %d coverage lines -> %s' % (len(checks), len(covs), out))
    if os.path.exists(WORKDB):
        p = subprocess.run([sys.executable, '-I', WORKDB, 'check-results', os.path.join(out, 'check.jsonl'),
                            os.path.join(out, 'coverage.jsonl')], capture_output=True, text=True)
        print('workdb.py check-results: ' + (p.stdout.strip().split('\n')[-1] if p.stdout.strip() else p.stderr.strip()[-200:]))


def joined_options(argv):
    """`--fncheck-arg --semantic`: its value starts with a dash, which argparse takes for an option."""
    out, i = [], 0
    while i < len(argv):
        if argv[i] == '--fncheck-arg' and i + 1 < len(argv):
            out.append('--fncheck-arg=' + argv[i + 1])
            i += 2
        else:
            out.append(argv[i])
            i += 1
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--fncheck', required=True)
    r.add_argument('--out', required=True)
    r.add_argument('--corpus', action='append')
    r.add_argument('--jobs', default='4', help='processes side by side, or auto: what the free memory above %d%%%% '
                   'holds at %d MB a process, at most --max-jobs' % (FREE_FLOOR * 100, PEAK_MB))
    r.add_argument('--max-jobs', type=int, default=2)
    r.add_argument('--max-mem-gb', type=float, default=4.0,
                   help='a ps2xFnCheck process group above this footprint is killed, its file a fault with phase '
                        'memory (default %(default)s; 0: no cap)')
    r.add_argument('--chunk', type=int, default=150)
    r.add_argument('--timeout', type=int, default=3600)
    r.add_argument('--only', help='ADDR,... or @FILE')
    r.add_argument('--fncheck-arg', action='append', help='more ps2xFnCheck options (one string)')
    r.add_argument('--semantic', action='store_true', help='the semantic verdict in place of --cycles (the merge gate)')
    r.add_argument('--frame-index', metavar='DIR', help='first the folders\' index of leftover reads in DIR, then the '
                   'replay with it: frame residue a later function reads fails (frame_residue_read)')
    r.add_argument('--build', help='the build ps2xFnCheck comes from (default: the one it is in)')
    r.add_argument('--emit', metavar='RUN', help='after the replay, the work database\'s check and coverage '
                   'results as RESULTS/RUN/{check,coverage}.jsonl (fncheck_corpus.py emit)')
    r.add_argument('--results', default=RESULTS, help='with --emit: the results folder (default %(default)s)')
    r.add_argument('--cache', default=CACHE, help='the verdict cache folder (default %(default)s)')
    r.add_argument('--no-cache', action='store_true', help='neither read nor fill the cache')
    r.add_argument('--incremental', action='store_true', help='replay only files whose key changed')
    r.add_argument('--extras-changed', action='store_true',
                   help='--safepoints and --markers lines of functions whose source and original are unchanged '
                        'from the cache even when the checker changed (all but --extras-for)')
    r.add_argument('--extras-for', help='ADDR,... or @FILE: functions always checked with the extras')
    r.add_argument('--verify', type=float, default=0.01, help='share of cached files replayed as a check (default 0.01)')
    r.add_argument('--seed', type=int, default=1)
    ci = sub.add_parser('cache-import')
    ci.add_argument('dir')
    ci.add_argument('--build')
    ci.add_argument('--digests', help='the digests `digests` wrote for the build the run was made with')
    dg = sub.add_parser('digests')
    dg.add_argument('--build', required=True)
    dg.add_argument('--fncheck')
    dg.add_argument('--corpus', action='append')
    dg.add_argument('--cache', default=CACHE)
    dg.add_argument('--out', required=True)
    ci.add_argument('--fncheck')
    ci.add_argument('--cache', default=CACHE)
    ci.add_argument('--map-corpus', metavar='FROM=TO', help='the run\'s corpus folder FROM as this host\'s TO: another '
                    'host\'s run of the same commit imported under this host\'s recordings and build')
    cg = sub.add_parser('cache-gc')
    cg.add_argument('--cache', default=CACHE)
    cg.add_argument('--days', type=float, default=14)
    sm = sub.add_parser('same')
    sm.add_argument('a')
    sm.add_argument('b')
    sm.add_argument('--show', type=int, default=10)
    sm.add_argument('--ignore', help='fields left out of the comparison (comma separated, e.g. checker)')
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--ignore-new', action='store_true')
    c.add_argument('--no-worse', action='store_true', help='only a pass lost fails')
    c.add_argument('--semantic', action='store_true', help='B must be a semantic run')
    c.add_argument('--show', type=int, default=20)
    c.add_argument('--replayed', action='store_true',
                   help='compare only the files B has (B an --only run): a file only in A was not run, counted apart and '
                        'not as a difference (a file only in B still differs)')
    v = sub.add_parser('verdicts')
    v.add_argument('dir')
    v.add_argument('--list', action='store_true')
    g = sub.add_parser('coverage')
    g.add_argument('dir', nargs='+')
    g.add_argument('--route-prefix')
    g.add_argument('--covered-out')
    g.add_argument('--status')
    g.add_argument('--worst', type=int, default=20)
    g.add_argument('--json', action='store_true')
    e = sub.add_parser('emit')
    e.add_argument('dir')
    e.add_argument('--run', required=True)
    e.add_argument('--results', default=RESULTS)
    mk = sub.add_parser('markers')
    mk.add_argument('dir', nargs='+')
    sv = sub.add_parser('saves')
    sv.add_argument('dir', nargs='+')
    sv.add_argument('--only', help='ADDR,... or @FILE')
    sv.add_argument('--held', action='store_true', help='also functions with only held_differ')
    ar = sub.add_parser('args')
    ar.add_argument('dir', nargs='+')
    ar.add_argument('--only')
    ar.add_argument('--handed', action='store_true')
    vz = sub.add_parser('vu0')
    vz.add_argument('dir', nargs='+')
    vz.add_argument('--only', help='ADDR,... or @FILE')
    sp = sub.add_parser('safepoints')
    sp.add_argument('dir', nargs='+')
    sp.add_argument('--all', action='store_true')
    pp = sub.add_parser('prune-plan')
    pp.add_argument('dir', nargs='+')
    pp.add_argument('--out')
    gen = sub.add_parser('generate')
    gen.add_argument('--fncheck', required=True)
    gen.add_argument('--out', required=True)
    gen.add_argument('functions', nargs='+')
    a = p.parse_args(joined_options(sys.argv[1:]))
    sys.exit({'run': cmd_run, 'cache-import': cmd_cache_import, 'cache-gc': cmd_cache_gc, 'prune-plan': cmd_prune_plan, 'digests': cmd_digests, 'same': cmd_same, 'compare': cmd_compare, 'verdicts': cmd_verdicts, 'coverage': cmd_coverage,
              'emit': cmd_emit, 'generate': cmd_generate, 'markers': cmd_markers,
              'safepoints': cmd_safepoints, 'saves': cmd_saves, 'args': cmd_args, 'vu0': cmd_vu0}[a.cmd](a) or 0)


if __name__ == '__main__':
    main()
