#!/usr/bin/env python3
# zaxis_texdump.py: dump and re-import the textures of Z-Axis' Aggressive
# Inline, Dave Mirra Freestyle BMX 2 and BMX XXX (PS2, Xbox and GameCube).
#
# MIT License
#
# Copyright (c) 2026 the Aggressive Inline PC project
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
"""Dump (and put back) the textures of Aggressive Inline, Dave Mirra
Freestyle BMX 2 and BMX XXX from a backup of your own disc.

  dump (the default command):
    zaxis_texdump.py IMAGE OUT [--game ai|dm2|bmxxxx] [--png|--raw|--both]
                     [--only PATTERN] [--list] [--no-mips] [--keep-key]

  put edited PNGs back into the game's files (a modified copy of each file):
    zaxis_texdump.py import DUMP MODS [--image IMAGE] [--resize] [--only PATTERN]

  write those files into a copy of the disc image:
    zaxis_texdump.py patch-image MODS IMAGE NEW_IMAGE

IMAGE is a PS2 ISO (or a raw .bin CD image), an Xbox XISO, a GameCube RVZ or
GCM/ISO, or a folder with a disc's extracted files. Python 3.8+, standard
library only (a zstd-compressed RVZ also needs Python 3.14+, the zstandard
package or the zstd command). The image is only ever read.
"""
import argparse
import bz2
import fnmatch
import hashlib
import json
import lzma
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from collections import OrderedDict

VERSION = 1
GAMES = {"ai": "Aggressive Inline", "dm2": "Dave Mirra Freestyle BMX 2", "bmxxxx": "BMX XXX"}
CONSOLES = {"ps2": "PS2", "xbox": "Xbox", "gc": "GameCube", None: "unknown console"}
# Movies and sound: never hold textures, skipped for speed.
SKIP_EXT = (".pss", ".psm", ".bik", ".h4m", ".xmv", ".wma", ".zsd", ".vag", ".adx", ".irx", ".sfd")
MANIFEST = "zaxis_texdump.json"
MOD_MANIFEST = "zaxis_texdump-mods.json"


class DumpError(Exception):
    pass


def u32(d, at):
    return struct.unpack_from("<I", d, at)[0]


# ===========================================================================
# Disc images. Each source lists its files as (path, size, handle) and reads a
# file by handle. Paths use "/" and the disc's own case.
# ===========================================================================

class FolderSource:
    """A disc's extracted files in a folder."""

    console = None
    kind = "folder"

    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.files = []
        for folder, dirs, names in os.walk(self.root):
            dirs.sort()
            for name in sorted(names):
                full = os.path.join(folder, name)
                rel = os.path.relpath(full, self.root).replace(os.sep, "/")
                if rel in (MANIFEST, MOD_MANIFEST):
                    continue
                self.files.append((rel, os.path.getsize(full), full))
        lower = {p.lower() for p, _, _ in self.files}
        if "system.cnf" in lower:
            self.console = "ps2"
        elif "default.xbe" in lower or "media/default.xbe" in lower:
            self.console = "xbox"
        elif any(p.endswith(".dol") for p in lower):
            self.console = "gc"

    def read(self, handle, size):
        with open(handle, "rb") as f:
            return f.read()

    def close(self):
        pass


class SectorFile:
    """A disc image read in 2048-byte sectors: plain (ISO) or raw 2352-byte CD
    sectors (a .bin: mode 1 user data at +16, mode 2 form 1 at +24)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.raw = 0
        self.user = 0
        head = self.f.read(16)
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            self.raw = 2352
            self.user = 24 if head[15] == 2 else 16
        self.size = os.fstat(self.f.fileno()).st_size

    def sectors(self):
        return self.size // (self.raw or 2048)

    def read(self, sector, size):
        if not self.raw:
            self.f.seek(sector * 2048)
            return self.f.read(size)
        out = bytearray()
        while len(out) < size:
            self.f.seek(sector * self.raw + self.user)
            out += self.f.read(min(2048, size - len(out)))
            sector += 1
        return bytes(out)

    def close(self):
        self.f.close()


class IsoSource:
    """ISO9660 (the PS2 discs: DVD images, and CD images as .bin)."""

    console = "ps2"
    kind = "iso"

    def __init__(self, path):
        self.disc = SectorFile(path)
        self.kind = "bin" if self.disc.raw else "iso"
        pvd = self.disc.read(16, 2048)
        if pvd[1:6] != b"CD001":
            raise DumpError("no ISO9660 volume")
        self.files = []
        self.records = {}   # path -> byte offset of its directory record (in 2048-byte sectors)
        root = self._record(pvd, 156)
        self._walk(root[1], root[2], "", set())

    @staticmethod
    def _record(buf, at):
        extent = u32(buf, at + 2)
        size = u32(buf, at + 10)
        flags = buf[at + 25]
        n = buf[at + 32]
        name = buf[at + 33:at + 33 + n].decode("latin-1").split(";")[0]
        if name.endswith(".") and len(name) > 1:
            name = name[:-1]
        return buf[at], extent, size, flags, name

    def _walk(self, extent, size, prefix, seen):
        if extent in seen:
            return
        seen.add(extent)
        buf = self.disc.read(extent, size)
        at = 0
        while at < len(buf):
            if buf[at] == 0:
                at = (at // 2048 + 1) * 2048
                continue
            length, ext, sz, flags, name = self._record(buf, at)
            if name and name not in ("\x00", "\x01"):
                if flags & 2:
                    self._walk(ext, sz, prefix + name + "/", seen)
                else:
                    self.files.append((prefix + name, sz, ext))
                    self.records[prefix + name] = extent * 2048 + at
            at += length

    def read(self, handle, size):
        return self.disc.read(handle, size)

    def close(self):
        self.disc.close()


XBOX_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XBOX_PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)


class XisoSource:
    """Xbox XDVDFS (an XISO, or a full image with the game partition)."""

    console = "xbox"
    kind = "xiso"

    def __init__(self, path):
        self.f = open(path, "rb")
        for base in XBOX_PARTITIONS:
            self.f.seek(base + 0x10000)
            head = self.f.read(28)
            if head[:20] == XBOX_MAGIC:
                break
        else:
            raise DumpError("no Xbox XDVDFS volume")
        self.base = base
        root, size = struct.unpack_from("<II", head, 20)
        self.files = []
        self.records = {}  # path -> byte offset of its directory entry
        self._walk(root, size, "")

    def _walk(self, sector, size, prefix):
        if not size:
            return
        self.f.seek(self.base + sector * 2048)
        table = self.f.read(size)
        todo, seen = [0], set()
        while todo:
            index = todo.pop()
            at = index * 4
            if index in seen or at + 14 > len(table):
                continue
            seen.add(index)
            left, right, start, length, attrs, n = struct.unpack_from("<HHIIBB", table, at)
            if left == 0xFFFF:
                continue
            name = table[at + 14:at + 14 + n].decode("latin-1")
            if left:
                todo.append(left)
            if right:
                todo.append(right)
            if attrs & 0x10:
                self._walk(start, length, prefix + name + "/")
            else:
                self.files.append((prefix + name, length, start))
                self.records[prefix + name] = self.base + sector * 2048 + at

    def read(self, handle, size):
        self.f.seek(self.base + handle * 2048)
        return self.f.read(size)

    def close(self):
        self.f.close()


# --- GameCube: plain GCM/ISO or Dolphin's RVZ -------------------------------

def zstd_decompressor():
    """zstd from Python 3.14's compression.zstd, the zstandard package or the
    zstd command; None if there is none."""
    try:
        from compression import zstd  # Python 3.14+
        return lambda data, size: zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
        d = zstandard.ZstdDecompressor()
        return lambda data, size: d.decompress(data, max_output_size=max(size, 1))
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if exe:
        return lambda data, size: subprocess.run([exe, "-dcq"], input=data, stdout=subprocess.PIPE,
                                                 check=True).stdout
    return None


class RvzJunk:
    """Dolphin's junk-data generator (a lagged Fibonacci generator, 521
    words) that RVZ uses to rebuild a disc's padding from a 68-byte seed."""

    K, J = 521, 32

    def __init__(self, seed, skip):
        w = list(struct.unpack(">17I", seed))
        for i in range(17, self.K):
            w.append(((w[i - 17] << 23) & 0xFFFFFFFF) ^ (w[i - 16] >> 9) ^ w[i - 1])
        self.w = [(x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000) for x in w]
        for _ in range(4):
            self._step()
        self.pos = 0
        self.skip(skip)

    def _step(self):
        w, k, j = self.w, self.K, self.J
        for i in range(j):
            w[i] ^= w[i + k - j]
        for i in range(j, k):
            w[i] ^= w[i - j]
        self.block = None

    def skip(self, n):
        self.pos += n
        while self.pos >= self.K * 4:
            self._step()
            self.pos -= self.K * 4

    def take(self, n):
        out = []
        while n:
            if self.block is None:
                self.block = struct.pack(">521I", *self.w)
            m = min(n, self.K * 4 - self.pos)
            out.append(self.block[self.pos:self.pos + m])
            self.pos += m
            n -= m
            if self.pos == self.K * 4:
                self._step()
                self.pos = 0
        return b"".join(out)


