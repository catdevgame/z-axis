// The game's input recorder and its playback (readable round gamehand5):
// replayFrame runs once a frame before the frame's move (replayAndPads,
// which then hands the pads to replayPadFrame). In every mode the frame's
// time is whole 60ths of a second; recording ("Recording" on the debug
// overlay) and playing back keep a byte stream of records:
//
//   a header byte: bits 0 .. 2 what follows (1 a buttons record, 2 a stick
//   record, 4 a time record; 7 the end of the recording), bits 3 .. 7 the
//   frames before the next header (written into it when that one is made);
//   a time record, 12 bytes: the frame time in 60ths (a byte), player one's
//   x, the time recorded so far; a buttons record, 2 bytes: the buttons,
//   bit 14 the pad; a stick record, 2 bytes: x and y as signed nibbles
//   (sevenths), the pad.
//
// Recording writes only the time records here (replayPadFrame writes the
// pads' records); playback reads all of them into the replay's two pad
// records at replayPads. Field names past the offsets are inferred from what
// the code does.

#include "aggressive/game_state.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/scene/scene.h"
#include "zge/debug/debug.h"
#include "aggressive/objects.h"
#include "aggressive/replay.h"
#include "aggressive/zone_memory.h"
#include "aggressive/level/animated_objects.h"
#include "aggressive/level/objectives.h"
#include "aggressive/level/props.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/replay.cpp.h"

namespace aggressive
{
    namespace
    {
        enum class ReplayMode : uint8_t
        {
            Off = 0,
            Recording = 1,
            Playing = 2,
            Stopped = 3, // the frame time kept whole as when off
            ContinueRecording = 4, // setReplayMode's: recording on from where playback is (then Recording)
        };

        // A pad as playback leaves it (0x1C bytes, two at replayPads).
        struct ReplayPad
        {
            uint32_t buttons; // +0x00
            uint32_t changed; // +0x04 the buttons whose state the last record changed
            ee::Float x;      // +0x08 the stick, -8/7 .. 1
            ee::Float y;      // +0x0C
            uint8_t _10[0x08];
            uint8_t fresh;    // +0x18 1 after a reset: the next recorded frame writes the buttons
            uint8_t _19[0x03];
        };
        static_assert(sizeof(ReplayPad) == 0x1C && offsetof(ReplayPad, x) == 0x08 && offsetof(ReplayPad, fresh) == 0x18);

        // A time record as written to the stream.
        struct TimeRecord
        {
            uint8_t frames;           // +0x00 the frame time in 60ths
            uint8_t _01[0x03];        // left as the stack had them
            ee::Float playerX;        // +0x04 player one's position x
            ee::Float recordedTime;   // +0x08 the time recorded so far
        };
        static_assert(sizeof(TimeRecord) == 0x0C);
        // A buttons record: bits 0 .. 11 the pad's buttons, 12 its 0x10000,
        // 14 the pad.
        struct ButtonsRecord
        {
            uint16_t bits;
        };
        // A stick record: x and y in sevenths, a signed nibble each (x the
        // low one), then the pad.
        struct StickRecord
        {
            uint8_t xy;
            uint8_t pad;
        };
        static_assert(sizeof(ButtonsRecord) == 2 && sizeof(StickRecord) == 2);

        inline ee::Global<ReplayMode, sym::g_replayMode> g_replayMode;
        inline ee::Global<uint32_t, sym::g_replayStart> g_replayStart;   // the stream
        inline ee::Global<uint32_t, sym::g_replayCursor> g_replayCursor;  // the next byte
        inline ee::Global<uint32_t, sym::g_replayHeader> g_replayHeader;  // the last header written (0: none)
        inline ee::Global<int32_t, sym::g_replayFrames> g_replayFrames;   // frames since (recording) or left of (playing) the header
        inline ee::Global<ee::Float, sym::g_replayFrameTime> g_replayFrameTime;
        inline ee::Global<int32_t, sym::g_replayFrameCount> g_replayFrameCount;
        inline ee::Global<ee::Float, sym::g_replayTime> g_replayTime;
        inline ee::Global<ee::Float, sym::g_maxFrameTime> g_maxFrameTime;       // 0.05
        inline ee::Global<ee::Float, sym::g_maxRecordFrameTime> g_maxRecordFrameTime; // 0.05
        constexpr uint32_t kReplayPads = sym::replayPads;  // ReplayPad[2]
        constexpr uint32_t kReplayPadsEnd = kReplayPads + 2 * static_cast<uint32_t>(sizeof(ReplayPad));

        constexpr uint32_t kRecordingText = sym::recordingText; // "Recording"

        constexpr int32_t kStreamRoom = 0x2E7D;       // bytes of stream before recording stops
        constexpr int32_t kMostFramesPerHeader = 25;  // a header's count fits 5 bits
        constexpr uint8_t kTimeFollows = 0x04, kButtonsFollow = 0x01, kStickFollows = 0x02, kEnd = 0x07;

