// LZO1X decompression (collection/extras/lzo1x.h): a port of LZO 2.10's
// lzo1x_decompress_safe, lzo-2.10/src/lzo1x_d.ch built with LZO_SAFE (the
// byte-wise copies; no dictionary, not LZO1Y).
//
// lzo1x_d.ch -- implementation of the LZO1X decompression algorithm
//
// This file is part of the LZO real-time data compression library.
//
// Copyright (C) 1996-2017 Markus Franz Xaver Johannes Oberhumer
// All Rights Reserved.
//
// The LZO library is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// The LZO library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// Markus F.X.J. Oberhumer
// http://www.oberhumer.com/opensource/lzo/
// (the notice's e-mail address left out here: the repository's scan keeps
// addresses out of tracked files; it is in lzo1x_d.ch.)
#include "lzo1x.h"

namespace zx::app::extras
{
    namespace
    {
        constexpr size_t kM2MaxOffset = 0x0800; // M2_MAX_OFFSET
        constexpr size_t kM4Base = 0x4000;      // a far match's distance starts here
    }

    bool lzo1xDecompress(const uint8_t *in, size_t size, size_t most, std::vector<uint8_t> &out)
    {
        out.clear();
        out.reserve(most);
        size_t ip = 0;
        size_t t = 0;
        size_t matchAt = 0; // m_pos: where in `out` a match copies from
        // NEED_IP, NEED_OP and TEST_LB of the safe build.
        const auto haveIn = [&](size_t n) { return size - ip >= n; };
        const auto haveOut = [&](size_t n) { return most - out.size() >= n; };
        const auto copyIn = [&](size_t n)
        {
            out.insert(out.end(), in + ip, in + ip + n);
            ip += n;
        };
        // A match's distance back from the output's end, checked (TEST_LB).
        const auto matchBack = [&](size_t distance)
        {
            if (distance > out.size())
                return false;
            matchAt = out.size() - distance;
            return true;
        };
        const auto copyMatch = [&](size_t n)
        {
            for (size_t i = 0; i < n; ++i)
                out.push_back(out[matchAt + i]);
        };
        // A long length: 255 for each zero byte, then the byte that ends them
        // (TEST_IV / TEST_OV: such a run cannot outgrow the input).
        const auto longLength = [&](size_t base, size_t &length)
        {
            length = 0;
            while (ip < size && in[ip] == 0)
            {
                length += 255;
                ++ip;
            }
            if (!haveIn(1))
                return false;
            length += base + in[ip++];
            return true;
        };

        if (!haveIn(1))
            return false;
        if (in[ip] > 17)
        {
            t = in[ip++] - 17u;
            if (t < 4)
                goto match_next;
            if (!haveOut(t) || !haveIn(t + 3))
                return false;
            copyIn(t);
            goto first_literal_run;
        }
        for (;;)
        {
            if (!haveIn(3))
                return false;
            t = in[ip++];
            if (t >= 16)
                goto match;
            // a literal run
            if (t == 0 && !longLength(15, t))
                return false;
            if (!haveOut(t + 3) || !haveIn(t + 6))
                return false;
            copyIn(t + 3);

        first_literal_run:
            t = in[ip++];
            if (t >= 16)
                goto match;
            // a 3-byte match after a literal run, up to 0xC00 back
            {
                const size_t distance = (1 + kM2MaxOffset) + (t >> 2) + (size_t(in[ip++]) << 2);
                if (!matchBack(distance) || !haveOut(3))
                    return false;
                copyMatch(3);
            }
            goto match_done;

            // the matches
            for (;;)
            {
            match:
                if (t >= 64) // M2: 3 to 8 bytes, up to 0x800 back
                {
                    const size_t distance = 1 + ((t >> 2) & 7) + (size_t(in[ip++]) << 3);
                    t = (t >> 5) - 1;
                    if (!matchBack(distance) || !haveOut(t + 2))
                        return false;
                    copyMatch(t + 2);
                    goto match_done;
                }
                else if (t >= 32) // M3: up to 0x4000 back
                {
                    t &= 31;
                    if (t == 0 && !longLength(31, t))
                        return false;
                    if (!haveIn(2))
                        return false;
                    const size_t distance = 1 + (in[ip] >> 2) + (size_t(in[ip + 1]) << 6);
                    ip += 2;
                    if (!matchBack(distance))
                        return false;
                }
                else if (t >= 16) // M4: 0x4000 to 0xBFFF back, or the end
                {
                    size_t distance = (t & 8) << 11;
                    t &= 7;
                    if (t == 0 && !longLength(7, t))
                        return false;
                    if (!haveIn(2))
                        return false;
                    distance += (in[ip] >> 2) + (size_t(in[ip + 1]) << 6);
                    ip += 2;
                    if (distance == 0)
                        goto eof_found;
                    if (!matchBack(distance + kM4Base))
                        return false;
                }
                else // M1: 2 bytes, up to 0x400 back
                {
                    const size_t distance = 1 + (t >> 2) + (size_t(in[ip++]) << 2);
                    if (!matchBack(distance) || !haveOut(2))
                        return false;
                    copyMatch(2);
                    goto match_done;
                }
                // copy match: t + 2 bytes (byte by byte: it may overlap itself)
                if (!haveOut(t + 2))
                    return false;
                copyMatch(t + 2);

            match_done:
                // The low two bits of the byte two back: 0 to 3 literals follow.
                t = in[ip - 2] & 3u;
                if (t == 0)
                    break;
            match_next:
                if (!haveOut(t) || !haveIn(t + 3))
                    return false;
                copyIn(t);
                t = in[ip++];
            }
        }

    eof_found:
        // Bytes may follow the end marker (LZO_E_INPUT_NOT_CONSUMED): BMX
        // XXX's ZLZO chunks are padded to four bytes.
        return true;
    }
}
