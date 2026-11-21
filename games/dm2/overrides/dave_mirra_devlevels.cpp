// Dave Mirra Freestyle BMX 2 (SLUS-20159): every level open in the level
// select and every trick open, for developers and test runs (recordings of
// the rider on several levels and of the locked tricks). Off by default;
// ZX_DEV_UNLOCK_ALL=1 (as Aggressive Inline's) turns both on,
// ZX_DM2_OPEN_LEVELS=1 the levels alone, ZX_DM2_OPEN_TRICKS=1 the tricks
// alone.
//
// The level select (UILIB.ZAL's MainMenu.SelectLevel) asks
// bmxSetupLevelStats (0x1AFE58) for each value it shows; that native plays
// LockLevel (the toggler disabled: its OnSelect does nothing) unless bit 0
// ("open") of the level's progress flags is set for player 1's rider
// (0x25A190), or in two-player Free Ride for either rider (0x25A194). The
// flags are a byte of each rider's record: 0x2174B4 + rider x 0x3C8 +
// 0x13C (the progress) + level x 0x44 + 0x3C (dave_mirra_xcareer.cpp's
// notes). A fresh game opens only Woodward Camp (level 2), and ProQuest
// opens the others as their tiers are done.
//
// Here the bit is set for every rider and each of DM2's eight levels at
// every vsync (so a loaded save is opened too); the other bits (competition
// open, tiers done, the medal) are left as they are. The save the game
// writes then has the levels open.
//
// Every trick open too (ZX_DEV_UNLOCK_ALL=1 or ZX_DM2_OPEN_TRICKS=1):
// each rider's record has a bit a trick at +0x3A0 (20 bytes), set: the
// rider cannot do it. A new roster (initGame) sets the bit of every
// trick under 0x48 whose kind (0x245520) has 0x8 (17 air tricks, the tap
// sequences' tricks: 0x09, 0x0B, 0x13, 0x25 .. 0x32) and clears the rider's
// own (the rider's group of the sequences, 0x2172B4 + rider x 0x18: four
// entries, one or two used); a ProQuest reward of kind 3
// (giveReward) clears one, and the "everything" cheat (bmxCheckCheat, code
// 8) clears all 20 bytes for riders 0 .. 16. The physics rider copies the
// 20 bytes into its +0x42 when made and at each level start
// (airTrickByTaps, grindTrick, stallTrick test them). Here they are cleared
// for every rider at every vsync, as the cheat does; nothing else changes.
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_devlevels.cpp.h"
#include "machine.h"
#include "runtime/boot_config.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace dave_mirra::devlevels
{
    namespace
    {
        constexpr uint32_t kRiders = sym::dm2::g_riderCharacters, kRiderSize = 0x3C8u, kRiderCount = 21u;
        constexpr uint32_t kProgress = 0x13Cu, kProgressSize = 0x44u, kProgressFlags = 0x3Cu;
        constexpr uint8_t kLevelOpen = 0x01u;
        constexpr uint32_t kLevels = 8u; // DM2's own levels, 0 .. 7 (8 is the park editor)
        constexpr uint32_t kTricksOff = 0x3A0u, kTricksOffSize = 0x14u; // a bit a trick: set, locked

        bool enabled(const char *name)
        {
            const char *value = zx::boot::text(name);
            return value && value[0] == '1';
        }

        void openLevels(uint8_t *rdram, uint64_t)
        {
            for (uint32_t rider = 0; rider < kRiderCount; ++rider)
                for (uint32_t level = 0; level < kLevels; ++level)
                {
                    const uint32_t at = kRiders + rider * kRiderSize + kProgress + level * kProgressSize + kProgressFlags;
                    if (uint8_t *flags = getMemPtr(rdram, at))
                        *flags |= kLevelOpen;
                }
        }

        void openTricks(uint8_t *rdram, uint64_t)
        {
            for (uint32_t rider = 0; rider < kRiderCount; ++rider)
                for (uint32_t i = 0; i < kTricksOffSize; ++i)
                    if (uint8_t *bits = getMemPtr(rdram, kRiders + rider * kRiderSize + kTricksOff + i))
                        *bits = 0;
        }
    }

    bool install(PS2Machine &runtime)
    {
        const bool all = enabled("ZX_DEV_UNLOCK_ALL");
        bool any = false;
        if (all || enabled("ZX_DM2_OPEN_LEVELS"))
        {
            runtime.addVSyncHook(&openLevels);
            std::cout << "[dave-mirra] every level open in the level select (developers)" << std::endl;
            any = true;
        }
        if (all || enabled("ZX_DM2_OPEN_TRICKS"))
        {
            runtime.addVSyncHook(&openTricks);
            std::cout << "[dave-mirra] every trick open for every rider (developers)" << std::endl;
            any = true;
        }
        return any;
    }
}

namespace dave_mirra
{
    bool installOpenLevels(PS2Machine &runtime)
    {
        return devlevels::install(runtime);
    }
}
