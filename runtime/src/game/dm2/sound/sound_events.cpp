// DM2's sound events (its game block, after the rider's physics): the rolling
// sounds of the riders' wheels and how loud a placed sound is heard. DM2's own
// code; Aggressive Inline's sound/events.cpp is its ancestor (a rolling sound
// there has one source, here two).

#include "sound_events.h"
#include "sound_mode.h"
#include "sound_state.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/sound/sound.h"

#include <cstdint>
#include "sym/dm2/sound/sound_events.cpp.h"

namespace game::dm2
{
    namespace
    {

        // A rolling sound's two sources (sound_events.h's RollSlot) start
        // as the main bank's sample 0 (the wheels') and 9 (the second, or
        // 10). Where a rolling sound's sources start: high above the level.
        constexpr float kRollStartHeight = 20000.0f;
        constexpr uint32_t kWheelsSample = 0, kSecondSample = 9;

        // updateRollSound's sound word: the surface's own sample in the low
        // byte, its kind in the next; bit 16 gives the second source sample
        // 10 (else 9), bit 17 a kind 1 sound sample 11 (else the surface's).
        // Kind 2 takes the surface's sample + 12; another kind keeps the
        // sample the wheels have.
        constexpr uint32_t surfaceSampleOf(uint32_t sound) { return sound & 0xFFu; }
        constexpr uint32_t rollKindOf(uint32_t sound) { return sound >> 8 & 0xFFu; }
        constexpr uint32_t kRollKindOwn = 1, kRollKindShifted = 2;
        constexpr uint32_t kRollBit16 = 0x10000, kRollBit17 = 0x20000;
        constexpr uint32_t kBit17Sample = 11, kShiftedSamples = 12;
        constexpr uint32_t kSecondSampleBit16 = 10;
        // Full volume from speed 182 on; the pitch is the speed / 10, at most 3600.
        constexpr int32_t kFullVolumeSpeed = 0xB6;
        constexpr float kPitchPerSpeed = 0.1f;
        constexpr float kTopPitch = 3600.0f;

    }

    // startRollSound: a rolling sound in the first free of the 16 slots: its
    // two sources started at the effects volume, high above the level.
    // Returns the slot, or -1 with none free or a source not started (the
    // first then stopped).
    int32_t startRollSound()
    {
        ee::Local<zge::Vec4> start;
        start->y = kRollStartHeight;
        start->x = 0.0f;
        start->z = 0.0f;
        uint32_t index = 0;
        while (index < kRollSlots && (*g_rollSlots)[index].source != kNoSource)
            ++index;
        if (index == kRollSlots)
            return -1;
        RollSlot &slot = (*g_rollSlots)[index];
        slot.source = static_cast<uint16_t>(zge::startSource(zge::sample(*g_mainBank, kWheelsSample), start));
        if (slot.source == kNoSource)
            return -1;
        slot.secondSource = static_cast<uint16_t>(zge::startSource(zge::sample(*g_mainBank, kSecondSample), start));
        if (slot.secondSource == kNoSource)
        {
            zge::stopSource(slot.source);
            slot.source = kNoSource;
            return -1;
        }
        zge::sourceVolume(slot.source, *g_effectsVolume);
        zge::sourceVolume(slot.secondSource, *g_effectsVolume);
        return static_cast<int32_t>(index);
    }
    READABLE_FUNCTION(startRollSound, Verified);

    // stopRollSound (slot): a rolling sound's two sources stopped. The slot
    // keeps their ids (kept: it is not freed here).
    void stopRollSound(uint32_t index)
    {
        const RollSlot &slot = ee::Ptr<RollSlot>(sym::rollSources)[index];
        zge::stopSource(slot.secondSource);
        game::tail(zge::stopSource, slot.source);
    }
    READABLE_FUNCTION(stopRollSound, Verified);

