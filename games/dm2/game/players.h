#pragma once
// DM2's players (game.c, players.cpp): how many a game has, whose turn it is,
// what each picked and the rider each rides.
//
// A single-player game counts no players (multiplayer players 0) and rides
// one rider; a multiplayer game's players (two) each have theirs. The
// level's set-up makes the riders (riderSetup) and keeps their indices here;
// riderIndexOf gives a player's.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/players.h.h"

namespace game::dm2
{
    constexpr uint32_t kMaxPlayers = 2;

    // What each player picked (the front end's; inferred): the rider character
    // they ride, an index of the characters' records.
    struct PlayerSetup
    {
        uint8_t flags;            // +0x00 kPlayerTips
        uint8_t controllerLayout; // +0x01 the controller configuration (the rider's controllerLayout at its turn's start)
        uint8_t cameraView;       // +0x02 the follow camera's view the player picked (the camera's set-up takes it)
        uint8_t character;
        uint8_t outfit;           // +0x04 the front end's "CurrentRiderOutfit<player>"
        uint8_t bike;             // +0x05 the front end's "CurrentBike<player>"
        char name[0x11 - 0x06];   // +0x06 "player" from the game's start-up
    };
    static_assert(offsetof(PlayerSetup, character) == 0x03 && sizeof(PlayerSetup) == 0x11);
    static_assert(offsetof(PlayerSetup, outfit) == 0x04 && offsetof(PlayerSetup, bike) == 0x05);
    // PlayerSetup::flags: the player gets the tips (a single-player turn's
    // start then sets every tip to be shown; inferred).
    constexpr uint8_t kPlayerTips = 0x02;
    // PlayerSetup::flags: the player's pad vibrates (rumble).
    constexpr uint8_t kPlayerVibration = 0x01;
    inline ee::Global<PlayerSetup[kMaxPlayers], sym::g_playerSetups> g_playerSetups;

    // The players of a multiplayer game (0 in single player), and whose turn it is.
    inline ee::Global<uint8_t, sym::g_multiplayerPlayers> g_multiplayerPlayers;
    inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;

    // The riders (rider indices): each multiplayer player's, a single-player
    // game's, and a watched replay's (its level's set-up, menu mode
    // kMenuViewReplay, makes that one).
    inline ee::Global<uint8_t[kMaxPlayers], sym::g_playerRiders> g_playerRiders;
    inline ee::Global<uint8_t, sym::g_singlePlayerRider> g_singlePlayerRider;
    inline ee::Global<uint8_t, sym::g_replayViewRider> g_replayViewRider;

    // The rider index of a player.
    uint8_t riderIndexOf(uint32_t player);

    // The start of a rider's record (rider.h's Rider): newRider takes it
    // as it is from a request with kRiderFromHead. A replay keeps its
    // rider's (replayRiderHead); an NPC's request has only the two models'
    // names ("bmxnpcInit: failed to create NPC rider. (rider model "%s",
    // bike model "%s")"), which newRider otherwise makes itself
    // ("Rider%02d", "Bike%02d_%d").
    struct RiderHead
    {
        char riderModel[0x20]; // +0x00
        uint8_t _20[0x40];     // +0x20 the stats and the rider's own tricks (rider.h)
        char bikeModel[0x20];  // +0x60
        uint8_t _80[0xB0];
    };
    static_assert(offsetof(RiderHead, bikeModel) == 0x60 && sizeof(RiderHead) == 0x130);
    // What newRider is given: which player's rider to make and from what.
    // A player's rider takes two blocks of the player's character record;
    // with kRiderFromHead the rider's record starts as `head`.
    struct NewRider
    {
        uint8_t flags;                // +0x00 kRiderNoPlayer, kRiderFromHead
        uint8_t player;               // +0x01
        uint8_t _02[6];
        uint8_t characterBlock[0x7A]; // +0x08 the character record's bytes from +0xBE (what they hold: not read yet)
        // +0x82 the bike model and the bike (the character record's two
        // bytes at +0x138: RiderCharacter::bikeModel, bike); newRider sets
        // them from the player's set-up for a built-in character and names
        // the bike model "Bike%02d_%d" by them (the bike 3 at most).
        uint8_t bikeModel;
        uint8_t bike;
        uint8_t _84[0x90 - 0x84];
        RiderHead head;               // +0x90
    };
    static_assert(offsetof(NewRider, characterBlock) == 0x08 && offsetof(NewRider, bikeModel) == 0x82 && offsetof(NewRider, bike) == 0x83);
    static_assert(offsetof(NewRider, head) == 0x90 && sizeof(NewRider) == 0x1C0);
    // NewRider::flags: the rider is no player's (its stats are not a
    // character's: its own tricks are newRider's defaults), and its record
    // starts as the request's head.
    constexpr uint8_t kRiderNoPlayer = 0x01;
    constexpr uint8_t kRiderFromHead = 0x02;

