#!/usr/bin/env python3
"""BMX XXX's Xbox and GameCube releases' content made into the PS2
release's formats, at build time only: the app bundles what this writes and
never reads a disc image or converts anything while BMX XXX runs.

    python3 -I bmx_releases.py convert --ps2 <PS2 disc files> --xbox <Xbox files> --gc <GameCube files>
                                       --out <dir> [--ffmpeg PATH] [--only challenges,strings,editor,movies,levels,
                                       create,trailers,songs]
    python3 -I bmx_releases.py strings --ps2 <PS2 disc files> --xbox <Xbox files> --gc <GameCube files>
                                                  the string tables' counts, written nowhere
    python3 -I bmx_releases.py selftest           the merges on hand-made records

--ps2 is the PS2 disc's files (its root or ASSETS), --xbox the Xbox disc's
as tools/xbox_files.py extracts them (its root, media or media/assets),
--gc the GameCube disc's as tools/gc_disc.py extracts them (its root or
assets). Any case. convert writes, under <out>:

  challenges/xbox/<LEVEL>/OBJCTV.TXT
      The challenge scripts of the four levels whose Xbox and GameCube
      scripts (the same files) differ from the PS2's: CHL_MNCO, CHL_NASA,
      CHL_SHEP, CHL_SYRA. They play the challenges' success and failure
      lines (the PS2 plays "AUDIO_PLACEHOLDER" or nothing there: its speech
      banks SPEECHMO, SPEECHLP, SPEECHSH and SPEECHSY.ZSD hold every one of
      those phrases), enable Sheep Hills' four pickup tethers, and fix a
      NASA and a Syracuse step; as they are.
  challenges/ps2-timers/<LEVEL>/OBJCTV.TXT
      The same with each TIMEOUT value the PS2's (Syracuse's two 45 s
      timers are 60 s on the Xbox and GameCube): the balance is the PS2's
      unless the player picks the Xbox's (Gameplay Tuning).
  strings/<table>
      The French, German and Spanish string tables ("string.common.fre",
      "string.level.sy.ger", ...: the entries of the PS2's STRING.ZAL by
      the same names) with each text the PS2 leaves in English that the
      GameCube (else the Xbox) translates, where that release's English is
      the PS2's word for word (a text the other release rewrote, for its
      pad or its levels, keeps the PS2's). The PS2's entries, in its order,
      with its translations; only the tables something is added to.
  movies/ASSETS/MOVIES/MENU/<LEVEL>.PSM
      The level select's preview movies, the Xbox's movies/menu/*.XMV
      (WMV2, 512x256, 29.97 frames a second, no sound) as the PSM files the
      PS2's level select names (its UI_menu: assets\\movies\\menu\\bronx.psm
      and on): MPEG-2 in an MPEG-2 program stream, no sound, as a PSM is.
  movies/ASSETS/MOVIES/ZAXIS.PSS
      The Z-Axis logo movie (the Xbox's movies/zaxis.xmv: 640x480, 243
      pictures over 11.6 s, no sound), the PS2's movie row 1 ("ZAxis"),
      at the PS2 movies' 512x256 and 29.97 frames a second (shown 4:3, as
      the gallery shows every movie).
  editor/OBJECT.OBJ
      The park editor's piece list: the PS2's, with the pieces the
      GameCube's editor/object.obj (the Xbox's object00.obj, the same file)
      adds after the PS2's last, so every piece keeps its number (a saved
      park names its pieces by number): POOL (Editobj_bowl_kidneypool, in
      BOWLS) and RECEPTACLE (dynamicsdef_trashcan_, a dynamic object), whose
      meshes the PS2's editor/Objects already holds. Names in capitals, as
      the PS2's.
  levels/ASSETS/LEVELS/<LEVEL>/<FILE>
      The four levels the GameCube disc alone has files for, whose rows
      the PS2's level table (g_levels) already holds: CMP_SCRT (8, the
      Secret Level), TST_FLAT (9), TST_GRND (10) and TST_VERT (11). Their
      files are the PS2's formats (art viewer exports: TBMP textures, the
      PS2's GEO, INS and DAT chunks), copied as they are, names in
      capitals; a level any of whose textures is not a TBMP (the GameCube's
      own TDDS) is left out. (TST_VERT2 is TST_VERT's scene exported again
      for the GameCube, its texture a DDS: it has no row, and is not made.)
  create/PARTS.ZAL, create/TEXTUREA.ZAL .. TEXTURED.ZAL, create/added.txt
      The create-a-rider libraries: the PS2's, each with the entries the
      other releases' have and it lacks after its own, so every PS2 entry
      keeps its place (named libraries: their NAME chunk kept). Parts: the
      GameCube's two more (GFU_NudeTop, GMHat_BballSw), the PS2's formats
      but for the 8x8 texture every part carries (its TDDS made the PS2's
      TBMP). Textures: the Xbox's (else the GameCube's) 51 more as they are
      (8-bit BMPs, as the PS2's): green skins and faces (_Hul), tops (one
      _Topless), suspenders, a soul patch, clown shoes, hats; one going on a
      part the parts made lack is left out. added.txt names the textures
      added, one a line (the runtime's lists and saved riders).
  movies/ASSETS/MOVIES/COMPEDGE/TUROK.PSS, VEXX.PSS
      The Competitive Edge trailers only the Xbox has (movies/CompEdge/
      turok.xmv, vexx.xmv: 512x432 WMV2 with 44.1 kHz sound), which the
      PS2's FrameAKAdemos names (AKA_OPT05, AKA_OPT06): 512x256 MPEG-2 with
      the PS2 movies' 48 kHz stereo PCM, as its other trailers are.
  music/<NN>.MP3, music/songs.txt
      The songs on the Xbox disc (audio/*.wma: 28) its music bank
      (audio/music.zsd, the PS2's 20) names none of, as MP3 (FLAC without
      FFmpeg's MP3 encoder), in name order; songs.txt a line each: the
      file, the Xbox's, the title (the Xbox file's name: the disc has no
      tags) and the play time. The runtime serves them as MUSIC.ZSD's waves
      20-27 (its tables served with them; the file never copied).
  manifest.txt
      What was made, with counts.

The runtime (game_overrides/bmx_xxx_releases.cpp, ZX_BMX_RELEASES_DIR)
serves these behind its settings.

The disc files and everything made from them are your data: never commit them.
"""
import argparse
import difflib
import os
import shutil
import struct
import subprocess
import sys

