// The glass: the "Glass" level objects, panes a rider breaks by riding into
// them (BMX XXX's glass, game/bmx/level/glass.cpp, descends from it): shown
// and tested while whole; the first touch breaks it (hidden, untested, a
// piece of debris from each face of its model, a sample), and the toucher
// goes through. Its state, place and turn are saved. DM2's own code (its
// game block, the level objects).

#include "breakable_save.h"
#include "dynamics_objects.h"
#include "object_node.h"
#include "object_sounds.h"

#include "dm2/fx/spawns.h"
#include "dm2/rider/collision/node_faces.h"
#include "dm2/sound/sound_events.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/level/glass.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kGlassTypeName{sym::glassTypeName}; // "Glass"

        // The pane's component: its sound (word 0, object_sounds.h) and
        // whether it is broken (byte 4).
        constexpr uint32_t kTagByte = 2, kTagWord = 8, kGlassTagSound = 0, kGlassTagBroken = 4;
        // The break's sample: soundEvent 37 (the sound sources), b 3: the
        // level bank's sample 3 once at the pane.
        constexpr uint32_t kSoundSourcesEvent = 37, kPlayOnce = 3, kBreakSample = 3, kFullLevel = 0x7F;
        uint32_t glassSound(ee::Ptr<zge::SceneObject> glass)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(glass), kTagWord, kGlassTagSound));
        }
    }

    // registerGlassType: the "Glass" level objects made panes of glass.
    uint32_t registerGlassType()
    {
        return game::tail(zge::registerEntityInit, kGlassTypeName, sym::newGlass);
    }
    READABLE_FUNCTION(registerGlassType, Verified);

    // glassBroken: whether the pane is broken.
    uint32_t glassBroken(ee::Ptr<zge::SceneObject> glass)
    {
        return zge::tagByte(zge::objectComponent(glass), kTagByte, kGlassTagBroken);
    }
    READABLE_FUNCTION(glassBroken, Verified);

    // setGlassBroken: the pane marked broken (or not).
    void setGlassBroken(ee::Ptr<zge::SceneObject> glass, uint32_t broken)
    {
        game::tail(zge::setTagByte, zge::objectComponent(glass), kTagByte, kGlassTagBroken, broken & 0xFFu);
    }
    READABLE_FUNCTION(setGlassBroken, Verified);

    // glassSaveSize (the save-size message): a pane's save is 48 bytes; its
    // sound has its turn.
    int32_t glassSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(BreakableSave);
        objectSoundMessage(message, glassSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(glassSaveSize, Verified);

    // glassSave (the save message): whether it is broken, where it stands
    // and how it is turned; its sound has its turn.
    int32_t glassSave(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<BreakableSave> save(message->data);
        save->broken = glassBroken(ee::Ptr<zge::SceneObject>(message->target));
        // (A quadword store: the save may sit off a 16-byte boundary.)
        zge::aligned(ee::Ptr<zge::Vec4>(&save->position)) =
            zge::quadwordOf(zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)));
        zge::quaternionCopy(&save->orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)));
        objectSoundMessage(message, glassSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(glassSave, Verified);

    // newGlass: the object made a pane: shown and tested unless broken; its
    // touch, save-size, save and restore messages; its sound started.
    int32_t newGlass(ee::Ptr<zge::SceneObject> glass)
    {
        const uint64_t node = objectNodeHandle(glass);
        const uint32_t whole = glassBroken(glass) == 0 ? 1u : 0u;
        rider::collision::showNode(node, whole);
        rider::collision::setNodeCollidable(node, whole);
        zge::subscribe(glass, kTouchMessage, zge::MessageHandler(sym::glassTouched));
        zge::subscribe(glass, zge::kSaveSizeMessage, zge::MessageHandler(sym::glassSaveSize));
        zge::subscribe(glass, zge::kSaveMessage, zge::MessageHandler(sym::glassSave));
        zge::subscribe(glass, zge::kRestoreMessage, zge::MessageHandler(sym::restoreGlass));
        startObjectSound(glass, static_cast<uint32_t>(zge::tagWord(zge::objectComponent(glass), kTagWord, kGlassTagSound)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newGlass, Verified);

    // glassFaceDebris (forEachNodeFace's visit; the context is the touch's
    // velocity): a piece of debris from the face's centre (its corners'
    // average). Always 1: every face.
    uint32_t glassFaceDebris(uint64_t /*node*/, ee::Ptr<rider::collision::NodeFace> face, ee::Ptr<zge::Vec3> velocity)
    {
        ee::Local<zge::Vec4> centre;
        *centre = zge::kZero;
        const uint32_t corners = face->vertexCount;
        for (uint32_t corner = 0; corner < corners; ++corner)
            *centre = *centre + face->vertices[corner].position;
        *centre = *centre * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(corners)));
        debris(centre, velocity, &face->normal);
        return 1;
    }
    READABLE_FUNCTION_WAITS(glassFaceDebris, Verified);

    // glassTouched (the touch message): a broken pane lets the toucher
    // through; a whole one breaks (hidden and untested, debris from each
    // face with the touch's velocity, the break's sample) and lets it
    // through. Its sound has its turn.
    int32_t glassTouched(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> glass(message->target);
        const ee::Ptr<TouchData> touch(message->data);
        if (glassBroken(glass) == 1)
            touch->accept = 0;
        else
        {
            const uint64_t node = objectNodeHandle(glass);
            rider::collision::showNode(node, 0);
            rider::collision::setNodeCollidable(node, 0);
            rider::collision::forEachNodeFace(node, rider::collision::NodeFaceVisit(sym::glassFaceDebris), &touch->velocity);
            soundEvent(kSoundSourcesEvent, zge::objectPosition(glass), kPlayOnce, kBreakSample, kFullLevel);
            setGlassBroken(glass, 1);
            touch->accept = 0;
        }
        objectSoundMessage(message, glassSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(glassTouched, Verified);

    // restoreGlass (the restore message): broken or whole as saved (shown
    // and tested while whole), at its saved place and turn; its sound has
    // its turn.
    int32_t restoreGlass(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> glass(message->target);
        const uint64_t node = objectNodeHandle(glass);
        const ee::Ptr<BreakableSave> save(message->data);
        setGlassBroken(glass, save->broken);
        const uint32_t whole = glassBroken(glass) == 0 ? 1u : 0u;
        rider::collision::showNode(node, whole);
        rider::collision::setNodeCollidable(node, whole);
        zge::setObjectPosition(glass, &save->position);
        zge::setObjectOrientation(glass, &save->orientation);
        objectSoundMessage(message, glassSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreGlass, Verified);
}
