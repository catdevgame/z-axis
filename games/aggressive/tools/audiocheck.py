#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy"]
# ///
"""Compare the native build's sound with PCSX2's at the same game moments.

For each scene the native runner and PCSX2 are driven to the same spot and
the same stretch of sound is recorded from:

  pcsx2  PCSX2's output, recorded from the process (platform/mac/appaudio.swift)
  tap    our exact SPU2 mix (ZX_SPU2_WAV), in emulated time
  host   what our process really played (appaudio on the runner), i.e. the
         tap after the host ring buffer, rate correction and SDL

Scenes (PCSX2 savestates from REFERENCE-RUN.md / REFERENCE-RUN-2.md; native
reaches the same state with the logiccheck.py / careercheck.py pad scripts):

  intro     boot movies, no input: Z-Axis and INTRO.PSS sound (MPEG audio path)
  menu      main menu music (slot 30, ref01-main-menu)
  tutorial  Tutorial, rolling towards the chef: skates and ambience (slot 34, ref05-talk-skating;
            the first tip box, slot 32, is silent in PCSX2 too)
  movielot  MOVIE LOT, standing at the level start: ambience (slot 80, car05-level-start)

The PCSX2 recording of a scene is the reference window. It is found in the
native recordings by normalised cross-correlation (so no exact start times
are needed), and the aligned windows are measured: level, peak, clipped
samples, clicks (sample-to-sample jumps far above the local signal) and
dropouts (digital silence inside sound) per second, speed (how far the
match drifts between the start and the end of the window), octave-band
spectrum, and waveform/envelope similarity. One PNG per scene shows the
three sources' waveform and spectrogram on the same time axis.

  tools/audiocheck.py run NAME [--scenes intro,menu,tutorial,movielot] [--secs 12] [--keep-raw]
  tools/audiocheck.py analyze NAME              # measure again from kept raw recordings
  tools/audiocheck.py wav FILE [FILE...]        # numbers for any 48 kHz s16 WAV(s), first = reference

Output: recomp/logs/audiocheck/NAME/: report.txt, <scene>.png, aligned
<scene>-{pcsx2,tap,host}.wav (listen to them side by side), the runner
logs. Raw recordings go to recomp/run/audiocheck/raw-NAME and are deleted
unless --keep-raw. `--native-only` reuses the PCSX2 recordings of a kept
run (AUDIOCHECK_PCSX2_FROM=<NAME>), `--pcsx2-only` records just PCSX2.

PCSX2 runs from an APFS clone of tools/pcsx2-test-data in
recomp/run/audiocheck/pcsx2-data (PINE slot 28019, output unmuted), so
the test data dir and its PCSX2.ini are never touched. appaudio records
one process only, so other sound on the Mac does not matter, but CPU load
does: the host recording only stays clean while the runner keeps 100 %
speed (the report shows the load average and the runner's SPU2 stats).

Environment: RUNNER (zx-runner), AUDIOCHECK_SCRATCH (default
recomp/run/audiocheck), extra ZX_* vars pass through to the runner.
Needs Screen Recording permission for the terminal (appaudio).
"""
import argparse
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import wave
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import logiccheck as lc  # noqa: E402

RECOMP = os.path.join(ROOT, "workspace", "recomp")
SCRATCH = os.environ.get("AUDIOCHECK_SCRATCH", os.path.join(RECOMP, "run", "audiocheck"))
OUT_ROOT = os.path.join(RECOMP, "logs", "audiocheck")
RUNNER = os.environ.get("RUNNER", os.path.join(os.path.dirname(os.path.dirname(ROOT)), "out", "build-ai", "runtime", "zx-runner"))
DISC_ELF = os.path.join(RECOMP, "disc-root", "SLUS_203.27")
WINID = os.path.join(RECOMP, "tools", "winid")
PCSX2PAD = os.path.join(HERE, "pcsx2pad.py")
PX_DATA = os.path.join(SCRATCH, "pcsx2-data")
PX_SLOT = "28019"
RATE = 48000
SOURCES = ("pcsx2", "tap", "host")

GUEST_VSYNC = 0x231CA4

# PCSX2 side: "boot" = fresh launch, then wait for the game's vsync counter;
# a number = savestate slot. Native side: pad script (vsync clock) and the
# condition (ZX_DUMP_WHEN syntax) that marks the spot; recording runs from
# the window appearing until `secs + after` seconds past the mark.
SCENES = {
    "intro": dict(desc="boot movies: Z-Axis + INTRO.PSS (MPEG audio)", px="boot", px_vsync=1260,
                  native_mark="0x%X>=1260" % GUEST_VSYNC, after=25, timeout=90),
    "menu": dict(desc="main menu music", px=30, script=("logic", "ref01-main-menu"), after=6, timeout=90),
    "tutorial": dict(desc="Tutorial: rolling to the chef (skates, ambience)", px=34,
                     script=("logic", "ref05-talk-skating"), after=6, timeout=120),
    "movielot": dict(desc="MOVIE LOT level start: ambience", px=80,
                     script=("career", "car05-level-start"), after=6, timeout=150),
}


def log(*a):
    print(*a, flush=True)


# ---- native pad scripts ------------------------------------------------------

