// A dynamics object's life (zge/collision/dynamics.h): its definition read
// from the file, its bodies put at their bones when it wakes, and the step:
// each body alone when it has no links, else the constraint system solved
// first; then the contacts and the sweep's boxes.

#include "dynamics.h"

#include "zge/platform/platform.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/collision/collision.h"
#include "zge/collision/sweep.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/debug/profiler.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics_update.cpp.h"


namespace zge
{

    namespace
    {
        inline ee::Global<ee::Float, sym::g_contactRestitution> g_contactRestitution;
        inline ee::Global<ee::Float, sym::g_contactDamping> g_contactDamping;
        inline ee::Global<ee::Float, sym::g_dynamicsInverseStep> g_dynamicsInverseStep;
        inline ee::Global<uint32_t, sym::g_dynamicsSteps> g_dynamicsSteps;
        inline ee::Global<uint32_t, sym::g_dynamicsProfileBar> g_dynamicsProfileBar;

        // The sphere collisionQuery is asked with: a body's centre and size.
        struct BoundingSphere
        {
            Vec4 centre;
            ee::Float radius;
        };
        inline ee::Global<BoundingSphere, sym::g_dynamicsProbe> g_dynamicsProbe;
        constexpr float kLinkedBodiesRadius = 1.2f;

        constexpr float kDefaultRestitution = 0.2f, kDefaultContactDamping = 0.1f;

        void clearXyz(Vec4 &v) { v.x = v.y = v.z = ee::Float::fromBits(0); }

        // A quaternion of a body as an address: a wild body (the cut ragdoll's)
        // keeps its wild address through a game call.
        ee::Ptr<Quaternion> quaternionField(ee::Ptr<RigidBody> body, size_t offset)
        {
            return (body.cast<uint8_t>() + static_cast<ptrdiff_t>(offset)).cast<Quaternion>();
        }

        // A body at its bone, with nothing moving it: a box's corner is the
        // bone's point. bindDynamicsToBones and wakeDynamics have it inline.
        void restBodyAtBone(ee::Ptr<RigidBody> body, ee::Ptr<Dynamics> dynamics, Vec4 &corner)
        {
            corner = body->boxSize * ee::Float(-1.0f);
            objectBonePoint(dynamics->sceneObject, body->bone, &corner, &body->position, 0);
            body->rest.w = ee::Float(1.0f);
            body->rest.x = body->rest.y = body->rest.z = ee::Float::fromBits(0);
            clearXyz(body->spin);
            clearXyz(body->velocity);
            if constexpr (!game::readable::kIsBmx) // (ZGE4's bodies keep no last state)
            {
                clearXyz(body->lastVelocity);
                clearXyz(body->lastSpin);
                body->pushedPosition = body->position;
                quaternionCopy(&body->lastOrientation, &body->orientation);
            }
            body->solvedForce = kOrigin;
            body->solvedTorque = kOrigin;
            body->contactCount = 0;
        }
    }

