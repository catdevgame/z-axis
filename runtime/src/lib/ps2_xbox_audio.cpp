#include "runtime/ps2_xbox_audio.h"

#include <algorithm>
#include <cstring>

namespace zx::xbox_audio
{
    namespace
    {
        uint32_t readU32(const uint8_t *p)
        {
            uint32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        constexpr int kStepTable[89] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
            66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
            408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
            2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
            8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
            29794, 32767};
        constexpr int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

        bool isPs2(const uint8_t *p) { return std::memcmp(p, "ZSNDPS2 ", 8) == 0; }
        bool isXbox(const uint8_t *p) { return std::memcmp(p, "ZSNDXBOX", 8) == 0; }
    }

    size_t waveTableEnd(const uint8_t *first, size_t size)
    {
        if (size < 0x28u || (!isPs2(first) && !isXbox(first)))
            return 0;
        const uint32_t count = readU32(first + 0x20);
        const uint32_t table = readU32(first + 0x24);
        if (count > 0x10000u)
            return 0;
        return static_cast<size_t>(table) + static_cast<size_t>(count) * (isXbox(first) ? 84u : 8u);
    }

    bool readWaveTable(const std::vector<uint8_t> &header, std::vector<Wave> &out, bool *xbox)
    {
        out.clear();
        const size_t end = waveTableEnd(header.data(), header.size());
        if (end == 0 || end > header.size())
            return false;
        const bool x = isXbox(header.data());
        if (xbox)
            *xbox = x;
        const uint32_t count = readU32(header.data() + 0x20);
        const uint32_t table = readU32(header.data() + 0x24);
        const size_t stride = x ? 84u : 8u;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t *e = header.data() + table + i * stride;
            Wave w;
            w.offset = readU32(e);
            w.size = readU32(e + 4);
            if (x)
            {
                w.flags = readU32(e + 8);
                const char *name = reinterpret_cast<const char *>(e + 20);
                w.name.assign(name, strnlen(name, 64));
            }
            out.push_back(std::move(w));
        }
        return true;
    }

    std::vector<int16_t> decodeAdpcm(const uint8_t *data, size_t size)
    {
        std::vector<int16_t> out;
        const size_t blocks = size / kAdpcmBlockBytes;
        out.reserve(blocks * kAdpcmBlockSamples);
        for (size_t b = 0; b < blocks; ++b)
        {
            const uint8_t *p = data + b * kAdpcmBlockBytes;
            int predictor = static_cast<int16_t>(p[0] | (p[1] << 8));
            int index = std::clamp<int>(p[2], 0, 88);
            // The header's sample, then 63 of the 64 codes.
            out.push_back(static_cast<int16_t>(predictor));
            for (size_t n = 0; n + 1u < kAdpcmBlockSamples; ++n)
            {
                const uint8_t byte = p[4 + n / 2u];
                const int code = (n & 1u) ? (byte >> 4) : (byte & 0x0F);
                const int step = kStepTable[index];
                int diff = step >> 3;
                if (code & 1)
                    diff += step >> 2;
                if (code & 2)
                    diff += step >> 1;
                if (code & 4)
                    diff += step;
                predictor += (code & 8) ? -diff : diff;
                predictor = std::clamp(predictor, -32768, 32767);
                index = std::clamp(index + kIndexTable[code], 0, 88);
                out.push_back(static_cast<int16_t>(predictor));
            }
        }
        return out;
    }
}