def _truncate(script, until):
    items = [i for i in script.split(",") if i and int(i.split("v", 1)[0]) <= until]
    return ",".join(items)


def native_script(kind, checkpoint):
    """(pad script up to the checkpoint, its script time in vsyncs)."""
    saved = lc.INPUT_LOG
    try:
        if kind == "logic":
            first, _ = lc.read_inputs()
            v = dict(lc.checkpoints())[checkpoint]
            return _truncate(lc.native_script(), v - first), v - first
        import careercheck as cc
        os.environ["CAREERCHECK_TO"] = checkpoint
        first, _ = cc.read_inputs()
        v = dict(cc.CHECKPOINTS)[checkpoint]
        return cc.native_script(), v - first
    finally:
        lc.INPUT_LOG = saved
        os.environ.pop("CAREERCHECK_TO", None)


# ---- process helpers ---------------------------------------------------------

def appaudio_bin():
    exe = os.path.join(SCRATCH, "appaudio")
    src = os.path.join(HERE, "..", "..", "..", "platform", "mac", "appaudio.swift")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(SCRATCH, exist_ok=True)
        log("building appaudio")
        subprocess.run(["swiftc", "-O", src, "-o", exe], check=True, capture_output=True)
    return exe


def window_up(pid, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = subprocess.run([WINID, str(pid)], capture_output=True, text=True).stdout.strip()
        if out:
            return True
        time.sleep(0.25)
    return False


def start_recorder(pid, path, seconds=900):
    rec = subprocess.Popen([appaudio_bin(), str(pid), str(seconds), path],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return rec


def stop_recorder(rec):
    if rec.poll() is None:
        rec.send_signal(signal.SIGINT)
    try:
        out, _ = rec.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        rec.kill()
        out, _ = rec.communicate()
    return (out or "").strip()


def px_env():
    env = dict(os.environ)
    env["PCSX2PAD_DATA"] = PX_DATA
    env["PCSX2PAD_SLOT"] = PX_SLOT
    return env


def pad(*args, check=True):
    r = subprocess.run([sys.executable, PCSX2PAD] + [str(a) for a in args], env=px_env(),
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit("pcsx2pad %s failed:\n%s%s" % (" ".join(map(str, args)), r.stdout, r.stderr))
    return r.stdout.strip()


def setup_pcsx2_data():
    if os.path.isdir(PX_DATA):
        return
    src = os.path.join(HERE, "pcsx2-test-data")
    log("cloning %s -> %s (APFS clone, no extra space)" % (src, PX_DATA))
    os.makedirs(SCRATCH, exist_ok=True)
    subprocess.run(["cp", "-cR", src, PX_DATA], check=True)
    ini = os.path.join(PX_DATA, "PCSX2", "inis", "PCSX2.ini")
    text = open(ini).read()
    text = re.sub(r"(?m)^PINESlot = .*$", "PINESlot = " + PX_SLOT, text)
    text = re.sub(r"(?m)^OutputMuted = .*$", "OutputMuted = false", text)
    open(ini, "w").write(text)
    for f in ("pcsx2pad.pid",):
        p = os.path.join(PX_DATA, f)
        if os.path.exists(p):
            os.remove(p)


def px_pid():
    try:
        return int(open(os.path.join(PX_DATA, "pcsx2pad.pid")).read().strip())
    except (OSError, ValueError):
        return None


def record_pcsx2(scenes, raw, secs):
    setup_pcsx2_data()
    pad("quit", check=False)
    notes = {}
    order = sorted(scenes, key=lambda s: SCENES[s]["px"] != "boot")   # boot first: it needs a fresh launch
    log("pcsx2: launching (data %s, PINE slot %s)" % (PX_DATA, PX_SLOT))
    log(pad("launch"))
    pid = px_pid()
    try:
        for name in order:
            sc = SCENES[name]
            path = os.path.join(raw, "%s-pcsx2.wav" % name)
            if sc["px"] == "boot":
                pad("waitval", "0x%X" % GUEST_VSYNC, "u32", ">=", sc["px_vsync"], 20000)
            else:
                pad("loadstate", sc["px"])
                time.sleep(0.5)
            rec = start_recorder(pid, path, secs + 1.5)
            out = stop_recorder_when_done(rec)
            notes[name] = out
            log("pcsx2 %-9s %s" % (name, out.splitlines()[-1] if out else "?"))
    finally:
        pad("quit", check=False)
    return notes


def stop_recorder_when_done(rec):
    out, _ = rec.communicate()
    return (out or "").strip()


def runner_env(name, raw, sc):
    env = dict(os.environ)
    env.setdefault("ZX_DEBUG_UI", "0")
    mc = os.path.join(RECOMP, "run", "mc-audio")
    for d in (mc, mc + "_slot1"):
        if os.path.isdir(d):
            shutil.rmtree(d)
    os.makedirs(mc)
    env["ZX_MC_ROOT"] = mc
    # The memory card path: the boot timing the reference was taken with.
    env.setdefault("ZX_SAVES", "card")
    support = os.path.join(SCRATCH, "support")
    os.makedirs(support, exist_ok=True)
    env.setdefault("ZX_CONTROLS_INI", os.path.join(support, "controls.ini"))
    env.setdefault("ZX_APP_SUPPORT_DIR", support)
    env.setdefault("ZX_APP_LOG_DIR", os.path.join(support, "Logs"))
    env["ZX_AUDIO"] = "on"  # the host recording is what the process plays: the one run that is heard
    env["ZX_SPU2_WAV"] = os.path.join(raw, "%s-tap.wav" % name)
    env["ZX_SPU2_WAV_SECONDS"] = str(sc["timeout"] + sc["after"] + 30)
    env.setdefault("ZX_SPU2_STATS", "5")
    env["ZX_DUMP_PREFIX"] = os.path.join(raw, "mark-" + name)
    if "script" in sc:
        script, v = native_script(*sc["script"])
        env["ZX_PAD_SCRIPT_CLOCK"] = "vsync"
        env["ZX_PAD_SCRIPT"] = script
        env["ZX_DUMP_WHEN"] = "audiomark:script>=%d" % v
    else:
        env["ZX_DUMP_WHEN"] = "audiomark:" + sc["native_mark"]
    return env


def record_native(scenes, raw, out_dir, secs, name_prefix):
    if not os.path.exists(RUNNER):
        raise SystemExit("no runner at %s (set RUNNER)" % RUNNER)
    notes = {}
    for name in scenes:
        sc = SCENES[name]
        env = runner_env(name, raw, sc)
        logpath = os.path.join(RECOMP, "logs", "audio-%s-%s.log" % (name_prefix, name))
        mark = os.path.join(raw, "mark-%s-audiomark.ram" % name)
        if os.path.exists(mark):
            os.remove(mark)
        host = os.path.join(raw, "%s-host.wav" % name)
        cwd = os.path.join(SCRATCH, "cwd")
        os.makedirs(cwd, exist_ok=True)
        with open(logpath, "w") as lf:
            proc = subprocess.Popen([RUNNER, DISC_ELF], cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT)
        rec = None
        t0 = time.time()
        status = "ok"
        try:
            if not window_up(proc.pid, 30):
                raise SystemExit("runner window did not appear (log %s)" % logpath)
            time.sleep(0.5)
            t_rec = time.time() - t0
            rec = start_recorder(proc.pid, host)
            while not os.path.exists(mark):
                if proc.poll() is not None:
                    raise SystemExit("runner exited (rc %s) before the mark, log %s" % (proc.returncode, logpath))
                if time.time() - t0 > sc["timeout"]:
                    status = "mark not reached in %d s" % sc["timeout"]
                    break
                time.sleep(0.1)
            t_mark = time.time() - t0
            time.sleep(secs + sc["after"])
        finally:
            rec_out = stop_recorder(rec) if rec else ""
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        if os.path.exists(mark):
            os.remove(mark)
        shutil.copy(logpath, os.path.join(out_dir, "native-%s.log" % name))
        text = open(logpath, errors="replace").read()
        spu2 = [l.strip() for l in text.splitlines() if l.startswith("[spu2]")]
        m = re.search(r"\[dump\] audiomark at vsync tick (\d+)", text)
        # Where the mark is in each recording: the tap starts with the
        # emulated machine (one vsync tick = 16667 us), the host recording
        # when the window came up.
        notes[name] = dict(status=status, mark_wall=round(t_mark, 2), rec_wall=round(t_rec, 2),
                           load=round(os.getloadavg()[0], 1),
                           mark_tap=int(m.group(1)) * 0.016667 if m else None,
                           appaudio=rec_out.splitlines()[-1] if rec_out else "",
                           spu2=spu2[-1] if spu2 else "", spu2_lines=spu2)
        log("native %-9s %s, mark at %.1f s wall; %s" % (name, status, t_mark, notes[name]["spu2"]))
    return notes


# ---- WAV / analysis ----------------------------------------------------------

def read_wav(path):
    with wave.open(path) as w:
        if w.getframerate() != RATE or w.getsampwidth() != 2:
            raise SystemExit("%s: need 48 kHz 16-bit" % path)
        ch = w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2].astype(np.float32)


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.clip(np.round(x), -32768, 32767).astype("<i2").tobytes())


def mono(x):
    return x.mean(axis=1)


def ncc_search(ref, sig, decim):
    """Normalised cross-correlation of ref against every position of sig, at
    1/decim of the rate (boxcar). Returns (positions*decim, ncc)."""
    def dec(v):
        n = len(v) // decim * decim
        return v[:n].reshape(-1, decim).mean(axis=1)
    r = dec(ref).astype(np.float64)
    s = dec(sig).astype(np.float64)
    r = r - r.mean()
    if len(s) < len(r):
        return np.array([0]), np.array([0.0])
    n = 1
    while n < len(s) + len(r):
        n *= 2
    corr = np.fft.irfft(np.fft.rfft(s, n) * np.conj(np.fft.rfft(r, n)), n)[:len(s) - len(r) + 1]
    c = np.concatenate([[0.0], np.cumsum(s)])
    c2 = np.concatenate([[0.0], np.cumsum(s * s)])
    m = len(r)
    win_sum = c[m:] - c[:-m]
    win_sq = c2[m:] - c2[:-m]
    var = np.maximum(win_sq - win_sum * win_sum / m, 1e-9)
    ncc = corr / np.sqrt(var * max((r * r).sum(), 1e-9))
    return np.arange(len(ncc)) * decim, ncc


def envelope(x, block=480):
    m = mono(x).astype(np.float64)
    n = len(m) // block * block
    return np.sqrt((m[:n].reshape(-1, block) ** 2).mean(axis=1) + 1.0)


def locate(ref, sig, lo=0, hi=None):
    """Offset of ref (a window) inside sig[lo:hi]: waveform NCC at 8 kHz,
    refined at 48 kHz; envelope NCC (10 ms) when the waveforms do not
    correlate. Returns (offset in samples, waveform ncc, envelope ncc)."""
    hi = len(sig) if hi is None else min(hi, len(sig))
    lo = max(0, lo)
    sub = sig[lo:hi]
    rm, sm = mono(ref), mono(sub)
    pos, ncc = ncc_search(rm, sm, 6)
    best = int(np.argmax(ncc))
    off, wav_ncc = int(pos[best]), float(ncc[best])
    # Envelope match (robust against phase / filtering differences).
    er, es = np.log(envelope(ref)), np.log(envelope(sub))
    epos, encc = ncc_search(er, es, 1)
    eb = int(np.argmax(encc))
    env_off, env_ncc = int(epos[eb]) * 480, float(encc[eb])
    if wav_ncc < 0.3 and env_ncc > wav_ncc:
        off = env_off
    else:
        # Refine to the sample on a 1 s chunk from the middle.
        a = len(rm) // 2 - RATE // 2
        chunk = rm[max(a, 0):max(a, 0) + RATE]
        lo2 = max(0, off + max(a, 0) - 12)
        seg = sm[lo2:lo2 + len(chunk) + 24]
        if len(seg) >= len(chunk):
            p2, n2 = ncc_search(chunk, seg, 1)
            k = int(np.argmax(n2))
            off = lo2 + int(p2[k]) - max(a, 0)
    return lo + off, wav_ncc, env_ncc


def speed_ratio(ref, sig, off):
    """Playback speed of sig relative to ref around the aligned offset: the
    start and the end of the window are matched separately."""
    L = len(ref)
    c = 2 * RATE
    if L < 4 * c:
        return None, None
    starts = (int(0.15 * L), L - int(0.15 * L) - c)
    found = []
    for s in starts:
        chunk = ref[s:s + c]
        lo = off + s - int(0.03 * L) - RATE // 10
        hi = off + s + c + int(0.03 * L) + RATE // 10
        o, w, e = locate(chunk, sig, lo, hi)
        found.append((o, max(w, e)))
    (o1, q1), (o2, q2) = found
    return (o2 - o1) / float(starts[1] - starts[0]), min(q1, q2)


def clicks(x, floor=2000.0, k=8.0):
    """Sample-to-sample discontinuities: |second difference| above k times
    its 10 ms RMS and above `floor`; clustered within 2 ms."""
    hits = np.zeros(len(x), dtype=bool)
    for c in range(2):
        v = x[:, c].astype(np.float64)
        d2 = np.zeros_like(v)
        d2[2:] = v[2:] - 2 * v[1:-1] + v[:-2]
        sq = np.concatenate([[0.0], np.cumsum(d2 * d2)])
        w = 480
        if len(v) <= w:
            continue
        local = np.sqrt((sq[w:] - sq[:-w]) / w)
        local = np.concatenate([np.full(w // 2, local[0]), local, np.full(len(v) - len(local) - w // 2, local[-1])])
        hits |= (np.abs(d2) > floor) & (np.abs(d2) > k * local)
    idx = np.flatnonzero(hits)
    if len(idx) == 0:
        return []
    keep = [idx[0]]
    for i in idx[1:]:
        if i - keep[-1] > 96:
            keep.append(i)
    return keep


def dropouts(x):
    """Runs of >= 2 ms of digital silence (|s| <= 2) with sound (> -50 dBFS)
    in the 50 ms before and after. Returns [(start, length)]."""
    blk = 48
    m = np.abs(x).max(axis=1)
    n = len(m) // blk
    if n < 4:
        return []
    bmax = m[:n * blk].reshape(n, blk).max(axis=1)
    rms = np.sqrt((mono(x)[:n * blk].astype(np.float64).reshape(n, blk) ** 2).mean(axis=1))
    silent = bmax <= 2
    out = []
    i = 0
    loud = 32768 * 10 ** (-50 / 20)
    while i < n:
        if silent[i]:
            j = i
            while j < n and silent[j]:
                j += 1
            if j - i >= 2:
                before = rms[max(0, i - 50):i]
                after = rms[j:j + 50]
                if len(before) and len(after) and before.mean() > loud and after.mean() > loud:
                    out.append((i * blk, (j - i) * blk))
            i = j
        else:
            i += 1
    return out


OCTAVES = [63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]


def band_levels(x):
    m = mono(x).astype(np.float64)
    nper = 4096
    if len(m) < nper:
        return [None] * len(OCTAVES)
    win = np.hanning(nper)
    hop = nper // 2
    frames = (len(m) - nper) // hop + 1
    psd = np.zeros(nper // 2 + 1)
    for i in range(frames):
        seg = m[i * hop:i * hop + nper] * win
        psd += np.abs(np.fft.rfft(seg)) ** 2
    psd /= max(frames, 1)
    freqs = np.fft.rfftfreq(nper, 1.0 / RATE)
    total = psd.sum() + 1e-12
    out = []
    for f in OCTAVES:
        sel = (freqs >= f / np.sqrt(2)) & (freqs < f * np.sqrt(2))
        out.append(10 * np.log10(psd[sel].sum() / total + 1e-12))
    return out


def measure(x):
    secs = len(x) / RATE
    full = np.abs(x) >= 32767
    clip_runs = 0
    for c in range(2):
        f = full[:, c].astype(np.int8)
        starts = np.flatnonzero(np.diff(np.concatenate([[0], f])) == 1)
        ends = np.flatnonzero(np.diff(np.concatenate([f, [0]])) == -1)
        clip_runs += int(((ends - starts + 1) >= 2).sum())
    rms = float(np.sqrt((x.astype(np.float64) ** 2).mean()))
    side = float(np.sqrt(((x[:, 0] - x[:, 1]).astype(np.float64) ** 2).mean()) / 2)
    mid = float(np.sqrt(((x[:, 0] + x[:, 1]).astype(np.float64) ** 2).mean()) / 2)
    ck = clicks(x)
    do = dropouts(x)
    return dict(
        secs=secs,
        rms_db=20 * np.log10(max(rms, 1e-3) / 32768),
        peak=int(np.abs(x).max()) if len(x) else 0,
        clipped=int(full.sum()),
        clip_runs=clip_runs,
        clicks=len(ck), clicks_ps=len(ck) / secs if secs else 0,
        click_times=[round(float(i) / RATE, 3) for i in ck[:40]],
        dropouts=len(do), dropouts_ps=len(do) / secs if secs else 0,
        dropout_ms=sum(l for _, l in do) * 1000 / RATE,
        dropout_times=[round(float(s) / RATE, 3) for s, _ in do[:40]],
        side_db=20 * np.log10(max(side, 1e-3) / max(mid, 1e-3)),
        bands=band_levels(x),
    )


def envelope_similarity(a, b):
    ea, eb = np.log(envelope(a)), np.log(envelope(b))
    n = min(len(ea), len(eb))
    if n < 10:
        return 0.0
    ea, eb = ea[:n] - ea[:n].mean(), eb[:n] - eb[:n].mean()
    return float((ea * eb).sum() / np.sqrt((ea * ea).sum() * (eb * eb).sum() + 1e-12))


def analyze_scene(name, raw, out_dir, secs, notes):
    paths = {s: os.path.join(raw, "%s-%s.wav" % (name, s)) for s in SOURCES}
    have = {s: p for s, p in paths.items() if os.path.exists(p)}
    if "pcsx2" not in have:
        log("%s: no PCSX2 recording" % name)
        return None
    px = read_wav(have["pcsx2"])
    # Reference window: secs from 1 s into the PCSX2 recording (after the
    # savestate load settles), or the whole recording if shorter.
    start = min(RATE, max(0, len(px) - int(secs * RATE)))
    ref = px[start:start + int(secs * RATE)]
    result = dict(scene=name, desc=SCENES.get(name, {}).get("desc", ""), sources={})
    windows = {"pcsx2": ref}
    result["sources"]["pcsx2"] = dict(offset=start / RATE, ncc=1.0, env_ncc=1.0, speed=1.0, **measure(ref))
    nat = (notes or {}).get(name) or {}
    sc = SCENES.get(name, {})
    for s in ("tap", "host"):
        if s not in have:
            continue
        sig = read_wav(have[s])
        if len(sig) < len(ref):
            log("%s %s: recording shorter than the window" % (name, s))
            continue
        # Expected position of the PCSX2 window: the native mark (+1 s like
        # the PCSX2 window after a savestate load). Search around it.
        mark = None
        if s == "tap" and nat.get("mark_tap") is not None:
            mark = nat["mark_tap"]
        elif s == "host" and nat.get("mark_wall") is not None and nat.get("rec_wall") is not None:
            mark = nat["mark_wall"] - nat["rec_wall"]
        lo, hi = 0, len(sig)
        if mark is not None:
            before = 20 if sc.get("px") == "boot" else 3
            lo = max(0, int((mark - before) * RATE))
            hi = min(len(sig), int((mark + sc.get("after", 10) + secs + 2) * RATE))
        if hi - lo < len(ref):
            lo, hi = max(0, len(sig) - len(ref)), len(sig)
        off, wncc, encc = locate(ref, sig, lo, hi)
        matched = max(wncc, encc) >= 0.5
        if not matched and mark is not None:
            off = min(max(0, int((mark + 1.0) * RATE)), len(sig) - len(ref))
        sp, q = speed_ratio(ref, sig, off) if matched else (None, None)
        n = int(len(ref) * (sp if sp and 0.9 < sp < 1.1 else 1.0))
        win = sig[off:off + n]
        if len(win) < len(ref) // 2:
            log("%s %s: no usable window" % (name, s))
            continue
        windows[s] = win
        m = measure(win)
        result["sources"][s] = dict(offset=off / RATE, ncc=wncc, env_ncc=encc, matched=matched, speed=sp, speed_q=q,
                                    env_sim=envelope_similarity(ref, win[:len(ref)]), **m)
    for s, w in windows.items():
        write_wav(os.path.join(out_dir, "%s-%s.wav" % (name, s)), w)
    if notes:
        result["native"] = notes.get(name)
    render_png(os.path.join(out_dir, "%s.png" % name), name, result, windows)
    return result


def fmt_report(results, header):
    lines = [header, ""]
    for r in results:
        if not r:
            continue
        lines.append("== %s: %s" % (r["scene"], r["desc"]))
        px = r["sources"]["pcsx2"]
        if px["dropout_ms"] > 100 or px["clicks_ps"] > 1.0:
            lines.append("   WARNING: the PCSX2 recording itself has %.0f ms of dropouts, %.2f clicks/s"
                         " (PCSX2 slowed down: Mac loaded?); run this scene again" % (px["dropout_ms"], px["clicks_ps"]))
        nat = r.get("native")
        if nat:
            lines.append("   native: %s, mark %.1f s wall, load %s; %s" % (nat.get("status"), nat.get("mark_wall", 0),
                                                                        nat.get("load", "?"), nat.get("spu2", "")))
            if nat.get("appaudio"):
                lines.append("   host recorder: %s" % nat["appaudio"])
        lines.append("   %-6s %8s %7s %6s %8s %9s %8s %9s %8s %8s %7s %7s %7s" % (
            "source", "at(s)", "match", "env", "speed", "rms dBFS", "peak", "clipped", "clk/s", "drop/s", "drop ms", "side", "envsim"))
        for s in SOURCES:
            d = r["sources"].get(s)
            if not d:
                continue
            lines.append("   %-6s %8.2f %7.3f %6.3f%s%8s %9.1f %8d %9d %8.2f %8.2f %7.0f %7.1f %7s" % (
                s, d["offset"], d["ncc"], d["env_ncc"], " " if d.get("matched", True) else "!", "%.4f" % d["speed"] if d.get("speed") else "-",
                d["rms_db"], d["peak"], d["clipped"], d["clicks_ps"], d["dropouts_ps"], d["dropout_ms"],
                d["side_db"], "%.3f" % d["env_sim"] if "env_sim" in d else "-"))
        lines.append("   octave bands, dB relative to the total (" + " ".join("%5s" % (f if f < 1000 else "%dk" % (f // 1000)) for f in OCTAVES) + ")")
        base = r["sources"]["pcsx2"]["bands"]
        for s in SOURCES:
            d = r["sources"].get(s)
            if not d:
                continue
            row = " ".join("%5.1f" % b if b is not None else "    -" for b in d["bands"])
            if s != "pcsx2":
                diff = " ".join("%+5.1f" % (b - a) if b is not None and a is not None else "    -"
                                for a, b in zip(base, d["bands"]))
                row += "   vs pcsx2: " + diff
            lines.append("   %-6s %s" % (s, row))
        for s in SOURCES:
            d = r["sources"].get(s)
            if d and (d["click_times"] or d["dropout_times"]):
                lines.append("   %-6s clicks at %s%s; dropouts at %s" % (
                    s, d["click_times"][:12], " ..." if d["clicks"] > 12 else "", d["dropout_times"][:12]))
        lines.append("")
    lines.append("columns: at = window start in that recording; match = waveform NCC (1 = identical")
    lines.append("waveform, < 0.3 = matched on the envelope); env = envelope NCC (! = no match:")
    lines.append("window taken at the native mark + 1 s); speed = duration")
    lines.append("of the window in that source / PCSX2's; clipped = samples at full scale; clk/s =")
    lines.append("discontinuities per second; drop/s, drop ms = digital-silence gaps inside sound;")
    lines.append("side = side/mid level in dB (stereo width); envsim = 10 ms envelope correlation")
    lines.append("with PCSX2 over the window.")
    return "\n".join(lines)


# ---- PNG ---------------------------------------------------------------------

FONT = {  # 5x7, rows top to bottom, 5 bits each
    "A": "0E 11 11 1F 11 11 11", "B": "1E 11 11 1E 11 11 1E", "C": "0E 11 10 10 10 11 0E",
    "D": "1E 11 11 11 11 11 1E", "E": "1F 10 10 1E 10 10 1F", "F": "1F 10 10 1E 10 10 10",
    "G": "0E 11 10 17 11 11 0F", "H": "11 11 11 1F 11 11 11", "I": "0E 04 04 04 04 04 0E",
    "J": "07 02 02 02 02 12 0C", "K": "11 12 14 18 14 12 11", "L": "10 10 10 10 10 10 1F",
    "M": "11 1B 15 15 11 11 11", "N": "11 11 19 15 13 11 11", "O": "0E 11 11 11 11 11 0E",
    "P": "1E 11 11 1E 10 10 10", "Q": "0E 11 11 11 15 12 0D", "R": "1E 11 11 1E 14 12 11",
    "S": "0F 10 10 0E 01 01 1E", "T": "1F 04 04 04 04 04 04", "U": "11 11 11 11 11 11 0E",
    "V": "11 11 11 11 11 0A 04", "W": "11 11 11 15 15 15 0A", "X": "11 11 0A 04 0A 11 11",
    "Y": "11 11 0A 04 04 04 04", "Z": "1F 01 02 04 08 10 1F",
    "0": "0E 11 13 15 19 11 0E", "1": "04 0C 04 04 04 04 0E", "2": "0E 11 01 02 04 08 1F",
    "3": "1F 02 04 02 01 11 0E", "4": "02 06 0A 12 1F 02 02", "5": "1F 10 1E 01 01 11 0E",
    "6": "06 08 10 1E 11 11 0E", "7": "1F 01 02 04 08 08 08", "8": "0E 11 11 0E 11 11 0E",
    "9": "0E 11 11 0F 01 02 0C", " ": "00 00 00 00 00 00 00", ".": "00 00 00 00 00 0C 0C",
    "-": "00 00 00 1F 00 00 00", ":": "00 0C 0C 00 0C 0C 00", "%": "18 19 02 04 08 13 03",
    "/": "00 01 02 04 08 10 00", "(": "02 04 08 08 08 04 02", ")": "08 04 02 02 02 04 08",
    "+": "00 04 04 1F 04 04 00", ",": "00 00 00 00 0C 04 08", "=": "00 00 1F 00 1F 00 00",
    "_": "00 00 00 00 00 00 1F", ">": "08 04 02 01 02 04 08", "<": "02 04 08 10 08 04 02",
    "|": "04 04 04 04 04 04 04",
}


def draw_text(img, x, y, text, color=(230, 230, 230), scale=2):
    for ch in text.upper():
        rows = FONT.get(ch, FONT[" "]).split()
        for ry, row in enumerate(rows):
            bits = int(row, 16)
            for rx in range(5):
                if bits & (0x10 >> rx):
                    img[y + ry * scale:y + (ry + 1) * scale, x + rx * scale:x + (rx + 1) * scale] = color
        x += 6 * scale


def colormap(v):
    """v in [0,1] -> RGB (dark blue, purple, orange, yellow)."""
    stops = np.array([[0, 0, 4], [40, 11, 84], [136, 34, 106], [212, 72, 66], [249, 142, 9], [252, 255, 164]], float)
    p = np.clip(v, 0, 1) * (len(stops) - 1)
    i = np.minimum(p.astype(int), len(stops) - 2)
    f = (p - i)[..., None]
    return (stops[i] * (1 - f) + stops[i + 1] * f).astype(np.uint8)


def spectrogram(x, width, height, lo_db=-100.0, hi_db=-10.0):
    m = mono(x).astype(np.float64) / 32768.0
    nfft = 2048
    win = np.hanning(nfft)
    hop = max(1, (len(m) - nfft) // max(width - 1, 1))
    cols = []
    for i in range(width):
        s = i * hop
        seg = m[s:s + nfft]
        if len(seg) < nfft:
            seg = np.pad(seg, (0, nfft - len(seg)))
        cols.append(np.abs(np.fft.rfft(seg * win)) / (nfft / 4))
    spec = np.array(cols).T  # freq x time
    freqs = np.fft.rfftfreq(nfft, 1.0 / RATE)
    # Log frequency axis 40 Hz .. 24 kHz, top = high.
    edges = np.geomspace(40, 24000, height + 1)
    img = np.zeros((height, width))
    for r in range(height):
        sel = (freqs >= edges[r]) & (freqs < edges[r + 1])
        if not sel.any():
            k = np.searchsorted(freqs, edges[r])
            sel = slice(min(k, len(freqs) - 1), min(k, len(freqs) - 1) + 1)
        img[height - 1 - r] = spec[sel].max(axis=0)
    db = 20 * np.log10(img + 1e-9)
    return colormap((db - lo_db) / (hi_db - lo_db))


def waveform(x, width, height):
    img = np.full((height, width, 3), 16, np.uint8)
    n = len(x)
    mid = height // 2
    img[mid, :] = (60, 60, 60)
    for c, color in ((0, (90, 170, 255)), (1, (255, 150, 90))):
        v = x[:, c]
        for i in range(width):
            a, b = i * n // width, max((i + 1) * n // width, i * n // width + 1)
            seg = v[a:b]
            if not len(seg):
                continue
            lo, hi = seg.min() / 32768.0, seg.max() / 32768.0
            y0 = int(mid - hi * (mid - 1))
            y1 = int(mid - lo * (mid - 1))
            img[max(y0, 0):min(y1 + 1, height), i] = color if c == 0 else np.maximum(img[max(y0, 0):min(y1 + 1, height), i], color) // 1
        clip = np.abs(v) >= 32767
        for i in np.unique(np.flatnonzero(clip) * width // max(n, 1)):
            img[0:3, min(i, width - 1)] = (255, 40, 40)
            img[height - 3:height, min(i, width - 1)] = (255, 40, 40)
    return img


def write_png(path, img):
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def render_png(path, name, result, windows):
    W, WAVE_H, SPEC_H, LABEL_H, PAD = 1400, 90, 220, 26, 10
    order = [s for s in SOURCES if s in windows]
    H = 40 + len(order) * (LABEL_H + WAVE_H + SPEC_H + PAD) + 30
    img = np.full((H, W + 2 * PAD, 3), 24, np.uint8)
    draw_text(img, PAD, 12, "%s: %s" % (name, result["desc"]), scale=2)
    y = 40
    dur = len(windows["pcsx2"]) / RATE
    for s in order:
        d = result["sources"][s]
        label = "%s  rms %.1f dBFS  peak %d  clipped %d  clicks/s %.2f  dropouts/s %.2f" % (
            s, d["rms_db"], d["peak"], d["clipped"], d["clicks_ps"], d["dropouts_ps"])
        if s != "pcsx2" and d.get("speed"):
            label += "  speed %.4f  match %.2f" % (d["speed"], max(d["ncc"], d["env_ncc"]))
        draw_text(img, PAD, y + 6, label, scale=2)
        y += LABEL_H
        x = windows[s][:len(windows["pcsx2"])] if s != "pcsx2" else windows[s]
        img[y:y + WAVE_H, PAD:PAD + W] = waveform(x, W, WAVE_H)
        y += WAVE_H
        img[y:y + SPEC_H, PAD:PAD + W] = spectrogram(x, W, SPEC_H)
        # Clicks and dropouts as ticks above the spectrogram.
        for t in d["click_times"]:
            i = PAD + min(int(t / dur * W), W - 1)
            img[y:y + 8, i] = (255, 255, 255)
        for t in d["dropout_times"]:
            i = PAD + min(int(t / dur * W), W - 1)
            img[y:y + 14, i] = (0, 255, 120)
        y += SPEC_H + PAD
    draw_text(img, PAD, y + 4, "0 S .. %.1f S | WAVE: L BLUE R ORANGE, RED = FULL SCALE | SPECTROGRAM 40 HZ - 24 KHZ LOG, -100..-10 DBFS | TICKS: WHITE CLICK, GREEN DROPOUT" % dur, scale=1)
    write_png(path, img)


# ---- commands ----------------------------------------------------------------

def run(args):
    name = args.name
    scenes = args.scenes.split(",")
    for s in scenes:
        if s not in SCENES:
            raise SystemExit("unknown scene %s (have %s)" % (s, ",".join(SCENES)))
    raw = os.path.join(SCRATCH, "raw-" + name)
    out_dir = os.path.join(OUT_ROOT, name)
    os.makedirs(raw, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    notes = {}
    if not args.native_only:
        record_pcsx2(scenes, raw, args.secs)
    else:
        src = os.environ.get("AUDIOCHECK_PCSX2_FROM")
        if src:
            for s in scenes:
                p = os.path.join(SCRATCH, "raw-" + src, "%s-pcsx2.wav" % s)
                if os.path.exists(p):
                    shutil.copy(p, raw)
    if not args.pcsx2_only:
        notes = record_native(scenes, raw, out_dir, args.secs, name)
        json.dump(notes, open(os.path.join(raw, "native.json"), "w"), indent=1)
    finish(name, scenes, raw, out_dir, args.secs, notes, args.keep_raw)


def finish(name, scenes, raw, out_dir, secs, notes, keep_raw):
    results = [analyze_scene(s, raw, out_dir, secs, notes) for s in scenes]
    header = "audiocheck %s (%s), runner %s" % (name, time.strftime("%Y-%m-%d %H:%M"), RUNNER)
    text = fmt_report(results, header)
    open(os.path.join(out_dir, "report.txt"), "w").write(text + "\n")
    json.dump([r for r in results if r], open(os.path.join(out_dir, "report.json"), "w"), indent=1, default=float)
    print(text)
    log("\nwrote %s (report.txt, <scene>.png, aligned WAVs)" % out_dir)
    if not keep_raw:
        shutil.rmtree(raw, ignore_errors=True)
    else:
        log("raw recordings kept in %s" % raw)


def analyze(args):
    raw = os.path.join(SCRATCH, "raw-" + args.name)
    out_dir = os.path.join(OUT_ROOT, args.name)
    os.makedirs(out_dir, exist_ok=True)
    notes = {}
    p = os.path.join(raw, "native.json")
    if os.path.exists(p):
        notes = json.load(open(p))
    scenes = [s for s in (args.scenes.split(",") if args.scenes else SCENES)
              if os.path.exists(os.path.join(raw, "%s-pcsx2.wav" % s))]
    finish(args.name, scenes, raw, out_dir, args.secs, notes, True)


def wavs(args):
    ref = read_wav(args.files[0])
    for f in args.files:
        x = read_wav(f)
        m = measure(x)
        extra = ""
        if f != args.files[0] and len(x) >= len(ref):
            off, w, e = locate(ref, x)
            extra = "  at %.3f s match %.3f env %.3f" % (off / RATE, w, e)
        print("%s: %.1f s rms %.1f dBFS peak %d clipped %d clicks/s %.2f dropouts/s %.2f (%.0f ms)%s" % (
            f, m["secs"], m["rms_db"], m["peak"], m["clipped"], m["clicks_ps"], m["dropouts_ps"], m["dropout_ms"], extra))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("name")
    r.add_argument("--scenes", default=",".join(SCENES))
    r.add_argument("--secs", type=float, default=12.0)
    r.add_argument("--keep-raw", action="store_true")
    r.add_argument("--native-only", action="store_true")
    r.add_argument("--pcsx2-only", action="store_true")
    a = sub.add_parser("analyze")
    a.add_argument("name")
    a.add_argument("--scenes", default="")
    a.add_argument("--secs", type=float, default=12.0)
    w = sub.add_parser("wav")
    w.add_argument("files", nargs="+")
    args = ap.parse_args()
    {"run": run, "analyze": analyze, "wav": wavs}[args.cmd](args)


if __name__ == "__main__":
    main()
