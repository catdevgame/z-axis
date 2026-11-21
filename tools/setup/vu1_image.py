#!/usr/bin/env python3
"""vu1_image.py: a game's VU1 micro memory image, the code its renderer uploads to VU1, from the game's ELF.

    vu1_image.py --game bmx --elf SLUS_204.15 --out-dir RECOMP/generated-vu1

The build makes the VU1 ahead-of-time code (ps2/CMakeLists.txt, ZX_VU1_AOT_DIR and
ZX_VU1_AOT_EXTRA_DIRS) from the micro memory images in a folder, <hash>.vu1code: the 16 KiB VU1 runs,
named by its FNV-1a hash (VU1Interpreter::codeHash), which the generated code registers under. A run
with ZX_VU1_CODE_DUMP writes the same file (tools/VU1_AOT.md); this one needs no run.

The renderer's set-up sends VU1 its microprograms once (loadMicroprograms, initRenderer):
dmaSendChain(1, codeEnd, 837, 0), a DMA chain with CHCR.TTE (0x40 of 837), so VIF1 gets each tag's
upper 64 bits as two VIF codes, then the tag's data. The chain is walked as vu0_upload.py walks VU0's
(chain_stream), and its MPG codes are written into the micro memory, which is empty before them. Each
game's codeEnd is AI's symbol (tools/readable_symbols.tsv) through the game's bind table (tools/bind).
The image is the one each game runs from then on: AI's 6a07ea435fe244b3, DM2's 836c2d1891f4ba03 and
BMX XXX's cbcf1983f3f877ad are what their runs dumped.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# (the symbol tables' reader: tools/readable)
sys.path.insert(1, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'readable'))
import readable_symbols  # noqa: E402
from vu0_upload import chain_stream  # noqa: E402

MICRO_MEMORY = 0x4000   # VU1's code memory: 2,048 instruction pairs of 8 bytes
CHAIN = 'codeEnd'       # the microprograms' chain (zge/render/ps2/frame_packets.cpp kMicroprogramChain)
TTE = True              # dmaSendChain's 837: CHCR.TTE
VIF_NOP, VIF_MPG = 0x00, 0x4A
GAMES = ('ai', 'dm2', 'bmx')


def chain_address(game):
    """The game's address of the microprograms' chain."""
    entry = readable_symbols.table().syms.get(CHAIN)
    if not entry or entry[0] != 'data':
        raise SystemExit('vu1_image: no data row %s in readable_symbols.tsv' % CHAIN)
    if game == 'ai':
        return entry[1]
    bind = readable_symbols.game_bind(game)
    address = bind.datum(entry[1]) if bind else None
    if address is None:
        raise SystemExit('vu1_image: %s\'s bind table has no %s (AI 0x%08X)' % (game, CHAIN, entry[1]))
    return address


def micro_memory(stream):
    """VU1's code memory after VIF1 took STREAM: its MPG codes' instructions, the rest zero."""
    memory = bytearray(MICRO_MEMORY)
    at = 0
    while at < len(stream):
        code, = struct.unpack_from('<I', stream, at)
        at += 4
        command, num, immediate = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        if command == VIF_MPG:
            size = (num or 256) * 8
            start = immediate * 8
            if start + size > MICRO_MEMORY or at + size > len(stream):
                raise SystemExit('vu1_image: the MPG at stream byte %d goes past the micro memory or the stream' % (at - 4))
            memory[start:start + size] = stream[at:at + size]
            at += size
        elif command != VIF_NOP:
            raise SystemExit('vu1_image: VIF code 0x%08X at stream byte %d: an upload is NOPs and MPGs' % (code, at - 4))
    return bytes(memory)


def code_hash(image):
    """VU1Interpreter::codeHash: FNV-1a, 64 bits."""
    value = 0xCBF29CE484222325
    for byte in image:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True, choices=GAMES)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--out-dir', required=True, help='the folder the image goes in, as <hash>.vu1code')
    args = ap.parse_args()
    address = chain_address(args.game)
    image = micro_memory(chain_stream(readable_symbols.Elf(args.elf), address, TTE))
    name = '%016x.vu1code' % code_hash(image)
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, name), 'wb') as f:
        f.write(image)
    print('vu1_image: %s: %s from %s at 0x%08X' % (args.game, name, CHAIN, address))


if __name__ == '__main__':
    main()
