#pragma once
// BMX XXX's players and rider characters (bmx.c, players.cpp): how many
// play, whose turn it is, what each picked, and the characters' records.
// BMX XXX's own code, DM2's line (game/dm2/game/players.h).
//
// Names come from the code's use, the game's own tables (the characters'
// and the levels' names) and DM2's readable pairs; "(inferred)" marks a
// meaning read from use only.

#include "bmx/profile/player_setup.h"
#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/profile/players.h.h"

namespace zge
{
    struct ModelRecord; // zge/scene/model_records.h
}

namespace game::bmx
{
    constexpr uint32_t kMaxPlayers = 2;

    // What each player picked (profile/player_setup.h).
    inline ee::Global<PlayerSetup[kMaxPlayers], sym::g_playerSetups> g_playerSetups;
    // PlayerSetup::character of the reward text's secret look.
    constexpr uint8_t kSecretLookCharacter = 12;

    // How many play (1 at the game's start), and whose turn it is.
    inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
    inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;
    uint8_t currentPlayer();

    // The levels (the game's table of them: "The Bronx, NYC" .. "Launch Pad
    // 69", the two competitions, the secret level, the three test levels,
    // and "Mullaly Park" as its 13th row, which the characters keep nothing
    // for).
    constexpr uint32_t kLevels = 12;
    struct LevelInfo
    {
        ee::Ptr<const char> code;   // +0x00 two letters ("DM")
        ee::Ptr<const char> name;   // +0x04 its files' name ("CHL_DAM")
        ee::Ptr<const char> title;  // +0x08 ("The Dam")
        uint16_t flags;             // +0x0C (kLevelOpen)
        uint16_t gameMode;          // +0x0E the game mode it is played in: kChallengeMode (0) for the six challenge
                                    //       levels ("CHL_"), kCompetitionMode (1) for the competitions ("CMP_"),
                                    //       the secret and test levels, 2 for the two parks (the tour's start
                                    //       of a level, and the whole-game rewards' count of levels)
    };
    static_assert(offsetof(LevelInfo, flags) == 0x0C && sizeof(LevelInfo) == 0x10);
    // LevelInfo::gameMode of a challenge level (the game's first mode,
    // game/round.h's kChallengeMode).
    constexpr uint16_t kChallengeLevelMode = 0;
    // LevelInfo::flags bit 0: a new character's record of the level starts
    // with its bit 0 set too (the first level alone, of the twelve).
    constexpr uint16_t kLevelOpen = 0x1;

    // A character's record of one level (0x206 bytes): the level's goals
    // (its PERSISTENT objectives; the objectives' code keeps the counts and
    // the marks, game/bmx/objectives) and the level's own flags. A challenge
    // level's two counts start at 20.
    constexpr uint32_t kLevelObjectives = 0x200;
    struct LevelGoals
    {
        uint8_t goals;      // +0x00 how many goals the level has (PERSISTENT objectives that are not a set's children)
        uint8_t goalsDone;  // +0x01 how many of the goals that are not gaps are done
        uint8_t gaps;       // +0x02 how many goals are gaps (their names start with GAP)
        uint8_t gapsDone;   // +0x03 how many of the gaps are done
        uint8_t placeRewards; // +0x04 a competition's place rewards the character has: 3 - its best place (3 for
                              //       the first, 1 for the third), 0 for none
        uint8_t flags;      // +0x05 kLevelGoalsOpen, kLevelGoalsAllDone, kLevelGoalsContestWon, kLevelGoalsCutsceneWon
        uint8_t marks[kLevelObjectives]; // +0x06 by the objective's place in the level's file (the objectives' GoalMark)
    };
    static_assert(offsetof(LevelGoals, flags) == 0x05 && offsetof(LevelGoals, marks) == 0x06 && sizeof(LevelGoals) == 0x206);
    constexpr uint8_t kChallengeLevelCount = 20;
    // LevelGoals::flags. Open: from the level's kLevelOpen in a new record,
    // and set by a level reward. AllDone and ContestWon: set as a turn gives
    // the level's last goal reward, or its first place's (giveTurnRewards).
    // CutsceneWon: set by a cutscene reward.
    constexpr uint8_t kLevelGoalsOpen = 0x01;
    constexpr uint8_t kLevelGoalsAllDone = 0x02;
    constexpr uint8_t kLevelGoalsContestWon = 0x04;
    constexpr uint8_t kLevelGoalsCutsceneWon = 0x20;

