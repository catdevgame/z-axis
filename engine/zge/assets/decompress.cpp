// File decompression: the LZ formats the engine's files use (and the
// encoder of method 1, which nothing in the game calls).



#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/assets/zfile_library.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/decompress.cpp.h"

namespace zge
{

    namespace
    {
        // A method 1 block: a word of header (its first byte kLzStored: the
        // rest is the data as it is), then runs of 16 tokens, each run after a
        // word whose bits say which are matches.
        constexpr uint32_t kLzHeaderBytes = 4;
        constexpr uint8_t kLzStored = 1;
        constexpr uint32_t kLzTokensInRun = 16;
        constexpr uint32_t kLzCheckEachTokenBytes = 0x20; // this near the end the run may stop short
        constexpr uint32_t kLzShortestMatch = 3;
        constexpr uint32_t kLzLongestMatch = kLzShortestMatch + 0xF;
        constexpr uint8_t kLzCompressed = 0;
        constexpr uint32_t kLzWorkBytes = 0x4010;

        // What the decoder has seen: 4096 places in sets of eight, each set
        // for a hash of three bytes; a match names a place. Places are
        // filled in turn across the sets, as the compressor filled its own.
        constexpr uint32_t kLzPlaces = 0x1000;
        constexpr uint32_t kLzSetMask = 0xFF8;
        constexpr uint32_t kLzWordMask = 7;
        constexpr uint32_t kLzHashFactor = 0x9E5F;
        struct LzHistory
        {
            ee::Ptr<ee::Ptr<uint8_t>> places;
            ee::Ptr<uint8_t> output;
            uint32_t turn = 0;

            void remember(uint32_t set, uint32_t position)
            {
                places[set + turn] = output + position;
                turn = (turn + 1) & kLzWordMask;
            }
            // The three bytes at `position` go in the set they hash to.
            void rememberBytes(uint32_t position)
            {
                const uint32_t bytes = uint32_t{output[position]} << 8 ^ uint32_t{output[position + 1]} << 4 ^ output[position + 2];
                remember((bytes * kLzHashFactor) >> 1 & kLzSetMask, position);
            }
        };
    }

    void copyByteRun(ee::Ptr<uint8_t> from, ee::Ptr<uint8_t> to, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            to[i] = from[i];
    }
    READABLE_FUNCTION(copyByteRun, Verified);

    // Method 1's decoder. `work` is 0x4010 bytes for the dictionary.
    void lzDecompress(uint32_t work, ee::Ptr<uint8_t> source, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        if (source[0] == kLzStored)
        {
            const uint32_t stored = bytes - kLzHeaderBytes;
            copyByteRun(source + kLzHeaderBytes, output, stored);
            *bytesOut = stored;
            return;
        }

        LzHistory history{ee::Ptr<ee::Ptr<uint8_t>>((work + 3) & ~3u), output};
        for (uint32_t i = 0; i < kLzPlaces; ++i)
            history.places[i] = ee::Ptr<uint8_t>(sym::g_lzEmptyMatch);

        uint32_t in = kLzHeaderBytes;
        uint32_t written = 0;
        uint32_t tokenBits = 1;   // a run's bits, over a marker: 1 when they are used up
        uint32_t unhashed = 0;    // literals not in the dictionary yet
        while (in != bytes)
        {
            if (tokenBits == 1)
            {
                tokenBits = 0x10000u | source[in] | uint32_t{source[in + 1]} << 8;
                in += 2;
            }
            const bool nearEnd = static_cast<int32_t>(in) > static_cast<int32_t>(bytes - kLzCheckEachTokenBytes);
            for (uint32_t token = nearEnd ? 1 : kLzTokensInRun; token != 0; --token)
            {
                if ((tokenBits & 1) == 0)
                {
                    output[written] = source[in];
                    ++written;
                    ++in;
                    if (++unhashed == kLzShortestMatch)
                    {
                        history.rememberBytes(written - kLzShortestMatch);
                        unhashed = kLzShortestMatch - 1;
                    }
                }
                else
                {
                    const uint32_t lengthAndPlace = source[in];
                    const uint32_t place = (lengthAndPlace & 0xF0u) << 4 | source[in + 1];
                    in += 2;
                    const uint32_t start = written;
                    const ee::Ptr<uint8_t> match = history.places[place];
                    const uint32_t length = kLzShortestMatch + (lengthAndPlace & 0xFu);
                    for (uint32_t i = 0; i < length; ++i)
                        output[written++] = match[i];
                    for (; unhashed != 0; --unhashed)
                        history.rememberBytes(start - unhashed);
                    history.remember(place & kLzSetMask, start);
                }
                tokenBits >>= 1;
            }
        }
        *bytesOut = written;
    }
    READABLE_FUNCTION(lzDecompress, Verified);

