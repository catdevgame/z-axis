// DM2's rider contacts (physics.c): the ground below a point, the level's
// bounds, a point against a plane, the rider's and the wheels' contact
// records, and a surface sound's code.
//
// Counterparts: DM2 GameCube, BMX XXX PS2.

#include "collision/collision.h"
#include "rider.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstring>
#include "zge/collision/collision.h"
#include "zge/text/cstring.h"
#include "sym/dm2/rider/contact.cpp.h"

namespace game::dm2::rider
{
    namespace
    {
        // DM2's collision results (game.c's collision API, round 2's
        // `adapt` functions).
        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kHitObjectFlag = 0x2u;

        // levelBounds' copy of the world's box.
        inline ee::Global<Bounds, sym::g_levelBounds> g_levelBounds;
        // How far above the level's bottom groundBelow's ray stops (0.01).
        inline ee::Global<ee::Float, sym::g_rayAboveBottom> g_rayAboveBottom;
        // How far above the point groundBelow's ray starts.
        constexpr ee::Float kRayStartAbove = 2.0f;

        // A contact record by its kind: an object's 4 bytes, a face's or
        // edge's 8; nothing past the kinds. (The original inlines it.)
        void copyContact(uint8_t *record, const void *contact, uint32_t kind)
        {
            kind &= 0xFFu;
            if (kind >= kContactKinds)
                return;
            zge::memoryCopy(record, contact, kind == kContactObject ? 4u : 8u);
        }
    }

    // A copy of the world's box (min, max; y grows downwards).
    Bounds *levelBounds()
    {
        const zge::Vec4 *world = zge::collisionWorldBounds(); // min, max
        // The original copies the box into its frame first (four LD / SD)
        // and reads the copy. The copy is dead after the return, but DM2
        // reads it again: later code takes a vector's w from an uninitialised
        // stack slot there (with the frame left unwritten, 279 bytes of
        // dm2logic's RAM differed at the level's start: vectors' w 1.0
        // became 0), so the frame is written as the original writes it.
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

    // The ground below a point: a ray from just above it down to the level's
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
        to->y = bottom - *g_rayAboveBottom;

        const collision::RayHit *found = collision::rayCast(&from->x, &to->x);
        if (!found)
            return 0;
        collision::hitPlane(found, &plane->x);
        const int64_t face = collision::hitFace(found);
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

    // A point's distance from a plane (normal xyz, distance w): n.p + d.
    ee::Float planeDistance(const ee::Float *point, const ee::Float *plane)
    {
        return point[0] * plane[0] + point[1] * plane[1] + point[2] * plane[2] + plane[3];
    }
    READABLE_FUNCTION(sym::planeDistance2, planeDistance, Verified);

    // The rider's contact: its kind and record (a face, or an object for
    // kContactObject).
    void setContact(Rider *rider, const void *contact, uint32_t kind)
    {
        rider->contactKind = static_cast<uint8_t>(kind);
        copyContact(rider->contact, contact, kind);
    }
    READABLE_FUNCTION(setContact, Verified);

    // A wheel's contact: where it touches, its kind and record.
    void setWheelContact(Rider *rider, uint32_t wheel, const void *contact, uint32_t kind, const ee::Float *point)
    {
        WheelContact &record = rider->wheels[wheel];
        *vec4(record.point) = *vec4(point);
        record.kind = static_cast<uint8_t>(kind);
        copyContact(record.contact, contact, kind);
    }
    READABLE_FUNCTION(setWheelContact, Verified);

    // A rider sound's code for soundEvent: the kind and the surface (or
    // variant), with two flags (their meaning unread).
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
