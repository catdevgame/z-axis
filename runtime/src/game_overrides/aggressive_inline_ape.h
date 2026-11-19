#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class PS2Runtime;

namespace aggressive_inline
{
    // The Ape, the Xbox release's secret skater (PS2X_APE=1 and the Xbox
    // files): select screen entry, model, stats, unlock, career record.
    void installApe(PS2Runtime &runtime);

    namespace ape
    {
        // The Ape's career (skater record 0) after the game's own data in a
        // career save body, as the game's serializer writes a skater; `used`
        // is the game's part (0: found by walking the body) and grows by the
        // block. False (body unchanged) when the Ape is off or there is no
        // room. For the native saves (the game's buffer and the autosave).
        bool appendCareer(const uint8_t *rdram, std::vector<uint8_t> &body, size_t &used);
    }
}
