#!/usr/bin/env python3
"""Read a GameCube disc image (RVZ, or a plain GCM/ISO) at build time.

    python3 -I tools/gc_disc.py info    <image>
    python3 -I tools/gc_disc.py list    <image> [PATTERN ...] [--sys] [--long]
    python3 -I tools/gc_disc.py extract <image> --out <dir> [PATTERN ...] [--sys]
    python3 -I tools/gc_disc.py verify  <image> [PATTERN ...]

PATTERN is a path on the disc (assets/levels/Local/LOCAL.zs), a directory
(assets/levels/TUT/Estate: everything below it) or a glob with `*`, `?` and
`[...]` (assets/levels/*/*.log; `*` also crosses `/`); any case. No pattern
means every file. --sys adds the system files as sys/boot.bin, sys/bi2.bin,
sys/apploader.img, sys/main.dol and sys/fst.bin.

info: the image header (game ID, title, codec, chunk size, file count).
list: path and size of each file, by disc offset (--long adds the offset).
extract: the files into <dir>/<disc path>, plus <dir>/index.tsv (path, size,
disc offset) for what was written; every written file's size is checked
against the FST.
verify: reads every chosen file (all by default) and checks its length
against the FST; with no pattern it also reads the whole image once and
prints its CRC32, MD5 and SHA-1 (comparable with redump.org's).

RVZ (Dolphin's format): zstd, bzip2, LZMA, LZMA2 or stored groups, with
RVZ's packing (junk data regenerated from its seed). GameCube images only
(Wii partitions and WIA are refused). bzip2 and LZMA use the standard
library; zstd uses Python 3.14's compression.zstd, else the `zstandard`
package, else the `zstd` command.

The disc and everything read from it are your data: never commit them.
"""
import argparse
import binascii
import bz2
import fnmatch
import hashlib
import lzma
import os
import shutil
import struct
import subprocess
import sys
from collections import OrderedDict

RVZ_MAGIC = b"RVZ\x01"
WIA_MAGIC = b"WIA\x01"
GC_MAGIC = 0xC2339F3D  # at disc offset 0x1C
CODECS = {0: "none", 1: "purge", 2: "bzip2", 3: "lzma", 4: "lzma2", 5: "zstd"}
BLOCK = 0x8000  # RVZ junk alignment (Wii block size, used for GC too)


def fail(text):
    sys.exit("gc_disc: " + text)


# --- zstd from whatever is available ---------------------------------------

def _zstd_backend():
    try:
        from compression import zstd  # Python 3.14+
        return lambda data, size: zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
        dctx = zstandard.ZstdDecompressor()
        return lambda data, size: dctx.decompress(data, max_output_size=max(size, 1))
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if exe:
        def run(data, size):
            return subprocess.run([exe, "-dcq"], input=data, stdout=subprocess.PIPE,
                                  check=True).stdout
        return run
    return None


# --- RVZ junk data: Dolphin's lagged Fibonacci generator --------------------

LFG_K, LFG_J, SEED_WORDS = 521, 32, 17


