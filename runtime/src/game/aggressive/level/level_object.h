#pragma once

// The level object and its players, the way the game reaches them: the
// top-level state record's +0x10 is the level object, whose +0x10 starts the
// players.

#include "aggressive/skater/skater.h"

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/level_object.h.h"

namespace aggressive
{
    // The level being played.
    struct LevelObject
    {
        union
        {
            // +0x00 kLevelLoading, kLevelStopReason, kLevelStoppedBy. The
            // original's bit fields sit in a doubleword, so a change to one
            // rewrites roundKind and mode with what they held.
            uint64_t flags;
            struct
            {
                uint8_t _00[2];
                uint8_t roundKind; // +0x02 the split-screen round picked (by score when 0, kCardGameRound, kAnimalRescueRound)
                uint8_t _03;
                uint32_t mode;     // +0x04 the play mode's record, cleared while loading
            };
        };
        uint32_t runTimer;         // +0x08 the timed run's or round's timer, 0 for none
        uint32_t _0c;
        skater::Player players[2]; // +0x10
    };
    static_assert(offsetof(LevelObject, roundKind) == 2 && offsetof(LevelObject, mode) == 4 && offsetof(LevelObject, runTimer) == 8);
    static_assert(offsetof(LevelObject, players) == 0x10 && sizeof(LevelObject) == 0x1270);

    constexpr uint64_t kLevelLoading = 0x02;      // LevelObject::flags: set for the length of loadLevel
    constexpr uint64_t kLevelStopReason = 0x3F00; // why play stopped: kStop* << 8
    constexpr uint64_t kLevelStoppedBy = 0xC000;  // the player who stopped it
    constexpr uint32_t kLevelStopReasonShift = 8;
    constexpr uint8_t kCardGameRound = 3, kAnimalRescueRound = 4;
}

namespace aggressive::parts
{
    // The state the game is in.
    struct TopState
    {
        uint8_t _00[0x10];
        uint32_t level; // +0x10 the level object, kept between levels
    };
    static_assert(offsetof(TopState, level) == 0x10);

    inline ee::Global<uint32_t, sym::g_topState> g_topState;
    constexpr uint32_t kTopStateLevel = offsetof(TopState, level);
    constexpr uint32_t kFirstPlayer = offsetof(LevelObject, players);
    constexpr uint32_t kPlayerBytes = sizeof(skater::Player);

    inline ee::Ptr<TopState> topState()
    {
        return ee::Ptr<TopState>(*g_topState);
    }

    inline uint32_t levelObject()
    {
        return *ee::at<uint32_t>(*g_topState + kTopStateLevel);
    }

    inline ee::Ptr<LevelObject> currentLevel()
    {
        return ee::Ptr<LevelObject>(levelObject());
    }

    // inlineGameGetSkaterHandle: player `index` of the level.
    inline uint32_t skaterHandle(int32_t index)
    {
        return levelObject() + static_cast<uint32_t>(index) * kPlayerBytes + kFirstPlayer;
    }

    inline ee::Ptr<skater::Player> levelPlayer(int32_t index)
    {
        return ee::Ptr<skater::Player>(skaterHandle(index));
    }

    // The level object's state field (bits 8-13 of its first doubleword).
    inline void setLevelState(uint64_t state)
    {
        uint64_t &word = currentLevel()->flags;
        word = (word & ~kLevelStopReason) | (state << kLevelStopReasonShift);
    }
}
