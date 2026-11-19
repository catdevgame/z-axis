#!/usr/bin/env python3
"""The career (ProQuest) data of Dave Mirra Freestyle BMX 2's Xbox-only levels,
Venice and Greenville, from the Xbox executable, at build time only (called
by dm2_xbox.py convert, which writes <out>/CAREER.BIN; the runtime,
game_overrides/dave_mirra_xcareer.cpp, reads that file and never the disc).

    python3 -I dm2_xbox_career.py show --xbox <DM2 Xbox files>   the tables as text
    python3 -I dm2_xbox_career.py points --dir <convert's out> --level VENICE --out <file>
                                       developers: the gap faces' and goal objects' places
                                       for the runtime's PS2X_DM2_XCAREER_TEST
    python3 -I dm2_xbox_career.py selftest

Career goals and gaps are not in the level files: they are tables in the
executable (default.xbe, the US release: title 0x41430001, built 2001-11-09),
the PS2's tables (SLUS-20159) with two more levels. Per level L (the Xbox's
numbers: 0-7 as on the PS2, 8 Venice, 9 Greenville, 10 the park editor):

| table | Xbox | PS2 | per level |
|---|---|---|---|
| goals | 0x1A6090 | 0x233900 | 0x1B80: 5 tiers (rookie, amateur, pro, hardcore, expert) x 4 goals x 0x160 |
| gaps | 0x1A4330 | 0x231BD0 | 0x280: 32 x {jump tag, land tag, flags, points, name} |
| tier rewards | 0x1B7390 | 0x241500 | 0x28: 5 tiers x 2 x {u16 type, u16 value} |
| medal rewards | 0x1B7520 | 0x241640 | 0x18: 3 medals x 2 rewards |
| all-gaps reward | 0x1B76D0 | 0x241810 | 4 |
| local pros | 0x1A5C78 | 0x2335C8 | 100: 5 x {u32 rider, name, model, bike, tier, ...} + 4 |

A goal (0x160): u8 parts, u32 respect at +4, three entities at +8 (activated
when the goal is done; only Venice's canal-gate goal has any), then four
parts of 0x50: u32 type, parameters, four entities at +0x14 (activated when
the part is done). The game's goal init (PS2 FUN_0017F648, Xbox 0x41C30)
turns the names of each part's own objects into hashes once (types 1-3:
+0x10; 6-7: +8; 8-13 and 23: +4; 14-21: +4 and +8); the entity names stay
strings. Venice and Greenville use only part types the PS2 executable
handles (1, 2, 3, 8, 10, 13, 14, 17, 18, 19, 22).

Rewards: 8 opens a level, 9 its competition, 2 respect, 13 a tip, 16 a
cheat code, 3 a pro trick, 12 a sponsor... The Xbox's rows for its eight
levels differ from the PS2's where they open Venice (Airport's pro tier,
Airport's bronze) and Greenville (Commercial's pro tier and bronze, in place
of Galloon's, which Greenville's pro tier opens on the Xbox). Those rewards
(type 8 or 9 for level 8 or 9) are written as "extra rewards" of the PS2
level's row; the runtime keeps the PS2 rows as they are and adds the extras.

The competition's score scale (PS2 0x231BB0, u32 per level: the judges'
score is run score x 128 / it) has no Xbox counterpart (the Xbox judges
differently): Venice gets Airport's (200,000), Greenville Galloon's (145,000),
the PS2 levels with the same rookie target scale (Venice 100,000, Airport
80,000; Greenville and Galloon 50,000).

Texts (each language's string table, string.eus/euk/ger/fre): the keys the
game formats with the level number (goals, the local pros' places, the
level's name and instructions) for Venice's and Greenville's numbers, written
for the runtime to show in place of Woodward Camp's (whose slot the levels
run in), and the reward texts for opening them (KSTRSP_REWARD_LEVEL08/09,
KSTRSP_REWARD_CONTEST_LEV08/09; unused on the PS2).

Python 3.8+, standard library only. Never commit what this writes.
"""
import os
import struct
import sys