CHALLENGE_LEVELS = ("CHL_MNCO", "CHL_NASA", "CHL_SHEP", "CHL_SYRA")
LANGUAGES = ("fre", "ger", "spa")
ENGLISH = "eus"
TABLE_GROUPS = ("common",) + tuple("level." + g for g in ("dm", "lp", "mo", "my", "ra", "se", "sh", "sy", "tf", "tg",
                                                           "tv", "ug"))
# The PS2 level select's PSM names (UI_menu FrameLevelSelect, by menu value)
# and the Xbox movie of each.
MENU_MOVIES = (("BRONX", "bronx"), ("DAM", "dam"), ("RAMPAGE", "rampage"), ("SHEEP", "sheep"),
               ("SYRACUSE", "syracuse"), ("UGP", "ugp"), ("LASVEGAS", "lasvegas"), ("NASA", "nasa"))
MOVIE_SIZE = (512, 256)
# The GameCube's levels with rows in the PS2's level table (g_levels) and no
# files on the PS2 disc, in the PS2's formats.
GC_LEVELS = ("CMP_SCRT", "TST_FLAT", "TST_GRND", "TST_VERT")
MOVIE_RATE = "30000/1001"
# The create-a-rider libraries (assets\\create): its four texture libraries and its parts.
CREATE_TEXTURES = ("TEXTUREA", "TEXTUREB", "TEXTUREC", "TEXTURED")
CREATE_PARTS = "PARTS"
# A named library's entries start on this (the PS2's create libraries' alignment).
LIBRARY_ALIGN = 32
# The Competitive Edge trailers the PS2's FrameAKAdemos names (assets\\movies\\CompEdge\\<name>.pss) and only
# the Xbox has (its AKA_OPT05 and AKA_OPT06 lines).
TRAILERS = ("TUROK", "VEXX")
TRAILER_BITRATE = "5000k"
# The Xbox's songs no table names (the runtime serves them as MUSIC.ZSD's waves 20-27): MP3 (FFmpeg's libmp3lame),
# else FLAC, what every build of the app decodes.
SONG_BITRATE = "192k"
ASF_FILE_PROPERTIES = bytes.fromhex("A1DCAB8C47A9CF118EE400C00C205365")
def say(text):
    print("bmx_releases: " + text, flush=True)


