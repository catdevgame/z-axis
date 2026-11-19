#pragma once

// The career's save data per skater (the save's skater records, 0x510 bytes
// each from g_skaterRecords, by the skater's index): from +0x2A8 the state each of
// the 14 levels was left in, which entering the level again restores
// (restoreCareerLevel, level/load.cpp) and leaving it saves (saveCareerLevel).
// Each block is a heap copy and its size. The head of a skater record is the
// skater's profile, which a player works on (Player +0x5E0) in a level.

#include "ee/ee.h"

#include <cstddef>
#include "sym/aggressive/career.h.h"

namespace aggressive::level { struct AnimObjectState; struct SavedObjective; } // (level/animated_objects.h, level/objectives.h)
namespace aggressive::script { struct SavedChildState; }                       // (script/child_states.h)

namespace aggressive
{
    struct CareerLevelState
    {
        ee::Ptr<level::AnimObjectState> objectStates; // +0x00 the script objects' states, 16 bytes each (+0 the object's name hash)
        uint32_t objectStatesSize;    // +0x04 their bytes
        ee::Ptr<uint32_t> scriptVariables; // +0x08 the level script's variables (scriptRestoreVariables)
        uint32_t scriptVariablesSize; // +0x0C
        ee::Ptr<level::SavedObjective> goals; // +0x10 the goals' state (the goals module's block, loadObjectiveState)
        uint32_t goalsSize;           // +0x14
        ee::Ptr<uint8_t> props;       // +0x18 the PropDef props' flags, a byte each (loadLevelPropState)
        uint32_t propsSize;           // +0x1C
        ee::Ptr<script::SavedChildState> visibility; // +0x20 the objects shown and hidden (restoreChildStates), and how many
        uint32_t visibilityCount;     // +0x24
        ee::Float completion;         // +0x28 (saveCareerProgress sets it from the goals)
    };
    static_assert(sizeof(CareerLevelState) == 0x2C);

    // One of the skater's seven stats (skater/experience.cpp).
    struct StatRecord
    {
        uint8_t id;       // +0 the stat's index
        uint8_t level;    // +1 0..7 (no more points at 7)
        uint8_t bonus;    // +2 added to the level: the attribute points spent
        uint8_t total;    // +3 level + bonus
        int32_t points;   // +4 toward the next level
        ee::Float totalF; // +8 the total as a float
    };
    static_assert(sizeof(StatRecord) == 12);
    constexpr uint32_t kStatCount = 7;

    // What the skater has done in one level.
    struct LevelRecord
    {
        uint32_t challengePoints; // +0x00 what its challenges are worth so far
        int32_t goalsDone;       // +0x04
        uint32_t resetWord;      // +0x08 cleared with the record; no reader found
        uint8_t collected;       // +0x0C the level's collectibles found
        uint8_t collectibles;    // +0x0D how many it has
        uint8_t juiceExtends;    // +0x0E the juice extends found
        uint8_t juiceExtendsTotal; // +0x0F how many it has
        uint32_t rewards;        // +0x10 what it has unlocked (LevelReward)
        uint32_t score;          // +0x14 (a player's score is this record's)
        int32_t rollovers;       // +0x18
        // +0x1C (added up into SkaterProfile::continues; the skater's stats
        // screen shows that as the Continue Count).
        union
        {
            int32_t continues;
            int32_t _1c;
        };
        ee::Float secondsPlayed; // +0x20
    };
    static_assert(sizeof(LevelRecord) == 0x24 && offsetof(LevelRecord, score) == 0x14 && offsetof(LevelRecord, secondsPlayed) == 0x20);

    // LevelRecord::rewards
    enum LevelReward : uint32_t
    {
        kRewardNextLevel = 0x02,
        kRewardEditorPieces = 0x04,
        kRewardCinematic = 0x08,
        kRewardSecretCharacter = 0x10,
    };

    constexpr uint32_t kProfileLevels = 14;
    constexpr uint32_t kCareerLevels = 14;
    // Levels 0 to 6 are the career's; 7 is the Tutorial.
    constexpr uint32_t kCareerLevelCount = 7;
    constexpr uint32_t kLevelTutorial = 7;

    // SkaterProfile::lootFlags: the attributes' pick-ups by stat (kLootStat*),
    // kLootUltraSkates, and the levels' keys, bit level + 9.
    constexpr uint64_t kLootUltraSkates = 0x80;