    // shutdownDynamicsPool (ZGE2 destroys the pool as it stands and keeps the word).
    uint32_t shutdownDynamicsPool()
    {
        const uint32_t pool = *g_dynamicsDefinitionPool;
        if constexpr (game::readable::kIsDm2)
            destroyPool(ee::Ptr<Pool>(pool));
        else if (pool != 0)
        {
            destroyPool(ee::Ptr<Pool>(pool));
            *g_dynamicsDefinitionPool = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownDynamicsPool, Verified);

    // resetDynamicsDefinitions: all definitions gone, an empty pool. (ZGE4 only destroys the pool: it makes one
    // again when the next definition loads; ZGE2 destroys it without asking whether there is one.)
    uint32_t resetDynamicsDefinitions()
    {
        if constexpr (game::readable::kIsBmx)
        {
            shutdownDynamicsPool();
            return 1;
        }
        const uint32_t pool = *g_dynamicsDefinitionPool;
        if (game::readable::kIsDm2 || pool != 0)
        {
            destroyPool(ee::Ptr<Pool>(pool));
            *g_dynamicsDefinitionPool = createPool(kDynamicsBytes, 0u, sym::freeDynamicsDefinitionRecord);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(resetDynamicsDefinitions, Verified);

    // loadDynamicsDefinition: a ZDYN chunk: a definition with the default contact
    // numbers, its chunks read into it, its bodies linked if it has links.
    // Returns it.
    ee::Ptr<Dynamics> loadDynamicsDefinition(ee::Ptr<ChunkReader> reader)
    {
        if (enterChunk(reader) == 0)
            return ee::Ptr<Dynamics>();
        if constexpr (game::readable::kIsBmx) // ZGE4 makes the definitions' pool here, at the first definition
            if (*g_dynamicsDefinitionPool == 0)
                *g_dynamicsDefinitionPool = createPool(kDynamicsBytes, 0u, sym::freeDynamicsDefinitionRecord);
        const ee::Ptr<Dynamics> dynamics(poolAlloc(ee::Ptr<Pool>(*g_dynamicsDefinitionPool)));
        memoryFill(dynamics, 0, kDynamicsBytes);
        dynamics->restitution = ee::Float(kDefaultRestitution);
        if constexpr (!game::readable::kIsDm2) // (ZGE2's dynamics have the one contact number)
            dynamics->contactDamping = ee::Float(kDefaultContactDamping);
        *g_currentDynamics = dynamics;
        parseChunks(reader, 0u);
        leaveChunk(reader);
        if (dynamics->links)
            buildBodyHierarchy(dynamics);
        return *g_currentDynamics;
    }
    READABLE_FUNCTION_WAITS(loadDynamicsDefinition, Verified);

    // bindDynamicsToBones: the dynamics given to an object: each body with a bone
    // put at it, at rest.
    void bindDynamicsToBones(ee::Ptr<SceneObject> sceneObject, ee::Ptr<Dynamics> dynamics)
    {
        ee::Local<Vec4> corner;
        dynamics->sceneObject = sceneObject;
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            if (body->bone == 0)
                continue;
            bodyFromBone(dynamics, body, &body->position, &body->rotation, &body->orientation);
            restBodyAtBone(body, dynamics, corner);
        }
    }
    READABLE_FUNCTION(bindDynamicsToBones, Verified);

    // wakeDynamics: an object's dynamics start: its pose brought up to date,
    // each body found its bone and channel and put there, then each body
    // finds its parent body up the bones and is integrated once with no
    // time, so that its state is whole; then bound.
    void wakeDynamics(ee::Ptr<SceneObject> object, ee::Ptr<Dynamics> dynamics, uint32_t flags)
    {
        ee::Local<Vec4> corner;
        const auto boneChannelOf = [&](ee::Ptr<RigidBody> body) -> ee::Ptr<AnimationChannel>
        {
            const ee::Ptr<Animator> animator = object->animator;
            return animator ? findBoneChannel(animator, body->nameHash) : ee::Ptr<AnimationChannel>();
        };
        dynamics->sceneObject = object;
        updatePose(object);
        dynamics->bodies->boneDriven = 1;
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            const ee::Ptr<SceneBone> bone = findModelBone(object, body->nameHash);
            body->bone = bone;
            body->boneChannel = boneChannelOf(body);
            if (bone != 0)
            {
                bodyFromBone(dynamics, body, &body->position, &body->rotation, &body->orientation);
                modelBoneBindPosition(dynamics->sceneObject, bone, &body->bindPosition);
                restBodyAtBone(body, dynamics, corner);
                if constexpr (game::readable::kIsBmx) // ZGE4 brings the world inertia up to date here
                    updateWorldInertia(body);
            }
        }
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            ee::Ptr<SceneBone> parentBone = body->bone ? boneParent(object->scene, body->bone) : ee::Ptr<SceneBone>();
            body->parentBody = ee::Ptr<RigidBody>();
            // Up the bones to the first that has a body.
            while (parentBone != 0)
            {
                ee::Ptr<RigidBody> other = dynamics->bodies;
                while (other && other->bone != parentBone)
                    other = other->next;
                body->parentBody = other;
                if (other)
                    break;
                parentBone = boneParent(object->scene, parentBone);
            }
            body->boneChannel = boneChannelOf(body);
            if constexpr (!game::readable::kIsBmx)
                body->lastPosition = body->position;
            *g_dynamicsStepSeconds = ee::Float(0.0f);
            integrateBody(body);
        }
        bindDynamicsToBones(object, dynamics);
        dynamics->flags = flags;
    }
    READABLE_FUNCTION_WAITS(wakeDynamics, Verified);

