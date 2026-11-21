// BMX XXX's way out of a level (level_exit.h). BMX XXX's own code (game.c's
// block); worldNodesOnly is word for word DM2's (games/dm2/game/game_frame.cpp).

#include "level_exit.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/overlay/displays.h"
#include "player_riders.h"
#include "round.h"
#include "score.h"

#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"

#include "bmx/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/game/level_exit.cpp.h"

namespace game::bmx::rider::collision
{
    uint32_t firstNode(const Query *query); // hits.cpp
    int32_t firstFace(const Query *query);
}

namespace game::bmx
{
    namespace
    {
        // The surface kinds of the level's way out (a face's tag): the
        // ground before it, where the exit warning shows, and the exit itself.
        constexpr uint32_t kSurfaceExitWarning = 9;
        constexpr uint32_t kSurfaceExit = 10;
        // How far below the rider the ground is looked for (y grows downwards).
        constexpr float kGroundProbe = 25.0f;
        constexpr uint32_t kWorldNodesOnly = sym::worldNodesOnly;

        // What the exit warning's display takes: the player, and 0 to show
        // it or kOverlayNone to take it away.
        struct ExitWarningMessage
        {
            uint8_t player;
            uint8_t item;
        };
        constexpr uint8_t kOverlayNone = 0xFF;
        // The camera's modes the exit goes through: the cut, then the
        // follow camera again.
        constexpr uint32_t kCameraCut = 3, kCameraFollow = 0;
        constexpr uint8_t kContactRailEdge = 5, kContactRailEdgeB = 6;

        // A node's face as a reference, or none when the node has no
        // collision faces.
        rider::collision::FaceRef faceOfNode(uint64_t node, int32_t face)
        {
            if (rider::collision::nodeFaceCount(node) == 0)
                return 0;
            return rider::collision::makeFace(static_cast<uint32_t>(node), face);
        }

        // What the rider is in contact with, as a face: the face itself, a
        // rail edge's face, an object's node (its first face). A contact of
        // no known kind gives none.
        rider::collision::FaceRef contactFace(const rider::Rider &rider)
        {
            if (rider.contactKind == kContactRailEdge || rider.contactKind == kContactRailEdgeB)
            {
                const rider::collision::EdgeRef edge = rider::contactRef(&rider);
                const uint64_t node = static_cast<uint64_t>(static_cast<int64_t>(rider::collision::refNode(edge)));
                return faceOfNode(node, rider::collision::edgeFace(edge));
            }
            if (rider.contactKind == rider::kContactObject)
            {
                uint32_t object;
                std::memcpy(&object, rider.contact, sizeof(object));
                const ee::Ptr<zge::Node> node = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(object));
                return faceOfNode(rider::collision::nodeHandle(ee::Address(node)), 0);
            }
            if (rider.contactKind < rider::kContactKinds)
                return rider::contactRef(&rider);
            return 0;
        }

        // Whether a node belongs to no object (it is the world's own).
        bool worldNode(uint64_t node)
        {
            const ee::Ptr<zge::Node> owner(static_cast<uint32_t>(rider::collision::nodeOf(node)));
            return !zge::nodeSceneObject(owner, ee::Ptr<ee::Ptr<zge::SceneBone>>());
        }
    }

    // worldNodesOnly: the ground probe's query filter. A face passes; a
    // whole node passes only when it is the world's own, not a scene
    // object's.
    int32_t worldNodesOnly(uint64_t node, int32_t face)
    {
        constexpr int32_t kWholeNode = -1;
        if (face != kWholeNode)
            return 1;
        return worldNode(node) ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(worldNodesOnly, Verified);

    // checkLevelExit: a player's frame of the level's way out.
    //
    // The ground is the face the rider touches (a rail's or an object's
    // too), or, when that is an object's (or the rider touches nothing), the
    // world's face within 25 below the rider. On the ground before the exit
    // or on the exit, with the camera in play and the turn running, the exit
    // warning shows. On the exit itself the trick line ends unscored, the
    // rider is put back at the spawn, the camera cuts there, the warning is
    // taken away and the overlay fades in.
    void checkLevelExit(uint32_t player)
    {
        ee::Local<ExitWarningMessage> warning;
        ee::Local<rider::collision::Query> below;
        ee::Local<zge::Vec4> from;
        ee::Local<zge::Vec4> to; // its w is left as it was

        player &= 0xFFu;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::collision::FaceRef ground = contactFace(*rider);
        if (ground == 0 || !worldNode(static_cast<uint64_t>(static_cast<int64_t>(rider::collision::refNode(ground)))))
        {
            *from = *rider::vec4(rider->position);
            to->x = 0.0f;
            to->y = kGroundProbe;
            to->z = 0.0f;
            *to = *to + *rider::vec4(rider->position);
            if (rider::collision::segmentQueryRaw(&from->x, &to->x, kWorldNodesOnly, 0u, below))
            {
                const int32_t face = rider::collision::firstFace(below);
                ground = faceOfNode(rider::collision::firstNode(below), face);
            }
        }
        if (ground == 0)
            return;
        const uint32_t kind = rider::collision::faceSurfaceKind(ground);
        if (kind != kSurfaceExitWarning && kind != kSurfaceExit)
            return;
        if (cameraState() != 0 || roundPhase() != kTurnRunningPhase)
            return;
        warning->player = static_cast<uint8_t>(player);
        warning->item = 0;
        overlayMessage(kOverlayExitWarning, warning);
        if (rider::collision::faceSurfaceKind(ground) != kSurfaceExit)
            return;
        if (rider->score.lineState != 0)
            trickLineEnd(player, 0);
        rider::resetPhysics(rider, 1);
        rider::riderReset(riderIndexOf(player), 1);
        setCameraMode(playerActionCamera(player), 1, kCameraCut);
        setCameraMode(playerActionCamera(player), 1, kCameraFollow);
        warning->item = kOverlayNone;
        overlayMessage(kOverlayExitWarning, warning);
        playOverlayFade(player, 0);
    }
    READABLE_FUNCTION_WAITS(checkLevelExit, Verified);
}
