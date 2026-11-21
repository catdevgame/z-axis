// BMX XXX's rider touching an object (physics.c): the touch message. DM2's
// (games/dm2/rider/collision/touch.cpp), read in BMX XXX's machine code.

#include "collision.h"

#include "../engine_calls.h"
#include "../game_calls.h"
#include "../rider.h"

#include "bmx/game/pickups.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstring>
#include "sym/bmx/rider/collision/touch.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        // The rider's touched part, as the bone slot nodeSceneObject fills.
        ee::Ptr<ee::Ptr<zge::SceneBone>> touchedPartSlot(Rider *rider) { return ee::Ptr<uint32_t>(&rider->touchedPart).cast<ee::Ptr<zge::SceneBone>>(); }
        // The contact kinds that hold no face (DM2's edge and object, and
        // BMX XXX's kind 6, which holds nothing to touch).
        constexpr uint8_t kContactEdge = 5;
        constexpr uint8_t kContactKind6 = 6;
        // The caller's word that has the touched part's place read when the
        // game did not take the touch.
        constexpr uint32_t kTouchReadsPart = 3;
    }

    // The object under a hit, or with no hit under the rider's contact,
    // touched: found (the hit's object, or the object of the face's node;
    // an edge contact's face, if its node has faces), then told with a
    // touch message carrying where, how fast and the caller's word. A
    // rider with a camera shows the message to the game first
    // (riderActivatedObject); when the game leaves it and the caller's
    // word is 3, a newly touched object's part has its place and rotation
    // kept for riding on it. Returns the message's `accept` (1 unless the
    // receiver cleared it), or 1 with no object. DM2's, which reads the
    // part's place on every new touch and tells the game after sending. A
    // rider without a camera sends the message's header as the frame left
    // it: nothing here fills it.
    int32_t touchObject(Rider *rider, const RayHit *hit, uint32_t how)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<TouchData> data;
        ee::Local<zge::Matrix> partMatrix;
        ee::Local<zge::Vec4> origin;
        uint32_t object = 0;
        FaceRef face = 0;
        if (hit)
        {
            if (hitFlags(hit) & kHitObject)
                object = hitObject(hit);
            else
                face = hitFace(hit);
        }
        else if (rider->contactKind < kContactKinds)
        {
            uint64_t contact;
            std::memcpy(&contact, rider->contact, sizeof(contact));
            if (rider->contactKind == kContactEdge)
            {
                // The edge's face, when its node has faces.
                const uint64_t node = widen(static_cast<uint32_t>(contact));
                const int32_t edgeFaceIndex = edgeFace(contact);
                face = nodeFaceCount(node) != 0 ? makeFace(static_cast<uint32_t>(node), edgeFaceIndex) : 0;
            }
            else if (rider->contactKind == kContactObject)
                object = static_cast<uint32_t>(contact);
            else if (rider->contactKind != kContactKind6)
                face = contact;
        }
        if (!object)
            object = ee::Address(zge::nodeSceneObject(ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(widen(static_cast<uint32_t>(refNode(face)))))), {}));
        data->how = how;
        data->accept = 1;
        if (!object)
            return 1;

        if (hit)
        {
            data->face = faceIndex(hitFace(hit));
            data->node = static_cast<uint32_t>(refNode(hitFace(hit)));
            hitPoint(hit, data->point);
        }
        else if (face)
        {
            data->face = faceIndex(face);
            data->node = static_cast<uint32_t>(refNode(face));
            faceCentre(face, data->point);
        }
        else
        {
            // The object's own node: its face 0 if it has faces.
            data->face = 0;
            const uint64_t node = nodeHandle(widen(ee::Address(zge::objectLevelNode(ee::Ptr<zge::SceneObject>(object)))));
            data->node = static_cast<uint32_t>(node);
            faceCentre(nodeFaceCount(node) != 0 ? static_cast<FaceRef>(static_cast<uint32_t>(node)) : 0, data->point);
        }
        *vec4(data->velocity) = *vec4(rider->velocity);
        data->accept = 1;

        if (!hasFlag(rider, kFlagNoPlayer))
        {
            message->type = kMessageTouch;
            message->sender = ee::Address(rider->messenger);
            message->data = data;
            message->target = object;
            message->flags = zge::kMessageNow;
            if (riderActivatedObject(message) != 0)
            {
                data->accept = 1;
            }
            else if (how == kTouchReadsPart && rider->touchedObject != object)
            {
                const ee::Ptr<ee::Ptr<zge::SceneBone>> partOut = touchedPartSlot(rider);
                zge::nodeSceneObject(ee::Ptr<zge::Node>(static_cast<uint32_t>(nodeOf(widen(data->node)))), partOut);
                const ee::Ptr<zge::SceneObject> touched(object);
                const ee::Ptr<zge::SceneBone> part(rider->touchedPart);
                zge::objectBoneMatrix(touched, part, partMatrix, 0u);
                zge::matrixToQuaternion(quaternion(rider->touchRotation), &*partMatrix);
                *origin = zge::kZero; // the part's origin
                zge::objectBonePoint(touched, part, ee::Ptr<const zge::Vec4>(&*origin), ee::Ptr<zge::Vec4>(vec4(rider->touchPoint)), 0u);
                rider->touchedObject = object;
            }
        }
        if (data->accept == 0)
            return 0;
        zge::sendEngineMessage(message);
        return data->accept;
    }
    READABLE_FUNCTION(touchObject, Verified);

    namespace
    {
        // Past a hit the ray is not stopped by, it goes on from this far beyond it.
        inline ee::Global<ee::Float, sym::g_rayPastHit> g_rayPastHit; // 0.001
    }

    // rayCastTouching (rider, from, to, how): the ray from `from` to `to`, going on past each hit whose object does
    // not take the rider's touch (`how`, as touchObject): the first hit taken, or none.
    RayHit *rayCastTouching(Rider *rider, const ee::Float *from, const ee::Float *to, uint32_t how)
    {
        ee::Local<zge::Vec4> along;
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> end;
        ee::Local<zge::Vec4> beyond;
        *along = *vec4(to) - *vec4(from);
        ee::Float left = zge::vec3Length(&along->xyz());
        *along = *along * (ee::Float(1.0f) / left);
        *start = *vec4(from);
        while (ee::Float(0.0f) < left)
        {
            *end = *along * left;
            *end = *end + *start;
            RayHit *const hit = rayCast(&start->x, &end->x);
            if (hit == nullptr)
                break;
            if (touchObject(rider, hit, how) != 0)
                return hit;
            hitPoint(hit, &end->x);
            *beyond = *end - *start;
            const ee::Float past = zge::dot(*along, *beyond) + *g_rayPastHit;
            *end = *along * past;
            *start = *end + *start;
            left = left - past;
        }
        return nullptr;
    }
    READABLE_FUNCTION(rayCastTouching, Verified);
}
