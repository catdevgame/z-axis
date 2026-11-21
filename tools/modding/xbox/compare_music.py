"""PS2 MUSIC.ZSD song (PS-ADPCM, stereo in 0x800-byte L/R pieces, 44.1 kHz)
vs the Xbox WMA of the same song: decode an excerpt of each, compare spectra.

    .venv/bin/python -I tools/compare_music.py 0 --start 60 --secs 20 --out /tmp/ai-xbox-compare

Song index = MUSIC.ZSD wave = Xbox music.zsd wave (same order, the 13
licensed songs). Writes <out>/music_<n>_ps2.wav, _xbox.wav and a spectrum
plot PNG; prints the -60 dB bandwidth of each. Disc files are data only.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env)
PS2 = os.path.join(zpaths.AI_WORKSPACE, "recomp", "disc-root", "ASSETS", "AUDIO", "MUSIC.ZSD")
XBOX = os.path.join(zpaths.XBOX_DATA, "disc", "media", "assets", "audio")

F = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def zsd_waves(path: str):
    d = open(path, "rb").read(0x10000)
    count, table = struct.unpack_from("<II", d, 0x20)
    if d[:8] == b"ZSNDPS2 ":
        return [struct.unpack_from("<II", d, table + 8 * i) for i in range(count)]
    out = []
    for i in range(count):
        e = d[table + 84 * i : table + 84 * (i + 1)]
        off, size, flags = struct.unpack_from("<III", e)
        out.append((off, size, flags, e[20:].split(b"\0")[0].decode()))
    return out


def psx_adpcm(blocks: bytes) -> np.ndarray:
    """Decode PS-ADPCM (16-byte blocks, 28 samples each) to int16."""
    n = len(blocks) // 16
    out = np.empty(n * 28, np.int32)
    s1 = s2 = 0
    b = np.frombuffer(blocks, np.uint8, n * 16).reshape(n, 16)
    nib = np.empty((n, 28), np.int32)
    nib[:, 0::2] = b[:, 2:] & 15
    nib[:, 1::2] = b[:, 2:] >> 4
    nib = np.where(nib >= 8, nib - 16, nib)
    shift = (b[:, 0] & 15).astype(np.int32)
    filt = np.minimum((b[:, 0] >> 4).astype(np.int32), 4)
    raw = (nib << 12) >> shift[:, None]
    k = 0
    for i in range(n):
        f0, f1 = F[filt[i]]
        r = raw[i]
        for j in range(28):
            s = int(r[j]) + ((s1 * f0 + s2 * f1 + 32) >> 6)
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out[k] = s
            k += 1
            s2, s1 = s1, s
    return out.astype(np.int16)


def ps2_song(index: int, start: float, secs: float, rate: int = 44100):
    off, size = zsd_waves(PS2)[index]
    piece = 0x800  # bytes per channel piece
    samples_per_piece = piece // 16 * 28
    first = int(start * rate) // samples_per_piece
    count = int(secs * rate) // samples_per_piece + 1
    with open(PS2, "rb") as f:
        f.seek(off + first * 2 * piece)
        data = f.read(count * 2 * piece)
    pieces = np.frombuffer(data, np.uint8)[: len(data) // (2 * piece) * 2 * piece].reshape(-1, 2, piece)
    left = psx_adpcm(pieces[:, 0].tobytes())
    right = psx_adpcm(pieces[:, 1].tobytes())
    return np.stack([left, right], 1)


def xbox_song(index: int, start: float, secs: float):
    name = zsd_waves(os.path.join(XBOX, "music.zsd"))[index][3]
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(start), "-t", str(secs), "-i", os.path.join(XBOX, name),
         "-f", "s16le", "-ac", "2", "-ar", "44100", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(pcm, np.int16).reshape(-1, 2), name


def write_wav(path, pcm, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.astype("<i2").tobytes())


def spectrum(pcm, rate=44100, n=4096):
    x = pcm.astype(np.float64).mean(1)
    frames = len(x) // n
    win = np.hanning(n)
    acc = np.zeros(n // 2 + 1)
    for i in range(frames):
        acc += np.abs(np.fft.rfft(x[i * n : (i + 1) * n] * win)) ** 2
    db = 10 * np.log10(acc / max(1, frames) + 1e-9)
    return np.fft.rfftfreq(n, 1 / rate), db


def bandwidth(freqs, db, drop=60):
    peak = db[(freqs > 50) & (freqs < 5000)].max()
    above = np.where(db > peak - drop)[0]
    return freqs[above.max()] if len(above) else 0


def plot(freqs, curves, out, title):
    from PIL import Image, ImageDraw

    W, H, M = 900, 420, 40
    img = Image.new("RGB", (W, H), (16, 16, 16))
    d = ImageDraw.Draw(img)
    top = max(c.max() for _, c, _ in curves)
    lo = top - 100
    for k in range(0, 23, 2):
        x = M + (W - 2 * M) * (k * 1000) / 22050
        d.line([(x, M), (x, H - M)], fill=(50, 50, 50))
        d.text((x - 6, H - M + 4), f"{k}k", fill=(160, 160, 160))
    for db in range(0, 101, 20):
        y = M + (H - 2 * M) * db / 100
        d.line([(M, y), (W - M, y)], fill=(50, 50, 50))
        d.text((4, y - 6), f"-{db}", fill=(160, 160, 160))
    for label, c, colour in curves:
        pts = [(M + (W - 2 * M) * f / 22050, M + (H - 2 * M) * min(1, max(0, (top - v) / 100))) for f, v in zip(freqs, c)]
        d.line(pts, fill=colour, width=1)
    y = 8
    d.text((M, y), title, fill=(255, 255, 0))
    for label, _, colour in curves:
        y += 0
    d.text((W - 360, 8), "  ".join(lbl for lbl, _, _ in curves), fill=(220, 220, 220))
    for i, (label, _, colour) in enumerate(curves):
        d.rectangle([W - 380, 10 + 12 * i, W - 370, 18 + 12 * i], fill=colour)
    img.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("song", type=int)
    ap.add_argument("--start", type=float, default=60)
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--out", default="/tmp/ai-xbox-compare")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    p = ps2_song(a.song, a.start, a.secs)
    x, name = xbox_song(a.song, a.start, a.secs)
    write_wav(os.path.join(a.out, f"music_{a.song:02d}_ps2.wav"), p)
    write_wav(os.path.join(a.out, f"music_{a.song:02d}_xbox.wav"), x)
    fp, dp = spectrum(p)
    fx, dx = spectrum(x)
    bp, bx = bandwidth(fp, dp), bandwidth(fx, dx)
    rms = lambda s: 20 * np.log10(np.sqrt((s.astype(np.float64) ** 2).mean()) / 32768 + 1e-12)  # noqa: E731
    print(f"song {a.song} ({name}): PS2 -60dB bandwidth {bp / 1000:.1f} kHz, RMS {rms(p):.1f} dBFS; "
          f"Xbox WMA {bx / 1000:.1f} kHz, RMS {rms(x):.1f} dBFS")
    for cut in (12000, 15000, 16000, 18000, 20000):
        i = np.searchsorted(fp, cut)
        print(f"  at {cut / 1000:.0f} kHz: PS2 {dp[i] - dp.max():6.1f} dB, Xbox {dx[i] - dx.max():6.1f} dB (rel. peak)")
    plot(fp, [("PS2 ADPCM", dp, (255, 120, 80)), ("Xbox WMA", dx, (80, 180, 255))],
         os.path.join(a.out, f"music_{a.song:02d}_spectrum.png"), f"song {a.song}: {name}, {a.secs:.0f} s from {a.start:.0f} s")


if __name__ == "__main__":
    main()
