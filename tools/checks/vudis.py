#!/usr/bin/env python3
"""Disassemble PS2 VU microcode (upper + lower pairs).

Usage: vudis.py <code.bin> [start_byte_addr] [count]
Addresses are byte addresses into micro memory (8 bytes per pair).
"""
import struct
import sys

BC = "xyzw"


def dest(i):
    d = (i >> 21) & 0xF
    return "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b)


UPPER = {}
for base, name in ((0x00, "ADD"), (0x04, "SUB"), (0x08, "MADD"), (0x0C, "MSUB"), (0x10, "MAX"),
                   (0x14, "MINI"), (0x18, "MUL")):
    for k in range(4):
        UPPER[base + k] = (name + BC[k], "bc")
UPPER.update({0x1C: ("MULq", "q"), 0x1D: ("MAXi", "i"), 0x1E: ("MULi", "i"), 0x1F: ("MINIi", "i"),
              0x20: ("ADDq", "q"), 0x21: ("MADDq", "q"), 0x22: ("ADDi", "i"), 0x23: ("MADDi", "i"),
              0x24: ("SUBq", "q"), 0x25: ("MSUBq", "q"), 0x26: ("SUBi", "i"), 0x27: ("MSUBi", "i"),
              0x28: ("ADD", "3"), 0x29: ("MADD", "3"), 0x2A: ("MUL", "3"), 0x2B: ("MAX", "3"),
              0x2C: ("SUB", "3"), 0x2D: ("MSUB", "3"), 0x2E: ("OPMSUB", "3"), 0x2F: ("MINI", "3")})

UPPER2 = {}
for base, name in ((0x00, "ADDA"), (0x04, "SUBA"), (0x08, "MADDA"), (0x0C, "MSUBA"), (0x18, "MULA")):
    for k in range(4):
        UPPER2[base + k] = (name + BC[k], "abc")
UPPER2.update({0x10: ("ITOF0", "2"), 0x11: ("ITOF4", "2"), 0x12: ("ITOF12", "2"), 0x13: ("ITOF15", "2"),
               0x14: ("FTOI0", "2"), 0x15: ("FTOI4", "2"), 0x16: ("FTOI12", "2"), 0x17: ("FTOI15", "2"),
               0x1C: ("MULAq", "aq"), 0x1D: ("ABS", "2"), 0x1E: ("MULAi", "aq"), 0x1F: ("CLIP", "clip"),
               0x20: ("ADDAq", "aq"), 0x21: ("MADDAq", "aq"), 0x22: ("ADDAi", "aq"), 0x23: ("MADDAi", "aq"),
               0x24: ("SUBAq", "aq"), 0x25: ("MSUBAq", "aq"), 0x26: ("SUBAi", "aq"), 0x27: ("MSUBAi", "aq"),
               0x28: ("ADDA", "a3"), 0x29: ("MADDA", "a3"), 0x2A: ("MULA", "a3"), 0x2C: ("SUBA", "a3"),
               0x2D: ("MSUBA", "a3"), 0x2E: ("OPMULA", "a3"), 0x2F: ("NOP", "none")})


def upper(i):
    fn = i & 0x3F
    fd, fs, ft = (i >> 6) & 31, (i >> 11) & 31, (i >> 16) & 31
    flags = "".join(f for f, b in (("[I]", 31), ("[E]", 30), ("[M]", 29), ("[D]", 28), ("[T]", 27)) if i >> b & 1)
    d = dest(i)
    if fn >= 0x3C:
        idx = ((i >> 6) & 0x1F) << 2 | (i & 3)
        name, kind = UPPER2.get(idx, ("?u2_%02x" % idx, "none"))
        if kind == "abc":
            s = "%s.%s ACC, vf%d, vf%d%s" % (name, d, fs, ft, name[-1])
        elif kind == "2":
            s = "%s.%s vf%d, vf%d" % (name, d, ft, fs)
        elif kind == "aq":
            s = "%s.%s ACC, vf%d, %s" % (name, d, fs, "Q" if "q" in name else "I")
        elif kind == "clip":
            s = "CLIPw.xyz vf%d, vf%dw" % (fs, ft)
        elif kind == "a3":
            s = "%s.%s ACC, vf%d, vf%d" % (name, d, fs, ft)
        else:
            s = name
    else:
        name, kind = UPPER.get(fn, ("?u_%02x" % fn, "3"))
        if kind == "bc":
            s = "%s.%s vf%d, vf%d, vf%d%s" % (name, d, fd, fs, ft, name[-1])
        elif kind in ("q", "i"):
            s = "%s.%s vf%d, vf%d, %s" % (name, d, fd, fs, kind.upper())
        else:
            s = "%s.%s vf%d, vf%d, vf%d" % (name, d, fd, fs, ft)
    return s + (" " + flags if flags else "")


LOWER2 = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV",
          0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR",
          0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP",
          0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy",
          0x75: "EATANxz", 0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP",
          0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}
LOWER = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
         0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
         0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
         0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}


def simm11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