sys.dont_write_bytecode = True

MAGIC = b"DM2XCAR1"
VERSION = 1

TITLE_ID = 0x41430001
TIMESTAMP = 0x3BEB77EF

GOALS, GAPS, TIER_REWARDS, MEDAL_REWARDS, GAPS_REWARD, PROS, COMP_SCALE = range(1, 8)
TABLES = {  # kind: (Xbox address, size per level)
    GOALS: (0x1A6090, 0x1B80),
    GAPS: (0x1A4330, 0x280),
    TIER_REWARDS: (0x1B7390, 0x28),
    MEDAL_REWARDS: (0x1B7520, 0x18),
    GAPS_REWARD: (0x1B76D0, 4),
    PROS: (0x1A5C78, 100),
}
KIND_NAMES = {GOALS: "goals", GAPS: "gaps", TIER_REWARDS: "tier rewards", MEDAL_REWARDS: "medal rewards",
              GAPS_REWARD: "all-gaps reward", PROS: "local pros", COMP_SCALE: "competition scale"}
TIER_GOALS = (1, 4, 4, 4, 1)
TIER_KEYS = ("EASY", "HARD", "XTRA", "EXPERT")
GOAL_SIZE, PART_SIZE, TIER_SIZE = 0x160, 0x50, 0x580
# Xbox level -> (output folder, competition scale)
LEVELS = {8: ("VENICE", 200000), 9: ("GREENVIL", 145000)}
SLOT = 2  # Woodward Camp: the runtime's slot for the extra levels
LANGUAGES = ("string.eus", "string.euk", "string.ger", "string.fre")  # PS2 STRING.EUS, EUK, GER, FRE
REL_HASHED = 1  # the goal init turns this name into its hash


def say(text):
    print("[dm2xbox] " + text, flush=True)


def name_hash(name):
    """The engine's name hash (DM2 0x133F18): case-insensitive ELF hash."""
    h = 0
    for ch in name.encode("latin1"):
        c = ch - 32 if 97 <= ch <= 122 else ch
        h = ((h << 4) + c) & 0xFFFFFFFF
        high = h & 0xF0000000
        if high:
            h ^= high >> 24
        h &= ~high & 0xFFFFFFFF
    return h


class Xbe:
    """default.xbe as an address space (its sections' raw data)."""

    def __init__(self, data):
        if data[:4] != b"XBEH":
            raise ValueError("default.xbe is not an XBE")
        self.data = data
        base = struct.unpack_from("<I", data, 0x104)[0]
        count, headers = struct.unpack_from("<II", data, 0x11C)
        cert = struct.unpack_from("<I", data, 0x118)[0] - base
        self.title = struct.unpack_from("<I", data, cert + 8)[0]
        self.timestamp = struct.unpack_from("<I", data, 0x114)[0]
        self.sections = []
        for i in range(count):
            _, vaddr, _, raw, size, name = struct.unpack_from("<6I", data, headers - base + i * 0x38)
            n = data[name - base:data.index(b"\0", name - base)].decode("latin1")
            self.sections.append((n, vaddr, size, raw))

    def offset(self, va, n=1):
        for _, v, size, raw in self.sections:
            if v <= va and va + n <= v + size:
                return raw + va - v
        return None

    def bytes(self, va, n):
        at = self.offset(va, n)
        if at is None:
            raise ValueError("0x%X is not in the XBE's data" % va)
        return self.data[at:at + n]

    def u32(self, va):
        return struct.unpack("<I", self.bytes(va, 4))[0]

    def string(self, va):
        """The C string at va when va is in .rdata or .data, else None."""
        for n, v, size, raw in self.sections:
            if n in (".rdata", ".data") and v <= va < v + size:
                at = raw + va - v
                end = self.data.find(b"\0", at, at + 256)
                if end < 0:
                    return None
                s = self.data[at:end]
                if s and all(32 <= c < 127 for c in s):
                    return s.decode("latin1")
                return None
        return None


