#!/usr/bin/env python3
"""The Xbox release's files for the native port, from your own Xbox disc image,
at build time only (the app never reads a disc image).

    python3 -I tools/xbox_files.py extract --iso <Xbox image> --cache <dir>
    python3 -I tools/xbox_files.py bundle --cache <dir> --out <dir> [--ffmpeg <ffmpeg>]

extract: the whole disc into <cache> (media/..., default.xbe), once per image
(size and modification time in <cache>/.stamp). The image is an XISO (the
XDVDFS volume at 0x10000) or a full image with the game partition at a known
offset. Python 3.8+, standard library only (Windows build machine too).

bundle: the files the port reads at run time (runtime/ps2_xbox_disc.h: the
app sets ZX_XBOX_DIR to Resources/xbox) into <out>/assets/..., as listed
in tools/xbox-files.txt (paths relative to media/assets, `*` and `?`
allowed, any case), and the Zoo's level-select flythrough as an MPEG-2 PSM
like the PS2's (Movies/Flythrus/ZOO.PSM, 512x256, made with ffmpeg if there
is one; the app shows a still picture otherwise).

Never commit what this makes: it is your disc's data.
"""
import argparse
import fnmatch
import os
import shutil
import struct
import subprocess
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


def say(text):
    print("[xbox] " + text, flush=True)


def find_volume(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 0x10000)
        head = f.read(28)
        if head[:20] == MAGIC:
            root, size = struct.unpack_from("<II", head, 20)
            return base, root, size
    return None


def walk(f, base, sector, size, path, out):
    """Every (relative path, sector, size) below a directory table."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack, seen = [0], set()
    while stack:
        index = stack.pop()
        at = index * 4
        if index in seen or at + 14 > len(table):
            continue
        seen.add(index)
        left, right, start, length, attributes, name_length = struct.unpack_from("<HHIIBB", table, at)
        if left == 0xFFFF:
            continue
        name = table[at + 14:at + 14 + name_length].decode("latin-1")
        if left:
            stack.append(left)
        if right:
            stack.append(right)
        child = path + [name]
        if attributes & 0x10:
            walk(f, base, start, length, child, out)
        else:
            out.append(("/".join(child), start, length))


def extract(args):
    iso, cache = args.iso, args.cache
    stat = os.stat(iso)
    stamp = "%d %d %s" % (stat.st_size, int(stat.st_mtime), os.path.basename(iso))
    stamp_file = os.path.join(cache, ".stamp")
    if os.path.isfile(stamp_file) and open(stamp_file).read() == stamp:
        say("Xbox files already extracted in %s" % cache)
        return 0
    with open(iso, "rb") as f:
        volume = find_volume(f)
        if not volume:
            say("%s is not an Xbox disc image (no XDVDFS volume)" % iso)
            return 1
        base, root, size = volume
        files = []
        walk(f, base, root, size, [], files)
        if not any(p.lower() == "default.xbe" for p, _, _ in files):
            say("%s: no default.xbe, not a game disc" % iso)
            return 1
        tmp = cache + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        total = 0
        for path, start, length in files:
            target = os.path.join(tmp, *path.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            f.seek(base + start * SECTOR)
            with open(target, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        break
                    o.write(chunk)
                    left -= len(chunk)
            total += length
    shutil.rmtree(cache, ignore_errors=True)
    os.replace(tmp, cache)
    with open(stamp_file, "w") as s:
        s.write(stamp)
    say("extracted %d files, %d MB, into %s" % (len(files), total >> 20, cache))
    return 0


def assets_root(cache):
    for candidate in (os.path.join(cache, "media", "assets"), os.path.join(cache, "assets"), cache):
        if os.path.isdir(candidate) and os.path.basename(candidate).lower() == "assets":
            return candidate
    # any case
    for name in os.listdir(cache):
        if name.lower() == "media":
            for inner in os.listdir(os.path.join(cache, name)):
                if inner.lower() == "assets":
                    return os.path.join(cache, name, inner)
    return None


def bundle(args):
    assets = assets_root(args.cache)
    if not assets:
        say("no media/assets in %s" % args.cache)
        return 1
    listing = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xbox-files.txt")
    patterns = []
    for line in open(listing, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            patterns.append(line.lower())
    out_assets = os.path.join(args.out, "assets")
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(out_assets)
    count = size = 0
    for folder, _, names in os.walk(assets):
        for name in names:
            source = os.path.join(folder, name)
            relative = os.path.relpath(source, assets).replace(os.sep, "/")
            if any(fnmatch.fnmatchcase(relative.lower(), p) for p in patterns):
                target = os.path.join(out_assets, *relative.split("/"))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(source, target)
                count += 1
                size += os.path.getsize(source)
    say("bundled %d Xbox files (%d MB) into %s" % (count, size >> 20, args.out))
    # The Zoo's flythrough as the PS2 level select plays them: MPEG-2 video
    # in an MPEG program stream, 512x256, no sound.
    movie = None
    for folder, _, names in os.walk(assets):
        for name in names:
            if name.lower() == "zoo.bik" and os.path.basename(folder).lower() == "flythrus":
                movie = os.path.join(folder, name)
    ffmpeg = args.ffmpeg or shutil.which("ffmpeg")
    if movie and ffmpeg:
        target = os.path.join(out_assets, "Movies", "Flythrus", "ZOO.PSM")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        command = [ffmpeg, "-v", "error", "-y", "-i", movie, "-an", "-vf", "scale=512:256",
                   "-c:v", "mpeg2video", "-b:v", "4800k", "-maxrate", "6000k", "-bufsize", "1835k",
                   "-g", "15", "-r", "30000/1001", "-f", "vob", target]
        if subprocess.run(command).returncode == 0:
            say("Zoo flythrough: %s (%d KB)" % (target, os.path.getsize(target) >> 10))
        else:
            say("ffmpeg could not make the Zoo flythrough; the level select shows a still picture")
    elif movie:
        say("no ffmpeg: no Zoo flythrough (the level select shows a still picture)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    e = sub.add_parser("extract")
    e.add_argument("--iso", required=True)
    e.add_argument("--cache", required=True)
    b = sub.add_parser("bundle")
    b.add_argument("--cache", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--ffmpeg")
    args = parser.parse_args()
    return extract(args) if args.command == "extract" else bundle(args)


if __name__ == "__main__":
    sys.exit(main())
