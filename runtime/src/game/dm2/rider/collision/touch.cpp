// DM2's touch: the rider tells the object it touches, by a ray
// hit or by its contact, with the engine's message 0x102 (the touch's
// point, the rider's velocity, the face), and the game side's touch
// handler (triggers, goals). AI's counterparts: its object messages
// (objects.h; skaterTouchSurface, rideObject).

#include "collision.h"

#include "../rider.h"
#include "../../game/goals.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstring>
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/collision/touch.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        constexpr uint32_t kMessageTouch = 0x102u;
        constexpr uint8_t kContactEdge = 5;               // a contact kind holding an edge (grind)

        // Message 0x102's data: where, how fast, which face; the receiver
        // may clear `accept`.
        struct TouchMessage
        {
            ee::Float point[4];
            ee::Float velocity[4];
            int32_t face;
            uint32_t node;
            int32_t accept;
            uint8_t unknown3C[0x04];
        };
        static_assert(sizeof(TouchMessage) == 0x30);

    }

    uint32_t hitFlags(const RayHit *hit);
    uint32_t hitObject(const RayHit *hit);
    FaceRef hitFace(const RayHit *hit);
    void hitPoint(const RayHit *hit, ee::Float *point);
    void hitPlane(const RayHit *hit, ee::Float *plane);
    void faceCentre(FaceRef ref, ee::Float *centre);
    uint64_t nodeHandle(uint64_t node);
    uint64_t nodeOf(uint64_t node);

    // The rider touches an object: the one a ray hit (or the object
    // owning the hit face), or with no hit the one its contact names.
    // A newly touched object's part, the touch's point in it and the part's
    // rotation are kept; the object gets message 0x102 and the game side's
    // touch handler runs (unless the rider's kFlagNoPlayer). Returns the message's
    // accept (1 when nothing was touched).
    int32_t touchObject(Rider *rider, const RayHit *hit)
    {
        ee::Local<zge::Vec4> origin; // the hit's plane first (unused)
        ee::Local<TouchMessage> message;
        ee::Local<zge::Matrix> partMatrix;
        uint32_t object = 0;
        FaceRef face = 0;
        if (hit)
        {
            if (hitFlags(hit) & kHitObject)
                object = hitObject(hit);
            else
            {
                hitPlane(hit, &origin->x);
                face = hitFace(hit);
            }
        }
        else if (rider->contactKind < kContactKinds)
        {
            uint64_t contact;
            std::memcpy(&contact, rider->contact, sizeof(contact));
            if (rider->contactKind == kContactEdge)
                face = makeFace(static_cast<uint32_t>(contact), edgeFace(contact)); // the edge's face
            else if (rider->contactKind == kContactObject)
                object = static_cast<uint32_t>(contact);
            else
                face = contact;
        }
        if (!object)
            object = engine::nodeObject(nodeOf(widen(refNode(face))), 0u);
        if (!object)
        {
            rider->touchedObject = 0;
            return 1;
        }

        if (hit)
        {
            message->face = faceIndex(hitFace(hit));
            message->node = static_cast<uint32_t>(refNode(hitFace(hit)));
            hitPoint(hit, message->point);
        }
        else if (face)
        {
            message->face = faceIndex(face);
            message->node = static_cast<uint32_t>(refNode(face));
            faceCentre(face, message->point);
        }
        else
        {
            // The object's own node, its face 0.
            message->face = 0;
            const uint32_t node = static_cast<uint32_t>(nodeHandle(ee::Address(zge::objectLevelNode(ee::Ptr<zge::SceneObject>(object)))));
            message->node = node;
            faceCentre(static_cast<FaceRef>(node), message->point);
        }

        if (rider->touchedObject != object)
        {
            origin = zge::kZero; // the part's origin
            engine::nodeObject(nodeOf(widen(message->node)), &rider->touchedPart);
            const ee::Ptr<zge::SceneObject> touched(object);
            const ee::Ptr<zge::SceneBone> part(rider->touchedPart);
            zge::objectBoneMatrix(touched, part, partMatrix, 0u);
            zge::matrixToQuaternion(quaternion(rider->touchRotation), partMatrix);
            zge::objectBonePoint(touched, part, origin, vec4(rider->touchPoint), 0u);
        }
        *vec4(message->velocity) = *vec4(rider->velocity);
        message->accept = 1;
        zge::sendNewMessage(rider->messenger, object, kMessageTouch, message, 1);
        if (!(rider->flags & kFlagNoPlayer))
            gameTouchHandler(static_cast<uint32_t>(rider->player), ee::Ptr<zge::SceneObject>(object));
        const int32_t accept = message->accept;
        rider->touchedObject = object;
        return accept;
    }
    READABLE_FUNCTION(touchObject, Verified);
}
