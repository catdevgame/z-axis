// The animate system: what makes a skater's shirt, coat, ponytails and bob
// follow the body (aggressive/skater/rig.cpp builds them). Points with a mass,
// held to a bone or free, are moved by Verlet-style steps: forces and velocity
// first, then four rounds of constraints, each round's places for a point
// averaged by weight, and the velocity follows what the rounds did. Groups of
// planes keep points out of the body, and pivots turn bones to follow the
// points. The system's lists are all made at set-up, in place.
//
// The steps' working vectors are ee::Local, on the stack as the original's: a
// matrix built on the stack later (a camera object's) takes the w lanes it
// never sets from what they leave there.

#include "zge/memory/pool.h"
#include "zge/assets/chunk_reader.h"
#include "zge/collision/collision.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/route.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/animate_system.cpp.h"

namespace zge
{

    namespace
    {

        // How many rounds animate gives the constraints (4).
        inline ee::Global<uint32_t, sym::g_animateRounds> g_animateRounds;
        // Set by animate for the functions it calls.
        inline ee::Global<ee::Float, sym::g_animateStep> g_animateStep;
        inline ee::Global<ee::Ptr<AnimateSystem>, sym::g_animateSystem> g_animateSystem;

        constexpr ee::Float kFarBehindPlanes{-1000.0f};
        constexpr ee::Float kVelocityKept{0.95f};
        constexpr ee::Float kNotQuiteParallel{0.0001f};
        constexpr ee::Float kPointOnCentre{0.0001f};

        // Half of every constraint: `place` is what it asks of the point,
        // weighed already.
        void addSolvedPlace(ee::Ptr<AnimatePoint> point, const Vec4 &place, ee::Float weight)
        {
            point->solvedSum += place;
            point->solvedWeight = point->solvedWeight + weight;
        }

        // Frees a list of records that hold nothing else.
        template <typename Record>
        void freeRecords(ee::Ptr<Record> record)
        {
            while (record)
            {
                const ee::Ptr<Record> following = record->next;
                heapFree(record);
                record = following;
            }
        }

        // Whether a plane lets this point through.
        bool planeIgnores(ee::Ptr<AnimatePlane> plane, ee::Ptr<AnimatePoint> point)
        {
            for (uint32_t i = 0; i < plane->ignoredCount; ++i)
                if (same(plane->ignored[i], point))
                    return true;
            return false;
        }

    }

    // freeAnimatePoints
    void freeAnimatePoints(ee::Ptr<AnimatePoint> points) { freeRecords(points); }
    READABLE_FUNCTION_WAITS(freeAnimatePoints, Verified);

    // freeAnimatePivots
    void freeAnimatePivots(ee::Ptr<AnimatePivot> pivots) { freeRecords(pivots); }
    READABLE_FUNCTION_WAITS(freeAnimatePivots, Verified);

    // freeAnimateConstraints
    void freeAnimateConstraints(ee::Ptr<AnimateConstraint> constraints) { freeRecords(constraints); }
    READABLE_FUNCTION_WAITS(freeAnimateConstraints, Verified);

    // freeAnimatePlanes
    void freeAnimatePlanes(ee::Ptr<AnimatePlane> planes) { freeRecords(planes); }
    READABLE_FUNCTION_WAITS(freeAnimatePlanes, Verified);

    // freeAnimatePlaneGroups
    void freeAnimatePlaneGroups(ee::Ptr<AnimatePlaneGroup> group)
    {
        while (group)
        {
            const ee::Ptr<AnimatePlaneGroup> following = group->next;
            freeAnimatePlanes(group->planes);
            heapFree(group);
            group = following;
        }
    }
    READABLE_FUNCTION_WAITS(freeAnimatePlaneGroups, Verified);

    // freeAnimateSpheres
    void freeAnimateSpheres(ee::Ptr<AnimateSphere> spheres) { freeRecords(spheres); }
    READABLE_FUNCTION_WAITS(freeAnimateSpheres, Verified);

    // freeAnimatePulls
    void freeAnimatePulls(ee::Ptr<AnimatePull> pulls) { freeRecords(pulls); }
    READABLE_FUNCTION_WAITS(freeAnimatePulls, Verified);

    // pushPointOutOfPlanes: each group's planes are the sides of a solid: a point
    // behind all of them is inside, and goes out through the nearest. A plane
    // that lists the point does not count, as if it had no such side.
    void pushPointOutOfPlanes(ee::Ptr<AnimatePoint> point)
    {
        ee::Local<Vec4> push;
        for (ee::Ptr<AnimatePlaneGroup> group = (*g_animateSystem)->planeGroups; group; group = group->next)
        {
            ee::Float nearest = kFarBehindPlanes;
            bool inside = false;
            ee::Ptr<AnimatePlane> nearestPlane;
            for (ee::Ptr<AnimatePlane> plane = group->planes; plane; plane = plane->next)
            {
                const ee::Float inFront = dot(point->position, plane->worldNormal) - plane->worldDistance;
                if (planeIgnores(plane, point))
                    continue;
                if (ee::Float(0.0f) < inFront)
                {
                    inside = false;
                    break;
                }
                if (nearest < inFront)
                {
                    nearest = inFront;
                    inside = true;
                    nearestPlane = plane;
                }
            }
            if (inside)
            {
                push = nearestPlane->worldNormal * -nearest;
                point->position += push;
            }
        }
    }
    READABLE_FUNCTION(pushPointOutOfPlanes, Verified);

    // integratePointMotion: the step's move: the force's acceleration, then the
    // velocity kept at 0.95. A held point only remembers where it was.
    void integratePointMotion(ee::Ptr<AnimatePoint> point)
    {
        ee::Local<Vec4> move;
        ee::Local<Vec4> speedGained;
        ee::Local<Vec4> acceleration;
        if constexpr (!game::readable::kIsBmx) // ZGE4's animate keeps where the points were, before the planes move
            point->lastPosition = point->position;
        if (point->kind == kPointHeld)
            return;
        const ee::Float step = *g_animateStep;
        acceleration = point->force * point->inverseMass;
        move = point->velocity * step;
        speedGained = *acceleration * step;
        point->position += move;
        point->velocity += speedGained;
        point->velocity *= kVelocityKept;
        // The acceleration's own share of the move: over half a step, not
        // half a step squared, the original's.
        acceleration = *acceleration * (step * ee::Float(0.5f));
        point->position += acceleration;
    }
    READABLE_FUNCTION(integratePointMotion, Verified);

    // updateAnimatePlane: a plane from its bone to the world.
    void updateAnimatePlane(ee::Ptr<AnimatePlane> plane)
    {
        ee::Local<Matrix> boneMatrix;
        objectBoneMatrix(plane->objectOrTake([] { return (*g_animateSystem)->object; }), plane->bone, boneMatrix, kBoneInWorld);
        plane->worldPlace = transformed(boneMatrix, plane->place);
        plane->worldNormal = rotated(boneMatrix, plane->normal);
        plane->worldDistance = dot(plane->worldNormal, plane->worldPlace);
    }
    READABLE_FUNCTION(updateAnimatePlane, Verified);

    // startPointSolve: the rounds start with the point where it is, at the weight
    // of its mass.
    void startPointSolve(ee::Ptr<AnimatePoint> point)
    {
        point->solved = point->position;
        point->solvedSum = point->position * point->mass;
        point->solvedWeight = point->mass;
    }
    READABLE_FUNCTION(startPointSolve, Verified);

    // finishPointSolve: what the rounds moved the point becomes velocity (half of
    // it) and the point goes there. A held point's velocity is (0, 0, 0, 1).
    void finishPointSolve(ee::Ptr<AnimatePoint> point)
    {
        ee::Local<Vec4> moved;
        if (point->kind == kPointHeld)
        {
            point->velocity = kZero;
            return;
        }
        moved = point->solved - point->position;
        moved = *moved * (ee::Float(0.5f) / *g_animateStep);
        point->velocity += moved;
        point->position = point->solved;
    }
    READABLE_FUNCTION(finishPointSolve, Verified);

    // solveAnchor: kConstraintAnchor. No route: only the dead newAnimateAnchor
    // makes one.
    void solveAnchor(ee::Ptr<AnimateConstraint> constraint)
    {
        ee::Local<Vec4> place;
        place = constraint->offset * constraint->stiffness;
        addSolvedPlace(constraint->first, place, constraint->stiffness);
    }
    READABLE_FUNCTION(solveAnchor, Draft);

    // solveBonePoint: kConstraintBonePoint: the point goes to its place on the
    // bone at once, not only in the sum.
    void solveBonePoint(ee::Ptr<AnimateConstraint> constraint)
    {
        ee::Local<Vec4> place;
        ee::Local<Matrix> boneMatrix;
        if constexpr (game::readable::kIsBmx) // ZGE4 places it on the constraint's own object
            objectBoneMatrix(constraint->ownObject(), constraint->bone, boneMatrix, kBoneInWorld);
        else
            objectBoneMatrix((*g_animateSystem)->object, constraint->bone, boneMatrix, kBoneInWorld);
        place = transformed(boneMatrix, constraint->offset);
        const ee::Ptr<AnimatePoint> point = constraint->first;
        point->position = place;
        point->solved = place;
        addSolvedPlace(point, *place * constraint->stiffness, constraint->stiffness);
    }
    READABLE_FUNCTION(solveBonePoint, Verified);

    // solveBoneOffset: kConstraintBoneOffset: the two points an offset apart, the
    // offset turned with the bone.
    void solveBoneOffset(ee::Ptr<AnimateConstraint> constraint)
    {
        ee::Local<Vec4> firstPlace;
        ee::Local<Vec4> secondPlace;
        ee::Local<Vec4> offset;
        ee::Local<Matrix> boneMatrix;
        objectBoneMatrix((*g_animateSystem)->object, constraint->bone, boneMatrix, kBoneInWorld);
        const ee::Ptr<AnimatePoint> first = constraint->first, second = constraint->second;
        offset = rotated(boneMatrix, constraint->offset);
        secondPlace = first->solved + offset;
        firstPlace = second->solved - offset;
        addSolvedPlace(first, *firstPlace * constraint->stiffness, constraint->stiffness);
        addSolvedPlace(second, *secondPlace * constraint->stiffness, constraint->stiffness);
    }
    READABLE_FUNCTION(solveBoneOffset, Verified);

    namespace
    {
        constexpr float kSamePlaceSquared = 0.001f; // (BMX XXX's solveDistance: points closer than this have no direction)
    }

