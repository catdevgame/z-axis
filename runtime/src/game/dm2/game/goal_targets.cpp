// DM2's level goals (game.c): the targets of the objectives whose label is
// up, drawn near the rider (goals.h).

#include "goals.h"

#include "players.h"
#include "round.h"
#include "dm2/action/action_state.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/collision/node_faces.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/goal_targets.cpp.h"

namespace zge
{
    // (primitives.cpp, in no header)
    int32_t drawLines(uint32_t primitiveFlags, uint32_t vertices, uint32_t vertexCount, uint32_t vertexMatrix);
}

namespace game::dm2
{
    namespace rider::collision
    {
        void faceCentre(FaceRef ref, ee::Float *centre); // faces.cpp
        EdgeRef faceLinkedEdge(FaceRef face);           // tags.cpp
    }

    namespace
    {
        // The colour a challenge's targets are drawn in (0x00BBGGRR words), by challenge.
        inline ee::Global<uint32_t[kLevelChallenges], sym::g_challengeTargetColours> g_challengeTargetColours;
        // How near the rider (squared: 25) a target is drawn.
        inline ee::Global<ee::Float, sym::g_faceTargetReach> g_faceTargetReach;
        inline ee::Global<ee::Float, sym::g_edgeTargetReach> g_edgeTargetReach;
        inline ee::Global<ee::Float, sym::g_objectTargetReach> g_objectTargetReach;
        // The goals' frames drawn (the targets' drawing pulses with it).
        inline ee::Global<uint32_t, sym::g_goalTargetFrame> g_goalTargetFrame;
        // Set, the rider's own contact and the edges about it are drawn too (a debug setting).
        inline ee::Global<int32_t, sym::g_riderDebugDraw> g_debugDraw;
        constexpr float kDebugEdgesRadius = 10.0f;
        constexpr float kHalf = 0.5f;

        using rider::collision::NodeFace;

        // Scaffolding (address-call): the debug draw of the edges about a
        // point has no readable body yet.
        namespace calls
        {
            void drawEdgesNear(const ee::Float *point, ee::Float radius) { ee::game(sym::drawEdgesNear, point, radius); }
        }

        // Whether the scene's last draw listed the node.
        bool listedThisDraw(uint32_t node)
        {
            return zge::nodeListedFrame(*ee::Ptr<zge::Node>(node)) == (*zge::g_collisionWorld)->frameStamp;
        }
    }

    void drawFaceTarget(uint64_t face, uint32_t colour);
    void drawEdgeTarget(uint64_t edge, uint32_t colour);
    void drawObjectTarget(uint32_t object, uint32_t colour);

    void drawFaceTargetNear(uint64_t face, const ee::Float *riderPosition, uint32_t colour)
    {
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> apart;
        if (!listedThisDraw(static_cast<uint32_t>(rider::collision::refNode(face))))
            return;
        rider::collision::faceCentre(face, &centre->x);
        *apart = *centre - *rider::vec4(riderPosition);
        if (rider::collision::engine::lengthSquared(&apart->x) < *g_faceTargetReach)
            drawFaceTarget(face, colour);
    }
    READABLE_FUNCTION_VU0(drawFaceTargetNear, Verified);

    void drawEdgeTargetNear(uint64_t edge, const ee::Float *riderPosition, uint32_t colour)
    {
        ee::Local<zge::Vec4> middle; // the edge's first end, then its middle
        ee::Local<zge::Vec4> other;  // its other end, then the middle from the rider
        if (!listedThisDraw(static_cast<uint32_t>(rider::collision::refNode(edge))))
            return;
        rider::collision::edgePoint(edge, 0, &middle->x);
        rider::collision::edgePoint(edge, 1, &other->x);
        const ee::Float x = middle->x + (other->x - middle->x) * ee::Float(kHalf);
        const ee::Float y = middle->y + (other->y - middle->y) * ee::Float(kHalf);
        const ee::Float z = middle->z + (other->z - middle->z) * ee::Float(kHalf);
        middle->x = x;
        middle->y = y;
        middle->z = z;
        *other = *middle - *rider::vec4(riderPosition);
        if (rider::collision::engine::lengthSquared(&other->x) < *g_edgeTargetReach)
            drawEdgeTarget(edge, colour);
    }
    READABLE_FUNCTION_VU0(drawEdgeTargetNear, Verified);

