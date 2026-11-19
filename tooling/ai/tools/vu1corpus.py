#!/usr/bin/env python3
"""Read a VU1 run corpus (PS2X_VU1_RECORD, see PS2Recomp ps2_vu1_record.h).

Usage:
  vu1corpus.py summary <corpus>            runs per program, packets, vertices
  vu1corpus.py negq <corpus> [limit]       drawn vertices with Q < 0 per program
  vu1corpus.py runs <corpus> [pc]          list runs (optionally of one MSCAL pc)

The program of an MSCNT run is the MSCAL pc that started the chain.
"""
import struct
import sys
from collections import Counter, defaultdict

STATE_SIZE = 664
STATE_FMT = "<128I16i4I8IQQ8B4I"


def parse_state(raw):
    v = struct.unpack(STATE_FMT, raw)
    vf = [list(v[i * 4:i * 4 + 4]) for i in range(32)]
    vi = list(v[128:144])
    acc = list(v[144:148])
    q, p, i, r, pc, mac, clip, status = v[148:156]
    cycles, kicks = v[156:158]
    return dict(vf=vf, vi=vi, acc=acc, q=q, p=p, i=i, r=r, pc=pc, mac=mac, clip=clip, status=status,
                cycles=cycles, kicks=kicks)


def read_corpus(path, want_data=False):
    with open(path, "rb") as f:
        magic = f.read(8)
        assert magic == b"PS2XVU1R", "not a VU1 corpus"
        (version,) = struct.unpack("<I", f.read(4))
        codes = []
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                return
            typ, size = struct.unpack("<II", hdr)
            payload = f.read(size)
            if typ == 1:
                codes.append(payload)
                continue
            if typ != 2:
                continue
            o = 0
            seq, vsync = struct.unpack_from("<QQ", payload, o); o += 16
            kind, start, top, itop, maxc, code_id, idle = struct.unpack_from("<7I", payload, o); o += 28
            sin = payload[o:o + STATE_SIZE]; o += STATE_SIZE
            sout = payload[o:o + STATE_SIZE]; o += STATE_SIZE
            deltas = []
            for _ in range(2):
                (n,) = struct.unpack_from("<I", payload, o); o += 4
                d = []
                for _ in range(n):
                    (idx,) = struct.unpack_from("<I", payload, o); o += 4
                    d.append((idx, payload[o:o + 16])); o += 16
                deltas.append(d)
            (np_,) = struct.unpack_from("<I", payload, o); o += 4
            packets = []
            for _ in range(np_):
                (sz,) = struct.unpack_from("<I", payload, o); o += 4
                packets.append(payload[o:o + sz]); o += sz
            yield dict(seq=seq, vsync=vsync, kind=kind, start=start, top=top, itop=itop, code_id=code_id,
                       idle=idle, state_in=sin, state_out=sout, data_in=deltas[0], data_out=deltas[1],
                       packets=packets, code=codes[code_id])


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def gif_vertices(packet):
    """Yield (prim, regs) for every XYZ2/XYZF2 in PACKED tags: dict with q, x, y, z, adc."""
    o = 0
    prim = None
    while o + 16 <= len(packet):
        lo, hi = struct.unpack_from("<QQ", packet, o); o += 16
        nloop = lo & 0x7FFF
        eop = (lo >> 15) & 1
        pre = (lo >> 46) & 1
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        if pre:
            prim = (lo >> 47) & 0x7FF
        regs = [(hi >> (4 * k)) & 0xF for k in range(nreg)]
        if flg == 0:
            q = None
            for _ in range(nloop):
                for r in regs:
                    w = struct.unpack_from("<4I", packet, o); o += 16
                    if r == 0x0:
                        prim = w[0] & 0x7FF
                    elif r == 0x2:
                        q = f32(w[2])
                    elif r in (0x4, 0x5):
                        x, y = w[0] & 0xFFFF, w[1] & 0xFFFF
                        if r == 0x4:
                            z = (w[2] >> 4) & 0xFFFFFF
                        else:
                            z = w[2]
                        adc = (w[3] >> 15) & 1
                        yield prim, dict(q=q, x=x, y=y, z=z, adc=adc, off=o - 16)
                    elif r == 0xE:
                        pass
        elif flg == 1:
            o += ((nloop * nreg + 1) & ~1) * 8
        else:
            o += nloop * 16
        if eop:
            break


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    chain_pc = None
    if cmd == "summary":
        runs = Counter(); verts = Counter(); pk = Counter()
        for r in read_corpus(path):
            if r["kind"] == 0:
                chain_pc = r["start"]
            runs[chain_pc] += 1
            pk[chain_pc] += len(r["packets"])
            for p in r["packets"]:
                for prim, v in gif_vertices(p):
                    verts[chain_pc] += 1
        for pc, n in sorted(runs.items(), key=lambda x: -verts[x[0]]):
            print("prog 0x%04x runs %6d packets %6d vertices %8d" % (pc, n, pk[pc], verts[pc]))
    elif cmd == "negq":
        limit = int(sys.argv[3]) if len(sys.argv) > 3 else 5
        neg = Counter(); total = Counter(); examples = defaultdict(list)
        for r in read_corpus(path):
            if r["kind"] == 0:
                chain_pc = r["start"]
            for pi, p in enumerate(r["packets"]):
                for prim, v in gif_vertices(p):
                    if v["adc"]:
                        continue
                    total[chain_pc] += 1
                    if v["q"] is not None and v["q"] < 0:
                        neg[chain_pc] += 1
                        if len(examples[chain_pc]) < limit:
                            examples[chain_pc].append((r["seq"], r["start"], r["kind"], pi, prim, v))
        for pc in sorted(total, key=lambda k: -neg[k]):
            print("prog 0x%04x drawn vertices %8d with q<0 %7d" % (pc, total[pc], neg[pc]))
            for e in examples[pc]:
                print("   run %d (%s 0x%x) packet %d prim 0x%x %s" % (e[0], "MSCAL" if e[2] == 0 else "MSCNT",
                                                                    e[1], e[3], e[4] or 0, e[5]))
    elif cmd == "runs":
        want = int(sys.argv[3], 0) if len(sys.argv) > 3 else None
        for r in read_corpus(path):
            if r["kind"] == 0:
                chain_pc = r["start"]
            if want is not None and chain_pc != want:
                continue
            si, so = parse_state(r["state_in"]), parse_state(r["state_out"])
            print("run %d vsync %d %s 0x%x prog 0x%x top 0x%x itop 0x%x cycles %d end_pc 0x%x packets %d din %d dout %d" % (
                r["seq"], r["vsync"], "MSCAL" if r["kind"] == 0 else "MSCNT", r["start"], chain_pc, r["top"],
                r["itop"], so["cycles"] - si["cycles"], so["pc"], len(r["packets"]), len(r["data_in"]),
                len(r["data_out"])))


if __name__ == "__main__":
    main()
