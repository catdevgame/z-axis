// DM2's game frame (round.h): what the game does each frame of play for the
// riders: their scoring and goals, the level's exit, the running turn's
// clock and its end. DM2's own code (game.c).

#include "round.h"
#include "goals.h"
#include "players.h"
#include "score.h"
#include "tips.h"

#include "dm2/action/action.h"
#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "dm2/sound/sound_mode.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/game/game_frame.cpp.h"

namespace game::dm2::rider
{
    ee::Ptr<Rider> riderOf(uint32_t index);                    // rider_state.cpp
    uint8_t actionState(const Rider *rider, uint32_t level);   // rider_state.cpp
    uint8_t physicsState(const Rider *rider);                  // physics_state.cpp
}

namespace game::dm2::rider::collision
{
    uint32_t firstNode(const Query *query); // hits.cpp
    int32_t firstFace(const Query *query);
}

namespace game::dm2
{
    namespace
    {
        // The phase's clock and the running turn's end (round.cpp): the
        // frames left, how far the end has got, and the frames since the
        // rider came to rest.
        inline ee::Global<uint32_t, sym::g_phaseFramesLeft> g_phaseFramesLeft;
        inline ee::Global<uint8_t, sym::g_turnEndStage> g_turnEndStage;
        inline ee::Global<uint8_t, sym::g_turnEndFrames> g_turnEndFrames;
        // Not zero: the turn's clock stands still. Nothing in this build sets it.
        inline ee::Global<uint32_t, sym::g_clockStopped> g_clockStopped;

        // How far a running turn's end has got (g_turnEndStage; a turn starts
        // at kTurnRiding).
        enum TurnEndStage : uint8_t
        {
            kTurnRiding = 0,   // until the time is out, or a line is begun
            kTurnLineBegun = 1, // a one-line turn goes on once a line is open; any turn once the time is out
            kTurnLastLine = 2, // waiting for the open line to end with the rider on the ground
            kTurnAtRest = 3,   // the line is in: kRestFrames more frames
            kTurnEnded = 4,    // the turn is over (in play: not behind a screen)
        };
        constexpr uint8_t kRestFrames = 30;
        // The multiplayer modes whose turn is one line: it ends with the
        // first line the rider finishes, time left or not.
        constexpr uint8_t kFirstOneLineMode = kSeriesMode, kLastOneLineMode = kFirstBestTurnMode;

        // The clock's last seconds are counted aloud: the sound event, with
        // the seconds left, each second from kCountedSeconds down.
        constexpr uint32_t kCountdownSound = 0x27;
        constexpr uint32_t kFramesPerSecond = 60;
        constexpr uint32_t kCountedSeconds = 5;

        // The surface kinds of the level's way out (a face's tag): the
        // ground before it, where the exit warning shows, and the exit itself.
        constexpr uint32_t kSurfaceExitWarning = 9;
        constexpr uint32_t kSurfaceExit = 10;
        // How far below the rider the ground is looked for (y grows downwards).
        constexpr float kGroundProbe = 25.0f;
        constexpr uint32_t kWorldNodesOnly = sym::worldNodesOnly;

        constexpr uint32_t kPlayPhaseStopped = 4; // (round.cpp)
        constexpr uint8_t kActionPlay = 0;        // the action mode of play (no screen up)

        // What the rider is in contact with, as a face: the face itself, a
        // rail edge's face, an object's node (its first face). A contact of
        // no known kind leaves `before` as it was.
        rider::collision::FaceRef contactFace(const rider::Rider &rider, rider::collision::FaceRef before)
        {
            constexpr uint8_t kContactRailEdge = 5;
            if (rider.contactKind >= rider::kContactKinds)
                return before;
            if (rider.contactKind == kContactRailEdge)
            {
                const rider::collision::EdgeRef edge = rider.contactRef();
                return rider::collision::makeFace(static_cast<uint32_t>(edge), rider::collision::edgeFace(edge));
            }
            if (rider.contactKind == rider::kContactObject)
            {
                uint32_t object;
                std::memcpy(&object, rider.contact, sizeof(object));
                const ee::Ptr<zge::Node> node = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(object));
                return static_cast<uint32_t>(rider::collision::nodeHandle(ee::Address(node)));
            }
            return rider.contactRef();
        }

