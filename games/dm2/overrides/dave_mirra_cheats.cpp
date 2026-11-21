// Dave Mirra Freestyle BMX 2 (SLUS-20159): what DM2's unlock cheats open of
// the extras (dave_mirra.h, cheats). The game's general codes
// (bmxCheckCheat, games/dm2/menu/cheats.cpp) and the Cheats page's Unlock
// Everything (dave_mirra_gameplay.cpp) open things for the characters 0 ..
// 16; Create Player's riders 17 .. 20 (dave_mirra_creator.cpp, which calls
// these) get what a cheat opened for every one of them, and its level
// select's Back. The Xbox levels' part is dave_mirra_xcareer.cpp's.

#include "dave_mirra.h"

#include "runtime/zaxis_ui.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace dave_mirra::cheats
{

    namespace
    {
        // A RiderCharacter record (games/dm2/game/players.h): what the cheats open.
        constexpr uint32_t kCharacterBytes = 0x3C8;
        constexpr uint32_t kOpenBikes = 0x064, kOpenLooks = 0x065, kSponsorShown = 0x068, kLevels = 0x13C,
                           kTricksOff = 0x3A0;
        constexpr uint32_t kSponsorRows = 9, kLevelBytes = 0x44, kLevelFlags = 0x3C, kTricksOffBytes = 0x14;
        constexpr uint32_t kLevelRows = 9; // DM2's eight and the park editor's
        constexpr uint8_t kLevelOpen = 0x01, kAllBikesOpen = 5, kAllLooksOpen = 2;
        constexpr uint32_t kFirstCreated = kGameCheatRiders;
    }

    size_t shareWithCreatedRiders(uint8_t *records, const uint8_t *before)
    {
        // Does `has` hold for every one of the game's riders in these records?
        const auto every = [](const uint8_t *in, const auto &has)
        {
            for (uint32_t n = 0; n < kGameCheatRiders; ++n)
                if (!has(in + n * kCharacterBytes))
                    return false;
            return true;
        };
        size_t given = 0;
        // What a cheat opened for every one of them now (and not before),
        // set by `give` in each created rider.
        const auto share = [&](const auto &has, const auto &give)
        {
            if (!every(records, has) || every(before, has))
                return;
            for (uint32_t n = kFirstCreated; n < kRiderRecords; ++n)
                give(records + n * kCharacterBytes);
            ++given;
        };
        for (uint32_t level = 0; level < kLevelRows; ++level)
        {
            const uint32_t at = kLevels + level * kLevelBytes + kLevelFlags;
            share([at](const uint8_t *r) { return (r[at] & kLevelOpen) != 0; },
                  [at](uint8_t *r) { r[at] |= kLevelOpen; });
        }
        share([](const uint8_t *r) { return r[kOpenBikes] == kAllBikesOpen; },
              [](uint8_t *r) { r[kOpenBikes] = kAllBikesOpen; });
        share([](const uint8_t *r) { return r[kOpenLooks] == kAllLooksOpen; },
              [](uint8_t *r) { r[kOpenLooks] = kAllLooksOpen; });
        for (uint32_t sponsor = 0; sponsor < kSponsorRows; ++sponsor)
        {
            const uint32_t at = kSponsorShown + sponsor * 4u;
            share([at](const uint8_t *r) { return r[at] | r[at + 1] | r[at + 2] | r[at + 3]; },
                  [at](uint8_t *r)
                  {
                      const uint32_t shown = 1;
                      std::memcpy(r + at, &shown, sizeof(shown));
                  });
        }
        for (uint32_t trick = 0; trick < kTricksOffBytes * 8u; ++trick)
        {
            const uint32_t at = kTricksOff + trick / 8u;
            const uint8_t bit = static_cast<uint8_t>(1u << (trick % 8u));
            share([at, bit](const uint8_t *r) { return (r[at] & bit) == 0; },
                  [at, bit](uint8_t *r) { r[at] = static_cast<uint8_t>(r[at] & ~bit); });
        }
        return given;
    }

    // The level select (MainMenu.SelectLevel) goes Back to the trick menu,
    // and from there to the bike select: the screens a Free Ride came
    // through. A created rider's ride came from Create Rider's RIDE and the
    // game has no bikes for characters 17 .. 20 (the bike select showed
    // KSTRBIKENAME170 and its stats' text codes): its Back goes to Create
    // Rider instead, on that rider.
    bool patchLevelSelectBack(std::string &text)
    {
        zx::zaxis::Span select = zx::zaxis::findObject(text, "MainMenu/SelectLevel");
        if (!select.found())
            return false;
        std::string created;
        for (uint32_t n = kFirstCreated; n < kRiderRecords; ++n)
            created += ":ZXCreated" + std::to_string(n) + "\\n";
        return zx::zaxis::replaceIn(text, select, "OnCancel\t\"PLAY DeInit\\nPLAY .TrickMenu.Init\"",
                             "OnCancel\t\"DECLARE String ZXBack\\nSET ZXBack {\\\"ZXCreated\\\"}\\n"
                             "STRCAT ZXBack Root.CurrentRider1\\nGOTO ZXBack\\nUNDECLARE ZXBack\\n"
                             "PLAY DeInit\\nPLAY .TrickMenu.Init\\nGOTO End\\n" +
                                 created +
                                 "UNDECLARE ZXBack\\nPLAY DeInit\\nPLAY Root.PlayerCreator.Init\\n:End\"");
    }
}