class RvzImage:
    BLOCK = 0x8000

    def __init__(self, path):
        self.f = open(path, "rb")
        h1 = self.f.read(0x48)
        magic, _, _, h2_size, h2_hash, self.size, _, h1_hash = struct.unpack(">4sIII20sQQ20s", h1)
        if magic == b"WIA\x01":
            raise DumpError("WIA images are not supported: convert it to RVZ or ISO in Dolphin")
        if hashlib.sha1(h1[:0x34]).digest() != h1_hash:
            raise DumpError("the RVZ header is damaged")
        h2 = self.f.read(h2_size)
        if hashlib.sha1(h2).digest() != h2_hash:
            raise DumpError("the RVZ header is damaged")
        disc_type, self.codec, _, self.chunk = struct.unpack_from(">IIiI", h2, 0)
        self.header = h2[0x10:0x90]
        (n_part, _, _, _, n_raw, raw_off, raw_size, n_group, group_off, group_size,
         cdata_size) = struct.unpack_from(">IIQ20sIQIIQIB", h2, 0x90)
        self.cdata = h2[0xD5:0xD5 + cdata_size]
        if disc_type != 1 or n_part:
            raise DumpError("a Wii image, not a GameCube one")
        if self.codec not in (0, 2, 3, 4, 5):
            raise DumpError("RVZ compression %d is not supported" % self.codec)
        self.zstd = None
        if self.codec == 5:
            self.zstd = zstd_decompressor()
            if self.zstd is None:
                raise DumpError("this RVZ image is zstd-compressed: use Python 3.14 or newer, or install "
                                "the zstandard package (pip install zstandard) or the zstd command, "
                                "or convert the image to ISO in Dolphin")
        raw = self._table(raw_off, raw_size, n_raw * 24)
        self.raw = [struct.unpack_from(">QQII", raw, i * 24) for i in range(n_raw)]
        groups = self._table(group_off, group_size, n_group * 12)
        self.groups = [struct.unpack_from(">III", groups, i * 12) for i in range(n_group)]
        self.cache = OrderedDict()

    def _decompress(self, data, size):
        c = self.codec
        if c == 0:
            return data
        if c == 2:
            return bz2.decompress(data)
        if c == 5:
            return self.zstd(data, size)
        if c == 3:
            p = self.cdata
            filt = {"id": lzma.FILTER_LZMA1, "lc": p[0] % 9, "lp": (p[0] // 9) % 5, "pb": p[0] // 45,
                    "dict_size": u32(p, 1)}
        else:
            p = self.cdata[0]
            filt = {"id": lzma.FILTER_LZMA2,
                    "dict_size": 0xFFFFFFFF if p == 40 else (2 | (p & 1)) << (p // 2 + 11)}
        return lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[filt]).decompress(data, max_length=size)

    def _table(self, offset, csize, size):
        self.f.seek(offset)
        out = self._decompress(self.f.read(csize), size)
        if len(out) < size:
            raise DumpError("an RVZ table is truncated")
        return out

    def _group(self, index, disc_offset, size):
        data = self.cache.get(index)
        if data is not None:
            self.cache.move_to_end(index)
            return data
        off4, dsize, packed = self.groups[index]
        compressed = dsize & 0x80000000
        dsize &= 0x7FFFFFFF
        if not dsize:
            data = bytes(size)
        else:
            self.f.seek(off4 << 2)
            data = self.f.read(dsize)
            if compressed:
                data = self._decompress(data, packed or size)
            if packed:
                data = self._unpack(data[:packed], disc_offset, size)
        if len(data) < size:
            raise DumpError("RVZ group %d is short" % index)
        data = data[:size]
        self.cache[index] = data
        if len(self.cache) > 16:
            self.cache.popitem(last=False)
        return data

    def _unpack(self, packed, disc_offset, size):
        out, at, done = [], 0, 0
        while at < len(packed) and done < size:
            word = struct.unpack_from(">I", packed, at)[0]
            at += 4
            n = word & 0x7FFFFFFF
            if word & 0x80000000:
                out.append(RvzJunk(packed[at:at + 68], disc_offset % self.BLOCK).take(n))
                at += 68
            else:
                out.append(packed[at:at + n])
                at += n
            disc_offset += n
            done += n
        return b"".join(out)

    def read(self, offset, size):
        out = bytearray()
        end = min(offset + size, self.size)
        if offset < 0x80:
            n = min(end, 0x80) - offset
            out += self.header[offset:offset + n]
            offset += n
        while offset < end:
            for data_off, data_size, g0, ng in self.raw:
                base = data_off - data_off % self.BLOCK
                limit = data_off + data_size
                if base <= offset < limit:
                    rel = offset - base
                    gi = rel // self.chunk
                    if gi >= ng:
                        raise DumpError("RVZ offset 0x%X is outside its groups" % offset)
                    goff = gi * self.chunk
                    gsize = min(self.chunk, limit - base - goff)
                    data = self._group(g0 + gi, base + goff, gsize)
                    n = min(end, base + goff + gsize) - offset
                    out += data[rel - goff:rel - goff + n]
                    offset += n
                    break
            else:
                nxt = min([d for d, _, _, _ in self.raw if d > offset] + [end])
                out += bytes(nxt - offset)
                offset = nxt
        return bytes(out)

    def close(self):
        self.f.close()


class PlainImage:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size

    def read(self, offset, size):
        self.f.seek(offset)
        return self.f.read(size)

    def close(self):
        self.f.close()


GC_MAGIC = 0xC2339F3D


class GameCubeSource:
    """A GameCube disc: the files of its FST."""

    console = "gc"

    def __init__(self, path):
        with open(path, "rb") as f:
            magic = f.read(4)
        if magic in (b"RVZ\x01", b"WIA\x01"):
            self.image, self.kind = RvzImage(path), "rvz"
        else:
            self.image, self.kind = PlainImage(path), "gcm"
        head = self.image.read(0, 0x440)
        if len(head) < 0x440 or struct.unpack_from(">I", head, 0x1C)[0] != GC_MAGIC:
            raise DumpError("no GameCube disc header")
        self.game_id = head[:6].decode("ascii", "replace")
        self.title = head[0x20:0x60].split(b"\0")[0].decode("latin-1")
        self.dol_off, self.fst_off, self.fst_size = struct.unpack_from(">III", head, 0x420)
        fst = self.image.read(self.fst_off, self.fst_size)
        count = struct.unpack_from(">I", fst, 8)[0]
        names = count * 12
        self.files = []
        self.records = {}  # path -> FST entry index
        stack = [(count, "")]
        for i in range(1, count):
            while len(stack) > 1 and i >= stack[-1][0]:
                stack.pop()
            word, a, b = struct.unpack_from(">III", fst, i * 12)
            at = names + (word & 0xFFFFFF)
            name = fst[at:fst.index(b"\0", at)].decode("latin-1")
            path = stack[-1][1] + name
            if word >> 24:
                stack.append((b, path + "/"))
            else:
                self.files.append((path, b, a))
                self.records[path] = i

    def read(self, handle, size):
        return self.image.read(handle, size)

    def close(self):
        self.image.close()


def open_source(path):
    if os.path.isdir(path):
        return FolderSource(path)
    if not os.path.isfile(path):
        raise DumpError("%s: no such file or folder" % path)
    with open(path, "rb") as f:
        head = f.read(0x40)
    if head[:4] in (b"RVZ\x01", b"WIA\x01") or (len(head) >= 0x20 and
                                                 struct.unpack_from(">I", head, 0x1C)[0] == GC_MAGIC):
        return GameCubeSource(path)
    if path.lower().endswith(".cue"):
        # a CD's cue sheet: its (first) track file
        with open(path, "r", errors="replace") as f:
            for line in f:
                if line.strip().upper().startswith("FILE"):
                    name = line.strip()[4:].strip().rsplit(" ", 1)[0].strip().strip('"')
                    return open_source(os.path.join(os.path.dirname(path), name))
        raise DumpError("%s names no track file" % path)
    errors = []
    for kind in (XisoSource, IsoSource):
        try:
            return kind(path)
        except (DumpError, struct.error, ValueError, IndexError) as e:
            errors.append(str(e))
    raise DumpError("not a disc image this tool knows (PS2 ISO/BIN, Xbox XISO, GameCube RVZ/GCM/ISO)")


def detect_game(src):
    """(console, game) of a source; game is None when unknown."""
    console = src.console
    files = {p.lower(): (p, s, h) for p, s, h in src.files}
    title = ""
    if isinstance(src, GameCubeSource):
        title = src.title
    cnf = files.get("system.cnf")
    if cnf:
        text = src.read(cnf[2], cnf[1]).decode("latin-1")
        serials = {"SLUS_203.27": "ai", "SLUS_201.59": "dm2", "SLUS_204.15": "bmxxxx"}
        for serial, game in serials.items():
            if serial in text.upper():
                return console, game
    xbe = files.get("default.xbe") or files.get("media/default.xbe")
    if xbe:
        d = src.read(xbe[2], xbe[1])
        try:
            base, cert = u32(d, 0x104), u32(d, 0x118)
            c = cert - base
            title = d[c + 12:c + 92].decode("utf-16-le").split("\0")[0]
        except (struct.error, UnicodeDecodeError):
            pass
    t = title.lower()
    if "aggressive" in t:
        return console, "ai"
    if "mirra" in t:
        return console, "dm2"
    if "xxx" in t:
        return console, "bmxxxx"
    names = " ".join(files)
    if "skaters/" in names:
        return console, "ai"
    if ".zix" in names:
        return console, "bmxxxx"
    if "riders/" in names:
        return console, "dm2"
    return console, None


# ===========================================================================
# LZO1X (BMX XXX's Xbox ZLZO chunks): the standard stream format.
# ===========================================================================

def lzo1x_decompress(src, size):
    out = bytearray()
    ip = 0

    def ext(ip, base):
        add = 0
        while src[ip] == 0:
            add += 255
            ip += 1
        return base + add + src[ip], ip + 1

    t = src[0]
    state = 0
    if t > 17:
        ip = 1
        t -= 17
        out += src[ip:ip + t]
        ip += t
        state = t if t < 4 else 4
    try:
        while True:
            t = src[ip]
            ip += 1
            if t < 16:
                if state == 0:
                    if t == 0:
                        t, ip = ext(ip, 15)
                    t += 3
                    out += src[ip:ip + t]
                    ip += t
                    state = 4
                    continue
                if state == 4:
                    dist = 0x801 + (t >> 2) + (src[ip] << 2)
                    length = 3
                else:
                    dist = 1 + (t >> 2) + (src[ip] << 2)
                    length = 2
                ip += 1
            elif t >= 64:
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
                ip += 1
                length = (t >> 5) + 1
            elif t >= 32:
                length = t & 31
                if length == 0:
                    length, ip = ext(ip, 31)
                length += 2
                dist = 1 + ((src[ip] | (src[ip + 1] << 8)) >> 2)
                ip += 2
            else:
                length = t & 7
                if length == 0:
                    length, ip = ext(ip, 7)
                length += 2
                dist = ((t & 8) << 11) + ((src[ip] | (src[ip + 1] << 8)) >> 2)
                ip += 2
                if dist == 0:
                    break
                dist += 0x4000
            start = len(out) - dist
            if start < 0:
                raise DumpError("damaged LZO data")
            if dist >= length:
                out += out[start:start + length]
            else:
                for i in range(length):
                    out.append(out[start + i])
            state = src[ip - 2] & 3
            if state:
                out += src[ip:ip + state]
                ip += state
    except IndexError:
        raise DumpError("damaged LZO data")
    if len(out) != size:
        raise DumpError("LZO data gave %d bytes, wanted %d" % (len(out), size))
    return bytes(out)


def _lzo_emit(data, matches):
    """An LZO1X stream for data, with matches [(position, distance, length)]
    in order and literals between them."""
    out = bytearray()

    def literals(start, stop):
        t = stop - start
        if not t:
            return
        if not out and t <= 238:
            out.append(17 + t)
        elif out and t <= 3:
            out[-2] |= t
        elif t <= 18:
            out.append(t - 3)
        else:
            tt = t - 18
            out.append(0)
            while tt > 255:
                tt -= 255
                out.append(0)
            out.append(tt)
        out.extend(data[start:stop])

    def extra(m):
        while m > 255:
            m -= 255
            out.append(0)
        out.append(m)

    lit = 0
    for pos, off, length in matches:
        literals(lit, pos)
        lit = pos + length
        if length <= 8 and off <= 0x800:
            o = off - 1
            out.append(((length - 1) << 5) | ((o & 7) << 2))
            out.append(o >> 3)
            continue
        if off <= 0x4000:
            o = off - 1
            if length <= 33:
                out.append(32 | (length - 2))
            else:
                out.append(32)
                extra(length - 33)
        else:
            o = off - 0x4000
            hi = (o >> 11) & 8
            if length <= 9:
                out.append(16 | hi | (length - 2))
            else:
                out.append(16 | hi)
                extra(length - 9)
        out.append((o << 2) & 0xFF)
        out.append((o >> 6) & 0xFF)
    literals(lit, len(data))
    out += b"\x11\x00\x00"
    return bytes(out)


def _lzo_candidates(data, depth):
    """For each position, the matches worth considering: [(distance, length)]."""
    n = len(data)
    heads = {}
    cands = [()] * n
    for ip in range(n - 2):
        key = data[ip:ip + 3]
        chain = heads.get(key)
        if chain is None:
            heads[key] = [ip]
            continue
        found = []
        limit = n - ip
        best = 0
        for c in reversed(chain[-depth:]):
            off = ip - c
            if off > 0xBFFF:
                break
            m = 0
            while m + 16 <= limit and data[c + m:c + m + 16] == data[ip + m:ip + m + 16]:
                m += 16
            while m < limit and data[c + m] == data[ip + m]:
                m += 1
            if m >= 3 and (m > best or (off <= 0x800 and m <= 8)):
                found.append((off, m))
                best = max(best, m)
        cands[ip] = found
        chain.append(ip)
        if len(chain) > 4 * depth:
            del chain[:-depth]
    return cands


def _lzo_cost(off, length):
    if length <= 8 and off <= 0x800:
        return 2
    if off <= 0x4000:
        return 3 if length <= 33 else 4 + (length - 34) // 255
    return 3 if length <= 9 else 4 + (length - 10) // 255


def lzo1x_compress(data, depth=32):
    """LZO1X: the smaller of a greedy parse and a cost-based (shortest
    path) parse over the same match candidates."""
    n = len(data)
    cands = _lzo_candidates(data, depth)
    # greedy, with one step of lazy matching
    greedy, ip = [], 0
    while ip < n - 2:
        best = max(cands[ip], key=lambda c: c[1], default=None)
        if best and (best[1] >= 4 or best[0] <= 0x800):
            nxt = max(cands[ip + 1], key=lambda c: c[1], default=None) if ip + 1 < n - 2 else None
            if nxt and nxt[1] > best[1] + 1:
                ip += 1
                continue
            greedy.append((ip, best[0], best[1]))
            ip += best[1]
        else:
            ip += 1
    # shortest path: a literal costs a byte and a share of its run's header
    inf = float("inf")
    cost = [inf] * (n + 1)
    cost[0] = 0.0
    step = [None] * (n + 1)
    for i in range(n):
        ci = cost[i]
        if ci + 1.08 < cost[i + 1]:
            cost[i + 1] = ci + 1.08
            step[i + 1] = (0, 1)
        for off, m in cands[i]:
            lengths = set(range(3, min(m, 8) + 1)) | {m} if off <= 0x800 else {m, min(m, 9), min(m, 33)}
            for length in lengths:
                if length < 3 or (length == 3 and off > 0x800):
                    continue
                c = ci + _lzo_cost(off, length)
                if c < cost[i + length]:
                    cost[i + length] = c
                    step[i + length] = (off, length)
    parsed, i = [], n
    while i > 0:
        off, length = step[i]
        i -= length
        if off:
            parsed.append((i, off, length))
    parsed.reverse()
    return min(_lzo_emit(data, greedy), _lzo_emit(data, parsed), key=len)


# ===========================================================================
# PNG
# ===========================================================================

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _png_chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


def png_rgba(w, h, rgba):
    """PNG bytes of RGBA pixels (RGB if all opaque)."""
    if rgba[3::4] == b"\xff" * (w * h):
        rgb = bytearray(w * h * 3)
        rgb[0::3] = rgba[0::4]
        rgb[1::3] = rgba[1::4]
        rgb[2::3] = rgba[2::4]
        data, ctype, stride = rgb, 2, w * 3
    else:
        data, ctype, stride = rgba, 6, w * 4
    raw = b"".join(b"\0" + data[y * stride:(y + 1) * stride] for y in range(h))
    return (PNG_SIG + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
            + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _png_chunk(b"IEND", b""))


def png_indexed(w, h, bits, rows, palette):
    """PNG bytes of a palette picture: rows of packed indices (4 or 8 bits,
    first pixel in the high nibble), palette of (r, g, b, a)."""
    plte = b"".join(bytes(c[:3]) for c in palette)
    alphas = bytes(c[3] for c in palette)
    out = PNG_SIG + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bits, 3, 0, 0, 0))
    out += _png_chunk(b"PLTE", plte)
    if alphas.count(255) != len(alphas):
        out += _png_chunk(b"tRNS", alphas.rstrip(b"\xff") or b"\xff")
    raw = b"".join(b"\0" + r for r in rows)
    return out + _png_chunk(b"IDAT", zlib.compress(raw, 6)) + _png_chunk(b"IEND", b"")


