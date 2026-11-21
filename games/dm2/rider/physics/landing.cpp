// DM2's rider meeting a surface from the air (physics.c): the rows turned a
// little toward the surface's plane, the rider pushed out to a gap above it
// and the velocity into it taken away. lipTest does it against a quarter
// pipe's lip (an edge's upright plane), landingFaceTest against the face
// landed on.
//
// Counterparts: DM2 GameCube 0x80085F08
// (lipTest), 0x8008616C (landingFaceTest).

#include "physics.h"

#include "../collision/collision.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/physics/landing.cpp.h"

namespace game::dm2::rider::physics
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

        // to = row + (normal - row) x the share, xyz.
        void blendToward(ee::Float *to, const ee::Float *row, const ee::Float *normal)
        {
            const ee::Float share = *g_alignShare;
            const ee::Float y = row[1] + (normal[1] - row[1]) * share;
            const ee::Float z = row[2] + (normal[2] - row[2]) * share;
            to[0] = row[0] + (normal[0] - row[0]) * share;
            to[1] = y;
            to[2] = z;
        }

        // Out to `gap` from the plane (at `distance` now), and the velocity
        // along the normal taken away.
        void pushOut(Rider *rider, const ee::Float *normal, ee::Float distance, ee::Float gap)
        {
            if (distance < gap)
            {
                const ee::Float out = gap - distance;
                const ee::Float z = rider->position[2] + normal[2] * out;
                const ee::Float y = rider->position[1] + normal[1] * out;
                rider->position[0] = rider->position[0] + normal[0] * out;
                rider->position[2] = z;
                rider->position[1] = y;
            }
            const ee::Float into = zge::dot(*vec4(normal), *vec4(rider->velocity));
            const ee::Float z = rider->velocity[2] - normal[2] * into;
            const ee::Float y = rider->velocity[1] - normal[1] * into;
            rider->velocity[0] = rider->velocity[0] - normal[0] * into;
            rider->velocity[1] = y;
            rider->velocity[2] = z;
        }
        void negate(zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); }
    }

    // Against a lip (an edge's upright plane, the side the rider is on): the
    // up row a share toward the plane's normal (unless kFlagTrickTurn) when within
    // 50 degrees of it or `anyAngle`, then out to the lip's gap. GameCube
    // 0x80085F08.
    void lipTest(Rider *rider, EdgeRef edge, int32_t anyAngle)
    {
        if (edge == 0)
            return;
        ee::Local<ee::Float[4]> plane;
        ee::Local<ee::Float[4]> normal;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<ee::Float[4]> edgePlane;
        collision::edgePlane(edge, edgePlane);
        *vec4(plane) = *vec4(edgePlane);
        *vec4(normal) = *vec4(plane);
        ee::Float distance = rider::planeDistance(rider->position, plane);
        if (distance < 0.0f)
        {
            distance = -distance;
            negate(*vec4(normal));
        }
        const ee::Float facing = zge::dot(*vec4(rider->up), *vec4(normal));
        if (anyAngle == 0 && !(zge::angleAcos(facing) < *g_lipSteepest))
            return;
        if ((rider->flags & kFlagTrickTurn) == 0)
        {
            zge::Matrix &r = *rows;
            blendToward(&r.row(1).x, rider->up, normal);
            r.row(2) = *vec4(rider->forward);
            r.row(0) = zge::cross(r.row(1), r.row(2));
            r.row(0) = zge::unit(r.row(0));
            r.row(2) = zge::cross(r.row(0), r.row(1));
            setOrigin(&r.row(3).x);
            engine::copyMatrix(rider->right, rows);
        }
        pushOut(rider, normal, distance, *g_lipGap);
    }
    READABLE_FUNCTION(lipTest, Verified);

    // Against the face landed on (its plane through its first corner, the
    // side the rider's first row is on): that row a share toward the face's
    // normal, the rows made orthonormal again, then out to half a unit above
    // it. GameCube 0x8008616C.
    void landingFaceTest(Rider *rider, FaceRef face)
    {
        ee::Local<ee::Float[4]> plane;
        ee::Local<ee::Float[4]> normal;
        ee::Local<ee::Float[4]> corner;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        collision::faceNormal(face, &normal[0]);
        collision::faceVertex(face, 0u, corner);
        plane[0] = normal[0];
        plane[1] = normal[1];
        plane[2] = normal[2];
        plane[3] = -zge::dot(*vec4(normal), *vec4(corner));
        *vec4(normal) = *vec4(plane);
        const ee::Float distance = rider::planeDistance(rider->position, plane);
        zge::Matrix &r = *rows;
        r.row(0) = *vec4(normal);
        if (zge::dot(*vec4(rider->right), *vec4(normal)) < 0.0f)
            negate(r.row(0));
        blendToward(&r.row(0).x, rider->right, &r.row(0).x);
        r.row(1) = *vec4(rider->up);
        r.row(2) = zge::cross(r.row(0), r.row(1));
        r.row(2) = zge::unit(r.row(2));
        r.row(1) = zge::cross(r.row(2), r.row(0));
        setOrigin(&r.row(3).x);
        engine::copyMatrix(rider->right, rows);
        pushOut(rider, normal, distance, kLandingGap);
    }
    READABLE_FUNCTION(landingFaceTest, Verified);

    namespace
    {
        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kHitObjectFlag = 0x2u;

        // The world's up (0, -1, 0).
        inline ee::Global<ee::Float[4], sym::down> g_worldUp;
        // |v.xyz| (DM2's, through the game: the carried register below is
        // what it leaves).
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline ee::Float lengthByCall(const ee::Float *v) { return ee::game<ee::Float>(sym::vec3Length, v); }
        // Surface kinds: 0 ground; 1 .. 3 with a linked edge (1 and 3 a lip's,
        // inferred); 4 kept as it is.
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceLip = 1;
        constexpr uint32_t kSurfaceLipB = 3;
        constexpr uint32_t kSurfaceKind4 = 4;
        // Rider::flags 0x8000 of the three bits 0x38000: a lip's own landing
        // (inferred); landing on the ground clears 0x30000.
        constexpr uint32_t kFlagsLipLanding = 0x38000u;
        constexpr uint32_t kFlagLipLanding = 0x8000u;
        constexpr uint32_t kFlagsGroundClears = 0x30000u;
        // A landing on a lip (state 8): the face steeper than 75 degrees,
        // the right row within 45 degrees of its normal, the velocity more
        // than 30 degrees from upright.
        constexpr int32_t kLipFaceSteepest = 13654;
        constexpr int32_t kLipRightWithin = 8192;
        constexpr int32_t kLipVelocityFromUp = 5461;
        constexpr uint8_t kContactLipFace = 7;
    }

    // In the air, a ray's hit below: whether the rider lands, and the
    // contact it lands on. Off a lip (state 8) only a steep face (a wall
    // ridden up) under the right row: the rows aligned to it, the velocity
    // the forward row; else a face (the surface kind the contact's, a lip
    // edge's plane met for kinds 1 and 3) or an object. 1 landed, 0 not.
    // GameCube 0x80086400.
    int32_t landingTest(Rider *rider, const collision::RayHit *ray)
    {
        ee::Local<ee::Float[4]> direction;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<ee::Float[4]> normal;
        ee::Local<FaceRef> contact;
        ee::Local<uint32_t> object;
        ee::Local<FaceRef> face;
        *contact = 0;
        *object = 0;
        *face = collision::hitFace(ray);
        const uint32_t kind = collision::faceSurfaceKind(*face);

        if (physicsState(rider) != kPhysicsLipTrick)
        {
            if ((collision::hitFlags(ray) & kHitFaceFlag) && *face != 0)
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
                    if ((contactKind == kSurfaceLip || contactKind == kSurfaceLipB) &&
                        (rider->flags & kFlagsLipLanding) == kFlagLipLanding)
                        lipTest(rider, edge, 0);
                }
                else
                {
                    rider->flags &= ~kFlagsGroundClears;
                }
                setContact(rider, contact, contactKind);
                return 1;
            }
            if ((collision::hitFlags(ray) & kHitObjectFlag) == 0)
                return 0;
            *object = collision::hitObject(ray);
            setContact(rider, object, uint32_t(kContactObject));
            return 1;
        }

        collision::faceNormal(*face, &normal[0]);
        if ((collision::hitFlags(ray) & kHitFaceFlag) == 0 || *face == 0)
            return 0;
        if (collision::acceptAll() == 0)
            return 0;
        if (zge::angleAcos(ee::abs(normal[1])) < kLipFaceSteepest)
            return 0;
        if (!(zge::angleAcos(ee::abs(zge::dot(*vec4(rider->right), *vec4(normal)))) < kLipRightWithin))
            return 0;
        landingFaceTest(rider, *face);
        setContact(rider, face, uint32_t(kContactLipFace));
        *vec4(direction) = *vec4(rider->velocity);
        const ee::Float speed = lengthByCall(rider->velocity);
        // The scale carries what the last call left beside 1 / speed, as
        // airGravity's does.
        const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
        if (!(ee::Float(0.0f) < speed))
            return 0;
        *vec4(direction) = zge::scaledCarrying(*vec4(direction), ee::Float(1.0f) / speed, carried);
        if (zge::angleAcos(ee::abs(zge::dot(*vec4(direction), *vec4(*g_worldUp)))) < kLipVelocityFromUp)
            return 0;
        zge::Matrix &r = *rows;
        r.row(0) = *vec4(rider->right);
        r.row(2) = *vec4(direction);
        if (zge::dot(r.row(2), *vec4(rider->forward)) < 0.0f)
            negate(r.row(2));
        r.row(1) = zge::cross(r.row(2), r.row(0));
        setOrigin(&r.row(3).x);
        engine::copyMatrix(rider->right, rows);
        return 1;
    }
    READABLE_FUNCTION(landingTest, Verified);
}