    // solveDistance: kConstraintDistance: each point a rest length from where
    // the other is this round, weighed by the other's share of the masses.
    // The first's share divides by twice the second's mass, not the two
    // masses, so it is always half: the bug kept.
    void solveDistance(ee::Ptr<AnimateConstraint> constraint)
    {
        ee::Local<Vec4> firstPlace;
        ee::Local<Vec4> secondPlace;
        ee::Local<Vec4> apart;
        const ee::Ptr<AnimatePoint> first = constraint->first, second = constraint->second;
        apart = first->solved - second->solved;
        if constexpr (game::readable::kIsBmx) // ZGE4 leaves two points (nearly) at one place as they are
            if (squaredLength(*apart) < ee::Float(kSamePlaceSquared))
                return;
        apart = unit(apart);
        apart = *apart * constraint->restLength;
        const ee::Float secondMass = second->mass, firstMass = first->mass;
        const ee::Float firstWeight = secondMass * constraint->stiffness / (secondMass + secondMass);
        const ee::Float secondWeight = firstMass * constraint->stiffness / (firstMass + secondMass);
        firstPlace = second->solved + apart;
        firstPlace = *firstPlace * firstWeight;
        secondPlace = first->solved - apart;
        secondPlace = *secondPlace * secondWeight;
        addSolvedPlace(first, firstPlace, firstWeight);
        addSolvedPlace(second, secondPlace, secondWeight);
    }
    READABLE_FUNCTION(solveDistance, Verified);

    // solveConstraint: the kind's rule, any other kind none.
    void solveConstraint(ee::Ptr<AnimateConstraint> constraint)
    {
        switch (constraint->kind)
        {
        case kConstraintAnchor: game::tail(zge::solveAnchor, constraint); break;
        case kConstraintDistance: game::tail(zge::solveDistance, constraint); break;
        case kConstraintBonePoint: game::tail(zge::solveBonePoint, constraint); break;
        case kConstraintBoneOffset: game::tail(zge::solveBoneOffset, constraint); break;
        default: break;
        }
    }
    READABLE_FUNCTION(solveConstraint, Verified);

    // averagePointSolve: the end of a round: the point at the weighted average of
    // what was asked of it. The sum carries on into the next round.
    void averagePointSolve(ee::Ptr<AnimatePoint> point)
    {
        if (point->kind != kPointHeld)
            point->solved = point->solvedSum * (ee::Float(1.0f) / point->solvedWeight);
    }
    READABLE_FUNCTION(averagePointSolve, Verified);

    // collidePoint
    void collidePoint(ee::Ptr<AnimatePoint> point) { game::tail(zge::pushPointOutOfPlanes, point); }

    namespace
    {
        // ZGE4's level collision (y is down): the sweep starts this far above where the point was, and a point
        // that hits stops this far above the hit.
        constexpr ee::Float kSweepStartAbove{0.05f};
        constexpr ee::Float kStopAboveHit{0.01f};
    }

    // collidePointWithLevel: ZGE4's collidePoint. A point whose collision is off stays as it is. With
    // withLevel, the line it moved along this step (from a little above where it was) is swept through the
    // level, and a hit puts it just above the first face hit, each axis it keeps excepted; then it leaves
    // the planes as ZGE3's does.
    void collidePointWithLevel(ee::Ptr<AnimatePoint> point, uint32_t withLevel)
    {
        ee::Local<QueryResults> hits;
        ee::Local<QuerySegment> sweep;
        ee::Local<Vec4> hit;
        if (!point->collides())
            return;
        if (withLevel)
        {
            sweep->start = point->lastPosition;
            sweep->end = point->position;
            if (!point->keepsAxis(1))
                sweep->start.y = sweep->start.y - kSweepStartAbove;
            if (collisionQuery(static_cast<uint32_t>(CollisionKind::Segment), sweep, kQueryWorldPoints, 0, 0, 0, hits, 0))
            {
                *hit = hits->nodes[0]->faces->point;
                hit->y = hit->y - kStopAboveHit;
                if (!point->keepsAxis(0))
                    point->position.x = hit->x;
                if (!point->keepsAxis(1))
                    point->position.y = hit->y;
                if (!point->keepsAxis(2))
                    point->position.z = hit->z;
            }
        }
        pushPointOutOfPlanes(point);
    }
    READABLE_FUNCTION_OWN(collidePoint, Verified, bmx, collidePointWithLevel);

    // alignBonesToPoints: each pivot's bone turned in its parent's axes from its
    // rest direction to the line between its points. A pivot on a pivot's bone
    // goes by the rotation that one has just got, not the bone's old one.
    void alignBonesToPoints(ee::Ptr<AnimateSystem> system)
    {
        ee::Local<Vec4> rest;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> axis;
        ee::Local<Matrix> parentMatrix;
        ee::Local<Matrix> parentInverse;
        ee::Local<Matrix> boneMatrix;
        ee::Local<Quaternion> turn;
        ee::Local<Quaternion> parentRotation;
        ee::Local<Quaternion> inverseRotation;
        for (ee::Ptr<AnimatePivot> pivot = system->pivots; pivot; pivot = pivot->next)
        {
            if (!pivot->parent)
            {
                objectBoneMatrix(system->object, pivot->parentBone, parentMatrix, kBoneInWorld);
                matrixToQuaternion(parentRotation, parentMatrix);
            }
            else
            {
                quaternionCopy(parentRotation, &pivot->parent->rotation);
                quaternionToMatrix(parentMatrix, parentRotation);
            }
            objectBoneMatrix(system->object, pivot->bone, boneMatrix, kBoneInWorld);
            matrixToQuaternion(&pivot->rotation, boneMatrix);
            quaternionInverse(inverseRotation, parentRotation);
            quaternionToMatrix(parentInverse, inverseRotation);

            rest = pivot->restDirection;
            direction = pivot->to->position - pivot->from->position;
            direction = rotated(parentInverse, direction);
            rest = unit(rest);
            direction = unit(direction);
            axis = cross(rest, direction);
            if (kNotQuiteParallel < dot(axis, axis))
            {
                const ee::Float sine = vec3Length(&axis->xyz());
                const ee::Float cosine = dot(rest, direction);
                const Angle angle = static_cast<short>(angleAtan2(sine, cosine));
                axis = unit(axis);
                quaternionFromAxisAngle(turn, &axis->xyz(), angle);
            }
            else
                quaternionIdentity(turn);
            quaternionMultiply(&pivot->rotation, parentRotation, turn);
            setBoneRotation(pivot->channel, turn);
        }
    }

    ee::Ptr<SceneObject> attachedParent(ee::Ptr<SceneObject> object); // (scene_objects.cpp)

    namespace
    {
        // A matrix row's three axes (pivotFrame leaves its w).
        void setAxes(Vec4 &row, const Vec4 &axes)
        {
            row.x = axes.x;
            row.y = axes.y;
            row.z = axes.z;
        }
    }

    // pivotFrame (ZGE4 only): the frame of a pivot's three points in out's three axis rows: along (from -> to),
    // across (from -> the third, made square to along) and their normal (along x across), each negated as the
    // kind word's top byte says, in the rows the kind's axis order gives. The translation row and the w lanes
    // are left as they are; the sixth order leaves the third row's z too (the bug kept).
    inline void pivotFrame(ee::Ptr<Matrix> out, ee::Ptr<AnimatePivot> pivot)
    {
        ee::Local<Vec4> along;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> across;
        along = pivot->to->position - pivot->from->position;
        along = unit(along);
        across = pivot->thirdPoint()->position - pivot->from->position;
        across = unit(across);
        normal = cross(along, across);
        normal = unit(normal);
        across = cross(normal, along);
        across = unit(across);
        const uint32_t negated = pivot->kindWord();
        if (negated & kNegateAlong)
            vec3Negate(&along->xyz(), &along->xyz());
        if (negated & kNegateNormal)
            vec3Negate(&normal->xyz(), &normal->xyz());
        if (negated & kNegateAcross)
            vec3Negate(&across->xyz(), &across->xyz());
        Matrix &frame = *out;
        switch (pivot->kindWord() & kPivotKindMask)
        {
        case 1: setAxes(frame.row(0), along); setAxes(frame.row(1), across); setAxes(frame.row(2), normal); break;
        case 2: setAxes(frame.row(0), along); setAxes(frame.row(1), normal); setAxes(frame.row(2), across); break;
        case 3: setAxes(frame.row(1), along); setAxes(frame.row(2), across); setAxes(frame.row(0), normal); break;
        case 4: setAxes(frame.row(1), along); setAxes(frame.row(0), across); setAxes(frame.row(2), normal); break;
        case 5: setAxes(frame.row(2), along); setAxes(frame.row(1), across); setAxes(frame.row(0), normal); break;
        case 6:
            frame.row(2).x = along->x;
            frame.row(2).y = along->y;
            setAxes(frame.row(0), across);
            setAxes(frame.row(1), normal);
            break;
        default: break;
        }
    }
    READABLE_FUNCTION_ONLY(bmx, pivotFrame);

