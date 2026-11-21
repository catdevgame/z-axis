// DM2's replays (replays.h): the recorder's slots and what a replay records.
// DM2's own code (its game block, in physics.c's range).

#include "replays.h"

#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/memory/regions.h"
#include "zge/math/types.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/replay/replays.cpp.h"

namespace game::dm2::rider
{
    ee::Ptr<Rider> riderOf(uint32_t index); // rider_state.cpp
    int32_t riderFrame(uint32_t player); // actions/frame.cpp
}

namespace game::dm2::rider::physics
{
    void riderReset(uint32_t index, uint32_t shown); // respawn.cpp
}
namespace game::dm2::rider
{
    PadInput *padInput(uint32_t port); // input.cpp
}

namespace game::dm2
{
    namespace
    {
        // A replay's record, as far as these read it. Its saved part (what a
        // memory card or the game's own library of replays holds) starts at
        // +0x64 and runs to `bytes`.
        // The part of the rider's record a replay keeps (up to its controller layout).
        constexpr uint32_t kRiderHeadBytes = 0x130;
        struct Replay
        {
            uint8_t state;            // +0x00 kReplay*: what the replay is doing (setReplayState)
            uint8_t repeats;          // +0x01 the recording's current run: frames its input repeats
            uint8_t flags;            // +0x02 its maker's (ReplaySetup::flags)
            uint8_t rider;            // +0x03 the rider it follows (0xFF: none, the whole round's)
            uint32_t frame;           // +0x04 frames recorded or played since it started
            ee::Ptr<uint8_t> cursor;  // +0x08 where the recording is written or read
            uint32_t lastButtons;     // +0x0C the pad's buttons as last recorded or played (replayUpdate)
            uint16_t lastSticks[2];   // +0x10 the sticks as last recorded or played: x steps in the high byte,
                                      //       y steps in the low (127 a full deflection)
            LevelGoals goalsBeforePlay; // +0x14 the level's goals as a playback started, put back after it
            uint8_t _58[0x60 - 0x58];
            uint32_t bytes;           // +0x60 the record's size, its saved part's end
            // +0x64 the saved part: the rider as the recording started and the game it was made in.
            uint8_t controllerLayout; // +0x64 the rider's (Rider::controllerLayout)
            uint8_t multiplayerMode;  // +0x65
            uint8_t singlePlayerMode; // +0x66 (setSinglePlayerMode's)
            uint8_t level;            // +0x67 the session's level
            uint8_t _68[0x70 - 0x68];
            uint8_t riderHead[kRiderHeadBytes]; // +0x70 the rider record's first bytes
            ee::Float position[4];    // +0x1A0 the rider's position (Rider +0x1040)
            ee::Float rows[16];       // +0x1B0 the rider's rows and origin (copyRotationAndTranslation's)
            uint32_t dataCapacity;    // +0x1F0 the bytes its recording may take
            uint32_t recordedFrames;  // +0x1F4 counted while recording; a playback ends at it
            uint32_t goalsOffset;     // +0x1F8 where the level's goals as the recording started are kept
            uint32_t dataOffset;      // +0x1FC where its recording starts
        };
        static_assert(offsetof(Replay, rider) == 0x03 && offsetof(Replay, bytes) == 0x60);
        static_assert(offsetof(Replay, singlePlayerMode) == 0x66 && offsetof(Replay, level) == 0x67);
        static_assert(offsetof(Replay, cursor) == 0x08 && offsetof(Replay, goalsBeforePlay) == 0x14 &&
                      offsetof(Replay, controllerLayout) == 0x64 && offsetof(Replay, riderHead) == 0x70 &&
                      offsetof(Replay, position) == 0x1A0 && offsetof(Replay, rows) == 0x1B0);
        static_assert(offsetof(Replay, dataCapacity) == 0x1F0 && offsetof(Replay, goalsOffset) == 0x1F8 &&
                      offsetof(Replay, dataOffset) == 0x1FC);
        // A new replay's layout: its record, the level's goals as the
        // recording starts, then the recording (4 KB unless asked).
        constexpr uint32_t kReplayGoalsOffset = 0x200;
        constexpr uint32_t kReplayDataOffset = kReplayGoalsOffset + sizeof(LevelGoals);
        static_assert(kReplayDataOffset == 0x244);
        constexpr uint32_t kDefaultReplayData = 0x1000;
        constexpr uint8_t kNoReplay = 0xFF;


        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        // The single-player game the session plays (a watched replay's comes from it).
        inline ee::Global<uint8_t, sym::g_sessionSinglePlayerMode> g_sessionSinglePlayerMode;
        // A recording is a run of entries of three bytes: a header whose low
        // five bits are the frames the input before it repeated (kRepeatsMask;
        // at most kMaxRepeats before an entry is forced) and whose high three
        // are the entry's kind, then two bytes: the buttons that changed (low
        // byte first), or a stick's steps (y, then x). A replay made with
        // kReplayRecordsPositions follows each entry with the rider's
        // position, twelve bytes (recordReplayPosition).
        constexpr uint8_t kRepeatsMask = 0x1F;
        constexpr uint8_t kMaxRepeats = 0x1F;
        constexpr uint32_t kEntryKindShift = 5;
        enum ReplayEntryKind : uint32_t
        {
            kButtonsEntry = 0,
            kLeftStickEntry = 1,
            kRightStickEntry = 2,
        };
        constexpr uint32_t kEntryKinds = 3;
        constexpr uint32_t kPositionBytes = 12;
        // The bytes an entry of each kind takes (three each).
        inline ee::Global<uint8_t[kEntryKinds], sym::replayEntryBytes> replayEntryBytes;
        // A stick's deflection per recorded step (1/127), kept twice.
        inline ee::Global<ee::Float, sym::replayStickUnit> replayStickUnit;
        inline ee::Global<ee::Float, sym::replayStickUnit2> replayStickUnit2;
        constexpr uint32_t kSavedPartOffset = 0x64;

