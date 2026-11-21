// Dave Mirra BMX 2's unlock cheats and the extras (game_overrides/dave_mirra.h, cheats): what a cheat opened for
// every one of the game's 17 riders, Create Player's riders 17 .. 20 get; and Back from a created rider's level
// select goes to Create Rider (dave_mirra_creator.cpp).
#include "MiniTest.h"

#include "dm2/overrides/dave_mirra.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // A RiderCharacter record (games/dm2/game/players.h): the fields the cheats open.
    constexpr uint32_t kRecord = 0x3C8;
    constexpr uint32_t kOpenBikes = 0x064, kOpenLooks = 0x065, kSponsorShown = 0x068, kLevels = 0x13C, kTricksOff = 0x3A0;
    constexpr uint32_t kLevelBytes = 0x44, kLevelFlags = 0x3C;

    uint8_t *rider(std::vector<uint8_t> &records, uint32_t n) { return records.data() + n * kRecord; }
    uint8_t &levelFlags(std::vector<uint8_t> &records, uint32_t n, uint32_t level)
    {
        return rider(records, n)[kLevels + level * kLevelBytes + kLevelFlags];
    }

    // The records as a new game makes them: Woodward Camp (2) and the park (8) open, one bike, one look, some tricks
    // off.
    std::vector<uint8_t> newGame()
    {
        std::vector<uint8_t> records(kRecord * dave_mirra::cheats::kRiderRecords, 0);
        for (uint32_t n = 0; n < dave_mirra::cheats::kRiderRecords; ++n)
        {
            levelFlags(records, n, 2) = 1;
            levelFlags(records, n, 8) = 1;
            rider(records, n)[kOpenBikes] = 1;
            rider(records, n)[kOpenLooks] = 1;
            rider(records, n)[kSponsorShown] = 1;
            rider(records, n)[kTricksOff + 1] = 0x0A;
            rider(records, n)[kTricksOff + 4] = 0xE0;
        }
        return records;
    }

    // bmxCheckCheat's "everything" (games/dm2/menu/cheats.cpp): for the game's 17.
    void everything(std::vector<uint8_t> &records)
    {
        for (uint32_t n = 0; n < dave_mirra::cheats::kGameCheatRiders; ++n)
        {
            rider(records, n)[kOpenBikes] = 5;
            rider(records, n)[kOpenLooks] = 2;
            for (uint32_t s = 0; s < 9; ++s)
                rider(records, n)[kSponsorShown + 4 * s] = 1;
            for (uint32_t l = 0; l < 8; ++l)
                levelFlags(records, n, l) |= 1;
            std::memset(rider(records, n) + kTricksOff, 0, 0x14);
        }
    }
}

void register_dm2_cheats_tests()
{
    MiniTest::Case("Dm2CheatsCreatedRiders", [](TestCase &tc)
    {
        tc.Run("everything: the created riders get it all", [](TestCase &t)
        {
            std::vector<uint8_t> records = newGame();
            const std::vector<uint8_t> before = records;
            everything(records);
            const size_t given = dave_mirra::cheats::shareWithCreatedRiders(records.data(), before.data());
            t.IsTrue(given > 0, "something given");
            for (uint32_t n = 17; n < 21; ++n)
            {
                bool levels = true;
                for (uint32_t l = 0; l < 9; ++l)
                    levels = levels && (levelFlags(records, n, l) & 1);
                t.IsTrue(levels, "every level open");
                t.IsTrue(rider(records, n)[kOpenBikes] == 5 && rider(records, n)[kOpenLooks] == 2, "bikes and looks");
                bool sponsors = true, tricks = true;
                for (uint32_t s = 0; s < 9; ++s)
                    sponsors = sponsors && rider(records, n)[kSponsorShown + 4 * s] == 1;
                for (uint32_t i = 0; i < 0x14; ++i)
                    tricks = tricks && rider(records, n)[kTricksOff + i] == 0;
                t.IsTrue(sponsors && tricks, "sponsors shown, no trick off");
            }
        });

        tc.Run("one rider's code: the created riders get nothing", [](TestCase &t)
        {
            std::vector<uint8_t> records = newGame();
            const std::vector<uint8_t> before = records;
            rider(records, 3)[kOpenBikes] = 5;          // a character's bike code
            for (uint32_t l = 0; l < 8; ++l)            // a character's level code
                levelFlags(records, 5, l) |= 1;
            const std::vector<uint8_t> cheated = records;
            t.IsTrue(dave_mirra::cheats::shareWithCreatedRiders(records.data(), before.data()) == 0, "nothing given");
            t.IsTrue(records == cheated, "the records as the game left them");
        });

        tc.Run("a trick code: that trick on for the created riders", [](TestCase &t)
        {
            std::vector<uint8_t> records = newGame();
            const std::vector<uint8_t> before = records;
            for (uint32_t n = 0; n < 17; ++n)           // trick 0x0B's bit cleared for every character
                rider(records, n)[kTricksOff + 1] &= static_cast<uint8_t>(~0x08);
            t.IsTrue(dave_mirra::cheats::shareWithCreatedRiders(records.data(), before.data()) == 1, "one thing given");
            for (uint32_t n = 17; n < 21; ++n)
                t.IsTrue(rider(records, n)[kTricksOff + 1] == 0x02 && rider(records, n)[kTricksOff + 4] == 0xE0,
                         "that bit alone cleared");
        });

        tc.Run("open for every rider before: nothing new", [](TestCase &t)
        {
            std::vector<uint8_t> records = newGame();
            everything(records);
            const std::vector<uint8_t> before = records;
            rider(records, 0)[kOpenLooks] = 2;          // the code again
            const std::vector<uint8_t> cheated = records;
            t.IsTrue(dave_mirra::cheats::shareWithCreatedRiders(records.data(), before.data()) == 0, "nothing given");
            t.IsTrue(records == cheated, "the created riders as they were");
        });

        tc.Run("Back from a created rider's level select", [](TestCase &t)
        {
            std::string text = "///////////////////////////\r\n// UIBuilder Generated File\r\n\r\n"
                               "OBJECT \"guiScreenSpace\" \"MainMenu\"\r\n{\r\n\tVisible\t1\r\n"
                               "\tOBJECT \"guiToggler\" \"SelectLevel\"\r\n\t{\r\n\t\tVisible\t0\r\n"
                               "\t\tOnCancel\t\"PLAY DeInit\\nPLAY .TrickMenu.Init\"\r\n\t}\r\n}\r\n";
            t.IsTrue(dave_mirra::cheats::patchLevelSelectBack(text), "patched");
            t.IsTrue(text.find("STRCAT ZXBack Root.CurrentRider1\\nGOTO ZXBack\\n") != std::string::npos,
                     "by the first rider's character");
            t.IsTrue(text.find("PLAY DeInit\\nPLAY .TrickMenu.Init\\nGOTO End\\n:ZXCreated17\\n:ZXCreated18\\n"
                               ":ZXCreated19\\n:ZXCreated20\\n") != std::string::npos,
                     "the game's riders back to the trick menu, the created ones on");
            t.IsTrue(text.find("PLAY Root.PlayerCreator.Init\\n:End\"") != std::string::npos, "to Create Rider");
            std::string other = "OBJECT \"guiScreenSpace\" \"MainMenu\"\r\n{\r\n}\r\n";
            const std::string copy = other;
            t.IsTrue(!dave_mirra::cheats::patchLevelSelectBack(other) && other == copy, "no level select: left alone");
        });
    });
}