def ifind(root, rel):
    cur = root
    for part in rel.split("/"):
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = [n for n in names if n.lower() == part.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def assets_of(root, *candidates):
    for c in candidates:
        p = ifind(root, c) if c else root
        if p and os.path.isdir(p) and os.path.basename(p).lower() == "assets":
            return p
    return None


def name_hash(name):
    """The engine's name hash (zge/text/strings.h nameHashOf): ELF hash, ASCII letters upper-cased."""
    h = 0
    for c in name.encode("latin1"):
        if 97 <= c <= 122:
            c -= 32
        h = ((h << 4) + c) & 0xFFFFFFFF
        high = h & 0xF0000000
        if high:
            h ^= high >> 24
        h &= ~high & 0xFFFFFFFF
    return h


# ---- the ZDIR library (assets\string.zal) -------------------------------------------------------------------------

def read_zdir(data):
    """[(name hash, flags, bytes)] of a ZDIR library (DIDF, GRID, DLEN, OFST)."""
    if data[:4] != b"ZDIR" or data[8:12] != b"DIDF":
        raise ValueError("not a ZDIR library")
    count = struct.unpack_from("<I", data, 0x10)[0]
    grid = 0x18
    dlen = grid + 8 + 4 * count
    ofst = dlen + 8 + 4 * count
    if data[grid:grid + 4] != b"GRID" or data[dlen:dlen + 4] != b"DLEN" or data[ofst:ofst + 4] != b"OFST":
        raise ValueError("not a ZDIR library")
    out = []
    for i in range(count):
        key = struct.unpack_from("<I", data, grid + 8 + 4 * i)[0]
        word = struct.unpack_from("<I", data, dlen + 8 + 4 * i)[0]
        at = struct.unpack_from("<I", data, ofst + 8 + 4 * i)[0]
        out.append((key, word >> 28, data[at:at + (word & 0x0FFFFFFF)]))
    return out


def read_named_zdir(data):
    """[(name hash, flags, bytes, name)] of a ZDIR library with its NAME chunk (the names, NUL-separated, in the
    entries' order), as the create-a-rider libraries are."""
    count = struct.unpack_from("<I", data, 0x10)[0]
    at = 0x18 + 3 * (8 + 4 * count)
    if data[at:at + 4] != b"NAME":
        raise ValueError("a ZDIR library without names")
    size = struct.unpack_from("<I", data, at + 4)[0]
    names = data[at + 8:at + 8 + size].split(b"\0")[:count]
    return [(key, flags, blob, name.decode("latin1")) for (key, flags, blob), name in zip(read_zdir(data), names)]


def write_named_zdir(entries):
    """A ZDIR library with its NAME chunk, each entry at LIBRARY_ALIGN, the file padded to it (zeros)."""
    count = len(entries)
    names = b"".join(name.encode("latin1") + b"\0" for _, _, _, name in entries)
    names += b"\0" * (-len(names) % 4)
    body = bytearray(struct.pack("<4sIII", b"DIDF", 8, count, 0))
    body += struct.pack("<4sI", b"GRID", 4 * count) + b"".join(struct.pack("<I", key) for key, _, _, _ in entries)
    body += struct.pack("<4sI", b"DLEN", 4 * count) + b"".join(
        struct.pack("<I", (flags << 28) | len(blob)) for _, flags, blob, _ in entries)
    offsets = 8 + len(body) + 8
    body += struct.pack("<4sI", b"OFST", 4 * count) + bytes(4 * count)
    body += struct.pack("<4sI", b"NAME", len(names)) + names
    out = bytearray(struct.pack("<4sI", b"ZDIR", len(body))) + body
    for i, (_, _, blob, _) in enumerate(entries):
        out += bytes(-len(out) % LIBRARY_ALIGN)
        struct.pack_into("<I", out, offsets + 4 * i, len(out))
        out += blob
    out += bytes(-len(out) % LIBRARY_ALIGN)
    return bytes(out)


# ---- string tables (zge/text/strings.h StringTable) ----------------------------------------------------------------

def read_table(blob):
    """[(key, text bytes or None)] in the table's order (offset 0: no text)."""
    size, count = struct.unpack_from("<II", blob, 0)
    out = []
    for i in range(count):
        key, at = struct.unpack_from("<II", blob, 8 + 8 * i)
        if at == 0:
            out.append((key, None))
        else:
            end = blob.index(b"\0", at)
            out.append((key, blob[at:end]))
    return out


def write_table(entries):
    """A table's bytes: size, count, the entries, then each text once, in the entries' order."""
    head = 8 + 8 * len(entries)
    texts = bytearray()
    offsets = []
    for _, text in entries:
        if text is None:
            offsets.append(0)
        else:
            offsets.append(head + len(texts))
            texts += text + b"\0"
    out = bytearray(struct.pack("<II", head + len(texts), len(entries)))
    for (key, _), at in zip(entries, offsets):
        out += struct.pack("<II", key, at)
    out += texts
    return bytes(out)


def first_texts(entries):
    """key -> the text a lookup finds (the first entry of the key: stringTableText's linear search)."""
    out = {}
    for key, text in entries:
        out.setdefault(key, text)
    return out


def merge_table(ps2_english, ps2_language, others):
    """The PS2 table of a language with each text it leaves in English that another release translates, where that
    release's English is the PS2's. others: [(english entries, language entries)], the first that has one wins.
    Returns (entries, added, translated before)."""
    english = first_texts(ps2_english)
    sources = [(first_texts(e), first_texts(l)) for e, l in others]
    out = []
    seen = set()
    added = 0
    before = 0
    for key, text in ps2_language:
        first = key not in seen
        seen.add(key)
        if not first or text is None:
            out.append((key, text))
            continue
        if text != english.get(key):
            before += 1
            out.append((key, text))
            continue
        new = None
        for other_english, other_language in sources:
            theirs = other_language.get(key)
            if theirs is not None and other_english.get(key) == english.get(key) and theirs != other_english.get(key):
                new = theirs
                break
        if new is None:
            out.append((key, text))
        else:
            added += 1
            out.append((key, new))
    return out, added, before


def table_name(group, language):
    return "string.%s.%s" % (group, language)


def string_tables(ps2, xbox, gc):
    """{table name: (merged entries, added, translated before, entries)} for the tables something is added to, and
    the counts of every table."""
    libraries = {}
    for release, assets in (("ps2", ps2), ("xbox", xbox), ("gc", gc)):
        path = ifind(assets, "string.zal") if assets else None
        if not path:
            if release == "ps2":
                raise SystemExit("bmx_releases: no STRING.ZAL in the PS2 files")
            continue
        libraries[release] = {key: blob for key, _, blob in read_zdir(open(path, "rb").read())}
    made = {}
    counts = []
    for group in TABLE_GROUPS:
        english_key = name_hash(table_name(group, ENGLISH))
        if english_key not in libraries["ps2"]:
            continue
        ps2_english = read_table(libraries["ps2"][english_key])
        for language in LANGUAGES:
            key = name_hash(table_name(group, language))
            if key not in libraries["ps2"]:
                continue
            others = []
            for release in ("gc", "xbox"):
                lib = libraries.get(release, {})
                if english_key in lib and key in lib:
                    others.append((read_table(lib[english_key]), read_table(lib[key])))
            entries, added, before = merge_table(ps2_english, read_table(libraries["ps2"][key]), others)
            counts.append((table_name(group, language), len(first_texts(entries)), before, added))
            if added:
                made[table_name(group, language)] = entries
    return made, counts


# ---- challenge scripts ---------------------------------------------------------------------------------------------

def timeout_value(line):
    words = line.split()
    if len(words) == 2 and words[0] == "TIMEOUT" and words[1].startswith('"') and words[1].endswith('"'):
        return words[1]
    return None


def ps2_timers(ps2_text, xbox_text):
    """The Xbox script with each changed TIMEOUT line the PS2's. Returns (text, lines put back)."""
    a = ps2_text.splitlines(keepends=True)
    b = xbox_text.splitlines(keepends=True)
    out = []
    restored = 0
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if op == "replace" and i2 - i1 == j2 - j1 and all(
                timeout_value(a[i]) and timeout_value(b[j]) for i, j in zip(range(i1, i2), range(j1, j2))):
            out.extend(a[i1:i2])
            restored += i2 - i1
        else:
            out.extend(b[j1:j2])
    return "".join(out), restored


def audio_cues(text):
    """The AUDIO lines' phrase names (comments dropped)."""
    out = []
    for line in text.splitlines():
        words = line.split("//")[0].split()
        if len(words) == 2 and words[0] == "AUDIO":
            out.append(words[1].strip('"'))
    return out


def challenges(ps2, others, out):
    lines = []
    for level in CHALLENGE_LEVELS:
        ps2_file = ifind(ps2, "levels/%s/objctv.txt" % level)
        theirs = [ifind(o, "levels/%s/objctv.txt" % level) for o in others if o]
        theirs = [t for t in theirs if t]
        if not ps2_file or not theirs:
            say("no %s objctv.txt in the PS2 and the other releases' files: left out" % level)
            continue
        text = open(theirs[0], "rb").read().decode("latin1")
        for t in theirs[1:]:
            if open(t, "rb").read().decode("latin1") != text:
                say("%s: the Xbox and GameCube scripts differ; the first given is used" % level)
        ps2_text = open(ps2_file, "rb").read().decode("latin1")
        fixed, restored = ps2_timers(ps2_text, text)
        for kind, body in (("xbox", text), ("ps2-timers", fixed)):
            d = os.path.join(out, "challenges", kind, level)
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, "OBJCTV.TXT"), "wb").write(body.encode("latin1"))
        ps2_cues = audio_cues(ps2_text)
        cues = audio_cues(text)
        outcome = [c for c in cues if c.lower().endswith(("_success", "_failure"))]
        ps2_outcome = [c for c in ps2_cues if c.lower().endswith(("_success", "_failure"))]
        lines.append("challenges %s: %d success and failure lines (the PS2's %d; its AUDIO_PLACEHOLDER %d), %d TIMEOUT "
                     "put back to the PS2's" % (level, len(outcome), len(ps2_outcome), ps2_cues.count("AUDIO_PLACEHOLDER"),
                                               restored))
    return lines


# ---- the park editor's pieces -------------------------------------------------------------------------------------

