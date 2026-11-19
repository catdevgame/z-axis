// BMX XXX's rider contacts (physics.c): the ground below a point, the
// level's bounds, a point against a plane, the rider's and the wheels'
// contact records, and a surface sound's code.
// DM2's (game/dm2/rider/contact.cpp); BMX XXX's ground ray starts higher
// above the point and ends past the level's bottom.

#include "collision/collision.h"
#include "rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include "zge/collision/collision.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/contact.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The collision layer's hits (collision.h).
        constexpr uint32_t kHitFaceFlag = collision::kHitFace;
        constexpr uint32_t kHitObjectFlag = collision::kHitObject;

        // levelBounds' copy of the world's box.
        inline ee::Global<Bounds, sym::g_levelBounds> g_levelBounds;
        // groundBelow's ray: from this far above the point to this far past
        // the level's bottom (y grows downwards). DM2's starts 2 above and
        // stops 0.01 short.
        constexpr ee::Float kRayStartAbove = 4.0f;
        constexpr ee::Float kRayPastBottom = 1.0f;

        // A contact record's bytes by its kind: an object's 4, a face's or
        // edge's 8.
        uint32_t contactBytes(uint32_t kind) { return kind == kContactObject ? 4u : 8u; }
    }

    // A copy of the world's box (min, max; y grows downwards).
    Bounds *levelBounds()
    {
        const zge::Vec4 *world = zge::collisionWorldBounds(); // min, max
        // The original copies the box into its frame first and reads the
        // copy, as DM2's does (game/dm2/rider/contact.cpp: DM2 reads that
        // stack again), so the frame is written as the original writes it.
        ee::Local<Bounds> copy;
        *vec4(copy->min) = world[0];
        *vec4(copy->max) = world[1];
        Bounds &bounds = *g_levelBounds;
        for (int i = 0; i < 3; ++i)
            bounds.min[i] = copy->min[i];
        for (int i = 0; i < 3; ++i)
            bounds.max[i] = copy->max[i];
        return &bounds;
    }
    READABLE_FUNCTION(levelBounds, Verified);

    // The ground below a point: a ray from above it down past the level's
    // bottom. A face facing up (its normal's y below 0) is the ground: its
    // point and plane, and the face, or the object it belongs to (1); else
    // 0.
    int32_t groundBelow(const ee::Float *point, GroundHit *hit)
    {
        ee::Local<zge::Vec4> from;
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> plane;
        from = *vec4(point);
        to = *vec4(point);
        from->y = from->y - kRayStartAbove;
        const ee::Float bottom = levelBounds()->max[1];
        hit->flags = 0;
        hit->face = 0;
        hit->object = 0;
        to->y = bottom + kRayPastBottom;

        const collision::RayHit *found = collision::rayCast(&from->x, &to->x);
        if (!found)
            return 0;
        collision::hitPlane(found, &plane->x);
        const int64_t face = static_cast<int64_t>(collision::hitFace(found));
        if (!(plane->y < 0.0f))
            return 0;
        collision::hitPoint(found, hit->point);
        hit->plane[0] = plane->x;
        hit->plane[1] = plane->y;
        hit->plane[2] = plane->z;
        hit->plane[3] = plane->w;
        if ((collision::hitFlags(found) & kHitFaceFlag) && face != 0)
        {
            hit->face = face;
            return 1;
        }
        if (collision::hitFlags(found) & kHitObjectFlag)
        {
            hit->flags |= kGroundObject;
            hit->object = collision::hitObject(found);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(groundBelow, Verified);

    namespace
    {
        // groundContactBelow's ray: from one up row above the rider down 100.
        constexpr ee::Float kGroundRayLength = 100.0f;
        // The contact kinds by a face's surface kind: 0 ground, 1 .. 3 a face with a linked edge (else 0), 4 as
        // it is, 5 and above ground; an object's kContactObject; none -1.
        constexpr uint32_t kSurfaceKind4 = 4;
        constexpr int32_t kNoContactKind = -1;
    }

    // The contact the ground straight below the rider would give (a ray from one up row above his position, 100
    // down): its kind (as a ray's hit while riding: a face's by its surface kind, an object's, or -1 for none),
    // the face, its linked edge and the object; the height above it into groundHeight. 1 with a hit, else 0.
    int32_t groundContactBelow(Rider *rider, int32_t *kind, uint64_t *face, uint64_t *edge, uint32_t *object)
    {
        ee::Local<zge::Vec4> from;
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> point;
        *object = 0;
        *edge = 0;
        *face = 0;
        const ee::Float y = rider->position[1] + rider->up[1];
        const ee::Float x = rider->position[0] + rider->up[0];
        const ee::Float z = rider->position[2] + rider->up[2];
        *kind = kNoContactKind;
        from->y = y;
        to->x = x;
        to->y = y + kGroundRayLength;
        to->z = z;
        from->x = x;
        from->z = z;
        const collision::RayHit *hit = collision::rayCast(&from->x, &to->x);
        if (!hit)
            return 0;
        collision::hitPoint(hit, &point->x);
        const collision::FaceRef found = collision::hitFace(hit);
        rider->groundHeight = point->y - from->y;
        if ((collision::hitFlags(hit) & kHitFaceFlag) && found != 0)
        {
            *face = found;
            const uint32_t surface = collision::faceSurfaceKind(found);
            if (surface < kSurfaceKind4)
            {
                if (surface == 0)
                {
                    *kind = 0;
                }
                else
                {
                    *kind = int32_t(collision::faceSurfaceKind(found));
                    *edge = collision::faceLinkedEdge(found);
                    if (*edge == 0)
                        *kind = 0;
                }
            }
            else
            {
                *kind = surface == kSurfaceKind4 ? int32_t(kSurfaceKind4) : 0;
            }
        }
        else if (collision::hitFlags(hit) & kHitObjectFlag)
        {
            *kind = kContactObject;
            *object = collision::hitObject(hit);
        }
        return 1;
    }
    READABLE_FUNCTION(groundContactBelow, Verified);

    // A point's distance from a plane (normal xyz, distance w): n.p + d.
    ee::Float planeDistance(const ee::Float *point, const ee::Float *plane)
    {
        return point[0] * plane[0] + point[1] * plane[1] + point[2] * plane[2] + plane[3];
    }
    READABLE_FUNCTION(sym::planeDistance2, planeDistance, Verified);

    // The rider's contact: its kind and record (a face, or an object for
    // kContactObject; nothing past the kinds).
    void setContact(Rider *rider, const void *contact, uint32_t kind)
    {
        rider->contactKind = static_cast<uint8_t>(kind);
        kind &= 0xFFu;
        if (kind >= kContactKinds)
            return;
        game::tail(zge::memoryCopy, rider->contact, contact, contactBytes(kind));
    }
    READABLE_FUNCTION(setContact, Verified);

    // A wheel's contact: where it touches, its kind and record.
    void setWheelContact(Rider *rider, uint32_t wheel, const void *contact, uint32_t kind, const ee::Float *point)
    {
        WheelContact &record = rider->wheels[wheel];
        *vec4(record.point) = *vec4(point);
        record.kind = static_cast<uint8_t>(kind);
        kind &= 0xFFu;
        if (kind >= kContactKinds)
            return;
        game::tail(zge::memoryCopy, record.contact, contact, contactBytes(kind));
    }
    READABLE_FUNCTION(setWheelContact, Verified);

    // A rider sound's code for soundEvent: the kind and the surface (or
    // variant), with two flags (their meaning unread, as in DM2).
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface)
    {
        uint32_t code = kind << 8 | surface;
        if (flag20000)
            code |= 0x20000u;
        if (flag10000)
            code |= 0x10000u;
        return static_cast<int32_t>(code);
    }
    READABLE_FUNCTION(surfaceSoundFlags, Verified);
}
