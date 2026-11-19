#!/usr/bin/env python3
"""vu0_upload.py: a game's VU0 upload, the VIF0 stream its boot sends VU0, from the game's ELF.

    vu0_upload.py --game bmx --elf SLUS_204.15 --out GENERATED_DIR/vu0_upload.vif

ps2xFnCheck replays recorded calls with VU0's code memory as the game had uploaded it (ee/fncheck.h
Vu0Upload): it sends this stream through the runtime's VIF0 (PS2Memory::processVIF0Data) before its
replays, so a VCALLMS runs the game's microprogram. game.cmake builds a game's vu0_upload.vif in from
the folder of its generated code, which goes with the generated code to every host that builds the
checker; the ELF does not.

The stream is what the DMAC hands VIF0 for each DMA chain the game sends it, walked as the runtime walks
one (PS2Memory::readDmaChainTag): with CHCR.TTE each tag's upper 64 bits (two VIF codes), then the tag's
data. The chains come from UPLOADS, by their symbols (tools/readable_symbols.tsv).

Beside it, vu0_upload.recomp names the recompile it was made for: the SHA-1 of the folder's
register_functions.cpp, which every recompile of the ELF writes. game.cmake stops the configure when a game
of UPLOADS has generated code without its upload, or with one made for another recompile (the folder was
regenerated, or its upload copied from another), and says to run this again.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_symbols  # noqa: E402

# Each game's uploads to VU0, in the order its boot sends them: (the chain's symbol, CHCR.TTE).
# BMX XXX: initPlatform's last step, loadVu0Microprogram, dmaSendChain(0, vu0MicroprogramChain, 837, 0);
# 837 (0x345) has CHCR.TTE (0x40). AI and DM2 send VU0 nothing (their only VCALLMSR is sceDevVu0Exec,
# which nothing calls).
UPLOADS = {
    'bmx': [('vu0MicroprogramChain', True)],
}

TAG_REFE, TAG_CNT, TAG_NEXT, TAG_REF, TAG_REFS, TAG_CALL, TAG_RET, TAG_END = range(8)
MAX_TAGS = 1 << 16


def chain_stream(elf, address, tte):
    """The bytes the DMAC hands the channel for the source chain at `address`."""
    out = bytearray()
    tadr, stack = address, []
    for _ in range(MAX_TAGS):
        tag = elf.data(tadr, 16)
        if tag is None:
            raise SystemExit('vu0_upload: the chain\'s tag at 0x%X is not in the ELF\'s data' % tadr)
        low, high = struct.unpack_from('<QQ', tag)
        qwc, tag_id = low & 0xFFFF, (low >> 28) & 7
        addr = (low >> 32) & 0x7FFFFFFF
        if low & (1 << 31):
            raise SystemExit('vu0_upload: the tag at 0x%X asks for an interrupt (not in a boot upload)' % tadr)
        if low >> 63:
            raise SystemExit('vu0_upload: the tag at 0x%X points into the scratchpad' % tadr)
        after = tadr + 16
        end = False
        if tag_id == TAG_REFE:
            data, end = addr, True
        elif tag_id == TAG_CNT:
            data, tadr = after, after + qwc * 16
        elif tag_id == TAG_NEXT:
            data, tadr = after, addr
        elif tag_id in (TAG_REF, TAG_REFS):
            data, tadr = addr, after
        elif tag_id == TAG_CALL:
            if len(stack) == 2:
                raise SystemExit('vu0_upload: a third call deep at 0x%X' % tadr)
            data = after
            stack.append(after + qwc * 16)
            tadr = addr
        elif tag_id == TAG_RET:
            data = after
            if stack:
                tadr = stack.pop()
            else:
                end = True
        else:
            data, end = after, True
        if tte:
            out += struct.pack('<Q', high)
        if qwc:
            payload = elf.data(data, qwc * 16)
            if payload is None:
                raise SystemExit('vu0_upload: the data at 0x%X (%d quadwords) is not in the ELF\'s data' % (data, qwc))
            out += payload
        if end:
            return bytes(out)
    raise SystemExit('vu0_upload: the chain at 0x%X has no end in %d tags' % (address, MAX_TAGS))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True, choices=sorted(UPLOADS))
    ap.add_argument('--elf', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    table = readable_symbols.Table().own(args.game)
    elf = readable_symbols.Elf(args.elf)
    stream = bytearray()
    for symbol, tte in UPLOADS[args.game]:
        if symbol not in table.syms:
            raise SystemExit('vu0_upload: no %s row %s in readable_symbols.tsv' % (args.game, symbol))
        stream += chain_stream(elf, table.syms[symbol][1], tte)
    folder = os.path.dirname(os.path.abspath(args.out))
    registrar = os.path.join(folder, 'register_functions.cpp')
    if not os.path.isfile(registrar):
        raise SystemExit('vu0_upload: %s is not a generated code folder (no register_functions.cpp)' % folder)
    with open(args.out, 'wb') as f:
        f.write(stream)
    with open(registrar, 'rb') as f:
        recompile = hashlib.sha1(f.read()).hexdigest()
    with open(os.path.join(folder, 'vu0_upload.recomp'), 'w') as f:
        f.write(recompile + '\n')
    print('vu0_upload: %s: %d bytes from %s' % (args.game, len(stream), ', '.join(s for s, _ in UPLOADS[args.game])))


if __name__ == '__main__':
    main()