def _unfilter(raw, h, stride, bpp):
    rows = []
    prev = bytearray(stride)
    at = 0
    for _ in range(h):
        ftype = raw[at]
        cur = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if ftype == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif ftype == 2:
            cur = bytearray((a + b) & 255 for a, b in zip(cur, prev))
        elif ftype == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:
            for i in range(stride):
                if i >= bpp:
                    a, c = cur[i - bpp], prev[i - bpp]
                else:
                    a = c = 0
                b = prev[i]
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif ftype:
            raise DumpError("damaged PNG (filter %d)" % ftype)
        rows.append(cur)
        prev = cur
    return rows


def read_png(path):
    """(width, height, RGBA bytes) of a PNG file (any colour type)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != PNG_SIG:
        raise DumpError("%s is not a PNG file" % path)
    at, idat, plte, trns, ihdr = 8, [], None, None, None
    while at + 8 <= len(data):
        n, tag = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + n]
        at += 12 + n
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    if not ihdr:
        raise DumpError("%s: damaged PNG" % path)
    w, h, depth, ctype, _, _, interlace = ihdr
    if interlace:
        raise DumpError("%s is an interlaced PNG: save it again without interlacing" % path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise DumpError("%s: unknown PNG colour type %d" % (path, ctype))
    bits = channels * depth
    stride = (w * bits + 7) // 8
    rows = _unfilter(zlib.decompress(b"".join(idat)), h, stride, max(1, bits // 8))
    out = bytearray(w * h * 4)
    if depth == 16:
        rows = [r[0::2] for r in rows]
        depth = 8
    if ctype == 3 or (ctype == 0 and depth < 8):
        # unpack to one index (or grey level) per byte
        if depth < 8:
            per = 8 // depth
            mask = (1 << depth) - 1
            unpacked = []
            for r in rows:
                u = bytearray(w)
                for x in range(w):
                    u[x] = (r[x // per] >> ((per - 1 - x % per) * depth)) & mask
                unpacked.append(u)
            rows = unpacked
        if ctype == 0:
            scale = 255 // ((1 << depth) - 1)
            rows = [bytearray(v * scale for v in r) for r in rows]
            channels = 1
    if ctype == 3:
        pal = plte or b""
        ncol = len(pal) // 3
        alpha = (trns or b"") + b"\xff" * 256
        tables = [bytes(pal[3 * i + c] if i < ncol else 0 for i in range(256)) for c in range(3)]
        tables.append(bytes(alpha[i] for i in range(256)))
        idx = bytes(b"".join(bytes(r[:w]) for r in rows))
        for c in range(4):
            out[c::4] = idx.translate(tables[c])
        return w, h, bytes(out)
    flat = b"".join(bytes(r) for r in rows)
    if ctype == 6:
        return w, h, flat
    if ctype == 2:
        out[0::4] = flat[0::3]
        out[1::4] = flat[1::3]
        out[2::4] = flat[2::3]
        out[3::4] = b"\xff" * (w * h)
        if trns and len(trns) >= 6:
            key = bytes((trns[1], trns[3], trns[5]))
            for i in range(w * h):
                if flat[3 * i:3 * i + 3] == key:
                    out[4 * i + 3] = 0
        return w, h, bytes(out)
    if ctype == 4:
        g, a = flat[0::2], flat[1::2]
    else:
        g, a = flat, b"\xff" * (w * h)
        if trns and len(trns) >= 2:
            key = trns[1]
            a = bytes(0 if v == key else 255 for v in g)
    out[0::4] = g
    out[1::4] = g
    out[2::4] = g
    out[3::4] = a
    return w, h, bytes(out)


# ===========================================================================
# Pixels
# ===========================================================================

def normalize(rgba):
    """RGBA with the colour of fully transparent pixels zeroed (editors are
    free to change it)."""
    if b"\x00" not in rgba[3::4]:
        return rgba
    out = bytearray(rgba)
    a = rgba[3::4]
    i = a.find(0)
    while i >= 0:
        out[4 * i:4 * i + 3] = b"\0\0\0"
        i = a.find(0, i + 1)
    return bytes(out)


def same_pixels(a, b):
    return a == b or normalize(a) == normalize(b)


def changed_set(old, new):
    """Indices of the pixels that differ (transparent ones compare equal)."""
    if same_pixels(old, new):
        return set()
    a, b = normalize(old), normalize(new)
    return {i for i in range(len(a) // 4) if a[4 * i:4 * i + 4] != b[4 * i:4 * i + 4]}


def resize(rgba, w, h, nw, nh):
    """Area average when shrinking, nearest when growing."""
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        y0 = y * h // nh
        y1 = max(y0 + 1, (y + 1) * h // nh)
        for x in range(nw):
            x0 = x * w // nw
            x1 = max(x0 + 1, (x + 1) * w // nw)
            acc = [0, 0, 0, 0]
            for sy in range(y0, y1):
                row = sy * w
                for sx in range(x0, x1):
                    p = (row + sx) * 4
                    acc[0] += rgba[p]
                    acc[1] += rgba[p + 1]
                    acc[2] += rgba[p + 2]
                    acc[3] += rgba[p + 3]
            k = (y1 - y0) * (x1 - x0)
            q = (y * nw + x) * 4
            out[q:q + 4] = bytes((v + k // 2) // k for v in acc)
    return bytes(out)


def regenerate_level(orig, w, h, prev, pw, ph, prev_changed):
    """A mip level kept as it was except where its source pixels in the level
    above changed: there, the 2x2 (or so) average of the level above.
    Returns (pixels, changed set)."""
    if not prev_changed:
        return orig, set()
    targets = set()
    for i in prev_changed:
        x, y = i % pw, i // pw
        targets.add((y * h // ph) * w + x * w // pw)
    out = bytearray(orig)
    for i in targets:
        x, y = i % w, i // w
        x0, y0 = x * pw // w, y * ph // h
        x1, y1 = max(x0 + 1, (x + 1) * pw // w), max(y0 + 1, (y + 1) * ph // h)
        acc = [0, 0, 0, 0]
        for sy in range(y0, y1):
            for sx in range(x0, x1):
                p = (sy * pw + sx) * 4
                for c in range(4):
                    acc[c] += prev[p + c]
        k = (x1 - x0) * (y1 - y0)
        out[4 * i:4 * i + 4] = bytes((v + k // 2) // k for v in acc)
    return bytes(out), targets


def flip_rows(data, stride, h):
    return b"".join(data[(h - 1 - y) * stride:(h - y) * stride] for y in range(h))


def median_cut(counts, n):
    """Up to n colours for {colour int (ABGR as read little endian): count}:
    (palette as (r, g, b, a) tuples, {colour: palette index})."""
    cols = [((c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24), k) for c, k in counts.items()]
    boxes = [cols]
    while len(boxes) < n:
        best, best_score, best_ch = None, 0, 0
        for bi, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for ch in range(4):
                vals = [c[0][ch] for c in box]
                rng = max(vals) - min(vals)
                score = rng * rng * sum(k for _, k in box) ** 0.5
                if rng and score > best_score:
                    best, best_score, best_ch = bi, score, ch
        if best is None:
            break
        box = sorted(boxes[best], key=lambda c: c[0][best_ch])
        total = sum(k for _, k in box)
        acc, cut = 0, 1
        for i, (_, k) in enumerate(box):
            acc += k
            if acc * 2 >= total:
                cut = min(max(i + 1, 1), len(box) - 1)
                break
        boxes[best:best + 1] = [box[:cut], box[cut:]]
    palette, index = [], {}
    for bi, box in enumerate(boxes):
        total = sum(k for _, k in box)
        palette.append(tuple((sum(c[ch] * k for c, k in box) + total // 2) // total for ch in range(4)))
        for (c, _) in box:
            index[c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24)] = bi
    return palette, index


def nearest(colour, palette):
    r, g, b, a = colour & 255, (colour >> 8) & 255, (colour >> 16) & 255, colour >> 24
    best, best_d = 0, 1 << 30
    for i, p in enumerate(palette):
        d = (p[0] - r) ** 2 + (p[1] - g) ** 2 + (p[2] - b) ** 2 + 2 * (p[3] - a) ** 2
        if d < best_d:
            best, best_d = i, d
    return best


# ===========================================================================
# Block compression (DXT1, DXT3, DXT5)
# ===========================================================================

def _c565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _colour_palette(c0, c1, four):
    p0, p1 = _c565(c0), _c565(c1)
    if four or c0 > c1:
        return [bytes(p0) + b"\xff", bytes(p1) + b"\xff",
                bytes((2 * a + b + 1) // 3 for a, b in zip(p0, p1)) + b"\xff",
                bytes((a + 2 * b + 1) // 3 for a, b in zip(p0, p1)) + b"\xff"]
    return [bytes(p0) + b"\xff", bytes(p1) + b"\xff",
            bytes((a + b) // 2 for a, b in zip(p0, p1)) + b"\xff", b"\0\0\0\0"]


def _alpha5_palette(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1 + 3) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1 + 2) // 5 for i in range(1, 5)] + [0, 255]


def dxt_size(w, h, kind):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if kind == 1 else 16)


def decode_dxt(raw, w, h, kind):
    """RGBA of a DXT1 (kind 1), DXT3 (3) or DXT5 (5) level."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    W = bw * 4
    out = bytearray(W * bh * 16)
    step = 8 if kind == 1 else 16
    colour = struct.Struct("<HHI").unpack_from
    pals = {}
    arows = {}
    o = 0
    for by in range(bh):
        for bx in range(bw):
            co = o if kind == 1 else o + 8
            c0, c1, bits = colour(raw, co)
            pal = pals.get((c0, c1))
            if pal is None:
                pal = pals[(c0, c1)] = _colour_palette(c0, c1, kind != 1)
            dst = (by * 4 * W + bx * 4) * 4
            for row in range(4):
                b = bits >> (8 * row)
                out[dst:dst + 16] = pal[b & 3] + pal[(b >> 2) & 3] + pal[(b >> 4) & 3] + pal[(b >> 6) & 3]
                dst += W * 4
            if kind == 3:
                dst = (by * 4 * W + bx * 4) * 4
                for row in range(4):
                    v = raw[o + 2 * row] | (raw[o + 2 * row + 1] << 8)
                    a = arows.get(v)
                    if a is None:
                        a = arows[v] = bytes(((v >> (4 * i)) & 15) * 17 for i in range(4))
                    out[dst + 3:dst + 16:4] = a
                    dst += W * 4
            elif kind == 5:
                ap = _alpha5_palette(raw[o], raw[o + 1])
                bits48 = int.from_bytes(raw[o + 2:o + 8], "little")
                dst = (by * 4 * W + bx * 4) * 4
                for row in range(4):
                    out[dst + 3:dst + 16:4] = bytes(ap[(bits48 >> (3 * (4 * row + i))) & 7] for i in range(4))
                    dst += W * 4
            o += step
    if W == w and bh * 4 == h:
        return bytes(out)
    return b"".join(bytes(out[y * W * 4:y * W * 4 + w * 4]) for y in range(h))


def _block_pixels(rgba, w, h, bx, by):
    px = []
    for j in range(4):
        y = min(by * 4 + j, h - 1)
        for i in range(4):
            x = min(bx * 4 + i, w - 1)
            p = (y * w + x) * 4
            px.append(tuple(rgba[p:p + 4]))
    return px


def _to565(c):
    r = min(31, max(0, int(c[0] * 31 / 255 + 0.5)))
    g = min(63, max(0, int(c[1] * 63 / 255 + 0.5)))
    b = min(31, max(0, int(c[2] * 31 / 255 + 0.5)))
    return (r << 11) | (g << 5) | b