def object_defs(text):
    """[(MaxObjectName, the definition's text)] of a piece list, in its order."""
    out = []
    at = 0
    while True:
        begin = text.find("\t\tObjectDefBegin", at)
        if begin < 0:
            return out
        end = text.index("ObjectDefEnd", begin) + len("ObjectDefEnd")
        end = text.index("\n", end) + 1
        body = text[begin:end]
        name = ""
        for line in body.splitlines():
            words = line.split(None, 1)
            if len(words) == 2 and words[0] == "MaxObjectName":
                name = words[1].strip().strip('"')
        out.append((name.lower(), body))
        at = end


def capitals_name(body):
    """A definition with its Name in capitals, as the PS2's list writes them."""
    out = []
    for line in body.splitlines(keepends=True):
        words = line.split(None, 1)
        if len(words) == 2 and words[0] == "Name":
            line = line.replace(words[1].strip(), words[1].strip().upper())
        out.append(line)
    return "".join(out)


def editor_pieces(ps2, others, out):
    ps2_file = ifind(ps2, "editor/object.obj")
    theirs = [ifind(o, "editor/object00.obj") or ifind(o, "editor/object.obj") for o in others if o]
    theirs = [t for t in theirs if t and os.path.getsize(t) != os.path.getsize(ps2_file or t)]
    if not ps2_file or not theirs:
        return ["editor: no other piece list"]
    text = open(ps2_file, "rb").read().decode("latin1")
    have = {name for name, _ in object_defs(text)}
    added = [(name, capitals_name(body)) for name, body in object_defs(open(theirs[0], "rb").read().decode("latin1"))
             if name and name not in have]
    end = text.rindex("ObjectListEnd")
    end = text.rindex("\t}", 0, end)
    text = text[:end] + "".join(body for _, body in added) + text[end:]
    d = os.path.join(out, "editor")
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "OBJECT.OBJ"), "wb").write(text.encode("latin1"))
    return ["editor OBJECT.OBJ: %d pieces (the PS2's %d), added after them: %s" % (
        len(have) + len(added), len(have), ", ".join(name for name, _ in added))]


# ---- movies --------------------------------------------------------------------------------------------------------

def encode_movie(ffmpeg, source, target, crop=None):
    """An Xbox movie as MPEG-2 in an MPEG-2 program stream, 512x256 at 29.97 frames a second, no sound; crop:
    ffmpeg's crop filter's w:h:x:y first."""
    os.makedirs(os.path.dirname(target), exist_ok=True)
    w, h = MOVIE_SIZE
    filters = ("crop=%s," % crop if crop else "") + "scale=%d:%d:flags=lanczos,fps=%s" % (w, h, MOVIE_RATE)
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-an",
           "-vf", filters, "-pix_fmt", "yuv420p",
           "-c:v", "mpeg2video", "-b:v", "6000k", "-maxrate", "8000k", "-bufsize", "1835008", "-g", "15", "-bf", "2",
           "-aspect", "4:3", "-f", "vob", target + ".tmp"]
    subprocess.run(cmd, check=True)
    os.replace(target + ".tmp", target)


def count_pictures(ffprobe, path):
    if not ffprobe:
        return -1
    r = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                        "stream=nb_read_frames", "-of", "default=nokey=1:noprint_wrappers=1", path],
                       capture_output=True, text=True)
    try:
        return int(r.stdout.strip())
    except ValueError:
        return -1


def movies(xbox, out, ffmpeg):
    lines = []
    if not xbox:
        return ["movies: no Xbox files: none"]
    if not ffmpeg:
        return ["movies: no ffmpeg: none"]
    ffprobe = shutil.which("ffprobe") or (os.path.join(os.path.dirname(ffmpeg), "ffprobe")
                                          if os.path.exists(os.path.join(os.path.dirname(ffmpeg), "ffprobe")) else None)
    base = os.path.join(out, "movies", "ASSETS", "MOVIES")
    jobs = [(ifind(xbox, "movies/menu/%s.xmv" % x), os.path.join(base, "MENU", ps2 + ".PSM"), None)
            for ps2, x in MENU_MOVIES]
    # (4:3, as the game shows its movies: the gallery's frame is the screen's)
    jobs.append((ifind(xbox, "movies/zaxis.xmv"), os.path.join(base, "ZAXIS.PSS"), None))
    for source, target, crop in jobs:
        rel = os.path.relpath(target, os.path.join(out, "movies"))
        if not source:
            lines.append("movies %s: no Xbox movie" % rel)
            continue
        encode_movie(ffmpeg, source, target, crop)
        lines.append("movies %s: from %s, %d pictures (the Xbox's %d), %d bytes" % (
            rel, os.path.basename(source), count_pictures(ffprobe, target), count_pictures(ffprobe, source),
            os.path.getsize(target)))
    return lines


# ---- create-a-rider ----------------------------------------------------------------------------------------------

def model_chunks(blob):
    """A part's model (FMDL > MODL): [(tag, bytes)] of its chunks (TXRC, TGEO, TGIN), or None."""
    if blob[:4] != b"FMDL" or blob[8:12] != b"MODL":
        return None
    end = 16 + struct.unpack_from("<I", blob, 12)[0]
    at, out = 16, []
    while at + 8 <= end:
        size = struct.unpack_from("<I", blob, at + 4)[0]
        out.append((blob[at:at + 4], blob[at + 8:at + 8 + size]))
        at += 8 + size
    return out


def model_blob(chunks):
    body = b"".join(tag + struct.pack("<I", len(data)) + data for tag, data in chunks)
    modl = b"MODL" + struct.pack("<I", len(body)) + body
    return b"FMDL" + struct.pack("<I", len(modl)) + modl


STRIP_START, STRIP_FLIPPED, STRIP_VERTEX = 0x8000, 0x4000, 0x3FFF
PS2_GROUP_ENTRIES = 60  # a PS2 strip group's most entries (every PS2 create part's at most)


def iff_chunks(data):
    """[(tag, bytes)] of a run of chunks (4-byte tag, 32-bit size)."""
    at, out = 0, []
    while at + 8 <= len(data):
        size = struct.unpack_from("<I", data, at + 4)[0]
        out.append((data[at:at + 4], data[at + 8:at + 8 + size]))
        at += 8 + size
    return out


def iff_blob(chunks):
    return b"".join(tag + struct.pack("<I", len(data)) + data for tag, data in chunks)


