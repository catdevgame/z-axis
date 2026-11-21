// BMX XXX's "CutScene" level object type. BMX XXX's own code (its game
// block); names from what each function does and the type's string. No
// recording reaches its activation, which reads the object's entries and
// does nothing with them.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/cut_scenes.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;

    namespace
    {
        constexpr uint32_t kTypeName = sym::cutSceneTypeName; // "CutScene"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // The cut scene's own tag data (objectComponent): ten entries of a
        // word and a text, and a last text. What they mean is not read from
        // this code: nothing uses them.
        constexpr uint32_t kEntries = 10;
        constexpr uint32_t kEntryWordTag = 4;    // word, +kEntryStride an entry: 0 is an empty entry
        constexpr uint32_t kEntryTextTag = 8;    // text
        constexpr uint32_t kEntryStride = 0x44;
        constexpr uint32_t kLastTextTag = 0x2AC; // text
        constexpr uint32_t kTagWord = 8, kTagText = 0x10; // the tag accessors' second argument (the value's kind)
        constexpr uint32_t kTextBytes = 0x40;
    }

    uint32_t initCutScene(ee::Ptr<zge::SceneObject> cutScene);

    // registerCutSceneType: the "CutScene" type, made as a plain entity and
    // set up by initCutScene.
    uint32_t registerCutSceneType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initCutScene));
    }
    READABLE_FUNCTION(registerCutSceneType, Verified);

    // initCutScene (the type's init): its node not drawn and not collided
    // with; it takes the activation. Always 1.
    uint32_t initCutScene(ee::Ptr<zge::SceneObject> cutScene)
    {
        const uint64_t node = objectNodeHandle(cutScene);
        rider::collision::showNode(node, 0);
        rider::collision::setNodeCollidable(node, 0);
        zge::subscribe(cutScene, kActivateMessage, zge::MessageHandler(sym::activateCutScene));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCutScene, Draft);

    // activateCutScene (kActivateMessage): the entries that are not empty
    // gathered in the frame (an empty one's place is taken by the next),
    // then the last text; nothing is done with them.
    int32_t activateCutScene(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<uint32_t[12]> words;
        ee::Local<char[kEntries][kTextBytes]> texts;
        ee::Local<uint32_t[12]> textAddresses;
        ee::Local<char[kTextBytes]> lastText;
        const ee::Ptr<zge::SceneObject> cutScene(message->target);
        uint32_t kept = 0;
        for (uint32_t entry = 0; entry < kEntries; ++entry)
        {
            words[kept] = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(cutScene), kTagWord, kEntryWordTag + entry * kEntryStride));
            const uint32_t text = texts.address() + kept * kTextBytes;
            zge::tagString(zge::objectComponent(cutScene), kTagText, kEntryTextTag + entry * kEntryStride, text);
            textAddresses[kept] = text;
            if (words[kept] != 0)
                ++kept;
        }
        zge::tagString(zge::objectComponent(cutScene), kTagText, kLastTextTag, lastText.address());
        return 0;
    }
    READABLE_FUNCTION(activateCutScene, Draft);
}