    // sweepUpdateBody: a body's box in the sweep moved to where it now is, axis by
    // axis (a move up the high end first, down the low end).
    void sweepUpdateBody(ee::Ptr<Sweep> sweep, ee::Ptr<RigidBody> body)
    {
        if (!body->sweepEntry || !sweep)
            return;
        constexpr float kMostMoved = 1e+20f;
        const ee::Float *centre = &body->position.x;
        // (Both words read at each use: the original's calls can move them.)
        const auto lowEnd = [&](uint32_t axis) { return body->sweepEntry->lowEnds[axis]; };
        // How far the box's middle is from the body, limited.
        const auto offset = [&](uint32_t axis)
        {
            const ee::Ptr<SweepEnd> low = lowEnd(axis);
            const ee::Float by = centre[axis] - (low->value + low->other->value) / ee::Float(2.0f);
            if (ee::Float(kMostMoved) < by)
                return ee::Float(kMostMoved);
            return by < ee::Float(-kMostMoved) ? ee::Float(-kMostMoved) : by;
        };
        // The ends moved and put right.
        const auto move = [&](uint32_t axis, ee::Float by)
        {
            ee::Ptr<SweepEnd> low = lowEnd(axis);
            low->value = low->value + by;
            ee::Ptr<SweepEnd> high = lowEnd(axis)->other;
            high->value = high->value + by;
            if (ee::Float(0.0f) < by)
            {
                sweepMoveUp(sweep, sweep->axes[axis], lowEnd(axis)->other, axis * 2);
                sweepLowUp(sweep, sweep->axes[axis], lowEnd(axis), axis * 2 + 1);
            }
            else
            {
                sweepLowDown(sweep, sweep->axes[axis], lowEnd(axis), axis * 2);
                sweepHighDown(sweep, sweep->axes[axis], lowEnd(axis)->other, axis * 2 + 1);
            }
        };
        for (uint32_t axis = 0; axis < 3; ++axis)
            move(axis, offset(axis));
    }
    READABLE_FUNCTION_WAITS(sweepUpdateBody, Verified);

    // integrateBodyRotation: a body with a parent: its orientation slerped towards the
    // parent's and its own bone's turn, as far as turnShare says.
    void integrateBodyRotation(ee::Ptr<RigidBody> body)
    {
        ee::Local<Quaternion> turned;
        const ee::Ptr<RigidBody> parent = body->parentBody;
        if (!parent)
            return;
        quaternionMultiply(turned, &parent->orientation, body->boneTurnQuaternion());
        quaternionAccumulate(body->solvedTurnQuaternion(), body->solvedTurnQuaternion(), turned);
        quaternionSlerp(&body->orientation, &body->orientation, body->solvedTurnQuaternion(), body->turnShare);
        quaternionToMatrix(&body->rotation, &body->orientation);
    }
    READABLE_FUNCTION(integrateBodyRotation, Draft);

    // saveBodyState: what the step starts from. (ZGE4 keeps none of it: its bodies have no last state, and this
    // does nothing there.)
    void saveBodyState(ee::Ptr<RigidBody> body)
    {
        if constexpr (game::readable::kIsBmx)
            return;
        quaternionCopy(quaternionField(body, offsetof(RigidBody, lastOrientation)), quaternionField(body, offsetof(RigidBody, orientation)));
        body->lastVelocity = body->velocity;
        body->lastSpin = body->spin;
        body->pushedPosition = body->position;
        copyBodyMatrix(&body->lastInverseInertia, &body->worldInverseInertia);
        body->lastForce = body->solvedForce;
        body->lastTorque = body->solvedTorque;
    }
    READABLE_FUNCTION(saveBodyState, Verified);