def _encode_colour(px, mode):
    """8 bytes of a colour block. mode 'dxt1': 4 colours, or 3 + transparent
    if a pixel is transparent; 'four': always 4 colours (DXT3/5)."""
    transparent = [p[3] < 128 for p in px] if mode == "dxt1" else [False] * 16
    pts = [p[:3] for p, t in zip(px, transparent) if not t]
    if not pts:
        return struct.pack("<HHI", 0, 0, 0xFFFFFFFF)
    n = len(pts)
    mean = [sum(p[c] for p in pts) / n for c in range(3)]
    cov = [[0.0] * 3 for _ in range(3)]
    for p in pts:
        d = [p[c] - mean[c] for c in range(3)]
        for i in range(3):
            for j in range(3):
                cov[i][j] += d[i] * d[j]
    axis = [max(p[c] for p in pts) - min(p[c] for p in pts) for c in range(3)]
    if not any(axis):
        axis = [1.0, 1.0, 1.0]
    for _ in range(6):
        nxt = [sum(cov[i][j] * axis[j] for j in range(3)) for i in range(3)]
        m = max(abs(v) for v in nxt)
        if m < 1e-9:
            break
        axis = [v / m for v in nxt]
    proj = [sum((p[c] - mean[c]) * axis[c] for c in range(3)) for p in pts]
    lo, hi = min(proj), max(proj)
    norm = sum(a * a for a in axis) or 1.0
    e0 = [mean[c] + axis[c] * hi / norm for c in range(3)]
    e1 = [mean[c] + axis[c] * lo / norm for c in range(3)]
    c0, c1 = _to565(e0), _to565(e1)
    three = any(transparent)
    if three:
        if c0 > c1:
            c0, c1 = c1, c0
    else:
        if c0 < c1:
            c0, c1 = c1, c0
    pal = [tuple(p[:3]) for p in _colour_palette(c0, c1, not three if mode == "dxt1" else True)]
    choices = range(3) if three else range(4)
    if not three and c0 == c1:
        choices = range(1)
    bits = 0
    for i, p in enumerate(px):
        if transparent[i]:
            k = 3
        else:
            k = min(choices, key=lambda j: (pal[j][0] - p[0]) ** 2 + (pal[j][1] - p[1]) ** 2 + (pal[j][2] - p[2]) ** 2)
        bits |= k << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def _encode_alpha5(px):
    al = [p[3] for p in px]
    a0, a1 = max(al), min(al)
    if a0 == a1:
        return bytes((a0, a1)) + bytes(6)
    pal = _alpha5_palette(a0, a1)
    bits = 0
    for i, a in enumerate(al):
        k = min(range(8), key=lambda j: abs(pal[j] - a))
        bits |= k << (3 * i)
    return bytes((a0, a1)) + bits.to_bytes(6, "little")


