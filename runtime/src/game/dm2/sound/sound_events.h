#pragma once
// DM2's sound events (sound_events.cpp): the riders' rolling sounds and how
// loud a placed sound is heard.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstdint>
#include "sym/dm2/sound/sound_events.h.h"

namespace game::dm2
{
    // The rolling sounds (16 slots; a rider's rollSound picks one): each
    // two sources, the wheels' and a second one. The sound mode frees them
    // (setGameMode: a level's start frees the wheels' of each, its end both).
    constexpr uint32_t kRollSlots = 16;
    constexpr uint16_t kNoSource = 0xFFFF;
    struct RollSlot
    {
        uint16_t source;       // 0xFFFF: free
        uint16_t secondSource;
    };
    static_assert(sizeof(RollSlot) == 4);
    inline ee::Global<RollSlot[kRollSlots], sym::rollSources> g_rollSlots;

    // A rolling sound started in a free slot (its index, or -1), stopped, and
    // its frame (its samples by the sound word, its volume and pitch by the
    // speed, at a position).
    int32_t startRollSound();
    void stopRollSound(uint32_t index);
    void updateRollSound(uint32_t index, uint32_t sound, ee::Address position, int32_t speed);
    // How loud and where the listener hears a sound at `position` (none: the
    // level itself, centred).
    // The sound event (the game's one call for a sound: sound_events.cpp's
    // cases say what each event does with a .. d); its answer or 0.
    int32_t soundEvent(uint32_t event, ee::Address a, uint32_t b, uint32_t c, uint32_t d);
    uint32_t placedVolume(ee::Ptr<zge::Vec4> position, uint32_t level, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan,
                          ee::Ptr<uint8_t> third);
}
