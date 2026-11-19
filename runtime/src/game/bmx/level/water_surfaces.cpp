// BMX XXX's "WaterSurface" level object type: water a rider falls into.
// BMX XXX's own code (its game block); names from what each function does
// and the type's string.

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
#include "sym/bmx/level/water_surfaces.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeFaceCount;
    using rider::collision::nodeHandle;

    namespace
    {
        constexpr uint32_t kTypeName = sym::waterSurfaceTypeName; // "WaterSurface"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // What the rider is given when it touches the water: its bail's
        // kind and its contact's kind (setContact's), both used here only
        // among the functions read so far.
        constexpr uint8_t kBailInWater = 9;
        constexpr uint32_t kContactWater = 10;
    }

    uint32_t initWaterSurface(ee::Ptr<zge::SceneObject> water);

    // registerWaterSurfaceType: the "WaterSurface" type, made as a plain
    // entity and set up by initWaterSurface.
    uint32_t registerWaterSurfaceType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initWaterSurface));
    }
    READABLE_FUNCTION(registerWaterSurfaceType, Verified);

    // initWaterSurface (the type's init): its messages subscribed; the
    // rider's collision meets its node. Always 1.
    uint32_t initWaterSurface(ee::Ptr<zge::SceneObject> water)
    {
        zge::subscribe(water, zge::kRemovedMessage, zge::MessageHandler(sym::waterSurfaceRemoved));
        zge::subscribe(water, zge::kUpdateMessage, zge::MessageHandler(sym::updateWaterSurface));
        zge::subscribe(water, kRiderTouchMessage, zge::MessageHandler(sym::riderTouchesWater));
        zge::subscribe(water, kEnableMessage, zge::MessageHandler(sym::enableWaterSurface));
        zge::subscribe(water, kDisableMessage, zge::MessageHandler(sym::disableWaterSurface));
        rider::collision::setNodeCollidable(objectNodeHandle(water), 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initWaterSurface, Verified);

    // waterSurfaceRemoved (the engine's removed message): nothing to do.
    int32_t waterSurfaceRemoved(ee::Ptr<zge::EngineMessage>)
    {
        return 0;
    }
    READABLE_FUNCTION(waterSurfaceRemoved, Verified);

    // updateWaterSurface (the frame's update): nothing to do.
    int32_t updateWaterSurface(ee::Ptr<zge::EngineMessage>)
    {
        return 0;
    }
    READABLE_FUNCTION(updateWaterSurface, Verified);

    // riderTouchesWater (kRiderTouchMessage): the rider bails into the
    // water, its contact the face touched, unless the touch is none or a
    // crash already. Either way the rider is not pushed off.
    int32_t riderTouchesWater(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<uint64_t> contact;
        const ee::Ptr<RiderTouch> touch(message->data);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(rider::playerIndexOfNode(message->sender));
        if (touch->kind != kTouchNone && touch->kind != kTouchCrash)
        {
            rider::setPhysicsState(rider, rider::kPhysicsBail);
            rider->bailKind = kBailInWater;
            const uint32_t node = touch->node;
            const uint32_t face = touch->face;
            if (nodeFaceCount(node) == 0)
                *contact = 0;
            else
                *contact = (static_cast<uint64_t>(face) << 32) | node;
            rider::setContact(rider, contact, kContactWater);
        }
        touch->pushOff = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(riderTouchesWater, Verified);

    // enableWaterSurface (kEnableMessage): watched for riders again, its
    // node collided with and drawn.
    int32_t enableWaterSurface(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(message->target));
        zge::subscribe(message->target, zge::kUpdateMessage, zge::MessageHandler(sym::updateWaterSurface));
        zge::subscribe(message->target, kRiderTouchMessage, zge::MessageHandler(sym::riderTouchesWater));
        rider::collision::setNodeCollidable(nodeHandle(ee::Address(node)), 1);
        rider::collision::showNode(nodeHandle(ee::Address(node)), 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableWaterSurface, Draft);

    // disableWaterSurface (kDisableMessage): no longer watched, its node
    // neither collided with nor drawn.
    int32_t disableWaterSurface(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(message->target));
        zge::unsubscribe(message->target, zge::kUpdateMessage, zge::MessageHandler(sym::updateWaterSurface));
        zge::unsubscribe(message->target, kRiderTouchMessage, zge::MessageHandler(sym::riderTouchesWater));
        rider::collision::setNodeCollidable(nodeHandle(ee::Address(node)), 0);
        rider::collision::showNode(nodeHandle(ee::Address(node)), 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableWaterSurface, Draft);
}