    // A rider character's record (0x1FF4 bytes; the saved game holds 17:
    // the thirteen riders, "Twan" .. "Amish Boy", and four custom riders).
    constexpr uint32_t kRiderCharacters = 17;
    constexpr uint32_t kCharacterStats = 9;
    // The bike parts a character finds (BIKE pickups): four bikes of six
    // parts each (frame, fork, handlebars, wheel, crank, seat).
    constexpr uint32_t kCharacterBikes = 4, kBikeParts = 6;
    struct RiderCharacter
    {
        uint16_t flags;                 // +0x0000 kCharacterLocked, kCharacterWonBike, kCharacterWonLevel
        char firstName[0x20];           // +0x0002
        char lastName[0x20];            // +0x0022
        uint8_t unused;                 // +0x0042 0 in a new record (resetRiderCharacter); read nowhere
        uint8_t bikes;                  // +0x0043 the bikes it has: 1 in a new record, a bike reward for bike n makes it n + 1 at least
        uint8_t stats[kCharacterStats]; // +0x0044 nine values of 1 .. 8 from the game's table (inferred: the rider's stats)
        uint8_t ownTricks[2];           // +0x004D from the game's table: the rider's own tricks (newRider copies them to Rider::ownTricks)
        uint8_t _4f;
        // +0x0050 the rider's saved model (the game save fills it from the
        // live model and a load applies it): lists of 0x20-byte names with
        // their numbers for the model's parts, textures, lattice and
        // skeleton, then four weights. Laid out by the game save's code
        // (game/bmx/save).
        uint8_t savedModel[0x78C - 0x50];
        uint8_t bike;                   // +0x078C the character's line of bikes (0 .. 6): the first number of its bikes' names (the bike parts screen's "$KSTR_BIKENAME_%d_%d"); from the game's table
        uint8_t bikeChoice;             // +0x078D which of the line's five bikes the character rides (0 .. 4: the second
                                        //         number of "$KSTR_BIKENAME_%d_%d"; DM2's bike byte): kSavedRecordsBelow
                                        //         or more, the character's play sets no saved record
        LevelGoals levels[kLevels];     // +0x078E
        uint8_t bikePartsFound;         // +0x1FD6 how many bike parts the character has found (of 24)
        uint8_t bikeParts[kCharacterBikes][kBikeParts]; // +0x1FD7 by bike and part: kBikePartFound, the level it was
                                                        //         found in (bikePartLevel)
        uint8_t _1fef;
        ee::Ptr<zge::ModelRecord> model; // +0x1FF0 the rider's live model (a load finds it by the character's model name)
    };
    // RiderCharacter::flags. Locked: a new game sets it for the 13th rider
    // alone, "Amish Boy", and a character reward clears it. The other two
    // are set by a bike reward and a level reward; what reads them is not
    // read yet. AllGoals, AllContests: set as a turn gives the reward for
    // every challenge level's goals, or for every competition won; AllGame:
    // the reward for both (giveTurnRewards). (The game's own code reads and
    // writes these as one word.)
    constexpr uint16_t kCharacterLocked = 0x0001;
    constexpr uint16_t kCharacterWonBike = 0x0004;
    constexpr uint16_t kCharacterAllGoals = 0x0020;
    constexpr uint16_t kCharacterAllContests = 0x0040;
    constexpr uint16_t kCharacterAllGame = 0x0080;
    constexpr uint16_t kCharacterWonLevel = 0x0100;
    // RiderCharacter::bikeChoice below this: the character's play can set the
    // level's saved records (checkGameRecord; DM2's rule on its bike byte,
    // which takes the same place, but BMX XXX's bike is +0x78C).
    constexpr uint8_t kSavedRecordsBelow = 4;
    static_assert(offsetof(RiderCharacter, unused) == 0x42 && offsetof(RiderCharacter, bikes) == 0x43);
    static_assert(offsetof(RiderCharacter, lastName) == 0x22 && offsetof(RiderCharacter, stats) == 0x44);
    static_assert(offsetof(RiderCharacter, ownTricks) == 0x4D && offsetof(RiderCharacter, bike) == 0x78C);
    static_assert(offsetof(RiderCharacter, levels) == 0x78E && sizeof(RiderCharacter) == 0x1FF4);
    static_assert(offsetof(RiderCharacter, savedModel) == 0x50 && offsetof(RiderCharacter, model) == 0x1FF0);
    static_assert(offsetof(RiderCharacter, bikePartsFound) == 0x1FD6 && offsetof(RiderCharacter, bikeParts) == 0x1FD7);
    // RiderCharacter::bikeParts: a part found, marked with the level it was
    // found in (bits 2 .. 5; a pickup sets bit 1 with bit 0).
    constexpr uint8_t kBikePartFound = 0x01;
    constexpr uint32_t kBikePartLevelShift = 2;
    constexpr uint8_t kBikePartLevelMask = 0x0F;
    inline uint32_t bikePartLevel(uint8_t part) { return (part >> kBikePartLevelShift) & kBikePartLevelMask; }

