#!/usr/bin/env python3
"""Independent VU1 micro-mode simulator for differential checks of recorded runs.

Written from the VU manual / PCSX2 semantics, separately from the runtime's
VU1Interpreter, to cross-check what a recorded run emits:

  vu1sim.py <corpus.vu1r> [--first N] [--count N] [--pc 0xPC] [--fused] [--verbose]

For every run (MSCAL, or MSCNT from its recorded pc) it simulates the program from the recorded input
state and data memory and compares the PATH1 packets and the data memory
written with the recording. Floats round toward zero, with the product of
MADD/MSUB rounded before the add (PCSX2's order, VU1Interpreter since it is
built with -ffp-contract=off); --fused rounds them once (VU1Interpreter
built with FMA contraction, corpora recorded before that change).

Timing model: one pair per cycle; reads of VF/ACC/VI results stall until the
writer's latency (FMAC/LQ/MOVE/MFIR 4, ACC 1, IALU 1, ILW 4); MAC/status/clip
flags become visible 4 cycles after issue; Q after 7 (DIV, SQRT) or 13
(RSQRT); a branch reading a VI written by the previous IALU instruction sees
the old value; XGKICK sends the packet as it is in memory when issued.
"""
import math
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import vu1corpus  # noqa: E402

MAXF = 0x7F7FFFFF


def u2f(u):
    """Operand read: denormals are zero, exponent 255 is +-max."""
    e = (u >> 23) & 0xFF
    if e == 0:
        return -0.0 if u >> 31 else 0.0
    if e == 255:
        u = (u & 0x80000000) | MAXF
    return struct.unpack("<f", struct.pack("<I", u))[0]


def raw2f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def rz(x):
    """Exact value (Fraction) or double -> (float32 bits, flags), rounding toward
    zero. flags: 1 Z, 2 S, 4 U, 8 O. A Fraction keeps its sign in .neg (for -0)."""
    if isinstance(x, Exact):
        return x.rz()
    sign = 0x80000000 if math.copysign(1.0, x) < 0 else 0
    flags = 2 if sign else 0
    a = abs(x)
    if a == 0.0:
        return sign, flags | 1
    if math.isinf(a) or math.isnan(a):
        return sign | MAXF, flags | 8
    m, e = math.frexp(a)  # a = m * 2^e, 0.5 <= m < 1
    exp = e - 1 + 127
    if exp >= 255:
        return sign | MAXF, flags | 8
    if exp <= 0:
        return sign, flags | 5
    mant = int(m * (1 << 24))  # truncation, 2^23 <= mant < 2^24
    return sign | (exp << 23) | (mant - (1 << 23)), flags


def rzf(x):
    return rz(x)[0]


class Exact:
    """Exact rational with a signed zero, rounded once (toward zero)."""
    __slots__ = ("v", "neg")

    def __init__(self, v, neg=False):
        from fractions import Fraction
        self.v = Fraction(v)
        self.neg = neg if self.v == 0 else self.v < 0

    @staticmethod
    def of(f):
        return Exact(f, math.copysign(1.0, f) < 0)

    def add(self, other):
        r = self.v + other.v
        # IEEE: x + (-x) is +0 toward zero; -0 + -0 is -0
        return Exact(r, self.neg and other.neg if r == 0 else False)

    def neg_(self):
        return Exact(-self.v, not self.neg)

    def mul(self, other):
        return Exact(self.v * other.v, self.neg != other.neg)

    def rz(self):
        from fractions import Fraction
        sign = 0x80000000 if self.neg else 0
        flags = 2 if sign else 0
        a = abs(self.v)
        if a == 0:
            return sign, flags | 1
        e = a.numerator.bit_length() - a.denominator.bit_length()
        if Fraction(2) ** e > a:
            e -= 1
        exp = e + 127
        if exp >= 255:
            return sign | MAXF, flags | 8
        if exp <= 0:
            return sign, flags | 5
        mant = int(a / (Fraction(2) ** e) * (1 << 23))
        return sign | (exp << 23) | (mant - (1 << 23)), flags


