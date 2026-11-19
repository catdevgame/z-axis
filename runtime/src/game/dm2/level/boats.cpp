// The boats: the "Boat" level objects, each on a path follower as a traffic
// car is (traffic_cars.cpp: the same junction choice), at the speed its
// component gives, with no driving of its own, and a wake where it goes
// (water_wake.h). Made, and its messages: the frame, its save, its removal,
// a touch. DM2's own code (its game block, after the water surfaces).

#include "dynamics_objects.h"
#include "object_sounds.h"
#include "traffic_cars.h"
#include "water_wake.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/boats.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kBoatTypeName{sym::boatTypeName}; // "Boat"

        // The boat's component (eight words): its follower (word 0), its
        // path's name hash (4), its speed (a float at 8), the weights of a
        // junction's three exits (floats from 0x0C), its random number
        // (0x18) and its sound (0x1C, object_sounds.h).
        constexpr uint32_t kBoatTagWords = 8, kBoatTagFollower = 0, kBoatTagPath = 4, kBoatTagSpeed = 8;
        constexpr uint32_t kBoatTagExitWeights = 0x0C, kBoatTagSound = 0x1C;
        // The follower's end mode for a boat: on from the path's other end
        // (follower.cpp's kFollowerLoops).
        constexpr uint32_t kBoatFollowerLoops = 2;

        // Every boat's frame counts here (nothing reads it).
        inline ee::Global<uint32_t, sym::g_boatFrames> g_boatFrames;

        // A boat's save (the save message): where its follower is, then its
        // component's record as it is.
        constexpr uint32_t kBoatComponentBytes = 0x20;
        struct BoatSave
        {
            uint32_t segment;  // +0x00 its segment's name hash
            uint32_t endMode;  // +0x04 the follower's
            ee::Float speed;   // +0x08
            ee::Float along;   // +0x0C
            uint8_t component[kBoatComponentBytes]; // +0x10
        };
        static_assert(offsetof(BoatSave, component) == 0x10 && sizeof(BoatSave) == 0x30);

        // The boat's sound.
        uint32_t boatSound(ee::Ptr<zge::SceneObject> boat)
        {
            return static_cast<uint32_t>(zge::tagWord(zge::objectComponent(boat), kBoatTagWords, kBoatTagSound));
        }
    }

    // setBoatFollower, boatFollower: the boat's follower, its component's
    // first word (the code of object_components.cpp's pair, word for word).
    int32_t setBoatFollower(ee::Ptr<zge::SceneObject> boat, ee::Address follower)
    {
        return game::tail(zge::setTagWord, zge::objectComponent(boat), kBoatTagWords, kBoatTagFollower, follower);
    }
    READABLE_FUNCTION(setBoatFollower, Verified);

    ee::Ptr<zge::Follower> boatFollower(ee::Ptr<zge::SceneObject> boat)
    {
        return ee::Ptr<zge::Follower>(static_cast<uint32_t>(zge::tagWord(zge::objectComponent(boat), kBoatTagWords, kBoatTagFollower)));
    }
    READABLE_FUNCTION(boatFollower, Verified);

    // newBoat: the object made a boat: its messages; a follower on its path
    // at its speed (on from the path's other end at the last point),
    // snapped to where it stands; shown; its sound started.
    int32_t newBoat(ee::Ptr<zge::SceneObject> boat)
    {
        zge::subscribe(boat, zge::kUpdateMessage, zge::MessageHandler(sym::boatFrame));
        zge::subscribe(boat, zge::kRemovedMessage, zge::MessageHandler(sym::boatRemoved));
        zge::subscribe(boat, zge::kSaveMessage, zge::MessageHandler(sym::boatSave));
        zge::subscribe(boat, zge::kRestoreMessage, zge::MessageHandler(sym::boatRestore));
        zge::subscribe(boat, kTouchMessage, zge::MessageHandler(sym::boatTouched));
        zge::subscribe(boat, zge::kSaveSizeMessage, zge::MessageHandler(sym::boatSaveSize));
        const int32_t pathName = zge::tagWord(zge::objectComponent(boat), kBoatTagWords, kBoatTagPath);
        const ee::Float speed = zge::tagFloat(zge::objectComponent(boat), kBoatTagWords, kBoatTagSpeed);
        const ee::Ptr<zge::PathRecord> path = zge::findPathRecord(static_cast<uint32_t>(pathName));
        const ee::Ptr<zge::Follower> follower = zge::newPathFollower(path, kBoatFollowerLoops, speed, ee::Float(0.0f));
        setBoatFollower(boat, follower);
        zge::followerSnap(follower, zge::objectPosition(boat));
        zge::setSceneObjectShown(boat, 1);
        startObjectSound(boat, static_cast<uint32_t>(zge::tagWord(zge::objectComponent(boat), kBoatTagWords, kBoatTagSound)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(newBoat, Verified);

    // boatRemoved (the removed message): the boat's follower freed; its
    // sound has its turn (and goes).
    int32_t boatRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        zge::freePathFollower(boatFollower(ee::Ptr<zge::SceneObject>(message->target)));
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatRemoved, Verified);

    // boatFrame (the update message): the boat's frame: along its segment by
    // the frame's step, or at the segment's end on to the next one (the way
    // it goes, else back; it stays on its follower's own course with none);
    // turned to the path and put at the follower's point; its wake where it
    // is, by how far it moved; its sound's turn.
    int32_t boatFrame(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::Vec3> up;
        ee::Local<zge::Vec4> moved;
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> now;
        ee::Local<zge::Matrix> placed;
        ++*g_boatFrames;
        const ee::Ptr<zge::SceneObject> boat(message->target);
        const ee::Ptr<zge::Follower> follower = boatFollower(boat);
        const ee::Float step = *ee::Ptr<ee::Float>(message->data);
        zge::followerPosition(follower, start);

        const ee::Float speed = zge::followerSpeed(follower);
        const ee::Float length = zge::segmentEnd(zge::followerSegment(follower));
        const ee::Float next = zge::followerAlong(follower) * length + speed * step;
        if (next < ee::Float(0.0f) || length < next)
        {
            const uint32_t backwards = speed < ee::Float(0.0f) ? 1u : 0u;
            int32_t segment = chooseNextSegment(boat, follower, backwards, kBoatTagWords, kBoatTagExitWeights);
            if (segment == kNoSegment)
                segment = chooseNextSegment(boat, follower, backwards ^ 1u, kBoatTagWords, kBoatTagExitWeights);
            if (segment == kNoSegment)
                zge::advanceFollower(follower, step);
            else
            {
                zge::setFollowerSegment(follower, zge::findPathRecord(static_cast<uint32_t>(segment)));
                zge::setFollowerSpeed(follower, speed);
                zge::setFollowerAlong(follower, ee::Float(0.0f) < speed ? ee::Float(0.0f) : ee::Float(1.0f));
            }
        }
        else
            zge::advanceFollower(follower, step);
        zge::followerPosition(follower, now);
        *moved = *now - *start;

        // Turned to the path (its forward and side axes the other way) and
        // put at the follower's point; the wake there.
        *up = zge::Vec3{ee::Float(0.0f), ee::Float(1.0f), ee::Float(0.0f)};
        zge::followerOrientation(follower, up, placed);
        for (const int row : {2, 0})
        {
            zge::Vec4 &axisRow = placed->row(row);
            axisRow.x = -axisRow.x;
            axisRow.y = -axisRow.y;
            axisRow.z = -axisRow.z;
        }
        zge::setObjectRotationMatrix(boat, placed);
        zge::setObjectPosition(boat, &placed->translation());
        zge::updatePose(boat);
        startWaterWake(&placed->translation(), moved);
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatFrame, Verified);

    // boatSaveSize (the save-size message): a boat's save is 48 bytes; its
    // sound has its turn.
    int32_t boatSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(BoatSave);
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatSaveSize, Verified);

    // boatSave (the save message): where its follower is and the boat's
    // component, made in the frame and copied out; its sound has its turn.
    int32_t boatSave(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<BoatSave> save;
        const ee::Ptr<zge::SceneObject> boat(message->target);
        const ee::Ptr<zge::Follower> follower = boatFollower(boat);
        save->endMode = zge::followerEndMode(follower);
        save->segment = zge::pathNameHash(zge::followerSegment(follower));
        save->along = zge::followerAlong(follower);
        save->speed = zge::followerSpeed(follower);
        zge::memoryCopy(save->component, zge::objectComponent(boat), kBoatComponentBytes);
        zge::memoryCopy(message->data, save, sizeof(BoatSave));
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatSave, Verified);

    // boatRestore (the restore message): the follower put back where the
    // save says, the boat's component copied back (its follower word set
    // again); its sound has its turn.
    int32_t boatRestore(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> boat(message->target);
        const ee::Ptr<zge::Follower> follower = boatFollower(boat);
        const ee::Ptr<BoatSave> save(message->data);
        zge::setFollowerSegment(follower, zge::findPathRecord(save->segment));
        zge::setFollowerAlong(follower, save->along);
        zge::setFollowerSpeed(follower, save->speed);
        zge::setFollowerEndMode(follower, save->endMode);
        zge::memoryCopy(zge::objectComponent(boat), save->component, kBoatComponentBytes);
        setBoatFollower(boat, follower);
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatRestore, Verified);

    // boatTouched (the touch message): its sound has its turn.
    int32_t boatTouched(ee::Ptr<zge::EngineMessage> message)
    {
        objectSoundMessage(message, boatSound(ee::Ptr<zge::SceneObject>(message->target)));
        return 1;
    }
    READABLE_FUNCTION_WAITS(boatTouched, Draft);

    // registerBoatType: the "Boat" level objects made boats.
    uint32_t registerBoatType()
    {
        return game::tail(zge::registerEntityInit, kBoatTypeName, sym::newBoat);
    }
    READABLE_FUNCTION(registerBoatType, Verified);
}
