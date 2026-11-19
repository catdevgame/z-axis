#pragma once

// LZO1X decompression on the host, for the extras' readers (BMX XXX's Xbox
// texture libraries keep each texture as a ZLZO chunk: the unpacked size,
// then an LZO1X stream). Ported from LZO 2.10's lzo1x_decompress_safe
// (lzo-2.10/src/lzo1x_d.ch with LZO_SAFE: every read and write checked), as
// NOTICE says. The game's own decoder of the same format (the engine's file
// method 2) is zge's lzoDecompress, which works on the game's memory.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::app::extras
{
    // The stream `in` unpacked into `out` (resized to what it holds). False
    // when the stream is cut short, reaches before its output's start or
    // would write more than `most` bytes; bytes after its end marker are
    // left (lzo1x_decompress_safe's LZO_E_INPUT_NOT_CONSUMED).
    bool lzo1xDecompress(const uint8_t *in, size_t size, size_t most, std::vector<uint8_t> &out);
}