    // The saved game (0x21FFC bytes): what the rewards opened for every
    // character, the language, then the characters' records. The bytes not
    // named are not read by anything written yet.
    constexpr uint32_t kSavedMovies = 20;
    struct SavedGame
    {
        uint16_t features;                              // +0x00000 a bit per feature a feature reward opened
        uint16_t featuresOn;                            // +0x00002 a bit per opened feature turned on (the extras' special
                                                        //          modes menu turns a feature's bit over)
        uint8_t flags;                                  // +0x00004 kSavedMovieWon
        uint8_t _00005;
        uint8_t editorThemes;                           // +0x00006 the park editor's themes open: a theme reward for theme n makes it n + 1 at least
        uint8_t movesLanded[0x1F - 0x07];               // +0x00007 a bit a trick ever landed (0 .. 99), then a bit a modifier
                                                        //          from bit 100 (trickLineEnd)
        uint8_t language;                               // +0x0001F (ui/strings.h)
        uint8_t channelVolumes[3];                      // +0x00020 the volumes of sound channels 0, 1 and 2 (the menus' SFX, music and speech settings)
        uint8_t _00023;
        RiderCharacter characters[kRiderCharacters];    // +0x00024
        uint8_t teamRewards;                            // +0x21F58 1 in a new game; each team reward adds one
        uint8_t teamMembers[7];                         // +0x21F59 the rider characters of the team (the menus' "TeamMemCharNum"; 0xFF each in a new game)
        uint32_t movies[kSavedMovies];                  // +0x21F60 kMovieRewardBit0, kMovieRewardBit1
        uint8_t _21fb0[0x21FFC - 0x21FB0];
    };
    static_assert(offsetof(SavedGame, featuresOn) == 0x02 && offsetof(SavedGame, flags) == 0x04 && offsetof(SavedGame, editorThemes) == 0x06);
    static_assert(offsetof(SavedGame, language) == 0x1F && offsetof(SavedGame, characters) == 0x24 && sizeof(SavedGame) == 0x21FFC);
    static_assert(offsetof(SavedGame, teamRewards) == 0x21F58 && offsetof(SavedGame, movies) == 0x21F60);
    // SavedGame::featuresOn: feature 0 (the features menu's first, $KSTR_FEATURENAME_00) lets the stick steer
    // the ghost ride (ghostRideState, its one reader). The other bits' readers are not read yet.
    constexpr uint16_t kFeatureGhostRideSteering = 0x0001;
    // SavedGame::flags: set by a movie reward. (The game's own code reads
    // and writes it as one word with the three bytes after it.)
    constexpr uint8_t kSavedMovieWon = 0x01;
    // SavedGame::movies[n]: a movie reward sets both, one after the other;
    // what tells them apart is not read yet.
    constexpr uint32_t kMovieRewardBit0 = 0x1;
    constexpr uint32_t kMovieRewardBit1 = 0x2;
    inline ee::Global<SavedGame, sym::g_savedGame> g_savedGame;

    // The character a player rides, and its record of a level.
    ee::Ptr<RiderCharacter> playerCharacter(uint32_t player);
    ee::Ptr<LevelGoals> playerLevelGoals(uint32_t player, uint32_t level);
    // A character's record as a new game has it.
    void resetRiderCharacter(uint32_t character);
    // Whether the character a player's setup picked has a female model (its
    // parts' names "GF..", a male model's "GM..").
    uint32_t isFemaleRider(ee::Ptr<const PlayerSetup> setup);
}
