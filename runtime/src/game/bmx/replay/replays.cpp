// BMX XXX's replays (replays.h): the recorder's slots and what a replay
// keeps. BMX XXX's own code (its game block, in physics.c's range), DM2's
// line (game/dm2/replay/replays.cpp).

#include "replays.h"
#include "zge/math/vec.h"
#include "zge/math/quaternion.h"

#include "bmx/game/player_riders.h"
#include "bmx/game/round.h"
#include "bmx/input/input_record.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "bmx/weapon/player_weapon.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/replay/replays.cpp.h"

namespace game::bmx
{
    namespace
    {
        // A pad's button words as the input record a replay keeps (the same layout).
        const InputRecord &inputRecordOf(const rider::PadInput *pad) { return *reinterpret_cast<const InputRecord *>(&pad->buttons); }
        // The part of the rider's record a replay keeps (up to its controller layout).
        constexpr uint32_t kRiderHeadBytes = 0x120;
        // A level's goals (bmx.c's LevelGoals), kept whole.
        // Scaffolding (raw size): until bmx.c's players header is in this tree.
        constexpr uint32_t kLevelGoalsBytes = 0x206;

        // A replay's record. Its saved part (what a memory card or the
        // game's own library of replays holds) starts at +0x240 and runs to
        // `bytes`.
        struct Replay
        {
            uint8_t state;            // +0x00 kReplay*: what the replay is doing (setReplayState)
            uint8_t flags;            // +0x01 its maker's (ReplaySetup::flags)
            uint8_t repeats;          // +0x02 the recording's current run: frames its input repeats
            uint8_t rider;            // +0x03 the rider it follows
            uint32_t weapon;          // +0x04 the rider's weapon (setReplayWeapon)
            uint32_t frame;           // +0x08 frames recorded or played since it started
            ee::Ptr<uint8_t> cursor;  // +0x0C where the recording is written or read
            uint32_t bytes;           // +0x10 the record's size, its saved part's end
            uint8_t _14[0x20 - 0x14];
            union
            {
                uint32_t lastButtons; // +0x20 an input replay: the pad's buttons as last recorded or played
                uint32_t positionKind; //      a motion replay's playback: the kind of the last position entry
                                      //       (how the rider is posed: in the air, on a rail, on the ground)
            };
            uint16_t lastSticks[2];   // +0x24 the sticks as last recorded or played
            uint8_t lastLevels[4];    // +0x28 the pad's four further inputs likewise (a recording keeps the first)
            union
            {
                // An input replay: the level's goals as a playback started, put back after it.
                uint8_t goalsBeforePlay[kLevelGoalsBytes]; // +0x2C
                // A motion replay: the rider's rows and position as the recording or the playback has
                // them, one matrix: the position is its fourth row.
                struct
                {
                    uint8_t _2c[4];
                    ee::Float rows[12];    // +0x30 a motion replay's playback matrix (not the record's own rows at +0x380)
                    ee::Float position[4]; // +0x60 its fourth row, the playback position (not the record's own at +0x370)
                } motion;
            };
            uint8_t _234[0x240 - 0x234];
            // +0x240 the saved part: the rider as the recording started and the game it was made in.
            uint8_t controllerLayout; // +0x240 the rider's (Rider::controllerLayout)
            uint8_t format;           // +0x241 ReplayFormat
            uint8_t gameMode;         // +0x242
            uint8_t sessionKind;      // +0x243
            uint8_t level;            // +0x244 the level played
            uint8_t playerWatches;    // +0x245 the player's set-up byte (PlayerSetup::watches)
            uint8_t _246[0x250 - 0x246];
            uint8_t riderHead[kRiderHeadBytes]; // +0x250 the rider record's first bytes
            ee::Float position[4];    // +0x370 the rider's position as recorded or placed: replayRiderPosition's (the motion union has its own)
            ee::Float rows[16];       // +0x380 the rider's rows likewise: replayRiderRows' (the motion union has its own)
            uint32_t dataCapacity;    // +0x3C0 the bytes its recording may take
            uint32_t recordedFrames;  // +0x3C4 counted while recording
            uint32_t goalsOffset;     // +0x3C8 where the level's goals as the recording started are kept
            uint32_t dataOffset;      // +0x3CC where its recording starts
        };
        static_assert(offsetof(Replay, rider) == 0x03 && offsetof(Replay, weapon) == 0x04 && offsetof(Replay, frame) == 0x08);
        static_assert(offsetof(Replay, cursor) == 0x0C && offsetof(Replay, bytes) == 0x10 && offsetof(Replay, lastButtons) == 0x20);
        static_assert(offsetof(Replay, lastSticks) == 0x24 && offsetof(Replay, goalsBeforePlay) == 0x2C);
        static_assert(offsetof(Replay, motion.rows) == 0x30 && offsetof(Replay, motion.position) == 0x60);
        static_assert(offsetof(Replay, controllerLayout) == 0x240 && offsetof(Replay, playerWatches) == 0x245);
        static_assert(offsetof(Replay, riderHead) == 0x250 && offsetof(Replay, position) == 0x370 && offsetof(Replay, rows) == 0x380);
        static_assert(offsetof(Replay, dataCapacity) == 0x3C0 && offsetof(Replay, dataOffset) == 0x3CC);