def read_strip_groups(data):
    """TGFM: [(group word, [entries])], each group's entries padded to 4 bytes."""
    groups, at = [], 0
    while at + 4 <= len(data):
        head, count = struct.unpack_from("<HH", data, at)
        groups.append((head, list(struct.unpack_from("<%dH" % count, data, at + 4))))
        at += (count * 2 + 7) & ~3
    return groups


def write_strip_groups(groups):
    out = b""
    for head, entries in groups:
        out += struct.pack("<HH%dH" % len(entries), head, len(entries), *entries)
        out += b"\0" * (-len(out) % 4)
    return out


def gc_strips_to_ps2(groups):
    """A GameCube part's strip groups (GX strips: an entry with 0x8000 starts one, the triangles' winding
    alternating along it) as the PS2's (zge/native/strips.h: an entry with 0x8000 draws nothing, one with 0x4000
    draws its triangle the other way round): each strip's entries with the flag of their triangle's turn, strips
    longer than a group cut (the next part starting again from the two entries before), packed whole into groups
    of at most PS2_GROUP_ENTRIES by their group word."""
    out = []
    for head, entries in groups:
        strips, current, turn = [], [], 0
        for entry in entries:
            if entry & STRIP_START:
                if current and not current[-1] & STRIP_START:
                    strips.append(current)
                    current = []
                turn = 0
                current.append(STRIP_START | (entry & STRIP_VERTEX))
                continue
            current.append((entry & STRIP_VERTEX) | (STRIP_FLIPPED if len(current) >= 2 and turn % 2 else 0))
            turn += 1
        if current:
            strips.append(current)
        for strip in strips:
            while len(strip) > PS2_GROUP_ENTRIES:
                strips_part = strip[:PS2_GROUP_ENTRIES]
                strip = [STRIP_START | (e & STRIP_VERTEX) for e in strip[PS2_GROUP_ENTRIES - 2:PS2_GROUP_ENTRIES]] + \
                    strip[PS2_GROUP_ENTRIES:]
                out.append((head, strips_part))
            if out and out[-1][0] == head and len(out[-1][1]) + len(strip) <= PS2_GROUP_ENTRIES:
                out[-1][1].extend(strip)
            else:
                out.append((head, list(strip)))
    return out


def strip_triangles(groups, gamecube):
    """The triangles strip groups draw, each as (a, b, c) in its winding, from its smallest vertex."""
    out = []
    for _, entries in groups:
        turn = 0
        for k, entry in enumerate(entries):
            if entry & STRIP_START:
                turn = 0
                continue
            if k < 2:
                continue
            a, b, c = (entries[k - 2] & STRIP_VERTEX, entries[k - 1] & STRIP_VERTEX, entry & STRIP_VERTEX)
            flipped = turn % 2 == 1 if gamecube else bool(entry & STRIP_FLIPPED)
            turn += 1
            if len({a, b, c}) < 3:
                continue
            t = (b, a, c) if flipped else (a, b, c)
            while t[0] > t[1] or t[0] > t[2]:
                t = (t[1], t[2], t[0])
            out.append(t)
    return sorted(out)


def part_materials(blob):
    """A create part's material table (TGEO > TMTB: its MATI records, whose render words are each release's own)."""
    chunks = model_chunks(blob)
    geo = dict(iff_chunks(chunks[1][1])) if chunks and len(chunks) > 1 else {}
    return geo.get(b"TMTB")


MATERIAL_RECORD = 168  # a MATI record of a material table (its 8-byte head and 160 bytes)
MATERIAL_LAYERS_AT, MATERIAL_LAYER = 8 + 44, 28  # its layer count, then its layers of 28 bytes


def material_layers(table):
    """[(record start, [layer offsets])] of a material table, or None when it is not MATI records."""
    out = []
    for at in range(0, len(table), MATERIAL_RECORD):
        record = table[at:at + MATERIAL_RECORD]
        if len(record) != MATERIAL_RECORD or record[:4] != b"MATI":
            return None
        count = struct.unpack_from("<I", record, MATERIAL_LAYERS_AT)[0]
        if MATERIAL_LAYERS_AT + 4 + count * MATERIAL_LAYER > MATERIAL_RECORD:
            return None
        out.append((at, [at + MATERIAL_LAYERS_AT + 4 + i * MATERIAL_LAYER for i in range(count)]))
    return out


def material_tables(ps2_parts, gc_parts):
    """GameCube material layer -> the PS2's (a layer's render words are each release's own; the rest of a material
    is the same), the PS2 layer the parts both have give most for it."""
    gc = {name.lower(): part_materials(blob) for _, _, blob, name in gc_parts}
    found = {}
    for _, _, blob, name in ps2_parts:
        mine, theirs = part_materials(blob), gc.get(name.lower())
        if mine is None or theirs is None or len(mine) != len(theirs):
            continue
        layers = material_layers(mine)
        if layers is None or layers != material_layers(theirs):
            continue
        for _, offsets in layers:
            for at in offsets:
                counts = found.setdefault(theirs[at:at + MATERIAL_LAYER], {})
                layer = mine[at:at + MATERIAL_LAYER]
                counts[layer] = counts.get(layer, 0) + 1
    return {theirs: max(counts, key=lambda layer: (counts[layer], layer)) for theirs, counts in found.items()}


def ps2_materials(table, layers_map):
    """A GameCube material table with its layers the PS2's (material_tables), or None for a layer no part shows."""
    layers = material_layers(table)
    if layers is None:
        return None
    out = bytearray(table)
    for _, offsets in layers:
        for at in offsets:
            layer = layers_map.get(bytes(table[at:at + MATERIAL_LAYER]))
            if layer is None:
                return None
            out[at:at + MATERIAL_LAYER] = layer
    return bytes(out)