        // Messages the script objects answer for the replay's object states
        // (sendNewMessage's engine numbers): the bytes of the object's
        // state (-> uint32_t), its state written at an ObjectReplayCursor's
        // cursor, and read back from one.
        constexpr uint32_t kReplayStateBytes = 0x9FC, kSaveReplayState = 0x9FD, kRestoreReplayState = 0x9FE;

        // What saveObjectReplay and restoreObjectsReplay hand an object.
        struct ObjectReplayCursor
        {
            uint32_t object; // +0x00 the last object written
            uint32_t cursor; // +0x04 where its state goes (or comes from)
        };

        // The level as recording began, put back when playback starts
        // (g_replayStartState): each player's replay state (playerReplaySaveSize,
        // savePlayerReplayState), then copies of the level's parts in the
        // career level states' layout (career.h; the visibility is not kept).
        struct ReplayStart
        {
            uint32_t playerStateBytes[2]; // +0x00
            uint32_t playerStates[2];     // +0x08 locked blocks
            ee::Ptr<level::AnimObjectState> objectStates; // +0x10 the script objects' states (saveAnimObjectStates)
            uint32_t objectStatesSize;    // +0x14
            uint32_t scriptVariables;     // +0x18 a copy of the level script's variables
            uint32_t scriptVariablesSize; // +0x1C
            uint32_t goals;               // +0x20 the goals' state (saveObjectiveState)
            uint32_t goalsSize;           // +0x24
            uint32_t props;               // +0x28 the props' flags (saveLevelPropState)
            uint32_t propsSize;           // +0x2C
        };
        static_assert(sizeof(ReplayStart) == 0x30 && offsetof(ReplayStart, objectStates) == 0x10);
        inline ee::Global<ReplayStart, sym::g_replayStartState> g_replayStartState;
        // 44 bytes initReplay clears; nothing else in the game names them.
        constexpr uint32_t kReplayCleared = sym::replayCleared, kReplayClearedBytes = 0x2C;

        inline ee::Global<uint32_t, sym::g_replayCapacity> g_replayCapacity; // the stream's bytes

        constexpr uint32_t kSetDmmMode = sym::setDmmMode;         // (mode) the engine's

        const ee::Float kFramesPerSecond(60.0f);
        const ee::Float kHalf(0.5f);

        // The replay's pad for `pad` (0 or 1).
        ee::Ptr<ReplayPad> replayPad(uint32_t pad)
        {
            return ee::Ptr<ReplayPad>(sym::replayPads) + pad;
        }

        // t in 60ths, rounded half away from zero.
        int32_t roundToFrames(ee::Float t)
        {
            const ee::Float frames = t * kFramesPerSecond;
            const ee::Float rounded = frames < ee::Float(0.0f) ? frames - kHalf : frames + kHalf;
            return ee::toInt(rounded);
        }

        // The frame time made whole 60ths in place: 0 stays 0.
        ee::Float wholeFrames(ee::Ptr<ee::Float> dt)
        {
            int32_t frames = 0;
            if (!(*dt == ee::Float(0.0f)))
                frames = roundToFrames(*dt);
            const ee::Float whole = ee::toFloat(frames) / kFramesPerSecond;
            *dt = whole;
            return whole;
        }

        // One button of a buttons record: marked changed when it differs
        // from the pad's, then set as the record has it. `recordBit` is the
        // record's bit, `field` the pad's (the same bit for bits 0 .. 11;
        // bit 12 goes to 0x10000, done by the caller).
        void playButton(const ee::Ptr<ReplayPad> &pad, uint32_t record, uint32_t recordBit, uint32_t field)
        {
            const uint32_t fieldBit = static_cast<uint32_t>(std::countr_zero(field));
            uint32_t buttons = pad->buttons;
            if (((record >> recordBit) & 1u) != ((buttons >> fieldBit) & 1u))
            {
                pad->changed = pad->changed | field;
                buttons = pad->buttons;
            }
            const uint32_t set = buttons | field;
            const bool down = (record >> recordBit) & 1u;
            pad->buttons = down ? set : buttons & ~field;
        }

        // A stick record's nibble (sevenths, signed) as the stick's value.
        ee::Float stickValue(int32_t sevenths) { return ee::toFloat(sevenths) / ee::Float(7.0f); }
        int32_t stickX(uint8_t xy) { return static_cast<int32_t>(static_cast<uint32_t>(xy) << 28) >> 28; }
        int32_t stickY(uint8_t xy) { return static_cast<int32_t>(static_cast<uint32_t>(xy) << 24) >> 28; }
    }

    // setFrameTime (frame time *, seconds): the replay's frame time set,
    // and stored at `frameTime` too unless it is none.
    void setFrameTime(ee::Ptr<ee::Float> frameTime, ee::Float seconds)
    {
        *g_replayFrameTime = seconds;
        if (frameTime)
            *frameTime = seconds;
    }
    READABLE_FUNCTION(setFrameTime, Verified);

