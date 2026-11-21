// A rigid body's own step (zge/collision/dynamics.h): its velocity and spin
// damped, the forces applied and limited, its orientation turned, and its
// place given back to the bone or the object it drives.

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics_body.cpp.h"


namespace zge
{

    namespace
    {
        // What is taken off the speeds each step, and the limits on them.
        constexpr float kVelocityDrag = -0.001f, kSpinDrag = -0.005f;
        constexpr float kMostSpeedSquared = 1e+04f, kMostSpeed = 100.0f;
        constexpr float kMostSpinSquared = 1e+02f, kMostSpin = 10.0f;

        constexpr uint32_t kObjectFollowsBody = 1u << 0; // Dynamics::flags
        constexpr uint32_t kChangedPosition = 0x3;       // setAnimatorChanges

        // contactImpulse's numbers.
        constexpr float kImpulseSeparating = 0.2f;
        constexpr float kImpulseSoftness = 1e-03f;
        constexpr float kImpulseRestitution = -1.8f; // -(1 + 0.8)
    }

    // updateWorldInertia: the inertia and its inverse in the world, from the body's
    // own by its rotation: R I R^-1.
    void updateWorldInertia(ee::Ptr<RigidBody> body)
    {
        ee::Local<Matrix> product;
        ee::Local<Matrix> rotation;
        ee::Local<Matrix> inverse;
        if constexpr (game::readable::kIsBmx) // ZGE4 starts its two work matrices as the identity
        {
            matrixIdentity(inverse);
            matrixIdentity(product);
        }
        copyBodyMatrix(rotation, &body->rotation);
        matrixClearTranslation(rotation);
        matrixTranspose3(inverse, rotation);
        matrixMultiply(product, &body->inverseInertia, inverse);
        matrixMultiply(&body->worldInverseInertia, rotation, product);
        matrixMultiply(product, &body->inertia, inverse);
        matrixMultiply(&body->worldInertia, rotation, product);
    }
    READABLE_FUNCTION(updateWorldInertia, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
    }

    // bodyFromBone: where a body's bone is: its point, rotation and orientation.
    void bodyFromBone(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body, Vec4 *positionOut, Matrix *rotationOut, Quaternion *orientationOut)
    {
        ee::Local<Vec4> origin;
        memset(origin, 0, sizeof(Vec4));
        objectBonePoint(dynamics->sceneObject, body->bone, origin, positionOut, 0);
        objectBoneMatrix(dynamics->sceneObject, body->bone, rotationOut, 0);
        // ZGE4 takes the quaternion from the matrix itself; ZGE3 goes through Euler angles.
        if constexpr (game::readable::kIsBmx)
            matrixToQuaternion(orientationOut, rotationOut);
        else
        {
            ee::Local<EulerAngles> angles;
            matrixToEuler(angles, rotationOut);
            quaternionFromEuler(orientationOut, angles);
        }
    }
    READABLE_FUNCTION(bodyFromBone, Verified);

    // applyBodyToBone: a body's orientation on its bone, as a turn from its
    // parent body's; the bones between them straightened.
    void applyBodyToBone(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body)
    {
        ee::Local<Quaternion> turn;
        ee::Local<Quaternion> inverse;
        const ee::Ptr<RigidBody> parent = body->parentBody;
        if (!parent)
        {
            if (body->boneChannel != 0)
                setBoneRotation(body->boneChannel, &body->orientation);
            return;
        }
        quaternionInverse(inverse, &parent->orientation);
        quaternionMultiply(turn, inverse, &body->orientation);
        setBoneRotation(body->boneChannel, turn);
        ee::Ptr<SceneBone> bone = boneParent(dynamics->sceneObject->scene, body->bone);
        if (body->parentBody->bone == bone)
            return;
        quaternionIdentity(turn);
        while (body->parentBody->bone != bone)
        {
            const ee::Ptr<SceneObject> object = dynamics->sceneObject;
            const uint32_t id = boneId(object->scene, bone);
            const ee::Ptr<AnimationChannel> channel = findBoneChannel(object->animator, id);
            setBoneRotation(channel, turn);
            bone = boneParent(dynamics->sceneObject->scene, bone);
        }
    }
    READABLE_FUNCTION(applyBodyToBone, Verified);