    // updateRollSound: a rolling sound's frame: its samples by the sound
    // word, both sources' volume (the effects volume, less below speed 182)
    // and pitch (the speed / 10, at most 3600), both at `position`.
    void updateRollSound(uint32_t index, uint32_t sound, ee::Address position, int32_t speed)
    {
        const RollSlot &slot = ee::Ptr<RollSlot>(sym::rollSources)[index];
        if (speed < 0)
            speed = -speed;
        const uint32_t kind = rollKindOf(sound);
        uint32_t surface = surfaceSampleOf(sound);
        uint32_t volume = *g_effectsVolume;
        if (speed < kFullVolumeSpeed)
            volume = static_cast<uint32_t>(static_cast<int32_t>(volume * static_cast<uint32_t>(speed)) / kFullVolumeSpeed) & 0xFFu;
        const ee::Float scaled = ee::toFloat(speed) * ee::Float(kPitchPerSpeed);
        const uint32_t pitch = static_cast<uint32_t>(static_cast<int16_t>(ee::toInt(ee::min(scaled, ee::Float(kTopPitch)))));
        if (kind == kRollKindOwn)
        {
            if (sound & kRollBit17)
                surface = kBit17Sample;
            zge::sourceSample(slot.source, ee::Address(zge::sample(*g_mainBank, surface)));
        }
        else if (kind == kRollKindShifted)
        {
            zge::sourceSample(slot.source, ee::Address(zge::sample(*g_mainBank, surface + kShiftedSamples)));
        }
        zge::sourceVolume(slot.source, volume);
        zge::sourcePitch(slot.source, pitch);
        const uint32_t second = (sound & kRollBit16) ? kSecondSampleBit16 : kSecondSample;
        zge::sourceSample(slot.secondSource, ee::Address(zge::sample(*g_mainBank, second)));
        zge::sourceVolume(slot.secondSource, volume);
        zge::sourcePitch(slot.secondSource, pitch);
        zge::setSourcePosition(slot.source, ee::Ptr<zge::Vec4>(position));
        game::tail(zge::setSourcePosition, slot.secondSource, ee::Ptr<zge::Vec4>(position));
    }
    READABLE_FUNCTION(updateRollSound, Verified);

    // placedVolume (position, level, volume *, pan *, third *): how loud and
    // where the listener hears a sound at `position`, its volume times
    // level / 128; with no position, the level itself, centred (and the
    // third byte 0). Returns positionHeard's answer (1 with no position).
    uint32_t placedVolume(ee::Ptr<zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan,
                          ee::Ptr<uint8_t> third)
    {
        const uint32_t level = levelArgument & 0xFFu;
        if (position)
        {
            const uint32_t heard = zge::positionHeard(position, volume, pan);
            *volume = static_cast<uint8_t>((static_cast<uint32_t>(*volume) * level) >> 7);
            return heard;
        }
        *volume = static_cast<uint8_t>(level);
        *pan = 0;
        *third = 0;
        return 1;
    }
    READABLE_FUNCTION(placedVolume, Verified);
}

namespace game::dm2
{
    namespace
    {
        // How the listener hears a placed sound (placedVolume's outputs).
        struct Placed
        {
            uint8_t volume;
            int8_t pan;
            uint8_t third; // 0 for a sound with no position; nothing reads it
        };

        // The volume kinds event 0 sets (by a): the effects', the music's,
        // the speech's (which nothing plays at here).
        constexpr uint32_t kEffectsVolume = 0, kMusicVolume = 1, kSpeechVolume = 2;

        // The voices' priorities: a sound of its own (unplaced), a placed
        // one (with a pitch and a pan).
        constexpr uint8_t kUnplacedPriority = 0x21, kPlacedPriority = 0x20;
        constexpr uint8_t kFullGroupVolume = 0x7F;