def gc_part_to_ps2(blob, placeholder, materials):
    """A GameCube create part (FMDL > MODL: TXRC, TGEO, TGIN) in the PS2's: its 8x8 texture the PS2's
    placeholder, its geometry's (TGEO > TGPT) strip groups the PS2's (the TGPH's group count with them), its
    material table the PS2's of the parts with its own (materials: material_tables); the rest is the PS2's formats
    already. None when it is not such a part, or no PS2 part has its materials."""
    chunks = model_chunks(blob)
    if not chunks or [t for t, _ in chunks] != [b"TXRC", b"TGEO", b"TGIN"]:
        return None
    geo = iff_chunks(chunks[1][1])
    pieces = dict(geo)
    made_materials = ps2_materials(pieces.get(b"TMTB", b""), materials)
    if b"TGPT" not in pieces or made_materials is None:
        return None
    part = iff_chunks(pieces[b"TGPT"])
    tags = [t for t, _ in part]
    if b"TGPH" not in tags or b"TGFM" not in tags:
        return None
    groups = read_strip_groups(dict(part)[b"TGFM"])
    ps2 = gc_strips_to_ps2(groups)
    if strip_triangles(ps2, False) != strip_triangles(groups, True):
        raise ValueError("the strips changed the triangles")
    head = dict(part)[b"TGPH"]
    if struct.unpack_from("<I", head, 8)[0] != len(groups):
        return None
    part = [(t, head[:8] + struct.pack("<I", len(ps2)) + head[12:] if t == b"TGPH" else
             write_strip_groups(ps2) if t == b"TGFM" else d) for t, d in part]
    geo = [(t, iff_blob(part) if t == b"TGPT" else made_materials if t == b"TMTB" else d) for t, d in geo]
    return model_blob([(b"TXRC", placeholder), (b"TGEO", iff_blob(geo)), chunks[2]])


def part_group(texture):
    """The part a create-a-rider texture goes on, as createAModelGetFilteredListOfAssetsInLibrary groups them: "G"
    for the texture's "T", up to its second underscore (TMHat_Bunny_Bk_03 -> GMHat_Bunny), lower case."""
    words = texture.split("_")
    return ("G" + texture[1:len(words[0]) + 1 + len(words[1])]).lower() if len(words) > 1 else ""


def create_a_rider(ps2, others, out):
    """create/PARTS.ZAL and create/TEXTURE[A-D].ZAL: the PS2's libraries with the entries the other release has
    after them, so every PS2 entry keeps its place; create/added.txt names the textures added."""
    lines = []
    folder = os.path.join(out, "create")
    os.makedirs(folder, exist_ok=True)
    # Parts: the GameCube's are the PS2's formats but for the 8x8 texture each carries (a TDDS; every PS2 part's
    # is one same TBMP), their strips (GX's, gc_strips_to_ps2) and their material tables' render words, so a
    # GameCube part with those made the PS2's takes its place. The Xbox's are its own (TXPR, other geometry): none.
    ps2_parts_file = ifind(ps2, "create/parts.zal")
    ps2_parts = read_named_zdir(open(ps2_parts_file, "rb").read())
    placeholder = next((d for tag, d in model_chunks(ps2_parts[0][2]) if tag == b"TXRC"), None)
    have = {name.lower() for _, _, _, name in ps2_parts}
    added_parts = []
    other_parts = set()
    for other in others:
        path = ifind(other, "create/parts.zal") if other else None
        if not path:
            continue
        theirs = read_named_zdir(open(path, "rb").read())
        other_parts |= {name.lower() for _, _, _, name in theirs}
        materials = material_tables(ps2_parts, theirs)
        for key, flags, blob, name in theirs:
            chunks = model_chunks(blob)
            if name.lower() in have or not chunks or [t for t, _ in chunks] != [b"TXRC", b"TGEO", b"TGIN"]:
                continue
            if not texture_kinds(chunks[0][1]).get("TDDS") or placeholder is None:
                continue  # (not a GameCube part)
            made = gc_part_to_ps2(blob, placeholder, materials)
            if made is None:
                continue
            added_parts.append((key, flags, made, name))
            have.add(name.lower())
    open(os.path.join(folder, CREATE_PARTS + ".ZAL"), "wb").write(write_named_zdir(ps2_parts + added_parts))
    lines.append("create PARTS.ZAL: %d parts (the PS2's %d), added after them: %s" % (
        len(ps2_parts) + len(added_parts), len(ps2_parts), ", ".join(n for _, _, _, n in added_parts) or "none"))
    # Textures: the other release's that the PS2's lack (the first given that has the library), but for those
    # going on a part the other release has and these parts do not.
    added_names = []
    for library in CREATE_TEXTURES:
        ps2_file = ifind(ps2, "create/%s.zal" % library)
        mine = read_named_zdir(open(ps2_file, "rb").read())
        names = {name.lower() for _, _, _, name in mine}
        extra, left_out = [], []
        for other in others:
            path = ifind(other, "create/%s.zal" % library) if other else None
            if not path:
                continue
            for key, flags, blob, name in read_named_zdir(open(path, "rb").read()):
                if name.lower() in names:
                    continue
                names.add(name.lower())
                group = part_group(name)
                if group in other_parts and group not in have:
                    left_out.append(name)
                    continue
                extra.append((key, flags, blob, name))
        open(os.path.join(folder, library + ".ZAL"), "wb").write(write_named_zdir(mine + extra))
        added_names += [name for _, _, _, name in extra]
        lines.append("create %s.ZAL: %d textures (the PS2's %d)%s" % (
            library, len(mine) + len(extra), len(mine),
            ", %d left out (no part): %s" % (len(left_out), ", ".join(left_out)) if left_out else ""))
    open(os.path.join(folder, "added.txt"), "w").write("".join(n + "\n" for n in added_names))
    lines.append("create added.txt: %d textures (%d green skin _Hul, %d _Topless)" % (
        len(added_names), sum("_HUL" in n.upper() for n in added_names),
        sum("_TOPLESS" in n.upper() for n in added_names)))
    return lines


# ---- the Xbox's unused songs ------------------------------------------------------------------------------------

def asf_seconds(data):
    """A WMA's (ASF's) play time: its File Properties object's play duration (100 ns units) less its preroll (ms)."""
    at = data.find(ASF_FILE_PROPERTIES)
    if at < 0 or at + 88 > len(data):
        return 0.0
    play, _, preroll = struct.unpack_from("<QQQ", data, at + 64)
    return max(play / 1e7 - preroll / 1000.0, 0.0)


def bank_wave_names(data):
    """The wave files an Xbox music bank (ZSNDXBOX) names."""
    names, at = [], 0
    while True:
        end = data.find(b".WMA", at)
        if end < 0:
            return names
        start = end
        while start > 0 and 32 < data[start - 1] < 127:
            start -= 1
        names.append(data[start:end + 4].decode("latin1"))
        at = end + 4