    // driveBodyFromBone: the body set to be pulled toward its bone's pose
    // (springBodyToBoneTurn): turned with its parent each step (followsParentTurn), its boneSpring
    // and boneDamping; with a parent body, boneTurn becomes the bone's
    // rotation turned by each bone's between it and the parent body's bone,
    // and turnShare is set. A body with no channel gets turnShare alone.
    // (Nothing in AI calls it; DM2's bail does, its own copy.)
    void driveBodyFromBone(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body, ee::Float spring, ee::Float damping, ee::Float turnShare)
    {
        body->followsParentTurn = 1;
        const ee::Ptr<RigidBody> parent = body->parentBody;
        body->boneSpring = spring;
        body->boneDamping = damping;
        if (!parent)
            return;
        if (body->boneChannel != 0)
        {
            quaternionCopy(body->boneTurnQuaternion(), boneRotation(body->boneChannel));
            for (ee::Ptr<SceneBone> bone = boneParent(dynamics->sceneObject->scene, body->bone); bone != body->parentBody->bone;
                 bone = boneParent(dynamics->sceneObject->scene, bone))
            {
                const ee::Ptr<SceneObject> object = dynamics->sceneObject;
                const uint32_t id = boneId(object->scene, bone);
                const ee::Ptr<AnimationChannel> channel = findBoneChannel(object->animator, id);
                quaternionMultiply(body->boneTurnQuaternion(), boneRotation(channel), body->boneTurnQuaternion());
            }
        }
        body->turnShare = turnShare;
    }
    READABLE_FUNCTION(driveBodyFromBone, Draft);

    // applyBodyOrientation: a body's orientation on its object; through its bone, the
    // object then left unturned, if it has one.
    void applyBodyOrientation(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body)
    {
        ee::Local<Quaternion> identity;
        quaternionIdentity(identity);
        if (body->boneChannel != 0)
        {
            setBoneRotation(body->boneChannel, &body->orientation);
            setObjectOrientation(dynamics->sceneObject, identity);
        }
        else
            setObjectOrientation(dynamics->sceneObject, &body->orientation);
    }
    READABLE_FUNCTION_WAITS(applyBodyOrientation, Verified);

