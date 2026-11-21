// BMX XXX's rider meeting a surface from the air (physics.c): the rows
// turned a little toward the surface's plane, the rider pushed out to a
// gap above it and the velocity into it taken away. lipTest does it
// against a quarter pipe's lip (an edge's upright plane), landingFaceTest
// against the face landed on. DM2's (games/dm2/rider/physics/landing.cpp),
// read in BMX XXX's machine code.

#include "physics.h"

#include "../collision/collision.h"
#include "../rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/landing.cpp.h"

namespace game::bmx::rider
{
    using collision::EdgeRef;
    using collision::FaceRef;

    namespace
    {
        // A row's share turned toward the surface's normal a frame (0.085).
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_alignShare;
        // The lip: the gap kept from its plane (1.2), and the steepest the
        // up row may be from it unless asked (50 degrees).
        inline ee::Global<ee::Float, sym::g_lipGap> g_lipGap;
        inline ee::Global<int16_t, sym::g_lipSteepest> g_lipSteepest;
        // The gap kept above a landing face.
        constexpr ee::Float kLandingGap = 0.5f;

        // to = row + (normal - row) x the share, x, y and z.
        void blendToward(zge::Vec4 &to, const zge::Vec4 &row, const zge::Vec4 &normal)
        {
            const ee::Float share = *g_alignShare;
            const ee::Float x = row.x + (normal.x - row.x) * share;
            const ee::Float y = row.y + (normal.y - row.y) * share;
            const ee::Float z = row.z + (normal.z - row.z) * share;
            to.x = x;
            to.y = y;
            to.z = z;
        }

