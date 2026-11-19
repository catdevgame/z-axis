#pragma once

// The level objects' records (objects.cpp: a pool of them at levelObjectsBegin .. levelObjectsEnd, each the record
// a scene object's owner word points to) and ZGE4's particle emitter tags, as objects.cpp and BMX XXX's own
// object look-ups (game/bmx/scene) read them.

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace zge
{
    struct TagRecord;

    struct LevelObject
    {
        ListNode link;                 // +0x00 on the object list (g_objectList)
        ee::Ptr<SceneObject> object;   // +0x08 0: the record is free
        ee::Ptr<Node> node;            // +0x0C its root node, models only
        uint32_t type;                 // +0x10 its EntityType
    };
    static_assert(sizeof(LevelObject) == 0x14);
    // (objects.cpp) what the scene object belongs to: one of the pool's records, or something else.
    uint32_t sceneObjectOwner(ee::Ptr<SceneObject> object);

    // ZGE4's particle emitter data (a particle emitter object's animator word), as far as the level objects read
    // it: the emitter's tags (readZpedChunk: the bytes of its ZPED chunk past the record, read in after it;
    // objectComponent's answer for a particle emitter object).
    struct Zge4EmitterTags
    {
        uint8_t _000[0x144];
        ee::Ptr<TagRecord> tags; // +0x144
    };
    // An emitter object keeps its data where other objects keep an animator.
    inline ee::Ptr<Zge4EmitterTags> emitterTagsOf(ee::Ptr<SceneObject> emitter) { return emitter->animator.cast<Zge4EmitterTags>(); }
}