    // applyBodiesToObjects: the step's results on the object: each body with a bone
    // keeps its state; one that drives its bone moves the object (with
    // kObjectFollowsBody) and turns it, the others set their bones. The pose
    // is out of date afterwards.
    void applyBodiesToObjects(ee::Ptr<Dynamics> dynamics)
    {
        ee::Local<Vec4> offset;
        ee::Local<Vec4> zero;
        // (ZGE2 neither reads the animator nor marks the pose stale after the bodies.)
        ee::Ptr<Animator> animator;
        if constexpr (!game::readable::kIsDm2)
            animator = dynamics->sceneObject->animator;
        memset(offset, 0, sizeof(Vec4));
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            if (body->bone == 0)
                continue;
            if constexpr (!game::readable::kIsBmx) // (ZGE4's bodies keep no last state)
            {
                body->lastPosition = body->position;
                body->lastSpin = body->spin;
                body->lastVelocity = body->velocity;
            }
            if (body->boneDriven == 0)
            {
                applyBodyToBone(dynamics, body);
                continue;
            }
            if ((dynamics->flags & kObjectFollowsBody) != 0)
            {
                memset(zero, 0, sizeof(Vec4));
                objectPosition(dynamics->sceneObject); // (the original asks, and drops the answer)
                offset = rotated(body->rotation, body->boxSize);
                offset = body->position + offset;
                setObjectPosition(dynamics->sceneObject, offset);
                if (body->boneChannel != 0)
                    setBoneTranslation(body->boneChannel, zero);
            }
            applyBodyOrientation(dynamics, body);
        }
        if constexpr (!game::readable::kIsDm2)
        {
            markPoseStale(dynamics->sceneObject);
            if (animator)
                setAnimatorChanges(dynamics->sceneObject, animator->changes | kChangedPosition);
        }
    }
    READABLE_FUNCTION_WAITS(applyBodiesToObjects, Verified);

    // integrateBody: a step of a body: drag, the solved force and torque (each
    // speed limited), its position by half the change, its orientation by the
    // spin, normalised, and all that follows from it.
    void integrateBody(ee::Ptr<RigidBody> body)
    {
        // The working vectors live on the game's stack, as the original's
        // do: what they leave there (w lanes included) is what the next
        // functions at this depth find (resolveBodyContacts, pairTouched).
        ee::Local<Vec4> change;       // the velocity's change, then half of it plus the old velocity
        ee::Local<Vec4> lastVelocity;
        ee::Local<Vec4> spinChange;
        ee::Local<Vec4> moved;
        ee::Local<Vec4> drag;
        ee::Local<Vec4> spinDrag;
        ee::Local<Quaternion> turn;
        ee::Local<Quaternion> halfSpin;
        drag = body->velocity * kVelocityDrag;
        body->velocity = body->velocity + *drag;
        spinDrag = body->spin * kSpinDrag;
        body->spin = body->spin + *spinDrag;
        change = body->solvedForce * (*g_dynamicsStepSeconds / body->mass);
        // ZGE4 scales the velocity by the step here and adds half the change scaled below; ZGE3 adds the half
        // change to the velocity and scales the sum.
        if constexpr (game::readable::kIsBmx)
            lastVelocity = body->velocity * *g_dynamicsStepSeconds;
        else
            lastVelocity = body->velocity;
        body->velocity = body->velocity + *change;
        if (ee::Float(kMostSpeedSquared) < dot(body->velocity, body->velocity))
        {
            body->velocity = unit(body->velocity);
            body->velocity = body->velocity * kMostSpeed;
        }
        spinChange = rotated(body->worldInverseInertia, body->solvedTorque);
        spinChange = *spinChange * *g_dynamicsStepSeconds;
        body->spin = body->spin + *spinChange;
        if (ee::Float(kMostSpinSquared) < dot(body->spin, body->spin))
        {
            body->spin = unit(body->spin);
            body->spin = body->spin * kMostSpin;
        }
        if constexpr (game::readable::kIsBmx)
        {
            change = *change * (*g_dynamicsStepSeconds * ee::Float(0.5f));
            moved = *lastVelocity + *change;
        }
        else
        {
            change = *change * ee::Float(0.5f);
            change = *change + *lastVelocity;
            moved = *change * *g_dynamicsStepSeconds;
        }
        body->position = body->position + *moved;

        // dq/dt = w q / 2.
        halfSpin->x = body->spin.x * ee::Float(0.5f);
        halfSpin->y = body->spin.y * ee::Float(0.5f);
        halfSpin->z = body->spin.z * ee::Float(0.5f);
        halfSpin->w = ee::Float(0.0f);
        quaternionMultiply(turn, halfSpin, &body->orientation);
        quaternionScale(turn, turn, *g_dynamicsStepSeconds);
        quaternionAdd(&body->orientation, &body->orientation, turn);
        quaternionNormalize(&body->orientation);
        quaternionToMatrix(&body->rotation, &body->orientation);
        updateWorldInertia(body);
    }
    READABLE_FUNCTION(integrateBody, Verified);

    // pointVelocity: how fast a point of the body moves: v + w x r.
    //
    // Both working vectors stay on the game's stack, where the next body's
    // updateWorldInertia finds them: its inverse sits over them and a
    // transpose writes no w, so the second row's w is `turning`'s (0).
    // matrixMultiply leaves it in VU0, the next transform stores it (a lip
    // edge's ends, from there the skater's position and the camera's w).
    void pointVelocity(ee::Ptr<RigidBody> body, const Vec4 *point, Vec4 *out)
    {
        ee::Local<Vec4> turning;
        ee::Local<Vec4> arm;
        arm = *point - body->position;
        turning = cross(body->spin, arm);
        *out = *turning + body->velocity;
    }
    READABLE_FUNCTION_VU0(pointVelocity, Verified);

    // projectBoxOnAxis: a box along an axis: its centre, and how far it reaches.
    void projectBoxOnAxis(ee::Ptr<RigidBody> body, const Vec4 *axis, ee::Float *centreOut, ee::Float *radiusOut)
    {
        const Vec4 &half = body->halfExtents->half;
        *centreOut = dot(*axis, body->position);
        const ee::Float alongX = ee::abs(half.x * dot(matrixRow(body->rotation, 0), *axis));
        const ee::Float alongY = ee::abs(half.y * dot(matrixRow(body->rotation, 1), *axis));
        const ee::Float alongZ = ee::abs(half.z * dot(matrixRow(body->rotation, 2), *axis));
        *radiusOut = alongX + alongY + alongZ;
    }
    READABLE_FUNCTION_VU0(projectBoxOnAxis, Verified);

    // projectBoxAxesOnAxis: projectBoxOnAxis with the box's axes handed in
    // (xAxis, yAxis, zAxis) in place of its rotation's rows. (Nothing in the
    // game calls it.)
    void projectBoxAxesOnAxis(ee::Ptr<RigidBody> body, ee::Ptr<const Vec4> xAxis, ee::Ptr<const Vec4> yAxis, ee::Ptr<const Vec4> zAxis,
                              ee::Ptr<const Vec4> axis, ee::Ptr<ee::Float> centreOut, ee::Ptr<ee::Float> radiusOut)
    {
        const Vec4 &half = body->halfExtents->half;
        *centreOut = dot(*axis, body->position);
        const ee::Float alongX = ee::abs(half.x * dot(*xAxis, *axis));
        const ee::Float alongY = ee::abs(half.y * dot(*yAxis, *axis));
        const ee::Float alongZ = ee::abs(half.z * dot(*zAxis, *axis));
        *radiusOut = alongX + alongY + alongZ;
    }
    READABLE_FUNCTION(projectBoxAxesOnAxis, Draft);

    // contactImpulse: a body meeting something moving at `otherVelocity`: if
    // they are not parting, an impulse along the normal that keeps 0.8 of
    // their speed.
    void contactImpulse(ee::Ptr<RigidBody> body, const Vec4 *point, const Vec4 *normal, const Vec4 *otherVelocity)
    {
        // Every working vector on the game's stack, as the original's: what
        // they leave there is what the caller's next callees find (the
        // skater's, a car's and an NPC's boxes against a body).
        ee::Local<Vec4> velocityAtPoint;
        ee::Local<Vec4> other;
        ee::Local<Vec4> arm;
        ee::Local<Vec4> relative;
        ee::Local<Vec4> turned;     // what the body's turning takes: (I^-1 (r x n)) x r
        ee::Local<Vec4> impulse;
        ee::Local<Vec4> change;     // of the velocity
        ee::Local<Vec4> spinChange;
        pointVelocity(body, point, velocityAtPoint);
        other = *otherVelocity;
        arm = *point - body->position;
        relative = *velocityAtPoint - *other;
        const ee::Float closing = dot(*normal, *relative);
        if (ee::Float(kImpulseSeparating) < dot(*relative, *normal))
            return;
        const ee::Float inverseMass = ee::Float(1.0f) / body->mass;
        turned = cross(rotated(body->worldInverseInertia, cross(*arm, *normal)), *arm);
        const ee::Float turning = dot(*normal, *turned);
        impulse = *normal * (closing * ee::Float(kImpulseRestitution) / (inverseMass + ee::Float(kImpulseSoftness) + turning));
        change = *impulse * (ee::Float(1.0f) / body->mass);
        body->velocity = body->velocity + *change;
        spinChange = rotated(body->worldInverseInertia, cross(*arm, *impulse));
        body->spin = body->spin + *spinChange;
        body->contactCount = 1;
    }
    READABLE_FUNCTION(contactImpulse, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float vec3Length(ee::Ptr<const Vec3> vector) { return zge::vec3Length(vector); }
        inline ee::Global<uint32_t, sym::g_dynamicsBodyColour> g_dynamicsBodyColour;
        inline ee::Global<uint32_t, sym::g_dynamicsBodySphereColour> g_dynamicsBodySphereColour;
        constexpr uint32_t kDebugLineFlags = 1, kDebugPointFlags = 2;
    }

    // drawDynamicsBody: a debug drawing of a box body: a line through its
    // centre along each of its axes, its half extent each way, and a point
    // at each of its spheres (their centres in the body's frame) with its
    // radius. (drawDynamicsObject calls it, and nothing calls that.)
    void drawDynamicsBody(ee::Ptr<RigidBody> body)
    {
        ee::Local<Vec4> start;
        ee::Local<Vec4> end;
        ee::Local<uint32_t> colour;
        ee::Local<uint32_t> sphereColour;
        const Matrix &rotation = body->rotation;
        *colour = *g_dynamicsBodyColour;
        const BodyExtents &extents = *body->halfExtents;
        *sphereColour = *g_dynamicsBodySphereColour;

        const auto drawAxis = [&](uint32_t axis, ee::Float half)
        {
            const Vec4 reach = matrixRow(rotation, axis) * half;
            *end = body->position - reach;
            *start = body->position + reach;
            addDebugLine(start, end, colour, kDebugLineFlags);
        };
        drawAxis(0, extents.half.x);
        drawAxis(1, extents.half.y);
        drawAxis(2, extents.half.z);

        for (uint32_t i = 0; i < body->sphereCount; ++i)
        {
            const BodySphere &sphere = body->spheres[i];
            *start = body->position;
            *start = *start + matrixRow(rotation, 0) * sphere.centre.x;
            *start = *start + matrixRow(rotation, 1) * sphere.centre.y;
            *start = *start + matrixRow(rotation, 2) * sphere.centre.z;
            // (addDebugPoint's fourth argument is not read: the original hands
            // on whatever its register holds.)
            addDebugPoint(start, sphereColour, kDebugPointFlags, 0, sphere.radius);
        }
    }
    READABLE_FUNCTION_WAITS(drawDynamicsBody, Draft);
}