    // alignBonesByPivotKind: ZGE4's alignBonesToPoints. Each pivot's parent frame is ZGE3's (its parent pivot's
    // rotation, else its parent bone's), but a pivot on a root bone takes the object's: the from point's bone in
    // its own object while this object is attached to another, else this object's orientation. Then the pivot's
    // kind: ZGE3's turn to the line (the inverse's matrix made from the identity), its points' frame turned by its
    // offset, or its parent's turn.
    void alignBonesByPivotKind(ee::Ptr<AnimateSystem> system)
    {
        ee::Local<Vec4> rest;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> axis;
        ee::Local<Matrix> parentMatrix;
        ee::Local<Matrix> parentInverse;
        ee::Local<Matrix> boneMatrix;
        ee::Local<Quaternion> turn;
        ee::Local<Quaternion> parentRotation;
        ee::Local<Quaternion> inverseRotation;
        for (ee::Ptr<AnimatePivot> pivot = system->pivots; pivot; pivot = pivot->next)
        {
            if (!pivot->onRootBone())
            {
                if (!pivot->parent)
                {
                    objectBoneMatrix(system->object, pivot->parentBone, parentMatrix, kBoneInWorld);
                    matrixToQuaternion(parentRotation, parentMatrix);
                }
                else
                {
                    quaternionCopy(parentRotation, &pivot->parent->rotation);
                    quaternionToMatrix(parentMatrix, parentRotation);
                }
            }
            else if (attachedParent(system->object))
            {
                const ee::Ptr<AnimatePoint> from = pivot->from;
                objectBoneMatrix(from->ownObject(), from->bone, parentMatrix, kBoneInWorld);
                matrixToQuaternion(parentRotation, parentMatrix);
            }
            else
            {
                quaternionCopy(parentRotation, objectOrientation(system->object));
                quaternionToMatrix(parentMatrix, parentRotation);
            }
            quaternionInverse(inverseRotation, parentRotation);

            const uint32_t kind = pivot->kindWord();
            if (kind == kPivotFollowLine)
            {
                objectBoneMatrix(system->object, pivot->bone, boneMatrix, kBoneInWorld);
                matrixToQuaternion(&pivot->rotation, boneMatrix);
                matrixIdentity(parentInverse);
                quaternionToMatrix(parentInverse, inverseRotation);
                rest = pivot->restDirection;
                direction = pivot->to->position - pivot->from->position;
                direction = rotated(parentInverse, direction);
                rest = unit(rest);
                direction = unit(direction);
                axis = cross(rest, direction);
                if (kNotQuiteParallel < dot(axis, axis))
                {
                    const ee::Float sine = vec3Length(&axis->xyz());
                    const ee::Float cosine = dot(rest, direction);
                    const Angle angle = static_cast<short>(angleAtan2(sine, cosine));
                    axis = unit(axis);
                    quaternionFromAxisAngle(turn, &axis->xyz(), angle);
                }
                else
                    quaternionIdentity(turn);
                quaternionMultiply(&pivot->rotation, parentRotation, turn);
                setBoneRotation(pivot->channel, turn);
            }
            else if (kind - kPivotFrameFirst <= kPivotFrameLast - kPivotFrameFirst)
            {
                // (The bone's matrix is made into the parent's and not read.)
                objectBoneMatrix(system->object, pivot->bone, parentMatrix, kBoneInWorld);
                pivotFrame(boneMatrix, pivot);
                matrixToQuaternion(turn, boneMatrix);
                quaternionMultiply(&pivot->rotation, turn, pivot->frameOffsetOf());
                quaternionMultiply(turn, inverseRotation, &pivot->rotation);
                setBoneRotation(pivot->channel, turn);
            }
            else if (kind == kPivotFollowParent)
            {
                quaternionCopy(&pivot->rotation, parentRotation);
                quaternionIdentity(turn);
                setBoneRotation(pivot->channel, turn);
            }
        }
    }
    READABLE_FUNCTION_OWN(alignBonesToPoints, Verified, bmx, alignBonesByPivotKind);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Ptr<AnimateSystem> block) { ee::tailCall<void>(sym::heapFree, block); }
        }
    }

    // freeAnimateSystem
    void freeAnimateSystem(ee::Ptr<AnimateSystem> system)
    {
        freeAnimatePoints(system->points);
        freeAnimatePivots(system->pivots);
        freeAnimateConstraints(system->constraints);
        freeAnimatePlaneGroups(system->planeGroups);
        freeAnimateSpheres(system->spheres);
        freeAnimatePulls(system->pulls);
        tail::heapFree(system);
    }
    READABLE_FUNCTION_WAITS(freeAnimateSystem, Verified);

    // addAnimatePlaneGroup
    ee::Ptr<AnimatePlaneGroup> addAnimatePlaneGroup(ee::Ptr<AnimateSystem> system)
    {
        const uint32_t memory = heapAllocate(sizeof(AnimatePlaneGroup));
        const ee::Ptr<AnimatePlaneGroup> group(memoryFill(memory, 0, sizeof(AnimatePlaneGroup)));
        if (group)
            group->next = system->planeGroups;
        system->planeGroups = group;
        return group;
    }
    READABLE_FUNCTION_WAITS(addAnimatePlaneGroup, Verified);

    // newAnimatePlane: a plane of a group, in the bone's axes. updateAnimatePlane
    // gives it its world place.
    ee::Ptr<AnimatePlane> newAnimatePlane(ee::Ptr<AnimatePlaneGroup> group, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> place,
                                          ee::Ptr<const Vec4> normal)
    {
        const uint32_t memory = heapAllocate(sizeof(AnimatePlane));
        const ee::Ptr<AnimatePlane> plane(memoryFill(memory, 0, sizeof(AnimatePlane)));
        if (plane)
        {
            plane->place = quadwordOf(place);
            plane->normal = quadwordOf(normal);
            plane->bone = bone;
            plane->next = group->planes;
        }
        group->planes = plane;
        plane->ignoredCount = 0;
        return plane;
    }
    READABLE_FUNCTION_WAITS(newAnimatePlane, Verified);

    // newBonePivot: a pivot turns `bone` to follow from -> to, which point
    // along its rest direction now.
    ee::Ptr<AnimatePivot> newBonePivot(ee::Ptr<AnimateSystem> system, ee::Ptr<SceneBone> bone, ee::Ptr<AnimatePoint> from,
                                       ee::Ptr<AnimatePoint> to)
    {
        ee::Local<Matrix> parentMatrix;
        ee::Local<Vec4> line;
        const uint32_t memory = heapAllocate(sizeof(AnimatePivot));
        const ee::Ptr<AnimatePivot> pivot(memoryFill(memory, 0, sizeof(AnimatePivot)));
        pivot->from = from;
        pivot->to = to;
        const ee::Ptr<SceneBone> parent = boneParent(system->object->scene, bone);
        if (!parent)
            pivot->restDirection = to->position - from->position;
        else
        {
            line = to->position - from->position;
            objectBoneMatrix(system->object, parent, parentMatrix, kBoneInScene);
            pivot->restDirection = rotated(parentMatrix, line);
        }
        pivot->parentBone = parent;
        pivot->bone = bone;
        pivot->next = system->pivots;
        const ee::Ptr<SceneObject> object = system->object;
        const uint32_t boneId = zge::boneId(object->scene, bone);
        pivot->channel = findBoneChannel(object->animator, boneId);
        system->pivots = pivot;
        ++system->pivotCount;
        return pivot;
    }
    READABLE_FUNCTION_WAITS(newBonePivot, Verified);

    // newAnimatePull
    ee::Ptr<AnimatePull> newAnimatePull(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second,
                                        ee::Float strength, ee::Float damping)
    {
        const uint32_t memory = heapAllocate(sizeof(AnimatePull));
        const ee::Ptr<AnimatePull> pull(memoryFill(memory, 0, sizeof(AnimatePull)));
        pull->first = first;
        pull->second = second;
        pull->strength = strength;
        pull->damping = damping;
        pull->next = system->pulls;
        system->pulls = pull;
        return pull;
    }
    READABLE_FUNCTION_WAITS(newAnimatePull, Verified);

    // newAnimateSystem
    ee::Ptr<AnimateSystem> newAnimateSystem(ee::Ptr<SceneObject> object)
    {
        const uint32_t memory = heapAllocate(sizeof(AnimateSystem));
        const ee::Ptr<AnimateSystem> system(memoryFill(memory, 0, sizeof(AnimateSystem)));
        system->object = object;
        system->setUnit();
        return system;
    }
    READABLE_FUNCTION_WAITS(newAnimateSystem, Verified);

    // newBoneConstraint: kConstraintBoneOffset. The offset is how the points lie
    // now, in the bone's axes. A record is taken before the bone is looked at,
    // so bone 0 leaks one.
    ee::Ptr<AnimateConstraint> newBoneConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second,
                                                 ee::Ptr<SceneBone> bone, ee::Float stiffness)
    {
        ee::Local<Matrix> inverse;
        ee::Local<Matrix> boneMatrix;
        ee::Local<Vec4> line;
        const uint32_t memory = heapAllocate(sizeof(AnimateConstraint));
        const ee::Ptr<AnimateConstraint> constraint(memoryFill(memory, 0, sizeof(AnimateConstraint)));
        if (!bone)
            return ee::Ptr<AnimateConstraint>();
        line = second->position - first->position;
        objectBoneMatrix(system->object, bone, boneMatrix, kBoneInScene);
        matrixTranspose3(inverse, boneMatrix);
        constraint->offset = rotated(inverse, line);
        constraint->first = first;
        constraint->kind = kConstraintBoneOffset;
        constraint->second = second;
        constraint->stiffness = stiffness;
        constraint->bone = bone;
        constraint->setObjectOf(*first);
        constraint->next = system->constraints;
        system->constraints = constraint;
        ++system->constraintCount;
        return constraint;
    }
    READABLE_FUNCTION_WAITS(newBoneConstraint, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Ptr<Vec4> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // newAnimateBoneSpace: the point in the bone's axes, in a new record on the
    // system's list. The offset's w is whatever the last vector operation left,
    // the bug kept. A record is taken before the bone is looked at, so bone 0
    // leaks one. (rig.cpp declares this function: its integer parameters stay.)
    uint32_t newAnimateBoneSpace(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> point, ee::Ptr<SceneBone> bone, ee::Float stiffness)
    {
        ee::Local<Vec4> relative;
        ee::Local<Matrix> inverse;
        ee::Local<Matrix> boneMatrix;
        memset(relative, 0, sizeof(Vec4));
        const uint32_t memory = heapAllocate(sizeof(AnimateConstraint));
        const uint32_t recordAddress = memoryFill(memory, 0, sizeof(AnimateConstraint));
        if (!bone)
            return 0;
        if constexpr (game::readable::kIsBmx) // ZGE4 takes the bone on the point's own object
            objectBoneMatrix(point->ownObject(), bone, boneMatrix, kBoneInScene);
        else
            objectBoneMatrix(system->object, bone, boneMatrix, kBoneInScene);

        // The point relative to the bone's position (the origin through the
        // matrix), turned into the bone's axes.
        relative = transformed(boneMatrix, relative);
        relative = point->position - relative;
        if constexpr (game::readable::kIsBmx) // ZGE4 starts the inverse from the identity
            matrixIdentity(inverse);
        matrixTranspose3(inverse, boneMatrix);
        const ee::Ptr<AnimateConstraint> record(recordAddress);
        record->offset = rotated(inverse, relative);
        record->first = point;
        record->kind = kConstraintBonePoint;
        record->stiffness = stiffness;
        if constexpr (game::readable::kIsBmx) // ZGE4 keeps the point's object in the record
            record->setObjectOf(*point);
        record->bone = bone;
        record->next = system->constraints;
        system->constraints = record;
        ++system->constraintCount;
        return recordAddress;
    }
    READABLE_FUNCTION_VU0(newAnimateBoneSpace, Verified);

    // newDistanceConstraint: kConstraintDistance, the rest length how far apart the
    // points are now.
    ee::Ptr<AnimateConstraint> newDistanceConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first, ee::Ptr<AnimatePoint> second,
                                                     ee::Float stiffness)
    {
        ee::Local<Vec4> apart;
        const uint32_t memory = heapAllocate(sizeof(AnimateConstraint));
        const ee::Ptr<AnimateConstraint> constraint(memoryFill(memory, 0, sizeof(AnimateConstraint)));
        apart = first->position - second->position;
        const ee::Float length = vec3Length(&apart->xyz());
        constraint->first = first;
        constraint->second = second;
        constraint->stiffness = stiffness;
        constraint->restLength = length;
        constraint->kind = kConstraintDistance;
        constraint->next = system->constraints;
        system->constraints = constraint;
        ++system->constraintCount;
        return constraint;
    }
    READABLE_FUNCTION_WAITS(newDistanceConstraint, Verified);

    // newBoneAttachedPoint: a point of `mass` at `place` in the bone.
    ee::Ptr<AnimatePoint> newBoneAttachedPoint(ee::Ptr<AnimateSystem> system, ee::Ptr<SceneBone> bone, int32_t kind, ee::Ptr<const Vec4> place,
                                               uint32_t id, ee::Float mass)
    {
        const uint32_t memory = heapAllocate(sizeof(AnimatePoint));
        const ee::Ptr<AnimatePoint> point(memoryFill(memory, 0, sizeof(AnimatePoint)));
        objectBonePoint(system->object, bone, place, &point->position, kBoneInScene);
        ee::Float inverseMass(0.0f);
        point->kind = kind;
        point->id = id;
        point->mass = mass;
        if (inverseMass < mass)
            inverseMass = ee::Float(1.0f) / mass;
        point->inverseMass = inverseMass;
        point->bone = bone;
        point->next = system->points;
        point->setObject(system->object);
        system->points = point;
        point->boneOffset = quadwordOf(place);
        ++system->pointCount;
        return point;
    }
    READABLE_FUNCTION_WAITS(newBoneAttachedPoint, Verified);

    // placeAnimateSphere: a sphere's centre from its bone. No route: only the dead
    // newAnimateDriver makes a sphere.
    void placeAnimateSphere(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimateSphere> sphere)
    {
        game::tail(objectBonePoint, system->object, sphere->bone, &sphere->boneCentre, &sphere->centre, kBoneInWorld);
    }
    READABLE_FUNCTION(placeAnimateSphere, Draft);

    // pushPointsOutOfSphere: every free point inside the sphere goes out to its
    // surface, the one on its centre stays. Like placeAnimateSphere, no route.
    void pushPointsOutOfSphere(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimateSphere> sphere)
    {
        ee::Local<Vec4> out;
        ee::Local<Vec4> fromCentre;
        const ee::Float radiusSquared = sphere->radius * sphere->radius;
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
        {
            if (point->kind == kPointHeld)
                continue;
            fromCentre = point->position - sphere->centre;
            const ee::Float squared = dot(fromCentre, fromCentre);
            if (squared < radiusSquared && kPointOnCentre < squared)
            {
                out = unit(fromCentre);
                out = *out * sphere->radius;
                point->position = sphere->centre + out;
            }
        }
    }
    READABLE_FUNCTION(pushPointsOutOfSphere, Draft);

    // applyPullToPoint: the spring's force on `point`, towards `other`, less the
    // damping of its speed, into its velocity. A held point takes none.
    void applyPullToPoint(ee::Ptr<AnimatePull> pull, ee::Ptr<AnimatePoint> point, ee::Ptr<AnimatePoint> other)
    {
        ee::Local<Vec4> apart;
        ee::Local<Vec4> spring;
        ee::Local<Vec4> drag;
        ee::Local<Vec4> speedGained;
        apart = other->position - point->position;
        if (point->kind == kPointHeld)
            return;
        spring = *apart * pull->strength;
        drag = point->velocity * -pull->damping;
        speedGained = spring + drag;
        speedGained = *speedGained * (*g_animateStep / point->mass);
        point->velocity += speedGained;
    }
    READABLE_FUNCTION(applyPullToPoint, Verified);

    // applyAnimatePull
    void applyAnimatePull(ee::Ptr<AnimatePull> pull)
    {
        applyPullToPoint(pull, pull->first, pull->second);
        game::tail(zge::applyPullToPoint, pull, pull->second, pull->first);
    }
    READABLE_FUNCTION(applyAnimatePull, Verified);

    // animate: a step of the system: the planes and spheres follow their
    // bones, pulls and forces move the points, the constraints have their
    // rounds, then points leave the planes and the pivots turn their bones.
    // updateObjectPose: skaterSway's first system of the frame.
    void animate(ee::Ptr<AnimateSystem> system, uint32_t updateObjectPose READABLE_PARAM_ONLY(bmx, uint32_t pivotsSkipped = 0), ee::Float step)
    {
        const ee::Ptr<SceneObject> object = system->object;
        const ee::Ptr<Animator> animator = object->animator;
        *g_animateStep = step;
        *g_animateSystem = system;
        if (updateObjectPose)
            updatePose(object);
        markPoseStale(system->object);
        if (animator)
            animator->changes |= kPoseChangesPending;

        for (ee::Ptr<AnimatePlaneGroup> group = system->planeGroups; group; group = group->next)
            for (ee::Ptr<AnimatePlane> plane = group->planes; plane; plane = plane->next)
                updateAnimatePlane(plane);
        for (ee::Ptr<AnimateSphere> sphere = system->spheres; sphere; sphere = sphere->next)
            placeAnimateSphere(system, sphere);
        for (ee::Ptr<AnimatePull> pull = system->pulls; pull; pull = pull->next)
            applyAnimatePull(pull);
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            integratePointMotion(point);
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            startPointSolve(point);

        for (uint32_t round = 0; round < *g_animateRounds; ++round)
        {
            for (ee::Ptr<AnimateConstraint> constraint = system->constraints; constraint; constraint = constraint->next)
                solveConstraint(constraint);
            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                averagePointSolve(point);
        }

        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            finishPointSolve(point);
        for (ee::Ptr<AnimateSphere> sphere = system->spheres; sphere; sphere = sphere->next)
            pushPointsOutOfSphere(system, sphere);
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            if (point->kind != kPointHeld)
                collidePoint(point);
        game::tail(zge::alignBonesToPoints, system);
    }

    // animateSystemChain: ZGE4's animate: every system on the chain from this one (AnimateSystem::next) gets
    // animate's step, each posed (updateObjectPose for the first, always for the rest), with these differences:
    // the points first remember where they were (ZGE3's integratePointMotion does that), the constraints get the
    // system's share of the rounds, the free points collide with the level when the system asks it, and the
    // pivots turn their bones unless skipPivots (the callers' sub-steps before the last). The step and the
    // system the solvers read are set once, the first's, and so is the animator each system marks.
    void animateSystemChain(ee::Ptr<AnimateSystem> system, uint32_t updateObjectPose, uint32_t skipPivots, ee::Float step)
    {
        const ee::Ptr<Animator> animator = system->object->animator;
        *g_animateStep = step;
        *g_animateSystem = system;
        for (; system; system = system->nextInChain())
        {
            if (updateObjectPose)
                updatePose(system->object);
            markPoseStale(system->object);
            if (animator)
                animator->changes |= kPoseChangesPending;

            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                point->lastPosition = point->position;
            for (ee::Ptr<AnimatePlaneGroup> group = system->planeGroups; group; group = group->next)
                for (ee::Ptr<AnimatePlane> plane = group->planes; plane; plane = plane->next)
                    updateAnimatePlane(plane);
            for (ee::Ptr<AnimateSphere> sphere = system->spheres; sphere; sphere = sphere->next)
                placeAnimateSphere(system, sphere);
            for (ee::Ptr<AnimatePull> pull = system->pulls; pull; pull = pull->next)
                applyAnimatePull(pull);
            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                integratePointMotion(point);
            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                startPointSolve(point);

            const uint32_t rounds = system->roundsOf(*g_animateRounds);
            for (uint32_t round = 0; round < rounds; ++round)
            {
                for (ee::Ptr<AnimateConstraint> constraint = system->constraints; constraint; constraint = constraint->next)
                    solveConstraint(constraint);
                for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                    averagePointSolve(point);
            }

            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                finishPointSolve(point);
            for (ee::Ptr<AnimateSphere> sphere = system->spheres; sphere; sphere = sphere->next)
                pushPointsOutOfSphere(system, sphere);
            for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                if (point->kind != kPointHeld)
                    collidePointWithLevel(point, system->levelCollision());
            if (!skipPivots)
                alignBonesToPoints(system);
            updateObjectPose = 1; // the systems after the first are always posed
        }
    }
    READABLE_FUNCTION_WAITS_OWN(animate, Verified, bmx, animateSystemChain);

    // resetAnimatePoints: every point back at its place on its bone, still.
    namespace
    {
        // ZGE4's reset: every system on the chain from this one (AnimateSystem::next), each point placed on its own
        // object (AnimatePoint::object). False where the systems are not chained (ZGE3). (A template, so that the
        // chain's link is named only in ZGE4's build.)
        template <typename System = AnimateSystem>
        bool resetChainedAnimatePoints(ee::Ptr<System> system)
        {
            if constexpr (!game::readable::kIsBmx)
                return false;
            else
            {
                for (; system; system = system->next)
                    for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
                    {
                        point->velocity = kZero;
                        objectBonePoint(point->objectOr(system->object), point->bone, &point->boneOffset, &point->position, kBoneInWorld);
                    }
                return true;
            }
        }
    }

    void resetAnimatePoints(ee::Ptr<AnimateSystem> system)
    {
        if (resetChainedAnimatePoints(system))
            return;
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
        {
            point->velocity = kZero;
            objectBonePoint(system->object, point->bone, &point->boneOffset, &point->position, kBoneInWorld);
        }
    }
    READABLE_FUNCTION(resetAnimatePoints, Verified);

    // sortAnimatePivots: the pivots in order, a pivot on a bone before those on its
    // children (swapping what the records hold, not the records), each with
    // its parent's pivot.
    void sortAnimatePivots(ee::Ptr<AnimateSystem> system)
    {
        ee::Local<AnimatePivot> held;
        bool swapped;
        do
        {
            swapped = false;
            for (ee::Ptr<AnimatePivot> pivot = system->pivots; pivot; pivot = pivot->next)
            {
                ee::Ptr<AnimatePivot> other = pivot;
                while (other && !same(pivot->parentBone, other->bone))
                    other = other->next;
                if (!other)
                    continue;
                if constexpr (game::readable::kIsBmx)
                {
                    // ZGE4 swaps the whole records, each putting its own link back.
                    memoryCopy(held, pivot, kPivotCopyBytes);
                    const ee::Ptr<AnimatePivot> pivotNext = pivot->next;
                    memoryCopy(pivot, other, kPivotCopyBytes);
                    pivot->next = pivotNext;
                    const ee::Ptr<AnimatePivot> otherNext = other->next;
                    memoryCopy(other, held, kPivotCopyBytes);
                    other->next = otherNext;
                    swapped = true;
                }
                else
                {
                    memoryCopy(held, pivot, kPivotCopyBytes);
                    memoryCopy(pivot, other, kPivotCopyBytes);
                    memoryCopy(other, held, kPivotCopyBytes);
                    swapped = true;
                    // Each record keeps its place on the list.
                    other->next = pivot->next;
                    pivot->next = held->next;
                }
            }
        } while (swapped);

        for (ee::Ptr<AnimatePivot> pivot = system->pivots; pivot; pivot = pivot->next)
        {
            pivot->parent = ee::Ptr<AnimatePivot>();
            for (ee::Ptr<AnimatePivot> candidate = system->pivots; !same(candidate, pivot); candidate = candidate->next)
            {
                if (same(pivot->parentBone, candidate->bone))
                {
                    pivot->parent = candidate;
                    break;
                }
            }
        }
    }
    READABLE_FUNCTION(sortAnimatePivots, Verified);

    // setAnimateSystemCollision: ZGE4 only: whether the system's free points collide with the level too
    // (collidePointWithLevel). One of BMX XXX's rigs sets it.
    inline void setAnimateSystemCollision(ee::Ptr<AnimateSystem> system, uint32_t on)
    {
        system->setLevelCollision(on);
    }
    READABLE_FUNCTION_ONLY(bmx, setAnimateSystemCollision);

    int32_t resetAnimatorPose(ee::Ptr<SceneObject> object); // (animation.cpp)

    namespace
    {
        // ZGE4's body rigs (BodyRig), made when the first is asked for.
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_bodyRigPool> g_bodyRigPool;

        // The limbs' rig built on the object as it stands: the upper arms and upper legs held to their bones, the
        // lower arms, lower legs, hands and feet free and kept in length, pivots for the eight limb bones, planes
        // around the hips, and the level's collision on. The original's, by address.
        void buildLimbRig(ee::Ptr<SceneObject> object, ee::Ptr<BodyRig> rig) { ee::game<void>(sym::bmx::buildLimbRig, object, rig); }
    }

    // newLimbRig: ZGE4 only: a body rig of the object's limbs. The rig is built on the model at rest and upright:
    // the animator's pose is reset and the object turned to no rotation for the build, then turned back (its
    // animator stays reset).
    inline ee::Ptr<BodyRig> newLimbRig(ee::Ptr<SceneObject> object)
    {
        if (!*g_bodyRigPool)
            *g_bodyRigPool = ee::Ptr<Pool>(createPool(sizeof(BodyRig), 1, 0));
        const ee::Ptr<BodyRig> rig(poolAlloc(*g_bodyRigPool));
        ee::Local<Quaternion> upright;
        ee::Local<Quaternion> turned;
        quaternionIdentity(upright);
        quaternionCopy(turned, objectOrientation(object));
        resetAnimatorPose(object);
        setObjectOrientation(object, upright);
        updatePose(object);
        buildLimbRig(object, rig);
        setObjectOrientation(object, turned);
        return rig;
    }
    READABLE_FUNCTION_ONLY(bmx, newLimbRig);

    namespace
    {
        // The whole body's rigs built on the object as it stands: every arm and leg bone with its elbow and knee,
        // and the one with no elbow or knee bones. The originals', by address.
        void buildWholeBodyRig(ee::Ptr<SceneObject> object, ee::Ptr<BodyRig> rig) { ee::game<void>(sym::bmx::buildWholeBodyRig, object, rig); }
        void buildWholeBodyRigWithoutJoints(ee::Ptr<SceneObject> object, ee::Ptr<BodyRig> rig)
        {
            ee::game<void>(sym::bmx::buildWholeBodyRigWithoutJoints, object, rig);
        }
        // The soft parts' five builders (SoftPartsRig's comment has each one's bones). The originals', by address.
        void buildShirtSystem(ee::Ptr<SoftPartsRig> rig, ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::buildShirtSystem, rig, object); }
        void buildHairSystem(ee::Ptr<SoftPartsRig> rig, ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::buildHairSystem, rig, object); }
        void buildBreastsSystem(ee::Ptr<SoftPartsRig> rig, ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::buildBreastsSystem, rig, object); }
        void buildSkirtSystem(ee::Ptr<SoftPartsRig> rig, ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::buildSkirtSystem, rig, object); }
        void buildNecklaceSystem(ee::Ptr<SoftPartsRig> rig, ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::buildNecklaceSystem, rig, object); }
    }

    // newWholeBodyRig, newWholeBodyRigWithoutJoints (ZGE4 only): newLimbRig's steps around the whole body's builders.
    inline ee::Ptr<BodyRig> newWholeBodyRig(ee::Ptr<SceneObject> object)
    {
        if (!*g_bodyRigPool)
            *g_bodyRigPool = ee::Ptr<Pool>(createPool(sizeof(BodyRig), 1, 0));
        const ee::Ptr<BodyRig> rig(poolAlloc(*g_bodyRigPool));
        ee::Local<Quaternion> upright;
        ee::Local<Quaternion> turned;
        quaternionIdentity(upright);
        quaternionCopy(turned, objectOrientation(object));
        resetAnimatorPose(object);
        setObjectOrientation(object, upright);
        updatePose(object);
        buildWholeBodyRig(object, rig);
        setObjectOrientation(object, turned);
        return rig;
    }
    READABLE_FUNCTION_ONLY(bmx, newWholeBodyRig);

    inline ee::Ptr<BodyRig> newWholeBodyRigWithoutJoints(ee::Ptr<SceneObject> object)
    {
        if (!*g_bodyRigPool)
            *g_bodyRigPool = ee::Ptr<Pool>(createPool(sizeof(BodyRig), 1, 0));
        const ee::Ptr<BodyRig> rig(poolAlloc(*g_bodyRigPool));
        ee::Local<Quaternion> upright;
        ee::Local<Quaternion> turned;
        quaternionIdentity(upright);
        quaternionCopy(turned, objectOrientation(object));
        resetAnimatorPose(object);
        setObjectOrientation(object, upright);
        updatePose(object);
        buildWholeBodyRigWithoutJoints(object, rig);
        setObjectOrientation(object, turned);
        return rig;
    }
    READABLE_FUNCTION_ONLY(bmx, newWholeBodyRigWithoutJoints);

    // stepBodyRig (ZGE4 only): a step of a rig that is stepping: the pivots' bone rotations cleared first unless
    // the rig keeps them, the system animated (the object's pose updated, the pivots turning their bones), then
    // drawn for debugging.
    inline void stepBodyRig(ee::Ptr<BodyRig> rig, ee::Float seconds)
    {
        if (rig->stepping == 0)
            return;
        if (rig->rotationsKept == 0)
            clearPivotRotations(rig->system);
        animate(rig->system, 1 READABLE_PARAM_ONLY(bmx, 0), seconds);
        game::tail(zge::drawAnimateSystem, rig->system);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, stepBodyRig);

    namespace
    {
        // ZGE4's soft parts rigs (SoftPartsRig), made as the body rigs' pool is.
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_softPartsRigPool> g_softPartsRigPool;

        // A rig handed back to its pool, and the pool destroyed with its last record.
        inline void giveBackRig(ee::Ptr<Pool> &pool, ee::Address rig)
        {
            poolFree(pool, rig);
            if (zge::poolFirst(ee::Ptr<PoolList>(&pool->used)) == 0)
            {
                destroyPool(pool);
                pool = ee::Ptr<Pool>();
            }
        }
    }

    // newOwnBoneConstraint (ZGE4 only): newBoneConstraint on the first point's own bone, with no offset from it.
    inline ee::Ptr<AnimateConstraint> newOwnBoneConstraint(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> first,
                                                           ee::Ptr<AnimatePoint> second, ee::Float stiffness)
    {
        const ee::Ptr<AnimateConstraint> constraint = newBoneConstraint(system, first, second, first->bone, stiffness);
        constraint->offset.x = ee::Float::fromBits(0);
        constraint->offset.y = ee::Float::fromBits(0);
        constraint->offset.z = ee::Float::fromBits(0);
        return constraint;
    }
    READABLE_FUNCTION_ONLY(bmx, newOwnBoneConstraint);

    // newSoftPartsRig (ZGE4 only): a cleared rig with the five systems the object's model has bones for, built on
    // the model at rest and upright as newLimbRig builds its own; not settling.
    inline ee::Ptr<SoftPartsRig> newSoftPartsRig(ee::Ptr<SceneObject> object)
    {
        if (!*g_softPartsRigPool)
            *g_softPartsRigPool = ee::Ptr<Pool>(createPool(sizeof(SoftPartsRig), 1, 0));
        const ee::Ptr<SoftPartsRig> rig(poolAlloc(*g_softPartsRigPool));
        memoryFill(rig, 0, sizeof(SoftPartsRig));
        ee::Local<Quaternion> turned;
        ee::Local<Quaternion> upright;
        quaternionIdentity(upright);
        quaternionCopy(turned, objectOrientation(object));
        resetAnimatorPose(object);
        setObjectOrientation(object, upright);
        updatePose(object);
        buildShirtSystem(rig, object);
        buildHairSystem(rig, object);
        buildBreastsSystem(rig, object);
        buildSkirtSystem(rig, object);
        buildNecklaceSystem(rig, object);
        setObjectOrientation(object, turned);
        rig->settleSteps = 0;
        return rig;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newSoftPartsRig);

    // clearPivotRotations (ZGE4 only): every pivot's bone back to no rotation in the animator, and the object's
    // pose worked out again from that.
    inline void clearPivotRotations(ee::Ptr<AnimateSystem> system)
    {
        ee::Local<Quaternion> none;
        ee::Ptr<AnimatePivot> pivot = system->pivots;
        quaternionIdentity(none);
        for (; pivot; pivot = pivot->next)
            setBoneRotation(pivot->channel, none);
        markPoseStale(system->object);
        updatePose(system->object);
    }
    READABLE_FUNCTION_ONLY(bmx, clearPivotRotations);

    // freeSoftPartsRig (ZGE4 only): the rig's systems freed and the rig given back.
    inline void freeSoftPartsRig(ee::Ptr<SoftPartsRig> rig)
    {
        if (!rig)
            return;
        if (rig->shirt)
            freeAnimateSystem(rig->shirt);
        if (rig->hair)
            freeAnimateSystem(rig->hair);
        if (rig->breasts)
            freeAnimateSystem(rig->breasts);
        if (rig->skirt)
            freeAnimateSystem(rig->skirt);
        if (rig->necklace)
            freeAnimateSystem(rig->necklace);
        giveBackRig(*g_softPartsRigPool, rig);
    }
    READABLE_FUNCTION_ONLY(bmx, freeSoftPartsRig);

    // resetSoftPartsRig (ZGE4 only): every system's points back on their bones, at rest; the shirt's three points'
    // places in the world kept for its step.
    inline void resetSoftPartsRig(ee::Ptr<SoftPartsRig> rig)
    {
        if (!rig)
            return;
        if (rig->shirt)
        {
            resetAnimatePoints(rig->shirt);
            constexpr uint32_t kPointOfPlace[3] = {1, 2, 0};
            for (uint32_t place = 0; place < 3; ++place)
            {
                const ee::Ptr<AnimatePoint> point = rig->shirtPoints[kPointOfPlace[place]];
                objectBonePoint(rig->shirt->object, point->bone, &point->boneOffset, &rig->shirtPlaces[place], kBoneInWorld);
            }
        }
        if (rig->hair)
            resetAnimatePoints(rig->hair);
        if (rig->breasts)
            resetAnimatePoints(rig->breasts);
        if (rig->skirt)
            resetAnimatePoints(rig->skirt);
        if (rig->necklace)
            game::tail(zge::resetAnimatePoints, rig->necklace);
    }
    READABLE_FUNCTION_ONLY(bmx, resetSoftPartsRig);

    namespace
    {
        // The shirt's flutter: how far its sine and cosine swing the push around 1, the most a point's squared
        // move since the last step counts for, a turn of the phase as the original has it (6.28), and a random
        // word's 31 bits as a fraction.
        constexpr float kShirtSwing = 0.9f;
        constexpr float kShirtMoveLimit = 0.04f;
        constexpr float kShirtTurn = 6.28f;
        constexpr float kRandomFraction = 4.656612873077393e-10f; // 2 to the -31
        constexpr uint32_t kRandomBits = 0x7FFFFFFFu;

        // A random fraction, 0 to 1.
        inline ee::Float randomFraction()
        {
            return ee::toFloat(static_cast<int32_t>(randomNumber() & kRandomBits)) * ee::Float(kRandomFraction);
        }

        // How far (squared) a shirt point is from where the last step left it, kShirtMoveLimit at most.
        inline ee::Float shirtPointMove(ee::Ptr<AnimatePoint> point, const Vec4 &place)
        {
            ee::Local<Vec4> moved;
            *moved = point->position - place;
            const ee::Float squared = dot(*moved, *moved);
            return squared < ee::Float(kShirtMoveLimit) ? dot(*moved, *moved) : ee::Float(kShirtMoveLimit);
        }
    }

    // stepSoftPartsRig (ZGE4 only): a step of the rig's systems, the first one animated updating the object's
    // pose. The breasts' two bones are then turned to three times their rotation; the shirt's three points are
    // pushed down by random amounts that swing with its phase and grow with how far each point last moved, and
    // their places kept. While the rig settles, each step ends by putting the points back (resetSoftPartsRig).
    inline void stepSoftPartsRig(ee::Ptr<SoftPartsRig> rig, ee::Float seconds)
    {
        if (!rig)
            return;
        uint32_t updateObjectPose = 1;
        if (rig->breasts)
        {
            animate(rig->breasts, 1 READABLE_PARAM_ONLY(bmx, static_cast<uint32_t>(rig->settleSteps)), seconds);
            updateObjectPose = 0;
            ee::Local<Quaternion> tripled;
            const ee::Ptr<AnimationChannel> channels[2] = {rig->breastLeftChannel, rig->breastRightChannel};
            for (const ee::Ptr<AnimationChannel> channel : channels)
            {
                const ee::Ptr<Quaternion> rotation = boneRotation(channel);
                quaternionMultiply(&*tripled, &*rotation, &*rotation);
                quaternionMultiply(&*tripled, &*tripled, &*rotation);
                setBoneRotation(channel, tripled);
            }
        }
        if (rig->hair)
        {
            animate(rig->hair, updateObjectPose READABLE_PARAM_ONLY(bmx, static_cast<uint32_t>(rig->settleSteps)), seconds);
            updateObjectPose = 0;
        }
        if (rig->shirt)
        {
            const ee::Float sine = angleSin(radiansToAngle(rig->shirtPhase));
            const ee::Float sway = sine * ee::Float(kShirtSwing) + ee::Float(1.0f);
            const ee::Float cosine = angleCos(radiansToAngle(rig->shirtPhase));
            const ee::Float gust = rig->shirtGust - ee::Float(0.5f);
            const ee::Float lift = cosine * ee::Float(kShirtSwing) + ee::Float(1.0f);
            const ee::Float swayed = sway * gust;
            const ee::Float lifted = lift * gust;
            const ee::Float moved1 = shirtPointMove(rig->shirtPoints[1], rig->shirtPlaces[0]);
            const ee::Float moved2 = shirtPointMove(rig->shirtPoints[2], rig->shirtPlaces[1]);
            const ee::Float moved0 = shirtPointMove(rig->shirtPoints[0], rig->shirtPlaces[2]);

            const ee::Float turn = seconds * (rig->shirtRate * ee::Float(kShirtTurn));
            const ee::Float liftedPush = lifted * rig->shirtPush;
            const ee::Float swayedPush = swayed * rig->shirtPush;
            const ee::Float turned = turn * moved0 / ee::Float(kShirtMoveLimit);
            const ee::Float push0 = swayedPush * moved0 / ee::Float(kShirtMoveLimit);
            const ee::Float push2 = liftedPush * moved2 / ee::Float(kShirtMoveLimit);
            const ee::Float push1 = liftedPush * moved1 / ee::Float(kShirtMoveLimit);
            rig->shirtPhase = rig->shirtPhase + (seconds + rig->shirtGust * turned);
            if (ee::Float(kShirtTurn) < rig->shirtPhase)
            {
                // A turn done: the phase back by one, and a new gust.
                const ee::Float draw = randomFraction() * ee::Float(0.5f);
                rig->shirtPhase = rig->shirtPhase - ee::Float(kShirtTurn);
                rig->shirtGust = draw + ee::Float(0.5f);
            }
            const ee::Float drop1 = randomFraction() * push1;
            rig->shirtPoints[1]->position.y = rig->shirtPoints[1]->position.y - drop1;
            const ee::Float drop2 = randomFraction() * push2;
            rig->shirtPoints[2]->position.y = rig->shirtPoints[2]->position.y - drop2;
            const ee::Float drop0 = randomFraction() * push0;
            rig->shirtPoints[0]->position.y = rig->shirtPoints[0]->position.y - drop0;
            rig->shirtPlaces[2] = rig->shirtPoints[0]->position;
            rig->shirtPlaces[1] = rig->shirtPoints[2]->position;
            rig->shirtPlaces[0] = rig->shirtPoints[1]->position;
            animate(rig->shirt, updateObjectPose READABLE_PARAM_ONLY(bmx, static_cast<uint32_t>(rig->settleSteps)), seconds);
            updateObjectPose = 0;
        }
        if (rig->skirt)
        {
            animate(rig->skirt, updateObjectPose READABLE_PARAM_ONLY(bmx, static_cast<uint32_t>(rig->settleSteps)), seconds);
            updateObjectPose = 0;
        }
        if (rig->necklace)
            animate(rig->necklace, updateObjectPose READABLE_PARAM_ONLY(bmx, static_cast<uint32_t>(rig->settleSteps)), seconds);
        if (rig->settleSteps != 0)
        {
            rig->settleSteps = rig->settleSteps - 1;
            resetSoftPartsRig(rig);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, stepSoftPartsRig);

    // clearSoftPartsRotations (ZGE4 only): clearPivotRotations for each of the rig's systems.
    inline void clearSoftPartsRotations(ee::Ptr<SoftPartsRig> rig)
    {
        if (!rig)
            return;
        if (rig->shirt)
            clearPivotRotations(rig->shirt);
        if (rig->hair)
            clearPivotRotations(rig->hair);
        if (rig->breasts)
            clearPivotRotations(rig->breasts);
        if (rig->skirt)
            clearPivotRotations(rig->skirt);
        if (rig->necklace)
            game::tail(zge::clearPivotRotations, rig->necklace);
    }
    READABLE_FUNCTION_ONLY(bmx, clearSoftPartsRotations);

    // settleSoftPartsRig (ZGE4 only): the rig's next `steps` steps settle it (SoftPartsRig::settleSteps).
    inline void settleSoftPartsRig(ee::Ptr<SoftPartsRig> rig, int32_t steps)
    {
        rig->settleSteps = steps;
    }
    READABLE_FUNCTION_ONLY(bmx, settleSoftPartsRig);

    // freeBodyRig (ZGE4 only): the rig's system freed and the rig given back.
    inline void freeBodyRig(ee::Ptr<BodyRig> rig)
    {
        if (!rig)
            return;
        freeAnimateSystem(rig->system);
        giveBackRig(*g_bodyRigPool, rig);
    }
    READABLE_FUNCTION_ONLY(bmx, freeBodyRig);

    // resetBodyRig (ZGE4 only): the rig's points back on their bones, at rest.
    inline void resetBodyRig(ee::Ptr<BodyRig> rig)
    {
        game::tail(zge::resetAnimatePoints, rig->system);
    }
    READABLE_FUNCTION_ONLY(bmx, resetBodyRig);

    // ---- the systems no level uses: spheres, anchors, the debug draw ---------

    // newAnimateSphere (was newAnimateDriver): a sphere of `radius` on the
    // bone, its centre at `centre` in the bone (the aligned quadword it is
    // in), at the front of the system's list.
    ee::Ptr<AnimateSphere> newAnimateSphere(ee::Ptr<AnimateSystem> system, ee::Ptr<SceneBone> bone, ee::Ptr<const Vec4> centre,
                                            ee::Float radius)
    {
        const ee::Ptr<AnimateSphere> sphere(memoryFill(heapAllocate(sizeof(AnimateSphere)), 0, sizeof(AnimateSphere)));
        if (sphere)
            sphere->next = system->spheres;
        system->spheres = sphere;
        sphere->boneCentre = quadwordOf(centre);
        sphere->bone = bone;
        sphere->radius = radius;
        return sphere;
    }
    READABLE_FUNCTION_WAITS(newAnimateSphere, Draft);

    // newAnimateAnchor: a constraint that holds the point at `place` (the
    // aligned quadword it is in), at the front of the system's list.
    ee::Ptr<AnimateConstraint> newAnimateAnchor(ee::Ptr<AnimateSystem> system, ee::Ptr<AnimatePoint> point, ee::Ptr<const Vec4> place,
                                                 ee::Float stiffness)
    {
        const ee::Ptr<AnimateConstraint> anchor(memoryFill(heapAllocate(sizeof(AnimateConstraint)), 0, sizeof(AnimateConstraint)));
        anchor->offset = quadwordOf(place);
        anchor->first = point;
        anchor->stiffness = stiffness;
        anchor->kind = kConstraintAnchor;
        anchor->next = system->constraints;
        system->constraints = anchor;
        ++system->constraintCount;
        return anchor;
    }
    READABLE_FUNCTION_WAITS(newAnimateAnchor, Draft);

    namespace
    {
        // The debug draw's colours (RGBA in memory) and sizes.
        constexpr uint32_t kDebugRed = sym::g_animateDebugRed;
        constexpr uint32_t kDebugBlue = sym::g_animateDebugBlue;
        constexpr uint32_t kDebugYellow = sym::g_animateDebugYellow; // alpha 0x1F
        constexpr uint32_t kDebugCyan = sym::g_animateDebugCyan;
        constexpr ee::Float kPointRadius = 0.05f;
        constexpr ee::Float kPlaceRadius = 0.025f;
        // ZGE4 marks the points of ids 1 and 5 (the ZPDN data's) a second time, in cyan and larger.
        constexpr uint32_t kMarkedPointId = 1, kMarkedPointId2 = 5;
        constexpr ee::Float kMarkedPointRadius = 0.07f;
        // A plane's normal this near the x axis (their dot product; ZGE4: its size above kNormalAlongX) takes
        // the y axis to build its square from.
        constexpr ee::Float kNormalOffX = 0.01f;
        constexpr ee::Float kNormalAlongX = 0.9f;
        constexpr ee::Float kHalfSide = 0.5f;
        // The flags the debug lists are given (bits 0 and 1; debugDrawFlags
        // keeps bit 1). ZGE4 gives its points, spheres and squares bit 0 alone.
        constexpr uint32_t kSphereFlags = game::readable::perGame(3u, 3u, 1u), kLineFlags = 1;
        constexpr uint32_t kTriangleFlags = game::readable::perGame(3u, 3u, 1u);

    }

    // drawAnimateSystem (debug): the points (red), each constraint (a cyan
    // line from its point to where it holds it; the places in blue), the
    // spheres (yellow) and each plane as a yellow square of side 1 on it.
    // Each plane's bone matrix is made but not used, from the object of the
    // system animate last ran, not this one's (ZGE4: the plane's own object, which it takes from that system the
    // first time). ZGE4 draws nothing for no system and marks the points of ids 1 and 5 again (cyan).
    void drawAnimateSystem(ee::Ptr<AnimateSystem> system)
    {
        if (game::readable::kIsBmx && !system)
            return;
        ee::Local<Vec4> held;          // a bone point constraint's place
        ee::Local<Matrix> heldMatrix;
        ee::Local<Vec4> secondPlace;   // a bone offset constraint's places
        ee::Local<Vec4> firstPlace;
        ee::Local<Vec4> turnedOffset;
        ee::Local<Matrix> offsetMatrix;
        ee::Local<Vec4> across;        // a plane's square: its two half sides
        ee::Local<Vec4> up;
        ee::Local<Vec4[4]> corners;
        ee::Local<Matrix> planeMatrix;
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
        {
            // (addDebugPoint's fourth argument is not read: 0 here, a leftover
            // register in the original.)
            addDebugPoint(&point->position, kDebugRed, kSphereFlags, 0, kPointRadius);
            if (game::readable::kIsBmx && (point->id == kMarkedPointId || point->id == kMarkedPointId2))
                addDebugPoint(&point->position, kDebugCyan, kSphereFlags, 0, kMarkedPointRadius);
        }
        for (ee::Ptr<AnimateConstraint> constraint = system->constraints; constraint; constraint = constraint->next)
        {
            switch (constraint->kind)
            {
            case kConstraintDistance:
                addDebugLine(&constraint->first->position, &constraint->second->position, kDebugCyan, kLineFlags);
                break;
            case kConstraintAnchor:
                addDebugPoint(&constraint->offset, kDebugBlue, kSphereFlags, 0, kPlaceRadius);
                addDebugLine(&constraint->first->position, &constraint->offset, kDebugCyan, kLineFlags);
                break;
            case kConstraintBonePoint:
                objectBoneMatrix(system->object, constraint->bone, heldMatrix, kBoneInWorld);
                *held = transformed(*heldMatrix, constraint->offset);
                addDebugPoint(held, kDebugBlue, kSphereFlags, 0, kPlaceRadius);
                addDebugLine(&constraint->first->position, held, kDebugCyan, kLineFlags);
                break;
            case kConstraintBoneOffset:
                objectBoneMatrix(system->object, constraint->bone, offsetMatrix, kBoneInWorld);
                *turnedOffset = rotated(*offsetMatrix, constraint->offset);
                *secondPlace = constraint->second->position - *turnedOffset;
                *firstPlace = constraint->first->position + *turnedOffset;
                addDebugPoint(secondPlace, kDebugBlue, kSphereFlags, 0, kPlaceRadius);
                addDebugPoint(firstPlace, kDebugBlue, kSphereFlags, 0, kPlaceRadius);
                addDebugLine(&constraint->first->position, &constraint->second->position, kDebugCyan, kLineFlags);
                break;
            default:
                break;
            }
        }
        for (ee::Ptr<AnimateSphere> sphere = system->spheres; sphere; sphere = sphere->next)
            addDebugPoint(&sphere->centre, kDebugYellow, kSphereFlags, 0, sphere->radius);
        for (ee::Ptr<AnimatePlaneGroup> group = system->planeGroups; group; group = group->next)
        {
            for (ee::Ptr<AnimatePlane> plane = group->planes; plane; plane = plane->next)
            {
                across->y = 0.0f;
                across->x = 1.0f;
                across->z = 0.0f;
                objectBoneMatrix(plane->objectOrTake([] { return (*g_animateSystem)->object; }), plane->bone, planeMatrix, kBoneInWorld);
                const ee::Float along = dot(*across, plane->worldNormal);
                if (game::readable::kIsBmx ? kNormalAlongX < ee::abs(along) : along < kNormalOffX)
                {
                    across->y = 1.0f;
                    across->x = 0.0f;
                }
                *up = cross(*across, plane->worldNormal);
                *across = cross(*up, plane->worldNormal);
                *across = unit(*across);
                *up = unit(*up);
                *across = *across * kHalfSide;
                *up = *up * kHalfSide;
                corners[0] = *across + *up;
                corners[1] = *across - *up;
                corners[2] = *up - *across;
                corners[3] = corners[0] * -1.0f;
                for (uint32_t corner = 0; corner < 4; ++corner)
                    corners[corner] = corners[corner] + plane->worldPlace;
                drawTriangle(&corners[0], &corners[1], &corners[2], kDebugYellow, kTriangleFlags);
                drawTriangle(&corners[1], &corners[2], &corners[3], kDebugYellow, kTriangleFlags);
            }
        }
    }
    READABLE_FUNCTION_WAITS(drawAnimateSystem, Draft);

    // ---- systems from ZPDN chunks (no level has one) --------------------------

    // A ZPDN chunk's system (20 bytes from g_zpdnChunkPool): its
    // sub-chunks' records, each list the newest first.
    struct ZpdnPoint;
    struct ZpdnPivot;
    struct ZpdnConstraint;
    struct ZpvlRecord;
    struct ZpdnRecord
    {
        ee::Ptr<uint32_t> id;                // +0x00 ZPDF: the id buildAnimateSystemFromZpdn looks for
        ee::Ptr<ZpdnPoint> points;           // +0x04 ZPPT
        ee::Ptr<ZpdnPivot> pivots;           // +0x08 ZPLM
        ee::Ptr<ZpdnConstraint> constraints; // +0x0C ZPCT
        ee::Ptr<ZpvlRecord> zpvl;            // +0x10 ZPVL: read, nothing uses them
    };
    static_assert(sizeof(ZpdnRecord) == 20);

    // A point (ZPPT, 0x20 bytes read): newBoneAttachedPoint's.
    struct ZpdnPoint
    {
        uint32_t id;         // +0x00 its AnimatePoint::id: pivots and constraints name points by it
        uint32_t boneHash;   // +0x04
        uint32_t _08;
        ee::Float mass;      // +0x0C
        uint32_t _10;
        ee::Float x, y, z;   // +0x14 its place in the bone
        ee::Ptr<ZpdnPoint> next; // +0x20
    };
    static_assert(offsetof(ZpdnPoint, mass) == 0x0C && offsetof(ZpdnPoint, x) == 0x14 && offsetof(ZpdnPoint, next) == 0x20);

    // A pivot (ZPLM, 0x10 bytes read): newBonePivot's.
    struct ZpdnPivot
    {
        uint32_t _00;
        uint32_t fromId, toId; // +0x04
        uint32_t boneHash;     // +0x0C
        ee::Ptr<ZpdnPivot> next; // +0x10
    };
    static_assert(offsetof(ZpdnPivot, boneHash) == 0x0C && offsetof(ZpdnPivot, next) == 0x10);

    // A constraint (ZPCT, 0x28 bytes read).
    struct ZpdnConstraint
    {
        uint32_t _00;
        int32_t kind;            // +0x04 AnimateConstraintKind
        uint32_t firstId;        // +0x08
        uint32_t secondId;       // +0x0C
        uint32_t boneHash;       // +0x10
        ee::Float stiffness;     // +0x14
        uint32_t _18;
        ee::Float x, y, z;       // +0x1C an anchor's place
        ee::Ptr<ZpdnConstraint> next; // +0x28
    };
    static_assert(offsetof(ZpdnConstraint, stiffness) == 0x14 && offsetof(ZpdnConstraint, x) == 0x1C &&
                  offsetof(ZpdnConstraint, next) == 0x28);

    // A ZPVL record (0x20 bytes read).
    struct ZpvlRecord
    {
        uint8_t _00[0x20];
        ee::Ptr<ZpvlRecord> next; // +0x20
    };

    namespace
    {
        // Each sub-record gets a block of this size, a ZPCT too, though its
        // 40 bytes and next pointer reach 8 bytes past it.
        constexpr uint32_t kZpdnSubRecordBytes = 36;

        inline ee::Global<ee::Ptr<ZpdnRecord>, sym::g_loadingAnimateSystem> g_loadingAnimateSystem; // the ZPDN chunk being read
        inline ee::Global<ee::Ptr<Pool>, sym::g_zpdnChunkPool> g_zpdnChunkPool;
        inline ee::Global<int32_t, sym::g_movieInterface> g_movieInterface;

        // A sub-chunk's record: a cleared block with `bytes` of the chunk.
        uint32_t readSubRecord(ChunkReader *reader, uint32_t blockBytes, uint32_t bytes)
        {
            const uint32_t block = memoryFill(heapAllocate(blockBytes), 0, blockBytes);
            readChunk(reader, ee::Ptr<uint8_t>(block).get(), bytes);
            return block;
        }
        // The record pushed on the front of one of the loading system's lists.
        template <typename Record>
        void pushRecord(ee::Ptr<Record> record, ee::Ptr<Record> &list)
        {
            record->next = list;
            list = record;
        }
    }

    namespace
    {
        namespace tail
        {
            void heapFree(ee::Ptr<ZpvlRecord> block) { ee::tailCall<void>(sym::heapFree, block); }
        }
    }

    // freeZpdnRecordLists (was freeAnimateArrays; the ZPDN pool's free,
    // freeAnimateRecord): the record's blocks freed, the id's and the first
    // of each list's. Kept bug: only the newest of each list goes, the rest
    // leak (no level has a ZPDN chunk). The block that ends it is a tail
    // call.
    void freeZpdnRecordLists(ee::Ptr<ZpdnRecord> record)
    {
        if (record->id)
            heapFree(record->id);
        if (record->points)
            heapFree(record->points);
        if (record->constraints)
            heapFree(record->constraints);
        if (record->pivots)
            heapFree(record->pivots);
        if (record->zpvl)
            tail::heapFree(record->zpvl);
    }
    READABLE_FUNCTION_WAITS(freeZpdnRecordLists, Draft);

    // readZpdfChunk: the system's id, in a 4-byte block of its own.
    uint32_t readZpdfChunk(ChunkReader *reader)
    {
        const ee::Ptr<uint32_t> id(readSubRecord(reader, sizeof(uint32_t), sizeof(uint32_t)));
        (*g_loadingAnimateSystem)->id = id;
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpdfChunk, Draft);

    // readZpptChunk: a point.
    uint32_t readZpptChunk(ChunkReader *reader)
    {
        const ee::Ptr<ZpdnPoint> point(readSubRecord(reader, kZpdnSubRecordBytes, offsetof(ZpdnPoint, next)));
        pushRecord(point, (*g_loadingAnimateSystem)->points);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpptChunk, Draft);

    // readZplmChunk: a pivot.
    uint32_t readZplmChunk(ChunkReader *reader)
    {
        const ee::Ptr<ZpdnPivot> pivot(readSubRecord(reader, kZpdnSubRecordBytes, offsetof(ZpdnPivot, next)));
        pushRecord(pivot, (*g_loadingAnimateSystem)->pivots);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZplmChunk, Draft);

    // readZpctChunk: a constraint (kept: its 40 bytes and next pointer
    // overrun the 36-byte block by 8).
    uint32_t readZpctChunk(ChunkReader *reader)
    {
        const ee::Ptr<ZpdnConstraint> constraint(readSubRecord(reader, kZpdnSubRecordBytes, offsetof(ZpdnConstraint, next)));
        pushRecord(constraint, (*g_loadingAnimateSystem)->constraints);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpctChunk, Draft);

    // readZpvlChunk: a ZPVL record.
    uint32_t readZpvlChunk(ChunkReader *reader)
    {
        const ee::Ptr<ZpvlRecord> record(readSubRecord(reader, kZpdnSubRecordBytes, offsetof(ZpvlRecord, next)));
        pushRecord(record, (*g_loadingAnimateSystem)->zpvl);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpvlChunk, Draft);

    // readZpdnChunk: a ZPDN form: a cleared system record from the pool (the
    // pool made the first time) and its sub-chunks read into it. 0 when the
    // form cannot be entered.
    uint32_t readZpdnChunk(ChunkReader *reader)
    {
        if (enterChunk(reader) == 0)
            return 0;
        if (!*g_zpdnChunkPool)
            *g_zpdnChunkPool = ee::Ptr<Pool>(createPool(sizeof(ZpdnRecord), 0, sym::freeAnimateRecord));
        const ee::Ptr<ZpdnRecord> record(poolAlloc(*g_zpdnChunkPool));
        *g_loadingAnimateSystem = record;
        memoryFill(record, 0, sizeof(ZpdnRecord));
        parseChunks(reader, 0);
        leaveChunk(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpdnChunk, Draft);

    namespace
    {
        uint32_t poolFirst(ee::Ptr<Pool> pool) { return zge::poolFirst(ee::Ptr<PoolList>(&pool->used)); }
        uint32_t poolNext(uint32_t link) { return zge::poolNext(ee::Ptr<PoolLink>(link)); }
    }

    // findZpdnRecord: the ZPDN system read with this id, or 0.
    ee::Ptr<ZpdnRecord> findZpdnRecord(uint32_t id)
    {
        for (uint32_t link = poolFirst(*g_zpdnChunkPool); link; link = poolNext(link))
        {
            const ee::Ptr<ZpdnRecord> record = poolRecordAs<ZpdnRecord>(link);
            if (record->id && *record->id == id)
                return record;
        }
        return ee::Ptr<ZpdnRecord>();
    }
    READABLE_FUNCTION(findZpdnRecord, Draft);

    // findZpdnPoint (was findZpdnBone): the system's point of this id (its
    // AnimatePoint::id), or 0.
    ee::Ptr<AnimatePoint> findZpdnPoint(ee::Ptr<AnimateSystem> system, uint32_t id)
    {
        for (ee::Ptr<AnimatePoint> point = system->points; point; point = point->next)
            if (point->id == id)
                return point;
        return ee::Ptr<AnimatePoint>();
    }
    READABLE_FUNCTION(findZpdnPoint, Draft);

    // buildAnimateSystemFromZpdn: a new system for the object from the ZPDN
    // system of this id: its points, pivots and constraints. 0 when there is
    // none of that id, or when a pivot or constraint names a point it does
    // not have (the system made so far is left as it is). A bone point or
    // bone offset constraint's place is copied but not passed: the
    // constraint takes it from the pose.
    ee::Ptr<AnimateSystem> buildAnimateSystemFromZpdn(ee::Ptr<SceneObject> object, uint32_t id)
    {
        ee::Local<Vec4> place;
        ee::Local<Vec4> anchorPlace;
        ee::Local<Vec4> unusedPlace;
        const ee::Ptr<ZpdnRecord> record = findZpdnRecord(id);
        if (!record)
            return ee::Ptr<AnimateSystem>();
        updatePose(object);
        const ee::Ptr<AnimateSystem> system = newAnimateSystem(object);
        for (ee::Ptr<ZpdnPoint> point = record->points; point; point = point->next)
        {
            place->x = point->x;
            place->y = point->y;
            place->z = point->z;
            newBoneAttachedPoint(system, findModelBone(object, point->boneHash), kPointFree, place, point->id,
                                 point->mass);
        }
        for (ee::Ptr<ZpdnPivot> pivot = record->pivots; pivot; pivot = pivot->next)
        {
            const ee::Ptr<AnimatePoint> from = findZpdnPoint(system, pivot->fromId);
            const ee::Ptr<AnimatePoint> to = findZpdnPoint(system, pivot->toId);
            if (!from || !to)
                return ee::Ptr<AnimateSystem>();
            newBonePivot(system, findModelBone(object, pivot->boneHash), from, to);
        }
        for (ee::Ptr<ZpdnConstraint> constraint = record->constraints; constraint; constraint = constraint->next)
        {
            switch (constraint->kind)
            {
            case kConstraintDistance:
            {
                const ee::Ptr<AnimatePoint> first = findZpdnPoint(system, constraint->firstId);
                const ee::Ptr<AnimatePoint> second = findZpdnPoint(system, constraint->secondId);
                if (!first || !second)
                    return ee::Ptr<AnimateSystem>();
                newDistanceConstraint(system, first, second, constraint->stiffness);
                break;
            }
            case kConstraintAnchor:
            {
                const ee::Ptr<AnimatePoint> first = findZpdnPoint(system, constraint->firstId);
                if (!first)
                    return ee::Ptr<AnimateSystem>();
                anchorPlace->x = constraint->x;
                anchorPlace->y = constraint->y;
                anchorPlace->z = constraint->z;
                newAnimateAnchor(system, first, anchorPlace, constraint->stiffness);
                break;
            }
            case kConstraintBonePoint:
            {
                const ee::Ptr<AnimatePoint> first = findZpdnPoint(system, constraint->firstId);
                if (!first)
                    return ee::Ptr<AnimateSystem>();
                unusedPlace->x = constraint->x;
                unusedPlace->y = constraint->y;
                unusedPlace->z = constraint->z;
                newAnimateBoneSpace(system, first, findModelBone(object, constraint->boneHash), constraint->stiffness);
                break;
            }
            case kConstraintBoneOffset:
            {
                const ee::Ptr<AnimatePoint> first = findZpdnPoint(system, constraint->firstId);
                const ee::Ptr<AnimatePoint> second = findZpdnPoint(system, constraint->secondId);
                if (!first || !second)
                    return ee::Ptr<AnimateSystem>();
                unusedPlace->x = constraint->x;
                unusedPlace->y = constraint->y;
                unusedPlace->z = constraint->z;
                newBoneConstraint(system, first, second, findModelBone(object, constraint->boneHash),
                                  constraint->stiffness);
                break;
            }
            default:
                break;
            }
        }
        return system;
    }
    READABLE_FUNCTION_WAITS(buildAnimateSystemFromZpdn, Draft);

    uint32_t animateStubZero2()
    {
        return 0;
    }
    READABLE_FUNCTION(animateStubZero2, Draft);

    uint32_t shutdownZpdnChunks()
    {
        destroyPool(*g_zpdnChunkPool);
        *g_zpdnChunkPool = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownZpdnChunks, Draft);

    // G_movieInterface cleared.
    void clearMovieInterface()
    {
        *g_movieInterface = 0;
    }
    READABLE_FUNCTION(clearMovieInterface, Draft);
}