def string_table(data):
    """{key hash: text bytes} of a string table (STRING.EUS ...); offset 0 = empty."""
    count = struct.unpack_from("<I", data, 4)[0]
    out = {}
    for i in range(count):
        key, at = struct.unpack_from("<II", data, 8 + 8 * i)
        out[key] = data[at:data.index(b"\0", at)] if at else b""
    return out


def goal_string_fields(ptype):
    """Offsets in a part of the names the goal init hashes (PS2 FUN_0017F648)."""
    if ptype in (1, 2, 3):
        return (0x10,)
    if ptype in (6, 7):
        return (0x08,)
    if ptype in (8, 9, 10, 11, 12, 13, 23):
        return (0x04,)
    if 14 <= ptype <= 21:
        return (0x04, 0x08)
    return ()


class Strings:
    def __init__(self):
        self.items, self.index = [], {}

    def add(self, s):
        if s not in self.index:
            self.index[s] = len(self.items)
            self.items.append(s)
        return self.index[s]


def block_goals(xbe, level, strings, report):
    base = TABLES[GOALS][0] + level * TABLES[GOALS][1]
    data = bytearray(xbe.bytes(base, TABLES[GOALS][1]))
    rel = []

    def take(off, flags):
        va = struct.unpack_from("<I", data, off)[0]
        if va == 0:
            return
        s = xbe.string(va)
        if s is None:
            raise ValueError("goal table, level %d +0x%X: 0x%X is not a name" % (level, off, va))
        struct.pack_into("<I", data, off, 0)
        rel.append((off, strings.add(s), flags))

    for tier, goals in enumerate(TIER_GOALS):
        for g in range(4):
            at = tier * TIER_SIZE + g * GOAL_SIZE
            parts = data[at]
            if g >= goals:
                if any(data[at:at + GOAL_SIZE]):
                    raise ValueError("goal table, level %d: tier %d has a goal %d" % (level, tier, g))
                continue
            if not parts:
                continue
            report["goals"] += 1
            for k in range(3):  # the goal's entities
                take(at + 8 + 4 * k, 0)
            for p in range(parts):
                pa = at + 0x20 + p * PART_SIZE
                ptype = struct.unpack_from("<I", data, pa)[0]
                report["parts"] += 1
                report["type %d" % ptype] += 1
                hashed = goal_string_fields(ptype)
                for off in hashed:
                    take(pa + off, REL_HASHED)
                for k in range(4):  # the part's entities
                    take(pa + 0x14 + 4 * k, 0)
            # Nothing else in the goal may look like a name: integers only.
            for off in range(0, GOAL_SIZE, 4):
                va = struct.unpack_from("<I", data, at + off)[0]
                if va and xbe.string(va) is not None:
                    raise ValueError("goal table, level %d tier %d goal %d +0x%X: a name where none is expected"
                                     % (level, tier, g, off))
    return bytes(data), rel


def block_strings(xbe, kind, level, fields, strings):
    """A table row with string pointers at the offsets `fields` gives."""
    addr, size = TABLES[kind]
    data = bytearray(xbe.bytes(addr + level * size, size))
    rel = []
    for off in fields(data):
        va = struct.unpack_from("<I", data, off)[0]
        if va == 0:
            continue
        s = xbe.string(va)
        if s is None:
            raise ValueError("%s, level %d +0x%X: 0x%X is not a name" % (KIND_NAMES[kind], level, off, va))
        struct.pack_into("<I", data, off, 0)
        rel.append((off, strings.add(s), 0))
    return bytes(data), rel


def gap_fields(data):
    out = []
    for i in range(32):
        at = i * 20
        if struct.unpack_from("<I", data, at)[0] == 0:
            break
        out += [at, at + 4, at + 16]
    return out


def pro_fields(data):
    return [i * 0x14 + off for i in range(5) for off in (4, 8, 12)]


