#!/usr/bin/env python3
"""Read and write PS2 memory card images (PCSX2 .ps2, 8 MB with ECC).

Used to compare saves written by the native build (a host folder per card,
ZX_MC_ROOT, see docs/BRINGUP.md) with saves written by PCSX2, and to
move saves between the two.

  python3 tools/ps2mc.py info CARD                 # superblock, free clusters
  python3 tools/ps2mc.py ls CARD [DIR]             # list (recursive)
  python3 tools/ps2mc.py extract CARD DIR HOSTDIR  # copy card DIR (a save) to HOSTDIR/DIR
  python3 tools/ps2mc.py import CARD HOSTDIR       # write host folder HOSTDIR as a root dir of CARD
  python3 tools/ps2mc.py delete CARD DIR           # remove a root dir (a save) and its files
  python3 tools/ps2mc.py checkecc CARD             # verify every page's ECC

Format (as in mymc and PCSX2): 528-byte pages (512 data + 16 spare, 12 of
them ECC: 3 bytes per 128-byte chunk), 2-page (1 KB) clusters, a two-level
FAT (superblock ifc_list -> indirect FAT clusters -> FAT clusters of u32
entries: 0x80000000 | next, 0xFFFFFFFF end of chain, 0x7FFFFFFF free),
512-byte directory entries.
"""
import os
import struct
import sys
import time

PAGE = 512
RAW_PAGE = 528

DF_READ, DF_WRITE, DF_EXECUTE, DF_PROTECTED = 0x0001, 0x0002, 0x0004, 0x0008
DF_FILE, DF_DIR, DF_CLOSED, DF_0400, DF_EXISTS = 0x0010, 0x0020, 0x0080, 0x0400, 0x8000
# As mcman writes them: 0x8427 directories (and their . and ..), 0x8497 files.
MODE_DIR = DF_EXISTS | DF_0400 | DF_DIR | DF_READ | DF_WRITE | DF_EXECUTE
MODE_FILE = DF_EXISTS | DF_0400 | DF_CLOSED | DF_FILE | DF_READ | DF_WRITE | DF_EXECUTE
FAT_FREE = 0x7FFFFFFF
FAT_END = 0xFFFFFFFF


def _parity(b):
    b ^= b >> 4
    b ^= b >> 2
    b ^= b >> 1
    return b & 1


_PARITY = [_parity(b) for b in range(256)]
_CPMASK = []
for _b in range(256):
    _m = 0
    for _i, _mask in enumerate((0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0)):
        _m |= _PARITY[_b & _mask] << _i
    _CPMASK.append(_m)


def ecc128(chunk):
    """Hamming code of a 128-byte chunk: 3 bytes (column parity, line parity 0/1)."""
    cp, lp0, lp1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(chunk):
        cp ^= _CPMASK[b]
        if _PARITY[b]:
            lp0 ^= ~i
            lp1 ^= i
    return bytes((cp & 0xFF, lp0 & 0x7F, lp1 & 0xFF))


def page_spare(data):
    return b"".join(ecc128(data[i:i + 128]) for i in range(0, PAGE, 128)) + b"\0" * 4


def tod(t=None):
    """sceMcStDateTime: resv, sec, min, hour, day, month, year (u16), in JST
    (UTC+9) like the PS2 clock and PCSX2."""
    lt = time.gmtime((time.time() if t is None else t) + 9 * 3600)
    return struct.pack("<BBBBBBH", 0, lt.tm_sec, lt.tm_min, lt.tm_hour, lt.tm_mday, lt.tm_mon, lt.tm_year)


