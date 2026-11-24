// BMX XXX (SLUS-20415): ghost riding's rider (bmx_xxx_ghost.h).
//
// The rider is player one's: its rider record (g_riders[0], games/bmx/rider/rider.h Rider) holds the engine objects of its two
// models, the rider's (Rider::messenger, +0x2838) and the bike's (Rider::bikeModel, +0x2D50). The readable renderer's skinned
// draws of their trees are the rider's (engine/zge/host/rider_ghost.cpp).

#include "bmx_xxx_ghost.h"
#include "sym/game_overrides/bmx_xxx_ghost.cpp.h"

#include "machine.h"
#include "runtime/ghost_rider.h"
#include "runtime/ps2_memory_map.h"

#include <cstdint>
#include <cstring>

namespace bmx_xxx
{
    namespace
    {
        constexpr uint32_t kRiders = sym::bmx::g_riders; // the riders' records, player one's first
        constexpr uint32_t kRiderModel = 0x2838u;          // Rider::messenger, the rider model's engine object
        constexpr uint32_t kBikeModel = 0x2D50u;           // Rider::bikeModel, the bike's

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            address &= 0x1FFFFFFFu;
            if (address == 0u || address > PS2_RAM_SIZE - 4u)
                return 0u;
            uint32_t value = 0;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        }

        size_t riderObjects(const uint8_t *rdram, uint32_t (&objects)[zx::ghost_rider::kMaxRiderObjects])
        {
            const uint32_t rider = word(rdram, kRiders);
            if (!rider)
                return 0u;
            size_t count = 0;
            for (const uint32_t model : {word(rdram, rider + kRiderModel), word(rdram, rider + kBikeModel)})
                if (model)
                    objects[count++] = model;
            return count;
        }
    }

    void installGhost(PS2Machine &runtime) { zx::ghost_rider::install(runtime, &riderObjects); }
}
