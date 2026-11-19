#pragma once

// Mipmapped RGBA copies of PS2 textures, for the GPU renderer's native draws
// with texture filtering on (gs_quality.h Anisotropy 1..4).
//
// The GS samples a texture at one level (or the game's own mip levels, by
// depth), point or bilinear, so far ground drawn at a high internal
// resolution shimmers. With the option on, a native draw's PS2 texture is
// drawn from a copy with a full mip chain, trilinear and anisotropic, as a
// replacement picture is: the indices read through the palette on the CPU,
// TEXA applied per texel (exactly as the GS would for each texel), then each
// level a box filter of the one above. The GS's own levels (MIPTBP) are kept
// where the game has them; levels below its last are filtered from it.
//
// Plain functions on the CPU (the unit tests use them); the backend keeps
// the copies (gs_gpu_backend.cpp, "mipmapped copies").

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ps2x::gs_mips
{
    // FNV-1a 64 (the content keys of the copies).
    constexpr uint64_t kFnvBasis = 1469598103934665603ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    inline uint64_t fnvBytes(uint64_t h, const void *data, size_t n)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * kFnvPrime;
        return h;
    }

    inline uint64_t fnvWord(uint64_t h, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            h = (h ^ ((v >> (i * 8)) & 0xFFu)) * kFnvPrime;
        return h;
    }

    // A content hash, 8 bytes at a time (FNV-1a a byte at a time is a tenth
    // of a millisecond for a 256 x 256 texture). Keys compared in this
    // process only.
    inline uint64_t fnvBytes64(uint64_t h, const void *data, size_t n)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        size_t i = 0;
        for (; i + 8u <= n; i += 8u)
        {
            uint64_t w;
            std::memcpy(&w, p + i, sizeof(w));
            h = (h ^ w) * 0xFF51AFD7ED558CCDull;
            h ^= h >> 32;
        }
        for (; i < n; ++i)
            h = (h ^ p[i]) * kFnvPrime;
        return h;
    }

    // TEXA as the GS applies it to a texel or CLUT entry (RGBA8, a 16-bit
    // one expanded with its alpha bit as 0x80), as applyTexa in the
    // shaders: cls 0 32-bit (unchanged), 1 24-bit (alpha TA0, or 0 for
    // black with AEM), 2 16-bit (the alpha bit picks TA1, else TA0 or 0).
    inline uint32_t applyTexa(uint32_t c, uint32_t cls, uint32_t ta0, uint32_t ta1, bool aem)
    {
        if (cls == 0u)
            return c;
        const bool rgb0 = (c & 0x00FFFFFFu) == 0u;
        uint32_t a;
        if (cls == 1u)
            a = (aem && rgb0) ? 0u : ta0;
        else if ((c >> 24) & 0x80u)
            a = ta1;
        else
            a = (aem && rgb0) ? 0u : ta0;
        return (c & 0x00FFFFFFu) | ((a & 0xFFu) << 24);
    }

    struct Level
    {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> texels; // RGBA8 (R in the low byte), rows
    };

    // Levels of a full chain from w x h down to 1 x 1.
    inline uint32_t chainLength(uint32_t w, uint32_t h)
    {
        uint32_t n = 1;
        while (w > 1u || h > 1u)
        {
            w = std::max(w >> 1, 1u);
            h = std::max(h >> 1, 1u);
            ++n;
        }
        return n;
    }

    // The next level: each texel the mean of the 2 x 2 (or 2 x 1 on a side
    // already 1) above it. weighted: colour weighted by alpha (transparent
    // texels give no colour, so cut-out edges do not darken with distance;
    // all four clear: the plain mean), alpha the plain mean. Rounded.
    inline Level downsample(const Level &src, bool weighted)
    {
        Level d;
        d.width = std::max(src.width >> 1, 1u);
        d.height = std::max(src.height >> 1, 1u);
        d.texels.resize(static_cast<size_t>(d.width) * d.height);
        // A side already 1 takes its one texel twice: the mean of 2 x 1 is
        // then the mean of 2 x 2 with each counted twice (same rounding).
        const uint32_t dx = src.width > 1u ? 1u : 0u;
        for (uint32_t y = 0; y < d.height; ++y)
        {
            const uint32_t *r0 = src.texels.data() + static_cast<size_t>(src.height > 1u ? 2u * y : y) * src.width;
            const uint32_t *r1 = src.height > 1u ? r0 + src.width : r0;
            uint32_t *out = d.texels.data() + static_cast<size_t>(y) * d.width;
            for (uint32_t x = 0; x < d.width; ++x)
            {
                const uint32_t sx = dx ? 2u * x : x;
                const uint32_t c[4] = {r0[sx], r0[sx + dx], r1[sx], r1[sx + dx]};
                const uint32_t a[4] = {c[0] >> 24, c[1] >> 24, c[2] >> 24, c[3] >> 24};
                const uint32_t sumA = a[0] + a[1] + a[2] + a[3];
                uint32_t v = ((sumA + 2u) >> 2) << 24;
                if (!weighted || sumA == 0u || (a[0] == a[1] && a[1] == a[2] && a[2] == a[3]))
                {
                    for (uint32_t ch = 0; ch < 24u; ch += 8u)
                        v |= ((((c[0] >> ch) & 0xFFu) + ((c[1] >> ch) & 0xFFu) + ((c[2] >> ch) & 0xFFu) +
                               ((c[3] >> ch) & 0xFFu) + 2u) >> 2) << ch;
                }
                else
                {
                    for (uint32_t ch = 0; ch < 24u; ch += 8u)
                    {
                        const uint32_t sum = ((c[0] >> ch) & 0xFFu) * a[0] + ((c[1] >> ch) & 0xFFu) * a[1] +
                                             ((c[2] >> ch) & 0xFFu) * a[2] + ((c[3] >> ch) & 0xFFu) * a[3];
                        v |= std::min((sum + sumA / 2u) / sumA, 255u) << ch;
                    }
                }
                out[x] = v;
            }
        }
        return d;
    }

    // Completes a chain whose first levels are given (the GS's own, each
    // half the one above) down to 1 x 1.
    inline void completeChain(std::vector<Level> &levels, bool weighted)
    {
        if (levels.empty())
            return;
        const uint32_t total = chainLength(levels[0].width, levels[0].height);
        while (levels.size() < total)
            levels.push_back(downsample(levels.back(), weighted));
    }

    // Indices through a palette (256 RGBA8 entries, TEXA already applied).
    inline void expandIndexed(const uint8_t *indices, size_t count, const uint32_t *palette, uint32_t *out)
    {
        for (size_t i = 0; i < count; ++i)
            out[i] = palette[indices[i]];
    }

    // Bytes of a chain (RGBA8).
    inline size_t chainBytes(const std::vector<Level> &levels)
    {
        size_t b = 0;
        for (const Level &l : levels)
            b += l.texels.size() * 4u;
        return b;
    }
}
