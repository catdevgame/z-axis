#pragma once

// DM2's dynamics objects (dynamics_objects.cpp, bodies.cpp): the level
// objects that move as rigid bodies when something knocks them, the game's
// record of each, and what the game does when their bodies touch each other,
// a rider's bail bodies or the level. DM2's own code; Aggressive Inline's
// game/aggressive/level/dynamics_objects.cpp descends from it.

#include "ee/ee.h"
#include "zge/collision/dynamics.h"
#include "zge/math/types.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    namespace rider { struct Rider; }

    // The game's record of a dynamics object (0x70 bytes from the heap; the
    // object's component word and its body's record word hold it).
    struct DynamicsRecord
    {
        uint8_t _00[0x08];
        ee::Ptr<zge::SceneObject> sceneObject;  // +0x08
        ee::Ptr<zge::Dynamics> dynamics;        // +0x0C
        ee::Float riderDistanceSquared;         // +0x10 its body's from the current player's rider, at the last frame's check
        uint8_t _14[0x0C];
        zge::Vec4 startPosition;                // +0x20 where the level puts it
        zge::Quaternion startOrientation;       // +0x30
        zge::Vec4 bodyStartPosition;            // +0x40 its body's, when it was made
        zge::Quaternion bodyStartOrientation;   // +0x50
        // +0x60 bit 0 (kDynamicsAwake): it moves; from bit 2 up: the frames
        // in a row it has been nearly still.
        uint32_t flags;
        uint32_t _64;
        zge::MessageHandler frameHandler;       // +0x68 the frame handler subscribed to its scene object while it moves, 0: none
        uint32_t _6c;
    };
    static_assert(offsetof(DynamicsRecord, sceneObject) == 0x08 && offsetof(DynamicsRecord, startPosition) == 0x20);
    static_assert(offsetof(DynamicsRecord, bodyStartOrientation) == 0x50 && offsetof(DynamicsRecord, flags) == 0x60);
    static_assert(offsetof(DynamicsRecord, frameHandler) == 0x68 && sizeof(DynamicsRecord) == 0x70);

    constexpr uint32_t kDynamicsAwake = 0x1;
    // DynamicsRecord::flags' count of still frames: where it starts, and the
    // count at which the object goes to sleep.
    constexpr uint32_t kStillFramesShift = 2;
    constexpr uint32_t kStillFramesAsleep = 0x10;

    // A body's record word when the body is one of a rider's bail bodies:
    // the top four bits set, the rider's index in the low byte.
    constexpr uint32_t kRiderBodyTag = 0xF0000000u;

    // What the engine's touch message carries: where, how fast, which face
    // of which node; the receiver may clear `accept`.
    constexpr uint32_t kTouchMessage = 0x102;
    struct TouchData
    {
        zge::Vec4 point;    // +0x00
        zge::Vec4 velocity; // +0x10 units a step
        uint32_t face;      // +0x20
        uint32_t node;      // +0x24 the node's handle
        uint32_t accept;    // +0x28
        uint32_t _2c;
    };
    static_assert(offsetof(TouchData, face) == 0x20 && sizeof(TouchData) == 0x30);

    // The dynamics object's record, its component's first word (eight
    // words), and the same set.
    constexpr uint32_t kDynamicsTagWords = 8;
    ee::Ptr<DynamicsRecord> objectDynamicsRecord(ee::Ptr<zge::SceneObject> sceneObject);
    int32_t setObjectDynamicsRecord(ee::Ptr<zge::SceneObject> sceneObject, ee::Ptr<DynamicsRecord> record);

    // A level object's dynamics made from the node's definition (0 when it
    // has none): the game's record of it, where the object and its body
    // start, the body put in the level's sweep, the contact callback set.
    // Returns 1 when made.
    uint32_t createDynamicsObject(ee::Ptr<zge::SceneObject> sceneObject, ee::Ptr<zge::Node> node, uint32_t definitionName);
    // A dynamics object thrown from a rider: woken, its body given the
    // rider's velocity and 6 units a frame upwards, a spin, and its weight.
    void launchDynamicsObjectFromRider(ee::Ptr<rider::Rider> rider, ee::Ptr<zge::SceneObject> sceneObject);
    // An object's frame: further than 100 units from the current player's
    // rider, or with `reset`, the object and its body are put back where
    // they started, woken, and its frame handler goes. Awake and nearly
    // still, a frame is counted; sixteen in a row put it to sleep.
    void resetDynamicsObject(ee::Ptr<DynamicsRecord> record, uint32_t reset);
    // An object's frame message while it moves: its dynamics stepped a
    // frame (awake), its frame (resetDynamicsObject), its sound's turn.
    int32_t updateDynamicsObject(ee::Ptr<zge::EngineMessage> message);
    // Woken by a push: the frame message subscribed (updateDynamicsObject).
    void objectWoken(ee::Ptr<DynamicsRecord> record);
    // Asleep: the frame message it subscribed to dropped.
    void unsubscribeDynamicsContact(ee::Ptr<DynamicsRecord> record);
    // A box body woken and put in the level's sweep, its box the radius of
    // its half extents about its position.
    void addDynamicsBody(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::SceneObject> sceneObject);
    // A dynamics object's contact callback, asked about each face its body
    // touches. The faces of one part are summed (normals, centres) and
    // counted; when the part changes, the part before gets its due by its
    // owner's type: a "Car" knocks the body with its velocity along the
    // summed normal, a "waterSurface" slows the body and its spin to 0.8.
    // Then this face's owner: "glass" and "exploder" (and a "Car" a rider's
    // body touches) are sent the touch once a part, a "waterSurface" a
    // splash where the body meets the face. Returns 0 (the face ignored)
    // for water and for another "DynamicsObject", else 1. A rider's body
    // also tells the game's touch handler.
    uint32_t dynamicsObjectContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact);
}
