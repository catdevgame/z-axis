// The front end's level menu (level_menu.h): DM2's own code (its game block,
// registered as script functions by the menu set-up).

#include "level_menu.h"
#include "menu_variables.h"
#include "player_menu.h"
#include "../game/players.h"
#include "../game/round.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/level_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kLevelChoiceValuePath{sym::selectLevelCurValuePath}; // "MainMenu.SelectLevel.CurValue"
        // "PLAY MainMenu.SelectLevel.UnLockLevel", "PLAY MainMenu.SelectLevel.LockLevel"
        constexpr ee::Ptr<char> kUnlockLevel{sym::unlockLevelScript};
        constexpr ee::Ptr<char> kLockLevel{sym::lockLevelScript};
        constexpr ee::Ptr<const char> kCompetitionOpenPath{sym::selectLevelCompOpenPath};  // "MainMenu.SelectLevel.CompOpen"
        // The level's ProQuest lines ("MainMenu.SelectLevel.Frame.ProquestInfo.
        // RespectLevel.Text", "...GapLevel.Text", "...MedalLevel.Text",
        // "...ChallengeLevel.Text").
        constexpr ee::Ptr<const char> kRespectTextPath{sym::levelRespectTextPath};
        constexpr ee::Ptr<const char> kGapTextPath{sym::levelGapTextPath};
        constexpr ee::Ptr<const char> kMedalTextPath{sym::levelMedalTextPath};
        constexpr ee::Ptr<const char> kChallengeTextPath{sym::levelChallengeTextPath};
        // "%d OUT OF 5,000", and "%d,%03d OUT OF 5,000" above a thousand.
        constexpr uint32_t kRespectFormat = sym::levelRespectFormat;
        constexpr uint32_t kRespectThousandsFormat = sym::levelRespectThousandsFormat;
        constexpr uint32_t kOutOfFormat = sym::outOfFormat;                      // "%d OUT OF %d"
        constexpr uint32_t kMedalNoneText = sym::g_textNone;                     // "None"
        constexpr uint32_t kMedalBronzeText = sym::g_textBronze;                 // "Bronze"
        constexpr uint32_t kMedalSilverText = sym::g_textSilver;                 // "Silver"
        constexpr uint32_t kMedalGoldText = sym::g_textGold;                     // "Gold"
        // The string table's keys of what the challenge line says:
        // "$KSTRSP_OBJCTV_DONE", and "$KSTRSP_OBJCTV_MINST_00_00" ..
        // "..._04" for the five challenges, a fixed-size text each.
        constexpr uint32_t kChallengesDoneKey = sym::challengesDoneKey;
        constexpr uint32_t kChallengeKeys = sym::challengeKeys;
        constexpr uint32_t kChallengeKeyBytes = 0x20;
        constexpr uint32_t kChallenges = 5;

        // How many gaps each game level has (the level's set-up counts them).
        uint8_t levelGapCount(uint32_t gameLevel)
        {
            return *(ee::Ptr<uint8_t>(sym::g_levelGapCounts) + static_cast<ptrdiff_t>(gameLevel));
        }

        // The game level of the level choice's entry.
        uint32_t gameLevelOf(ee::Float entry)
        {
            return *(ee::Ptr<uint8_t>(sym::g_levelMenuGameLevels) + static_cast<ptrdiff_t>(ee::toInt(entry)));
        }

        // A character's record of a game level's goals.
        ee::Ptr<LevelGoals> levelGoals(uint32_t character, uint32_t gameLevel)
        {
            return ee::Ptr<LevelGoals>(&riderCharacter(character)->levels[0]) + static_cast<ptrdiff_t>(gameLevel);
        }

        // Whether the character has found the level's gap: a bit a gap, from
        // LevelGoals::gapsFound on.
        uint32_t gapFound(ee::Ptr<LevelGoals> goals, uint32_t gap)
        {
            const ee::Ptr<int32_t> words(ee::Address(&goals->gapsFound));
            return static_cast<uint32_t>(*(words + static_cast<ptrdiff_t>(gap >> 5)) >> (gap & 31)) & 1u;
        }
    }

    // bmxmenuGameLevelUpdate: the script's "bmxmenuGameLevelUpdate": the
    // level menu lists the levels in its own order; the game level of the
    // chosen entry goes to CurrentGameLevel.
    void bmxmenuGameLevelUpdate()
    {
        *g_currentGameLevel = (*g_levelMenuGameLevels)[*g_currentLevel];
    }
    READABLE_FUNCTION(bmxmenuGameLevelUpdate, Verified);

    void bmxSetupLevelStats()
    {
        ee::Local<char[0x20]> respectText;
        ee::Local<char[0x20]> gapText;
        ee::Local<ee::Float> entry;
        ee::Local<int32_t> competitionOpen;

        zge::zgui::copyVariableAtPath(kLevelChoiceValuePath, entry);

        // The level is open when it is open to the first player's character,
        // or with two players to either's.
        bool open;
        if (*g_currentRiderCount == 1)
            open = (levelGoals((*g_currentRiders)[0], gameLevelOf(*entry))->flags & kLevelOpen) != 0;
        else
            open = (levelGoals((*g_currentRiders)[0], gameLevelOf(*entry))->flags & kLevelOpen) != 0
                || (levelGoals((*g_currentRiders)[1], gameLevelOf(*entry))->flags & kLevelOpen) != 0;
        if (open)
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kUnlockLevel);
        else
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kLockLevel);

        // The rest is the first player's character's. A level whose
        // competition is not open yet cannot be picked for it.
        *competitionOpen = (levelGoals((*g_currentRiders)[0], gameLevelOf(*entry))->flags & kLevelCompetitionOpen) ? 1 : 0;
        zge::zgui::setVariableAtPath(kCompetitionOpenPath, competitionOpen);
        if (*competitionOpen == 0 && *g_currentRideType == kCompetition)
            *g_currentRideType = kLevelGoals;

        // The thousands get their comma above 1,000 here (the rider choice's
        // total gets it from 1,000 on).
        const uint32_t respect = *(ee::Ptr<uint32_t>(&riderCharacter((*g_currentRiders)[0])->levelRespect[0])
                                   + static_cast<ptrdiff_t>(gameLevelOf(*entry)));
        if (respect <= 1000)
            zge::formatString(respectText, kRespectFormat, respect);
        else
            zge::formatString(respectText, kRespectThousandsFormat, respect / 1000, respect % 1000);
        zge::zgui::setVariableAtPath(kRespectTextPath, respectText);

        // The gaps found, of the level's.
        uint32_t found = 0;
        const uint32_t gaps = levelGapCount(gameLevelOf(*entry));
        for (uint32_t gap = 0; gap < gaps; ++gap)
            found += gapFound(levelGoals((*g_currentRiders)[0], gameLevelOf(*entry)), gap);
        zge::formatString(gapText, kOutOfFormat, found, levelGapCount(gameLevelOf(*entry)));
        zge::zgui::setVariableAtPath(kGapTextPath, gapText);

        switch (levelGoals((*g_currentRiders)[0], gameLevelOf(*entry))->medal)
        {
        case kMedalBronze:
            zge::zgui::setVariableAtPath(kMedalTextPath, kMedalBronzeText);
            break;
        case kMedalNone:
            zge::zgui::setVariableAtPath(kMedalTextPath, kMedalNoneText);
            break;
        case kMedalSilver:
            zge::zgui::setVariableAtPath(kMedalTextPath, kMedalSilverText);
            break;
        case kMedalGold:
            zge::zgui::setVariableAtPath(kMedalTextPath, kMedalGoldText);
            break;
        default:
            break;
        }

        // The next challenge, or that they are all done.
        const ee::Ptr<LevelGoals> goals = levelGoals((*g_currentRiders)[0], gameLevelOf(*entry));
        if (goals->flags & kLevelChallengesDone)
            zge::zgui::setVariableAtPath(kChallengeTextPath, kChallengesDoneKey);
        else if (goals->challenge < kChallenges)
            zge::zgui::setVariableAtPath(kChallengeTextPath, kChallengeKeys + goals->challenge * kChallengeKeyBytes);
    }
    READABLE_FUNCTION(bmxSetupLevelStats, Verified);
}