class JunkGenerator:
    def __init__(self, seed):
        b = list(struct.unpack(">17I", seed))
        for i in range(SEED_WORDS, LFG_K):
            b.append(((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1])
        self.buf = [(x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000) for x in b]
        for _ in range(4):
            self._forward()
        self.pos = 0
        self.bytes = None

    def _forward(self):
        b = self.buf
        for i in range(LFG_J):
            b[i] ^= b[i + LFG_K - LFG_J]
        for i in range(LFG_J, LFG_K):
            b[i] ^= b[i - LFG_J]
        self.bytes = None

    def _block(self):
        if self.bytes is None:
            self.bytes = struct.pack(">521I", *self.buf)
        return self.bytes

    def skip(self, count):
        self.pos += count
        while self.pos >= LFG_K * 4:
            self._forward()
            self.pos -= LFG_K * 4

    def take(self, count):
        out = []
        while count:
            n = min(count, LFG_K * 4 - self.pos)
            out.append(self._block()[self.pos:self.pos + n])
            self.pos += n
            count -= n
            if self.pos == LFG_K * 4:
                self._forward()
                self.pos = 0
        return b"".join(out)


def rvz_unpack(packed, disc_offset, size):
    """Expand an RVZ-packed group: (u32 size, data) records, size bit 31 = junk
    regenerated from the 68-byte seed that follows."""
    out, at, done = [], 0, 0
    while at < len(packed) and done < size:
        word, = struct.unpack_from(">I", packed, at)
        at += 4
        length = word & 0x7FFFFFFF
        if word & 0x80000000:
            gen = JunkGenerator(packed[at:at + SEED_WORDS * 4])
            at += SEED_WORDS * 4
            gen.skip(disc_offset % BLOCK)
            out.append(gen.take(length))
        else:
            out.append(packed[at:at + length])
            at += length
        disc_offset += length
        done += length
    return b"".join(out)


# --- images ----------------------------------------------------------------

class PlainImage:
    kind = "gcm"

    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size

    def read(self, offset, size):
        self.f.seek(offset)
        return self.f.read(size)

    def describe(self):
        return "plain GCM/ISO image, %d bytes" % self.size


class RvzImage:
    kind = "rvz"

    def __init__(self, path, cache_groups=16):
        self.f = open(path, "rb")
        h1 = self.f.read(0x48)
        (magic, self.version, compat, h2_size, h2_hash, self.size, self.file_size,
         h1_hash) = struct.unpack(">4sIII20sQQ20s", h1)
        if magic == WIA_MAGIC:
            fail("WIA images are not supported (convert to RVZ or ISO in Dolphin)")
        if magic != RVZ_MAGIC:
            fail("not an RVZ image")
        if hashlib.sha1(h1[:0x34]).digest() != h1_hash:
            fail("RVZ header 1 hash mismatch")
        h2 = self.f.read(h2_size)
        if hashlib.sha1(h2).digest() != h2_hash:
            fail("RVZ header 2 hash mismatch")
        (self.disc_type, self.codec, self.level, self.chunk) = struct.unpack_from(">IIiI", h2, 0)
        self.disc_header = h2[0x10:0x90]
        (n_part, part_size, part_off, part_hash, n_raw, raw_off, raw_size,
         n_group, group_off, group_size, cdata_size) = struct.unpack_from(
            ">IIQ20sIQIIQIB", h2, 0x90)
        self.cdata = h2[0xD5:0xD5 + cdata_size]
        if self.disc_type != 1 or n_part:
            fail("not a GameCube image (Wii partitions are not supported)")
        if self.codec not in (0, 2, 3, 4, 5):
            fail("unsupported RVZ codec %d" % self.codec)
        self._zstd = _zstd_backend() if self.codec == 5 else None
        if self.codec == 5 and self._zstd is None:
            fail("zstd image: needs Python 3.14+, the zstandard package or the zstd command")
        raw = self._meta(raw_off, raw_size, n_raw * 24)
        self.raw = [struct.unpack_from(">QQII", raw, i * 24) for i in range(n_raw)]
        groups = self._meta(group_off, group_size, n_group * 12)
        self.groups = [struct.unpack_from(">III", groups, i * 12) for i in range(n_group)]
        self.cache = OrderedDict()
        self.cache_groups = cache_groups

    def describe(self):
        return ("RVZ v%08X, %s level %d, chunk %d KiB, %d groups, %d raw entries, "
                "image %d bytes from %d"
                % (self.version, CODECS[self.codec], self.level, self.chunk // 1024,
                   len(self.groups), len(self.raw), self.size, self.file_size))

    def _decompress(self, data, size):
        c = self.codec
        if c == 0:
            return data
        if c == 2:
            return bz2.decompress(data)
        if c == 5:
            return self._zstd(data, size)
        if c == 3:
            props = self.cdata
            d = props[0]
            filt = {"id": lzma.FILTER_LZMA1, "lc": d % 9, "lp": (d // 9) % 5, "pb": d // 45,
                    "dict_size": struct.unpack_from("<I", props, 1)[0]}
        else:
            p = self.cdata[0]
            dict_size = 0xFFFFFFFF if p == 40 else (2 | (p & 1)) << (p // 2 + 11)
            filt = {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[filt])
        return dec.decompress(data, max_length=size)

    def _meta(self, offset, csize, size):
        self.f.seek(offset)
        out = self._decompress(self.f.read(csize), size)
        if len(out) < size:
            fail("RVZ table truncated")
        return out

    def _group(self, index, disc_offset, size):
        hit = self.cache.get(index)
        if hit is not None:
            self.cache.move_to_end(index)
            return hit
        off4, dsize, packed = self.groups[index]
        compressed = bool(dsize & 0x80000000)
        dsize &= 0x7FFFFFFF
        if dsize == 0:
            data = bytes(size)
        else:
            self.f.seek(off4 << 2)
            data = self.f.read(dsize)
            if compressed:
                data = self._decompress(data, packed or size)
            if packed:
                data = rvz_unpack(data[:packed], disc_offset, size)
        if len(data) < size:
            fail("group %d decoded to %d bytes, wanted %d" % (index, len(data), size))
        data = data[:size]
        self.cache[index] = data
        if len(self.cache) > self.cache_groups:
            self.cache.popitem(last=False)
        return data

    def read(self, offset, size):
        out = bytearray()
        end = min(offset + size, self.size)
        if offset < 0x80:
            n = min(end, 0x80) - offset
            out += self.disc_header[offset:offset + n]
            offset += n
        while offset < end:
            for data_off, data_size, g0, ng in self.raw:
                base = data_off - data_off % BLOCK
                limit = data_off + data_size
                if base <= offset < limit:
                    rel = offset - base
                    gi = rel // self.chunk
                    if gi >= ng:
                        break
                    goff = gi * self.chunk
                    gsize = min(self.chunk, limit - base - goff)
                    chunk = self._group(g0 + gi, base + goff, gsize)
                    n = min(end, base + goff + gsize) - offset
                    out += chunk[rel - goff:rel - goff + n]
                    offset += n
                    break
            else:
                # Not covered by any raw data entry: zeros (none on GC images).
                nxt = min([d for d, _, _, _ in self.raw if d > offset] + [end])
                out += bytes(nxt - offset)
                offset = nxt
        return bytes(out)


def open_image(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic in (RVZ_MAGIC, WIA_MAGIC):
        return RvzImage(path)
    return PlainImage(path)


# --- the GameCube file system ----------------------------------------------

class Disc:
    def __init__(self, path):
        self.image = open_image(path)
        head = self.image.read(0, 0x440)
        if struct.unpack_from(">I", head, 0x1C)[0] != GC_MAGIC:
            fail("no GameCube disc magic at 0x1C")
        self.game_id = head[:6].decode("ascii", "replace")
        self.title = head[0x20:0x400].split(b"\0")[0].decode("latin-1")
        self.dol_off, self.fst_off, self.fst_size = struct.unpack_from(">III", head, 0x420)
        self.fst = self.image.read(self.fst_off, self.fst_size)
        self.files = self._walk()

    def _walk(self):
        fst = self.fst
        count = struct.unpack_from(">I", fst, 8)[0]
        names = count * 12
        out, stack = [], [(count, "")]  # (end index, directory path)
        for i in range(1, count):
            while stack and i >= stack[-1][0]:
                stack.pop()
            word, a, b = struct.unpack_from(">III", fst, i * 12)
            at = names + (word & 0xFFFFFF)
            name = fst[at:fst.index(b"\0", at)].decode("latin-1")
            path = stack[-1][1] + name if stack else name
            if word >> 24:
                stack.append((b, path + "/"))
            else:
                out.append((path, a, b))
        return out

    def system_files(self):
        img = self.image
        app = img.read(0x2440, 0x20)
        app_size = 0x20 + struct.unpack_from(">I", app, 0x14)[0] + struct.unpack_from(">I", app, 0x18)[0]
        dol = img.read(self.dol_off, 0x100)
        offs = struct.unpack_from(">18I", dol, 0)
        sizes = struct.unpack_from(">18I", dol, 0x90)
        dol_size = max([0x100] + [o + s for o, s in zip(offs, sizes) if s])
        return [("sys/boot.bin", 0, 0x440), ("sys/bi2.bin", 0x440, 0x2000),
                ("sys/apploader.img", 0x2440, app_size), ("sys/main.dol", self.dol_off, dol_size),
                ("sys/fst.bin", self.fst_off, self.fst_size)]


def select(files, patterns):
    if not patterns:
        return list(files)
    chosen, used = [], [False] * len(patterns)
    for path, off, size in files:
        low = path.lower()
        for n, pat in enumerate(patterns):
            p = pat.lower().strip("/")
            if low == p or low.startswith(p + "/") or fnmatch.fnmatchcase(low, p):
                chosen.append((path, off, size))
                used[n] = True
                break
    for pat, ok in zip(patterns, used):
        if not ok:
            print("gc_disc: nothing matches %s" % pat, file=sys.stderr)
    return chosen


def chosen_files(disc, args):
    files = disc.files + (disc.system_files() if getattr(args, "sys", False) else [])
    return sorted(select(files, args.patterns), key=lambda e: e[1])


def copy_out(image, off, size, dst=None, digest=None):
    done, step = 0, 4 << 20
    while done < size:
        data = image.read(off + done, min(step, size - done))
        if not data:
            break
        if dst:
            dst.write(data)
        if digest:
            for d in digest:
                d.update(data)
        done += len(data)
    return done


# --- commands ---------------------------------------------------------------

def cmd_info(args):
    disc = Disc(args.image)
    total = sum(s for _, _, s in disc.files)
    print("game id  %s" % disc.game_id)
    print("title    %s" % disc.title)
    print("image    %s" % disc.image.describe())
    print("fst      0x%X, %d bytes; main.dol at 0x%X" % (disc.fst_off, disc.fst_size, disc.dol_off))
    print("files    %d, %d bytes" % (len(disc.files), total))


def cmd_list(args):
    disc = Disc(args.image)
    files = chosen_files(disc, args)
    for path, off, size in files:
        if args.long:
            print("%10X %10d  %s" % (off, size, path))
        else:
            print("%10d  %s" % (size, path))
    print("%d files, %d bytes" % (len(files), sum(s for _, _, s in files)), file=sys.stderr)


def cmd_extract(args):
    disc = Disc(args.image)
    files = chosen_files(disc, args)
    if not files:
        fail("no files chosen")
    out = os.path.abspath(args.out)
    bad = 0
    rows = []
    for path, off, size in files:
        dest = os.path.join(out, *path.split("/"))
        if not os.path.abspath(dest).startswith(out + os.sep):
            fail("unsafe path on disc: %s" % path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            copy_out(disc.image, off, size, f)
        got = os.path.getsize(dest)
        if got != size:
            print("gc_disc: %s: wrote %d bytes, FST says %d" % (path, got, size), file=sys.stderr)
            bad += 1
        rows.append("%s\t%d\t0x%X" % (path, size, off))
    # index.tsv: what this and earlier extractions of the same disc wrote here.
    index = os.path.join(out, "index.tsv")
    head = "# %s %s, from %s\n" % (disc.game_id, disc.title, os.path.basename(args.image))
    merged = {}
    if os.path.exists(index):
        with open(index) as f:
            old = f.read().splitlines()
        if old and old[0].split(",")[0] == head.split(",")[0]:
            merged = {r.split("\t")[0]: r for r in old if r and not r.startswith("#")}
    merged.update({r.split("\t")[0]: r for r in rows})
    with open(index, "w") as f:
        f.write(head + "# path\tsize\tdisc offset\n")
        f.write("\n".join(sorted(merged.values(), key=str.lower)) + "\n")
    print("%d files, %d bytes into %s (sizes checked against the FST: %d wrong)"
          % (len(files), sum(s for _, _, s in files), out, bad))
    if bad:
        sys.exit(1)


def cmd_verify(args):
    disc = Disc(args.image)
    files = chosen_files(disc, args)
    bad = 0
    size = disc.image.size
    for path, off, fsize in files:
        if off + fsize > size:
            print("gc_disc: %s runs past the image end" % path, file=sys.stderr)
            bad += 1
            continue
        got = copy_out(disc.image, off, fsize)
        if got != fsize:
            print("gc_disc: %s: read %d bytes, FST says %d" % (path, got, fsize), file=sys.stderr)
            bad += 1
    print("%s: %d files read, %d with the wrong length" % (disc.game_id, len(files), bad))
    if not args.patterns:
        md5, sha1 = hashlib.md5(), hashlib.sha1()

        class Crc:
            value = 0

            def update(self, data):
                self.value = binascii.crc32(data, self.value)
        crc = Crc()
        copy_out(disc.image, 0, size, digest=(md5, sha1, crc))
        print("image    %d bytes, crc32 %08x, md5 %s, sha1 %s"
              % (size, crc.value, md5.hexdigest(), sha1.hexdigest()))
    if bad:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("info", cmd_info), ("list", cmd_list), ("extract", cmd_extract),
                     ("verify", cmd_verify)):
        p = sub.add_parser(name)
        p.add_argument("image")
        if name != "info":
            p.add_argument("patterns", nargs="*")
            p.add_argument("--sys", action="store_true", help="include sys/*.bin, main.dol")
        if name == "list":
            p.add_argument("--long", action="store_true", help="show disc offsets")
        if name == "extract":
            p.add_argument("--out", required=True)
        p.set_defaults(fn=fn)
    # Patterns may come after options too (argparse alone stops at the first).
    args, extra = ap.parse_known_args()
    unknown = [a for a in extra if a.startswith("-")]
    if unknown or (extra and not hasattr(args, "patterns")):
        ap.error("unrecognized arguments: %s" % " ".join(unknown or extra))
    if extra:
        args.patterns += extra
    args.fn(args)


if __name__ == "__main__":
    main()
