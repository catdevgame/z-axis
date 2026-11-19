#pragma once
// BMX XXX's sound events (bmx.c, events.cpp): the one function the game
// calls for a sound, how the listener hears a placed sound, the speech
// channel's phrases and the sound's frame. BMX XXX's own code; AI's
// game/aggressive/sound/events is the same design one game earlier (its
// event numbers differ).

#include "ee/plain.h"

#include <cstdint>

namespace zge { struct Vec4; }

namespace game::bmx
{
    // soundEvent (event, a, b, c, d): the event picks what to play or set
    // and what a .. d are (events.cpp says for each). Returns the event's
    // answer (a track, a name, a sound source) or 0; 1 for the events this
    // build leaves alone, 2 for a volume kind past speech.
    uint32_t soundEvent(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d);

    // How loud and where the listener hears a sample at `position`: the
    // volume it hears times level / 128; with no position, the level itself,
    // centred. Returns the engine's answer (1 with no position).
    uint32_t placedVolume(uint32_t sample, ee::Ptr<const zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan, ee::Ptr<uint8_t> third);

    // A phrase (a playlist of the speech bank) played on the speech channel,
    // heard from `position` when there is one. Nothing while the phrase
    // before it is held (returns kSpeechHeld), else the channel's answer (0:
    // it plays).
    // The volume and soundtrack events (the menus' script functions send
    // them): a channel's volume set; the track playing, a track played, how
    // many there are, a track put on and off the playlist and whether it is
    // on, its title and its artist.
    constexpr uint32_t kSoundSetVolume = 0;
    constexpr uint32_t kSoundCurrentTrack = 2, kSoundPlayTrack = 3, kSoundTrackCount = 4, kSoundTrackOn = 5,
                       kSoundTrackOff = 6, kSoundTrackIsOn = 7, kSoundTrackTitle = 8, kSoundTrackArtist = 9;
    // The menus' sound events (the UI's five sound hooks send them).
    constexpr uint32_t kSoundMenuMove = 12, kSoundMenuChange = 13, kSoundMenuSelect = 14, kSoundMenuCancel = 15,
                       kSoundMenuBlocked = 16;
    constexpr int32_t kSpeechHeld = 0x603;
    int32_t playSpeech(uint32_t phrase, ee::Ptr<const zge::Vec4> position);

    // The sound's frame: the music's playlist moved on, the speech kept
    // where it is heard from, then the engine's sound frames.
    void updateGameFrame();
}