    // transformContactFaces: the first 40 faces of the contact objects moved to the
    // world for the step.
    void transformContactFaces()
    {
        ee::Local<Vec4> point;
        ee::Local<ee::Ptr<uint16_t>> vertexIndices;
        uint32_t used = 0;
        for (int32_t objectIndex = 0; objectIndex < g_contactObjects->count; ++objectIndex)
        {
            const ee::Ptr<HitNode> object = contactObject(objectIndex);
            const ee::Ptr<ZgePart> part = object->part;
            const ee::Ptr<Node> node = object->node;
            for (uint32_t faceIndex = 0; faceIndex < contactObject(objectIndex)->faceCount; ++faceIndex)
            {
                const uint32_t face = contactObject(objectIndex)->faces[faceIndex].face;
                WorldFace &worldFace = (*g_worldFaces)[used];
                faceVertices(part, face, vertexIndices);
                for (uint32_t vertex = 0; vertex < 3; ++vertex)
                {
                    vertexPosition(part, (*vertexIndices)[vertex], point);
                    if constexpr (game::readable::kIsDm2) // (ZGE2 through its transformPoint)
                        transformPoint(&worldFace.vertices[vertex], nodeMatrix(node, 1u), point);
                    else
                        worldFace.vertices[vertex] = transformed(*nodeMatrix(node, 1u), point);
                }
                faceNormal(part, face, point);
                worldFace.normal = rotated(*nodeMatrix(node, 1u), point);
                if (++used >= kWorldFaceCount)
                    return;
            }
        }
    }
    READABLE_FUNCTION(transformContactFaces, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
    }