    // lzDecompress under its other name.
    void lzDecompressAlias(uint32_t work, ee::Ptr<uint8_t> source, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        game::tail(lzDecompress, work, source, bytes, output, bytesOut);
    }
    READABLE_FUNCTION(lzDecompressAlias, Draft);

    // lzCompress: method 1's encoder, lzDecompress's other half (the scheme
    // is Ross Williams' LZRW3-A). The header word's first byte says
    // compressed; then runs of 16 tokens, each run after the word of their
    // bits (1: a match). A token at `in` hashes its three bytes to a set of
    // eight places and takes the longest of their matches (up to 18 bytes),
    // or the byte as it is when that is under 3. The places are filled as
    // the decoder fills its own. Output that would grow past the input's size
    // is given up: the header's first byte says stored, and the input follows
    // as it is. bytesOut gets the size written.
    void lzCompress(uint32_t work, ee::Ptr<uint8_t> input, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        constexpr uint32_t kRunDone = 0xFFFF0000u; // the token bits' marker: gone after 16 shifts
        constexpr uint32_t kLiteralBit = 0x10000u;
        const auto storeAsIs = [&]
        {
            output[0] = kLzStored;
            copyByteRun(input, output + kLzHeaderBytes, bytes);
            *bytesOut = bytes + kLzHeaderBytes;
        };

        const ee::Ptr<ee::Ptr<uint8_t>> places((work + 3) & ~3u);
        for (uint32_t i = 0; i < kLzHeaderBytes; ++i)
            output[i] = kLzCompressed;
        uint32_t tokenBitsAt = kLzHeaderBytes;
        uint32_t out = tokenBitsAt + 2;
        const bool noRoom = bytes < out;
        for (uint32_t i = 0; i < kLzPlaces; ++i)
            places[i] = ee::Ptr<uint8_t>(sym::g_lzEmptyMatch);
        if (noRoom)
        {
            storeAsIs();
            return;
        }

        uint32_t tokenBits = kRunDone;
        uint32_t in = 0;
        // The decoder remembers a literal's three bytes once the next two are
        // out: the sets of the last two literals wait here for their place.
        ee::Ptr<ee::Ptr<uint8_t>> set, waiting, waitingLonger;
        uint32_t turn = 0;
        // Where the token started. The compiled code clears it at each run's
        // end, so the last bytes' literals remember address 0 less 2 (in the
        // first set, which nothing reads after).
        ee::Ptr<uint8_t> tokenStart;
        const auto remember = [&](ee::Ptr<ee::Ptr<uint8_t>> inSet, ee::Ptr<uint8_t> position)
        {
            inSet[turn] = position;
            turn = (turn + 1) & kLzWordMask;
        };
        const auto literal = [&]
        {
            output[out] = input[in];
            ++in;
            ++out;
            tokenBits &= ~kLiteralBit;
            if (waitingLonger)
                remember(waitingLonger, tokenStart - 2);
            waitingLonger = waiting;
            waiting = set;
        };
        const auto match = [&](uint32_t place, uint32_t length)
        {
            in += length;
            output[out] = static_cast<uint8_t>((place & 0xF00u) >> 4 | (length - kLzShortestMatch));
            output[out + 1] = static_cast<uint8_t>(place);
            out += 2;
            if (waiting)
            {
                if (waitingLonger)
                {
                    remember(waitingLonger, tokenStart - 2);
                    waitingLonger = ee::Ptr<ee::Ptr<uint8_t>>();
                }
                remember(waiting, tokenStart - 1);
                waiting = ee::Ptr<ee::Ptr<uint8_t>>();
            }
            remember(set, tokenStart);
        };

        for (;;)
        {
            // 16 tokens a run while 16 of the longest fit, then one; the last
            // 17 bytes are literals.
            uint32_t tokens = 1;
            const bool lastBytes = bytes < in + kLzLongestMatch;
            if (in + kLzTokensInRun * kLzLongestMatch <= bytes)
                tokens = kLzTokensInRun;
            else if (lastBytes && in == bytes)
                break;
            for (; tokens != 0; --tokens)
            {
                if (lastBytes)
                {
                    set = places;
                    literal();
                }
                else
                {
                    tokenStart = input + in;
                    const uint32_t three = uint32_t{input[in]} << 8 ^ uint32_t{input[in + 1]} << 4 ^ input[in + 2];
                    const uint32_t firstPlace = (three * kLzHashFactor) >> 1 & kLzSetMask;
                    set = places + firstPlace;
                    uint32_t bestLength = 0, best = 0;
                    for (uint32_t i = 0; i <= kLzWordMask; ++i)
                    {
                        const ee::Ptr<uint8_t> candidate = set[i];
                        if (input[in + bestLength] != candidate[bestLength])
                            continue;
                        uint32_t length = 0;
                        while (length < kLzLongestMatch && candidate[length] == input[in + length])
                            ++length;
                        if (bestLength < length)
                        {
                            bestLength = length;
                            best = i;
                        }
                    }
                    if (bestLength < kLzShortestMatch)
                        literal();
                    else
                        match(firstPlace + best, bestLength);
                }
                tokenBits >>= 1;
            }
            if ((tokenBits & kRunDone) == 0)
            {
                output[tokenBitsAt] = static_cast<uint8_t>(tokenBits);
                output[tokenBitsAt + 1] = static_cast<uint8_t>(tokenBits >> 8);
                tokenBits = kRunDone;
                tokenBitsAt = out;
                out += 2;
            }
            tokenStart = ee::Ptr<uint8_t>();
            if (bytes < out)
            {
                storeAsIs();
                return;
            }
        }

        // The last run's bits, moved down to where 16 tokens would leave
        // them; a bits word with no tokens after it is dropped.
        while (tokenBits & kRunDone)
            tokenBits >>= 1;
        output[tokenBitsAt] = static_cast<uint8_t>(tokenBits);
        output[tokenBitsAt + 1] = static_cast<uint8_t>(tokenBits >> 8);
        *bytesOut = tokenBitsAt + 2 == out ? out - 2 : out;
    }
    READABLE_FUNCTION(lzCompress, Draft);