def gap_count(data):
    n = 0
    while n < 32 and struct.unpack_from("<I", data, n * 20)[0]:
        n += 1
    return n


def texts(xbox, levels):
    """[(language, Xbox level or -1, key, text)]: the slot keys (Woodward's,
    the level number 02) for each level, and the fixed reward keys."""
    out = []
    for lang, name in enumerate(LANGUAGES):
        data = xbox.read(name)
        if data is None:
            raise ValueError("the Xbox files have no %s" % name)
        table = string_table(data)
        for level in levels:
            keys = ["KSTR_LEVELNAME_%02d", "KSTRSP_INST_LEV%02d", "KSTRSP_INST_COMP_LEV%02d",
                    "KSTRSP_INST_SESSION_LEV%02d", "KSTRSP_GOAL_LEV%02d_ROOKIE"]
            keys += ["KSTRSP_GOAL_LEV%%02d_%s_%d" % (t, i) for t in TIER_KEYS for i in range(4)]
            keys += ["KSTRSP_NPC_LOCATION_LEV%%02d_%d" % i for i in range(4)]
            for key in keys:
                text = table.get(name_hash(key % level), b"")
                out.append((lang, level, key % SLOT, text))
            for key in ("KSTRSP_REWARD_LEVEL%02d", "KSTRSP_REWARD_CONTEST_LEV%02d"):
                text = table.get(name_hash(key % level))
                if text:
                    out.append((lang, -1, key % level, text))
    return out


def extra_rewards(xbe, levels):
    """Rewards of the eight PS2 levels' rows that open an Xbox level or its
    competition: [(table kind, PS2 level, row, type, value)]."""
    out = []
    for kind, rows in ((TIER_REWARDS, 5), (MEDAL_REWARDS, 3)):
        addr, size = TABLES[kind]
        for level in range(8):
            for row in range(rows):
                for slot in range(2):
                    t, v = struct.unpack("<HH", xbe.bytes(addr + level * size + row * 8 + slot * 4, 4))
                    if t in (8, 9) and v in levels:
                        out.append((kind, level, row, t, v))
    return out