    // A rider character's record (the riders a player picks from; 21 of
    // them). It keeps, for each of the nine levels, the state of that level's
    // goals (inferred: a replay keeps a copy of it only in a level-goals game,
    // and puts it back after playing).
    constexpr uint32_t kLevels = 9;
    // The characters' records: the seventeen the game ships and four for
    // created riders.
    constexpr uint32_t kCharacters = 21;
    constexpr uint32_t kBuiltInCharacters = 17;
    // A goal of a level's challenge: its flags (kGoalRevealed, kGoalDone,
    // kGoalGiven, kGoalObjectivesMet) and two bytes of
    // four-bit parts (each a count in its low three bits and a mark in the
    // fourth; what the parts are is the goals' code's, not read yet).
    struct ChallengeGoal
    {
        uint8_t flags;
        uint8_t parts[2];
    };
    static_assert(sizeof(ChallengeGoal) == 3);
    constexpr uint8_t kGoalRevealed = 0x80; // its text is shown (else "KSTRSP_GOAL_UNKNOWN")
    constexpr uint8_t kGoalDone = 0x40;     // crossed out on the objectives screen
    // Given to the player by its NPC (giveGoal sets it with kGoalRevealed);
    // a challenge done with one earns its reward (inferred from the rewards'
    // checks).
    constexpr uint8_t kGoalGiven = 0x20;
    // The low five bits: how many of the goal's objectives are met (goalsFrame counts them up as each is met; the
    // goal is done when they reach its definition's objective count).
    constexpr uint8_t kGoalObjectivesMet = 0x1F;
    // The goals each challenge has room for, and the challenges a level has
    // (ROOKIE, AMATEUR, PRO, HARDCORE, EXPERT).
    constexpr uint32_t kChallengeGoals = 4;
    constexpr uint32_t kLevelChallenges = 5;
    struct LevelChallenge
    {
        ChallengeGoal goals[kChallengeGoals];
    };
    static_assert(sizeof(LevelChallenge) == 0x0C);
    struct LevelGoals
    {
        LevelChallenge challenges[kLevelChallenges]; // +0x00
        uint8_t flags;      // +0x3C LevelGoalsFlags
        uint8_t challenge;  // +0x3D which of the level's five challenges is next (0 .. 4)
        uint8_t goalsDone;  // +0x3E the current challenge's goals done: its count of them completes it (inferred)
        uint8_t medal;      // +0x3F the level's competition medal (LevelMedal)
        uint32_t gapsFound; // +0x40 a bit per gap of the level the character has found
    };
    static_assert(offsetof(LevelGoals, flags) == 0x3C && offsetof(LevelGoals, medal) == 0x3F);
    static_assert(offsetof(LevelGoals, goalsDone) == 0x3E);
    static_assert(offsetof(LevelGoals, gapsFound) == 0x40 && sizeof(LevelGoals) == 0x44);
    // LevelGoals::flags, as the front end's level choice reads them
    // (bmxSetupLevelStats): the level is open to the character, its
    // competition is open, and its challenges are all done.
    enum LevelGoalsFlags : uint8_t
    {
        kLevelOpen = 0x01,
        kLevelCompetitionOpen = 0x02,
        kLevelChallengesDone = 0x10,
    };
    // LevelGoals::medal ("None", "Bronze", "Silver", "Gold" in the level choice).
    enum LevelMedal : uint8_t
    {
        kMedalNone = 0,
        kMedalBronze = 1,
        kMedalSilver = 2,
        kMedalGold = 3,
    };
    // The levels with respect to earn: 5,000 each, 40,000 a character.
    constexpr uint32_t kRespectLevels = 8;
    constexpr uint32_t kSponsors = 8;
    constexpr uint32_t kCharacterTricks = 2;
    // Create Player's outfit types, and the bytes a created character's look
    // has in each (menu/player_creator.h).
    constexpr uint32_t kCreatorOutfitTypes = 2;
    constexpr uint32_t kCreatorLookBytes = 13;
    struct RiderCharacter
    {
        uint32_t flags;             // +0x000 (kCharacterSetupBits cleared at a game's set-up)
        // +0x004 the character's names, from the game's table: "Dave",
        // "Mirra", "Miracle Boy" ("Created", "Rider", "Created Rider" for
        // the last four records, the created riders').
        char firstName[0x20];
        char lastName[0x20];
        char nickname[0x20];
        uint8_t openBikes;          // +0x064 how many of the character's five bikes are open (1 at first)
        // +0x065 how many of the character's looks are open (1 at first, 2
        // at most: a reward's "KSTRSP_REWARD_LOOK%02d" opens the next);
        // below 2 the rider choice disables its "EnableCompetition" entry.
        uint8_t openLooks;
        uint8_t _066[0x68 - 0x66];
        // +0x068 by sponsor number: non-zero, the rider choice shows that
        // sponsor's logo (it shows 1 .. 8; the cheats set [0] with them).
        uint32_t sponsorShown[kSponsors + 1];
        uint8_t attributePoints;    // +0x08C the attribute points to spend on the stats (a reward's
                                    //        "KSTRSP_REWARD_ATTR00" adds them)
        uint8_t _08d[0x90 - 0x8D];
        uint32_t levelRespect[kRespectLevels]; // +0x090 the respect earned in each level
        uint32_t respect;           // +0x0B0 the character's respect in all
        uint8_t stats[8];           // +0x0B4 the character's eight stats (bar points; the game's table at first)
        uint8_t tricks[kCharacterTricks]; // +0x0BC the two tricks picked in the front end's trick menu (trick numbers)
        // +0x0BE a created character's look in each of Create Player's outfit
        // types (menu/player_creator.h: the bytes by CreatorLookByte).
        uint8_t creatorLooks[kCreatorOutfitTypes][kCreatorLookBytes];
        uint8_t _0d8[0x138 - 0xD8];
        // +0x138 its bike's model: the first number of the bike model's name
        // "Bike%02d_%d" (newRider), the second being the bike below.
        uint8_t bikeModel;
        // +0x139 the bike the first player took it into a ProQuest or
        // competition game with (BmxStartGame stores "CurrentBike1" here).
        uint8_t bike;
        uint8_t _13a[0x13C - 0x13A];
        LevelGoals levels[kLevels]; // +0x13C
        uint8_t tricksOff[0x14];    // +0x3A0 one bit a trick: set, the character cannot do it (a turn's start
                                    //        copies them to the rider's tricksOff)
        // +0x3B4 one bit a trick (0 .. 0x47) and, from bit 0x48, a modifier:
        // set once the character has landed it (the score's end of a run).
        uint8_t movesLanded[0x14];
    };
    static_assert(offsetof(RiderCharacter, openBikes) == 0x64 && offsetof(RiderCharacter, sponsorShown) == 0x68);
    static_assert(offsetof(RiderCharacter, openLooks) == 0x65 && offsetof(RiderCharacter, attributePoints) == 0x8C);
    static_assert(offsetof(RiderCharacter, levelRespect) == 0x90 && offsetof(RiderCharacter, respect) == 0xB0);
    static_assert(offsetof(RiderCharacter, firstName) == 0x04 && offsetof(RiderCharacter, nickname) == 0x44);
    static_assert(offsetof(RiderCharacter, stats) == 0xB4);
    static_assert(offsetof(RiderCharacter, tricks) == 0xBC && offsetof(RiderCharacter, bike) == 0x139);
    static_assert(offsetof(RiderCharacter, creatorLooks) == 0xBE);
    // RiderCharacter::flags: the rider choice does not offer the character
    // yet (the front end's bmxSetupUnLockPlayer reads it for the two hidden
    // riders; two of the cheat codes clear it).
    constexpr uint32_t kCharacterLocked = 0x1;
    // RiderCharacter::flags: the character's movie is open in the front end's
    // movie list (bmxmenuGameMovieUpdate shows its name plainly; the cheats set it).
    constexpr uint32_t kCharacterMovieOpen = 0x2;
    static_assert(offsetof(RiderCharacter, levels) == 0x13C && sizeof(RiderCharacter) == 0x3C8);
    static_assert(offsetof(RiderCharacter, tricksOff) == 0x3A0 && offsetof(RiderCharacter, movesLanded) == 0x3B4);
    // RiderCharacter::flags a reward sets as it opens a bike, a look or a
    // level (giveReward); a multiplayer game's set-up clears them for each
    // player. The game over menu offers the bike shop for the first two;
    // nothing reads the level's.
    constexpr uint32_t kCharacterNewBike = 0x1000;
    constexpr uint32_t kCharacterNewLook = 0x2000;
    constexpr uint32_t kCharacterNewLevel = 0x4000;
    constexpr uint32_t kCharacterSetupBits = kCharacterNewBike | kCharacterNewLook | kCharacterNewLevel;

    // The character a player rides, and its record of a level's goals.
    ee::Ptr<RiderCharacter> playerCharacter(uint32_t player);
    ee::Ptr<LevelGoals> playerLevelGoals(uint32_t player, uint32_t level);

    // RiderCharacter::movesLanded: a trick's bit is its number, a modifier's
    // its number plus this.
    constexpr uint32_t kFirstModifierBit = 0x48;
    // LevelGoals::flags: every gap of the level found.
    constexpr uint8_t kLevelAllGapsFound = 0x40;

    // The game's table of its built-in characters' names, by character
    // number (initGame copies them into the characters; the high scores
    // show the last name).
    struct CharacterNames
    {
        ee::Ptr<const char> firstName;
        ee::Ptr<const char> lastName;
        ee::Ptr<const char> nickname;
        ee::Ptr<const char> fullName; // "Dave Mirra" (the leaderboard's)
    };
    static_assert(sizeof(CharacterNames) == 0x10);
    constexpr ee::Ptr<CharacterNames> kCharacterNames{sym::g_characterNames};

    // RiderCharacter::bike below this: the character's play can set the
    // level's saved records (not on the fifth bike; the record check).
    constexpr uint8_t kSavedRecordsBelow = 4;
}
