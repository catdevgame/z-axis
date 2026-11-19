// The water surfaces: the "WaterSurface" level objects, each with the water
// record of its node (water.h), whose waves are stepped every fifth frame
// and disturbed at every fifth touch (the surfaces are made with the waves
// off, so neither moves anything in this build). Their type's registration,
// set-up and messages. DM2's own code (its game block, the level objects).

#include "dynamics_objects.h"
#include "object_node.h"
#include "object_sounds.h"
#include "water.h"

#include "dm2/rider/collision/collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/water_surfaces.cpp.h"

namespace game::dm2
{
    namespace
    {
        // Scaffolding (address-call): DM2's registerLevelObjectType (its code
        // differs from the engine's: the name and the set-up only).
        namespace tail
        {
            uint32_t registerLevelObjectType(ee::Ptr<const char> name, uint32_t setUp)
            {
                return ee::tailCall<uint32_t>(sym::registerLevelObjectType, name, setUp);
            }
        }

        // The surface's component: its water record (word 0) and its sound
        // (word 8, object_sounds.h).
        constexpr uint32_t kWaterTagWords = 8, kWaterTagRecord = 0, kWaterTagSound = 8;
        // The rider's physics' water level (impact.cpp's): the height of the
        // water touched last.
        inline ee::Global<ee::Float, sym::g_waterLevel> g_waterLevel;
        // Every water surface's frames and touches, counted together: the
        // waves step, and a touch disturbs them, on every fifth.
        inline ee::Global<uint32_t, sym::g_waterFrames> g_waterFrames;
        inline ee::Global<uint32_t, sym::g_waterTouches> g_waterTouches;
        constexpr uint32_t kWaterEvery = 5;
        // How deep a touch pushes the wave of the face's first vertex.
        constexpr int16_t kTouchWaveHeight = -25000;

        ee::Ptr<Water> waterRecord(ee::Ptr<zge::SceneObject> surface)
        {
            return ee::Ptr<Water>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(surface), kWaterTagWords, kWaterTagRecord)));
        }
        uint32_t waterSound(ee::Ptr<zge::SceneObject> water)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(water), kWaterTagWords, kWaterTagSound));
        }
        constexpr ee::Ptr<const char> kWaterSurfaceTypeName{sym::waterSurfaceTypeName}; // "WaterSurface"
    }

    // registerWaterSurfaceType: the "WaterSurface" level objects made water surfaces.
    uint32_t registerWaterSurfaceType()
    {
        return tail::registerLevelObjectType(kWaterSurfaceTypeName, sym::newWaterSurface);
    }
    READABLE_FUNCTION(registerWaterSurfaceType, Verified);

    // newWaterSurface (the type's set-up): its update, removed and touch
    // messages; its node's water made, drawn from the view, with no waves,
    // kept in the component; its node collided with; its sound started.
    int32_t newWaterSurface(ee::Ptr<zge::SceneObject> surface)
    {
        zge::subscribe(surface, zge::kUpdateMessage, zge::MessageHandler(sym::waterSurfaceFrame));
        zge::subscribe(surface, zge::kRemovedMessage, zge::MessageHandler(sym::waterSurfaceRemoved));
        zge::subscribe(surface, kTouchMessage, zge::MessageHandler(sym::waterSurfaceTouched));
        const ee::Ptr<Water> water = newWater(zge::objectLevelNode(surface));
        setWaterWaves(water, 0);
        setWaterUvFromView(water, 1);
        setWaterSpareFlag1(water, 0);
        setWaterSpareFlag0(water, 0);
        zge::setTagWord(zge::objectComponent(surface), kWaterTagWords, kWaterTagRecord, water);
        rider::collision::setNodeCollidable(objectNodeHandle(surface), 1);
        startObjectSound(surface, waterSound(surface));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newWaterSurface, Verified);

    // waterSurfaceFrame (the update message): the waves stepped on every
    // fifth frame (of all the surfaces'); its sound has its turn.
    int32_t waterSurfaceFrame(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<Water> water = waterRecord(ee::Ptr<zge::SceneObject>(message->target));
        if (*g_waterFrames % kWaterEvery == 0)
            stepWaterWaves(water);
        ++*g_waterFrames;
        objectSoundMessage(message, waterSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(waterSurfaceFrame, Verified);

    // waterSurfaceRemoved (the removed message): its water record freed;
    // its sound has its turn (and goes).
    int32_t waterSurfaceRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        freeWater(waterRecord(ee::Ptr<zge::SceneObject>(message->target)));
        objectSoundMessage(message, waterSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(waterSurfaceRemoved, Verified);

    // waterSurfaceTouched (the touch message): its height made the water
    // level; on every fifth touch (of all the surfaces') the touched face
    // pushes the waves down; the toucher goes through (the touch not
    // accepted). Its sound has its turn.
    int32_t waterSurfaceTouched(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<TouchData> touch(message->data);
        const ee::Ptr<Water> water = waterRecord(ee::Ptr<zge::SceneObject>(message->target));
        *g_waterLevel = water->surfaceHeight;
        if (*g_waterTouches % kWaterEvery == 0)
            setFaceWaveHeight(water, touch->face, kTouchWaveHeight);
        ++*g_waterTouches;
        touch->accept = 0;
        objectSoundMessage(message, waterSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(waterSurfaceTouched, Verified);
}