class Card:
    def __init__(self, path, write=False):
        self.path = path
        self.f = open(path, "r+b" if write else "rb")
        self.raw = self.f.read()
        if len(self.raw) % RAW_PAGE == 0:
            self.raw_page = RAW_PAGE
        elif len(self.raw) % PAGE == 0:
            self.raw_page = PAGE
        else:
            raise SystemExit("%s: not a card image" % path)
        self.buf = bytearray(self.raw)
        sb = self.page(0)
        if not sb.startswith(b"Sony PS2 Memory Card Format "):
            raise SystemExit("%s: unformatted" % path)
        (self.page_len, self.ppc, self.ppb, _, self.clusters, self.alloc_offset, self.alloc_end,
         self.root, self.backup1, self.backup2) = struct.unpack_from("<HHHHIIIIII", sb, 0x28)
        self.ifc = [x for x in struct.unpack_from("<32I", sb, 0x50) if x]
        self.csize = self.page_len * self.ppc
        self.epc = self.csize // 4             # FAT entries per cluster

    # -- raw access
    def page(self, n):
        o = n * self.raw_page
        return bytes(self.buf[o:o + PAGE])

    def write_page(self, n, data):
        assert len(data) == PAGE
        o = n * self.raw_page
        self.buf[o:o + PAGE] = data
        if self.raw_page == RAW_PAGE:
            self.buf[o + PAGE:o + RAW_PAGE] = page_spare(data)

    def cluster(self, n):
        return b"".join(self.page(n * self.ppc + i) for i in range(self.ppc))

    def write_cluster(self, n, data):
        data = data.ljust(self.csize, b"\0")
        for i in range(self.ppc):
            self.write_page(n * self.ppc + i, data[i * PAGE:(i + 1) * PAGE])

    # -- FAT
    def _fat_loc(self, n):
        per = self.epc
        ind = n // (per * per)
        fat_cluster = struct.unpack_from("<I", self.cluster(self.ifc[ind]), ((n // per) % per) * 4)[0]
        return fat_cluster, (n % per) * 4

    def fat(self, n):
        c, off = self._fat_loc(n)
        return struct.unpack_from("<I", self.cluster(c), off)[0]

    def set_fat(self, n, value):
        c, off = self._fat_loc(n)
        data = bytearray(self.cluster(c))
        struct.pack_into("<I", data, off, value)
        self.write_cluster(c, bytes(data))

    def chain(self, first):
        out = []
        n = first
        while True:
            out.append(n)
            e = self.fat(n)
            if e == FAT_END:
                return out
            if not e & 0x80000000:
                raise SystemExit("broken FAT chain at %d (0x%08x)" % (n, e))
            n = e & 0x7FFFFFFF

    def free_clusters(self):
        return [n for n in range(self.alloc_end) if self.fat(n) == FAT_FREE]

    # -- files
    def read_chain(self, first, size):
        data = b"".join(self.cluster(self.alloc_offset + n) for n in self.chain(first))
        return data[:size]

    def dir_entries(self, first, count):
        data = self.read_chain(first, count * 512)
        return [parse_entry(data[i * 512:(i + 1) * 512]) for i in range(count)]

    def root_entries(self):
        root = parse_entry(self.read_chain(self.root, 512))
        return self.dir_entries(self.root, root["length"])

    def lookup(self, path):
        """Directory entries of the directory at path ('/' or '/NAME')."""
        ents = self.root_entries()
        for part in [p for p in path.split("/") if p]:
            match = [e for e in ents if e["name"] == part and e["mode"] & DF_EXISTS]
            if not match:
                raise SystemExit("%s: not found" % path)
            e = match[0]
            if not e["mode"] & DF_DIR:
                return e
            ents = self.dir_entries(e["cluster"], e["length"])
        return ents

    def save(self):
        self.f.seek(0)
        self.f.write(self.buf)
        self.f.flush()


def parse_entry(b):
    mode, _, length = struct.unpack_from("<HHI", b, 0)
    cluster, dir_entry = struct.unpack_from("<II", b, 0x10)
    attr = struct.unpack_from("<I", b, 0x20)[0]
    name = b[0x40:0x60].split(b"\0")[0].decode("latin-1")
    return {"mode": mode, "length": length, "cluster": cluster, "dir_entry": dir_entry,
            "attr": attr, "name": name, "created": b[8:16], "modified": b[0x18:0x20]}


def make_entry(mode, length, cluster, dir_entry, name, t=None):
    b = bytearray(512)
    struct.pack_into("<HHI", b, 0, mode, 0, length)
    b[8:16] = tod(t)
    struct.pack_into("<II", b, 0x10, cluster, dir_entry)
    b[0x18:0x20] = tod(t)
    b[0x40:0x40 + len(name)] = name.encode("latin-1")
    return bytes(b)


def fmt_tod(b):
    _, s, m, h, d, mo, y = struct.unpack("<BBBBBBH", b)
    return "%04d-%02d-%02d %02d:%02d:%02d" % (y, mo, d, h, m, s)


def cmd_info(card):
    c = Card(card)
    print("clusters %d, alloc_offset %d, alloc_end %d, root %d, ifc %s" % (
        c.clusters, c.alloc_offset, c.alloc_end, c.root, c.ifc))
    print("free clusters (1 KB): %d" % len(c.free_clusters()))


def cmd_ls(card, path="/"):
    c = Card(card)

    def walk(p, ents, depth):
        for e in ents:
            if not e["mode"] & DF_EXISTS:
                continue
            kind = "d" if e["mode"] & DF_DIR else "-"
            print("%s%s %04x %8d %s %s" % ("  " * depth, kind, e["mode"], e["length"], fmt_tod(e["modified"]), e["name"]))
            if e["mode"] & DF_DIR and e["name"] not in (".", ".."):
                walk(p + "/" + e["name"], c.dir_entries(e["cluster"], e["length"]), depth + 1)

    ents = c.lookup(path)
    walk(path.rstrip("/"), ents if isinstance(ents, list) else [ents], 0)


def cmd_extract(card, path, hostdir):
    c = Card(card)
    name = path.strip("/")
    out = os.path.join(hostdir, name)
    os.makedirs(out, exist_ok=True)
    for e in c.lookup("/" + name):
        if e["mode"] & DF_EXISTS and e["mode"] & DF_FILE:
            data = c.read_chain(e["cluster"], e["length"]) if e["length"] else b""
            open(os.path.join(out, e["name"]), "wb").write(data)
            print("%8d %s" % (len(data), e["name"]))


def _alloc(c, free, n):
    """Take n clusters from the free list and chain them in the FAT."""
    if n > len(free):
        raise SystemExit("card full")
    got = [free.pop(0) for _ in range(n)]
    for a, b in zip(got, got[1:] + [None]):
        c.set_fat(a, FAT_END if b is None else 0x80000000 | b)
    return got


def _write_chain(c, clusters, data):
    for i, n in enumerate(clusters):
        c.write_cluster(c.alloc_offset + n, data[i * c.csize:(i + 1) * c.csize])


def cmd_import(card, hostdir):
    c = Card(card, write=True)
    name = os.path.basename(os.path.normpath(hostdir))
    if any(e["name"] == name and e["mode"] & DF_EXISTS for e in c.root_entries()):
        raise SystemExit("%s already on the card (delete it first)" % name)
    free = c.free_clusters()
    files = sorted(f for f in os.listdir(hostdir) if os.path.isfile(os.path.join(hostdir, f)))
    # Directory: ".", "..", then the files.
    count = 2 + len(files)
    dclusters = _alloc(c, free, -(-count * 512 // c.csize))
    root_ents = c.root_entries()
    slot = next((i for i, e in enumerate(root_ents) if not e["mode"] & DF_EXISTS), len(root_ents))
    entries = [make_entry(MODE_DIR, 0, c.root, slot, "."), make_entry(MODE_DIR, 0, 0, 0, "..")]
    for f in files:
        data = open(os.path.join(hostdir, f), "rb").read()
        fc = _alloc(c, free, max(1, -(-len(data) // c.csize))) if data else None
        if fc:
            _write_chain(c, fc, data)
        entries.append(make_entry(MODE_FILE, len(data), fc[0] if fc else FAT_END, 0, f))
    _write_chain(c, dclusters, b"".join(entries))
    # Root: add the entry and bump its length ("." entry of the root).
    new = make_entry(MODE_DIR, count, dclusters[0], 0, name)
    rchain = c.chain(c.root)
    rdata = bytearray(c.read_chain(c.root, len(rchain) * c.csize))
    if (slot + 1) * 512 > len(rdata):
        last = rchain[-1]
        extra = _alloc(c, free, 1)
        c.set_fat(last, 0x80000000 | extra[0])
        rchain += extra
        rdata += b"\0" * c.csize
    rdata[slot * 512:(slot + 1) * 512] = new
    if slot == len(root_ents):
        struct.pack_into("<I", rdata, 4, len(root_ents) + 1)
    _write_chain(c, rchain, bytes(rdata))
    c.save()
    print("imported %s: %d files, %d free clusters left" % (name, len(files), len(free)))


def cmd_delete(card, path):
    c = Card(card, write=True)
    name = path.strip("/")
    root_ents = c.root_entries()
    idx = next((i for i, e in enumerate(root_ents) if e["name"] == name and e["mode"] & DF_EXISTS), None)
    if idx is None:
        raise SystemExit("%s: not found" % name)
    d = root_ents[idx]
    for e in c.dir_entries(d["cluster"], d["length"])[2:]:
        if e["mode"] & DF_EXISTS and e["length"]:
            for n in c.chain(e["cluster"]):
                c.set_fat(n, FAT_FREE)
    for n in c.chain(d["cluster"]):
        c.set_fat(n, FAT_FREE)
    rchain = c.chain(c.root)
    rdata = bytearray(c.read_chain(c.root, len(rchain) * c.csize))
    struct.pack_into("<H", rdata, idx * 512, root_ents[idx]["mode"] & ~DF_EXISTS)
    _write_chain(c, rchain, bytes(rdata))
    c.save()
    print("deleted %s" % name)


def cmd_checkecc(card):
    c = Card(card)
    if c.raw_page != RAW_PAGE:
        raise SystemExit("no ECC in this image")
    bad = 0
    for n in range(len(c.raw) // RAW_PAGE):
        o = n * RAW_PAGE
        if c.raw[o + PAGE:o + PAGE + 12] != page_spare(c.raw[o:o + PAGE])[:12]:
            bad += 1
    print("%d pages, %d with ECC mismatch" % (len(c.raw) // RAW_PAGE, bad))


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return
    verbs = {"info": cmd_info, "ls": cmd_ls, "extract": cmd_extract, "import": cmd_import,
             "delete": cmd_delete, "checkecc": cmd_checkecc}
    if argv[1] not in verbs:
        raise SystemExit("unknown verb %s" % argv[1])
    verbs[argv[1]](*argv[2:])


if __name__ == "__main__":
    main(sys.argv)