        // Event 20's surface sounds by the kind in b's second byte: the
        // main bank's samples from 49 (another kind) or 58 (kind 2) by the
        // surface in b's low byte, or one of three (kinds 3 .. 5).
        constexpr uint32_t kSurfaceSounds = 49, kKind2SurfaceSounds = 58;
        // Event 22's hits: three by strength (c / 32, at most 2) for each
        // kind, from the main bank's 70.
        constexpr uint32_t kHitSounds = 70, kHitStrengths = 3;
        // Events 25 .. 31's sets in the placed sounds' bank (26 is the
        // looping one), by event.
        constexpr uint32_t kPlacedSetFirst[7] = {0, 0, 8, 12, 16, 20, 24};

        // Event 36 (the camera's place, in Devil's Peak): the side with x
        // above 0 has the level bank's first group louder the nearer the
        // camera is to height -20 (from 127 down to 50, by the data's
        // 127 / 30 a unit).
        inline ee::Global<ee::Float, sym::devilsPeakFadePerUnit> g_devilsPeakFadePerUnit;
        constexpr float kDevilsPeakHeight = -20.0f;
        constexpr int32_t kDevilsPeakNearest = 0x7F, kDevilsPeakFarthest = 0x32;
    }

    // soundEvent (event, a, b, c, d): the one function the game calls for a
    // sound. The event picks what to play or set and what a .. d are (each
    // case says; a is a position for the placed sounds). Returns the event's
    // answer (a track, a soundtrack's index, a source) or 0; 1 for the
    // events this build leaves alone (12, 13, 35 and from 52), 2 for a
    // volume kind past speech. Nothing before the sounds start (sound mode
    // -1); in a level's music phase 2 nothing for events 39 .. 50, paused
    // nothing for 18 .. 35. DM2's own: Aggressive Inline's soundEvent with
    // other events.
    int32_t soundEvent(uint32_t event, ee::Address a, uint32_t b, uint32_t c, uint32_t d)
    {
        // The frame's two locals: a copy of the position a points to
        // (events 36 .. 38), and how the listener hears a placed sound.
        ee::Local<zge::Vec4> position;
        ee::Local<Placed> placed;
        const auto copyPosition = [&] { *position = zge::aligned(ee::Ptr<zge::Vec4>(a)); };
        const auto hear = [&](uint32_t level) {
            placedVolume(ee::Ptr<zge::Vec4>(a), level, &placed->volume, &placed->pan, &placed->third);
        };
        // The shared tails, each answering 0: a sample of a bank at the
        // effects volume, a placed one as heard (or at a volume of its own).
        const auto playUnplaced = [](uint16_t bank, uint32_t index) {
            zge::playSample(zge::sample(bank, index), 0, *g_effectsVolume, 0, kUnplacedPriority);
            return 0;
        };
        const auto playPlaced = [&](uint16_t bank, uint32_t index) {
            zge::playSample(zge::sample(bank, index), 0, placed->volume, placed->pan, kPlacedPriority);
            return 0;
        };
        const auto playPlacedTimes = [&](ee::Ptr<zge::BankSample> sample, int16_t pitch, uint32_t volumeTimes) {
            zge::playSample(sample, pitch, static_cast<uint8_t>(volumeTimes >> 7), placed->pan, kPlacedPriority);
            return 0;
        };
        const auto effectsTimes = [](uint32_t level) { return static_cast<uint8_t>((*g_effectsVolume * level) >> 7); };

        if (*g_gameMode == -1)
            return 0;
        const uint32_t random = zge::randomNumber();
        const int32_t mode = *g_gameMode;
        if (mode == static_cast<int32_t>(kSoundModeLevel) && *g_playPhase == kLastMusicPhase)
        {
            if (event - 39u < 12u)
                return 0;
        }
        else if (mode == static_cast<int32_t>(kSoundModePaused) && event - 18u < 18u)
        {
            return 0;
        }
        if (event >= 52u)
            return 1;

        const uint32_t at = a; // a as a number
        switch (event)
        {
        case 0: // the volume of kind a set to b, and set on what plays
        {
            (&*g_effectsVolume)[at] = static_cast<uint8_t>(b);
            if (at == kMusicVolume)
            {
                if (mode == static_cast<int32_t>(kSoundModeFrontEnd))
                {
                    zge::setVoiceVolume(*g_musicVoice, *g_musicVolume); // the front end's music is a voice
                    return 0;
                }
                if (mode > 0 && mode < static_cast<int32_t>(kSoundModePlacedSounds))
                    zge::setStreamVolume(*g_musicStream, (*g_musicVolume * kMusicVolumeScale) >> 7);
                return 0;
            }
            if (at == kEffectsVolume)
            {
                // In a level (playing or paused): each group's source.
                if (mode == static_cast<int32_t>(kSoundModeFrontEnd) || mode <= 0 ||
                    mode >= static_cast<int32_t>(kSoundModePlacedSounds))
                    return 0;
                for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                    zge::setSourceMix((*g_groupVoices)[group], levelGroupVolume(group), 0);
                return 0;
            }
            if (at == kSpeechVolume)
                return 0;
            return 2;
        }
        case 1: // the sound driver's request {a, b}
            tellSoundDriver(static_cast<uint8_t>(at), static_cast<uint8_t>(b));
            return 0;
        case 2: // the track asked for, else the one playing
            return *g_nextTrack != kNoTrack ? *g_nextTrack : *g_currentTrack;
        case 3: // track a asked for next (the last at most), in the music's
                // phases 0 and 1; the one playing asks nothing
        {
            uint32_t track = at;
            if (!(track < zge::soundtrackTrackCount(*g_music) - 1))
                track = zge::soundtrackTrackCount(*g_music) - 1;
            if (static_cast<int32_t>(track) == *g_currentTrack)
                return 0;
            const int32_t phase = *g_playPhase;
            if (phase < kLastMusicPhase && phase >= 0)
                *g_nextTrack = static_cast<int8_t>(track);
            return 0;
        }
        case 4: // the soundtrack's index
            return zge::soundtrackIndex(*g_music);
        case 5: // the soundtrack of index a
            *g_music = zge::soundtrackAt(at);
            return 0;
        case 6: case 7: case 8: case 10: case 11: // the menu bank's 0, 1, 2, 4, 5 (move, change, select, blocked)
            return playUnplaced(*g_menuBank, event - 6);
        case 9: // cancel: the menu bank's 5 in the front end, else 3
            return playUnplaced(*g_menuBank, mode == static_cast<int32_t>(kSoundModeFrontEnd) ? 5u : 3u);
        case 16: // the menu bank's first group played once
            if (zge::groupCount(*g_menuBank) == 0)
                return 0;
            zge::playGroupOnce(zge::group(*g_menuBank, 0), kFullGroupVolume, 0);
            return 0;
        case 17: // the menu bank's sample b (a must be 0)
            if (at != 0 || !(b < zge::sampleCount(*g_menuBank)))
                return 0;
            zge::playSample(zge::sample(*g_menuBank, b), 0, *g_effectsVolume, 0, kPlacedPriority);
            return 0;
        case 18: // the main bank's sounds by b's low byte, placed at a, at level c: from 30
        case 19: //   from 21
        case 20: //   by b's kind and surface
        case 21: //   from 40
        case 22: //   a hit by b's second byte, by its strength c
        {
            hear(*g_effectsVolume);
            const uint32_t low = b & 0xFFu, kind = b >> 8 & 0xFFu;
            uint32_t index = 0;
            switch (event)
            {
            case 18: index = low + 30; break;
            case 19: index = low + 21; break;
            case 20:
                index = kind == 2 ? low + kKind2SurfaceSounds : kind == 3 ? 67u : kind == 4 ? 69u : kind == 5 ? 68u : low + kSurfaceSounds;
                break;
            case 21: index = low + 40; break;
            default: index = kind * kHitStrengths + (c >> 5 < 3u ? c >> 5 : 2u) + kHitSounds; break;
            }
            return playPlacedTimes(zge::sample(*g_mainBank, index), 0, placed->volume * c);
        }
        case 23: // the main bank's b from 97, placed at a, louder by c
            hear(*g_effectsVolume);
            placed->volume = static_cast<uint8_t>((((c << 5) + 31) * placed->volume) >> 7);
            return playPlaced(*g_mainBank, b + 97);
        case 24: // one of the crowd bank's four at random, placed at a, louder and higher by c
        {
            hear(*g_effectsVolume);
            placed->volume = static_cast<uint8_t>((((c << 4) + 79) * placed->volume) >> 7);
            const ee::Ptr<zge::BankSample> sample = zge::sample(*g_crowdBank, random & 0x3u);
            zge::playSample(sample, static_cast<int16_t>(static_cast<int32_t>(c << 22) >> 16), placed->volume, placed->pan,
                            kPlacedPriority);
            return 0;
        }
        case 26: // the looping placed sound: the placed bank's b from 4 (a new sample stops the old one)
        {
            const uint32_t index = b + 4;
            hear(*g_effectsVolume);
            if (*g_loopVoice != kNoVoice && *g_loopSample != index)
            {
                zge::stopVoice(*g_loopVoice, 3);
                *g_loopVoice = kNoVoice;
            }
            if (*g_loopVoice == kNoVoice)
            {
                *g_loopVoice = static_cast<uint8_t>(zge::allocateVoice(zge::sample(*g_placedBank, index), kUnplacedPriority));
                if (*g_loopVoice == kNoVoice)
                    return 0;
                *g_loopSample = static_cast<uint16_t>(index);
            }
            zge::setVoiceVolume(*g_loopVoice, placed->volume);
            zge::setVoicePan(*g_loopVoice, placed->pan);
            zge::startVoice(*g_loopVoice);
            return 0;
        }
        case 25: case 27: case 28: case 29: case 30: case 31: // the placed bank's sets of b, placed at a
            hear(*g_effectsVolume);
            return playPlaced(*g_placedBank, kPlacedSetFirst[event - 25] + b);
        case 32: // the placed bank's 28, placed at a (33: 29; 34: 30 or 31 at random)
            hear(*g_effectsVolume);
            return playPlaced(*g_placedBank, 28);
        case 33:
            hear(*g_effectsVolume);
            return playPlaced(*g_placedBank, 29);
        case 34:
            hear(*g_effectsVolume);
            return playPlaced(*g_placedBank, (random & 0x1u) | 0x1Eu);
        case 36: // the camera at a, in Devil's Peak: the level bank's groups by its side
        {
            copyPosition();
            if (*g_sessionLevel != kDevilsPeakLevel)
                return 0;
            if (ee::Float(0.0f) < position->x)
            {
                if (*g_firstGroupQuiet != 0)
                {
                    tellSoundDriver(2, 70);
                    for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                    {
                        const uint32_t volume = group == 0 ? *g_effectsVolume : (*g_effectsVolume * kQuietGroupVolume) >> 7;
                        zge::setSourceMix((*g_groupVoices)[group], volume, 0);
                    }
                    *g_firstGroupQuiet = 0;
                }
                if (zge::groupCount(*g_sourceBank) == 0)
                    return 0;
                const ee::Float away = ee::abs(position->y - ee::Float(kDevilsPeakHeight)) * *g_devilsPeakFadePerUnit;
                const int32_t level = kDevilsPeakNearest - ee::toInt(away);
                placed->volume = static_cast<uint8_t>(level < kDevilsPeakFarthest ? kDevilsPeakFarthest : level);
                zge::setSourceMix((*g_groupVoices)[0], effectsTimes(placed->volume), 0);
                return 0;
            }
            if (*g_firstGroupQuiet != 0)
                return 0;
            tellSoundDriver(2, 1);
            for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
            {
                const uint32_t volume = group == 0 ? (*g_effectsVolume * kQuietGroupVolume) >> 7 : *g_effectsVolume;
                zge::setSourceMix((*g_groupVoices)[group], volume, 0);
            }
            *g_firstGroupQuiet = 1;
            return 0;
        }
        case 37: // the sound sources: b 0 start the level bank's c at a (level d; returns it),
        case 38: //   1 stop source c, 2 its level d and position a, 3 the bank's c placed at a once
        {
            copyPosition();
            const uint16_t source = static_cast<uint16_t>(c);
            switch (b)
            {
            case 1:
                if (source != kNoSource)
                    zge::stopSource(source);
                return 0;
            case 0:
            {
                const uint16_t started = static_cast<uint16_t>(zge::startSource(zge::sample(*g_sourceBank, c), position));
                if (started == kNoSource)
                    return kNoSource;
                zge::sourceVolume(started, effectsTimes(d));
                return started;
            }
            case 2:
                if (source == kNoSource)
                    return 0;
                zge::sourceVolume(source, effectsTimes(d));
                zge::setSourcePosition(source, position);
                return 0;
            case 3:
                if (!(c < zge::sampleCount(*g_sourceBank)))
                    return 0;
                hear(effectsTimes(d));
                return playPlaced(*g_sourceBank, c);
            default:
                return 0;
            }
        }
        case 39: // the countdown's second a: the main bank's 102 at 0, 101 from 1 to 10
            if (at == 0)
                return playUnplaced(*g_mainBank, 102);
            if (at > 10)
                return 0;
            return playUnplaced(*g_mainBank, 101);
        case 40: // a trick line's next trick (a: the score's code 2): the main bank's 103, pitched a * 33 (500 at most)
        {
            const uint32_t pitch = at * 33 < 501u ? at * 33 : 500u;
            zge::playSample(zge::sample(*g_mainBank, 103), static_cast<int16_t>(pitch), *g_effectsVolume, 0, kUnplacedPriority);
            return 0;
        }
        case 42: // a trick line's end (a: its cheer, or the score's code 2): the level bank's 136 or 137 (b 0: 138
                 // or 139; the second from a 4), at (a - 2) * 16 + 63 and pitched (a - 2) * 30 + 256 less 0 .. 511
                 // at random
        {
            const uint32_t over = at - 2;
            placed->volume = static_cast<uint8_t>(over * 16 + 63);
            const int16_t pitch = static_cast<int16_t>(over * 30 + 256 - (random & 0x1FFu));
            const uint32_t index = b != 0 ? (at < 4 ? 0x88u : 0x89u) : (at < 4 ? 0x8Au : 0x8Bu);
            zge::playSample(zge::sample(*g_sourceBank, index), pitch, static_cast<uint8_t>((placed->volume * *g_effectsVolume) >> 7),
                            0, kPlacedPriority);
            return 0;
        }
        case 43: // a run's end: the main bank's 104 (a 1), 105 (a 0) or 106
            return playUnplaced(*g_mainBank, at == 1 ? 104u : at == 0 ? 105u : 106u);
        // The main bank's sounds by a or by the event.
        case 44: return playUnplaced(*g_mainBank, at + 106);
        case 45: return playUnplaced(*g_mainBank, at + 109);
        case 46: case 47: case 48: case 49: return playUnplaced(*g_mainBank, event + 67);
        case 50: return playUnplaced(*g_mainBank, at + 117);
        case 51: return playUnplaced(*g_mainBank, 120);
        case 12: case 13: case 35: // events this build leaves alone, answered 1
            return 1;
        default: // 14, 15, 41: nothing
            return 0;
        }
    }
    READABLE_FUNCTION_WAITS(soundEvent, Verified);
}
