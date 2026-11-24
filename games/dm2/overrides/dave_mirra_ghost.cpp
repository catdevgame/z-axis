// Dave Mirra Freestyle BMX 2 (SLUS-20159): ghost riding's rider (dave_mirra_ghost.h).
//
// The rider is player one's: its rider record (g_riders[0], games/dm2/rider/rider.h Rider) holds the engine objects of its two
// models, the rider's (Rider::messenger, +0x1218) and the bike's (Rider::bikeModel, +0x1660). The readable renderer's skinned
// draws of their trees are the rider's (engine/zge/host/rider_ghost.cpp).

#include "dave_mirra_ghost.h"
#include "sym/game_overrides/dave_mirra_ghost.cpp.h"

#include "machine.h"
#include "runtime/ghost_rider.h"
#include "runtime/ps2_memory_map.h"

#include <cstdint>
#include <cstring>

namespace dave_mirra
{
    namespace
    {
        constexpr uint32_t kRiders = sym::dm2::g_riders; // the riders' records, player one's first
        constexpr uint32_t kRiderModel = 0x1218u;          // Rider::messenger, the rider model's engine object
        constexpr uint32_t kBikeModel = 0x1660u;           // Rider::bikeModel, the bike's

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
