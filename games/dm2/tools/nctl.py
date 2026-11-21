#!/usr/bin/env python3
"""Drive a running native DM2 build through ZX_CONTROL (copy of the Aggressive Inline
repo's tools/nctl.py with DM2 paths and a scratch HOME under /tmp/ai-dm2).

    nctl.py start NAME [KEY=VAL ...]   launch the runner (recomp/run.sh env) with a control dir
    nctl.py NAME CMD [; CMD ...]       send commands, wait for each, print replies
    nctl.py stop NAME                  kill the runner

Commands are the runtime's (z-axis ps2/runtime/debug_control.h):
press BTN[+BTN] [hold] [gap], hold BTN n, down/up BTN, stick AXIS v [n], wait n,
until COND [timeout], peek ADDR [n], peekf ADDR [n], poke/or ADDR V, shot FILE,
dump FILE, echo TEXT. A relative shot/dump path goes to recomp/logs/.
Extra client-side commands: "sleep SECONDS".
Control dirs live in recomp/run/ctl-NAME; logs in recomp/logs/NAME.log.
"""
import os
import signal
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "workspace"))   # (the game's workspace: recomp/)
RECOMP = os.path.join(ROOT, "recomp")
LOGS = os.path.join(RECOMP, "logs")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))   # (tools/: zpaths.py)
import zpaths  # noqa: E402,F401  (the shared environment: no sound, ZX_AUDIO)


class RunnerGone(Exception):
    pass


def ctl_dir(name):
    return os.path.join(RECOMP, "run", "ctl-" + name)


def start(name, extra):
    d = ctl_dir(name)
    os.makedirs(d, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    for f in ("cmd", "out"):
        open(os.path.join(d, f), "w").close()
    env = dict(os.environ)
    env.setdefault("ZX_DEBUG_UI", "0")
    # The CPU renderer unless asked (ZX_GS_BACKEND=gpu as KEY=VAL or in the environment).
    env.setdefault("ZX_GS_BACKEND", "cpu")
    for kv in extra:
        k, v = kv.split("=", 1)
        env[k] = v
    env["ZX_CONTROL"] = d
    # Never the user's folders: a scratch HOME, support and log dirs, card.
    scratch = env.get("DM2_SCRATCH", "/tmp/ai-dm2")
    for sub in ("home", "mc0", "mc1"):
        os.makedirs(os.path.join(scratch, sub), exist_ok=True)
    env["HOME"] = os.path.join(scratch, "home")
    env["ZX_APP_SUPPORT_DIR"] = os.path.join(scratch, "home", "support")
    env["ZX_APP_LOG_DIR"] = os.path.join(scratch, "home", "logs")
    env.setdefault("ZX_MC_ROOT", os.path.join(scratch, "mc0"))
    runner = env.get("RUNNER", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "out", "build-coll",
                                             "runtime", "zx-runner" + (".exe" if os.name == "nt" else "")))
    log = open(os.path.join(LOGS, name + ".log"), "w")
    p = subprocess.Popen([runner, os.path.join(RECOMP, "disc-root", "SLUS_201.59")],
                         cwd=os.path.join(RECOMP, "run"), env=env, stdout=log,
                         stderr=subprocess.STDOUT, start_new_session=os.name != "nt")
    open(os.path.join(d, "pid"), "w").write(str(p.pid))
    print("started pid", p.pid, "control", d)


def stop(name):
    d = ctl_dir(name)
    try:
        pid = int(open(os.path.join(d, "pid")).read())
    except OSError:
        print("no pid")
        return
    try:
        os.kill(pid, signal.SIGTERM)
        time.sleep(1)
        if os.name != "nt":
            os.kill(pid, signal.SIGKILL)
    except (ProcessLookupError, OSError):
        pass
    print("stopped", pid)


def pid_alive(pid):
    if os.name == "nt":
        # os.kill(pid, 0) would terminate the process on Windows.
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def alive(d):
    try:
        return pid_alive(int(open(os.path.join(d, "pid")).read()))
    except (OSError, ValueError):
        return False


def send(name, commands, timeout=None, quiet=False, replies=None):
    d = ctl_dir(name)
    cmd_path, out_path = os.path.join(d, "cmd"), os.path.join(d, "out")
    for c in commands:
        c = c.strip()
        if not c:
            continue
        w = c.split()
        if w[0] == "sleep":
            time.sleep(float(w[1]))
            continue
        if w[0] in ("shot", "dump") and len(w) > 1 and not os.path.isabs(w[1]):
            w[1] = os.path.join(LOGS, w[1])
            c = " ".join(w)
        with open(cmd_path) as f:
            line_id = sum(1 for _ in f) + 1
        with open(cmd_path, "a") as f:
            f.write(c + "\n")
        deadline = time.time() + (timeout or 3600)
        while True:
            reply = None
            with open(out_path) as f:
                for line in f:
                    i, _, rest = line.partition(" ")
                    if i == str(line_id):
                        reply = rest.rstrip()
            if reply is not None:
                if not quiet:
                    print(f"{c} -> {reply}")
                if replies is not None:
                    replies.append(reply)
                break
            if not alive(d):
                print(f"{c} -> runner not running")
                raise RunnerGone(name)
            if time.time() > deadline:
                print(f"{c} -> client timeout")
                return 1
            time.sleep(0.05)
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    if argv[1] == "start":
        start(argv[2], argv[3:])
        return 0
    if argv[1] == "stop":
        stop(argv[2])
        return 0
    name = argv[1]
    commands = " ".join(argv[2:]).split(";")
    try:
        return send(name, commands)
    except RunnerGone:
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
