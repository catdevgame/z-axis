#!/usr/bin/env python3
"""The engine's asset libraries (.ZAL) read and written: a level's ANIMS, CUTCOMP, CUTSCENE and MODELS.ZAL
in BMX XXX, AI's MOTIONS.ZAL, the games' UILIB, FX, STRING and MOTGAME. It is the same form in all three
games (zge/assets/asset_library.h, asset_library.cpp).

A library is a ZDIR form, its directory: DIDF (the entry count and a word nothing reads), GRID (an id per
entry), DLEN (each entry's length, with its alignment in the top four bits: log2 of it plus 1, 0 also meaning
16 bytes; the stock libraries write 5 for 16), OFST (each entry's offset in the file), and in a compressed library CLEN (the stored lengths)
and NAME (the names). The entries follow the directory, each at its alignment. A library written here has
zeros between them. The stock libraries sometimes have more: up to 16 bytes past the alignment, some of
them not zero (the exporter's buffer, read by nothing: the game reads each entry's own length). Those bytes
are kept with the entry before them (LibraryEntry.slack) and the directory (AssetLibrary.lead), so the
stock files round trip byte for byte.

    python3 -I zge_library.py roundtrip FILE...    each read, written and compared
    python3 -I zge_library.py selftest

Python 3.8+, standard library only.
"""
import struct
import sys
from dataclasses import dataclass, field
from typing import List, Optional

DEFAULT_ALIGNMENT = 16
LENGTH_BITS = 0x0FFFFFFF


@dataclass
class LibraryEntry:
    """One asset: its id (GRID; the name's hash, which findAsset looks up), its bytes, and its alignment in
    the file."""
    id: int
    data: bytes
    alignment: int = DEFAULT_ALIGNMENT
    slack: Optional[bytes] = None   # the bytes after it up to the next entry (None: zeros to the next alignment)


