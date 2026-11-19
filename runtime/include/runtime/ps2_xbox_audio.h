#pragma once

// Sound banks of Z-Axis games' Xbox releases (Aggressive Inline's
// speech01.zsd, the French speech), for playing them in place of the PS2
// bank's waves.
//
// ZSND header (little endian), PS2 and Xbox alike: "ZSNDPS2 " or
// "ZSNDXBOX", u32 file size, u32 header size, ..., at 0x20 u32 wave count
// and u32 wave table offset. PS2 wave entries are 8 bytes {offset, size}
// (PS-ADPCM, sector aligned); Xbox ones 84 bytes {offset, size, flags,
// 8 bytes, name[64]} (Xbox IMA ADPCM, mono: 36-byte blocks of 64 samples,
// an int16 sample and the step index in front of 32 bytes of 4-bit codes,
// low nibble first).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::xbox_audio
{
    struct Wave
    {
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t flags = 0;
        std::string name; // Xbox only
    };

    // The wave table of a ZSND bank of either kind from its first bytes
    // (enough to hold the table). False if it is not one.
    bool readWaveTable(const std::vector<uint8_t> &header, std::vector<Wave> &out, bool *xbox = nullptr);
    // Bytes needed from the start of the file to read the table (0 if the
    // first 0x28 bytes are not a ZSND header).
    size_t waveTableEnd(const uint8_t *first0x28, size_t size);

    constexpr size_t kAdpcmBlockBytes = 36;
    constexpr size_t kAdpcmBlockSamples = 64;

    // Decodes whole mono blocks; a partial block at the end is dropped.
    std::vector<int16_t> decodeAdpcm(const uint8_t *data, size_t size);
}