    void drawObjectTargetNear(uint32_t object, const ee::Float *riderPosition, uint32_t colour)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> apart;
        const ee::Ptr<zge::SceneObject> target(object);
        if (!listedThisDraw(ee::Address(zge::sceneRootNode(zge::sceneOf(target)))))
            return;
        *position = *zge::objectPosition(target);
        *apart = *position - *rider::vec4(riderPosition);
        if (rider::collision::engine::lengthSquared(&apart->x) < *g_objectTargetReach)
            drawObjectTarget(object, colour);
    }
    READABLE_FUNCTION_VU0(drawObjectTargetNear, Verified);

    namespace
    {
        // The targets pulse with the goals' frames: every 64 frames their
        // strength starts at 252 and loses 4 a frame, never under 64.
        constexpr uint32_t kPulseFrames = 0x3F;
        constexpr uint32_t kPulseLeast = 0x40;
        // The primitive API's kind the targets are drawn as (the engine has
        // no name for it yet).
        constexpr uint32_t kDrawKindTwo = 2;
        // A face target is drawn this far off the surface, along its normal.
        constexpr float kLift = 0.01f;

        // The colour (0x00RRGGBB) at the pulse's strength now, as a vertex's
        // four bytes: red, green, blue, and the strength.
        void pulsedColour(uint8_t *rgba, uint32_t colour)
        {
            uint32_t strength = (kPulseFrames - (*g_goalTargetFrame & kPulseFrames)) << 2;
            if (!(kPulseFrames < strength))
                strength = kPulseLeast;
            strength &= 0xFF;
            rgba[3] = static_cast<uint8_t>(strength);
            rgba[0] = static_cast<uint8_t>((((colour >> 16) & 0xFF) * strength) >> 8);
            rgba[1] = static_cast<uint8_t>((((colour >> 8) & 0xFF) * strength) >> 8);
            rgba[2] = static_cast<uint8_t>(((colour & 0xFF) * strength) >> 8);
        }

        // A vertex's position as the quadword the original stores (x, y, z
        // and the word after them).
        zge::Vec4 &positionOf(ee::Ptr<zge::PrimitiveVertex> vertex)
        {
            return *vertex.cast<zge::Vec4>();
        }
        void setColour(ee::Ptr<zge::PrimitiveVertex> vertex, const uint8_t *rgba)
        {
            for (uint32_t byte = 0; byte < 4; ++byte)
                vertex->rgba[byte] = rgba[byte];
        }

        // An edge target is a box round the edge: four sides, each the four
        // lines round a square from the edge's first end to its second. Its
        // half-width grows from 0.1 to 0.3 over the 64 frames of the pulse.
        constexpr uint32_t kBoxSides = 4;
        constexpr uint32_t kSideCorners = 4;
        constexpr float kLeastHalfWidth = 0.1f;
        constexpr float kHalfWidthPerFrame = 0.0031746035f; // (0.2 / 63, as the data has it: a step over the nearest)
    }

    uint32_t drawObjectFaceTarget(uint64_t node, ee::Ptr<NodeFace> face, ee::Ptr<uint8_t> rgba);

    // drawFaceTarget: the face as one triangle of the goals' material in the
    // pulsed colour, lifted a little off the surface along the face's normal.
    // (The lift's fourth lane is not set: each vertex's word after its
    // position gets the corner's plus what the frame held.)
    void drawFaceTarget(uint64_t face, uint32_t colour)
    {
        ee::Local<zge::PrimitiveTriangle> triangle;
        ee::Local<zge::PrimitiveVertex[3]> vertices;
        ee::Local<zge::Vec4> lift;
        ee::Local<uint8_t[4]> rgba;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> corner;
        pulsedColour(&rgba[0], colour);
        rider::collision::faceNormal(face, &normal->x);
        lift->x = normal->x * ee::Float(kLift);
        lift->y = normal->y * ee::Float(kLift);
        lift->z = normal->z * ee::Float(kLift);
        for (uint32_t i = 0; i < 3; ++i)
        {
            rider::collision::faceVertex(face, i, &corner->x);
            const ee::Ptr<zge::PrimitiveVertex> vertex = ee::Ptr<zge::PrimitiveVertex>(vertices) + i;
            setColour(vertex, &rgba[0]);
            positionOf(vertex) = *corner + *lift;
            triangle->vertices[i] = vertex;
        }
        triangle->texture = *g_goalsMaterial;
        zge::drawPrimitiveStrips(kDrawKindTwo, triangle.address(), 1, ee::Address());
    }
    READABLE_FUNCTION_VU0(drawFaceTarget, Verified);

    // drawEdgeTarget: the edge as a box of lines of the goals' material in
    // the pulsed colour. The box's cross-section is a square round the edge,
    // in the frame of the edge's upright plane's normal and its direction;
    // each side's square runs from the edge's first end to its second.
    void drawEdgeTarget(uint64_t edge, uint32_t colour)
    {
        ee::Local<zge::Vec4> corner;
        ee::Local<zge::Vec4> turned;
        ee::Local<zge::PrimitiveLine[kBoxSides * kSideCorners]> lines;
        ee::Local<zge::PrimitiveVertex[kBoxSides * kSideCorners]> vertices;
        ee::Local<zge::Matrix> frame;
        ee::Local<uint8_t[4]> rgba;
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> end;
        pulsedColour(&rgba[0], colour);
        rider::collision::edgeDirection(edge, &direction->x);
        rider::collision::edgePlane(edge, &plane->x);

        zge::Vec4 &across = zge::matrixRow(*frame, 0);
        zge::Vec4 &normal = zge::matrixRow(*frame, 1);
        zge::Vec4 &along = zge::matrixRow(*frame, 2);
        along = *direction;
        normal = *plane;
        across = zge::cross(normal, along);
        zge::matrixRow(*frame, 3) = zge::kZero;

        const ee::Float halfWidth = ee::toFloat(static_cast<int32_t>(*g_goalTargetFrame & kPulseFrames)) *
                                        ee::Float(kHalfWidthPerFrame) + ee::Float(kLeastHalfWidth);
        for (uint32_t side = 0; side < kBoxSides; ++side)
        {
            const ee::Ptr<zge::PrimitiveVertex> square = ee::Ptr<zge::PrimitiveVertex>(vertices) + side * kSideCorners;
            for (uint32_t i = 0; i < kSideCorners; ++i)
            {
                // The corner's place in the square, and which end it is at:
                // corners 0 and 1 at the first end, 2 and 3 at the second;
                // corners 1 and 2 on the side's other edge.
                const bool atSecondEnd = (i & 2) != 0;
                const bool otherEdge = ((i + 1) & 2) != 0;
                const bool farSide = (side & 2) != 0;
                corner->z = atSecondEnd ? halfWidth : -halfWidth;
                if ((side & 1) == 0)
                {
                    corner->y = otherEdge ? -halfWidth : halfWidth;
                    corner->x = farSide ? halfWidth : -halfWidth;
                }
                else
                {
                    corner->y = farSide ? halfWidth : -halfWidth;
                    corner->x = otherEdge ? -halfWidth : halfWidth;
                }
                rider::collision::edgePoint(edge, atSecondEnd ? 1 : 0, &end->x);
                rider::collision::engine::transformPoint(&turned->x, frame, &corner->x);
                const ee::Ptr<zge::PrimitiveVertex> vertex = square + i;
                positionOf(vertex) = *turned + *end;
                setColour(vertex, &rgba[0]);
                zge::PrimitiveLine &line = lines[side * kSideCorners + i];
                line.first = vertex;
                line.second = square + ((i + 1) & 3);
                line.texture = *g_goalsMaterial;
            }
        }
        zge::drawLines(kDrawKindTwo, lines.address(), kBoxSides * kSideCorners, 0);
    }
    READABLE_FUNCTION_VU0(drawEdgeTarget, Verified);

    // drawObjectTarget: every face of the object's model drawn in the
    // pulsed colour.
    void drawObjectTarget(uint32_t object, uint32_t colour)
    {
        ee::Local<uint8_t[4]> rgba;
        pulsedColour(&rgba[0], colour);
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(object));
        rider::collision::forEachNodeFace(ee::Address(node), rider::collision::NodeFaceVisit(sym::drawObjectFaceTarget), rgba);
    }
    READABLE_FUNCTION(drawObjectTarget, Verified);

    // drawObjectFaceTarget (drawObjectTarget's visit of a face): the face as
    // one triangle of the goals' material in the colour, lifted a little
    // along its normal. Always 1: the visit goes on.
    uint32_t drawObjectFaceTarget(uint64_t node, ee::Ptr<NodeFace> face, ee::Ptr<uint8_t> rgba)
    {
        (void)node;
        ee::Local<zge::PrimitiveTriangle> triangle;
        ee::Local<zge::PrimitiveVertex[3]> vertices;
        ee::Local<zge::Vec4> lift;
        const uint32_t material = *g_goalsMaterial;
        *lift = face->normal * ee::Float(kLift);
        for (uint32_t i = 0; i < 3; ++i)
        {
            const ee::Ptr<zge::PrimitiveVertex> vertex = ee::Ptr<zge::PrimitiveVertex>(vertices) + i;
            positionOf(vertex) = face->vertices[i].position + *lift;
            setColour(vertex, rgba);
            triangle->vertices[i] = vertex;
        }
        triangle->texture = material;
        zge::drawPrimitiveStrips(kDrawKindTwo, triangle.address(), 1, ee::Address());
        return 1;
    }
    READABLE_FUNCTION_VU0(drawObjectFaceTarget, Verified);

    namespace
    {
        // A list's targets drawn near the rider, as faces, edges or objects.
        void drawFaces(const TargetList &list, const ee::Float *riderPosition, const LevelGoals &goals)
        {
            for (uint32_t i = 0; i < list.count; ++i)
                drawFaceTargetNear(list.targets[i], riderPosition, (*g_challengeTargetColours)[goals.challenge]);
        }
        void drawEdges(const TargetList &list, const ee::Float *riderPosition, const LevelGoals &goals)
        {
            for (uint32_t i = 0; i < list.count; ++i)
                drawEdgeTargetNear(list.targets[i], riderPosition, (*g_challengeTargetColours)[goals.challenge]);
        }
        void drawObjects(const TargetList &list, const ee::Float *riderPosition, const LevelGoals &goals)
        {
            for (uint32_t i = 0; i < list.count; ++i)
                drawObjectTargetNear(static_cast<uint32_t>(list.targets[i]), riderPosition,
                                     (*g_challengeTargetColours)[goals.challenge]);
        }

        // The objective state whose label is up (setObjectiveState: 2).
        constexpr uint32_t kLabelShownState = 2;
    }

    void drawGoalTargets()
    {
        if (multiplayerMode() == 0 && singlePlayerMode() == kLevelGoals)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(*g_currentPlayer, *g_sessionLevel);
            const ee::Float *const at = rider->position;
            const uint32_t challenge = goals->challenge;
            const GoalDefinition *definition = (*g_goalDefinitions)[*g_sessionLevel][challenge];
            for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[challenge]; ++goal, ++definition)
            {
                if (goals->challenges[challenge].goals[goal].flags & kGoalDone)
                    continue;
                for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
                {
                    if (objectiveState(*g_currentPlayer, goal, objective) != kLabelShownState)
                        continue;
                    const ObjectiveTargets &found = (*g_objectiveTargets)[goal][objective];
                    const TargetList &first = found.named[0];
                    const TargetList &second = found.named[1];
                    switch (definition->objectives[objective].kind)
                    {
                    case kObjectiveTransfer:
                    case kObjectiveTransferTrickSpin:
                    case kObjectiveTransferMotionSpin:
                    case kObjectiveTransferTrickMotion:
                        drawFaces(second, at, *goals);
                        [[fallthrough]];
                    case kObjectiveTrickOnFace:
                    case kObjectiveActionOnFace:
                    case kObjectiveWheelieOnFace:
                        drawFaces(first, at, *goals);
                        break;
                    case kObjectiveGrindTransfer:
                        drawEdges(second, at, *goals);
                        [[fallthrough]];
                    case kObjectiveTrickOnEdge:
                    case kObjectiveGrindOnEdge:
                        drawEdges(first, at, *goals);
                        break;
                    case kObjectiveObjectsTransfer:
                        drawObjects(second, at, *goals);
                        [[fallthrough]];
                    case kObjectiveTrickOnObject:
                    case kObjectiveActionOnObject:
                    case kObjectiveGrindOnObject:
                    case kObjectiveAirOffObject:
                    case kObjectiveTrickOffObject:
                    case kObjectiveTouchObject:
                    case kObjectiveTouchNonColliding:
                        drawObjects(first, at, *goals);
                        break;
                    case kObjectiveFaceEdgeTransfer:
                        drawFaces(first, at, *goals);
                        drawEdges(second, at, *goals);
                        break;
                    case kObjectiveFaceObjectTransfer:
                        drawFaces(first, at, *goals);
                        drawObjects(second, at, *goals);
                        break;
                    default:
                        break;
                    }
                }
            }

            if (*g_debugDraw != 0)
            {
                // The rider's own contact (a ground face with its linked edge too).
                const uint64_t contact = rider->contactRef();
                const uint32_t colour = (*g_challengeTargetColours)[goals->challenge];
                switch (rider->contactKind)
                {
                case 1:
                case 2:
                case 3:
                    drawEdgeTargetNear(rider::collision::faceLinkedEdge(contact), at, colour);
                    drawFaceTargetNear(contact, at, colour);
                    break;
                case 4:
                case 6:
                case 7:
                case 9:
                case 10:
                    drawFaceTargetNear(contact, at, colour);
                    break;
                case 5:
                    drawEdgeTargetNear(contact, at, colour);
                    break;
                case rider::kContactObject:
                    drawObjectTargetNear(static_cast<uint32_t>(contact), at, colour);
                    break;
                default:
                    break;
                }
                calls::drawEdgesNear(at, ee::Float(kDebugEdgesRadius));
            }
        }
        ++*g_goalTargetFrame;
    }
    READABLE_FUNCTION(drawGoalTargets, Verified);
}