@dataclass
class AssetLibrary:
    """A .ZAL file. `reserved` is DIDF's second word (0 in every stock library). `order` keeps the
    directory chunks' order as the file has it (DIDF, GRID, DLEN, OFST in every stock library). A
    library with CLEN or NAME is kept as `extra` chunks: no BMX XXX or AI level library has them."""
    entries: List[LibraryEntry] = field(default_factory=list)
    reserved: int = 0
    lead: Optional[bytes] = None    # the bytes between the directory and the first entry (None: zeros to its alignment)
    order: List[bytes] = field(default_factory=lambda: [b"DIDF", b"GRID", b"DLEN", b"OFST"])
    extra: dict = field(default_factory=dict)
    DIRECTORY = (b"DIDF", b"GRID", b"DLEN", b"OFST")

    @staticmethod
    def pack_length(length, alignment):
        if length > LENGTH_BITS:
            raise ValueError("an asset of %d bytes: past the 28-bit length" % length)
        code = alignment.bit_length()   # log2 + 1, as every stock library writes it (16: 5, not 0)
        if alignment != 1 << (code - 1) or code > 15:
            raise ValueError("an alignment of %d" % alignment)
        return length | code << 28

    @staticmethod
    def unpack_alignment(packed):
        code = packed >> 28
        return DEFAULT_ALIGNMENT if code == 0 else 1 << (code - 1)

    @classmethod
    def read(cls, data):
        if data[:4] != b"ZDIR":
            raise ValueError("not a ZDIR library")
        size = struct.unpack_from("<I", data, 4)[0]
        chunks, order, at = {}, [], 8
        while at < 8 + size:
            tag, n = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
            chunks[tag] = data[at + 8:at + 8 + n]
            order.append(tag)
            at += 8 + n
        if at != 8 + size:
            raise ValueError("a ZDIR directory of %d bytes runs past its size" % (at - 8))
        count, reserved = struct.unpack_from("<2I", chunks[b"DIDF"])
        ids = struct.unpack_from("<%dI" % count, chunks[b"GRID"])
        lengths = struct.unpack_from("<%dI" % count, chunks[b"DLEN"])
        offsets = struct.unpack_from("<%dI" % count, chunks[b"OFST"])
        if list(offsets) != sorted(offsets):
            raise ValueError("a library whose entries are not in file order")
        entries = [LibraryEntry(i, data[o:o + (n & LENGTH_BITS)], cls.unpack_alignment(n)) for i, n, o in zip(ids, lengths, offsets)]
        ends = list(offsets[1:]) + [len(data)]
        for e, o, nxt in zip(entries, offsets, ends):
            e.slack = data[o + len(e.data):nxt]
        lead = data[8 + size:offsets[0]] if offsets else data[8 + size:]
        self = cls(entries, reserved, lead, order, {t: d for t, d in chunks.items() if t not in cls.DIRECTORY})
        if self.extra:
            raise ValueError("a compressed library (%s): not written here" % b" ".join(self.extra).decode())
        return self

    def write(self):
        count = len(self.entries)
        sizes = {b"DIDF": 8, b"GRID": 4 * count, b"DLEN": 4 * count, b"OFST": 4 * count}
        at = 8 + sum(8 + sizes[t] for t in self.order)
        offsets, fills = [], []
        lead = self.lead if self.lead is not None else b"\0" * (-at % (self.entries[0].alignment if self.entries else DEFAULT_ALIGNMENT))
        at += len(lead)
        for k, e in enumerate(self.entries):
            offsets.append(at)
            at += len(e.data)
            nxt = self.entries[k + 1].alignment if k + 1 < len(self.entries) else DEFAULT_ALIGNMENT
            fill = e.slack if e.slack is not None else b"\0" * (-at % nxt)
            fills.append(fill)
            at += len(fill)
        body = {b"DIDF": struct.pack("<2I", count, self.reserved),
                b"GRID": struct.pack("<%dI" % count, *(e.id for e in self.entries)),
                b"DLEN": struct.pack("<%dI" % count, *(self.pack_length(len(e.data), e.alignment) for e in self.entries)),
                b"OFST": struct.pack("<%dI" % count, *offsets)}
        out = bytearray(b"ZDIR" + struct.pack("<I", sum(8 + sizes[t] for t in self.order)))
        for t in self.order:
            out += t + struct.pack("<I", len(body[t])) + body[t]
        out += lead
        for e, o, fill in zip(self.entries, offsets, fills):
            if o != len(out) or o % e.alignment:
                raise ValueError("asset %08x at 0x%x: off its alignment of %d" % (e.id, o, e.alignment))
            out += e.data + fill
        return bytes(out)

    def find(self, asset_id) -> Optional[LibraryEntry]:
        return next((e for e in self.entries if e.id == asset_id), None)


def cmd_roundtrip(paths):
    bad = 0
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        lib = AssetLibrary.read(data)
        out = lib.write()
        same = out == data
        bad += not same
        print("%s: %d entries, %d bytes, %s" % (p, len(lib.entries), len(data), "byte-identical" if same else "differs (%d written)" % len(out)))
    return 1 if bad else 0


def cmd_selftest():
    lib = AssetLibrary([LibraryEntry(7, b"abc"), LibraryEntry(9, b"x" * 20, 32)])
    data = lib.write()
    back = AssetLibrary.read(data)
    assert back.write() == data and len(data) % 16 == 0 and [e.data for e in back.entries] == [b"abc", b"x" * 20]
    assert struct.unpack_from("<2I", data, 0x40) == (80, 96)
    assert struct.unpack_from("<I", data, 0x10)[0] == 2 and back.find(9).alignment == 32
    empty = AssetLibrary().write()
    assert len(empty) == 48 and AssetLibrary.read(empty).entries == []
    print("zge_library selftest: ok")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["roundtrip"] and a[1:]:
        sys.exit(cmd_roundtrip(a[1:]))
    if a == ["selftest"]:
        sys.exit(cmd_selftest())
    raise SystemExit(__doc__)