        // A new replay's layout: its record, then (an input replay) the
        // level's goals as the recording starts, then the recording (4 KB
        // unless asked). A motion replay's recording starts where the goals
        // would; its buffer is as big as an input replay's all the same.
        constexpr uint32_t kReplayGoalsOffset = 0x400;
        constexpr uint32_t kInputDataOffset = kReplayGoalsOffset + kLevelGoalsBytes;
        static_assert(kInputDataOffset == 0x606);
        constexpr uint32_t kDefaultReplayData = 0x1000;
        constexpr uint32_t kSavedPartOffset = offsetof(Replay, controllerLayout);

        // A recording is a run of entries: a header byte whose low five bits
        // are the frames the state before it repeated and whose high three
        // are the entry's kind, then the kind's bytes, by format (this
        // table). The last kind ends a recording.
        constexpr uint8_t kRepeatsMask = 0x1F;
        constexpr uint32_t kEntryKinds = 8;
        constexpr uint32_t kEndEntry = 7;
        constexpr uint32_t kEntryKindShift = 5;
        const ee::Ptr<uint8_t[kEntryKinds]> kReplayEntryBytes(sym::replayEntryBytes);
        // The rider's position recorded with an entry (kReplayRecordsPositions): three floats.
        constexpr uint32_t kPositionBytes = 12;

        // The recorder's slots: a replay's record, or 0 for a free slot.
        constexpr uint32_t kReplaySlots = 0x10;
        inline ee::Global<ee::Ptr<Replay>[kReplaySlots], sym::g_replays> g_replays;

        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        ee::Ptr<Replay> replayAt(uint8_t replay)
        {
            return (*g_replays)[replay];
        }

        // The rider put where the recording started.
        void placeRider(rider::Rider *rider, const Replay *replay)
        {
            *rider::vec4(rider->position) = *rider::vec4(replay->position);
            *rider::matrix(rider->right) = *rider::matrix(replay->rows);
        }

        // A motion replay's rows and position start as the recording's (the
        // rows' fourth goes where the position then does).
        void startMotion(Replay *replay)
        {
            *rider::matrix(replay->motion.rows) = *rider::matrix(replay->rows);
            *rider::vec4(replay->motion.position) = *rider::vec4(replay->position);
        }
    }

    uint32_t allocReplayBuffer(uint32_t bytes)
    {
        return zge::heapAllocate(bytes);
    }
    READABLE_FUNCTION(allocReplayBuffer, Verified);

    void freeReplayBuffer(ee::Address buffer)
    {
        game::tail(zge::heapFree, buffer);
    }
    READABLE_FUNCTION(freeReplayBuffer, Verified);