        // The replays' buffers: eight of 16 KB beside the slots, each with its
        // in-use word; a bigger replay, or one past the eight, takes the heap.
        constexpr uint32_t kReplayBufferBytes = 0x4000;
        constexpr uint32_t kReplayBuffers = 8;
        struct ReplayBuffer
        {
            uint8_t bytes[kReplayBufferBytes];
            uint32_t inUse;
            uint8_t _4004[0x0C];
        };
        static_assert(offsetof(ReplayBuffer, inUse) == 0x4000 && sizeof(ReplayBuffer) == 0x4010);
        ee::Ptr<ReplayBuffer> replayBufferAt(uint32_t i)
        {
            return ee::Ptr<ReplayBuffer>(sym::g_replayBuffers) + static_cast<ptrdiff_t>(i);
        }

        // The recorder's slots: a replay's record, or 0 for a free slot.
        constexpr uint32_t kReplaySlots = 0x40;
        inline ee::Global<ee::Ptr<Replay>[kReplaySlots], sym::g_replays> g_replays;

        ee::Ptr<Replay> replayAt(uint8_t replay)
        {
            return (*g_replays)[replay];
        }
    }

    void clearReplays()
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
            (*g_replays)[slot] = ee::Ptr<Replay>();
    }
    READABLE_FUNCTION(clearReplays, Verified);

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

    // allocReplayBuffer: a free one of the replays' buffers for `bytes`, or
    // the heap's block when it is bigger or none is free.
    uint32_t allocReplayBuffer(uint32_t bytes)
    {
        if (bytes <= kReplayBufferBytes)
        {
            for (uint32_t i = 0; i < kReplayBuffers; ++i)
            {
                const ee::Ptr<ReplayBuffer> buffer = replayBufferAt(i);
                if (buffer->inUse == 0)
                {
                    buffer->inUse = 1;
                    return ee::Address(buffer);
                }
            }
        }
        return zge::heapAllocate(bytes);
    }
    READABLE_FUNCTION(allocReplayBuffer, Verified);

    // freeReplayBuffer: one of the replays' buffers given back, or the heap's
    // block freed.
    void freeReplayBuffer(ee::Address buffer)
    {
        for (uint32_t i = 0; i < kReplayBuffers; ++i)
        {
            if (buffer == ee::Address(replayBufferAt(i)))
            {
                replayBufferAt(i)->inUse = 0;
                return;
            }
        }
        game::tail(zge::heapFree, buffer);
    }
    READABLE_FUNCTION(freeReplayBuffer, Verified);

    // freeReplay: a replay's record freed and its slot emptied.
    void freeReplay(uint8_t replay)
    {
        freeReplayBuffer(replayAt(replay));
        (*g_replays)[replay] = ee::Ptr<Replay>();
    }
    READABLE_FUNCTION(freeReplay, Verified);

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

    // replayLevel, replaySinglePlayerMode: where and in what game a replay
    // was made (a watched replay's level is set up from them).
    uint8_t replayLevel(uint8_t replay)
    {
        return replayAt(replay)->level;
    }
    READABLE_FUNCTION(replayLevel, Verified);

    uint8_t replaySinglePlayerMode(uint8_t replay)
    {
        return replayAt(replay)->singlePlayerMode;
    }
    READABLE_FUNCTION(replaySinglePlayerMode, Verified);

    // replaySavedPart: where a replay's saved part starts and its bytes.
    void replaySavedPart(uint8_t replay, ee::Ptr<uint32_t> start, ee::Ptr<uint32_t> bytes)
    {
        const ee::Ptr<Replay> record = replayAt(replay);
        *start = ee::Address(record) + kSavedPartOffset;
        *bytes = record->bytes - kSavedPartOffset;
    }
    READABLE_FUNCTION(replaySavedPart, Verified);

    // readLibraryReplay: the library's current asset (a replay of the game's
    // own) read into `dst`. Two empty loops (8 and 4 steps) follow in the
    // original, code compiled away.
    void readLibraryReplay(ee::Ptr<zge::ZfileLibrary> library, ee::Address dst)
    {
        zge::readZfileLibraryAsset(library, dst);
    }
    READABLE_FUNCTION_WAITS(readLibraryReplay, Verified);

    // newReplay: a replay made in the first free slot, its record in a buffer
    // big enough for the recording asked for (4 KB when 0); in a level-goals
    // game a replay of the game's own keeps the level's goals as it starts.
    // Returns its slot, or kNoReplay when no slot (or no buffer) is left.
    uint8_t newReplay(ee::Ptr<ReplaySetup> setup)
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
        {
            if ((*g_replays)[slot])
                continue;
            const uint32_t dataBytes = setup->dataBytes == 0 ? kDefaultReplayData : (setup->dataBytes + 3) & ~3u;
            const uint32_t recordBytes = dataBytes + kReplayDataOffset;
            const ee::Ptr<Replay> replay(allocReplayBuffer(recordBytes));
            if (!replay)
                continue; // the next free slot tries again
            replay->flags = setup->flags;
            replay->goalsOffset = kReplayGoalsOffset;
            replay->dataCapacity = dataBytes;
            replay->state = kReplayIdle;
            replay->recordedFrames = 0;
            replay->rider = setup->rider;
            if (multiplayerMode() == 0 && singlePlayerMode() == kLevelGoals && (setup->flags & (kReplayPlaysOnly | kReplayMultiplayerSetUp)) == 0)
            {
                const uint8_t player = rider::riderOf(replay->rider)->player;
                zge::memoryCopy(ee::Address(replay) + replay->goalsOffset, playerLevelGoals(player, *g_sessionLevel),
                                sizeof(LevelGoals));
            }
            replay->dataOffset = kReplayDataOffset;
            (*g_replays)[slot] = replay;
            replay->bytes = recordBytes;
            return static_cast<uint8_t>(slot);
        }
        return kNoReplay;
    }
    READABLE_FUNCTION(newReplay, Verified);

    namespace
    {
        // A level-goals game: the only one whose replays keep the level's goals.
        bool levelGoalsGame()
        {
            return multiplayerMode() == 0 && singlePlayerMode() == kLevelGoals;
        }

        // A recording's or a playback's start: its counters cleared and the cursor at the recording's start.
        void rewind(ee::Ptr<Replay> replay)
        {
            replay->lastButtons = 0;
            for (uint32_t i = 0; i < 2; ++i)
                replay->lastSticks[i] = 0;
            replay->frame = 0;
        }
    }

    // setReplayState: a replay's state changed (the same state: nothing).
    // A rider with kFlagNoPlayer is reset in place and stepped once; any
    // other is spawned again.
    // Leaving a recording ends its last run; leaving a playback puts the
    // level's goals back as they were before it (a level-goals game's own
    // replays). Recording keeps the rider and the game as they start (not a
    // plays-only replay: the old state is returned); playing puts the rider back
    // as the recording started and respawns it (nothing recorded: the replay
    // goes idle). Returns the state set.
    uint8_t setReplayState(uint8_t slot, uint32_t state)
    {
        const ee::Ptr<Replay> replay = replayAt(slot);
        const uint8_t old = replay->state;
        if (state == old)
            return static_cast<uint8_t>(state);
        if (old == kReplayPlaying || old == kReplayPlayedOut)
        {
            if (state < kReplayPlaying && (replay->flags & kReplayPlaysOnly) == 0 && levelGoalsGame())
            {
                const uint8_t player = rider::riderOf(replay->rider)->player;
                zge::memoryCopy(playerLevelGoals(player, *g_sessionLevel), &replay->goalsBeforePlay, sizeof(LevelGoals));
            }
        }
        else if (old == kReplayRecording)
        {
            // The last run's byte and two zeros, while three bytes fit.
            const ee::Ptr<uint8_t> at = replay->cursor;
            if (ee::Address(at) < ee::Address(replay) + replay->dataCapacity + replay->dataOffset - 3)
            {
                at[0] = static_cast<uint8_t>(replay->repeats + 1);
                replay->cursor = at + 1;
                at[1] = 0;
                replay->cursor[1] = 0;
                replay->repeats = 0;
                replay->cursor = replay->cursor + 2;
            }
        }

        if (state == kReplayRecording)
        {
            if ((replay->flags & kReplayPlaysOnly) != 0)
                return replay->state;
            const ee::Ptr<rider::Rider> rider = rider::riderOf(replay->rider);
            replay->level = *g_sessionLevel;
            replay->singlePlayerMode = *g_sessionSinglePlayerMode;
            replay->multiplayerMode = multiplayerMode();
            replay->controllerLayout = rider->controllerLayout;
            zge::memoryCopy(&replay->riderHead, rider, kRiderHeadBytes);
            for (uint32_t i = 0; i < 4; ++i)
                replay->position[i] = rider->position[i];
            zge::copyRotationAndTranslation(&replay->rows, &rider->right);
            if (levelGoalsGame())
                zge::memoryCopy(ee::Address(replay) + replay->goalsOffset, playerLevelGoals(rider->player, *g_sessionLevel),
                                sizeof(LevelGoals));
            rewind(replay);
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
                const ee::Ptr<rider::Rider> rider = rider::riderOf(replay->rider);
                rider->controllerLayout = replay->controllerLayout;
                zge::memoryCopy(rider, &replay->riderHead, kRiderHeadBytes);
                if (replay->state < kReplayPlaying && levelGoalsGame())
                    zge::memoryCopy(&replay->goalsBeforePlay, playerLevelGoals(rider->player, *g_sessionLevel),
                                    sizeof(LevelGoals));
                if (levelGoalsGame() && (replay->flags & kReplayPlaysOnly) == 0)
                    zge::memoryCopy(playerLevelGoals(rider->player, *g_sessionLevel), ee::Address(replay) + replay->goalsOffset,
                                    sizeof(LevelGoals));
                for (uint32_t i = 0; i < 4; ++i)
                    rider->position[i] = replay->position[i];
                zge::copyRotationAndTranslation(&rider->right, &replay->rows);
                if ((rider->flags & rider::kFlagNoPlayer) == 0)
                    spawnPlayer(rider->player, 1);
                else
                {
                    rider::physics::riderReset(replay->rider, 1);
                    rider::riderFrame(replay->rider);
                }
                rewind(replay);
                replay->cursor = ee::Ptr<uint8_t>(ee::Address(replay) + replay->dataOffset);
                replay->repeats = replay->cursor[0] & kRepeatsMask;
            }
        }
        replay->state = static_cast<uint8_t>(state);
        return static_cast<uint8_t>(state);
    }
    READABLE_FUNCTION(setReplayState, Verified);

    // replayState: what a replay is doing (kReplay*).
    uint8_t replayState(uint8_t replay)
    {
        return replayAt(replay)->state;
    }
    READABLE_FUNCTION(replayState, Verified);

    // replayFrame: the frames recorded or played since it started.
    uint32_t replayFrame(uint8_t replay)
    {
        return replayAt(replay)->frame;
    }
    READABLE_FUNCTION(replayFrame, Draft);

    // replayRiderHead, replayRiderPosition, replayRiderRows: the rider as the
    // recording started (its record's first bytes, its position, its rows).
    ee::Ptr<uint8_t> replayRiderHead(uint8_t replay)
    {
        return ee::Ptr<uint8_t>(replayAt(replay)->riderHead);
    }
    READABLE_FUNCTION(replayRiderHead, Verified);

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

    // recordReplayPosition: the rider's drawn position (its model's) written
    // after an entry, byte by byte (the recording is not aligned).
    void recordReplayPosition(ee::Ptr<Replay> replay)
    {
        rider::Rider *const rider = rider::riderOf(replay->rider).get();
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(rider->messenger));
        const uint32_t lanes[3] = {position->x.bits(), position->y.bits(), position->z.bits()};
        for (const uint32_t lane : lanes)
        {
            const ee::Ptr<uint8_t> at = replay->cursor;
            at[0] = static_cast<uint8_t>(lane);
            at[1] = static_cast<uint8_t>(lane >> 8);
            at[2] = static_cast<uint8_t>(lane >> 16);
            at[3] = static_cast<uint8_t>(lane >> 24);
            replay->cursor = at + 4;
        }
    }
    READABLE_FUNCTION(recordReplayPosition, Draft);

    // skipReplayPosition: a playback past the position recorded after an
    // entry. The rider's drawn position is fetched as if to compare it, but
    // the comparison was compiled out of this build: nothing reads either.
    void skipReplayPosition(ee::Ptr<Replay> replay)
    {
        replay->cursor += kPositionBytes;
        rider::Rider *const rider = rider::riderOf(replay->rider).get();
        zge::objectPosition(ee::Ptr<zge::SceneObject>(rider->messenger));
    }
    READABLE_FUNCTION(skipReplayPosition, Draft);

    namespace
    {
        // Where a replay's recording ends: its data's offset plus its capacity.
        uint32_t recordingEnd(ee::Ptr<Replay> replay)
        {
            return ee::Address(replay) + replay->dataCapacity + replay->dataOffset;
        }

        // Whether one more entry of `kind` fits (with its position, if recorded).
        bool entryFits(ee::Ptr<Replay> replay, uint32_t kind)
        {
            uint32_t bytes = (*replayEntryBytes)[kind];
            if ((replay->flags & kReplayRecordsPositions) != 0)
                bytes += kPositionBytes;
            return ee::Address(replay->cursor) < recordingEnd(replay) - bytes;
        }

        // An entry written at the cursor: the header with the repeats so far,
        // its two bytes, then the rider's position if recorded; the repeats
        // start over.
        void writeEntry(ee::Ptr<Replay> replay, uint32_t kind, uint8_t low, uint8_t high)
        {
            const ee::Ptr<uint8_t> at = replay->cursor;
            at[0] = static_cast<uint8_t>(replay->repeats | (kind << kEntryKindShift));
            replay->cursor = at + 1;
            at[1] = low;
            at[2] = high;
            replay->cursor = at + 3;
            if ((replay->flags & kReplayRecordsPositions) != 0)
                recordReplayPosition(replay);
            replay->repeats = 0;
        }

        // A stick's recorded steps as its deflection (the high byte x, the low y).
        rider::Stick stickOf(uint16_t steps, ee::Float unit)
        {
            rider::Stick stick;
            stick.x = ee::toFloat(static_cast<int8_t>(steps >> 8)) * unit;
            stick.y = ee::toFloat(static_cast<int8_t>(steps)) * unit;
            return stick;
        }
    }

    // replayUpdate: every replay's frame. A recording replay counts the
    // frame and writes an entry for each change of its rider's pad (the
    // buttons, each stick in 127ths; a change is also forced every 31 frames),
    // stopping (idle) when its recording is full. A playing replay feeds its
    // rider's input record from the recording: the sticks last frame, the
    // entries due this frame (a run of them until a header with repeats),
    // then the buttons and sticks they leave; at its last recorded frame it
    // is played out.
    void replayUpdate()
    {
        for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
        {
            const ee::Ptr<Replay> replay = (*g_replays)[slot];
            if (!replay)
                continue;
            if (replay->state == kReplayRecording)
            {
                ++replay->recordedFrames;
                ++replay->frame;
                ++replay->repeats;
                rider::Rider *const rider = rider::riderOf(replay->rider).get();
                const rider::PadInput *const pad = rider::padInput(rider->player);
                if ((pad->flags & 1) != 0)
                    continue;
                if (pad->buttons.held != replay->lastButtons || replay->repeats == kMaxRepeats)
                {
                    if (entryFits(replay, kButtonsEntry))
                    {
                        const uint32_t changed = pad->buttons.held ^ replay->lastButtons;
                        writeEntry(replay, kButtonsEntry, static_cast<uint8_t>(changed), static_cast<uint8_t>(changed >> 8));
                    }
                    else
                        setReplayState(static_cast<uint8_t>(slot), kReplayIdle);
                    replay->lastButtons = pad->buttons.held;
                }
                for (uint32_t stick = 0; stick < 2; ++stick)
                {
                    const uint32_t x = static_cast<uint32_t>(ee::toInt(pad->buttons.sticks[stick].x * ee::Float(127.0f)));
                    const uint32_t y = static_cast<uint32_t>(ee::toInt(pad->buttons.sticks[stick].y * ee::Float(127.0f)));
                    const uint32_t steps = ((x & 0xFFu) << 8) | (y & 0xFFu);
                    if (steps == replay->lastSticks[stick])
                        continue;
                    if (entryFits(replay, kLeftStickEntry + stick))
                        writeEntry(replay, kLeftStickEntry + stick, static_cast<uint8_t>(y), static_cast<uint8_t>(x));
                    else
                        setReplayState(static_cast<uint8_t>(slot), kReplayIdle);
                    replay->lastSticks[stick] = static_cast<uint16_t>(steps);
                }
            }
            else if (replay->state == kReplayPlaying)
            {
                rider::Rider *const rider = rider::riderOf(replay->rider).get();
                const ee::Float unit = *replayStickUnit;
                if (replay->recordedFrames == 0)
                    continue;
                for (uint32_t stick = 0; stick < 2; ++stick)
                    rider->input.previous[stick] = stickOf(replay->lastSticks[stick], unit);

                uint32_t changed = 0;
                --replay->repeats;
                if (replay->repeats == 0)
                {
                    ee::Ptr<uint8_t> at = replay->cursor;
                    while (ee::Address(at) < recordingEnd(replay))
                    {
                        const uint32_t kind = at[0] >> kEntryKindShift;
                        replay->cursor = at + 1;
                        if (kind == kButtonsEntry)
                        {
                            changed = at[1] | (static_cast<uint32_t>(at[2]) << 8);
                            replay->cursor = at + 3;
                            if ((replay->flags & kReplayRecordsPositions) != 0)
                                skipReplayPosition(replay);
                        }
                        else if (kind < kEntryKinds)
                        {
                            replay->lastSticks[kind - 1] = static_cast<uint16_t>(at[1] | (static_cast<uint32_t>(at[2]) << 8));
                            replay->cursor += 2;
                            if ((replay->flags & kReplayRecordsPositions) != 0)
                                skipReplayPosition(replay);
                        }
                        at = replay->cursor;
                        replay->repeats = at[0] & kRepeatsMask;
                        if (replay->repeats != 0)
                            break;
                    }
                }
                rider->input.changed = changed;
                const ee::Float unit2 = *replayStickUnit2;
                const uint32_t held = replay->lastButtons ^ changed;
                replay->lastButtons = held;
                rider->input.held = held;
                for (uint32_t stick = 0; stick < 2; ++stick)
                    rider->input.sticks[stick] = stickOf(replay->lastSticks[stick], unit2);
                ++replay->frame;
                if (replay->frame == replay->recordedFrames)
                    setReplayState(static_cast<uint8_t>(slot), kReplayPlayedOut);
            }
        }
    }
    READABLE_FUNCTION(replayUpdate, Verified);
}