    // lzCompress under its other name.
    void lzCompressAlias(uint32_t work, ee::Ptr<uint8_t> input, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        game::tail(lzCompress, work, input, bytes, output, bytesOut);
    }
    READABLE_FUNCTION(lzCompressAlias, Draft);

    void copyByteRunTo(ee::Ptr<uint8_t> to, ee::Ptr<uint8_t> from, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            to[i] = from[i];
    }
    READABLE_FUNCTION(copyByteRunTo, Verified);

    namespace
    {
        constexpr uint32_t kLzoFarMatch = 0x4000, kLzoNearMatch = 0x800;

        // A long length: its base, plus 255 for every zero byte and the byte
        // that ends them.
        uint32_t longLength(ee::Ptr<uint8_t> &in, uint32_t base)
        {
            uint32_t byte = *in;
            ++in;
            while (byte == 0)
            {
                base += 255;
                byte = *in;
                ++in;
            }
            return base + byte;
        }
    }

    // Method 2's decoder, LZO1X's format: runs of literals and matches, each
    // match's last two bits a short run of literals. A first byte over 17
    // starts a run; a far match at distance 0 ends the block. 0, or 1 for a
    // block that is not one. It reads as far as the block says: `bytes` only
    // tells an empty block.
    uint32_t lzoDecompress(ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut, ee::Ptr<uint8_t> source, uint32_t bytes)
    {
        *bytesOut = 0;
        if (bytes == 0)
            return 0;

        ee::Ptr<uint8_t> in = source;
        uint32_t written = 0;
        bool literalsDone = false; // the next token is a match
        bool afterMatch = false;
        for (;;)
        {
            const uint32_t token = *in;
            ++in;
            if (!literalsDone)
            {
                uint32_t run = 0;
                if (token >= 0x10)
                {
                    if (in != source + 1)
                    {
                        in -= 1;
                        literalsDone = true;
                        afterMatch = false;
                        continue;
                    }
                    if (token < 0x12)
                        return 1;
                    run = token - 0x11;
                }
                else if (token == 0)
                    run = longLength(in, 18);
                else
                    run = token + 3;
                copyByteRunTo(output + written, in, run);
                in += run;
                written += run;
                literalsDone = true;
                afterMatch = false;
                continue;
            }

            uint32_t length = 0;
            uint32_t distance = 0;
            uint32_t literals = 0;
            if (token < 0x10)
            {
                literals = token & 3;
                distance = uint32_t{*in} << 2 | (token & 0xCu) >> 2;
                ++in;
                length = afterMatch ? 2 : 3;
                distance += afterMatch ? 1 : kLzoNearMatch + 1;
            }
            else if (token < 0x20)
            {
                const uint32_t low = token & 7;
                length = low == 0 ? longLength(in, 9) : low + 2;
                const uint32_t last = *in;
                ++in;
                literals = last & 3;
                distance = (token >> 3 & 1) << 14 | uint32_t{*in} << 6 | last >> 2;
                ++in;
                if (distance == 0)
                {
                    if (low != 1)
                        return 1;
                    *bytesOut = written;
                    return 0;
                }
                distance += kLzoFarMatch;
            }
            else if (token < 0x40)
            {
                const uint32_t low = token & 0x1F;
                length = low == 0 ? longLength(in, 33) : low + 2;
                const uint32_t last = *in;
                ++in;
                literals = last & 3;
                distance = (last >> 2 | uint32_t{*in} << 6) + 1;
                ++in;
            }
            else
            {
                literals = token & 3;
                length = (token >> 5) + 1;
                distance = (uint32_t{*in} << 3 | (token & 0x1Cu) >> 2) + 1;
                ++in;
            }

            copyByteRunTo(output + written, output + written - distance, length);
            written += length;
            if (literals == 0)
                literalsDone = false;
            else
            {
                copyByteRunTo(output + written, in, literals);
                written += literals;
                in += literals;
            }
            afterMatch = true;
        }
    }
    READABLE_FUNCTION(lzoDecompress, Verified);


    namespace
    {
        constexpr uint32_t kMethodLz = 1, kMethodLzo = 2;
    }

    // A file's data decompressed by the method its header names: 0, or 1 for
    // a method there is not or a block that fails.
    uint32_t decompressFileData(uint32_t method, ee::Ptr<uint8_t> source, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        if (method == kMethodLz)
        {
            const uint32_t work = heapAllocate(kLzWorkBytes);
            lzDecompress(work, source, bytes, output, bytesOut);
            heapFree(work);
            return 0;
        }
        if (method == kMethodLzo)
            return lzoDecompress(output, bytesOut, source, bytes) != 1 ? 0 : 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(decompressFileData, Verified);

    // compressFileData: decompressFileData's other half, for methods 0 and 1
    // (both lzCompress, with a work area of its own): 1, or 0 for any other
    // method. (Nothing in the game calls it.)
    uint32_t compressFileData(uint32_t method, ee::Ptr<uint8_t> input, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut)
    {
        if (method > kMethodLz)
            return 0;
        const uint32_t work = heapAllocate(kLzWorkBytes);
        lzCompressAlias(work, input, bytes, output, bytesOut);
        heapFree(work);
        return 1;
    }
    READABLE_FUNCTION_WAITS(compressFileData, Draft);
}