def build(xbox):
    """CAREER.BIN's bytes and a report, from the Xbox files (XboxFiles)."""
    from collections import Counter
    raw = None
    for rel in ("../../default.xbe", "../default.xbe", "default.xbe"):
        at = os.path.normpath(os.path.join(xbox.root, rel))
        if os.path.isfile(at):
            with open(at, "rb") as f:
                raw = f.read()
            break
    if raw is None:
        raise ValueError("the Xbox files have no default.xbe (tools/xbox_files.py extract keeps it)")
    xbe = Xbe(raw)
    if (xbe.title, xbe.timestamp) != (TITLE_ID, TIMESTAMP):
        raise ValueError("default.xbe is title 0x%08X built 0x%08X, not the US DM2 release (0x%08X, 0x%08X)"
                         % (xbe.title, xbe.timestamp, TITLE_ID, TIMESTAMP))
    # Anchors: the tables are where this release has them.
    checks = ((TABLES[GAPS][0] + 16, "pretty much free gap"), (TABLES[GAPS][0], "gap_01"),
              (TABLES[GOALS][0] + 2 * 0x1B80 + TIER_SIZE + 0x24, "mover_ladder01"),
              (TABLES[PROS][0] + 4, "Leigh Ramsdell"))
    for va, want in checks:
        if xbe.string(xbe.u32(va)) != want:
            raise ValueError("default.xbe: no %r at 0x%X" % (want, va))
    if xbe.bytes(TABLES[TIER_REWARDS][0], 8) != struct.pack("<4H", 13, 2, 0, 0) or \
            xbe.bytes(TABLES[GAPS_REWARD][0], 4) != struct.pack("<HH", 2, 1000):
        raise ValueError("default.xbe: the reward tables are not where this release has them")
    strings = Strings()
    report = Counter()
    levels = []
    for level, (folder, scale) in sorted(LEVELS.items()):
        blocks = []
        data, rel = block_goals(xbe, level, strings, report)
        blocks.append((GOALS, data, rel))
        report["gaps " + folder] = gap_count(xbe.bytes(TABLES[GAPS][0] + level * TABLES[GAPS][1], TABLES[GAPS][1]))
        data, rel = block_strings(xbe, GAPS, level, gap_fields, strings)
        blocks.append((GAPS, data, rel))
        for kind in (TIER_REWARDS, MEDAL_REWARDS, GAPS_REWARD):
            addr, size = TABLES[kind]
            blocks.append((kind, xbe.bytes(addr + level * size, size), []))
        data, rel = block_strings(xbe, PROS, level, pro_fields, strings)
        blocks.append((PROS, data, rel))
        blocks.append((COMP_SCALE, struct.pack("<I", scale), []))
        levels.append((folder, level, blocks))
    text = texts(xbox, LEVELS)
    extras = extra_rewards(xbe, LEVELS)
    out = bytearray(MAGIC)
    out += struct.pack("<II", VERSION, len(levels))
    for folder, level, blocks in levels:
        out += folder.encode("latin1").ljust(16, b"\0") + struct.pack("<II", level, len(blocks))
        for kind, data, rel in blocks:
            out += struct.pack("<III", kind, len(data), len(rel)) + data
            for off, s, flags in rel:
                out += struct.pack("<III", off, s, flags)
    out += struct.pack("<I", len(strings.items))
    for s in strings.items:
        b = s.encode("latin1")
        out += struct.pack("<I", len(b)) + b
    out += struct.pack("<I", len(text))
    for lang, level, key, t in text:
        k = key.encode("latin1")
        out += struct.pack("<iiI", lang, level, len(k)) + k + struct.pack("<I", len(t)) + t
    out += struct.pack("<I", len(extras))
    for kind, level, row, t, v in extras:
        out += struct.pack("<IIIHH", kind, level, row, t, v)
    report["strings"] = len(strings.items)
    report["texts"] = len(text)
    report["extra rewards"] = len(extras)
    return bytes(out), report, extras


def read(data):
    """CAREER.BIN back: (levels, strings, texts, extras); for the self-test and show."""
    if data[:8] != MAGIC:
        raise ValueError("not a CAREER.BIN")
    version, n = struct.unpack_from("<II", data, 8)
    at = 16
    levels = []
    for _ in range(n):
        folder = data[at:at + 16].rstrip(b"\0").decode("latin1")
        level, nb = struct.unpack_from("<II", data, at + 16)
        at += 24
        blocks = []
        for _ in range(nb):
            kind, size, nrel = struct.unpack_from("<III", data, at)
            at += 12
            body = data[at:at + size]
            at += size
            rel = [struct.unpack_from("<III", data, at + 12 * i) for i in range(nrel)]
            at += 12 * nrel
            blocks.append((kind, body, rel))
        levels.append((folder, level, blocks))
    ns = struct.unpack_from("<I", data, at)[0]
    at += 4
    strings = []
    for _ in range(ns):
        ln = struct.unpack_from("<I", data, at)[0]
        strings.append(data[at + 4:at + 4 + ln].decode("latin1"))
        at += 4 + ln
    nt = struct.unpack_from("<I", data, at)[0]
    at += 4
    text = []
    for _ in range(nt):
        lang, level, kl = struct.unpack_from("<iiI", data, at)
        key = data[at + 12:at + 12 + kl].decode("latin1")
        at += 12 + kl
        tl = struct.unpack_from("<I", data, at)[0]
        text.append((lang, level, key, data[at + 4:at + 4 + tl]))
        at += 4 + tl
    ne = struct.unpack_from("<I", data, at)[0]
    at += 4
    extras = [struct.unpack_from("<IIIHH", data, at + 16 * i) for i in range(ne)]
    at += 16 * ne
    if at != len(data):
        raise ValueError("CAREER.BIN: %d bytes left over" % (len(data) - at))
    return levels, strings, text, extras