def songs(xbox, out, ffmpeg):
    """music/<NN>.MP3 (or .FLAC) and music/songs.txt: the songs on the Xbox disc (audio/*.wma) its music bank
    (audio/music.zsd) does not name, in name order; songs.txt a line each: the file, the Xbox's file, the title
    (that file's name: the disc has no tags) and the play time in seconds (its ASF header's)."""
    if not xbox:
        return ["songs: no Xbox files: none"]
    if not ffmpeg:
        return ["songs: no ffmpeg: none"]
    bank = ifind(xbox, "audio/music.zsd")
    folder = ifind(xbox, "audio")
    if not bank or not folder:
        return ["songs: no Xbox music bank: none"]
    named = {n.lower() for n in bank_wave_names(open(bank, "rb").read())}
    files = sorted((n for n in os.listdir(folder) if n.lower().endswith(".wma") and n.lower() not in named), key=str.lower)
    target = os.path.join(out, "music")
    os.makedirs(target, exist_ok=True)
    encoders = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    codec = ["-c:a", "libmp3lame", "-b:a", SONG_BITRATE] if " libmp3lame " in encoders else ["-c:a", "flac"]
    extension = ".MP3" if "libmp3lame" in codec else ".FLAC"
    lines, listing = [], []
    for number, name in enumerate(files):
        song = "%02u%s" % (number + 1, extension)
        source = os.path.join(folder, name)
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-vn"] + codec +
                       [os.path.join(target, song)], check=True)
        seconds = asf_seconds(open(source, "rb").read(1 << 16))
        listing.append("%s\t%s\t%s\t%.3f\n" % (song, name, os.path.splitext(name)[0], seconds))
        lines.append("songs %s: %s, %.1f s" % (song, name, seconds))
    open(os.path.join(target, "songs.txt"), "w").write("".join(listing))
    lines.append("songs: %d (the Xbox bank names %d)" % (len(files), len(named)))
    return lines


# ---- the Competitive Edge trailers -------------------------------------------------------------------------------

def trailers(xbox, out, ffmpeg):
    """movies/ASSETS/MOVIES/COMPEDGE/<NAME>.PSS: the Xbox's movies/CompEdge/<name>.xmv (512x432 WMV2, 44.1 kHz Xbox
    ADPCM) as the PS2's Competitive Edge movies are: 512x256 MPEG-2 at 29.97 pictures a second, shown 4:3, with its
    sound as their 48 kHz stereo PCM (tools/dm2_xbox.py's PSS writer)."""
    import tempfile
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dm2_xbox import pss_mux, PSS_RATE, PSS_CHANNELS
    if not xbox:
        return ["trailers: no Xbox files: none"]
    if not ffmpeg:
        return ["trailers: no ffmpeg: none"]
    lines = []
    w, h = MOVIE_SIZE
    for name in TRAILERS:
        source = ifind(xbox, "movies/CompEdge/%s.xmv" % name)
        if not source:
            lines.append("trailers %s: no Xbox movie" % name)
            continue
        target = os.path.join(out, "movies", "ASSETS", "MOVIES", "COMPEDGE", name + ".PSS")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            m2v, raw = os.path.join(tmp, "v.m2v"), os.path.join(tmp, "a.pcm")
            subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-an",
                            "-vf", "scale=%d:%d:flags=lanczos,fps=%s" % (w, h, MOVIE_RATE), "-pix_fmt", "yuv420p",
                            "-c:v", "mpeg2video", "-b:v", TRAILER_BITRATE, "-maxrate", "8000k", "-bufsize", "1835008",
                            "-g", "15", "-bf", "2", "-aspect", "4:3", "-f", "mpeg2video", m2v], check=True)
            subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-vn",
                            "-ac", str(PSS_CHANNELS), "-ar", str(PSS_RATE), "-f", "s16le", raw], check=True)
            video = open(m2v, "rb").read()
            sound = open(raw, "rb").read()
        data = pss_mux(video, sound, 30000 / 1001)
        open(target + ".tmp", "wb").write(data)
        os.replace(target + ".tmp", target)
        lines.append("trailers %s.PSS: from %s, %.1f s of sound, %d bytes" % (
            name, os.path.basename(source), len(sound) / (PSS_RATE * 2 * PSS_CHANNELS), len(data)))
    return lines


def texture_kinds(data, at=0, end=None, found=None):
    """A TXR's texture chunks by kind ({"TBMP": n, ...}): FORMs of TXRCs, each a TXRH and a picture chunk."""
    found = {} if found is None else found
    end = len(data) if end is None else min(end, len(data))
    while at + 8 <= end:
        tag = data[at:at + 4]
        size = struct.unpack_from("<I", data, at + 4)[0]
        if tag in (b"FORM", b"TXRC"):
            texture_kinds(data, at + 8, at + 8 + size, found)
        elif tag != b"TXRH":
            kind = tag.decode("latin1")
            found[kind] = found.get(kind, 0) + 1
        at += 8 + size
    if at != end:
        found["(cut short)"] = 1
    return found


def gc_levels(gc, out):
    if not gc:
        return ["levels: no GameCube files: none"]
    lines = []
    for level in GC_LEVELS:
        source = ifind(gc, "levels/" + level)
        if not source or not os.path.isdir(source):
            lines.append("levels %s: not in the GameCube files" % level)
            continue
        names = sorted(n for n in os.listdir(source) if os.path.isfile(os.path.join(source, n)))
        txr = [n for n in names if n.upper() == level + ".TXR"]
        kinds = texture_kinds(open(os.path.join(source, txr[0]), "rb").read()) if txr else {}
        if not txr or set(kinds) != {"TBMP"} and set(kinds) != {"TBMP", "TZMP"}:
            lines.append("levels %s: textures %s, not the PS2's: left out" % (level, kinds or "none"))
            continue
        target = os.path.join(out, "levels", "ASSETS", "LEVELS", level)
        os.makedirs(target, exist_ok=True)
        total = 0
        for name in names:
            shutil.copyfile(os.path.join(source, name), os.path.join(target, name.upper()))
            total += os.path.getsize(os.path.join(source, name))
        lines.append("levels %s: %d files, %d bytes, %s" % (
            level, len(names), total, ", ".join("%d %s" % (n, k) for k, n in sorted(kinds.items()))))
    return lines


# ---- commands ------------------------------------------------------------------------------------------------------