    // updateDynamics: a step of `seconds`. Alone, each body integrates on its
    // own, with the contacts; with links, the system is solved first and the
    // bodies follow it. Then their objects.
    ee::Float updateDynamics(ee::Ptr<Dynamics> dynamics, ee::Ptr<Sweep> sweep, ee::Float seconds)
    {
        ee::Local<Vec4> zero;
        // Is the body near anything: the object list filled, its faces made ready.
        const auto probe = [&](const Vec4 &centre, ee::Float radius)
        {
            g_dynamicsProbe->centre = centre;
            g_dynamicsProbe->radius = radius;
            int32_t hit;
            if constexpr (game::readable::kIsDm2) // (ZGE2's query takes no cache)
                hit = collisionQueryUncached(0, g_dynamicsProbe.get(), 0, 0, 0, 0, ee::Ptr<HitNodeTable>(sym::g_contactObjects));
            else
                hit = collisionQuery(0, g_dynamicsProbe.get(), 0, 0, 0, 0, contactObjectResults(), 0);
            *g_collideWithFaces = static_cast<uint32_t>(hit);
            if (hit != 0)
                transformContactFaces();
        };
        const auto takeForces = [](ee::Ptr<RigidBody> body)
        {
            body->solvedForce = body->force;
            body->solvedTorque = body->torque;
        };

        // (ZGE2 runs the step in a substep loop whose count is 1: once.)
        *g_contactRestitution = dynamics->restitution;
        if constexpr (!game::readable::kIsDm2) // (ZGE2's damping is an engine constant: resolveBodyContacts)
            *g_contactDamping = dynamics->contactDamping;
        *g_dynamicsStepSeconds = seconds;
        *g_currentDynamics = dynamics;
        *g_dynamicsInverseStep = ee::Float(1.0f) / seconds;
        if (!dynamics->links)
        {
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                probe(body->position, body->halfExtents->length);
                takeForces(body);
                integrateBody(body);
                resolveBodyContacts(body);
                body->position = body->position + body->contactPush;
                sweepUpdateBody(sweep, body);
            }
        }
        else
        {
            memset(zero, 0, sizeof(Vec4));
            setBoneTranslation(dynamics->bodies->boneChannel, zero);
            updatePose((*g_currentDynamics)->sceneObject);
            startProfileBar(*g_dynamicsProfileBar);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                saveBodyState(body);
                takeForces(body);
                for (uint32_t &word : body->solvedTurn)
                    word = 0;
                body->springImpulse = kOrigin;
                body->springSpin = kOrigin;
            }
            solveDynamicsIsland(dynamics);
            probe(dynamics->bodies->position, ee::Float(kLinkedBodiesRadius));
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                integrateBody(body);
                resolveBodyContacts(body);
                if (body->contactCount != 0)
                    body->position = body->position + body->contactPush;
                if (body->contacts)
                    resolveBodyContactList(body);
            }
            stopProfileBar(*g_dynamicsProfileBar);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
                sweepUpdateBody(sweep, body);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
                if (body->followsParentTurn != 0)
                    integrateBodyRotation(body);
        }
        applyBodiesToObjects(dynamics);
        ++*g_dynamicsSteps;
        return seconds;
    }

    // updateDynamicsWithHeldContacts: ZGE4's updateDynamics. The contacts are resolved with the body's dynamics
    // (resolveBodyContactsFromDynamics), and a body alone is moved by its push only when it has a contact and
    // then has its held points resolved, as ZGE3 does only for linked bodies.
    ee::Float updateDynamicsWithHeldContacts(ee::Ptr<Dynamics> dynamics, ee::Ptr<Sweep> sweep, ee::Float seconds)
    {
        ee::Local<Vec4> zero;
        const auto probe = [&](const Vec4 &centre, ee::Float radius)
        {
            g_dynamicsProbe->centre = centre;
            g_dynamicsProbe->radius = radius;
            const int32_t hit = collisionQuery(0, g_dynamicsProbe.get(), 0, 0, 0, 0, contactObjectResults(), 0);
            *g_collideWithFaces = static_cast<uint32_t>(hit);
            if (hit != 0)
                transformContactFaces();
        };
        const auto takeForces = [](ee::Ptr<RigidBody> body)
        {
            body->solvedForce = body->force;
            body->solvedTorque = body->torque;
        };
        const auto pushOut = [](ee::Ptr<RigidBody> body)
        {
            if (body->contactCount != 0)
                body->position = body->position + body->contactPush;
            if (body->contacts)
                resolveBodyContactList(body);
        };

        *g_contactRestitution = dynamics->restitution;
        *g_contactDamping = dynamics->contactDamping;
        *g_dynamicsStepSeconds = seconds;
        *g_currentDynamics = dynamics;
        *g_dynamicsInverseStep = ee::Float(1.0f) / seconds;
        if (!dynamics->links)
        {
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                probe(body->position, body->halfExtents->length);
                takeForces(body);
                integrateBody(body);
                resolveBodyContactsFromDynamics(dynamics, body);
                pushOut(body);
                sweepUpdateBody(sweep, body);
            }
        }
        else
        {
            memset(zero, 0, sizeof(Vec4));
            setBoneTranslation(dynamics->bodies->boneChannel, zero);
            updatePose((*g_currentDynamics)->sceneObject);
            startProfileBar(*g_dynamicsProfileBar);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                saveBodyState(body);
                takeForces(body);
                for (uint32_t &word : body->solvedTurn)
                    word = 0;
                body->springImpulse = kOrigin;
                body->springSpin = kOrigin;
            }
            solveDynamicsIsland(dynamics);
            probe(dynamics->bodies->position, ee::Float(kLinkedBodiesRadius));
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            {
                integrateBody(body);
                resolveBodyContactsFromDynamics(dynamics, body);
                pushOut(body);
            }
            stopProfileBar(*g_dynamicsProfileBar);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
                sweepUpdateBody(sweep, body);
            for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
                if (body->followsParentTurn != 0)
                    integrateBodyRotation(body);
        }
        applyBodiesToObjects(dynamics);
        ++*g_dynamicsSteps;
        return seconds;
    }
    READABLE_FUNCTION_WAITS_OWN(updateDynamics, Verified, bmx, updateDynamicsWithHeldContacts);

    namespace // the game functions below call: ee/plain.h
    {
        // addDebugLine (zdebug): a line between two points in a colour.
        ee::Float vec3Length(ee::Ptr<const Vec3> vector) { return zge::vec3Length(vector); }
        inline ee::Global<uint32_t, sym::g_dynamicsConstraintColour> g_dynamicsConstraintColour;
    }

    void drawDynamicsConstraint(ee::Ptr<BodyConstraint> constraint);

    // drawDynamicsObject: a debug drawing of the dynamics: its box bodies
    // (drawDynamicsBody) and its kind 0 links' constraints.
    void drawDynamicsObject(ee::Ptr<Dynamics> dynamics)
    {
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            if (body->shape == 0)
                drawDynamicsBody(body);
        }
        for (ee::Ptr<DynamicsLink> link = dynamics->links; link; link = link->next)
        {
            if (link->kind == 0)
                drawDynamicsConstraint(link->constraint);
        }
    }
    READABLE_FUNCTION_WAITS(drawDynamicsObject, Draft);

    // drawDynamicsConstraint: a line from each body's position to its anchor
    // (turned by the body's rotation), in g_dynamicsConstraintColour.
    void drawDynamicsConstraint(ee::Ptr<BodyConstraint> constraint)
    {
        ee::Local<Vec4> anchor;
        ee::Local<uint32_t> colour;
        *colour = *g_dynamicsConstraintColour;
        const ee::Ptr<RigidBody> bodyA = constraint->bodyA;
        const ee::Ptr<RigidBody> bodyB = constraint->bodyB;
        *anchor = rotated(bodyA->rotation, constraint->anchorA);
        *anchor = bodyA->position + *anchor;
        addDebugLine(anchor, &bodyA->position, colour, 1);
        *anchor = rotated(bodyB->rotation, constraint->anchorB);
        *anchor = bodyB->position + *anchor;
        addDebugLine(anchor, &bodyB->position, colour, 1);
    }
    READABLE_FUNCTION_WAITS(drawDynamicsConstraint, Draft);

    // placeBodiesAtBones: the object posed, then each body given the model's
    // bone of its name and, when it has one, put there (bodyFromBone, its
    // box's low corner at the bone's point) and its last position with it.
    void placeBodiesAtBones(ee::Ptr<Dynamics> dynamics)
    {
        ee::Local<Vec4> corner;
        updatePose(dynamics->sceneObject);
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            body->bone = findModelBone(dynamics->sceneObject, body->nameHash);
            if (body->bone == 0)
                continue;
            bodyFromBone(dynamics, body, &body->position, &body->rotation, &body->orientation);
            *corner = body->boxSize * ee::Float(-1.0f);
            objectBonePoint(dynamics->sceneObject, body->bone, corner, &body->position, 0);
            body->lastPosition = body->position;
        }
    }
    READABLE_FUNCTION_WAITS(placeBodiesAtBones, Draft);

    // resetDynamicsBones: every body's bone and the object turned back to no
    // rotation, and the object posed again.
    void resetDynamicsBones(ee::Ptr<Dynamics> dynamics)
    {
        ee::Local<Quaternion> identity;
        quaternionIdentity(identity);
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            setBoneRotation(body->boneChannel, identity);
        setObjectOrientation(dynamics->sceneObject, identity);
        updatePose(dynamics->sceneObject);
    }
    READABLE_FUNCTION_WAITS(resetDynamicsBones, Draft);

    // addToSolvedForces: what the bone springs added up (springBodyToBoneTurn)
    // added to the body's solved torque and force.
    void addToSolvedForces(ee::Ptr<RigidBody> body)
    {
        body->solvedTorque = body->solvedTorque + body->springSpin;
        body->solvedForce = body->solvedForce + body->springImpulse;
    }
    READABLE_FUNCTION(addToSolvedForces, Draft);

    // restoreBodyState: the body put back to what saveBodyState kept.
    void restoreBodyState(ee::Ptr<RigidBody> body)
    {
        quaternionCopy(&body->orientation, &body->lastOrientation);
        body->spin = body->lastSpin;
        body->velocity = body->lastVelocity;
        body->solvedForce = body->lastForce;
        body->solvedTorque = body->lastTorque;
        body->position = body->pushedPosition;
        copyBodyMatrix(&body->worldInverseInertia, &body->lastInverseInertia, true);
    }
    READABLE_FUNCTION(restoreBodyState, Draft);

    // pushBodiesFromPoint: an impulse on each box body the point is inside
    // the reach of (its half extents' length): the impulse times the mass
    // turned about the body's centre into spin (through its world inverse
    // inertia), and the impulse added to its velocity as it is.
    void pushBodiesFromPoint(ee::Ptr<Dynamics> dynamics, ee::Ptr<const Vec4> point, ee::Ptr<const Vec4> impulse)
    {
        ee::Local<Vec4> offset;
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            *offset = *point - body->position;
            const ee::Float distance = vec3Length(&offset->xyz());
            if (body->shape != 0 || !(distance < body->halfExtents->length))
                continue;
            const Vec4 scaled = *impulse * body->mass;
            const Vec4 turn = rotated(body->worldInverseInertia, cross(*offset, scaled));
            body->spin = body->spin + turn;
            body->velocity = body->velocity + *impulse;
        }
    }
    READABLE_FUNCTION(pushBodiesFromPoint, Draft);

    // addBodyImpulseAt: an impulse at a point of the body: the impulse is
    // scaled by 4 where it is (the caller's vector changes), and the spin it
    // gives (the arm from the centre crossed with it, through the world
    // inverse inertia) goes to spinChange. Nothing is applied to the body.
    void addBodyImpulseAt(ee::Ptr<RigidBody> body, ee::Ptr<const Vec4> point, ee::Ptr<Vec4> impulse, ee::Ptr<Vec4> spinChange)
    {
        // The point a fifth of the impulse on: computed, and nothing reads it.
        const Vec4 unusedTip = *point + *impulse * 0.2f;
        (void)unusedTip;
        aligned(impulse) = *impulse * 4.0f;
        const Vec4 arm = *point - body->position;
        aligned(spinChange) = rotated(body->worldInverseInertia, cross(arm, *impulse));
    }
    READABLE_FUNCTION_VU0(addBodyImpulseAt, Draft);

    // springBodyToBoneTurn: springs that pull a body and its parent body
    // toward the turn its bone has on the parent's (boneTurn, which
    // driveBodyFromBone sets), with boneSpring and boneDamping.
    //
    // The anchor is the body's position plus its boxSize turned by its
    // orientation. Each body is pushed at points near its far side: its own
    // position plus 0.7 of the way from the anchor beyond it, plus each of its
    // axes (0.4 long). On each axis the push is how far the axis is from where
    // the bone's turn would put it, times boneSpring, less the velocity there
    // (relative to the other side's at the anchor) times boneDamping. The
    // impulses add up in springImpulse and their spin in springSpin; the solver
    // takes those in through addToSolvedForces. Last, the body's spin times
    // -boneDamping goes into its springImpulse.
    //
    // Kept as the original has them: the parent's side measures the velocity
    // of this body (not the parent's) at its points; the parent's springSpin
    // becomes this body's springSpin plus the push's spin, not its own plus
    // it; and an axis is flipped on one side only (the body's when it points
    // back toward the anchor, else the parent's).
    void springBodyToBoneTurn(ee::Ptr<RigidBody> body)
    {
        ee::Local<Vec4[3]> impulses;
        ee::Local<Vec4[3]> spinChanges;
        ee::Local<Vec4> corner;
        ee::Local<Vec4> anchor;
        ee::Local<Vec4> point;
        ee::Local<Quaternion> inverseTurn;
        ee::Local<Quaternion> targetTurn;       // where the parent's orientation and the bone's turn put the body
        ee::Local<Quaternion> parentTargetTurn; // where the body's orientation less the bone's turn puts the parent
        ee::Local<Matrix> targetRotation;
        ee::Local<Matrix> parentTargetRotation;
        ee::Local<Matrix> rotation;
        ee::Local<Matrix> parentRotation;
        ee::Local<Vec4> velocity;
        ee::Local<Vec4> parentAnchorVelocity;
        ee::Local<Vec4> anchorVelocity;

        memset(corner, 0, sizeof(Vec4));
        if (!body->parentBody)
            return;
        quaternionMultiply(targetTurn, &body->parentBody->orientation, body->boneTurnQuaternion());
        quaternionInverse(inverseTurn, body->boneTurnQuaternion());
        quaternionMultiply(parentTargetTurn, &body->orientation, inverseTurn);
        quaternionToMatrix(targetRotation, targetTurn);
        quaternionToMatrix(rotation, &body->orientation);
        quaternionToMatrix(parentRotation, &body->parentBody->orientation);
        quaternionToMatrix(parentTargetRotation, parentTargetTurn);

        *corner = rotated(*rotation, body->boxSize);
        *anchor = *corner + body->position;
        Vec4 nearSide = body->position - *anchor;
        nearSide = nearSide * 0.7f;
        nearSide = nearSide + body->position;
        Vec4 parentNearSide = body->parentBody->position - *anchor;
        parentNearSide = parentNearSide * 0.7f;
        parentNearSide = parentNearSide + body->parentBody->position;
        for (Matrix *m : {&*targetRotation, &*rotation, &*parentTargetRotation, &*parentRotation})
            for (uint32_t axis = 0; axis < 3; ++axis)
                matrixRow(*m, axis) = matrixRow(*m, axis) * 0.4f;
        const Vec4 offset = body->position - *anchor;

        auto flipAxis = [&](uint32_t axis)
        {
            if (dot(offset, matrixRow(*rotation, axis)) < ee::Float(0.0f))
            {
                vec3Negate(&matrixRow(*rotation, axis).xyz(), &matrixRow(*rotation, axis).xyz());
                vec3Negate(&matrixRow(*targetRotation, axis).xyz(), &matrixRow(*targetRotation, axis).xyz());
            }
            else
            {
                vec3Negate(&matrixRow(*parentRotation, axis).xyz(), &matrixRow(*parentRotation, axis).xyz());
                vec3Negate(&matrixRow(*parentTargetRotation, axis).xyz(), &matrixRow(*parentTargetRotation, axis).xyz());
            }
        };
        auto pushAxis = [&](uint32_t axis)
        {
            const ee::Ptr<Vec4> impulse = &impulses[axis];
            const ee::Ptr<Vec4> spinChange = &spinChanges[axis];

            *impulse = matrixRow(*targetRotation, axis) - matrixRow(*rotation, axis);
            *impulse = *impulse * body->boneSpring;
            *point = matrixRow(*rotation, axis) + nearSide;
            pointVelocity(body, point, velocity);
            *velocity = *velocity - *parentAnchorVelocity;
            *velocity = *velocity * -body->boneDamping;
            *impulse = *velocity + *impulse;
            addBodyImpulseAt(body, point, impulse, spinChange);
            body->springImpulse = *impulse + body->springImpulse;
            body->springSpin = *spinChange + body->springSpin;

            *impulse = matrixRow(*parentTargetRotation, axis) - matrixRow(*parentRotation, axis);
            *impulse = *impulse * body->boneSpring;
            *point = matrixRow(*parentRotation, axis) + parentNearSide;
            pointVelocity(body, point, velocity);
            *velocity = *velocity - *anchorVelocity;
            *velocity = *velocity * -body->boneDamping;
            *impulse = *velocity + *impulse;
            addBodyImpulseAt(body->parentBody, point, impulse, spinChange);
            body->parentBody->springImpulse = *impulse + body->parentBody->springImpulse;
            body->parentBody->springSpin = *spinChange + body->springSpin;
        };

        flipAxis(0);
        pointVelocity(body, anchor, anchorVelocity);
        pointVelocity(body->parentBody, anchor, parentAnchorVelocity);
        pushAxis(0);
        flipAxis(1);
        pushAxis(1);
        flipAxis(2);
        pushAxis(2);

        *velocity = body->spin * -body->boneDamping;
        body->springImpulse = body->springImpulse + *velocity;
    }
    READABLE_FUNCTION(springBodyToBoneTurn, Draft);
}