        // Out to `gap` from the plane (at `distance` now), and the velocity
        // along the normal taken away.
        void pushOut(Rider *rider, const zge::Vec4 &normal, ee::Float distance, ee::Float gap)
        {
            if (distance < gap)
            {
                const ee::Float out = gap - distance;
                rider->position[0] = rider->position[0] + normal.x * out;
                rider->position[2] = rider->position[2] + normal.z * out;
                rider->position[1] = rider->position[1] + normal.y * out;
            }
            const ee::Float into = zge::dot(normal, *vec4(rider->velocity));
            rider->velocity[0] = rider->velocity[0] - normal.x * into;
            rider->velocity[1] = rider->velocity[1] - normal.y * into;
            rider->velocity[2] = rider->velocity[2] - normal.z * into;
        }
        void negate(zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); }
    }

    // Against a lip (an edge's upright plane, the side the rider is on):
    // the up row a share toward the plane's normal (unless a turning
    // trick's turn is on) when within 50 degrees of it or `anyAngle`, then
    // out to the lip's gap. An edge with no plane does nothing. DM2's.
    void lipTest(Rider *rider, EdgeRef edge, int32_t anyAngle)
    {
        if (edge == 0)
            return;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<zge::Vec4> plane;
        zge::matrixIdentity(rows);
        if (collision::edgePlane(edge, &plane->x) == 0)
            return;
        *normal = *plane;
        ee::Float distance = planeDistance(rider->position, &plane->x);
        if (distance < ee::Float(0.0f))
        {
            distance = -distance;
            negate(*normal);
        }
        const ee::Float facing = zge::dot(*vec4(rider->up), *normal);
        if (anyAngle == 0 && !(zge::angleAcos(facing) < *g_lipSteepest))
            return;
        if (!hasFlag(rider, kFlagTrickTurn))
        {
            zge::Matrix &r = *rows;
            blendToward(r.row(1), *vec4(rider->up), *normal);
            r.row(2) = *vec4(rider->forward);
            r.row(0) = zge::cross(r.row(1), r.row(2));
            r.row(0) = zge::unit(r.row(0));
            r.row(2) = zge::cross(r.row(0), r.row(1));
            r.row(3) = zge::kZero;
            *matrix(rider->right) = r;
        }
        pushOut(rider, *normal, distance, *g_lipGap);
    }
    READABLE_FUNCTION(lipTest, Verified);

    // Against the face landed on (its plane through its first corner, the
    // side the rider's right row is on): that row a share toward the face's
    // normal, the rows made square again, then out to half a unit above
    // it. DM2's.
    void landingFaceTest(Rider *rider, FaceRef face)
    {
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> corner;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        collision::faceNormal(face, &normal->x);
        collision::faceVertex(face, 0u, &corner->x);
        *plane = *normal;
        plane->w = -zge::dot(*normal, *corner);
        *normal = *plane;
        const ee::Float distance = planeDistance(rider->position, &plane->x);
        zge::Matrix &r = *rows;
        r.row(0) = *normal;
        if (zge::dot(*vec4(rider->right), *normal) < ee::Float(0.0f))
            negate(r.row(0));
        blendToward(r.row(0), *vec4(rider->right), r.row(0));
        r.row(1) = *vec4(rider->up);
        r.row(2) = zge::cross(r.row(0), r.row(1));
        r.row(2) = zge::unit(r.row(2));
        r.row(1) = zge::cross(r.row(2), r.row(0));
        r.row(3) = zge::kZero;
        *matrix(rider->right) = r;
        pushOut(rider, *normal, distance, kLandingGap);
    }
    READABLE_FUNCTION(landingFaceTest, Verified);

    namespace
    {
        // Surface kinds (faceSurfaceKind; DM2's): 0 ground, 1 and 3 a lip's
        // face, 4 kept as it is. BMX XXX's kind 8 takes no lip trick's landing.
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceLip = 1;
        constexpr uint32_t kSurfaceLipB = 3;
        constexpr uint32_t kSurfaceKind4 = 4;
        constexpr uint32_t kSurfaceKind8 = 8;
        // A lip trick lands on a face no flatter than 75 degrees from
        // level, with the right row within 45 degrees of its normal, moving
        // no nearer than 30 degrees to straight up or down.
        constexpr int32_t kLipFaceSteepest = 0x3556;
        constexpr int32_t kLipRightWithin = 0x2000;
        constexpr int32_t kLipVelocityFromUp = 0x1555;
        constexpr uint8_t kContactLipFace = 8;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
    }

    // A landing's test of what the ray hit (the air's moves). Out of a lip
    // trick: a face becomes the contact by its surface (ground and the
    // kinds above 4 clear the two landing flags; kinds 1 .. 3 are the face
    // with a linked edge, and a lip's face tests the lip when the rider is
    // set to land on one; 4 kept), an object its contact; 1 when there is
    // one. In a lip trick: 1 when the face takes the landing (steep, the
    // right row toward it, not moving straight up or down), with the rows
    // turned to the velocity. DM2's, with BMX XXX's surface kind 8 refused.
    int32_t landingTest(Rider *rider, const collision::RayHit *ray)
    {
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<zge::Vec4> normal;
        ee::Local<FaceRef> contact;
        ee::Local<uint32_t> object;
        ee::Local<FaceRef> face;
        *contact = 0;
        *object = 0;
        *face = collision::hitFace(ray);
        const uint32_t kind = collision::faceSurfaceKind(*face);

        if (physicsState(rider) != kPhysicsLipTrick)
        {
            if ((collision::hitFlags(ray) & collision::kHitFace) != 0 && *face != 0)
            {
                uint32_t contactKind = 0;
                *contact = *face;
                if (kind == kSurfaceKind4)
                {
                    contactKind = kSurfaceKind4;
                }
                else if (kind < kSurfaceKind4 && kind != kSurfaceGround)
                {
                    const EdgeRef edge = collision::faceLinkedEdge(*face);
                    contactKind = edge == 0 ? 0 : kind;
                    if ((contactKind == kSurfaceLip || contactKind == kSurfaceLipB) && hasFlag(rider, kFlagLipLanding) &&
                        (rider->flags[3] & kHopFlags) == 0)
                        lipTest(rider, edge, 0);
                }
                else
                {
                    rider->flags[3] &= static_cast<uint8_t>(~kHopFlags);
                }
                setContact(rider, &*contact, contactKind);
                return 1;
            }
            if ((collision::hitFlags(ray) & collision::kHitObject) == 0)
                return 0;
            *object = collision::hitObject(ray);
            setContact(rider, &*object, uint32_t(kContactObject));
            return 1;
        }

        collision::faceNormal(*face, &normal->x);
        if ((collision::hitFlags(ray) & collision::kHitFace) == 0 || *face == 0)
            return 0;
        if (collision::faceSurfaceKind(*face) == kSurfaceKind8)
            return 0;
        if (collision::acceptAll() == 0)
            return 0;
        if (zge::angleAcos(ee::abs(normal->y)) < kLipFaceSteepest)
            return 0;
        if (!(zge::angleAcos(ee::abs(zge::dot(*vec4(rider->right), *normal))) < kLipRightWithin))
            return 0;
        landingFaceTest(rider, *face);
        setContact(rider, &*face, uint32_t(kContactLipFace));
        *direction = *vec4(rider->velocity);
        const ee::Float speed = zge::vec3Length(&vec4(rider->velocity)->xyz());
        if (!(ee::Float(0.0f) < speed))
            return 0;
        // The scale carries the velocity's upper half beside 1 / speed.
        *direction = zge::scaledCarrying(*direction, ee::Float(1.0f) / speed, physics::upperHalf(*vec4(rider->velocity)));
        if (zge::angleAcos(ee::abs(zge::dot(*direction, *g_worldUp))) < kLipVelocityFromUp)
            return 0;
        zge::Matrix &r = *rows;
        r.row(0) = *vec4(rider->right);
        r.row(2) = *direction;
        if (zge::dot(r.row(2), *vec4(rider->forward)) < ee::Float(0.0f))
            negate(r.row(2));
        r.row(1) = zge::cross(r.row(2), r.row(0));
        r.row(3) = zge::kZero;
        *matrix(rider->right) = r;
        return 1;
    }
    READABLE_FUNCTION(landingTest, Verified);
}
