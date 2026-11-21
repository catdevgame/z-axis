// BMX XXX's players and rider characters (players.h): BMX XXX's own code
// (bmx.c), DM2's line (games/dm2/game/players.cpp).

#include "players.h"
#include "bmx/create/menu_model.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/profile/players.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The game's tables a new character's record is made from, a row a
        // character: its names, its nine stats, and the two rows not read yet.
        struct CharacterNames
        {
            ee::Ptr<const char> first;  // +0x00 "Twan"
            ee::Ptr<const char> last;   // +0x04 ""
            uint8_t _08[0x14 - 0x08];
        };
        static_assert(sizeof(CharacterNames) == 0x14);
        struct CharacterStats
        {
            uint8_t value[kCharacterStats];
        };
        struct CharacterPair
        {
            uint8_t value[2];
        };
        const ee::Ptr<CharacterNames> kCharacterNames(sym::g_characterNames);
        const ee::Ptr<CharacterStats> kCharacterStatRows(sym::g_characterStats);
        const ee::Ptr<CharacterPair> kCharacterPairs(sym::g_characterPairs);
        const ee::Ptr<uint8_t> kCharacterBytes(sym::g_characterBytes);
        const ee::Ptr<LevelInfo> kLevelInfos(sym::g_levels);
        // The 13th rider, "Amish Boy".
        constexpr uint32_t kAmishBoy = 12;
        // The characters' records, in the saved game.
        const ee::Ptr<RiderCharacter> kRiderCharacterRecords(sym::g_savedGame + offsetof(SavedGame, characters));
    }

    // isFemaleRider: whether the character a player's setup picked has a
    // female model.
    uint32_t isFemaleRider(ee::Ptr<const PlayerSetup> setup)
    {
        return setupModelIsFemale(setup);
    }
    READABLE_FUNCTION(isFemaleRider, Verified);

    uint8_t currentPlayer()
    {
        return *g_currentPlayer;
    }
    READABLE_FUNCTION(currentPlayer, Verified);

    // playerCharacter: the record of the character the player picked.
    ee::Ptr<RiderCharacter> playerCharacter(uint32_t player)
    {
        const uint8_t character = (ee::Ptr<PlayerSetup>(sym::g_playerSetups) + static_cast<ptrdiff_t>(player))->character;
        return kRiderCharacterRecords + static_cast<ptrdiff_t>(character);
    }
    READABLE_FUNCTION(playerCharacter, Verified);

    // playerLevelGoals: that character's record of a level.
    ee::Ptr<LevelGoals> playerLevelGoals(uint32_t player, uint32_t level)
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        return ee::Ptr<LevelGoals>(&character->levels[0]) + static_cast<ptrdiff_t>(level);
    }
    READABLE_FUNCTION(playerLevelGoals, Verified);

    // resetRiderCharacter: the record cleared, then filled from the game's
    // tables: the character's names and stats, and each level's start (the
    // open level marked, a challenge level's two counts at 20).
    void resetRiderCharacter(uint32_t character)
    {
        const ee::Ptr<RiderCharacter> record = kRiderCharacterRecords + static_cast<ptrdiff_t>(character);
        zge::memoryFill(record, 0, sizeof(RiderCharacter));
        if (character == kAmishBoy)
            record->flags |= kCharacterLocked;
        const ee::Ptr<CharacterNames> names = kCharacterNames + static_cast<ptrdiff_t>(character);
        zge::stringCopy(ee::Ptr<char>(record->firstName), names->first);
        zge::stringCopy(ee::Ptr<char>(record->lastName), names->last);
        const ee::Ptr<CharacterStats> stats = kCharacterStatRows + static_cast<ptrdiff_t>(character);
        for (uint32_t i = 0; i < kCharacterStats; ++i)
            record->stats[i] = stats->value[i];
        const ee::Ptr<CharacterPair> pair = kCharacterPairs + static_cast<ptrdiff_t>(character);
        const uint8_t second = pair->value[1];
        const uint8_t first = pair->value[0];
        const uint8_t byte = kCharacterBytes[character];
        record->ownTricks[0] = first;
        record->bike = byte;
        record->ownTricks[1] = second;
        record->bikes = 1;
        record->unused = 0;
        record->bikeChoice = 0;
        for (uint32_t level = 0; level < kLevels; ++level)
        {
            LevelGoals &goals = record->levels[level];
            const ee::Ptr<LevelInfo> info = kLevelInfos + static_cast<ptrdiff_t>(level);
            if (info->flags & kLevelOpen)
                goals.flags |= kLevelGoalsOpen;
            if (info->gameMode == kChallengeLevelMode)
            {
                goals.goals = kChallengeLevelCount;
                goals.gaps = kChallengeLevelCount;
            }
        }
    }
    READABLE_FUNCTION(resetRiderCharacter, Verified);
}
