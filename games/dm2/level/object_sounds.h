#pragma once
// The level objects' sounds (object_sounds.cpp): a level object's sound (its
// tag's number, 1 .. 16: the level bank's sample number + 7) played from the
// object as a sound source, kept by object in a search tree; the one-shot
// sounds the object's messages play (a row of 16 samples each), and the
// frame that moves the kept sounds with their objects.

#include "ee/ee.h"
#include "zge/scene/scene.h"
#include "zge/scene/message.h"

#include <cstdint>

namespace game::dm2
{
    // The object's sound started (soundEvent 38 at the object's position,
    // full level) and kept with the object; nothing for a number out of
    // 1 .. 16 or a source not started.
    void startObjectSound(ee::Ptr<zge::SceneObject> object, uint32_t sound);
    // The object's sound has its turn at a message the object took (every
    // level object with a sound hands it each of its messages): the
    // message's row played once at the object, or at the removed message
    // the kept sound stopped. soundEvent's answer, or -1 when nothing
    // played.
    int32_t objectSoundMessage(ee::Ptr<zge::EngineMessage> message, uint32_t sound);
    // The kept sounds moved to their objects, the recent touches aged (each
    // play and replay frame).
    void objectSoundsFrame();
    // Every kept sound stopped and the sounds' records gone (the level's
    // end).
    void freeObjectSounds();
}