def roots(args):
    ps2 = assets_of(args.ps2, "ASSETS", "")
    if not ps2:
        raise SystemExit("bmx_releases: no ASSETS folder in %s" % args.ps2)
    xbox = assets_of(args.xbox, "media/assets", "assets", "") if args.xbox else None
    gc = assets_of(args.gc, "assets", "") if args.gc else None
    if not xbox and not gc:
        raise SystemExit("bmx_releases: neither the Xbox nor the GameCube files")
    return ps2, xbox, gc


def cmd_convert(args):
    ps2, xbox, gc = roots(args)
    only = set(args.only.split(",")) if args.only else {"challenges", "strings", "editor", "movies", "levels", "create",
                                                         "trailers", "songs"}
    os.makedirs(args.out, exist_ok=True)
    manifest = []
    if "challenges" in only:
        shutil.rmtree(os.path.join(args.out, "challenges"), ignore_errors=True)
        manifest += challenges(ps2, [xbox, gc], args.out)
    if "strings" in only:
        shutil.rmtree(os.path.join(args.out, "strings"), ignore_errors=True)
        made, counts = string_tables(ps2, xbox, gc)
        os.makedirs(os.path.join(args.out, "strings"), exist_ok=True)
        for name, entries in made.items():
            open(os.path.join(args.out, "strings", name), "wb").write(write_table(entries))
        for name, keys, before, added in counts:
            if added:
                manifest.append("strings %s: %d texts translated of %d keys, %d more than the PS2's %d" % (
                    name, before + added, keys, added, before))
    if "editor" in only:
        shutil.rmtree(os.path.join(args.out, "editor"), ignore_errors=True)
        manifest += editor_pieces(ps2, [gc, xbox], args.out)
    if "movies" in only:
        shutil.rmtree(os.path.join(args.out, "movies", "ASSETS", "MOVIES", "MENU"), ignore_errors=True)
        if os.path.exists(os.path.join(args.out, "movies", "ASSETS", "MOVIES", "ZAXIS.PSS")):
            os.remove(os.path.join(args.out, "movies", "ASSETS", "MOVIES", "ZAXIS.PSS"))
        manifest += movies(xbox, args.out, args.ffmpeg or shutil.which("ffmpeg"))
    if "levels" in only:
        shutil.rmtree(os.path.join(args.out, "levels"), ignore_errors=True)
        manifest += gc_levels(gc, args.out)
    if "create" in only:
        shutil.rmtree(os.path.join(args.out, "create"), ignore_errors=True)
        manifest += create_a_rider(ps2, [xbox, gc], args.out)
    if "trailers" in only:
        shutil.rmtree(os.path.join(args.out, "movies", "ASSETS", "MOVIES", "COMPEDGE"), ignore_errors=True)
        manifest += trailers(xbox, args.out, args.ffmpeg or shutil.which("ffmpeg"))
    if "songs" in only:
        shutil.rmtree(os.path.join(args.out, "music"), ignore_errors=True)
        manifest += songs(xbox, args.out, args.ffmpeg or shutil.which("ffmpeg"))
    open(os.path.join(args.out, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
    for line in manifest:
        say(line)


def cmd_strings(args):
    ps2, xbox, gc = roots(args)
    _, counts = string_tables(ps2, xbox, gc)
    for name, keys, before, added in counts:
        print("%-24s keys %5d  PS2 translated %5d  added %4d  now %5d" % (name, keys, before, added, before + added))


def cmd_selftest(_args):
    e = [(1, b"Start"), (2, b"Options"), (3, b"X button"), (2, b"dup"), (4, None)]
    fr = [(1, b"Start"), (2, b"Options"), (3, b"X button"), (2, b"dup"), (4, None)]
    gc_e = [(1, b"Start"), (2, b"Options"), (3, b"A button")]
    gc_f = [(1, b"D\xe9marrer"), (2, b"Options"), (3, b"bouton A")]
    merged, added, before = merge_table(e, fr, [(gc_e, gc_f)])
    assert merged == [(1, b"D\xe9marrer"), (2, b"Options"), (3, b"X button"), (2, b"dup"), (4, None)], merged
    assert (added, before) == (1, 0)
    blob = write_table(merged)
    assert read_table(blob) == merged and struct.unpack_from("<I", blob, 0)[0] == len(blob)
    ps2 = 'A\n\tTIMEOUT\t\t\t\t\t"45"\nB\n\tAUDIO\t"x"\n'
    xb = 'A\n\tTIMEOUT\t\t\t\t\t"60"\nB\n\tAUDIO\t"y_failure"\n\tAUDIO\t"z"\n'
    text, restored = ps2_timers(ps2, xb)
    assert restored == 1 and '"45"' in text and "y_failure" in text and '"60"' not in text, text
    assert name_hash("KSTR_COM_MOVIE_INTRO") == name_hash("kstr_com_movie_intro")
    txr = lambda kind: b"FORM" + struct.pack("<I", 32) + b"TXRC" + struct.pack("<I", 24) + b"TXRH" + \
        struct.pack("<I", 4) + b"head" + kind + struct.pack("<I", 4) + b"pict"
    assert texture_kinds(txr(b"TBMP") * 2) == {"TBMP": 2}, texture_kinds(txr(b"TBMP") * 2)
    assert texture_kinds(txr(b"TBMP") + txr(b"TDDS")) == {"TBMP": 1, "TDDS": 1}
    named = [(0x1234, 6, b"one", "TMHat_Bunny_Bk_03"), (0x5678, 6, b"second", "GMHat_Bunny")]
    assert read_named_zdir(write_named_zdir(named)) == named
    assert part_group("TMHat_Bunny_Bk_03") == "gmhat_bunny" and part_group("TFSkin_00_Hul_00") == "gfskin_00"
    asf = b"x" * 10 + ASF_FILE_PROPERTIES + bytes(48) + struct.pack("<QQQ", 30 * 10 ** 7, 0, 2600)
    assert abs(asf_seconds(asf) - 27.4) < 1e-9 and bank_wave_names(b"ZSNDXBOX\0A.WMA\0B.WMA\0") == ["A.WMA", "B.WMA"]
    print("selftest: ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("convert", "strings"):
        p = sub.add_parser(name)
        p.add_argument("--ps2", required=True)
        p.add_argument("--xbox")
        p.add_argument("--gc")
        if name == "convert":
            p.add_argument("--out", required=True)
            p.add_argument("--ffmpeg")
            p.add_argument("--only")
    sub.add_parser("selftest")
    args = parser.parse_args()
    {"convert": cmd_convert, "strings": cmd_strings, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    main()
