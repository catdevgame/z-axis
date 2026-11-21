// BMX XXX's rewards: what completing goals gives a player (a bike, a rider
// character, a level, a movie ..), and the text the reward display shows
// for one. BMX XXX's own code (its game block). The kinds' names are the
// reward texts' own ("KSTRSP_REWARD_<KIND>.."); what each kind opens is read
// from giveReward.

#include "objective_calls.h"
#include "objectives.h"

#include "bmx/game/npcs.h"
#include "bmx/profile/players.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/objective_rewards.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // The level reward for this level also clears the player's
        // kSetupTips.
        constexpr uint16_t kSecondLevel = 1;
        // A character reward's text numbers the characters from the 13th
        // rider (the first one a new game keeps locked).
        constexpr uint16_t kFirstRewardCharacter = 12;

        // The names of the reward texts in the string table.
        constexpr uint32_t kAttributeText = sym::bmx::g_textRewardAttribute;      // "KSTRSP_REWARD_ATTR00"
        constexpr uint32_t kRespectText = sym::bmx::g_textRewardRespect;          // "KSTRSP_REWARD_RESPECT00"
        constexpr uint32_t kCharacterTipText = sym::bmx::g_textRewardTipChar;     // "KSTRSP_REWARD_TIPCHAR%02d_%d"
        constexpr uint32_t kTrick02Text = sym::bmx::g_textRewardTrick02;          // "KSTRSP_REWARD_TRICK02"
        constexpr uint32_t kBikeText = sym::bmx::g_textRewardBike;                // "KSTRSP_REWARD_BIKE%02d"
        constexpr uint32_t kLookText = sym::bmx::g_textRewardLook;                // "KSTRSP_REWARD_LOOK%02d"
        constexpr uint32_t kSecretLookText = sym::bmx::g_textRewardLookSecret;    // "KSTRSP_REWARD_LOOKSECRET"
        constexpr uint32_t kCharacterText = sym::bmx::g_textRewardChar;           // "KSTRSP_REWARD_CHAR%02d"
        constexpr uint32_t kTeamText = sym::bmx::g_textRewardTeam;                // "KSTRSP_REWARD_TEAM"
        constexpr uint32_t kLevelText = sym::bmx::g_textRewardLevel;              // "KSTRSP_REWARD_LEVEL%02d"
        constexpr uint32_t kContestLevelText = sym::bmx::g_textRewardContestLev;  // "KSTRSP_REWARD_CONTEST_LEV%02d"
        constexpr uint32_t kEditThemeText = sym::bmx::g_textRewardEditTheme;      // "KSTRSP_REWARD_EDITTHEME%02d"
        constexpr uint32_t kEditPieceText = sym::bmx::g_textRewardEditPiece;      // "KSTRSP_REWARD_EDITPIECE%02d"
        constexpr uint32_t kTipText = sym::bmx::g_textRewardTip;                  // "KSTRSP_REWARD_TIP%02d"
        constexpr uint32_t kMovieText = sym::bmx::g_textRewardMovie;              // "KSTRSP_REWARD_MOVIE%02d"
        constexpr uint32_t kCutsceneText = sym::bmx::g_textRewardCutscene;        // "KSTRSP_REWARD_CUTSCENE%02d"
        constexpr uint32_t kCodeText = sym::bmx::g_textRewardCode;                // "KSTRSP_REWARD_CODE%02d"
        constexpr uint32_t kFeatureText = sym::bmx::g_textRewardFeature;          // "KSTRSP_REWARD_FEATURE%02d"

        // The tricks' names by trick ("turndown" .. "upside-down roll-aid"),
        // and, right after them, the modifiers' ("barspin" ..).
        constexpr uint32_t kNamedTricks = 0x56;
        inline ee::Global<ee::Ptr<const char>[100], sym::g_trickNames> g_trickNames;
        inline ee::Global<ee::Ptr<const char>[30], sym::g_modifierNames> g_modifierNames;
        constexpr uint8_t kNoTrick = 0xFF;

        // The rider's air tricks by tap sequence, four a rider character
        // (the rider's own table, rider/tricks.cpp; this file reads a
        // sequence's trick alone).
        struct TapSequence
        {
            uint8_t length;
            uint8_t taps[3];
            uint8_t trick; // +0x04
            uint8_t motion;
        };
        static_assert(sizeof(TapSequence) == 6);
        constexpr uint32_t kCharacterSequences = 4;
        uint8_t sequenceTrick(uint32_t character, uint32_t sequence)
        {
            // Indexed from the table's start, as the original does: a
            // sequence past a character's four reads the next character's.
            const ee::Ptr<TapSequence> sequences(sym::g_tapSequences);
            return sequences[character * kCharacterSequences + sequence].trick;
        }
    }

    // giveReward: the reward put into the saved game for the player. The
    // kinds not listed (an attribute, respect, a trick, a look, a contest
    // level, an editor piece, a tip, a code) change nothing here.
    void giveReward(uint32_t player, ee::Ptr<Reward> reward)
    {
        player &= 0xFF;
        PlayerSetup &setup = (*g_playerSetups)[player];
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        // (The player's goals for this level are fetched and not used.)
        playerLevelGoals(player, *g_currentLevel);
        switch (reward->kind)
        {
        case kRewardKindBike:
        {
            // The character has every bike up to this one.
            const int32_t bikes = reward->value + 1;
            const int32_t had = character->bikes;
            character->flags = character->flags | kCharacterWonBike;
            character->bikes = static_cast<uint8_t>(bikes < had ? had : bikes);
            break;
        }
        case kRewardKindCharacter:
            g_savedGame->characters[reward->value].flags = g_savedGame->characters[reward->value].flags & ~kCharacterLocked;
            break;
        case kRewardKindTeam:
            g_savedGame->teamRewards = g_savedGame->teamRewards + 1;
            break;
        case kRewardKindLevel:
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, reward->value);
            goals->flags = goals->flags | kLevelGoalsOpen;
            character->flags = character->flags | kCharacterWonLevel;
            if (reward->value == kSecondLevel)
                setup.flags = setup.flags & static_cast<uint8_t>(~kSetupTips);
            break;
        }
        case kRewardKindEditTheme:
        {
            // The park editor has every theme up to this one.
            const int32_t themes = reward->value + 1;
            const int32_t had = g_savedGame->editorThemes;
            g_savedGame->editorThemes = static_cast<uint8_t>(themes < had ? had : themes);
            break;
        }
        case kRewardKindMovie:
            g_savedGame->movies[reward->value] = g_savedGame->movies[reward->value] | kMovieRewardBit0;
            g_savedGame->movies[reward->value] = g_savedGame->movies[reward->value] | kMovieRewardBit1;
            g_savedGame->flags = g_savedGame->flags | kSavedMovieWon;
            break;
        case kRewardKindCutscene:
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, reward->value);
            goals->flags = goals->flags | kLevelGoalsCutsceneWon;
            break;
        }
        case kRewardKindFeature:
            g_savedGame->features = static_cast<uint16_t>(g_savedGame->features | (1u << (reward->value & 31)));
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(giveReward, Verified);

    // rewardText: the reward display's line for a reward, written to `text`:
    // the string table's text of the reward's name ("KSTRSP_REWARD_BIKE03"),
    // a format, written with the reward's value. 0 when the kind has no
    // text.
    //
    // A trick reward is told by the level's first character: its text is
    // that rider character's tip text for the reward's value, written with
    // the name of the trick the value's tap sequence of that character
    // does; no text when the sequence has no trick.
    int32_t rewardText(uint32_t player, ee::Ptr<Reward> reward, ee::Ptr<char> text)
    {
        ee::Local<char[0x200]> name;
        ee::Local<NpcFacts> teacher;
        const PlayerSetup &setup = (*g_playerSetups)[player];
        uint32_t trick = 0;
        name[0] = 0;
        switch (reward->kind)
        {
        case kRewardKindAttribute:
            zge::stringCopy(name, kAttributeText);
            break;
        case kRewardKindRespect:
            zge::stringCopy(name, kRespectText);
            break;
        case kRewardKindTrick:
            npcFacts(firstNpc(), teacher);
            trick = sequenceTrick(teacher->character, reward->value);
            if (trick != kNoTrick)
                zge::formatString(name, kCharacterTipText, teacher->character, reward->value);
            break;
        case kRewardKindTrick02:
            zge::formatString(name, kTrick02Text);
            break;
        case kRewardKindBike:
            zge::formatString(name, kBikeText, reward->value);
            break;
        case kRewardKindLook:
            if (setup.character == kSecretLookCharacter)
                zge::formatString(name, kSecretLookText);
            else
                zge::formatString(name, kLookText, reward->value);
            break;
        case kRewardKindCharacter:
            zge::formatString(name, kCharacterText, reward->value - kFirstRewardCharacter);
            break;
        case kRewardKindTeam:
            zge::formatString(name, kTeamText);
            break;
        case kRewardKindLevel:
            zge::formatString(name, kLevelText, reward->value);
            break;
        case kRewardKindContestLevel:
            zge::formatString(name, kContestLevelText, reward->value);
            break;
        case kRewardKindEditTheme:
            zge::formatString(name, kEditThemeText, reward->value);
            break;
        case kRewardKindEditPiece:
            zge::formatString(name, kEditPieceText, reward->value);
            break;
        case kRewardKindTip:
            zge::formatString(name, kTipText, reward->value);
            break;
        case kRewardKindCharacterTip:
            zge::formatString(name, kCharacterTipText, setup.character, reward->value);
            break;
        case kRewardKindMovie:
            zge::formatString(name, kMovieText, reward->value);
            break;
        case kRewardKindCutscene:
            zge::formatString(name, kCutsceneText, reward->value);
            break;
        case kRewardKindCode:
            zge::formatString(name, kCodeText, reward->value);
            break;
        case kRewardKindFeature:
            zge::formatString(name, kFeatureText, reward->value);
            break;
        default:
            break;
        }
        if (name[0] == 0)
            return 0;
        const char *format = zge::stringTableText(zge::nameHash(name));
        if (reward->kind != kRewardKindTrick)
            zge::formatString(text, format, reward->value);
        else if (reward->value < kNamedTricks)
            // (The reward's value picks the table, the trick the name.)
            zge::formatString(text, format, (*g_trickNames)[trick]);
        else
            zge::formatString(text, format, (*g_modifierNames)[trick - kNamedTricks]);
        return 1;
    }
    READABLE_FUNCTION_WAITS(rewardText, Verified);
}