    // SkaterProfile::unlockedTricks: every rider's special trick by bit; the
    // ultra skates' one.
    constexpr uint16_t kTrickUltraSkates = 0x200;

    // A skater's profile: the head of its save record.
    struct SkaterProfile
    {
        union
        {
            struct
            {
                uint8_t index;           // +0x00 the skater (low 5 bits)
                // +0x01 what experience is multiplied by (skater/experience.cpp): 1
                // once reset, one more with each level's key.
                uint8_t experienceMultiplier;
                uint8_t _02[2];
                // +0x04 the levels open to the skater, a bit each (a new record has
                // 0x2A0; the level select reads it, a cheat sets all of it).
                union
                {
                    uint32_t openLevels;
                    uint32_t bytes;
                };
            };
            // The first doubleword as the front end reads and writes it
            // whole to test or set the flags in index's top bits (bit 5 the
            // skater was named, bit 6 a cheat was used).
            uint64_t flagsWord;
        };
        uint64_t lootFlags;      // +0x08 what was picked up (LootFlag)
        uint16_t unlockedTricks; // +0x10 tricks by bit (a trick table entry's unlockBit)
        union
        {
            uint16_t _12;              // +0x12 0x80 when reset
            uint16_t editorPieceLevels; // the levels whose editor pieces this skater has won, by bit
        };
        ee::Float completion;    // +0x14 percent: the mean of the career levels'
        ee::Float timePlayed;    // +0x18 seconds
        ee::Float pointsPerSecond; // +0x1C score over all played levels, by the time played in all
        // +0x20 what the stats screen shows as Rainfall (millimetres; inches
        // on the screen), Snowfall and Lightning Strikes. Player::levelGrowth
        // is the same three values.
        union
        {
            ee::Float levelGrowth[3];
            struct
            {
                ee::Float rainfall, snowfall, lightning;
            };
        };
        int32_t saveCount;       // +0x2C
        int32_t continues;       // +0x30 the career levels' LevelRecord::continues added up
        int32_t rollovers;       // +0x34 of the score, in the career levels
        int32_t crashCount;      // +0x38
        int32_t bestTrickString; // +0x3C points
        int32_t crittersExterminated; // +0x40
        uint8_t statTotal;       // +0x44 the stats' totals added up
        uint8_t level;           // +0x45 statTotal + levelUps * 3
        uint8_t levelUps;        // +0x46
        uint8_t _47;
        char name[16];           // +0x48
        StatRecord stats[kStatCount]; // +0x58
        LevelRecord levels[kProfileLevels]; // +0xAC
        uint8_t _2a4[4];
    };
    static_assert(offsetof(SkaterProfile, name) == 0x48 && offsetof(SkaterProfile, stats) == 0x58 && offsetof(SkaterProfile, levels) == 0xAC);
    static_assert(offsetof(SkaterProfile, rollovers) == 0x34 && offsetof(SkaterProfile, crashCount) == 0x38 && offsetof(SkaterProfile, levelUps) == 0x46);
    static_assert(sizeof(SkaterProfile) == 0x2A8);

    // A level's best-run rows: a value and the name that set it.
    struct BestRun
    {
        union
        {
            int32_t value;       // a score or a count
            uint32_t points;     // recordBestScore's
            ee::Float measure;   // updateLevelRecord's: a time or a distance
        };
        char name[16];
    };
    static_assert(sizeof(BestRun) == 0x14);
    constexpr uint32_t kBestRunsPerLevel = 14;
    using LevelBestRuns = BestRun[kBestRunsPerLevel];
    inline ee::Global<LevelBestRuns[kCareerLevels], sym::bestRunsTable> g_bestRuns;

    constexpr uint32_t kSkaterRecordSize = 0x510;
    constexpr uint32_t kSkaterRecordCount = 20;
    // The skater records' career level states (g_skaterRecords + 0x2A8).
    constexpr uint32_t kCareerLevelStates = sym::g_levelRecords;
    constexpr uint32_t kSkaterRecords = sym::g_skaterRecords;
    inline ee::Ptr<CareerLevelState> careerLevelState(uint32_t skater, uint32_t level)
    {
        return ee::Ptr<CareerLevelState>(kCareerLevelStates + skater * kSkaterRecordSize + level * static_cast<uint32_t>(sizeof(CareerLevelState)));
    }
}