def rz_div(s, t):
    """s / t rounded toward zero from the exact quotient (no double rounding)."""
    from fractions import Fraction
    q = Fraction(s) / Fraction(t)
    sign = q < 0
    a = abs(q)
    if a == 0:
        return 0x80000000 if sign else 0
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    exp = e + 127
    if exp >= 255:
        return (0x80000000 if sign else 0) | MAXF
    if exp <= 0:
        return 0x80000000 if sign else 0
    mant = int(a / (Fraction(2) ** e) * (1 << 23))
    return (0x80000000 if sign else 0) | (exp << 23) | (mant - (1 << 23))


LANE = (8, 4, 2, 1)  # x y z w as dest bits


class Sim:
    def __init__(self, code, data, st, top, itop, fused=False):
        self.code = code
        self.mem = bytearray(data)
        self.vf = [list(r) for r in st["vf"]]
        self.vi = [v & 0xFFFF for v in st["vi"]]
        self.acc = list(st["acc"])
        self.q, self.p, self.i, self.r = st["q"], st["p"], st["i"], st["r"]
        self.mac, self.status, self.clip = st["mac"], st["status"], st["clip"]
        self.top, self.itop = top, itop
        self.fused = fused
        self.cycle = 0
        self.vf_ready = [[0] * 4 for _ in range(32)]
        self.vi_ready = [0] * 16
        self.acc_ready = [0] * 4
        self.flag_q = []  # (ready, kind, value)
        self.work_clip = self.clip
        self.q_pending = None  # (ready, value)
        self.packets = []
        self.vi_backup = None  # (reg, old) visible to the next branch

    # memory -----------------------------------------------------------
    def ld(self, qaddr):
        o = (qaddr & 0x3FF) * 16
        return list(struct.unpack_from("<4I", self.mem, o))

    def st(self, qaddr, words, dest):
        o = (qaddr & 0x3FF) * 16
        cur = list(struct.unpack_from("<4I", self.mem, o))
        for c in range(4):
            if dest & LANE[c]:
                cur[c] = words[c] & 0xFFFFFFFF
        struct.pack_into("<4I", self.mem, o, *cur)

    # pipelines ----------------------------------------------------------
    def commit(self):
        keep = []
        for ent in self.flag_q:
            ready, kind, val = ent
            if ready <= self.cycle:
                if kind == "fmac":
                    mac, cur, sticky = val
                    self.mac = mac
                    self.status = (self.status & 0xFF0) | cur | ((cur | sticky) << 6)
                elif kind == "clip":
                    self.clip = val
                elif kind == "fsset":
                    self.status = (self.status & 0x3F) | (val & 0xFC0)
            else:
                keep.append(ent)
        self.flag_q = keep
        if self.q_pending and self.q_pending[0] <= self.cycle:
            self.q = self.q_pending[1]
            di = self.q_pending[2]
            self.status = (self.status & 0xFCF) | di | (di << 6)
            self.q_pending = None

    def tick(self, n=1):
        for _ in range(n):
            self.cycle += 1
            self.commit()

    def wait_vf(self, reg, lanes):
        if reg == 0:
            return
        need = max([self.vf_ready[reg][c] for c in range(4) if lanes & LANE[c]] + [0])
        if need > self.cycle:
            self.tick(need - self.cycle)

    def wait_vi(self, reg):
        if reg and self.vi_ready[reg] > self.cycle:
            self.tick(self.vi_ready[reg] - self.cycle)

    def wait_acc(self, lanes):
        need = max([self.acc_ready[c] for c in range(4) if lanes & LANE[c]] + [0])
        if need > self.cycle:
            self.tick(need - self.cycle)

    # upper ----------------------------------------------------------------
    def fmac(self, kind, dest, fd, fs, ft, src, toacc, bc=0):
        a = [u2f(x) for x in self.vf[fs]]
        acc = [u2f(x) for x in self.acc]
        if src == "bc":
            b = [u2f(self.vf[ft][bc])] * 4
        elif src == "q":
            b = [u2f(self.q)] * 4
        elif src == "i":
            b = [u2f(self.i)] * 4
        else:
            b = [u2f(x) for x in self.vf[ft]]
        res = [0] * 4
        lflags = [0] * 4
        sticky = 0
        for c in range(4):
            if not dest & LANE[c]:
                continue
            if kind in ("opmula", "opmsub"):
                if c == 3:
                    x = 0.0
                    l, r = 0.0, 0.0
                else:
                    l, r = a[(1, 2, 0)[c]], b[(2, 0, 1)[c]]
            else:
                l, r = a[c], b[c]
            E = Exact.of
            if kind == "add":
                x = E(a[c]).add(E(b[c]))
            elif kind == "sub":
                x = E(a[c]).add(E(b[c]).neg_())
            elif kind in ("mul", "opmula"):
                x = E(l).mul(E(r)) if c < 3 or kind == "mul" else 0.0
            elif kind in ("madd", "msub", "opmsub"):
                prod = E(l).mul(E(r)) if (kind != "opmsub" or c < 3) else E(0.0)
                if not self.fused:
                    prod = E(raw2f(prod.rz()[0]))
                if kind == "madd":
                    x = E(acc[c]).add(prod)
                elif kind == "opmsub" and c == 3:
                    x = 0.0
                else:
                    x = E(acc[c]).add(prod.neg_())
                pf = E(l).mul(E(r)).rz()[1] if (kind != "opmsub" or c < 3) else 1
                sticky |= pf & 0xF
            res[c], lflags[c] = rz(x)
        mac = 0
        cur = 0
        for c in range(4):
            if not dest & LANE[c]:
                continue
            f = lflags[c]
            ln = LANE[c]
            if f & 1:
                mac |= ln
            if f & 2:
                mac |= ln << 4
            if f & 4:
                mac |= ln << 8
            if f & 8:
                mac |= ln << 12
            cur |= f
        target = self.acc if toacc else self.vf[fd]
        for c in range(4):
            if dest & LANE[c]:
                target[c] = res[c]
        self.flag_q.append((self.cycle + 4, "fmac", (mac, cur & 0xF, sticky)))
        return ("acc", dest) if toacc else ("vf", fd, dest)

    def upper(self, ins):
        op = ins & 0x3F
        dest = (ins >> 21) & 0xF
        ft, fs, fd = (ins >> 16) & 31, (ins >> 11) & 31, (ins >> 6) & 31
        if op < 0x3C:
            idx, toacc = op, False
        else:
            idx, toacc = (ins & 3) | ((ins >> 4) & 0x7C), True
        bckinds = {0: "add", 1: "sub", 2: "madd", 3: "msub"}
        # reads for stalls
        if not toacc and 0x10 <= op <= 0x17 or (not toacc and op in (0x1D, 0x1F, 0x2B, 0x2F)):
            # MAX / MINI on raw words
            self.wait_vf(fs, dest)
            if op <= 0x17:
                self.wait_vf(ft, LANE[op & 3])
                bv = [self.vf[ft][op & 3]] * 4
            elif op in (0x1D, 0x1F):
                bv = [self.i] * 4
            else:
                self.wait_vf(ft, dest)
                bv = list(self.vf[ft])
            ismax = op in (0x10, 0x11, 0x12, 0x13, 0x1D, 0x2B)
            out = []
            for c in range(4):
                x = struct.unpack("<i", struct.pack("<I", self.vf[fs][c]))[0]
                y = struct.unpack("<i", struct.pack("<I", bv[c]))[0]
                if x < 0 and y < 0:
                    v = min(x, y) if ismax else max(x, y)
                else:
                    v = max(x, y) if ismax else min(x, y)
                out.append(v & 0xFFFFFFFF)
            return ("write", fd, dest, out, 4)
        if toacc and 0x10 <= idx <= 0x17:
            self.wait_vf(fs, dest)
            out = []
            for c in range(4):
                v = self.vf[fs][c]
                if idx <= 0x13:
                    iv = struct.unpack("<i", struct.pack("<I", v))[0]
                    out.append(rzf(iv / (1, 16, 4096, 32768)[idx - 0x10]))
                else:
                    x = u2f(v) * (1, 16, 4096, 32768)[idx - 0x14]
                    x = max(min(x, 2147483647.0), -2147483648.0)
                    out.append(int(x) & 0xFFFFFFFF)
            return ("write", ft, dest, out, 4)
        if toacc and idx == 0x1D:  # ABS
            self.wait_vf(fs, dest)
            out = [rzf(abs(u2f(v))) for v in self.vf[fs]]
            return ("write", ft, dest, out, 4)
        if toacc and idx == 0x1F:  # CLIP
            self.wait_vf(fs, 0xE)
            self.wait_vf(ft, 1)
            w = abs(u2f(self.vf[ft][3]))
            x, y, z = (u2f(v) for v in self.vf[fs][:3])
            f = 0
            f |= 1 if x > w else 0
            f |= 2 if x < -w else 0
            f |= 4 if y > w else 0
            f |= 8 if y < -w else 0
            f |= 16 if z > w else 0
            f |= 32 if z < -w else 0
            self.work_clip = ((self.work_clip << 6) | f) & 0xFFFFFF
            self.flag_q.append((self.cycle + 4, "clip", self.work_clip))
            return None
        if toacc and idx in (0x2F, 0x30):
            return None
        # FMAC
        if idx <= 0x0F:
            kind, src, bc = bckinds[idx >> 2], "bc", idx & 3
        elif 0x18 <= idx <= 0x1B:
            kind, src, bc = "mul", "bc", idx & 3
        elif idx in (0x1C, 0x1E):
            kind, src, bc = "mul", "q" if idx == 0x1C else "i", 0
        elif 0x20 <= idx <= 0x27:
            kind = ("add", "madd", "add", "madd", "sub", "msub", "sub", "msub")[idx - 0x20]
            src, bc = ("q" if (idx & 2) == 0 else "i"), 0
        elif idx in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
            kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}[idx]
            src, bc = "v", 0
        elif idx == 0x2E:
            kind, src, bc = ("opmula" if toacc else "opmsub"), "v", 0
        else:
            raise RuntimeError("upper %08x" % ins)
        cross = kind in ("opmula", "opmsub")
        self.wait_vf(fs, 0xE if cross else dest)
        if src == "bc":
            self.wait_vf(ft, LANE[bc])
        elif src == "v":
            self.wait_vf(ft, 0xE if cross else dest)
        if kind in ("madd", "msub", "opmsub"):
            self.wait_acc(dest)
        return ("fmac", kind, dest, fd, fs, ft, src, toacc, bc)

    # main loop -------------------------------------------------------------
    def run(self, pc, max_pairs=200000):
        end_after = None
        branch = None
        n = 0
        while n < max_pairs:
            n += 1
            self.commit()
            lo, hi = struct.unpack_from("<II", self.code, pc)
            # stalls of upper are applied while decoding it
            up = self.upper(hi)
            low_writes = None
            if hi >> 31:
                self.vi_backup = None
            else:
                low_writes = self.lower(lo, pc)
            # execute upper with pre-pair state already read; apply results
            if up and up[0] == "fmac":
                _, kind, dest, fd, fs, ft, src, toacc, bc = up
                saved = list(self.vf[fd]) if not toacc else None
                r = self.fmac(kind, dest, fd, fs, ft, src, toacc, bc)
                if toacc:
                    for c in range(4):
                        if dest & LANE[c]:
                            self.acc_ready[c] = self.cycle + 1
                elif fd:
                    for c in range(4):
                        if dest & LANE[c]:
                            self.vf_ready[fd][c] = self.cycle + 4
            elif up and up[0] == "write":
                _, reg, dest, out, lat = up
                if reg:
                    for c in range(4):
                        if dest & LANE[c]:
                            self.vf[reg][c] = out[c]
                            self.vf_ready[reg][c] = self.cycle + lat
            if hi >> 31:
                self.i = lo
            if low_writes:
                for w in low_writes:
                    w()
            self.vf[0] = [0, 0, 0, 0x3F800000]
            self.vi[0] = 0
            npc = (pc + 8) & 0x3FFF
            if branch is not None:
                npc, branch = branch, None
            if self.pending_branch is not None:
                branch, self.pending_branch = self.pending_branch, None
            if end_after is not None:
                self.tick()
                break
            if hi & 0x40000000:
                end_after = True
            pc = npc
            self.tick()
        # drain flags
        self.tick(16)
        return pc

    pending_branch = None

    def lower(self, ins, pc):
        """Executes the lower instruction's reads now; returns write thunks."""
        bk = self.vi_backup
        self.vi_backup = None
        if ins in (0, 0x8000033C):
            return None
        op = (ins >> 25) & 0x7F
        dest = (ins >> 21) & 0xF
        ft, fs = (ins >> 16) & 31, (ins >> 11) & 31
        it, is_, id_ = (ins >> 16) & 15, (ins >> 11) & 15, (ins >> 6) & 15
        imm11 = ins & 0x7FF
        if imm11 & 0x400:
            imm11 -= 0x800
        sx = lambda v: v - 0x10000 if v & 0x8000 else v  # noqa: E731
        writes = []

        def set_vf(reg, words, d, lat=4):
            def w():
                if reg:
                    for c in range(4):
                        if d & LANE[c]:
                            self.vf[reg][c] = words[c] & 0xFFFFFFFF
                            self.vf_ready[reg][c] = self.cycle + lat
            writes.append(w)

        def set_vi(reg, v, lat=1, ialu=False):
            old = self.vi[reg]

            def w():
                if reg:
                    self.vi[reg] = v & 0xFFFF
                    self.vi_ready[reg] = self.cycle + lat
                    if ialu:
                        self.vi_backup = (reg, old)
            writes.append(w)

        def brvi(reg):
            if bk and bk[0] == reg:
                return bk[1]
            return self.vi[reg]

        def branch_to(target):
            self.pending_branch = target & 0x3FFF

        if op == 0x00:  # LQ
            self.wait_vi(is_)
            set_vf(ft, self.ld(sx(self.vi[is_]) + imm11), dest)
        elif op == 0x01:  # SQ
            self.wait_vi(it)
            self.wait_vf(fs, dest)
            self.st(sx(self.vi[it]) + imm11, self.vf[fs], dest)
        elif op == 0x04:  # ILW
            self.wait_vi(is_)
            words = self.ld(sx(self.vi[is_]) + imm11)
            c = next(k for k in range(4) if dest & LANE[k]) if dest else 3
            set_vi(it, words[c], 4)
        elif op == 0x05:  # ISW
            self.wait_vi(is_)
            self.wait_vi(it)
            v = self.vi[it]
            self.st(sx(self.vi[is_]) + imm11, [v] * 4, dest)
        elif op in (0x08, 0x09):  # IADDIU / ISUBIU
            self.wait_vi(is_)
            imm15 = ((ins >> 10) & 0x7800) | (ins & 0x7FF)
            set_vi(it, self.vi[is_] + (imm15 if op == 0x08 else -imm15), 1, True)
        elif op in (0x10, 0x12, 0x13):  # FCEQ FCAND FCOR
            imm = ins & 0xFFFFFF
            c = self.clip & 0xFFFFFF
            v = (c == imm) if op == 0x10 else ((c & imm) != 0) if op == 0x12 else ((c | imm) == 0xFFFFFF)
            set_vi(1, 1 if v else 0)
        elif op == 0x11:  # FCSET
            self.work_clip = ins & 0xFFFFFF
            self.flag_q = [e if not (e[1] == "clip" and e[0] == self.cycle + 4) else (e[0], "none", 0) for e in self.flag_q]
            self.flag_q.append((self.cycle + 4, "clip", self.work_clip))
        elif op in (0x14, 0x16, 0x17):  # FSEQ FSAND FSOR
            imm = (((ins >> 21) & 1) << 11) | (ins & 0x7FF)
            s = self.status & 0xFFF
            v = (1 if s == imm else 0) if op == 0x14 else (s & imm) if op == 0x16 else (s | imm)
            set_vi(it, v)
        elif op == 0x15:  # FSSET
            imm = (((ins >> 21) & 1) << 11) | (ins & 0x7FF)
            self.flag_q = [(e[0], "fmac", (e[2][0], 0, 0)) if (e[1] == "fmac" and e[0] == self.cycle + 4) else e for e in self.flag_q]
            self.flag_q.append((self.cycle + 4, "fsset", imm & 0xFC0))
        elif op in (0x18, 0x1A, 0x1B):  # FMEQ FMAND FMOR
            self.wait_vi(is_)
            m, v = self.mac & 0xFFFF, self.vi[is_]
            set_vi(it, (1 if m == v else 0) if op == 0x18 else (m & v) if op == 0x1A else (m | v))
        elif op == 0x1C:  # FCGET
            set_vi(it, self.clip & 0xFFF)
        elif op == 0x20:
            branch_to(pc + 8 + imm11 * 8)
        elif op == 0x21:
            set_vi(it, (pc + 16) // 8)
            branch_to(pc + 8 + imm11 * 8)
        elif op in (0x24, 0x25):
            self.wait_vi(is_)
            if op == 0x25:
                set_vi(it, (pc + 16) // 8)
            branch_to(brvi(is_) * 8)
        elif op in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            self.wait_vi(is_)
            self.wait_vi(it)
            a, b = sx(brvi(is_)), sx(brvi(it))
            take = {0x28: a == b, 0x29: a != b, 0x2C: a < 0, 0x2D: a > 0, 0x2E: a <= 0, 0x2F: a >= 0}[op]
            if take:
                branch_to(pc + 8 + imm11 * 8)
        elif op == 0x40:
            fn = ins & 0x3F
            if fn in (0x30, 0x31, 0x34, 0x35):
                self.wait_vi(is_)
                self.wait_vi(it)
                a, b = self.vi[is_], self.vi[it]
                v = {0x30: a + b, 0x31: a - b, 0x34: a & b, 0x35: a | b}[fn]
                set_vi(id_, v, 1, True)
            elif fn == 0x32:
                self.wait_vi(is_)
                imm5 = (ins >> 6) & 31
                imm5 = imm5 - 32 if imm5 & 16 else imm5
                set_vi(it, self.vi[is_] + imm5, 1, True)
            elif fn >= 0x3C:
                f2 = (ins & 3) | ((ins >> 4) & 0x7C)
                fsf, ftf = (ins >> 21) & 3, (ins >> 23) & 3
                if f2 == 0x30:  # MOVE
                    self.wait_vf(fs, dest)
                    set_vf(ft, list(self.vf[fs]), dest)
                elif f2 == 0x31:  # MR32
                    self.wait_vf(fs, 0xF)
                    v = self.vf[fs]
                    set_vf(ft, [v[1], v[2], v[3], v[0]], dest)
                elif f2 in (0x34, 0x36):  # LQI LQD
                    self.wait_vi(is_)
                    a = self.vi[is_] if f2 == 0x34 else (self.vi[is_] - 1) & 0xFFFF
                    set_vf(ft, self.ld(a), dest)
                    set_vi(is_, a + 1 if f2 == 0x34 else a, 1, True)
                elif f2 in (0x35, 0x37):  # SQI SQD
                    self.wait_vi(it)
                    self.wait_vf(fs, dest)
                    a = self.vi[it] if f2 == 0x35 else (self.vi[it] - 1) & 0xFFFF
                    self.st(a, self.vf[fs], dest)
                    set_vi(it, a + 1 if f2 == 0x35 else a, 1, True)
                elif f2 in (0x38, 0x39, 0x3A):  # DIV SQRT RSQRT
                    if self.q_pending:
                        self.tick(self.q_pending[0] - self.cycle)
                    t = u2f(self.vf[ft][ftf])
                    s = u2f(self.vf[fs][fsf])
                    self.wait_vf(ft, LANE[ftf])
                    if f2 != 0x39:
                        self.wait_vf(fs, LANE[fsf])
                    di = 0
                    if f2 == 0x38:
                        if t == 0:
                            di = 0x10 if s == 0 else 0x20
                            v = (0x80000000 if (math.copysign(1, s) * math.copysign(1, t)) < 0 else 0) | MAXF
                        else:
                            v = rz_div(s, t)
                    elif f2 == 0x39:
                        di = 0x10 if t < 0 else 0
                        v = rzf(math.sqrt(abs(t)))
                    else:
                        d = math.sqrt(abs(t))
                        di = 0x10 if t < 0 else 0
                        if d == 0:
                            di = 0x10 if s == 0 else 0x20
                            v = (0x80000000 if math.copysign(1, s) < 0 else 0) | MAXF
                        else:
                            v = rz_div(s, raw2f(rzf(d)))
                    self.q_pending = (self.cycle + (13 if f2 == 0x3A else 7), v, di)
                elif f2 == 0x3B:  # WAITQ
                    if self.q_pending:
                        self.tick(self.q_pending[0] - self.cycle)
                elif f2 == 0x3C:  # MTIR
                    self.wait_vf(fs, LANE[fsf])
                    set_vi(it, self.vf[fs][fsf] & 0xFFFF, 1, True)
                elif f2 == 0x3D:  # MFIR
                    self.wait_vi(is_)
                    v = sx(self.vi[is_]) & 0xFFFFFFFF
                    set_vf(ft, [v] * 4, dest)
                elif f2 in (0x3E, 0x3F):  # ILWR ISWR
                    self.wait_vi(is_)
                    if f2 == 0x3E:
                        words = self.ld(self.vi[is_])
                        c = next(k for k in range(4) if dest & LANE[k]) if dest else 3
                        set_vi(it, words[c], 4)
                    else:
                        self.wait_vi(it)
                        self.st(self.vi[is_], [self.vi[it]] * 4, dest)
                elif f2 == 0x68:
                    set_vi(it, self.top & 0x3FF)
                elif f2 == 0x69:
                    set_vi(it, self.itop & 0x3FF)
                elif f2 == 0x6C:  # XGKICK
                    self.wait_vi(is_)
                    self.packets.append(self.gif_packet(self.vi[is_]))
                else:
                    raise RuntimeError("lower2 %02x" % f2)
            else:
                raise RuntimeError("lower40 %08x" % ins)
        else:
            raise RuntimeError("lower %08x" % ins)
        return writes

    def gif_packet(self, qaddr):
        out = bytearray()
        a = qaddr & 0x3FF
        while True:
            tag = bytes(self.mem[a * 16:a * 16 + 16])
            out += tag
            a = (a + 1) & 0x3FF
            lo = struct.unpack_from("<Q", tag)[0]
            nloop, eop, flg = lo & 0x7FFF, (lo >> 15) & 1, (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            q = nloop * nreg if flg == 0 else ((nloop * nreg + 1) // 2 if flg == 1 else nloop)
            for _ in range(q):
                out += self.mem[a * 16:a * 16 + 16]
                a = (a + 1) & 0x3FF
            if eop:
                return bytes(out)


def main():
    args = sys.argv[1:]
    path = args[0]
    first = int(args[args.index("--first") + 1]) if "--first" in args else 0
    count = int(args[args.index("--count") + 1]) if "--count" in args else 200
    want_pc = int(args[args.index("--pc") + 1], 0) if "--pc" in args else None
    fused = "--fused" in args
    verbose = "--verbose" in args
    image = bytearray(0x4000)
    checked = same = 0
    for r in vu1corpus.read_corpus(path):
        for idx, b in r["data_in"]:
            image[idx * 16:idx * 16 + 16] = b
        data_in = bytes(image)
        for idx, b in r["data_out"]:
            image[idx * 16:idx * 16 + 16] = b
        if r["seq"] < first:
            continue
        if want_pc is not None and r["start"] != want_pc:
            continue
        if checked >= count:
            break
        st = vu1corpus.parse_state(r["state_in"])
        sim = Sim(r["code"], data_in, st, r["top"], r["itop"], fused)
        try:
            sim.run(r["start"] if r["kind"] == 0 else st["pc"])
        except Exception as e:  # noqa: BLE001
            print("run %d pc 0x%x: simulator stopped: %s" % (r["seq"], r["start"], e))
            checked += 1
            continue
        checked += 1
        ok_mem = bytes(sim.mem) == bytes(image)
        ok_pk = sim.packets == r["packets"]
        if ok_mem and ok_pk:
            same += 1
            continue
        print("run %d pc 0x%x: memory %s, packets %s (%d sim / %d recorded)" % (
            r["seq"], r["start"], "same" if ok_mem else "DIFFERS", "same" if ok_pk else "DIFFER",
            len(sim.packets), len(r["packets"])))
        if verbose:
            for i, (a, b) in enumerate(zip(sim.packets, r["packets"])):
                if a != b:
                    for o in range(0, min(len(a), len(b)), 16):
                        if a[o:o + 16] != b[o:o + 16]:
                            print("  packet %d qword %d sim %s rec %s" % (i, o // 16, a[o:o + 16].hex(), b[o:o + 16].hex()))
                            break
                    break
            if not ok_mem:
                for q in range(0x400):
                    if sim.mem[q * 16:q * 16 + 16] != image[q * 16:q * 16 + 16]:
                        print("  qword 0x%x sim %s rec %s" % (q, sim.mem[q * 16:q * 16 + 16].hex(), image[q * 16:q * 16 + 16].hex()))
                        break
    print("%d runs simulated, %d identical" % (checked, same))


if __name__ == "__main__":
    main()