    void clearReplays()
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
            (*g_replays)[slot] = ee::Ptr<Replay>();
    }
    READABLE_FUNCTION(clearReplays, Verified);

    // freeReplays: every replay freed (the level's end).
    void freeReplays()
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
        {
            if ((*g_replays)[slot])
                freeReplay(static_cast<uint8_t>(slot));
        }
    }
    READABLE_FUNCTION(freeReplays, Verified);

    // freeReplay: a replay's record freed and its slot emptied.
    void freeReplay(uint8_t replay)
    {
        freeReplayBuffer(replayAt(replay));
        (*g_replays)[replay] = ee::Ptr<Replay>();
    }
    READABLE_FUNCTION(freeReplay, Verified);

    // newReplay: a replay made in the first free slot, its record in a buffer
    // big enough for the recording asked for (4 KB when 0). An input replay
    // of the challenge's own (not one that plays only or a multiplayer
    // set-up's) keeps the level's goals as it starts. Returns its slot, or
    // kNoReplay when no slot (or no buffer) is left.
    uint8_t newReplay(ee::Ptr<ReplaySetup> setup)
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
        {
            if ((*g_replays)[slot])
                continue;
            const uint32_t dataBytes = setup->dataBytes == 0 ? kDefaultReplayData : (setup->dataBytes + 3) & ~3u;
            const ee::Ptr<Replay> replay(allocReplayBuffer(dataBytes + kInputDataOffset));
            if (!replay)
                continue; // the next free slot tries again
            uint32_t dataOffset = kReplayGoalsOffset;
            replay->format = setup->format;
            replay->flags = setup->flags;
            replay->state = kReplayIdle;
            replay->rider = setup->rider;
            replay->recordedFrames = 0;
            replay->dataCapacity = dataBytes;
            replay->goalsOffset = kReplayGoalsOffset;
            if (setup->format == kInputReplay)
            {
                dataOffset = kInputDataOffset;
                if (gameMode() == kChallengeMode && (setup->flags & (kReplayPlaysOnly | kReplayMultiplayerSetUp)) == 0)
                {
                    const ee::Address goals = ee::Address(replay) + replay->goalsOffset;
                    const uint8_t player = rider::riderOf(replay->rider)->player;
                    zge::memoryCopy(goals, playerLevelGoals(player, *g_currentLevel), kLevelGoalsBytes);
                }
            }
            replay->dataOffset = dataOffset;
            (*g_replays)[slot] = replay;
            replay->bytes = dataOffset + dataBytes;
            return static_cast<uint8_t>(slot);
        }
        return kNoReplay;
    }
    READABLE_FUNCTION(newReplay, Verified);

    // setReplayState: a replay's state changed (the same state: nothing).
    // Leaving a recording ends it with the end entry, while that fits;
    // leaving an input replay's playback for idle or recording puts the
    // level's goals back as they were before it (the challenge's own
    // replays). Recording keeps the game and the rider as they start (not a
    // plays-only replay: its state is returned as it is); playing puts the
    // rider back as the recording started (nothing recorded: the replay goes
    // idle): an input replay's rider is spawned again (one with
    // kFlagNoPlayer is reset in place and stepped once), a motion replay's
    // reset and stepped. Returns the state set.
    uint8_t setReplayState(uint8_t slot, uint32_t state)
    {
        const ee::Ptr<Replay> replay = replayAt(slot);
        rider::Rider *const rider = rider::riderOf(replay->rider).get();
        const uint8_t old = replay->state;
        if (state == old)
            return old;
        if (old == kReplayPlaying || old == kReplayPlayedOut)
        {
            if (state < kReplayPlaying && replay->format == kInputReplay && (replay->flags & kReplayPlaysOnly) == 0 &&
                gameMode() == kChallengeMode)
                zge::memoryCopy(playerLevelGoals(rider->player, *g_currentLevel), replay->goalsBeforePlay,
                                kLevelGoalsBytes);
        }
        else if (old == kReplayRecording)
        {
            const uint32_t endBytes = kReplayEntryBytes[replay->format][kEndEntry] + 1u;
            const ee::Ptr<uint8_t> at = replay->cursor;
            if (ee::Address(at) < ee::Address(replay) + replay->dataOffset + replay->dataCapacity - endBytes)
            {
                at[0] = static_cast<uint8_t>((replay->repeats + 1) | (kEndEntry << kEntryKindShift));
                replay->cursor = at + 1;
            }
        }

        if (state == kReplayRecording)
        {
            if ((replay->flags & kReplayPlaysOnly) != 0)
                return replay->state;
            replay->level = *g_currentLevel;
            replay->sessionKind = *g_sessionKind;
            replay->gameMode = gameMode();
            replay->controllerLayout = rider->controllerLayout;
            if (!rider::hasFlag(rider, rider::kFlagNoPlayer))
                replay->playerWatches = playerSetup(rider->player)->watches;
            zge::memoryCopy(replay->riderHead, rider, kRiderHeadBytes);
            *rider::vec4(replay->position) = *rider::vec4(rider->position);
            *rider::matrix(replay->rows) = *rider::matrix(rider->right);
            if (gameMode() == kChallengeMode)
            {
                const ee::Address goals = ee::Address(replay) + replay->goalsOffset;
                zge::memoryCopy(goals, playerLevelGoals(rider->player, *g_currentLevel), kLevelGoalsBytes);
            }
            if (replay->format == kInputReplay)
            {
                replay->lastButtons = 0;
                for (uint32_t i = 0; i < 2; ++i)
                    replay->lastSticks[i] = 0;
                for (uint32_t i = 0; i < sizeof(replay->lastLevels); ++i)
                    replay->lastLevels[i] = 0;
            }
            else if (replay->format == kMotionReplay)
            {
                startMotion(replay.get());
            }
            replay->frame = 0;
            replay->recordedFrames = 0;
            replay->cursor = ee::Ptr<uint8_t>(ee::Address(replay) + replay->dataOffset);
            replay->repeats = 0;
        }
        else if (state == kReplayPlaying)
        {
            if (replay->recordedFrames == 0)
            {
                state = kReplayIdle;
            }
            else
            {
                zge::memoryCopy(rider, replay->riderHead, kRiderHeadBytes);
                if (replay->format == kInputReplay)
                {
                    rider->controllerLayout = replay->controllerLayout;
                    if (replay->state < kReplayPlaying && gameMode() == kChallengeMode)
                        zge::memoryCopy(replay->goalsBeforePlay, playerLevelGoals(rider->player, *g_currentLevel),
                                        kLevelGoalsBytes);
                    if (gameMode() == kChallengeMode && (replay->flags & kReplayPlaysOnly) == 0)
                    {
                        const ee::Address goals = playerLevelGoals(rider->player, *g_currentLevel);
                        zge::memoryCopy(goals, ee::Address(replay) + replay->goalsOffset, kLevelGoalsBytes);
                    }
                    placeRider(rider, replay.get());
                    if (!rider::hasFlag(rider, rider::kFlagNoPlayer))
                    {
                        playerSetup(rider->player)->watches = replay->playerWatches;
                        spawnPlayer(rider->player, 1);
                    }
                    else
                    {
                        rider::riderReset(replay->rider, 1);
                        rider::riderFrame(replay->rider);
                    }
                    replay->lastButtons = 0;
                    for (uint32_t i = 0; i < 2; ++i)
                        replay->lastSticks[i] = 0;
                }
                else if (replay->format == kMotionReplay)
                {
                    placeRider(rider, replay.get());
                    rider::riderReset(replay->rider, 1);
                    rider::riderFrame(replay->rider);
                    startMotion(replay.get());
                    replay->lastButtons = 3;
                }
                replay->frame = 0;
                replay->cursor = ee::Ptr<uint8_t>(ee::Address(replay) + replay->dataOffset);
                replay->repeats = replay->cursor[0] & kRepeatsMask;
            }
        }
        replay->state = static_cast<uint8_t>(state);
        return static_cast<uint8_t>(state);
    }
    READABLE_FUNCTION(setReplayState, Verified);

    uint8_t replayState(uint8_t replay)
    {
        return replayAt(replay)->state;
    }
    READABLE_FUNCTION(replayState, Verified);

    uint8_t setReplayRider(uint8_t replay, uint8_t rider)
    {
        replayAt(replay)->rider = rider;
        return rider;
    }
    READABLE_FUNCTION(setReplayRider, Verified);

    uint8_t replayRider(uint8_t replay)
    {
        return replayAt(replay)->rider;
    }
    READABLE_FUNCTION(replayRider, Verified);

    uint32_t setReplayWeapon(uint8_t replay, uint32_t weapon)
    {
        replayAt(replay)->weapon = weapon;
        return weapon;
    }
    READABLE_FUNCTION(setReplayWeapon, Verified);

    uint32_t replayWeapon(uint8_t replay)
    {
        return replayAt(replay)->weapon;
    }
    READABLE_FUNCTION(replayWeapon, Draft);

    uint8_t replayPlayerWatches(uint8_t replay)
    {
        return replayAt(replay)->playerWatches;
    }
    READABLE_FUNCTION(replayPlayerWatches, Verified);

    // replayRiderHead, replayRiderPosition, replayRiderRows: the rider as the
    // recording started (its record's first bytes, its position, its rows).
    ee::Ptr<uint8_t> replayRiderHead(uint8_t replay)
    {
        return ee::Ptr<uint8_t>(replayAt(replay)->riderHead);
    }
    READABLE_FUNCTION(replayRiderHead, Draft);

    ee::Ptr<ee::Float> replayRiderPosition(uint8_t replay)
    {
        return ee::Ptr<ee::Float>(replayAt(replay)->position);
    }
    READABLE_FUNCTION(replayRiderPosition, Verified);

    ee::Ptr<ee::Float> replayRiderRows(uint8_t replay)
    {
        return ee::Ptr<ee::Float>(replayAt(replay)->rows);
    }
    READABLE_FUNCTION(replayRiderRows, Verified);

    uint8_t replayFormat(uint8_t replay)
    {
        return replayAt(replay)->format;
    }
    READABLE_FUNCTION(replayFormat, Verified);

    uint8_t replayLevel(uint8_t replay)
    {
        return replayAt(replay)->level;
    }
    READABLE_FUNCTION(replayLevel, Draft);

    // replaySavedPart: where a replay's saved part starts and its bytes.
    void replaySavedPart(uint8_t replay, ee::Ptr<uint32_t> start, ee::Ptr<uint32_t> bytes)
    {
        const ee::Ptr<Replay> record = replayAt(replay);
        *start = ee::Address(record) + kSavedPartOffset;
        *bytes = record->bytes - kSavedPartOffset;
    }
    READABLE_FUNCTION(replaySavedPart, Verified);

    // readLibraryReplay: the saved part read, then what a saved part does not
    // hold set as newReplay sets it: where the goals and the recording are,
    // and the record's size. Two empty loops (9 and 4 steps) follow in the
    // original, code compiled away.
    void readLibraryReplay(ee::Ptr<zge::ZfileLibrary> library, ee::Address savedPart)
    {
        zge::readZfileLibraryAsset(library, savedPart);
        const ee::Ptr<Replay> replay(savedPart - kSavedPartOffset);
        replay->goalsOffset = kReplayGoalsOffset;
        const uint32_t dataOffset = replay->format == kInputReplay ? kInputDataOffset : kReplayGoalsOffset;
        replay->dataOffset = dataOffset;
        replay->bytes = dataOffset + replay->dataCapacity;
    }
    READABLE_FUNCTION(readLibraryReplay, Verified);

    // recordReplayPosition: the rider's drawn position (its model's) written
    // at the cursor, byte by byte (the recording is not aligned).
    void recordReplayPosition(ee::Ptr<Replay> replay)
    {
        ee::Local<zge::Vec4> position;
        rider::Rider *const rider = rider::riderOf(replay->rider).get();
        *position = *zge::objectPosition(rider->messenger);
        const uint8_t *const bytes = ee::pointerAt<uint8_t>(position.address());
        for (uint32_t lane = 0; lane < kPositionBytes / 4; ++lane)
        {
            const ee::Ptr<uint8_t> at = replay->cursor;
            for (uint32_t i = 0; i < 4; ++i)
                at[i] = bytes[lane * 4 + i];
            replay->cursor = at + 4;
        }
    }
    READABLE_FUNCTION(recordReplayPosition, Draft);

    // skipReplayPosition: a playback past the position recorded with an
    // entry. The recorded position and the rider's drawn position are both
    // fetched and compared, but what a difference did was compiled out of
    // this build: nothing comes of it.
    void skipReplayPosition(ee::Ptr<Replay> replay)
    {
        ee::Local<uint8_t[kPositionBytes]> recorded;
        ee::Local<zge::Vec4> drawn;
        for (uint32_t lane = 0; lane < kPositionBytes / 4; ++lane)
        {
            const ee::Ptr<uint8_t> at = replay->cursor;
            for (uint32_t i = 0; i < 4; ++i)
                recorded[lane * 4 + i] = at[i];
            replay->cursor = at + 4;
        }
        rider::Rider *const rider = rider::riderOf(replay->rider).get();
        *drawn = *zge::objectPosition(rider->messenger);
    }
    READABLE_FUNCTION(skipReplayPosition, Draft);

    // beginReplayEntry: the header fits when the whole entry does (with the
    // rider's position, where an input replay records it: the position
    // follows the header at once).
    bool beginReplayEntry(uint8_t slot, uint32_t kind)
    {
        const ee::Ptr<Replay> replay = replayAt(slot);
        const uint32_t entryBytes = kReplayEntryBytes[replay->format][kind];
        uint32_t bytes = entryBytes + 1;
        if (replay->format == kInputReplay && (replay->flags & kReplayRecordsPositions) != 0)
            bytes = entryBytes + 1 + kPositionBytes;
        const ee::Ptr<uint8_t> at = replay->cursor;
        if (ee::Address(at) < ee::Address(replay) + replay->dataOffset + replay->dataCapacity - bytes)
        {
            at[0] = static_cast<uint8_t>(replay->repeats | (kind << kEntryKindShift));
            replay->cursor = at + 1;
            if (replay->format == kInputReplay && (replay->flags & kReplayRecordsPositions) != 0)
                recordReplayPosition(replay);
            replay->repeats = 0;
            return true;
        }
        setReplayState(slot, kReplayIdle);
        return false;
    }
    READABLE_FUNCTION(beginReplayEntry, Verified);

    namespace
    {
        // The entries of an input recording: the buttons that changed, a
        // stick's new place, the first further input's new level.
        constexpr uint32_t kButtonsEntry = 0, kLeftStickEntry = 1, kLevelEntry = 3;
        // The entries of a motion recording: the rider's rows as a
        // quaternion; the position, its kind saying how the rider is posed
        // there; the motion's name.
        constexpr uint32_t kRowsEntry = 0, kAirPositionEntry = 1, kRailPositionEntry = 2, kGroundPositionEntry = 3, kMotionEntry = 4;
        // A run of frames with no entry ends here: an empty entry is written.
        constexpr uint8_t kMaxRepeats = 0x1F;
        // A stick or a level is recorded in 127ths.
        constexpr ee::Float kSteps = 127.0f;
        constexpr ee::Float kStep = 1.0f / 127.0f;
        // A motion recording writes a position that moved this far (squared)
        // and rows when the up or the forward row moved this far (squared).
        constexpr ee::Float kPositionMoved = 0.01f, kRowMoved = 0.0025f;
        constexpr ee::Float kFrameSeconds = 1.0f / 60.0f;
        // A motion recording writes the motion's name when the rider's
        // motion restarted in the last motion step
        // (rider::kFlagMotionRestarted, stepRiderMotion's).

        // Where a replay's recording ends.
        uint32_t recordingEnd(ee::Ptr<Replay> replay)
        {
            return ee::Address(replay) + replay->dataOffset + replay->dataCapacity;
        }

        // `count` bytes written at, or read from, the cursor (the recording
        // is not aligned).
        void writeBytes(ee::Ptr<Replay> replay, const void *from, uint32_t count)
        {
            const uint8_t *const bytes = static_cast<const uint8_t *>(from);
            const ee::Ptr<uint8_t> at = replay->cursor;
            for (uint32_t i = 0; i < count; ++i)
                at[i] = bytes[i];
            replay->cursor = at + count;
        }
        void readBytes(ee::Ptr<Replay> replay, void *to, uint32_t count)
        {
            uint8_t *const bytes = static_cast<uint8_t *>(to);
            const ee::Ptr<uint8_t> at = replay->cursor;
            for (uint32_t i = 0; i < count; ++i)
                bytes[i] = at[i];
            replay->cursor = at + count;
        }

        // A stick as its recorded steps (the high byte x, the low y), and back.
        uint32_t stepsOf(const rider::Stick &stick)
        {
            const uint32_t x = static_cast<uint32_t>(ee::toInt(stick.x * kSteps));
            const uint32_t y = static_cast<uint32_t>(ee::toInt(stick.y * kSteps));
            return ((x & 0xFFu) << 8) | (y & 0xFFu);
        }
        rider::Stick stickOf(uint16_t steps)
        {
            rider::Stick stick;
            stick.x = ee::toFloat(static_cast<int8_t>(steps >> 8)) * kStep;
            stick.y = ee::toFloat(static_cast<int8_t>(steps)) * kStep;
            return stick;
        }

        // An input recording's frame: an entry for each change of the
        // rider's pad. A pad that gives no input is not recorded.
        void recordInput(uint8_t slot, ee::Ptr<Replay> replay, rider::Rider *rider)
        {
            rider::PadInput *const pad = rider::padInput(rider->player);
            if ((pad->flags & rider::kPadMissing) != 0)
                return;
            const InputRecord &input = inputRecordOf(pad);
            if (input.buttons.held != replay->lastButtons)
            {
                if (beginReplayEntry(slot, kButtonsEntry))
                {
                    const uint16_t changed = static_cast<uint16_t>(input.buttons.held ^ replay->lastButtons);
                    writeBytes(replay, &changed, 2);
                }
                replay->lastButtons = input.buttons.held;
            }
            for (uint32_t stick = 0; stick < 2; ++stick)
            {
                const uint16_t steps = static_cast<uint16_t>(stepsOf(input.buttons.sticks[stick]));
                if (steps == replay->lastSticks[stick])
                    continue;
                if (beginReplayEntry(slot, kLeftStickEntry + stick))
                    writeBytes(replay, &steps, 2);
                replay->lastSticks[stick] = steps;
            }
            const uint8_t level = static_cast<uint8_t>(ee::toInt(input.levels[0] * kSteps));
            if (level != replay->lastLevels[0])
            {
                if (beginReplayEntry(slot, kLevelEntry))
                    writeBytes(replay, &level, 1);
                replay->lastLevels[0] = level;
            }
        }

        // The kind a position is recorded as, by what the rider is doing:
        // on the ground the drawn position is recorded, else the physics'.
        uint32_t positionKindOf(uint8_t physicsState)
        {
            switch (physicsState)
            {
            case rider::kPhysicsGround:
            case 9: // (BMX XXX's own states 9 and 11 have no name yet)
            case rider::kPhysicsManual:
                return kGroundPositionEntry;
            case rider::kPhysicsGrindApproach:
            case rider::kPhysicsGrind:
            case rider::kPhysicsStallApproach:
            case rider::kPhysicsStall:
            case 7: // (DM2's state 7, not named here yet)
            case 11:
                return kRailPositionEntry;
            default:
                return kAirPositionEntry;
            }
        }
    }

    // replayUpdate: every replay's frame.
    //
    // A recording replay counts the frame. An input recording writes an
    // entry for each change of its rider's pad (the buttons, each stick and
    // the first further input in 127ths). A motion recording writes the
    // rider's position when it has moved a tenth of a unit, its rows (as a
    // quaternion) when the up or the forward row has turned a twentieth,
    // and the motion's name when a motion has just started. Either writes
    // an empty entry after 31 frames without one.
    //
    // A playing replay with frames recorded: an input replay feeds the
    // rider's input record (the weapon's, for a player who watches) from the
    // recording: the sticks and levels as last frame's, the entries due
    // this frame, then the buttons, sticks and levels they leave. A motion
    // replay takes the entries due, puts the rider's rows and position
    // where they say, steps its motion and poses it by the last position's
    // kind. At its last recorded frame a replay is played out.
    //
    // Kept as it is: the rows' entry copies the rider's whole matrix over
    // the replay's rows and position (the position gets the matrix's fourth
    // row), and a position read back keeps whatever fourth float the frame
    // held.
    void replayUpdate()
    {
        ee::Local<zge::Vec4> moved;
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> upMoved;
        ee::Local<zge::Vec4> forwardMoved;
        ee::Local<zge::Matrix> rows;
        ee::Local<zge::Quaternion> turn;
        for (uint32_t index = 0; index < kReplaySlots; ++index)
        {
            const uint8_t slot = static_cast<uint8_t>(index);
            const ee::Ptr<Replay> replay = (*g_replays)[index];
            if (!replay)
                continue;
            if (replay->state == kReplayRecording)
            {
                rider::Rider *const rider = rider::riderOf(replay->rider).get();
                ++replay->recordedFrames;
                ++replay->frame;
                ++replay->repeats;
                if (replay->format == kInputReplay)
                    recordInput(slot, replay, rider);
                else if (replay->format == kMotionReplay)
                {
                    const uint32_t kind = positionKindOf(rider::physicsState(rider));
                    *position = *rider::vec4(kind == kGroundPositionEntry ? rider->drawnPosition : rider->position);
                    zge::Vec4 &recorded = *rider::vec4(replay->motion.position);
                    *moved = *position - recorded;
                    if (kPositionMoved <= zge::squaredLength(*moved))
                    {
                        if (beginReplayEntry(slot, kind))
                            writeBytes(replay, &*position, kPositionBytes);
                        recorded = *position;
                    }
                    *forwardMoved = *rider::vec4(rider->forward) - *rider::vec4(&replay->motion.rows[8]);
                    *upMoved = *rider::vec4(rider->up) - *rider::vec4(&replay->motion.rows[4]);
                    if (kRowMoved <= zge::squaredLength(*forwardMoved) || kRowMoved <= zge::squaredLength(*upMoved))
                    {
                        if (beginReplayEntry(slot, kRowsEntry))
                        {
                            zge::matrixToQuaternion(&*turn, rider::matrix(rider->right));
                            writeBytes(replay, &*turn, sizeof(zge::Quaternion));
                        }
                        *rider::matrix(replay->motion.rows) = *rider::matrix(rider->right);
                    }
                    if (rider::hasFlag(rider, rider::kFlagMotionRestarted) && beginReplayEntry(slot, kMotionEntry))
                        writeBytes(replay, &rider->motionName, 4);
                }
                if (replay->repeats == kMaxRepeats)
                    beginReplayEntry(slot, kEndEntry);
            }
            else if (replay->state == kReplayPlaying)
            {
                rider::Rider *const rider = rider::riderOf(replay->rider).get();
                if (replay->recordedFrames == 0)
                    continue;
                if (replay->format == kInputReplay)
                {
                    const ee::Ptr<InputRecord> input =
                        replay->playerWatches == 1 ? ee::Ptr<InputRecord>(ee::Address(weaponInput(replay->weapon))) : ee::Ptr<InputRecord>(ee::Address(&rider->input));
                    for (uint32_t stick = 0; stick < 2; ++stick)
                        input->buttons.previous[stick] = stickOf(replay->lastSticks[stick]);
                    for (uint32_t level = 0; level < 4; ++level)
                        input->previousLevels[level] = ee::toFloat(static_cast<int32_t>(replay->lastLevels[level])) * kStep;

                    uint32_t changed = 0;
                    --replay->repeats;
                    if (replay->repeats == 0)
                    {
                        while (ee::Address(replay->cursor) < recordingEnd(replay))
                        {
                            const uint32_t kind = replay->cursor[0] >> kEntryKindShift;
                            replay->cursor = replay->cursor + 1;
                            if ((replay->flags & kReplayRecordsPositions) != 0)
                                skipReplayPosition(replay);
                            if (kind == kButtonsEntry)
                            {
                                uint16_t entry;
                                readBytes(replay, &entry, 2);
                                changed = entry;
                            }
                            else if (kind < kLevelEntry)
                                readBytes(replay, &replay->lastSticks[kind - kLeftStickEntry], 2);
                            else if (kind == kLevelEntry)
                                readBytes(replay, &replay->lastLevels[0], 1);
                            replay->repeats = replay->cursor[0] & kRepeatsMask;
                            if (replay->repeats != 0)
                                break;
                        }
                    }
                    input->buttons.changed = changed;
                    const uint32_t held = replay->lastButtons ^ changed;
                    replay->lastButtons = held;
                    input->buttons.held = held;
                    for (uint32_t stick = 0; stick < 2; ++stick)
                        input->buttons.sticks[stick] = stickOf(replay->lastSticks[stick]);
                    for (uint32_t level = 0; level < 4; ++level)
                        input->levels[level] = ee::toFloat(static_cast<int32_t>(replay->lastLevels[level])) * kStep;
                }
                else if (replay->format == kMotionReplay)
                {
                    --replay->repeats;
                    if (replay->repeats == 0)
                    {
                        zge::Vec4 &recorded = *rider::vec4(replay->motion.position);
                        while (ee::Address(replay->cursor) < recordingEnd(replay))
                        {
                            const uint32_t kind = replay->cursor[0] >> kEntryKindShift;
                            replay->cursor = replay->cursor + 1;
                            if (kind == kRowsEntry)
                            {
                                readBytes(replay, &*turn, sizeof(zge::Quaternion));
                                zge::quaternionToMatrix(&*rows, &*turn);
                                rows->translation() = recorded;
                                *rider::matrix(replay->motion.rows) = *rows;
                            }
                            else if (kind >= kAirPositionEntry && kind <= kGroundPositionEntry)
                            {
                                readBytes(replay, &*position, kPositionBytes);
                                recorded = *position;
                                replay->positionKind = kind;
                            }
                            else if (kind == kMotionEntry)
                            {
                                readBytes(replay, &rider->motionName, 4);
                                rider::riderMotionByHash(rider->motionName, &rider->motion);
                                if (rider->motion)
                                {
                                    rider->motionRate = 1.0f;
                                    rider->motionFlags = (rider->motionFlags | rider::kMotionRestart) & ~rider::kMotionFlag4;
                                }
                                else
                                    rider->motionFlags = rider->motionFlags | rider::kMotionFlag4;
                            }
                            replay->repeats = replay->cursor[0] & kRepeatsMask;
                            if (replay->repeats != 0)
                                break;
                        }
                        *rider::matrix(rider->right) = *rider::matrix(replay->motion.rows);
                        *rider::vec4(rider->rowsOrigin) = zge::kZero;
                        *rider::vec4(rider->position) = recorded;
                    }
                    rider::stepRiderMotion(rider, kFrameSeconds);
                    if (replay->positionKind == kRailPositionEntry)
                    {
                        // On a rail: the drawn rider's offset is the motion's root, mirrored, in the world.
                        zge::Vec4 &rootOffset = *rider::vec4(rider->rootOffset);
                        rootOffset = *rider::vec4(rider->motionRoot);
                        rootOffset.z = -rootOffset.z;
                        rootOffset.y = -rootOffset.y;
                        rootOffset = zge::transformed(*rider::matrix(rider->right), rootOffset);
                        rider::poseBikeAir(rider);
                    }
                    else if (replay->positionKind == kGroundPositionEntry)
                    {
                        std::memcpy(rider->body, rider->right, sizeof(rider->body));
                        *rider::vec4(rider->drawnPosition) = *rider::vec4(rider->position);
                        rider::poseBikeGround(rider);
                    }
                    else
                    {
                        *rider::vec4(rider->rootOffset) = zge::kZero;
                        rider::poseBikeAir(rider);
                    }
                }
                ++replay->frame;
                if (replay->frame == replay->recordedFrames)
                    setReplayState(slot, kReplayPlayedOut);
            }
        }
    }
    READABLE_FUNCTION_WAITS(replayUpdate, Verified);
}
