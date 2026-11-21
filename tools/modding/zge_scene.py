#!/usr/bin/env python3
"""A level's scene file (.ZIT) read and written: the same form in all three games.

    FSCN { TERR { TEHD, ZFTX, ZFGE, ZFIS, ZFSD } }

TEHD (0x30 bytes) is the collision world: the cell size, the world box's low and high corners, then the
grid counts per axis ((high - low) / cell, as the exporter wrote them; readTehdChunk reads the first 0x24
bytes and makes the collision world from them, newCollisionWorld). ZFTX, ZFGE, ZFIS and ZFSD are the
names of the level's TXR, GEO, INS and DAT files, read from the ZIT's own folder (includeChunkFile), each
ended with a zero and padded to 4 bytes with zeros. The names keep the case they were given: AI's
converter writes "tut.TXR", BMX XXX's stock files "CHL_MULY.TXR".

    python3 -I zge_scene.py roundtrip FILE...    each read, written and compared
    python3 -I zge_scene.py selftest

Python 3.8+, standard library only.
"""
import os
import struct
import sys
from dataclasses import dataclass
from typing import Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402

Chunk = L.Chunk
TEHD_BYTES = 0x30
CELL = 50.0     # every stock level's cell, in all three games' discs read so far


def _padded(text):
    data = text.encode("latin1") + b"\0"
    return data + b"\0" * (-len(data) % 4)


@dataclass
class SceneFile:
    """A ZIT: the collision world's cell size, box and grid counts (TEHD) and the level's file names
    ({b"ZFTX": "CHL_MULY.TXR", b"ZFGE": ..., b"ZFIS": ..., b"ZFSD": ...})."""
    cell: Tuple[float, float, float]
    low: Tuple[float, float, float]
    high: Tuple[float, float, float]
    counts: Tuple[int, int, int]
    names: dict
    NAMES = ((b"ZFTX", "TXR"), (b"ZFGE", "GEO"), (b"ZFIS", "INS"), (b"ZFSD", "DAT"))

    @classmethod
    def read(cls, data):
        tree = L.parse(data)
        if len(tree) != 1 or tree[0].tag != b"FSCN" or len(tree[0].kids) != 1 or tree[0].kids[0].tag != b"TERR":
            raise ValueError("a ZIT is FSCN { TERR }")
        kids = tree[0].kids[0].kids
        if [k.tag for k in kids] != [b"TEHD"] + [t for t, _ in cls.NAMES]:
            raise ValueError("a TERR of %r" % [k.tag for k in kids])
        if len(kids[0].data) != TEHD_BYTES:
            raise ValueError("a TEHD of %d bytes" % len(kids[0].data))
        v = struct.unpack("<9f3I", kids[0].data)
        names = {}
        for k in kids[1:]:
            text = k.data.split(b"\0", 1)[0].decode("latin1")
            if k.data != _padded(text):
                raise ValueError("a %r name not zero-padded to 4" % k.tag)
            names[k.tag] = text
        return cls(v[0:3], v[3:6], v[6:9], v[9:12], names)

    def write(self):
        kids = [Chunk(b"TEHD", struct.pack("<9f3I", *self.cell, *self.low, *self.high, *self.counts))]
        kids += [Chunk(tag, _padded(self.names[tag])) for tag, _ in self.NAMES]
        return L.serialize([Chunk(b"FSCN", kids=[Chunk(b"TERR", kids=kids)])])

    @classmethod
    def make(cls, level, low, high, cell=CELL):
        """The ZIT of level files named <level>.TXR, .GEO, .INS and .DAT (level as given: its case is kept)
        around the box (low, high), the counts rounded as the exporter's, at least 1."""
        counts = tuple(max(1, int(round((high[k] - low[k]) / cell))) for k in range(3))
        return cls((float(cell),) * 3, tuple(low), tuple(high), counts, {t: "%s.%s" % (level, ext) for t, ext in cls.NAMES})


def cmd_roundtrip(paths):
    bad = 0
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        try:
            out = SceneFile.read(data).write()
            same = out == data
        except ValueError as e:
            same, out = False, b""
            print("%s: %s" % (p, e))
        bad += not same
        print("%s: %d bytes, %s" % (p, len(data), "byte-identical" if same else "differs"))
    return 1 if bad else 0


def cmd_selftest():
    z = SceneFile.make("TEST", (-50.0, -50.0, -50.0), (50.0, 50.0, 50.0))
    data = z.write()
    assert SceneFile.read(data) == z and z.counts == (2, 2, 2) and z.names[b"ZFGE"] == "TEST.GEO"
    assert len(data) == 8 + 8 + 8 + TEHD_BYTES + 4 * (8 + 12)
    assert SceneFile.make("tut", (0, 0, 0), (1, 1, 1)).names[b"ZFTX"] == "tut.TXR"
    print("zge_scene selftest: ok")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["roundtrip"] and a[1:]:
        sys.exit(cmd_roundtrip(a[1:]))
    if a == ["selftest"]:
        sys.exit(cmd_selftest())
    raise SystemExit(__doc__)