    void setReplayMode(ReplayMode mode);
    uint32_t playerReplaySaveSize(ee::Ptr<skater::Player> player);                    // (below)
    uint32_t savePlayerReplayState(ee::Ptr<skater::Player> player, uint32_t block);    // (below)
    uint32_t restorePlayerReplayState(ee::Ptr<skater::Player> player, uint32_t block); // (below)

    namespace
    {

        // Playback's frame (replayFrame's, its records read into replayFrame's
        // locals): the recorded frame time; when the header's frames are used
        // up, the next headers' records are read until one lasts a frame or
        // more.
        void playBack(ee::Ptr<ee::Float> dt, const ee::Local<TimeRecord> &time, const ee::Local<ButtonsRecord> &buttons,
                      const ee::Local<StickRecord> &stick, const ee::Local<uint8_t> &header)
        {
            *dt = *g_replayFrameTime;
            const int32_t count = *g_replayFrameCount + 1;
            *g_replayFrameCount = count;
            int32_t framesLeft = *g_replayFrames;
            if (framesLeft > 0)
            {
                --framesLeft;
                *g_replayFrames = framesLeft;
            }
            for (uint32_t pad = 0; pad < 2; ++pad)
                replayPad(pad)->changed = 0;

            uint32_t pad = 0; // the last record's pad
            if (framesLeft == 0)
            {
                do
                {
                    zge::memoryCopy(header.address(), *g_replayCursor, 1);
                    uint8_t what = *header;
                    *g_replayFrames = what >> 3;
                    *g_replayCursor = *g_replayCursor + 1;
                    if ((what & 0x7u) == kEnd)
                    {
                        // The end: playback stops, and play stops for the pause.
                        setReplayMode(ReplayMode::Off);
                        const ee::Ptr<Level> level = currentLevel();
                        level->flags = static_cast<uint16_t>(level->flags & ~kLevelStopReasonBits);
                        enterGameMode(kModePaused);
                        *g_replayFrames = 0;
                        return;
                    }
                    if (what & kTimeFollows)
                    {
                        zge::memoryCopy(time.address(), *g_replayCursor, sizeof(TimeRecord));
                        *g_replayCursor = *g_replayCursor + sizeof(TimeRecord);
                        setFrameTime(dt, ee::toFloat(time->frames) / kFramesPerSecond);
                        what = *header;
                    }
                    if (what & kButtonsFollow)
                    {
                        zge::memoryCopy(buttons.address(), *g_replayCursor, sizeof(ButtonsRecord));
                        const uint32_t record = buttons->bits;
                        pad = (record >> 14) & 1u;
                        *g_replayCursor = *g_replayCursor + sizeof(ButtonsRecord);
                        const ee::Ptr<ReplayPad> to = replayPad(pad);
                        playButton(to, record, 0, 0x001u);
                        playButton(to, record, 1, 0x002u);
                        playButton(to, record, 2, 0x004u);
                        playButton(to, record, 3, 0x008u);
                        playButton(to, record, 7, 0x080u);
                        playButton(to, record, 5, 0x020u);
                        playButton(to, record, 4, 0x010u);
                        playButton(to, record, 6, 0x040u);
                        playButton(to, record, 8, 0x100u);
                        playButton(to, record, 10, 0x400u);
                        playButton(to, record, 9, 0x200u);
                        playButton(to, record, 11, 0x800u);
                        // Bit 12 is the pad's 0x10000.
                        uint32_t held = to->buttons;
                        if (((record >> 12) & 1u) != ((held >> 16) & 1u))
                        {
                            to->changed = to->changed | 0x10000u;
                            held = to->buttons;
                        }
                        if ((record >> 12) & 1u)
                            held |= 0x10000u;
                        else
                            held &= ~0x10000u;
                        what = *header;
                        to->buttons = held;
                    }
                    if (what & kStickFollows)
                    {
                        zge::memoryCopy(stick.address(), *g_replayCursor, sizeof(StickRecord));
                        const uint8_t xy = stick->xy;
                        pad = stick->pad;
                        *g_replayCursor = *g_replayCursor + sizeof(StickRecord);
                        const ee::Ptr<ReplayPad> to = replayPad(pad);
                        to->x = stickValue(stickX(xy));
                        to->y = stickValue(stickY(xy));
                    }
                } while (*g_replayFrames == 0);
            }
            *g_replayTime = *g_replayTime + *dt;
            if (count == 1)
                replayPad(pad)->changed = 0; // the first frame played: nothing changed
        }
        // The two replay pads cleared, each marked fresh (its next recorded
        // frame writes its buttons whatever changed).
        void clearReplayPads()
        {
            for (uint32_t pad = kReplayPads; pad < kReplayPadsEnd; pad += static_cast<uint32_t>(sizeof(ReplayPad)))
            {
                zge::memoryFill(pad, 0, static_cast<uint32_t>(sizeof(ReplayPad)));
                ee::Ptr<ReplayPad>(pad)->fresh = 1;
            }
        }

