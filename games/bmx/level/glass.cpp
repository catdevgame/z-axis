// BMX XXX's "Glass" level object type: a pane that breaks when a rider or a
// body runs into it fast enough, or when it is activated. BMX XXX's own code
// (its game block), the line of Aggressive Inline's glass
// (games/aggressive/level/glass.cpp: the same broken byte, the same debris
// from each face) with BMX XXX's messages: a collision that is fast enough
// along the face's normal becomes an activation, and the activation breaks
// the pane: debris from the centre of every face of its model, the type's
// "activate" sound, the pane hidden and no longer collided with. A broken
// pane lets everything through. Its state is saved with its place.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/fx/spawn_effects.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/engine_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/glass.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeHandle;
    using rider::collision::forEachNodeFace;
    using rider::collision::NodeFace;
    using rider::collision::NodeFaceCorner;
    using rider::collision::NodeFaceVisit;

    namespace
    {
        // The pane's own tag data (objectComponent).
        constexpr uint32_t kBrokenTag = 0;      // byte: broken
        constexpr uint32_t kDebrisSpeedTag = 1; // byte: how fast its debris leaves along each face's normal, in hundredths
        constexpr uint32_t kTagByte = 2;        // tagByte's second argument

        constexpr uint32_t kTypeName = sym::glassTypeName;                         // "Glass"
        constexpr uint32_t kActivateSoundFormat = sym::bmx::glassActivateSoundFormat; // "%s.activate"
        // The least speed into a face (along its normal) that breaks the pane.
        inline ee::Global<ee::Float, sym::g_glassBreakSpeed> g_glassBreakSpeed;
        // 0.01: the debris speed tag's unit.
        inline ee::Global<ee::Float, sym::g_glassHundredth> g_glassHundredth;
        // What the debris speed is scaled by on its way to a velocity.
        inline ee::Global<ee::Float, sym::g_glassDebrisSpeedScale> g_glassDebrisSpeedScale;

        constexpr uint32_t kCreateEntity = sym::createEntity;

        // What breakGlass hands the walk of the pane's faces.
        struct GlassBreak
        {
            zge::Vec4 velocity;     // +0x00 the activation's: what broke it was moving so
            ee::Float debrisSpeed;  // +0x10 along each face's normal
        };
        static_assert(offsetof(GlassBreak, debrisSpeed) == 0x10);
    }

    uint32_t initGlass(ee::Ptr<zge::SceneObject> glass);
    int32_t glassCollide(ee::Ptr<zge::EngineMessage> message);
    int32_t breakGlass(ee::Ptr<zge::EngineMessage> message);
    uint32_t glassBroken(ee::Ptr<zge::SceneObject> glass);
    void setGlassBroken(ee::Ptr<zge::SceneObject> glass, uint32_t broken);

    // registerGlassType: the "Glass" type, made as a plain entity and set up
    // by initGlass.
    uint32_t registerGlassType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initGlass));
    }
    READABLE_FUNCTION(registerGlassType, Verified);

    // initGlass (the type's init): shown and collided with unless broken;
    // its messages subscribed. Always 1.
    uint32_t initGlass(ee::Ptr<zge::SceneObject> glass)
    {
        const uint64_t node = objectNodeHandle(glass);
        if (glassBroken(glass) == 0)
        {
            rider::collision::showNode(node, 1);
            rider::collision::setNodeCollidable(node, 1);
        }
        else
        {
            rider::collision::showNode(node, 0);
            rider::collision::setNodeCollidable(node, 0);
        }
        zge::subscribe(glass, kRiderTouchMessage, zge::MessageHandler(sym::glassCollide));
        zge::subscribe(glass, kMessage105, zge::MessageHandler(sym::glassCollide));
        zge::subscribe(glass, kActivateMessage, zge::MessageHandler(sym::breakGlass));
        zge::subscribe(glass, zge::kSaveSizeMessage, zge::MessageHandler(sym::glassSaveSize));
        zge::subscribe(glass, zge::kSaveMessage, zge::MessageHandler(sym::saveGlass));
        zge::subscribe(glass, zge::kRestoreMessage, zge::MessageHandler(sym::restoreGlass));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initGlass, Verified);

    // glassCollide (kRiderTouchMessage and kMessage105): a broken pane lets the
    // collider through. A whole one breaks when the collision is fast
    // enough into the face met (either way through it); the second message
    // breaks it whatever the speed. It breaks by being activated by the
    // collider's rider, with the collision's velocity, and then lets the
    // collider through; a collision too slow leaves it pushed off.
    int32_t glassCollide(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec4> normal;
        ee::Local<ActivationEvent> event;
        ee::Local<zge::EngineMessage> activation;
        const ee::Ptr<zge::SceneObject> glass(message->target);
        const ee::Ptr<RiderTouch> collision(message->data);
        objectNodeHandle(glass);
        if (glassBroken(glass) == 1)
        {
            collision->pushOff = 0;
            return 0;
        }
        if (message->type != kMessage105)
        {
            const uint32_t faceNode = collision->node;
            const int32_t faceIndex = collision->face;
            const rider::collision::FaceRef face =
                rider::collision::nodeFaceCount(rider::collision::widen(faceNode)) != 0 ? rider::collision::makeFace(faceNode, faceIndex) : 0;
            rider::collision::faceNormal(face, &normal->x);
            const ee::Float along = zge::dot(*normal, collision->velocity);
            ee::Float into = -along;
            if (into < ee::Float(0.0f))
            {
                zge::vec3Negate(&normal->xyz(), &normal->xyz());
                into = along;
            }
            if (!(*g_glassBreakSpeed <= into))
                return 0;
        }
        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(message->sender));
        zge::memoryFill(event, 0, sizeof(ActivationEvent));
        event->quiet = 0;
        event->player = rider->player;
        if (message->type != kMessage105)
            event->velocity = zge::quadwordOf(ee::Ptr<zge::Vec4>(&collision->velocity));
        activation->sender = message->sender;
        activation->target = message->target;
        activation->type = kActivateMessage;
        activation->flags = zge::kMessageNow;
        activation->data = event;
        breakGlass(activation);
        collision->pushOff = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(glassCollide, Verified);

    // breakGlass (kActivateMessage): a whole pane broken: debris from each
    // face and the type's "activate" sound (neither when the event's first
    // word is set), then hidden, not collided with, broken.
    int32_t breakGlass(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<GlassBreak> breaking;
        const ee::Ptr<zge::SceneObject> glass(message->target);
        const ee::Ptr<ActivationEvent> event(message->data);
        const uint64_t node = objectNodeHandle(glass);
        if (glassBroken(glass) == 1)
            return 0;
        if (event->quiet == 0)
        {
            breaking->velocity = zge::quadwordOf(ee::Ptr<zge::Vec4>(&event->velocity));
            const uint32_t debrisSpeed = zge::tagByte(zge::objectComponent(glass), kTagByte, kDebrisSpeedTag);
            breaking->debrisSpeed = ee::toFloat(static_cast<int32_t>(debrisSpeed)) * *g_glassHundredth;
            forEachNodeFace(node, NodeFaceVisit(sym::glassFaceDebris), breaking);
            zge::formatString(soundName, kActivateSoundFormat, kTypeName);
            const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
            soundEvent(kObjectSoundEvent, position, kObjectSoundOnce, soundName.address(), kFullVolume);
        }
        rider::collision::showNode(node, 0);
        rider::collision::setNodeCollidable(node, 0);
        setGlassBroken(glass, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(breakGlass, Verified);

    namespace
    {
        // What a pane keeps in a saved level state.
        struct GlassSave
        {
            uint32_t broken;             // +0x00
            uint8_t _04[0xC];
            zge::Vec4 position;          // +0x10
            zge::Quaternion orientation; // +0x20
        };
        static_assert(sizeof(GlassSave) == 0x30 && offsetof(GlassSave, orientation) == 0x20);
    }

    // glassSaveSize (the engine's save size message).
    int32_t glassSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(GlassSave);
        return 0;
    }
    READABLE_FUNCTION(glassSaveSize, Verified);

    // restoreGlass (the engine's restore message): broken or whole as
    // saved, shown and collided with accordingly, at its saved place.
    int32_t restoreGlass(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> glass(message->target);
        const uint64_t node = objectNodeHandle(glass);
        const ee::Ptr<GlassSave> saved(message->data);
        setGlassBroken(glass, saved->broken);
        if (glassBroken(glass) == 0)
        {
            rider::collision::showNode(node, 1);
            rider::collision::setNodeCollidable(node, 1);
        }
        else
        {
            rider::collision::showNode(node, 0);
            rider::collision::setNodeCollidable(node, 0);
        }
        zge::setObjectPosition(glass, &saved->position);
        zge::setObjectOrientation(glass, &saved->orientation);
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreGlass, Verified);

    // saveGlass (the engine's save message).
    int32_t saveGlass(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<GlassSave> saved(message->data);
        saved->broken = glassBroken(ee::Ptr<zge::SceneObject>(message->target));
        zge::aligned(ee::Ptr<zge::Vec4>(&saved->position)) = zge::quadwordOf(zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target)));
        zge::quaternionCopy(&saved->orientation, zge::objectOrientation(ee::Ptr<zge::SceneObject>(message->target)));
        return 0;
    }
    READABLE_FUNCTION(saveGlass, Verified);

    // glassBroken: whether the pane is broken.
    uint32_t glassBroken(ee::Ptr<zge::SceneObject> glass)
    {
        return zge::tagByte(zge::objectComponent(glass), kTagByte, kBrokenTag);
    }
    READABLE_FUNCTION(glassBroken, Verified);

    // setGlassBroken: the pane marked broken (or not).
    void setGlassBroken(ee::Ptr<zge::SceneObject> glass, uint32_t broken)
    {
        game::tail(zge::setTagByte, zge::objectComponent(glass), kTagByte, kBrokenTag, broken & 0xFFu);
    }
    READABLE_FUNCTION(setGlassBroken, Verified);

    // glassFaceDebris (forEachNodeFace's visit; the context is breakGlass's
    // GlassBreak): a piece of debris from the face's centre, thrown along
    // the face's normal at the pane's debris speed on top of the breaking
    // velocity. Always 1: every face.
    uint32_t glassFaceDebris(uint64_t /*node*/, ee::Ptr<NodeFace> face, ee::Ptr<GlassBreak> breaking)
    {
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> velocity;
        *centre = zge::kZero;
        for (uint32_t corner = 0; corner < face->cornerCount; ++corner)
            *centre = *centre + face->corners[corner].position;
        *centre = *centre * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(face->cornerCount)));
        *velocity = face->normal * (breaking->debrisSpeed * *g_glassDebrisSpeedScale);
        *velocity = *velocity + breaking->velocity;
        faceDebris(centre, velocity, &face->normal);
        return 1;
    }
    READABLE_FUNCTION_WAITS(glassFaceDebris, Verified);
}
