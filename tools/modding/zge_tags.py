#!/usr/bin/env python3
"""The bike games' face and edge tag records: Dave Mirra BMX 2's (ZGE2) and
BMX XXX's (ZGE4) TGFE and TGFT records, the same layout in both. They are
smaller than Aggressive Inline's (ZGE3: 24 and 28 bytes, ai_level_write.EdgeTag
and FaceTag). These records go in ai_level_write.TagBuffer, which writes the
descriptor, the indices and the records around them.

The riders read them a byte or a word at a time (the engine's tagByte and
tagWord): games/dm2/rider/collision/tags.cpp, games/bmx/rider/collision/tags.cpp
and links.cpp. Every field below is read the same way in both games. Where a
meaning holds for one game only, its docstring says so.

    python3 -I zge_tags.py selftest

Python 3.8+, standard library only.
"""
import struct
import sys
from dataclasses import dataclass


@dataclass
class EdgeTag8:
    """A TGFE record (8 bytes): a face edge's tags. `tag` is the word the goals and collectors match (an
    edge's name hash in its object). `link_kind` is the surface kind of the faces the edge is linked to
    (1 a quarter pipe's lip, 2 a spine's; 0 none). The rider's link pass (links.cpp) ties the edge to the
    neighbouring faces of that kind. In `flags`, bit 0 means the edge can be ground. `grind_side` says
    which side the edge is ground from (0 .. 3; 3, on every BMX XXX record and nearly every DM2 one,
    decides it by the face's normal: edgeGrindSide). `wall_kind` is the wall the edge makes
    (0 .. 15, tagWallKind)."""
    tag: int
    link_kind: int = 0
    flags: int = 0
    grind_side: int = 3
    wall_kind: int = 0
    SIZE = 8
    GRINDABLE = 1
    SIDE_BY_NORMAL = 3

    @staticmethod
    def read(data, at):
        tag, link, flags, side, wall = struct.unpack_from("<I4B", data, at)
        return EdgeTag8(tag, link, flags, side, wall)

    def write(self):
        return struct.pack("<I4B", self.tag, self.link_kind, self.flags, self.grind_side, self.wall_kind)

    @property
    def grindable(self):
        return bool(self.flags & self.GRINDABLE)


@dataclass
class FaceTag12:
    """A TGFT record (12 bytes): a face's tags. `tag` is the word the goals match. `link` is a linked
    side: the face in its low 24 bits and the side in the top 8, with ~0 meaning none. It is 0, 1, 2, 7
    or 8 on DM2's disc and 0 in every BMX XXX record, and the rider's link pass writes it (setTagWord).
    `surface_kind` is 0 .. 12 in BMX XXX (1 a quarter pipe, 2 a spine; the riders swap 3 and 4 and map
    the kind through their table, tagSurfaceKind) and 0 .. 11 on DM2's disc. `launch` is byte 9
    (tagByte9), 50 on most faces: on a ramp contact (the rider's contact kind 4) the take-off's top
    speed as a percent of the top speed, and on a vert lip the index of its share in the vert table
    (takeoff.cpp). `leftover` is bytes 10 and 11, which neither game reads. They are not zero on disc
    (0, 255, 12, ...; 5,358 of BMX XXX's 66,872 records), so the record keeps them for byte-identical
    round trips. A face of our own writes 0."""
    tag: int
    surface_kind: int = 0
    launch: int = 50
    link: int = 0
    leftover: bytes = b"\0\0"
    SIZE = 12
    NO_LINK = 0xFFFFFFFF

    @staticmethod
    def read(data, at):
        tag, link, kind, launch = struct.unpack_from("<2I2B", data, at)
        return FaceTag12(tag, kind, launch, link, bytes(data[at + 10:at + 12]))

    def write(self):
        return struct.pack("<2I2B", self.tag, self.link, self.surface_kind, self.launch) + self.leftover.ljust(2, b"\0")[:2]


@dataclass
class CornerTag4:
    """A TGFV record (4 bytes; BMX XXX only, ZGE4's readTgfvChunk: the corner tags). There is one index
    per face corner (3 x face + corner) and the indices pick these records. Only the fast cars have them
    (fastCarDef_*, 14 parts in the eight levels): they are the corners a skitching rider holds on by.
    `kind` is byte 0, which cornerTagKind reads: 1 holds on the hold point's side, 2 holds across it with
    the bike mirrored (cornerKind's table: kCornerHold, kCornerHoldMirrored). `leftover` is bytes 1 to 3,
    kept as they are; the corner code reads byte 0 alone."""
    kind: int
    leftover: bytes = b"\0\0\0"
    SIZE = 4
    HOLD = 1
    HOLD_MIRRORED = 2

    @staticmethod
    def read(data, at):
        return CornerTag4(data[at], bytes(data[at + 1:at + 4]))

    def write(self):
        return bytes([self.kind]) + self.leftover.ljust(3, b"\0")[:3]


def cmd_selftest():
    e = EdgeTag8(0x12345678, 1, EdgeTag8.GRINDABLE, 3, 2)
    assert e.write() == bytes.fromhex("78563412") + bytes([1, 1, 3, 2]) and EdgeTag8.read(e.write(), 0) == e and e.grindable
    f = FaceTag12(7, 11, 50, 0, b"\xff\x0c")
    assert len(f.write()) == 12 and FaceTag12.read(f.write(), 0) == f and f.write()[8:10] == bytes([11, 50])
    c = CornerTag4(2, b"\x01\x02\x03")
    assert CornerTag4.read(c.write(), 0) == c and len(c.write()) == 4
    print("zge_tags selftest: ok")


if __name__ == "__main__":
    if sys.argv[1:] != ["selftest"]:
        raise SystemExit(__doc__)
    cmd_selftest()