        const ee::Ptr<ee::Float> kNoFrameTime{}; // setFrameTime's: the time not stored
        void setDmmMode(uint32_t mode) { ee::game(kSetDmmMode, mode); }
        // A message from no sender, answered at once.
        int32_t sendMessage(ee::Address object, uint32_t type, ee::Address data) { return zge::sendNewMessage(0u, object, type, data, 1u); }
    }

    // writeReplayEnd: the recording's end: the last header's frame count
    // cleared (the original's: the frames since it are lost) and an end
    // header (kEnd) written.
    void writeReplayEnd()
    {
        ee::Local<uint8_t> src;
        const uint32_t last = *g_replayHeader;
        *src = kEnd;
        if (last)
            *ee::Ptr<uint8_t>(last) = static_cast<uint8_t>(*ee::Ptr<uint8_t>(last) & 0x7u);
        const uint32_t cursor = *g_replayCursor;
        *g_replayFrames = 0;
        *g_replayHeader = cursor;
        zge::memoryCopy(cursor, src.address(), 1);
        *g_replayCursor = *g_replayCursor + 1;
    }
    READABLE_FUNCTION(writeReplayEnd, Verified);

    // addObjectReplayBytes (forEachObject's visit: object, total *): an object
    // that answers kReplayStateBytes adds its bytes and its name hash's 4.
    uint32_t addObjectReplayBytes(uint32_t object, uint32_t total)
    {
        ee::Local<uint32_t> data;
        *data = 0;
        const bool answered = sendMessage(object, kReplayStateBytes, data.address()) != 0;
        const uint32_t bytes = *data;
        if (answered)
            *ee::Ptr<uint32_t>(total) = *ee::Ptr<uint32_t>(total) + 4 + bytes;
        return 1;
    }
    READABLE_FUNCTION_WAITS(addObjectReplayBytes, Verified);

