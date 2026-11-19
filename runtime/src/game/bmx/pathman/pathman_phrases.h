#pragma once

// BMX XXX's path men (the level object types "PathMan" and "FastPathMan"):
// the phrases one speaks (pathman_phrases.cpp).

#include "ee/ee.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // A phrase's name hashed onto `hash` (the speaker's name hash), letters
    // as capitals: the sound a path man's phrase plays is looked up by it.
    uint32_t phraseHash(ee::Ptr<const uint8_t> text, uint32_t hash);

    // After a path man has spoken in passing it waits this many frames, and
    // up to as many again, before it does so again.
    constexpr uint32_t kLeastPhraseWait = 0x200;
    constexpr uint32_t kPhraseWaitSpread = 0x200;

    // The path man speaks one of its phrases (PathManPhrase, pathman.h).
    uint32_t sayPathManPhrase(ee::Ptr<zge::SceneObject> pathMan, uint32_t phrase);
    // What the things that carry a path man call: its "pickup" phrase when
    // it is picked up, and its "ride" phrase now and then while it rides.
    uint32_t sayPickupPhrase(ee::Ptr<zge::SceneObject> pathMan);
    void sayRidePhrase(ee::Ptr<zge::SceneObject> pathMan);
}