        // Whether a node belongs to no object (it is the world's own).
        bool worldNode(uint64_t handle)
        {
            return rider::collision::engine::nodeObject(rider::collision::nodeOf(handle), 0u) == 0;
        }
    }

    // The ground probe's query filter: every face, but of whole nodes only
    // the world's own (an object's node is passed over).
    int32_t worldNodesOnly(uint64_t node, int32_t face)
    {
        constexpr int32_t kWholeNode = -1;
        if (face != kWholeNode)
            return 1;
        return worldNode(node) ? 1 : 0;
    }
    READABLE_FUNCTION(worldNodesOnly, Verified);

    // gameRiderFrame: the game's frame for the riders.
    //
    // Each player's rider that is shown: its trick line and score, the
    // single-player trick records, the level's goals; a bail's text as it
    // starts; and the level's way out: on the ground before the exit the
    // exit warning shows, and riding onto the exit itself leaves the level
    // (kExitPhase). The ground is the face the rider touches, or, when that
    // is an object's (or the rider touches nothing), the world's face below.
    //
    // Then, while a turn runs, its end: the clock counts down (its last five
    // seconds aloud), and once the time is out (a one-line turn: once its
    // line is begun) the turn waits for the open line to end with the rider
    // on the ground, rests thirty frames, and is over (kTurnOverPhase).
    // Returns the round's phase.
    uint8_t gameRiderFrame()
    {
        ee::Local<OverlayReward> reward;
        ee::Local<rider::collision::Query> below;
        ee::Local<zge::Vec4> from;
        ee::Local<zge::Vec4> to; // its w is left as it was
        ee::Local<uint8_t> health; // the rider's, for the overlay
        ee::Local<uint8_t> exitWarning;
        ee::Local<uint32_t> clock;

        rider::collision::FaceRef ground = 0;
        for (uint32_t player = 0; *g_multiplayerPlayers == 0 ? player == 0 : player < *g_multiplayerPlayers; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            if ((rider->flags & rider::kFlagShown) == 0)
                continue;

            if (!(roundPhase() == kTurnRunningPhase && *g_turnEndStage == kTurnAtRest))
                trickScoreFrame(rider->player);
            if (multiplayerMode() == 0)
                tipsFrame(player);
            goalsFrame(player);
            health = rider->health;
            overlayMessage(kOverlayEmpty, health);

            if (rider::actionState(rider, rider::kLevelAction) == rider::kActionBail && rider->actionFrames[0] == 1)
            {
                reward->kind = kRewardFall;
                reward->value = ee::toFloat(static_cast<int32_t>(rider->bailKind));
                overlayMessage(kOverlayReward, reward);
            }

            ground = contactFace(*rider, ground);
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
                    ground = rider::collision::makeFace(static_cast<uint32_t>(rider::collision::firstNode(below)), face);
                }
            }
            if (ground != 0)
            {
                const uint32_t kind = rider::collision::faceSurfaceKind(ground);
                if ((kind == kSurfaceExitWarning || kind == kSurfaceExit) && actionMode() == kActionPlay &&
                    (roundPhase() == kFreeTurnPhase || roundPhase() == kTurnRunningPhase))
                {
                    exitWarning = 0;
                    overlayMessage(kOverlayExitWarning, exitWarning);
                    if (rider::collision::faceSurfaceKind(ground) == kSurfaceExit)
                        setRoundPhase(kExitPhase);
                }
            }

            if (roundPhase() != kTurnRunningPhase)
                continue;
            const bool lineOpen = rider->score.lineState != 0;
            switch (*g_turnEndStage)
            {
            case kTurnRiding:
                if (*g_phaseFramesLeft == 0 || lineOpen)
                    *g_turnEndStage = kTurnLineBegun;
                break;
            case kTurnLineBegun:
            {
                const uint8_t mode = multiplayerMode();
                const bool oneLine = mode >= kFirstOneLineMode && mode <= kLastOneLineMode;
                if (*g_phaseFramesLeft != 0 && !(oneLine && lineOpen))
                    break;
                if (lineOpen || rider::physicsState(rider) == rider::kPhysicsAir)
                    *g_turnEndStage = kTurnLastLine;
                else
                {
                    *g_turnEndFrames = 0;
                    *g_turnEndStage = kTurnAtRest;
                }
                break;
            }
            case kTurnLastLine:
                if (!lineOpen && rider::physicsState(rider) == rider::kPhysicsGround)
                {
                    *g_turnEndFrames = 0;
                    *g_turnEndStage = kTurnAtRest;
                }
                break;
            case kTurnAtRest:
                if (*g_turnEndFrames == kRestFrames)
                    *g_turnEndStage = kTurnEnded;
                break;
            default:
                break;
            }
        }

        const uint8_t phase = roundPhase();
        if (phase == kTurnRunningPhase)
        {
            if (singlePlayerMode() != kFreeRide && singlePlayerMode() != kParkTest)
            {
                if (*g_turnEndStage < kTurnLastLine && actionMode() == kActionPlay)
                {
                    const uint32_t left = *g_phaseFramesLeft;
                    if (left != 0 && left % kFramesPerSecond == 0 && left <= kCountedSeconds * kFramesPerSecond)
                        soundEvent(kCountdownSound, left / kFramesPerSecond, 0, 0, 0);
                }
                clock = *g_phaseFramesLeft;
                overlayMessage(kOverlayTimer, clock);
            }
            if (*g_turnEndStage == kTurnEnded && actionMode() == kActionPlay)
            {
                setRoundPhase(kTurnOverPhase);
                setPlayPhase(kPlayPhaseStopped);
            }
            if (*g_phaseFramesLeft != 0 && *g_clockStopped == 0)
                *g_phaseFramesLeft = *g_phaseFramesLeft - 1;
            *g_turnEndFrames = static_cast<uint8_t>(*g_turnEndFrames + 1);
        }
        else if (phase == kExitPhase)
        {
            *g_phaseFramesLeft = *g_phaseFramesLeft - 1;
            if (*g_phaseFramesLeft == 0)
            {
                setRoundPhase(kTurnOverPhase);
                setPlayPhase(kPlayPhaseStopped);
            }
        }
        return roundPhase();
    }
    READABLE_FUNCTION(gameRiderFrame, Verified);

    // The game task's frame (gameSetup makes the task): the level goals'
    // targets drawn. Always 1 (the task goes on).
    uint32_t gameTaskFrame()
    {
        drawGoalTargets();
        return 1;
    }
    READABLE_FUNCTION(gameTaskFrame, Verified);
}