def encode_dxt(rgba, w, h, kind, original=None):
    """DXT data of RGBA pixels. Blocks whose pixels the original's block
    already decodes to are copied from the original as they are."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    step = 8 if kind == 1 else 16
    old = decode_dxt(original, w, h, kind) if original else None
    out = bytearray()
    for by in range(bh):
        for bx in range(bw):
            px = _block_pixels(rgba, w, h, bx, by)
            if old is not None:
                ref = _block_pixels(old, w, h, bx, by)
                if all(a == b or (a[3] == 0 and b[3] == 0) for a, b in zip(px, ref)):
                    o = (by * bw + bx) * step
                    out += original[o:o + step]
                    continue
            if kind == 1:
                out += _encode_colour(px, "dxt1")
            elif kind == 3:
                v = 0
                for i, p in enumerate(px):
                    v |= ((p[3] * 15 + 127) // 255) << (4 * i)
                out += v.to_bytes(8, "little") + _encode_colour(px, "four")
            else:
                out += _encode_alpha5(px) + _encode_colour(px, "four")
    return bytes(out)


# ===========================================================================
# Uncompressed pixel layouts (Xbox formats and DDS masks)
# ===========================================================================

def _mask_shift(mask):
    if not mask:
        return 0, 0
    shift = 0
    while not (mask >> shift) & 1:
        shift += 1
    bits = 0
    while (mask >> (shift + bits)) & 1:
        bits += 1
    return shift, bits


class Layout:
    """Pixels of 8, 16 or 32 bits with channel masks (r, g, b, a); a grey
    layout uses the same mask for r, g and b. An absent alpha is opaque."""

    def __init__(self, bits, masks, name, signed=False):
        self.bits, self.masks, self.name = bits, masks, name
        self.fields = [_mask_shift(m) for m in masks]
        self.signed = signed   # signed bytes (bump maps): shown offset by 128

    def decode(self, raw, count):
        if self.signed:
            raw = bytes(b ^ 0x80 for b in raw[:count * self.bits // 8])
        bpp = self.bits // 8
        out = bytearray(count * 4)
        if self.bits == 32 and all(b in (0, 8) and s % 8 == 0 for s, b in self.fields):
            for c, (s, b) in enumerate(self.fields):
                out[c::4] = raw[s // 8:count * 4:4] if b else b"\xff" * count
            return bytes(out)
        fmt = {1: "B", 2: "H", 4: "I"}[bpp]
        vals = struct.unpack_from("<%d%s" % (count, fmt), raw, 0)
        luts = []
        for s, b in self.fields:
            if b:
                top = (1 << b) - 1
                luts.append((s, top, [(v * 255 + top // 2) // top for v in range(top + 1)]))
            else:
                luts.append(None)
        for i, v in enumerate(vals):
            for c in range(4):
                f = luts[c]
                out[4 * i + c] = f[2][(v >> f[0]) & f[1]] if f else 255
        return bytes(out)

    def encode(self, rgba, count):
        out = []
        for i in range(count):
            v = 0
            for c, (s, b) in enumerate(self.fields):
                if b:
                    top = (1 << b) - 1
                    if c < 3 and self.masks[0] == self.masks[1] == self.masks[2]:
                        if c:
                            continue
                        val = (rgba[4 * i] * 299 + rgba[4 * i + 1] * 587 + rgba[4 * i + 2] * 114) // 1000
                    else:
                        val = rgba[4 * i + c]
                    v |= ((val * top + 127) // 255) << s
            out.append(v)
        fmt = {8: "B", 16: "H", 32: "I"}[self.bits]
        packed = struct.pack("<%d%s" % (count, fmt), *out)
        if self.signed:
            packed = bytes(b ^ 0x80 for b in packed)
        return packed


def _grey(bits, lmask, amask, name):
    return Layout(bits, (lmask, lmask, lmask, amask), name)


# Xbox D3D formats: id -> (name, layout or DXT kind, swizzled)
XBOX_FORMATS = {
    0x00: ("L8", _grey(8, 0xFF, 0, "L8"), True),
    0x01: ("AL8", _grey(8, 0xFF, 0xFF, "AL8"), True),
    0x02: ("A1R5G5B5", Layout(16, (0x7C00, 0x3E0, 0x1F, 0x8000), "A1R5G5B5"), True),
    0x03: ("X1R5G5B5", Layout(16, (0x7C00, 0x3E0, 0x1F, 0), "X1R5G5B5"), True),
    0x04: ("A4R4G4B4", Layout(16, (0xF00, 0xF0, 0xF, 0xF000), "A4R4G4B4"), True),
    0x05: ("R5G6B5", Layout(16, (0xF800, 0x7E0, 0x1F, 0), "R5G6B5"), True),
    0x06: ("A8R8G8B8", Layout(32, (0xFF0000, 0xFF00, 0xFF, 0xFF000000), "A8R8G8B8"), True),
    0x07: ("X8R8G8B8", Layout(32, (0xFF0000, 0xFF00, 0xFF, 0), "X8R8G8B8"), True),
    0x0C: ("DXT1", 1, False),
    0x0E: ("DXT3", 3, False),
    0x0F: ("DXT5", 5, False),
    0x10: ("LIN_A1R5G5B5", Layout(16, (0x7C00, 0x3E0, 0x1F, 0x8000), "LIN_A1R5G5B5"), False),
    0x11: ("LIN_R5G6B5", Layout(16, (0xF800, 0x7E0, 0x1F, 0), "LIN_R5G6B5"), False),
    0x12: ("LIN_A8R8G8B8", Layout(32, (0xFF0000, 0xFF00, 0xFF, 0xFF000000), "LIN_A8R8G8B8"), False),
    0x13: ("LIN_L8", _grey(8, 0xFF, 0, "LIN_L8"), False),
    0x19: ("A8", Layout(8, (0, 0, 0, 0xFF), "A8"), True),
    0x1A: ("A8L8", _grey(16, 0xFF, 0xFF00, "A8L8"), True),
    0x1C: ("LIN_X1R5G5B5", Layout(16, (0x7C00, 0x3E0, 0x1F, 0), "LIN_X1R5G5B5"), False),
    0x1D: ("LIN_A4R4G4B4", Layout(16, (0xF00, 0xF0, 0xF, 0xF000), "LIN_A4R4G4B4"), False),
    0x1E: ("LIN_X8R8G8B8", Layout(32, (0xFF0000, 0xFF00, 0xFF, 0), "LIN_X8R8G8B8"), False),
    0x1F: ("LIN_A8", Layout(8, (0, 0, 0, 0xFF), "LIN_A8"), False),
    0x20: ("LIN_A8L8", _grey(16, 0xFF, 0xFF00, "LIN_A8L8"), False),
    0x28: ("V8U8", Layout(16, (0xFF, 0xFF00, 0, 0), "V8U8", signed=True), True),
    0x3A: ("A8B8G8R8", Layout(32, (0xFF, 0xFF00, 0xFF0000, 0xFF000000), "A8B8G8R8"), True),
    0x3B: ("B8G8R8A8", Layout(32, (0xFF00, 0xFF0000, 0xFF000000, 0xFF), "B8G8R8A8"), True),
    0x3C: ("R8G8B8A8", Layout(32, (0xFF000000, 0xFF0000, 0xFF00, 0xFF), "R8G8B8A8"), True),
    0x3F: ("LIN_A8B8G8R8", Layout(32, (0xFF, 0xFF00, 0xFF0000, 0xFF000000), "LIN_A8B8G8R8"), False),
}


def swizzle_table(w, h, d=1):
    """t[(z * h + y) * w + x] = the Xbox swizzled index of texel (x, y, z)."""
    xm = ym = zm = 0
    bit, m = 1, 1
    while bit < w or bit < h or bit < d:
        if bit < w:
            xm |= m
            m <<= 1
        if bit < h:
            ym |= m
            m <<= 1
        if bit < d:
            zm |= m
            m <<= 1
        bit <<= 1

    def deposit(v, mask):
        out, b = 0, 1
        while mask:
            low = mask & -mask
            if v & b:
                out |= low
            mask &= mask - 1
            b <<= 1
        return out
    xs = [deposit(x, xm) for x in range(w)]
    ys = [deposit(y, ym) for y in range(h)]
    zs = [deposit(z, zm) for z in range(d)]
    return [zv | yv | xv for zv in zs for yv in ys for xv in xs]


def unswizzle(raw, w, h, bpp, d=1):
    t = swizzle_table(w, h, d)
    out = bytearray(w * h * d * bpp)
    for i, s in enumerate(t):
        out[i * bpp:(i + 1) * bpp] = raw[s * bpp:(s + 1) * bpp]
    return bytes(out)


def swizzle(linear, w, h, bpp, d=1):
    t = swizzle_table(w, h, d)
    out = bytearray(w * h * d * bpp)
    for i, s in enumerate(t):
        out[s * bpp:(s + 1) * bpp] = linear[i * bpp:(i + 1) * bpp]
    return bytes(out)


# ===========================================================================
# Pictures. Each picture lists its levels as (w, h, offset, size) in its own
# bytes, decodes a level to RGBA and encodes RGBA back into a level's bytes of
# the same size.
# ===========================================================================

class Picture:
    fmt = "?"
    ext = ".bin"
    raw_start = 0     # where the standard file (BMP, DDS, ...) starts in the bytes
    palettized = False

    def __init__(self, data):
        self.data = data
        self.levels = []

    def decode(self, k):
        raise NotImplementedError

    def encode(self, k, rgba):
        raise NotImplementedError


class BmpPicture(Picture):
    """A Windows BMP (PS2 TBMP/TZMP, and loose .bmp files): 4/8-bit with a
    palette, or 16/24/32-bit. As the games' loader reads them: a palette
    entry of exactly magenta (255, 0, 255) is the colour key (transparent);
    TBMP pictures are otherwise opaque (their palette alpha is not used),
    TZMP pictures use the palette's alpha as it is (0-255; the game halves
    it to the PS2's 0-128 when it loads it). Direct-colour BMPs have no key."""

    ext = ".bmp"

    def __init__(self, data, keep_key=False, alpha=False):
        Picture.__init__(self, data)
        if data[:2] != b"BM" or len(data) < 54:
            raise DumpError("not a BMP")
        self.pixels = u32(data, 10)
        self.head = u32(data, 14)
        w, h = struct.unpack_from("<ii", data, 18)
        self.bpp = struct.unpack_from("<H", data, 28)[0]
        comp = u32(data, 30)
        if comp not in (0, 3) or self.bpp not in (4, 8, 16, 24, 32) or w <= 0 or h == 0:
            raise DumpError("BMP %d-bit, compression %d" % (self.bpp, comp))
        self.w, self.h, self.bottom_up = w, abs(h), h > 0
        self.stride = (w * self.bpp + 31) // 32 * 4
        if self.pixels + self.stride * self.h > len(data):
            raise DumpError("BMP is truncated")
        self.keep_key = keep_key
        self.levels = [(self.w, self.h, self.pixels, self.stride * self.h)]
        self.palettized = self.bpp <= 8
        self.mode = "alpha" if alpha else "key"
        self.fmt = "BMP %d-bit" % self.bpp
        if self.palettized:
            self.ncol = u32(data, 46) or (1 << self.bpp)
            self.ncol = min(self.ncol, 1 << self.bpp)
            self.pal_at = 14 + self.head
            raw = data[self.pal_at:self.pal_at + 4 * self.ncol]
            self.stored = [(raw[4 * i + 2], raw[4 * i + 1], raw[4 * i], raw[4 * i + 3]) for i in range(self.ncol)]
            self.palette = [self.shown(c) for c in self.stored] + [(0, 0, 0, 255)] * (256 - self.ncol)
        elif comp == 3 or self.bpp == 16:
            masks = struct.unpack_from("<3I", data, 54) if comp == 3 else (0x7C00, 0x3E0, 0x1F)
            self.layout = Layout(self.bpp, tuple(masks) + (0,), "bmp")
        else:
            self.layout = None

    def shown(self, c):
        """A stored palette entry as the game shows it (r, g, b, a)."""
        r, g, b, a = c
        if (r, g, b) == (255, 0, 255):
            return (r, g, b, 255 if self.keep_key else 0)
        return (r, g, b, a if self.mode == "alpha" else 255)

    def rows(self):
        """Top-down rows, each (w * bpp + 7) // 8 bytes."""
        n = (self.w * self.bpp + 7) // 8
        d, s = self.data, self.stride
        rows = [d[self.pixels + y * s:self.pixels + y * s + n] for y in range(self.h)]
        if self.bottom_up:
            rows.reverse()
        return rows

    def indices(self):
        """One index per pixel, top-down."""
        flat = b"".join(self.rows())
        if self.bpp == 8:
            return flat
        out = bytearray(len(flat) * 2)
        out[0::2] = flat.translate(bytes(i >> 4 for i in range(256)))
        out[1::2] = flat.translate(bytes(i & 15 for i in range(256)))
        if self.w % 2:
            n = (self.w + 1) // 2 * 2
            return b"".join(out[y * n:y * n + self.w] for y in range(self.h))
        return bytes(out)

    def decode(self, k=0):
        n = self.w * self.h
        out = bytearray(n * 4)
        if self.palettized:
            idx = self.indices()
            for c in range(4):
                out[c::4] = idx.translate(bytes(self.palette[i][c] for i in range(256)))
            return bytes(out)
        flat = b"".join(self.rows())
        if self.bpp == 24:
            out[0::4] = flat[2::3]
            out[1::4] = flat[1::3]
            out[2::4] = flat[0::3]
            out[3::4] = b"\xff" * n
        elif self.bpp == 32 and self.layout is None:
            out[0::4] = flat[2::4]
            out[1::4] = flat[1::4]
            out[2::4] = flat[0::4]
            out[3::4] = b"\xff" * n
        else:
            out = bytearray(self.layout.decode(flat, n))
        return bytes(out)

    def png(self):
        if self.palettized:
            return png_indexed(self.w, self.h, self.bpp, self.rows(), self.palette[:1 << self.bpp])
        return png_rgba(self.w, self.h, self.decode())

    # -- encoding ------------------------------------------------------------

    def canonical(self, v):
        """A pixel (ABGR int) as this palette can store it, in the form the
        palette lookups use; None for the colour key (magenta, and in a TBMP
        any pixel more than half transparent)."""
        if v & 0xFFFFFF == 0xFF00FF:
            return None
        if self.mode == "key":
            if v >> 24 < 128:
                return None
            return v | 0xFF000000
        return v

    def store_entry(self, c):
        """A shown palette colour as the BMP stores it (B, G, R, A): TBMP
        palettes keep alpha 0 as the games' files do."""
        r, g, b, a = c
        if (r, g, b) == (255, 0, 255):
            a = 0
        elif self.mode == "key":
            a = 0
        return bytes((b, g, r, a))

    def with_pixels(self, palette, index_rows):
        """The BMP with a new palette (shown colours) and top-down index rows
        (one byte per pixel)."""
        out = bytearray(self.data)
        entries = b"".join(self.store_entry(c) for c in palette[:self.ncol])
        entries += b"\0\0\0\0" * (self.ncol - len(palette[:self.ncol]))
        out[self.pal_at:self.pal_at + 4 * self.ncol] = entries
        order = range(self.h - 1, -1, -1) if self.bottom_up else range(self.h)
        for row_i, y in enumerate(order):
            idx = index_rows[y]
            at = self.pixels + row_i * self.stride
            if self.bpp == 4:
                packed = bytearray((self.w + 1) // 2)
                for x in range(0, self.w, 2):
                    hi = idx[x]
                    # an odd width's last low nibble is padding: kept as it was
                    lo = idx[x + 1] if x + 1 < self.w else self.data[at + x // 2] & 15
                    packed[x // 2] = (hi << 4) | lo
                row = bytes(packed)
            else:
                row = bytes(idx)
            out[at:at + len(row)] = row   # the row's padding bytes are kept as they were
        return bytes(out)

    def encode(self, k, rgba):
        """Direct-colour BMPs only (palettes are encoded per texture)."""
        n = self.w * self.h
        if self.bpp in (24, 32) and self.layout is None:
            bpp = self.bpp // 8
            flat = bytearray(n * bpp)
            flat[0::bpp] = rgba[2::4]
            flat[1::bpp] = rgba[1::4]
            flat[2::bpp] = rgba[0::4]
            if bpp == 4:
                old = b"".join(self.rows())
                flat[3::4] = old[3::4]
        else:
            flat = self.layout.encode(rgba, n)
        rowlen = self.w * self.bpp // 8
        out = bytearray(self.data[self.pixels:self.pixels + self.stride * self.h])
        for y in range(self.h):
            dst = (self.h - 1 - y if self.bottom_up else y) * self.stride
            out[dst:dst + rowlen] = flat[y * rowlen:(y + 1) * rowlen]
        return bytes(out)


class TgaPicture(Picture):
    """An uncompressed truecolour TGA (TTGA chunks, loose .tga)."""

    ext = ".tga"

    def __init__(self, data):
        Picture.__init__(self, data)
        if len(data) < 18:
            raise DumpError("not a TGA")
        idlen, cmap, kind = data[0], data[1], data[2]
        self.w, self.h = struct.unpack_from("<HH", data, 12)
        self.bpp, desc = data[16], data[17]
        if kind != 2 or cmap or self.bpp not in (24, 32):
            raise DumpError("TGA type %d, %d-bit" % (kind, self.bpp))
        self.top = bool(desc & 0x20)
        self.alpha = self.bpp == 32 and (desc & 15) > 0
        self.at = 18 + idlen
        size = self.w * self.h * self.bpp // 8
        if self.at + size > len(data):
            raise DumpError("TGA is truncated")
        self.levels = [(self.w, self.h, self.at, size)]
        self.fmt = "TGA %d-bit" % self.bpp

    def decode(self, k=0):
        n, bpp = self.w * self.h, self.bpp // 8
        flat = self.data[self.at:self.at + n * bpp]
        if not self.top:
            flat = flip_rows(flat, self.w * bpp, self.h)
        out = bytearray(n * 4)
        out[0::4] = flat[2::bpp]
        out[1::4] = flat[1::bpp]
        out[2::4] = flat[0::bpp]
        out[3::4] = flat[3::4] if self.alpha else b"\xff" * n
        return bytes(out)

    def encode(self, k, rgba):
        n, bpp = self.w * self.h, self.bpp // 8
        flat = bytearray(n * bpp)
        flat[0::bpp] = rgba[2::4]
        flat[1::bpp] = rgba[1::4]
        flat[2::bpp] = rgba[0::4]
        if bpp == 4:
            if self.alpha:
                flat[3::4] = rgba[3::4]
            else:
                old = self.data[self.at:self.at + 4 * n]
                if not self.top:
                    old = flip_rows(old, self.w * 4, self.h)
                flat[3::4] = old[3::4]
        flat = bytes(flat)
        if not self.top:
            flat = flip_rows(flat, self.w * bpp, self.h)
        return flat


class DdsPicture(Picture):
    """A DDS file (DM2's Xbox and all GameCube TDDS chunks after their u32,
    loose .dds files): DXT1/2/3/4/5 or uncompressed with channel masks."""

    ext = ".dds"

    def __init__(self, data, start):
        Picture.__init__(self, data)
        self.raw_start = start
        d = data
        if d[start:start + 4] != b"DDS ":
            raise DumpError("not a DDS")
        h, w = struct.unpack_from("<II", d, start + 12)
        flags = u32(d, start + 8)
        mips = u32(d, start + 28) if flags & 0x20000 or u32(d, start + 28) else 1
        mips = max(1, mips)
        pf_flags = u32(d, start + 80)
        fourcc = d[start + 84:start + 88]
        bits = u32(d, start + 88)
        masks = struct.unpack_from("<4I", d, start + 92)
        self.kind = None
        if pf_flags & 4:
            self.kind = {b"DXT1": 1, b"DXT2": 3, b"DXT3": 3, b"DXT4": 5, b"DXT5": 5}.get(fourcc)
            if not self.kind:
                raise DumpError("DDS %r" % fourcc)
            self.fmt = "DDS " + fourcc.decode()
        elif bits in (8, 16, 32) and pf_flags & 0x41:
            amask = masks[3] if pf_flags & 1 else 0
            if pf_flags & 0x40:
                self.layout = Layout(bits, (masks[0], masks[1], masks[2], amask), "dds")
            else:
                self.layout = Layout(bits, (0, 0, 0, amask), "dds")
            self.fmt = "DDS %d-bit RGB" % bits
        elif bits == 24 and pf_flags & 0x40:
            self.layout = None
            self.fmt = "DDS 24-bit RGB"
            self.masks24 = masks[:3]
            if masks[:3] != (0xFF0000, 0xFF00, 0xFF):
                raise DumpError("DDS 24-bit with masks %s" % (masks[:3],))
        else:
            raise DumpError("DDS pixel format flags %X, %d bits" % (pf_flags, bits))
        self.bits = bits
        at = start + 4 + u32(d, start + 4)
        for _ in range(mips):
            size = dxt_size(w, h, self.kind) if self.kind else w * h * bits // 8
            if at + size > len(d):
                break
            self.levels.append((w, h, at, size))
            at += size
            w, h = max(1, w // 2), max(1, h // 2)
        if not self.levels:
            raise DumpError("DDS is truncated")

    def decode(self, k):
        w, h, at, size = self.levels[k]
        raw = self.data[at:at + size]
        if self.kind:
            return decode_dxt(raw, w, h, self.kind)
        if self.layout is None:
            out = bytearray(w * h * 4)
            out[0::4] = raw[2::3]
            out[1::4] = raw[1::3]
            out[2::4] = raw[0::3]
            out[3::4] = b"\xff" * (w * h)
            return bytes(out)
        return self.layout.decode(raw, w * h)

    def encode(self, k, rgba):
        w, h, at, size = self.levels[k]
        orig = self.data[at:at + size]
        if self.kind:
            return encode_dxt(rgba, w, h, self.kind, orig)
        if self.layout is None:
            out = bytearray(w * h * 3)
            out[0::3] = rgba[2::4]
            out[1::3] = rgba[1::4]
            out[2::3] = rgba[0::4]
            return bytes(out)
        return self.layout.encode(rgba, w * h)


class XprPicture(Picture):
    """One texture of an Xbox XPR0 bundle (AI and BMX XXX TXPR chunks after
    their u32, loose .xpr files): DXT or uncompressed, swizzled or linear."""

    ext = ".xpr"

    def __init__(self, data, start, resource=0):
        Picture.__init__(self, data)
        self.raw_start = start
        d = data
        if d[start:start + 4] != b"XPR0":
            raise DumpError("not an XPR0")
        hsize = u32(d, start + 8)
        r = start + 12
        found = -1
        while r + 20 <= start + hsize:
            common = u32(d, r)
            if common == 0xFFFFFFFF:
                break
            rtype = (common >> 16) & 7
            if rtype == 4:
                found += 1
                if found == resource:
                    break
                r += 20
            elif rtype == 3:
                r += 12
            else:
                raise DumpError("XPR resource type %d" % rtype)
        else:
            raise DumpError("no texture in the XPR")
        if found != resource or u32(d, r) == 0xFFFFFFFF:
            raise DumpError("no texture in the XPR")
        _, offset, _, fmtword, size = struct.unpack_from("<5I", d, r)
        fid = (fmtword >> 8) & 0xFF
        if fid not in XBOX_FORMATS:
            raise DumpError("Xbox format 0x%02X" % fid)
        name, how, swz = XBOX_FORMATS[fid]
        self.kind = how if isinstance(how, int) else None
        self.layout = None if self.kind else how
        self.swizzled = swz
        self.fmt = "XPR " + name + (" (swizzled)" if swz else "")
        at = start + hsize + offset
        dims = (fmtword >> 4) & 15
        depth = 1 << ((fmtword >> 28) & 15) if dims == 3 else 1
        if fmtword & 4:
            raise DumpError("Xbox cube maps are not supported")
        if depth > 1 and self.kind:
            raise DumpError("compressed volume textures are not supported")
        if depth > 1:
            self.fmt += " volume"
        self.depths = []
        if size:  # linear with an explicit size
            w, h = (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1
            pitch = (((size >> 24) & 0xFF) + 1) * 64
            self.pitch = pitch
            mips = 1
        else:
            w, h = 1 << ((fmtword >> 20) & 15), 1 << ((fmtword >> 24) & 15)
            mips = max(1, (fmtword >> 16) & 15)
            self.pitch = 0
        for _ in range(mips):
            if self.kind:
                n = dxt_size(w, h, self.kind)
            else:
                n = (self.pitch * h) if self.pitch else w * h * depth * self.layout.bits // 8
            if at + n > len(d):
                break
            self.levels.append((w, h * depth, at, n))   # volume slices stacked top to bottom
            self.depths.append(depth)
            at += n
            w, h, depth = max(1, w // 2), max(1, h // 2), max(1, depth // 2)
        if not self.levels:
            raise DumpError("XPR texture is truncated")

    def decode(self, k):
        w, h, at, size = self.levels[k]
        raw = self.data[at:at + size]
        if self.kind:
            return decode_dxt(raw, w, h, self.kind)
        bpp = self.layout.bits // 8
        if self.swizzled:
            dz = self.depths[k]
            raw = unswizzle(raw, w, h // dz, bpp, dz)
        elif self.pitch and self.pitch != w * bpp:
            raw = b"".join(raw[y * self.pitch:y * self.pitch + w * bpp] for y in range(h))
        return self.layout.decode(raw, w * h)

    def encode(self, k, rgba):
        w, h, at, size = self.levels[k]
        orig = self.data[at:at + size]
        if self.kind:
            return encode_dxt(rgba, w, h, self.kind, orig)
        bpp = self.layout.bits // 8
        raw = self.layout.encode(rgba, w * h)
        if self.swizzled:
            dz = self.depths[k]
            return swizzle(raw, w, h // dz, bpp, dz)
        if self.pitch and self.pitch != w * bpp:
            out = bytearray(orig)
            for y in range(h):
                out[y * self.pitch:y * self.pitch + w * bpp] = raw[y * w * bpp:(y + 1) * w * bpp]
            return bytes(out)
        return raw


class RawPicture(Picture):
    """Aggressive Inline's PS2 loading screens: headerless 24-bit RGB."""

    ext = ".raw"
    SIZES = {640 * 448 * 3: (640, 448), 512 * 448 * 3: (512, 448)}

    def __init__(self, data):
        Picture.__init__(self, data)
        self.w, self.h = self.SIZES[len(data)]
        self.levels = [(self.w, self.h, 0, len(data))]
        self.fmt = "RAW 24-bit RGB"

    def decode(self, k=0):
        n = self.w * self.h
        out = bytearray(n * 4)
        out[0::4] = self.data[0::3]
        out[1::4] = self.data[1::3]
        out[2::4] = self.data[2::3]
        out[3::4] = b"\xff" * n
        return bytes(out)

    def encode(self, k, rgba):
        out = bytearray(self.w * self.h * 3)
        out[0::3] = rgba[0::4]
        out[1::3] = rgba[1::4]
        out[2::3] = rgba[2::4]
        return bytes(out)


def picture_of(tag, body, keep_key):
    if tag in (b"TBMP", b"TZMP"):
        return BmpPicture(body, keep_key, alpha=tag == b"TZMP")
    if tag == b"TTGA":
        return TgaPicture(body)
    if tag == b"TDDS":
        return DdsPicture(body, 4)
    if tag == b"TXPR":
        return XprPicture(body, 4)
    raise DumpError("unknown picture chunk %r" % tag)


# ===========================================================================
# Textures in the games' files
# ===========================================================================

class Texture:
    """A TXRC (TXRH header with the name, then one or more pictures) or a
    loose picture file. pics: [(tag, offset of the body in the blob, size)]."""

    def __init__(self, name, blob, at, pics, loose=None):
        self.name, self.blob, self.at, self.pics, self.loose = name, blob, at, pics, loose
        self.decoded = None   # [Picture]
        self.errors = []

    def label(self, tag):
        return "%s %s" % (tag.decode("latin-1") if isinstance(tag, bytes) else tag, "")


class GameFile:
    """A file's bytes, split into blobs: the file itself, or for BMX XXX's
    Xbox files (FORM of ZLZO chunks) each chunk's unpacked bytes."""

    def __init__(self, path, data):
        self.path, self.data = path, data
        self.lzo = []   # [(offset of the chunk header, chunk size)]
        if data[:4] == b"FORM" and data[8:12] == b"ZLZO":
            end = min(len(data), 8 + u32(data, 4))
            at = 8
            self.blobs = []
            while at + 8 <= end:
                tag, size = data[at:at + 4], u32(data, at + 4)
                if tag == b"ZLZO":
                    self.lzo.append((at, size))
                    self.blobs.append(lzo1x_decompress(data[at + 12:at + 8 + size], u32(data, at + 8)))
                at += 8 + size
        else:
            self.blobs = [data]

    def textures(self):
        out = []
        for bi, blob in enumerate(self.blobs):
            out += find_textures(blob, bi)
        return out


PICTURE_TAGS = (b"TBMP", b"TZMP", b"TTGA", b"TDDS", b"TXPR")


def find_textures(blob, bi=0):
    out = []
    n = len(blob)
    at = blob.find(b"TXRC")
    while at >= 0:
        end = at
        ok = False
        if at + 16 <= n:
            size = u32(blob, at + 4)
            end = at + 8 + size
            if end <= n and blob[at + 8:at + 12] == b"TXRH" and at + 16 + u32(blob, at + 12) <= end:
                ok = True
        if not ok:
            at = blob.find(b"TXRC", at + 1)
            continue
        name, pics, p = "", [], at + 8
        while p + 8 <= end:
            tag, cs = blob[p:p + 4], u32(blob, p + 4)
            if p + 8 + cs > end:
                break
            if tag == b"TXRH":
                name = blob[p + 8 + 0x14:p + 8 + min(cs, 0x34)].split(b"\0")[0].decode("latin-1")
            elif tag in PICTURE_TAGS or all(65 <= c <= 90 for c in tag):
                pics.append((tag, p + 8, cs))
            p += 8 + cs
        out.append(Texture(name, bi, at, pics))
        at = blob.find(b"TXRC", end)
    return out


LOOSE = {".bmp": "BMP", ".tga": "TGA", ".dds": "DDS", ".xpr": "XPR", ".raw": "RAW"}


def loose_pictures(path, data, keep_key):
    """Pictures of a loose file: [(name suffix, Picture)]."""
    kind = LOOSE.get(os.path.splitext(path)[1].lower())
    if kind == "BMP":
        return [("", BmpPicture(data, keep_key))]
    if kind == "TGA":
        return [("", TgaPicture(data))]
    if kind == "DDS":
        return [("", DdsPicture(data, 0))]
    if kind == "RAW" and len(data) in RawPicture.SIZES:
        return [("", RawPicture(data))]
    if kind == "XPR":
        out, i = [], 0
        while True:
            try:
                out.append(("" if i == 0 else "#%d" % i, XprPicture(data, 0, i)))
            except DumpError:
                if not out:
                    raise
                break
            i += 1
        return out
    return []


def safe_name(name):
    keep = []
    for ch in name:
        keep.append("_" if ch in '<>:"/\\|?*' or ord(ch) < 32 else ch)
    s = "".join(keep).strip(" .")
    return s or "unnamed"


def safe_path(path):
    parts = [p for p in path.split("/") if p not in ("", ".", "..")]
    return [safe_name(p) for p in parts]


def matches(patterns, *texts):
    if not patterns:
        return True
    for pat in patterns:
        p = pat.lower().replace("\\", "/")
        if not any(c in p for c in "*?["):
            if any(p in t.lower() for t in texts):
                return True
        elif any(fnmatch.fnmatchcase(t.lower(), p) for t in texts):
            return True
    return False


def texture_pictures(tex, blob, keep_key):
    """[Picture] of a texture, and the error text if one cannot be read."""
    pics = []
    for tag, at, size in tex.pics:
        if tag not in PICTURE_TAGS:
            return pics, "%s: unknown picture chunk" % tag.decode("latin-1")
        if size == 0:
            continue
        try:
            pics.append(picture_of(tag, blob[at:at + size], keep_key))
        except (DumpError, struct.error, IndexError, KeyError, ValueError) as e:
            return pics, "%s: %s" % (tag.decode("latin-1"), e)
    return pics, None


def level_list(pics):
    """[(picture index, level in that picture)] of a texture's pictures: the
    first picture's levels, then each further picture (the PS2's mip BMPs)."""
    out = []
    for pi, pic in enumerate(pics):
        for k in range(len(pic.levels)):
            out.append((pi, k))
    return out


def fmt_label(tag, pic):
    t = tag.decode("latin-1") if isinstance(tag, bytes) else tag
    return "%s %s" % (t, pic.fmt.split(" ", 1)[1] if " " in pic.fmt else pic.fmt)


# ===========================================================================
# dump
# ===========================================================================

class Report:
    def __init__(self):
        self.t0 = time.time()
        self.scanned = 0
        self.with_tex = 0
        self.textures = 0
        self.pictures = 0
        self.written = 0
        self.formats = {}
        self.failures = {}
        self.failure_examples = {}
        self.empty = 0

    def ok(self, label):
        self.formats[label] = self.formats.get(label, 0) + 1

    def fail(self, label, where):
        self.failures[label] = self.failures.get(label, 0) + 1
        self.failure_examples.setdefault(label, where)


def say(text, quiet=False):
    if not quiet:
        print(text, flush=True)


def cmd_dump(a):
    src = open_source(a.image)
    console, game = detect_game(src)
    if a.game:
        game = a.game
    out_root = os.path.abspath(a.out or ".")
    image_abs = os.path.abspath(a.image)
    image_dir = image_abs if os.path.isdir(image_abs) else os.path.dirname(image_abs)
    if not a.list:
        # never next to the image: not its own folder, not inside an extracted disc
        if out_root == image_dir or (os.path.isdir(image_abs) and out_root.startswith(image_dir + os.sep)):
            raise DumpError("choose an output folder away from the disc image (not %s)" % out_root)
    say("%s: %s, %s (%s image, %d files)" % (os.path.basename(a.image.rstrip("/\\")), CONSOLES.get(console),
                                             GAMES.get(game, "unknown game"), src.kind, len(src.files)))
    write_png = a.mode in ("png", "both") and not a.list
    write_raw = a.mode in ("raw", "both") and not a.list
    rep = Report()
    manifest = {"tool": "zaxis_texdump", "version": VERSION, "image": image_abs,
                "image_size": os.path.getsize(image_abs) if os.path.isfile(image_abs) else 0,
                "console": console, "game": game, "keep_key": a.keep_key, "mips": not a.no_mips,
                "files": OrderedDict()}
    files = [f for f in src.files if not f[0].lower().endswith(SKIP_EXT)]
    total = len(files)
    for fi, (path, size, handle) in enumerate(files):
        ext = os.path.splitext(path)[1].lower()
        data = src.read(handle, size)
        rep.scanned += 1
        entries = []
        try:
            gf = GameFile(path, data)
            texes = gf.textures()
        except DumpError as e:
            rep.fail("unreadable file", "%s: %s" % (path, e))
            continue
        if not texes and ext in LOOSE and b"TXRC" not in data:
            try:
                for suffix, pic in loose_pictures(path, data, a.keep_key):
                    t = Texture(os.path.splitext(os.path.basename(path))[0] + suffix, 0, 0, [], loose=pic)
                    t.suffix = suffix
                    texes.append(t)
            except (DumpError, struct.error, IndexError, KeyError, ValueError) as e:
                rep.fail("loose %s" % LOOSE[ext], "%s: %s" % (path, e))
        if not texes:
            continue
        texes = [t for t in texes if matches(a.only, path, path + "/" + t.name)]
        if not texes:
            continue
        rep.with_tex += 1
        used = set()
        folder = os.path.join(out_root, *safe_path(path))
        count = 0
        for ti, tex in enumerate(texes):
            blob = gf.blobs[tex.blob]
            if tex.loose is not None:
                pics, err = [tex.loose], None
                tags = [LOOSE[ext]]
            else:
                pics, err = texture_pictures(tex, blob, a.keep_key)
                tags = [t for t, _, s in tex.pics if s]
            if err:
                tag = err.split(":")[0]
                rep.fail(tag + " " + err.split(": ", 1)[1].split(" (")[0], "%s: %s" % (path, tex.name))
                continue
            if not pics:
                rep.empty += 1
                continue
            base = safe_name(tex.name or "texture_%04d" % ti)
            name = base
            n = 2
            while name.lower() in used:
                name = "%s~%d" % (base, n)
                n += 1
            used.add(name.lower())
            rep.textures += 1
            label = fmt_label(tags[0], pics[0])
            levels = level_list(pics)
            entry = {"name": tex.name, "file": name, "blob": tex.blob, "at": tex.at, "format": label,
                     "levels": []}
            if tex.loose is not None:
                entry["loose"] = tex.suffix
            failed = False
            if a.list:
                w, h = pics[0].levels[0][:2]
                print("  %-48s %-26s %4dx%-4d %d level%s" % (path + "/" + name, label, w, h, len(levels),
                                                            "" if len(levels) == 1 else "s"))
            for li, (pi, k) in enumerate(levels):
                pic = pics[pi]
                w, h = pic.levels[k][:2]
                rec = [None, w, h, 0, 0]
                if write_png and not (a.no_mips and li > 0):
                    fname = name + (".%d" % li if li else "") + ".png"
                    try:
                        if isinstance(pic, BmpPicture) and pic.palettized:
                            png = pic.png()
                        else:
                            png = png_rgba(w, h, pic.decode(k))
                    except (DumpError, struct.error, IndexError, ValueError) as e:
                        rep.fail(label, "%s: %s: %s" % (path, tex.name, e))
                        failed = True
                        break
                    os.makedirs(folder, exist_ok=True)
                    with open(os.path.join(folder, fname), "wb") as f:
                        f.write(png)
                    rep.written += 1
                    rec = [os.path.relpath(os.path.join(folder, fname), out_root).replace(os.sep, "/"),
                           w, h, len(png), zlib.crc32(png) & 0xFFFFFFFF]
                elif a.list or write_raw:
                    try:
                        pic.decode(k) if not a.list else None
                    except (DumpError, struct.error, IndexError, ValueError) as e:
                        rep.fail(label, "%s: %s: %s" % (path, tex.name, e))
                        failed = True
                        break
                entry["levels"].append(rec)
            if failed:
                continue
            if write_raw:
                os.makedirs(folder, exist_ok=True)
                for pi, pic in enumerate(pics):
                    raw = pic.data[pic.raw_start:]
                    fname = name + (".%d" % pi if pi else "") + pic.ext
                    with open(os.path.join(folder, fname), "wb") as f:
                        f.write(raw)
                    rep.written += 1
            rep.ok(label)
            rep.pictures += len(levels)
            entries.append(entry)
            count += 1
        if entries:
            manifest["files"][path] = {"size": size, "crc": zlib.crc32(data) & 0xFFFFFFFF, "textures": entries}
        if not a.list:
            say("  [%d/%d] %s: %d texture%s" % (fi + 1, total, path, count, "" if count == 1 else "s"), a.quiet)
    src.close()
    if write_png:
        os.makedirs(out_root, exist_ok=True)
        with open(os.path.join(out_root, MANIFEST), "w") as f:
            json.dump(manifest, f, indent=0)
    secs = time.time() - rep.t0
    print("files scanned %d, with textures %d; textures %d (%d pictures with mip levels), files written %d, "
          "failures %d; %.1f s" % (rep.scanned, rep.with_tex, rep.textures, rep.pictures, rep.written,
                                   sum(rep.failures.values()), secs))
    for label in sorted(rep.formats):
        print("  %-28s %6d" % (label, rep.formats[label]))
    if rep.empty:
        print("  (%d texture headers without a picture, skipped)" % rep.empty)
    for label in sorted(rep.failures):
        print("  FAILED %-21s %6d  e.g. %s" % (label, rep.failures[label], rep.failure_examples[label]))
    return 1 if rep.failures and not rep.textures else 0


# ===========================================================================
# import: edited PNGs back into the game's files
# ===========================================================================

class _PaletteView:
    """A palette BMP seen with new colours put into unused entries."""

    def __init__(self, pic, free, new, key):
        self.pic = pic
        self.palette = list(pic.palette)
        for i, c in zip(free, new):
            self.palette[i] = (c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24)
        if key:
            self.palette[free[len(new)]] = (255, 0, 255, 0)


def encode_palettized(pics, plevels, targets, keep_orig_palette):
    """New bytes for a texture's palette BMPs (they share one palette).
    plevels: indices into pics of the palette BMPs, targets: their new RGBA.
    keep_orig_palette: quantize to the original palette (only a mip level
    changed). Returns (new bodies, exact): exact is False when colours had to
    be approximated."""
    first = pics[plevels[0]]
    ncol = first.ncol
    values = [struct.unpack("<%dI" % (len(t) // 4), t) for t in targets]
    canon = [[first.canonical(v) for v in vals] for vals in values]
    # the original palette as lookups use it (indices past a short palette
    # included: some BMPs use them)
    entry = []
    for i, c in enumerate(first.palette[:1 << first.bpp]):
        if c[:3] == (255, 0, 255) and i < ncol:
            entry.append(None)
        else:
            entry.append(c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24))
    olds = [pics[pi].indices() for pi in plevels]
    lookup = {}
    key_index = None
    for i in range(ncol):
        if entry[i] is None:
            if key_index is None:
                key_index = i
        else:
            lookup.setdefault(entry[i], i)
    # 1. the original palette, if it holds every colour exactly
    exact = all(entry[o] == c or (c is None and key_index is not None) or (c is not None and c in lookup)
                for old, cs in zip(olds, canon) for o, c in zip(old, cs))
    if not exact and not keep_orig_palette:
        # 2. the original palette with the new colours in entries no pixel used
        used = set()
        for old in olds:
            used.update(old)
        free = [i for i in range(ncol) if i not in used]
        new = []
        for cs in canon:
            for c in cs:
                if c is not None and c not in lookup and c not in new:
                    new.append(c)
                    if len(new) > len(free):
                        break
        need_key = key_index is None and any(c is None for cs in canon for c in cs)
        if len(new) + need_key <= len(free):
            first = _PaletteView(first, free, new, need_key)
            for i, c in zip(free, new):
                entry[i] = c
                lookup[c] = i
            if need_key:
                key_index = free[len(new)]
                entry[key_index] = None
            exact = True
    if exact or keep_orig_palette:
        palette = list(first.palette[:ncol])
        if isinstance(first, _PaletteView):
            first = first.pic
        cache = {}

        def index_of(c):
            if c is None:
                return key_index if key_index is not None else 0
            i = lookup.get(c)
            if i is None:
                i = cache.get(c)
                if i is None:
                    choices = [p if p[:3] != (255, 0, 255) else (9999, 9999, 9999, 9999) for p in palette]
                    i = cache[c] = nearest(c, choices)
            return i
    else:
        counts = {}
        has_key = any(c is None for cs in canon for c in cs) or key_index is not None
        for cs in canon:
            for c in cs:
                if c is not None:
                    counts[c] = counts.get(c, 0) + 1
        room = ncol - (1 if has_key else 0)
        palette, mapping = median_cut(counts, max(1, room)) if counts else ([], {})
        # an average must not land on the key colour
        palette = [(254, 0, 255, p[3]) if p[:3] == (255, 0, 255) else p for p in palette]
        if has_key:
            key_index = len(palette)
            palette.append((255, 0, 255, 0))
        palette += [(0, 0, 0, 255)] * (ncol - len(palette))

        def index_of(c):
            if c is None:
                return key_index
            return mapping[c]
    same_palette = exact or keep_orig_palette
    bodies = {}
    for li, pi in enumerate(plevels):
        pic = pics[pi]
        if same_palette:
            # where the original index already gives the colour, keep it
            # (palettes may hold a colour twice, or several key entries)
            idx = bytes(o if entry[o] == c else index_of(c) for o, c in zip(olds[li], canon[li]))
        else:
            idx = bytes(index_of(c) for c in canon[li])
        rows = [idx[y * pic.w:(y + 1) * pic.w] for y in range(pic.h)]
        bodies[pi] = pic.with_pixels(palette, rows)
    return bodies, exact


def rebuild_texture(tex, blob, pics, new_levels):
    """The texture's TXRC bytes with every picture re-encoded from
    new_levels ({(picture index, level): RGBA}); same size as before."""
    out = bytearray(blob[tex.at:tex.at + 8 + u32(blob, tex.at + 4)])
    bodies = {}
    pal = [pi for pi, p in enumerate(pics) if isinstance(p, BmpPicture) and p.palettized]
    exact = True
    # The PS2 mip BMPs share level 0's palette as a rule; those that carry
    # a palette of their own are encoded on their own.
    shared = [pi for pi in pal if pics[pi].bpp == pics[pal[0]].bpp and pics[pi].stored == pics[pal[0]].stored] \
        if pal else []
    groups = ([shared] if shared else []) + [[pi] for pi in pal if pi not in shared]
    for group in groups:
        if not any((pi, 0) in new_levels for pi in group):
            continue
        targets = [new_levels.get((pi, 0)) or pics[pi].decode(0) for pi in group]
        keep = (group[0], 0) not in new_levels
        b, ok = encode_palettized(pics, group, targets, keep)
        exact = exact and ok
        bodies.update(b)
    for pi, pic in enumerate(pics):
        if pi in bodies:
            continue
        ks = [k for k in range(len(pic.levels)) if (pi, k) in new_levels]
        if not ks:
            continue
        body = bytearray(pic.data)
        for k in ks:
            w, h, at, size = pic.levels[k]
            enc = pic.encode(k, new_levels[(pi, k)])
            if len(enc) != size:
                raise DumpError("internal: level %d encoded to %d bytes, not %d" % (k, len(enc), size))
            body[at:at + size] = enc
        bodies[pi] = bytes(body)
    picture_chunks = [(t, at, s) for t, at, s in tex.pics if s]
    for pi, (tag, at, size) in enumerate(picture_chunks):
        if pi in bodies:
            body = bodies[pi]
            if len(body) != size:
                raise DumpError("internal: picture changed size")
            rel = at - tex.at
            out[rel:rel + size] = body
    return bytes(out), exact


def zix_update(text, shifts, chunks):
    """A ZIX index (BMX XXX: one text line per chunk, its offset in the 4th
    column) with the offsets of moved chunks updated: shifts is a list of
    (offset, growth) of the chunks that grew, chunks the offsets of the
    file's chunk bodies before. Only entries that pointed at a chunk are
    changed (some shipped indexes are stale and are left as they were);
    field widths are kept."""
    lines = text.split(b"\n")
    out = []
    for line in lines:
        cols = line.split(b"\t")
        if len(cols) >= 4 and cols[3].strip().isdigit() and int(cols[3]) in chunks:
            old = int(cols[3])
            moved = old + sum(d for at, d in shifts if old > at)
            if moved != old:
                field = str(moved).encode()
                cols[3] = field.rjust(len(cols[3])) if len(field) <= len(cols[3]) else field
            line = b"\t".join(cols)
        out.append(line)
    return b"\n".join(out)


def cmd_import(a):
    dump = os.path.abspath(a.dump)
    mpath = os.path.join(dump, MANIFEST)
    if not os.path.isfile(mpath):
        raise DumpError("%s has no %s: give the folder a PNG dump wrote" % (dump, MANIFEST))
    with open(mpath) as f:
        man = json.load(f)
    image = a.image or man["image"]
    if not os.path.exists(image):
        raise DumpError("the disc image %s is gone: give it with --image" % image)
    out_root = os.path.abspath(a.mods)
    if out_root == dump or out_root.startswith(dump + os.sep) or dump.startswith(out_root + os.sep):
        raise DumpError("write the modified files to a folder outside the dump")
    src = open_source(image)
    by_path = {p: (s, h) for p, s, h in src.files}
    keep_key = man.get("keep_key", False)
    t0 = time.time()
    written, changed_tex, problems = [], 0, []
    stats = {"exact": 0, "approx": 0, "blocks_kept": 0}
    for path, info in man["files"].items():
        # which textures have edited PNGs?
        todo = []
        for t in info["textures"]:
            edits = {}
            for li, rec in enumerate(t["levels"]):
                rel = rec[0]
                if not rel:
                    continue
                full = os.path.join(dump, *rel.split("/"))
                if not os.path.isfile(full):
                    continue
                with open(full, "rb") as f:
                    data = f.read()
                if a.reencode_all or len(data) != rec[3] or zlib.crc32(data) & 0xFFFFFFFF != rec[4]:
                    edits[li] = full
            if edits and matches(a.only, path, path + "/" + t["name"]):
                todo.append((t, edits))
        if not todo:
            continue
        if path not in by_path:
            problems.append("%s is not on this disc image" % path)
            continue
        size, handle = by_path[path]
        data = src.read(handle, size)
        if zlib.crc32(data) & 0xFFFFFFFF != info["crc"]:
            problems.append("%s differs from the dumped one: is this the same disc image?" % path)
            continue
        gf = GameFile(path, data)
        blobs = [bytearray(b) for b in gf.blobs]
        loose_data = None
        file_changed = False
        touched = set()
        for t, edits in todo:
            blob = gf.blobs[t["blob"]]
            loose = "loose" in t
            if loose:
                pic = dict(loose_pictures(path, data, keep_key)).get(t["loose"])
                pics, err = [pic], None if pic else "picture not found"
            else:
                at = t["at"]
                found = []
                if blob[at:at + 4] == b"TXRC":
                    found = find_textures(blob[at:at + 8 + u32(blob, at + 4)], t["blob"])
                if not found:
                    problems.append("%s: %s: texture not found where the dump saw it" % (path, t["name"]))
                    continue
                tex = found[0]
                tex.at = at
                tex.pics = [(tag, p + at, s) for tag, p, s in tex.pics]
                pics, err = texture_pictures(tex, blob, keep_key)
            if err:
                problems.append("%s: %s: %s" % (path, t["name"], err))
                continue
            levels = level_list(pics)
            new_levels = {}
            prev = None   # (pixels, w, h, changed set) of the level above
            bad = False
            for li, (pi, k) in enumerate(levels):
                pic = pics[pi]
                w, h = pic.levels[k][:2]
                orig = pic.decode(k)
                cur, ch = orig, set()
                if li in edits:
                    pw, ph, px = read_png(edits[li])
                    if (pw, ph) != (w, h):
                        if not a.resize:
                            problems.append("%s is %dx%d but the texture is %dx%d: draw it at that size, or "
                                            "use --resize" % (edits[li], pw, ph, w, h))
                            bad = True
                            break
                        px = resize(px, pw, ph, w, h)
                    ch = changed_set(orig, px)
                    if a.reencode_all:
                        ch = set(range(w * h))
                    cur = px if ch else orig
                if not ch and prev is not None and prev[3]:
                    # an unedited mip level below an edited one: redone where its source changed
                    cur, ch = regenerate_level(orig, w, h, prev[0], prev[1], prev[2], prev[3])
                if ch:
                    new_levels[(pi, k)] = cur
                prev = (cur, w, h, ch)
            if bad or not new_levels:
                continue
            if loose:
                body = bytearray(loose_data if loose_data is not None else data)
                pic = pics[0]
                for (pi, k), px in sorted(new_levels.items()):
                    w, h, at, sz = pic.levels[k]
                    if isinstance(pic, BmpPicture) and pic.palettized:
                        b, exact = encode_palettized([pic], [0], [px], False)
                        body[:] = b[0]
                    else:
                        body[at:at + sz] = pic.encode(k, px)
                loose_data = bytes(body)
                file_changed = True
                changed_tex += 1
                continue
            new_txrc, exact = rebuild_texture(tex, blob, pics, new_levels)
            stats["exact" if exact else "approx"] += 1
            blobs[t["blob"]][tex.at:tex.at + len(new_txrc)] = new_txrc
            touched.add(t["blob"])
            file_changed = True
            changed_tex += 1
        if not file_changed:
            continue
        # put the file back together
        extra = []
        if loose_data is not None:
            new = loose_data
        elif not gf.lzo:
            new = bytes(blobs[0])
        else:
            parts = [bytearray(data[:8])]
            pos = 8
            shifts = []
            for bi, (at, csize) in enumerate(gf.lzo):
                parts.append(data[pos:at])
                if bytes(blobs[bi]) == gf.blobs[bi] and not (a.reencode_all and bi in touched):
                    parts.append(data[at:at + 8 + csize])
                else:
                    packed = struct.pack("<I", len(blobs[bi])) + lzo1x_compress(bytes(blobs[bi]))
                    if lzo1x_decompress(packed[4:], len(blobs[bi])) != bytes(blobs[bi]):
                        raise DumpError("internal: LZO round trip failed")
                    if len(packed) <= csize:
                        packed += bytes(csize - len(packed))
                    else:
                        packed += bytes(-len(packed) % 4)
                        shifts.append((at + 8, len(packed) - csize))
                    parts.append(data[at:at + 4] + struct.pack("<I", len(packed)) + packed)
                pos = at + 8 + csize
            parts.append(data[pos:])
            new = bytearray(b"".join(bytes(p) for p in parts))
            struct.pack_into("<I", new, 4, len(new) - 8 if u32(data, 4) == len(data) - 8 else
                             u32(data, 4) + len(new) - len(data))
            new = bytes(new)
            if shifts:
                stem = os.path.splitext(path)[0].lower()
                for p, s, h in src.files:
                    if p.lower() == stem + ".zix":
                        zix = src.read(h, s)
                        extra.append((p, zix_update(zix, shifts, {at + 8 for at, _ in gf.lzo})))
                        problems.append("note: %s grew by %d bytes (LZO); %s updated to match" %
                                        (path, len(new) - len(data), p))
        for p, d in [(path, new)] + extra:
            target = os.path.join(out_root, *safe_path(p))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(d)
            written.append((p, len(d), by_path[p][0] if p in by_path else 0))
    src.close()
    if written:
        meta = {"tool": "zaxis_texdump", "version": VERSION, "image": os.path.abspath(image),
                "console": man.get("console"), "game": man.get("game"),
                "files": {p: {"size": n, "original_size": o} for p, n, o in written}}
        with open(os.path.join(out_root, MOD_MANIFEST), "w") as f:
            json.dump(meta, f, indent=1)
    for p in problems:
        print("  " + p)
    print("%d texture%s changed, %d file%s written to %s (%.1f s)" % (
        changed_tex, "" if changed_tex == 1 else "s", len(written), "" if len(written) == 1 else "s",
        out_root, time.time() - t0))
    if stats["approx"]:
        print("  %d palette texture%s needed a new palette (colours not in the original one)" % (
            stats["approx"], "" if stats["approx"] == 1 else "s"))
    for p, n, o in written:
        print("  %s (%d bytes%s)" % (p, n, "" if n == o else ", was %d" % o))
    return 1 if any(not p.startswith("note:") for p in problems) else 0


# ===========================================================================
# patch-image: modified files into a copy of the disc image
# ===========================================================================

def copy_with_progress(read, size, out, step=8 << 20):
    done = 0
    last = time.time()
    while done < size:
        chunk = read(done, min(step, size - done))
        if not chunk:
            raise DumpError("the image ended early")
        out.write(chunk)
        done += len(chunk)
        if time.time() - last > 2:
            print("  %d%%" % (done * 100 // size), flush=True)
            last = time.time()


def cmd_patch(a):
    mods = os.path.abspath(a.mods)
    image = os.path.abspath(a.image)
    new_image = os.path.abspath(a.new_image)
    if new_image == image:
        raise DumpError("the new image must be a different file: the original is never written")
    if os.path.exists(new_image) and not a.force:
        raise DumpError("%s exists: remove it or use --force" % new_image)
    if os.path.isdir(image):
        raise DumpError("patch-image needs a disc image file, not a folder")
    src = open_source(image)
    names = {p.lower(): (p, s, h) for p, s, h in src.files}
    changes = []
    for folder, dirs, files in os.walk(mods):
        dirs.sort()
        for fn in sorted(files):
            full = os.path.join(folder, fn)
            rel = os.path.relpath(full, mods).replace(os.sep, "/")
            if rel in (MOD_MANIFEST, MANIFEST) or fn.startswith("."):
                continue
            hit = names.get(rel.lower())
            if not hit:
                raise DumpError("%s is not a file of this disc image" % rel)
            with open(full, "rb") as f:
                changes.append((hit[0], hit[1], hit[2], f.read()))
    if not changes:
        raise DumpError("no modified files in %s" % mods)
    # plan: where each file goes; refuse anything that does not fit
    plan, refused = [], []
    if isinstance(src, IsoSource):
        for path, size, sector, data in changes:
            room = (size + 2047) // 2048 * 2048
            if len(data) > room:
                refused.append("%s grew from %d to %d bytes, past its %d-byte sectors on the ISO" %
                               (path, size, len(data), room))
            plan.append((path, sector * 2048, room, data, src.records[path]))
    elif isinstance(src, XisoSource):
        end = os.path.getsize(image)
        end = (end - src.base + 2047) // 2048 * 2048 + src.base
        for path, size, sector, data in changes:
            room = (size + 2047) // 2048 * 2048
            if len(data) <= room:
                plan.append((path, src.base + sector * 2048, room, data, src.records[path]))
            else:
                room = (len(data) + 2047) // 2048 * 2048
                plan.append((path, end, room, data, src.records[path]))
                end += room
    else:
        starts = sorted([h for _, _, h in src.files] + [src.fst_off, src.dol_off, src.image.size])
        for path, size, off, data in changes:
            nxt = min([s for s in starts if s > off] or [src.image.size])
            if off + len(data) > nxt:
                refused.append("%s grew from %d to %d bytes, past the %d bytes before the next file" %
                               (path, size, len(data), nxt - off))
            plan.append((path, off, nxt - off, data, src.records[path]))
    if refused:
        for r in refused:
            print("  " + r)
        raise DumpError("nothing written: these files do not fit in place")
    t0 = time.time()
    tmp = new_image + ".part"
    print("writing %s ..." % new_image, flush=True)
    with open(tmp, "wb") as out:
        if isinstance(src, IsoSource) and src.disc.raw:
            print("  (a raw .bin CD image: the copy is a plain 2048-byte-sector ISO)")
            sectors = src.disc.sectors()
            copy_with_progress(lambda off, n: src.disc.read(off // 2048, n), sectors * 2048, out)
        elif isinstance(src, GameCubeSource) and src.kind == "rvz":
            print("  (an RVZ image: the copy is a plain GCM/ISO image)")
            copy_with_progress(src.image.read, src.image.size, out)
        else:
            out.close()
            shutil.copyfile(image, tmp)
    with open(tmp, "r+b") as f:
        for path, off, room, data, record in plan:
            f.seek(off)
            f.write(data + bytes(max(0, min(room, (len(data) + 2047) // 2048 * 2048) - len(data))
                                 if not isinstance(src, GameCubeSource) else 0))
            if isinstance(src, IsoSource):
                f.seek(record + 10)
                f.write(struct.pack("<I", len(data)) + struct.pack(">I", len(data)))
            elif isinstance(src, XisoSource):
                f.seek(record + 4)
                f.write(struct.pack("<II", (off - src.base) // 2048, len(data)))
            else:
                f.seek(src.fst_off + record * 12 + 8)
                f.write(struct.pack(">I", len(data)))
    src.close()
    os.replace(tmp, new_image)
    # read every patched file back
    check = open_source(new_image)
    got = {p: (s, h) for p, s, h in check.files}
    bad = []
    for path, off, room, data, record in plan:
        s, h = got.get(path, (None, None))
        if s != len(data) or check.read(h, s) != data:
            bad.append(path)
    check.close()
    for path, off, room, data, record in plan:
        print("  %s: %d bytes%s" % (path, len(data), "" if off else ""))
    if bad:
        raise DumpError("read-back check failed for: %s" % ", ".join(bad))
    print("patched %d file%s into %s, read back and checked (%.1f s)" % (
        len(plan), "" if len(plan) == 1 else "s", new_image, time.time() - t0))
    return 0


# ===========================================================================

def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    command = "dump"
    if argv and argv[0] in ("dump", "import", "patch-image"):
        command = argv.pop(0)
    if command == "dump":
        ap = argparse.ArgumentParser(prog="zaxis_texdump.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("image", help="disc image (PS2 .iso/.bin, Xbox .iso, GameCube .rvz/.iso) or folder")
        ap.add_argument("out", nargs="?", help="output folder (never next to the image)")
        ap.add_argument("--game", choices=sorted(GAMES), help="override the detected game")
        mode = ap.add_mutually_exclusive_group()
        mode.add_argument("--png", dest="mode", action="store_const", const="png", help="PNG files (default)")
        mode.add_argument("--raw", dest="mode", action="store_const", const="raw",
                          help="each picture as the game stores it (.bmp, .tga, .dds, .xpr)")
        mode.add_argument("--both", dest="mode", action="store_const", const="both", help="PNG and raw")
        ap.add_argument("--only", action="append", metavar="PATTERN",
                        help="only matching files or textures (glob on 'disc/path' and 'disc/path/name'; "
                             "without * or ? any part matches); may be repeated")
        ap.add_argument("--list", action="store_true", help="list the textures, write nothing")
        ap.add_argument("--no-mips", action="store_true", help="skip the smaller mip levels")
        ap.add_argument("--keep-key", action="store_true",
                        help="show the colour key (magenta palette entries) opaque instead of transparent")
        ap.add_argument("-q", "--quiet", action="store_true", help="no per-file progress")
        ap.set_defaults(mode="png")
        a = ap.parse_args(argv)
        if not a.out and not a.list:
            ap.error("give an output folder (or --list)")
        fn = cmd_dump
    elif command == "import":
        ap = argparse.ArgumentParser(prog="zaxis_texdump.py import",
                                     description="Write edited PNGs of a dump back into the game's files: a "
                                                 "modified copy of each changed file, in MODS (same paths "
                                                 "as on the disc).")
        ap.add_argument("dump", help="the folder a PNG dump wrote (with zaxis_texdump.json)")
        ap.add_argument("mods", help="folder for the modified game files")
        ap.add_argument("--image", help="the disc image, if it moved since the dump")
        ap.add_argument("--resize", action="store_true", help="scale PNGs of another size to the texture's size")
        ap.add_argument("--only", action="append", metavar="PATTERN", help="only matching files or textures")
        ap.add_argument("--reencode-all", action="store_true",
                        help="treat every PNG as edited (a round-trip test of the encoders)")
        a = ap.parse_args(argv)
        fn = cmd_import
    else:
        ap = argparse.ArgumentParser(prog="zaxis_texdump.py patch-image",
                                     description="Write modified game files into a copy of the disc image. "
                                                 "The original image is only read. PS2 .bin and GameCube "
                                                 "RVZ images become a plain ISO/GCM copy.")
        ap.add_argument("mods", help="the folder import wrote")
        ap.add_argument("image", help="the original disc image")
        ap.add_argument("new_image", help="the patched copy to write")
        ap.add_argument("--force", action="store_true", help="overwrite NEW_IMAGE if it exists")
        a = ap.parse_args(argv)
        fn = cmd_patch
    try:
        return fn(a)
    except DumpError as e:
        print("zaxis_texdump: %s" % e, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("zaxis_texdump: stopped", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