def lower(i, pc):
    op = (i >> 25) & 0x7F
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    d = dest(i)
    if op == 0x40:
        fn = i & 0x3F
        if fn == 0x30:
            return "IADD vi%d, vi%d, vi%d" % (fd, fs, ft)
        if fn == 0x31:
            return "ISUB vi%d, vi%d, vi%d" % (fd, fs, ft)
        if fn == 0x32:
            imm = (i >> 6) & 31
            imm = imm - 32 if imm & 16 else imm
            return "IADDI vi%d, vi%d, %d" % (ft, fs, imm)
        if fn == 0x34:
            return "IAND vi%d, vi%d, vi%d" % (fd, fs, ft)
        if fn == 0x35:
            return "IOR vi%d, vi%d, vi%d" % (fd, fs, ft)
        if fn >= 0x3C:
            idx = ((i >> 6) & 0x1F) << 2 | (i & 3)
            name = LOWER2.get(idx, "?l2_%02x" % idx)
            fsf, ftf = BC[(i >> 21) & 3], BC[(i >> 23) & 3]
            if name in ("MOVE", "MR32"):
                return "%s.%s vf%d, vf%d" % (name, d, ft, fs)
            if name in ("LQI", "LQD"):
                return "%s.%s vf%d, (vi%d%s)" % (name, d, ft, fs, "++" if name == "LQI" else "--")
            if name in ("SQI", "SQD"):
                return "%s.%s vf%d, (vi%d%s)" % (name, d, fs, ft, "++" if name == "SQI" else "--")
            if name in ("DIV", "RSQRT"):
                return "%s Q, vf%d%s, vf%d%s" % (name, fs, fsf, ft, ftf)
            if name == "SQRT":
                return "SQRT Q, vf%d%s" % (ft, ftf)
            if name == "MTIR":
                return "MTIR vi%d, vf%d%s" % (ft, fs, fsf)
            if name == "MFIR":
                return "MFIR.%s vf%d, vi%d" % (d, ft, fs)
            if name in ("ILWR", "ISWR"):
                return "%s.%s vi%d, (vi%d)" % (name, d, ft, fs)
            if name in ("XGKICK", "XTOP", "XITOP"):
                return "%s vi%d" % (name, fs if name == "XGKICK" else ft)
            if name == "MFP":
                return "MFP.%s vf%d, P" % (d, ft)
            if name.startswith("E"):
                return "%s P, vf%d" % (name, fs)
            return name
        return "?l40_%02x" % fn
    name = LOWER.get(op, "?l_%02x" % op)
    imm = simm11(i)
    target = (pc + 8 + imm * 8) & 0x3FFF
    if name in ("LQ",):
        return "LQ.%s vf%d, %d(vi%d)" % (d, ft, imm, fs)
    if name in ("SQ",):
        return "SQ.%s vf%d, %d(vi%d)" % (d, fs, imm, ft)
    if name in ("ILW", "ISW"):
        return "%s.%s vi%d, %d(vi%d)" % (name, d, ft, imm, fs)
    if name in ("IADDIU", "ISUBIU"):
        imm15 = ((i >> 10) & 0x7800) | (i & 0x7FF)
        return "%s vi%d, vi%d, 0x%x" % (name, ft, fs, imm15)
    if name in ("B", "BAL"):
        return "%s %svi%d 0x%x" % (name, "", ft, target) if name == "BAL" else "B 0x%x" % target
    if name in ("JR", "JALR"):
        return "%s vi%d%s" % (name, fs, (", vi%d" % ft) if name == "JALR" else "")
    if name in ("IBEQ", "IBNE"):
        return "%s vi%d, vi%d, 0x%x" % (name, ft, fs, target)
    if name.startswith("IB"):
        return "%s vi%d, 0x%x" % (name, fs, target)
    if name in ("FCEQ", "FCAND", "FCOR"):
        return "%s vi1, 0x%06x" % (name, i & 0xFFFFFF)
    if name == "FCSET":
        return "FCSET 0x%06x" % (i & 0xFFFFFF)
    if name in ("FMEQ", "FMAND", "FMOR"):
        return "%s vi%d, vi%d" % (name, (i >> 16) & 15, (i >> 11) & 15)
    if name == "FCGET":
        return "FCGET vi%d" % ((i >> 16) & 15)
    if name.startswith("FS"):
        imm12 = (((i >> 21) & 1) << 11) | (i & 0x7FF)
        return ("FSSET 0x%03x" % imm12) if name == "FSSET" else "%s vi%d, 0x%03x" % (name, (i >> 16) & 15, imm12)
    return name


def main():
    code = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
    for pc in range(start, min(len(code), start + count * 8), 8):
        lo, hi = struct.unpack_from("<II", code, pc)
        u = upper(hi)
        if hi >> 31 & 1:
            l = "LOI %g (0x%08x)" % (struct.unpack("<f", struct.pack("<I", lo))[0], lo)
        else:
            l = lower(lo, pc)
        print("%04x: %-44s %s" % (pc, u, l))


if __name__ == "__main__":
    main()
