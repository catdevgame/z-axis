// updateGameFrame, the game's sound-and-effects frame (readable round 13):
// the level time, the one-shot sounds that have finished, the music player
// (a streamed track at a time, the next one chosen from the playlist; a
// script's audio sync that ducks it), the engine's effects, a sound that follows a node,
// and the sound system's own frame. 

#include "aggressive/game_state.h"
#include "aggressive/globals.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/platform/platform.h"
#include "zge/collision/dynamics.h"
#include "zge/scene/scene.h"
#include "zge/render/texture_effects.h"
#include "zge/sound/sound.h"
#include "zge/sound/streams.h"
#include "zge/sound/soundtrack.h"
#include "zge/debug/profiler.h"

#include <cstddef>
#include "aggressive/skater/skater.h"
#include "aggressive/effects.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/timers.h"
#include "aggressive/level/dynamics.h"
#include "aggressive/level/props.h"
#include "aggressive/level/scene_objects.h"

#include "sym/aggressive/game_frame.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kPlayerSize = 0x930;

        constexpr int32_t kTracks = 13;
        constexpr int8_t kNoTrack = -1;
        constexpr uint32_t kStatusPlaying = 2;

        struct Music
        {
            ee::Ptr<zge::Soundtrack> soundtrack; // +0x00 the one its tracks are played from (sound/events.cpp's g_music)
            ee::Ptr<zge::SoundStream> stream;     // +0x04 the track's stream
            ee::Ptr<zge::SoundStream> syncStream; // +0x08 the audio sync's (sound/events.cpp: the script's initializeAudioSync)
            int32_t syncState;      // +0x0C 0 none, 1 loading (startAudioSync starts it), 2 playing
            ee::Float syncDelay;    // +0x10 seconds before its end is looked for
            uint32_t syncListener;  // +0x14 the object told "syncStart"
            ee::Float gap;          // +0x18 seconds before the next track may start
            int8_t next;            // +0x1C the track to start next, -1 none
            int8_t requested;       // +0x1D
            uint8_t _1e[2];
            int32_t paused;         // +0x20
            uint8_t _24;
            uint8_t cycle;          // +0x25 go on to the playlist's next track (else repeat)
            uint8_t _26[2];
            uint8_t levelTrack[15]; // +0x28 the first track of each level
            uint8_t placedVoice;    // +0x37
            ee::Float duckDelay;    // +0x38 seconds before the second stream's end is looked for
            uint32_t placedNode;    // +0x3C a node a sound follows, 0 none
            ee::Ptr<zge::SoundStream> duckStream; // +0x40
            int32_t ducking;        // +0x44 the music is turned down for duckStream
        };
        static_assert(offsetof(Music, gap) == 0x18 && offsetof(Music, levelTrack) == 0x28);
        static_assert(offsetof(Music, placedVoice) == 0x37 && offsetof(Music, ducking) == 0x44);
        inline ee::Global<Music, sym::g_music> g_music;

        inline ee::Global<ee::Float, sym::g_frameSeconds6> g_frameSeconds;
        inline ee::Global<uint8_t[16], sym::g_oneShotVoices> g_oneShotVoices; // 255: free
        inline ee::Global<int8_t, sym::g_currentTrack> g_currentTrack;
        inline ee::Global<int32_t, sym::g_playPhase> g_playPhase; // [inferred] 0 .. 3 while playing

        bool hasMusic(const GameState &game) { return game.level != 7 && game.type != static_cast<GameType>(5); }

        // The track after `from` (wrapping at 13) whose playlist bit is set,
        // or `from` itself after a full turn; the current track follows.
        struct TrackSearchBlocks
        {
            uint32_t head, wrapped, notListed, test;
        };
        constexpr TrackSearchBlocks kFirstTrackSearch{points::updateGameFrame::loop2, points::updateGameFrame::taken11, points::updateGameFrame::taken12, points::updateGameFrame::taken13};
        constexpr TrackSearchBlocks kNextTrackSearch{points::updateGameFrame::loop3, points::updateGameFrame::taken15, points::updateGameFrame::taken16, points::updateGameFrame::taken17};

        int8_t nextTrack(int8_t from, const TrackSearchBlocks &at)
        {
            int32_t track = from + 1;
            for (bool found = false;;)
            {
                *g_currentTrack = static_cast<int8_t>(track);
                if (static_cast<int8_t>(track) >= kTracks)
                    *g_currentTrack = 0, track = 0;
                const int32_t t = static_cast<int8_t>(track);
                if ((g_gameState->playlist >> (t & 0x1F)) & 1u)
                    found = true;
                else
                {
                    if (t == from)
                        found = true;
                }
                if (found)
                    return static_cast<int8_t>(track);
                track = track + 1;
            }
        }
    }

    void updateGameFrame(ee::Float seconds)
    {
        ee::Local<uint8_t> volume2;
        ee::Local<int8_t> pan2;
        ee::Local<uint8_t[0x2]> third;
        ee::Local<uint32_t> status;
        const ee::Float levelTime = *g_levelTime + seconds;
        *g_frameSeconds = seconds;
        *g_levelTime = levelTime;
        for (uint8_t &voice : *g_oneShotVoices)
        {
            if (voice != 255 && zge::voiceState(voice) == 1)
            {
                zge::releaseVoice(voice);
                voice = 255;
            }
        }

        const int32_t mode = static_cast<int32_t>(*g_gameMode);
        if (mode == -1)
            return;
        Music &music = *g_music;
        GameState &game = *g_gameState;
        if (mode == static_cast<int32_t>(GameMode::Play))
        {
            if (*g_frameSeconds < music.gap)
                music.gap = music.gap - *g_frameSeconds;
            else
                g_music->gap = 0;
            const int32_t phase = *g_playPhase;
            if (phase >= 0 && phase < 4)
            {
                // The music's state.
                bool syncAsked = false; // a stream's status was read just below
                if (music.syncState == 1)
                    startAudioSync();
                else if (music.syncState != 0)
                {
                    if (music.syncState == 2)
                    {
                        if (!(music.syncDelay == ee::Float(0.0f)))
                        {
                            if (music.syncDelay <= seconds)
                                music.syncDelay = 0.0f;
                            else
                                music.syncDelay = music.syncDelay - seconds;
                        }
                        else
                        {
                            zge::streamStatus(music.syncStream, status);
                            syncAsked = true;
                            if (*status != 2)
                            {
                                music.syncState = 0;
                                if (hasMusic(game))
                                    zge::setStreamVolume(music.stream, game.musicVolume);
                            }
                        }
                    }
                }
                if (music.syncState == 0)
                {
                    if (!(music.duckDelay == ee::Float(0.0f)))
                    {
                        if (music.duckDelay <= seconds)
                            music.duckDelay = 0.0f;
                        else
                            music.duckDelay = music.duckDelay - seconds;
                    }
                    else
                    {
                        if (music.ducking)
                        {
                            zge::streamStatus(music.duckStream, status);
                            syncAsked = true;
                            if (*status != kStatusPlaying)
                            {
                                zge::setStreamVolume(music.stream, game.musicVolume);
                                music.ducking = 0;
                            }
                        }
                    }
                }

                // The track: when it has ended (or one is wanted) and the
                // gap is over, the next one starts.
                // With no music to ask about, the original's word holds what the
                // sync or duck stream gave above this frame, or is left from an
                // earlier call: 0 on all 24,624 reads of the nine A/B inputs, so
                // nothing has ended.
                bool asked = syncAsked;
                if (hasMusic(game))
                {
                    zge::streamStatus(music.stream, status);
                    asked = true;
                }
                const bool ended = asked && (*status == 1 || *status == 4);
                if (ended || music.requested != kNoTrack || music.next != kNoTrack)
                {
                    if (music.gap == ee::Float(0.0f) && !music.paused)
                    {
                        int8_t track = music.next;
                        if (track == kNoTrack)
                        {
                            const int8_t current = *g_currentTrack;
                            track = current;
                            if (current == kNoTrack)
                            {
                                // The level's own track, or the playlist's next.
                                const int8_t first = static_cast<int8_t>(music.levelTrack[game.level]);
                                *g_currentTrack = first;
                                if ((game.playlist >> (first & 0x1F)) & 1u)
                                    track = first;
                                else
                                    track = nextTrack(first, kFirstTrackSearch);
                            }
                            else
                            {
                                if (music.cycle)
                                {
                                    track = nextTrack(current, kNextTrackSearch);
                                    music.requested = kNoTrack;
                                }
                            }
                            music.next = track;
                        }
                        if (hasMusic(game))
                        {
                            const ee::Ptr<zge::WaveLocation> name = zge::soundtrackTrackLocation(music.soundtrack, static_cast<int32_t>(track));
                            zge::openStream(music.stream, name, 34);
                            zge::setStreamVolume(music.stream, game.musicVolume);
                            zge::playStream(music.stream);
                            *g_currentTrack = music.next;
                            music.next = kNoTrack;
                            music.gap = 2.0f;
                        }
                    }
                }
            }

            zge::updateEffects();
            // A sound placed at a node: its volume and pan from where it is.
            if (music.placedNode)
            {
                if (zge::soundVoiceState(music.placedVoice) != 2)
                    music.placedNode = 0;
                else
                {
                    const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(music.placedNode));
                    placedVolume(0, position, game.soundVolume, volume2, pan2, third);
                    zge::setChannelVolume(music.placedVoice, *volume2, static_cast<int32_t>(*pan2));
                }
            }
        }
        else if (mode == static_cast<int32_t>(GameMode::Paused))
            zge::updateEffects();
        zge::soundFrame();
        zge::streamsFrame();
        zge::audioFrame();
    }
    READABLE_FUNCTION_WAITS(updateGameFrame, Verified);

    // playFrame: a frame of the game: the frame's time kept, the pads read, the
    // top state's update, the follow camera (both players' in split screen),
    // then the frame's sound and effects. Returns the state's update's result.
    uint64_t playFrame(ee::Float seconds)
    {
        g_gameState->frameTime = seconds;
        updatePads();
        const uint64_t result = g_gameState->topState->update(seconds);
        updateCamera(&g_gameState->cameras[0], seconds);
        if (isSplitScreen())
            updateCamera(&g_gameState->cameras[1], seconds);
        updateGameFrame(seconds);
        return result;
    }
    READABLE_FUNCTION_WAITS(playFrame, Verified);

    // levelEffectsFrame: the level's own frame (seconds): its objects, the sound
    // emitters, the effects, the texture animations, the props and two more
    // steps (the last its tail call).
    uint64_t levelEffectsFrame(ee::Float seconds)
    {
        aggressive::level::levelObjectsFrame(seconds);
        zge::updateAnimators(seconds);
        updateSoundEmitters(seconds);
        updateLevelEffects(seconds);
        zge::animateTextures(seconds);
        aggressive::level::animateProps(seconds);
        levelFrame3(seconds);
        return game::tail(updateTimers, seconds);
    }
    READABLE_FUNCTION_WAITS(levelEffectsFrame, Verified);

    // updatePlayers: the players' update, both in split screen. Returns 0 in
    // split screen, else 1.
    int32_t updatePlayers(ee::Float seconds)
    {
        const ee::Ptr<Level> level = currentLevel();
        for (int32_t index = 0; index < playerCount(); ++index)
            aggressive::skater::playerUpdate(levelPlayer(level, index), seconds);
        return isSplitScreen() ? 0 : 1;
    }
    READABLE_FUNCTION_WAITS(updatePlayers, Verified);
}