def describe(data):
    levels, strings, text, extras = read(data)
    lines = []
    for folder, level, blocks in levels:
        lines.append("%s (Xbox level %d)" % (folder, level))
        for kind, body, rel in blocks:
            names = {off: strings[s] for off, s, _ in rel}
            if kind == GOALS:
                for tier, goals in enumerate(TIER_GOALS):
                    for g in range(goals):
                        at = tier * TIER_SIZE + g * GOAL_SIZE
                        parts, respect = body[at], struct.unpack_from("<I", body, at + 4)[0]
                        if not parts:
                            continue
                        ents = [names[o] for o in (at + 8, at + 12, at + 16) if o in names]
                        lines.append("  goal tier %d #%d: %d parts, %d respect%s" % (
                            tier, g, parts, respect, ("; when done: " + ", ".join(ents)) if ents else ""))
                        for p in range(parts):
                            pa = at + 0x20 + p * PART_SIZE
                            words = struct.unpack_from("<5I", body, pa)
                            show = [names.get(pa + 4 * i, words[i]) for i in range(5)]
                            ents = [names[pa + 0x14 + 4 * k] for k in range(4) if pa + 0x14 + 4 * k in names]
                            lines.append("    part type %d: %s%s" % (show[0], " ".join(str(v) for v in show[1:]),
                                                                    ("; then " + ", ".join(ents)) if ents else ""))
            elif kind == GAPS:
                for i in range(sum(1 for k in range(32) if k * 20 in names)):
                    at = i * 20
                    flags, points = struct.unpack_from("<II", body, at + 8)
                    lines.append("  gap %2d: %s -> %s, %d points, flags %d: %s" % (
                        i, names[at], names[at + 4], points, flags, names[at + 16]))
            elif kind in (TIER_REWARDS, MEDAL_REWARDS, GAPS_REWARD):
                pairs = [struct.unpack_from("<HH", body, 4 * i) for i in range(len(body) // 4)]
                lines.append("  %s: %s" % (KIND_NAMES[kind], " ".join("%d:%d" % p for p in pairs)))
            elif kind == PROS:
                for i in range(5):
                    at = i * 0x14
                    lines.append("  pro %d: rider %d %s (%s, %s), %s" % (
                        i, body[at], names.get(at + 4), names.get(at + 8), names.get(at + 12),
                        list(body[at + 16:at + 20])))
            elif kind == COMP_SCALE:
                lines.append("  competition scale %d" % struct.unpack("<I", body)[0])
    for kind, level, row, t, v in extras:
        lines.append("extra: %s row of PS2 level %d, %s %d: reward %d:%d" % (
            KIND_NAMES[kind], level, "tier" if kind == TIER_REWARDS else "medal", row, t, v))
    lines.append("%d texts (%d languages)" % (len(text), len({t[0] for t in text})))
    return "\n".join(lines)


def cmd_show(args):
    import dm2_xbox
    data, report, _ = build(dm2_xbox.XboxFiles(args.xbox))
    print(describe(data))
    print(", ".join("%s %d" % kv for kv in sorted(report.items())))


def level_points(geo, ins, tags, names):
    """World-space places for the runtime's career checks (developers): per
    face tag of `tags` the face of that tag nearest its faces' middle
    (centroid, normal); per instance name of `names` its origin and the
    middle of its highest upward face (+y is down)."""
    import dm2_level
    from dm2_level import parse, walk
    want = {name_hash(t): t for t in tags}
    objects = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        head = next(k for k in obj.kids if k.tag == b"TGHD")
        faces_out = []
        for part in (k for k in obj.kids if k.tag == b"TGPT"):
            st = {k.tag: k.data for k in part.kids}
            nv, nf = struct.unpack_from("<HH", st[b"TGPH"], 4)
            verts = [struct.unpack_from("<3f", st[b"TGVP"], 12 * i) for i in range(nv)]
            faces = [struct.unpack_from("<4H", st[b"TGFP"], 8 * i) for i in range(nf)]
            normals = [struct.unpack_from("<3f", st[b"TGFN"], 12 * i) for i in range(nf)] if b"TGFN" in st else []
            tags_of = [None] * nf
            if b"TGFT" in st:
                _, count, idx, recs = dm2_level.tag_buffer(st[b"TGFT"])
                for f, r in enumerate(struct.unpack_from("<%dH" % count, idx)):
                    if r != 0xFFFF and f < nf:
                        tags_of[f] = struct.unpack_from("<I", recs[r], 0)[0]
            for f, face in enumerate(faces):
                if max(face[1:]) >= nv:
                    continue
                c = tuple(sum(verts[i][k] for i in face[1:]) / 3 for k in range(3))
                faces_out.append((c, normals[f] if f < len(normals) else (0.0, -1.0, 0.0), tags_of[f]))
            if b"TGFE" in st:  # grind edges with a gap's tag (rail-to-rail gaps): the edge's middle
                _, count, idx, recs = dm2_level.tag_buffer(st[b"TGFE"])
                for e, r in enumerate(struct.unpack_from("<%dH" % count, idx)):
                    if r == 0xFFFF or e // 3 >= nf:
                        continue
                    t = struct.unpack_from("<I", recs[r], 0)[0]
                    face = faces[e // 3]
                    i0, i1 = face[1 + e % 3], face[1 + (e + 1) % 3]
                    if t in want and max(i0, i1) < nv:
                        c = tuple((verts[i0][k] + verts[i1][k]) / 2 for k in range(3))
                        faces_out.append((c, (0.0, -1.0, 0.0), ("edge", t)))
            break  # the first (detailed) part: the converter keeps one
        objects[struct.unpack_from("<I", head.data, 0x18)[0]] = faces_out
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}

    def place(d, v, rotate_only=False):
        for _ in range(16):
            v = dm2_level.quat_rotate(struct.unpack_from("<4f", d, 8), v)
            if not rotate_only:
                v = tuple(a + b for a, b in zip(v, struct.unpack_from("<3f", d, 0x18)))
            parent = struct.unpack_from("<I", d, 0x44)[0]
            d = by_id.get(parent) if parent else None
            if d is None:
                break
        return v
    tagged = {}
    lines = []
    for d in instances:
        name = d[0x4C:].split(b"\0")[0].decode("latin1")
        faces = objects.get(struct.unpack_from("<I", d, 0x40)[0], [])
        for c, n, t in faces:
            edge = isinstance(t, tuple)
            if (t[1] if edge else t) in want:
                tagged.setdefault(want[t[1] if edge else t], []).append((place(d, c), place(d, n, True), edge))
        if name in names:
            origin = place(d, (0.0, 0.0, 0.0))
            up = [(place(d, c), place(d, n, True)) for c, n, t in faces if not isinstance(t, tuple)]
            up = [u for u in up if u[1][1] < -0.5]
            top = min(up, key=lambda u: u[0][1])[0] if up else origin
            lines.append("object %s %.3f %.3f %.3f %.3f %.3f %.3f\n" % ((name,) + origin + top))
    for tag in tags:
        fs = tagged.get(tag)
        if not fs:
            lines.append("face %s none\n" % tag)
            continue
        edge = all(f[2] for f in fs)
        fs = [f for f in fs if f[2] == edge]
        mid = tuple(sum(f[0][k] for f in fs) / len(fs) for k in range(3))
        c, n, _ = min(fs, key=lambda f: sum((f[0][k] - mid[k]) ** 2 for k in range(3)))
        lines.append("%s %s %.3f %.3f %.3f %.3f %.3f %.3f %d\n" % (("edge" if edge else "face", tag) + c + n + (len(fs),)))
    return lines


def cmd_points(args):
    """For the runtime's PS2X_DM2_XCAREER_TEST (developers): the places of
    a converted level's gap faces and goal objects."""
    with open(os.path.join(args.dir, "CAREER.BIN"), "rb") as f:
        levels, strings, _, _ = read(f.read())
    folder = args.level.upper()
    level = next((l for l in levels if l[0] == folder), None)
    if level is None:
        raise ValueError("CAREER.BIN has no %s" % folder)
    tags, names = [], set()
    for kind, body, rel in level[2]:
        if kind == GAPS:
            tags += [strings[s] for off, s, _ in rel if off % 20 in (0, 4)]
        elif kind == GOALS:
            names.update(strings[s] for off, s, _ in rel)
    base = os.path.join(args.dir, folder, folder)
    with open(base + ".GEO", "rb") as f:
        geo = f.read()
    with open(base + ".INS", "rb") as f:
        ins = f.read()
    # Goal names are face tags (obj_*: a part's faces), edge tags (rails)
    # or instances (movers, dynamics): asked for as both.
    lines = level_points(geo, ins, tags + sorted(n for n in names if n not in tags), names)
    lines = [l for l in lines if not (l.endswith(" none\n") and l.split()[1] in names)]
    with open(args.out, "w", newline="\n") as f:
        f.writelines(lines)
    say("%s: %d gap faces (%d not found), %d goal objects" % (
        folder, len(tags), sum(1 for l in lines if l.endswith("none\n")), sum(1 for l in lines if l.startswith("object"))))


def cmd_selftest(args):
    """The format and the goal fields on a hand-made XBE (no disc needed)."""
    assert name_hash("gap_01") == name_hash("GAP_01")
    assert goal_string_fields(13) == (4,) and goal_string_fields(19) == (4, 8) and goal_string_fields(22) == ()

    class FakeXbe(Xbe):
        def __init__(self):
            self.mem = {}

        def string(self, va):
            return self.mem.get(va)

        def bytes(self, va, n):
            return self.blob[va - self.base:va - self.base + n]

    x = FakeXbe()
    x.base = TABLES[GOALS][0] + 8 * 0x1B80
    blob = bytearray(0x1B80)
    x.mem = {0x200000: "obj_a", 0x200010: "mover_b", 0x200020: "mover_w"}
    blob[TIER_SIZE] = 1  # amateur goal 0: one part
    struct.pack_into("<II", blob, TIER_SIZE, 1, 100)
    struct.pack_into("<I", blob, TIER_SIZE + 8, 0x200020)
    struct.pack_into("<II", blob, TIER_SIZE + 0x20, 13, 0x200000)
    struct.pack_into("<I", blob, TIER_SIZE + 0x34, 0x200010)
    x.blob = blob
    from collections import Counter
    strings, report = Strings(), Counter()
    data, rel = block_goals(x, 8, strings, report)
    assert report["goals"] == 1 and report["type 13"] == 1
    assert sorted((o, strings.items[s], f) for o, s, f in rel) == [
        (TIER_SIZE + 8, "mover_w", 0), (TIER_SIZE + 0x24, "obj_a", REL_HASHED), (TIER_SIZE + 0x34, "mover_b", 0)]
    assert struct.unpack_from("<I", data, TIER_SIZE + 0x24)[0] == 0
    struct.pack_into("<I", blob, TIER_SIZE + 0x50, 0x200000)  # a name where an integer belongs
    try:
        block_goals(x, 8, Strings(), Counter())
        raise AssertionError("a stray name was taken")
    except ValueError:
        pass
    print("dm2_xbox_career.py selftest: ok")


def main(argv):
    import argparse
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("--xbox", required=True)
    s.set_defaults(fn=cmd_show)
    q = sub.add_parser("points")
    q.add_argument("--dir", required=True, help="dm2_xbox.py convert's output")
    q.add_argument("--level", required=True)
    q.add_argument("--out", required=True)
    q.set_defaults(fn=cmd_points)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("dm2_xbox_career.py: %s" % e)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main(sys.argv[1:])