    // saveObjectReplay (forEachObject's visit: object, ObjectReplayCursor *):
    // an object with a replay state writes its name hash, then its state
    // (kSaveReplayState), at the cursor.
    uint32_t saveObjectReplay(ee::Ptr<zge::SceneObject> object, uint32_t writer)
    {
        ee::Local<uint32_t> data;
        ee::Local<uint32_t> src;
        const ee::Ptr<ObjectReplayCursor> to(writer);
        *data = 0;
        if (sendMessage(object, kReplayStateBytes, data.address()))
        {
            const uint32_t hash = zge::objectNameHash(object);
            const uint32_t cursor = to->cursor;
            *src = hash;
            zge::memoryCopy(cursor, src.address(), 4);
            to->object = ee::Address(object); // the cursor's word
            to->cursor = to->cursor + 4;
            const bool saved = sendMessage(object, kSaveReplayState, writer) != 0;
            const uint32_t bytes = *data;
            if (saved)
                to->cursor = to->cursor + bytes;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveObjectReplay, Verified);

    // initReplay (bytes): the replay set up at a level's load: both pads
    // fresh, a stream of `bytes` (a locked block, cleared), the frame
    // time 1 s, off.
    void initReplay(uint32_t bytes)
    {
        clearReplayPads();
        const uint32_t stream = allocateLockedBlock(bytes);
        *g_replayStart = stream;
        zge::memoryFill(stream, 0, bytes);
        *g_replayCapacity = bytes;
        *g_replayCursor = *g_replayStart;
        *g_replayHeader = 0;
        setFrameTime(kNoFrameTime, ee::Float(1.0f));
        *g_replayFrames = 0;
        *g_replayMode = ReplayMode::Off;
        *g_replayFrameCount = 0;
        zge::memoryFill(kReplayCleared, 0, kReplayClearedBytes);
    }
    READABLE_FUNCTION(initReplay, Verified);

    namespace
    {
        ee::Ptr<uint32_t> scriptVariablesCopy(ee::Ptr<uint32_t> bytes) { return script::scriptVariables(bytes); }
        void restoreScriptVariables(uint32_t block) { script::scriptRestoreVariables(ee::Ptr<uint32_t>(block)); }
    }

    // setReplayMode (mode): the replay's mode. Leaving a recording (but
    // for Stopped) writes its end. Recording: the stream cleared and the
    // level kept as it is (g_replayStartState); Playing: the level put
    // back as it was; both start from the stream's start, the pads fresh,
    // the frame time 5 s and the time and frames at 0. ContinueRecording:
    // the pads fresh and recording on from here, a frame taken back.
    void setReplayMode(ReplayMode mode)
    {
        ee::Local<uint32_t> bytes;
        ReplayStart *const start = g_replayStartState.get();
        if (*g_replayMode == ReplayMode::Recording && mode != ReplayMode::Stopped)
            writeReplayEnd();
        *g_replayMode = mode;

        // The pads fresh and the stream from its start (Recording and Playing).
        const auto restart = [&]() {
            clearReplayPads();
            *g_replayCursor = *g_replayStart;
            *g_replayHeader = 0;
            *g_replayFrames = 0;
                setFrameTime(kNoFrameTime, ee::Float(5.0f));
            *g_replayTime = ee::Float(0.0f);
            *g_replayFrameCount = 0;
                setDmmMode(315);
        };

        switch (mode)
        {
        case ReplayMode::Off:
        case ReplayMode::Stopped:
            return;

        case ReplayMode::Recording:
        {
            zge::memoryFill(*g_replayStart, 0, *g_replayCapacity);
            // Each player's replay state into a new locked block.
            for (int32_t index = 0; index < 2; ++index)
            {
                const ee::Ptr<skater::Player> player = levelPlayer(index);
                const uint32_t stateBytes = playerReplaySaveSize(player);
                const uint32_t old = start->playerStates[index];
                start->playerStateBytes[index] = stateBytes;
                if (old)
                    releaseZoneBlock(old);
                const uint32_t state = allocateLockedBlock(start->playerStateBytes[index]);
                start->playerStates[index] = state;
                savePlayerReplayState(player, state);
            }
            const auto keep = [&](uint32_t &block, uint32_t &size, ee::Address source) {
                if (!*bytes)
                {
                    size = 0;
                    block = 0;
                    return;
                }
                size = *bytes;
                if (block)
                    releaseZoneBlock(block);
                block = allocateLockedBlock(*bytes);
                zge::memoryCopy(block, source, *bytes);
            };
            keep(start->scriptVariables, start->scriptVariablesSize, scriptVariablesCopy(bytes));
            keep(start->goals, start->goalsSize, aggressive::level::saveObjectiveState(bytes));
            keep(start->props, start->propsSize, aggressive::level::saveLevelPropState(bytes));
            *bytes = aggressive::level::animObjectStateBytes();
            if (*bytes)
            {
                start->objectStatesSize = *bytes;
                if (start->objectStates)
                    releaseZoneBlock(start->objectStates);
                start->objectStates = ee::Ptr<level::AnimObjectState>(allocateLockedBlock(*bytes));
                aggressive::level::saveAnimObjectStates(start->objectStates);
            }
            else
            {
                start->objectStates = {};
                start->objectStatesSize = 0;
            }
            restart();
            return;
        }

        case ReplayMode::ContinueRecording:
        {
            clearReplayPads();
            *g_replayMode = ReplayMode::Recording;
                setFrameTime(kNoFrameTime, ee::Float(5.0f));
            *g_replayFrames = *g_replayFrames - 1;
            *g_replayFrameCount = *g_replayFrameCount - 1;
            return;
        }

        default:
            // Playing (and the modes past the table: the same start, no level put back).
            if (mode != ReplayMode::Playing)
            {
                restart();
                return;
            }
            break;
        }

        // Playing: the players' states and the level's parts put back.
        for (int32_t index = 0; index < 2; ++index)
            restorePlayerReplayState(levelPlayer(index), start->playerStates[index]); // (the original also hands the bytes, unread)
        if (start->scriptVariables)
            restoreScriptVariables(start->scriptVariables);
        if (start->goals)
            aggressive::level::loadObjectiveState(start->goals);
        if (start->props)
            aggressive::level::loadLevelPropState(start->props);
        if (start->objectStates)
            aggressive::level::restoreAnimObjectStates(start->objectStates, start->objectStatesSize);
        restart();
    }
    READABLE_FUNCTION_WAITS(setReplayMode, Verified);

    // replayFrame: the replay's frame (frameTime: the frame's time in
    // seconds, changed in place). Off and stopped, the frame time is made
    // whole 60ths, at most 0.05 s, and set if it differs from the last.
    // Recording: the same (counting the frames and the time recorded),
    // and while the stream has room, a new header and time record once the
    // frame time changes or the header has counted 25 frames; when it is
    // full, recording stops. Playing: the frame time is the recorded one;
    // when the header's frames are used up, the records after the next
    // headers are read (time, buttons, stick) until one lasts a frame or
    // more; the end record stops playback and pauses play.
    void replayFrame(ee::Ptr<ee::Float> frameTime)
    {
        // The records on their way to and from the stream.
        ee::Local<TimeRecord> time;
        ee::Local<ButtonsRecord> buttons;
        ee::Local<StickRecord> stick;
        ee::Local<uint8_t> header;

        const ReplayMode mode = *g_replayMode;
        ee::Float frameTimeToSet;
        if (mode == ReplayMode::Recording)
        {
            zge::debugPrint(0, 5, kRecordingText);
            wholeFrames(frameTime);
            *g_replayFrameCount = *g_replayFrameCount + 1;
            const int32_t used = static_cast<int32_t>(*g_replayCursor - *g_replayStart);
            *g_replayTime = *g_replayTime + *frameTime;
            frameTimeToSet = ee::min(*frameTime, *g_maxRecordFrameTime);
            if (used < kStreamRoom)
            {
                // A new header when the time changes or this one is full.
                const bool sameTime = *g_replayFrameTime == frameTimeToSet;
                const int32_t frames = *g_replayFrames + 1;
                *g_replayFrames = frames;
                if (sameTime && frames < kMostFramesPerHeader)
                    return;
                *header = kTimeFollows;
                uint32_t cursor = *g_replayCursor;
                if (const uint32_t last = *g_replayHeader)
                {
                    const ee::Ptr<uint8_t> lastHeader(last);
                    *lastHeader = static_cast<uint8_t>((*lastHeader & 0x7u) | (static_cast<uint8_t>(*g_replayFrames) << 3));
                    cursor = *g_replayCursor;
                }
                *g_replayHeader = cursor;
                zge::memoryCopy(cursor, header.address(), 1);
                const uint32_t recordAt = *g_replayCursor + 1;
                *g_replayCursor = recordAt;
                time->frames = !(frameTimeToSet == ee::Float(0.0f)) ? static_cast<uint8_t>(roundToFrames(frameTimeToSet)) : 0;
                time->recordedTime = *g_replayTime;
                time->playerX = levelPlayer(0)->position.x;
                zge::memoryCopy(recordAt, time.address(), sizeof(TimeRecord));
                *g_replayFrames = 0;
                *g_replayCursor = *g_replayCursor + sizeof(TimeRecord);
                setFrameTime(frameTime, ee::toFloat(time->frames) / kFramesPerSecond);
                return;
            }
            setReplayMode(ReplayMode::Off); // the stream is full
        }
        else if (mode == ReplayMode::Off || mode == ReplayMode::Stopped)
        {
            const ee::Float whole = wholeFrames(frameTime);
            frameTimeToSet = ee::min(whole, *g_maxFrameTime);
        }
        else if (mode == ReplayMode::Playing)
        {
            playBack(frameTime, time, buttons, stick, header);
            return;
        }
        else
            return;

        if (!(*g_replayFrameTime == frameTimeToSet))
            setFrameTime(frameTime, frameTimeToSet);
    }
    READABLE_FUNCTION_WAITS(replayFrame, Verified);

    // replayFrameTime: the replay's frame time (the original's only reader of
    // it from outside; no caller in the game).
    ee::Float replayFrameTime()
    {
        return *g_replayFrameTime;
    }
    READABLE_FUNCTION(replayFrameTime, Verified);

    // replayPadFrame (port): the pad's frame for the replay (after
    // replayFrame, replayAndPads' tail call). Recording: when the pad's
    // buttons changed (or the replay pad is fresh) a buttons record, and
    // when its stick moved a stick record, the stick kept to the sevenths
    // the record holds (written back to the pad too); a header before
    // them, the last header given its frame count. Playing: the replay
    // pad's buttons, changes and stick into the game's pad. Off and
    // stopped: nothing.
    void replayPadFrame(uint32_t port)
    {
        // The records on their way to the stream.
        ee::Local<ButtonsRecord> buttons;
        ee::Local<StickRecord> stick;
        ee::Local<uint8_t> header;
        const ee::Ptr<skater::PadState> pad = ee::Ptr<skater::PadState>(sym::g_pads) + port;
        const ReplayMode mode = *g_replayMode;
        if (mode != ReplayMode::Recording)
        {
            if (mode == ReplayMode::Playing)
            {
                const ee::Ptr<ReplayPad> from = replayPad(port);
                pad->held = from->buttons;
                pad->axes[1] = from->y;
                pad->pressed = from->changed;
                pad->axes[0] = from->x;
            }
            return;
        }

        const ee::Ptr<ReplayPad> to = replayPad(port);
        const uint32_t pressed = pad->pressed;
        to->changed = 0;
        bool buttonsRecorded = false, stickRecorded = false;
        if (pressed || to->fresh)
        {
            // Bits 0 .. 11 the pad's, 12 its 0x10000, 14 the port. Bits 13
            // and 15 are not set: they keep what the stack held there, and
            // go into the stream so (the original's do too).
            const uint32_t held = pad->held;
            buttons->bits = static_cast<uint16_t>((buttons->bits & ~0x5FFFu) | (held & 0xFFFu) | (((held >> 16) & 1u) << 12) | ((port & 1u) << 14));
            to->buttons = held;
            to->changed = pressed;
            buttonsRecorded = true;
            if (to->fresh)
            {
                to->fresh = 0;
                pad->pressed = 0;
            }
        }
        // The stick record: x and y in sevenths, a signed nibble each, then the port.
        const ee::Float sevenths(7.0f);
        const int32_t x = ee::toInt(pad->axes[0] * sevenths);
        stick->xy = static_cast<uint8_t>((stick->xy & 0xF0u) | (x & 0xF));
        const int32_t y = ee::toInt(pad->axes[1] * sevenths);
        stick->xy = static_cast<uint8_t>((stick->xy & 0x0Fu) | ((y & 0xF) << 4));
        stick->pad = static_cast<uint8_t>(port);
        const uint8_t xy = stick->xy;
        const ee::Float keptX = stickValue(stickX(xy));
        pad->axes[0] = keptX;
        const ee::Float keptY = stickValue(stickY(xy));
        pad->axes[1] = keptY;
        if (!(keptX == to->x) || !(keptY == to->y))
        {
            to->x = keptX;
            to->y = keptY;
            stickRecorded = true;
        }
        if (!buttonsRecorded && !stickRecorded)
            return;

        // A header (what follows), the last one given its frames.
        if (const uint32_t last = *g_replayHeader)
        {
            const ee::Ptr<uint8_t> lastHeader(last);
            *lastHeader = static_cast<uint8_t>((*lastHeader & 0x7u) | (static_cast<uint8_t>(*g_replayFrames) << 3));
        }
        *header = static_cast<uint8_t>((stickRecorded ? kStickFollows : 0) | (buttonsRecorded ? kButtonsFollow : 0));
        const uint32_t cursor = *g_replayCursor;
        *g_replayFrames = 0;
        *g_replayHeader = cursor;
        zge::memoryCopy(cursor, header.address(), 1);
        *g_replayCursor = *g_replayCursor + 1;
        if (buttonsRecorded)
        {
            zge::memoryCopy(*g_replayCursor, buttons.address(), sizeof(ButtonsRecord));
            *g_replayCursor = *g_replayCursor + sizeof(ButtonsRecord);
        }
        if (stickRecorded)
        {
            zge::memoryCopy(*g_replayCursor, stick.address(), sizeof(StickRecord));
            *g_replayCursor = *g_replayCursor + sizeof(StickRecord);
        }
    }
    READABLE_FUNCTION_WAITS(replayPadFrame, Verified);

    // replayRecorded: whether anything was recorded (the cursor past the
    // stream's start).
    bool replayRecorded()
    {
        return *g_replayCursor != *g_replayStart;
    }
    READABLE_FUNCTION(replayRecorded, Verified);

    // objectsReplayBytes: the bytes the script objects' replay states take
    // (addObjectReplayBytes over every object). No caller in the game.
    uint32_t objectsReplayBytes()
    {
        ee::Local<uint32_t> data;
        *data = 0;
        zge::forEachObject(sym::addObjectReplayBytes, data.address());
        return *data;
    }
    READABLE_FUNCTION_WAITS(objectsReplayBytes, Verified);

    // saveObjectsReplay (buffer): the script objects' replay states written
    // there (saveObjectReplay over every object). No caller in the game.
    uint32_t saveObjectsReplay(uint32_t buffer)
    {
        ee::Local<ObjectReplayCursor> writer;
        zge::memoryFill(writer.address(), 0, static_cast<uint32_t>(sizeof(ObjectReplayCursor)));
        writer->cursor = buffer;
        writer->object = 0;
        zge::forEachObject(sym::saveObjectReplay, writer.address());
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveObjectsReplay, Verified);

    // restoreObjectsReplay (buffer, bytes): the script objects' replay states read
    // back: each name hash's object found (findObject) and given its state
    // (kRestoreReplayState). Original bug kept: the loop's end test is
    // `bytes < 5`, which never changes, so with 5 bytes or more it never
    // ends (no caller in the game; under 5 bytes nothing is read).
    uint32_t restoreObjectsReplay(uint32_t buffer, uint32_t bytes)
    {
        ee::Local<ObjectReplayCursor> reader;
        ee::Local<uint32_t[1]> hash;
        ee::Local<uint32_t[1]> stateBytes;
        const uint32_t tooShort = bytes < 5u;
        uint32_t cursor = buffer;
        if (!tooShort)
        {
            for (;;)
            {
                *stateBytes = 0;
                zge::memoryCopy(hash.address(), cursor, 4);
                cursor += 4;
                const ee::Ptr<zge::SceneObject> object = *zge::findObjects(hash, 1);
                reader->cursor = cursor;
                reader->object = ee::Address(object); // the cursor's word
                sendMessage(object, kReplayStateBytes, stateBytes.address());
                sendMessage(reader->object, kRestoreReplayState, reader.address());
                cursor += *stateBytes;
                if (tooShort)
                    break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreObjectsReplay, Verified);

    // cycleReplayMode: the replay's mode stepped (a debug key's [inferred]):
    // off to recording, recording to playing, playing to off; stopped and
    // continuing stay. No caller in the game.

    void cycleReplayMode()
    {
        const ReplayMode mode = *g_replayMode;
        switch (mode)
        {
        case ReplayMode::Off:
            game::tail(setReplayMode, ReplayMode::Recording);
            return;
        case ReplayMode::Recording:
            game::tail(setReplayMode, ReplayMode::Playing);
            return;
        case ReplayMode::Playing:
            game::tail(setReplayMode, ReplayMode::Off);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(cycleReplayMode, Verified);

    // replayAndPads (frame time *): the replay's frame, then its pads (the
    // tail call replayPadFrame with port 0: recorded or played back).
    namespace
    {
        void runReplayFrame(ee::Ptr<ee::Float> frameTime) { replayFrame(frameTime); }
    }

    void replayAndPads(ee::Ptr<ee::Float> frameTime)
    {
        runReplayFrame(frameTime);
        game::tail(replayPadFrame, 0u);
    }
    READABLE_FUNCTION_WAITS(replayAndPads, Verified);

    namespace
    {
        // A player's block: its bytes in all, the player, then per object
        // slot (the four and the current object's) its bytes and its objects.
        constexpr uint32_t kPlayerBlockHead = sizeof(uint32_t) + sizeof(skater::Player);
        constexpr uint32_t kPlayerSlots = 5;
        constexpr uint32_t kSlotHeadBytes = kPlayerSlots * sizeof(uint32_t);

        // Slot `slot` of the player's five: the four object slots, then
        // (slot 4) the current object's, which follows them.
        uint32_t playerSlot(ee::Ptr<skater::Player> player, uint32_t slot)
        {
            return ee::Ptr<uint32_t>(player->objectSlots)[slot];
        }
        // The same slot's object list (a slot starts with it; the player keeps the slot as a word).
        ee::Ptr<zge::List> playerSlotList(ee::Ptr<skater::Player> player, uint32_t slot)
        {
            return ee::Ptr<zge::List>(playerSlot(player, slot));
        }
    }

    // playerReplaySaveSize: the bytes of a player's block.
    uint32_t playerReplaySaveSize(ee::Ptr<skater::Player> player)
    {
        uint32_t bytes = kPlayerBlockHead + kSlotHeadBytes;
        bytes += slotSaveSize(playerSlotList(player, 0));
        for (uint32_t slot = 1; slot < kPlayerSlots; ++slot)
            bytes += slotSaveSize(playerSlotList(player, slot));
        return bytes;
    }
    READABLE_FUNCTION_WAITS(playerReplaySaveSize, Verified);

    // savePlayerReplayState: a player's block for the replay. Returns 1.
    uint32_t savePlayerReplayState(ee::Ptr<skater::Player> player, uint32_t block)
    {
        ee::Local<uint32_t> slotBytes;
        ee::Local<uint32_t> total;
        uint32_t cursor = block + kPlayerBlockHead;
        *slotBytes = 0;
        *total = 4;
        zge::memoryCopy(block + 4, player, sizeof(skater::Player));
        *total += sizeof(skater::Player);
        for (uint32_t slot = 0;; ++slot)
        {
            *slotBytes = slotSaveSize(playerSlotList(player, slot));
            zge::memoryCopy(cursor, slotBytes.address(), sizeof(uint32_t));
            const uint32_t objects = cursor + sizeof(uint32_t);
            *total += sizeof(uint32_t);
            if (slot + 1 == kPlayerSlots)
            {
                saveSlotObjects(playerSlotList(player, slot), objects, *slotBytes);
                *total += *slotBytes;
                break;
            }
            cursor = objects;
            saveSlotObjects(playerSlotList(player, slot), objects, *slotBytes);
            cursor += *slotBytes;
            *total += *slotBytes;
        }
        zge::memoryCopy(block, total.address(), sizeof(uint32_t));
        return 1;
    }
    READABLE_FUNCTION_WAITS(savePlayerReplayState, Verified);

    // restorePlayerReplayState: a player from its block. Returns 1.
    uint32_t restorePlayerReplayState(ee::Ptr<skater::Player> player, uint32_t block)
    {
        ee::Local<uint32_t> remaining;
        ee::Local<uint32_t> slotBytes;
        uint32_t objects = block + kPlayerBlockHead + sizeof(uint32_t);
        *remaining = 0;
        *slotBytes = 0;
        zge::memoryCopy(remaining.address(), block, sizeof(uint32_t));
        *remaining -= sizeof(uint32_t);
        zge::memoryCopy(player, block + 4, sizeof(skater::Player));
        *remaining -= sizeof(skater::Player);
        uint32_t head = block + kPlayerBlockHead;
        for (uint32_t slot = 0; slot < kPlayerSlots; ++slot)
        {
            zge::memoryCopy(slotBytes.address(), head, sizeof(uint32_t));
            const uint32_t objectSlot = playerSlot(player, slot);
            *remaining -= sizeof(uint32_t);
            restoreSlotObjects(objectSlot, objects);
            *remaining -= *slotBytes;
            if (slot + 1 < kPlayerSlots)
            {
                head = objects + *slotBytes;
                objects = head + sizeof(uint32_t);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(restorePlayerReplayState, Verified);
}
