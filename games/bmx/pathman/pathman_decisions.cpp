// BMX XXX's path men (pathman.h): what a path man does about the riders.
// Each frame its animation plays on, it goes down a list of things that can
// happen, and the first that does is acted on: the rider takes its
// objective, the rider activates it, the rider has reached it, the rider
// comes at it, the rider lingers by it once it is down, or it is time to
// greet the rider. BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_phrases.h"
#include "pathman_random.h"
#include "pathman_tags.h"

#include "bmx/game/round.h"
#include "bmx/game/player_riders.h"
#include "bmx/objectives/objectives.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/plane.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/bmx/pathman/pathman_decisions.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::vec4;

    namespace
    {
        // (g_sessionKind, game/player_riders.h: 0 is the session in which
        // riders activate level objects by riding up to them.)

        // A rider slower than this does not make a path man dodge.
        constexpr ee::Float kNoSpeed = 0.00001f;
        // The squared distance within which a path man greets a rider.
        constexpr ee::Float kGreetRangeSquared = 1000.0f;
        // A rider this many frames beside a path man that is down is told so.
        constexpr uint32_t kLingerFrames = 0x400;
        // Up (y grows downwards), as a point.
        constexpr Vec4 kUp{0.0f, -1.0f, 0.0f, 1.0f};

        // The bit of an object's flags the engine sets while it is shown
        // (setSceneObjectShown).
        constexpr uint64_t kObjectShown = kObjectMoved;

        // The rider of the player whose turn it is.
        ee::Ptr<rider::Rider> currentRider()
        {
            return rider::riderOf(riderIndexOf(currentPlayer()));
        }
    }

    // decidePathMan: the first of these that holds is acted on. Nothing
    // while the path man is paused.
    void decidePathMan(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->animation.paused != 0)
            return;
        if (riderMayTakeObjective(pathMan) != 0)
        {
            game::tail(givePathManObjective, pathMan);
            return;
        }
        if (riderActivatesPathMan(pathMan, 0) != 0)
            return;
        if (riderReachesPathMan(pathMan) != 0)
        {
            game::tail(pathManIsHit, pathMan);
            return;
        }
        const uint32_t side = riderComesAtPathMan(pathMan);
        if (side != 0)
        {
            game::tail(pathManDodges, pathMan, side);
            return;
        }
        if (riderLingersByPathMan(pathMan) != 0)
        {
            game::tail(sayRiderLingers, pathMan);
            return;
        }
        if (pathManMayGreet(pathMan) != 0)
            game::tail(pathManGreets, pathMan);
    }
    READABLE_FUNCTION_WAITS(decidePathMan, Verified);

    // pathManWasHit: 1 in the hit state and the one after it.
    uint32_t pathManWasHit(ee::Ptr<SceneObject> pathMan)
    {
        const int32_t state = pathManRecord(pathMan)->animation.state;
        return state == kPathManHit || state == kPathManAfterHit ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(pathManWasHit, Verified);

    // riderMayTakeObjective: 1 when, in the game's first mode, the path man
    // gives an objective, a rider is within its notice range, the current
    // player's rider asks for one (kFlagObjectiveAsked: move 24 held), and
    // the objectives say the player may activate it now.
    uint32_t riderMayTakeObjective(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (gameMode() != 0)
            return 0;
        if (record->objectiveName[0] == 0)
            return 0;
        if (pathManRiderWithin(pathMan, record->noticeRangeSquared) == 0)
            return 0;
        const ee::Ptr<rider::Rider> rider = currentRider();
        if (!rider::hasFlag(&*rider, rider::kFlagObjectiveAsked))
            return 0;
        if (!record->objective)
            return 0;
        return objectives::playerCanActivateObjective(currentPlayer(), record->objective) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(riderMayTakeObjective, Verified);

    // givePathManObjective: the path man's objective is activated for the
    // current player.
    void givePathManObjective(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        const uint32_t player = currentPlayer();
        game::tail(objectives::activatePlayerObjectiveByName, player, objectiveNameOf(record));
    }
    READABLE_FUNCTION_WAITS(givePathManObjective, Verified);

    // riderActivatesPathMan: in session kind 0 the game is told that the
    // current player's rider activates the path man, and 1 is answered when
    // the game took the activation. Nothing is told when whether the rider's
    // drawn body is within touching range equals `notWhenWithin`: the frame's
    // decisions ask with 0 (the rider must be that near), the touched
    // message with 1 (the rider must not be).
    uint32_t riderActivatesPathMan(ee::Ptr<SceneObject> pathMan, uint32_t notWhenWithin)
    {
        ee::Local<pathman::ActivationEvent> event;
        ee::Local<EngineMessage> activation;
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        const ee::Ptr<rider::Rider> rider = currentRider();
        if (*g_sessionKind != 0)
            return 0;
        if (notWhenWithin == pathManDrawnRiderWithin(pathMan, record->touchRangeSquared))
            return 0;
        memoryFill(event, 0, sizeof(pathman::ActivationEvent));
        event->quiet = 0;
        event->player = rider->player;
        activation->target = pathMan;
        activation->sender = ee::Address(rider->messenger);
        activation->type = pathman::kActivateMessage;
        activation->data = event.address();
        activation->flags = kMessageNow;
        return riderActivatedObject(activation) == 1 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(riderActivatesPathMan, Verified);

    // riderReachesPathMan: 1 when a rider's drawn body is within touching
    // range of a path man that is not down already.
    uint32_t riderReachesPathMan(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (pathManWasHit(pathMan) != 0)
            return 0;
        return pathManDrawnRiderWithin(pathMan, record->touchRangeSquared) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(riderReachesPathMan, Verified);

    // pathManIsHit: the hit state, and with it the "hit" phrase.
    void pathManIsHit(ee::Ptr<SceneObject> pathMan)
    {
        if (setPathManState(pathMan, kPathManHit, pathManBlendTime(pathMan)))
            game::tail(sayPathManPhrase, pathMan, kPhraseHit);
    }
    READABLE_FUNCTION_WAITS(pathManIsHit, Verified);

    // riderComesAtPathMan: whether the current player's rider is riding at
    // the path man: within its notice range, moving (its back wheel's speed
    // over the ground), and moving against the
    // way the path man's body faces. The answer is the side of the path man
    // the rider is on (1 or 2: either side of the upright plane through the
    // path man along its facing), 0 when the rider does not come at it or
    // the path man is down, dodging already or just has.
    uint32_t riderComesAtPathMan(ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<Vec4> position;
        ee::Local<Vec4> sideways;
        ee::Local<Vec4> facing;
        ee::Local<Vec4> plane;
        ee::Local<Vec4> up;
        const ee::Ptr<rider::Rider> rider = currentRider();
        const ee::Ptr<Vec4> at = pathManPosition(pathMan, position);
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (pathManWasHit(pathMan) != 0)
            return 0;
        if (pathManDrawnRiderWithin(pathMan, record->noticeRangeSquared) == 0)
            return 0;
        const int32_t state = record->animation.state;
        if (state == kPathManDodgeMirrored || state == kPathManDodge || state == kPathManAfterDodge)
            return 0;
        if (ee::abs(rider->wheelPoses[1].speed) < kNoSpeed)
            return 0;

        const ee::Ptr<Vec4> bodyFacing = pathManBodyFacing(pathMan, facing);
        if (!(dot(*bodyFacing, *vec4(rider->wheelPoses[1].velocity)) <= 0.0f))
            return 0;
        *up = kUp;
        *sideways = cross(*facing, *up);
        planeThrough(plane, at, sideways);
        return planeDistance(plane, rider::vec4(rider->drawnPosition)) <= 0.0f ? 1 : 2;
    }
    READABLE_FUNCTION_WAITS(riderComesAtPathMan, Verified);

    // pathManDodges: the dodge to the side away from the rider (the
    // mirrored one for side 2), and with it the "hit" phrase.
    void pathManDodges(ee::Ptr<SceneObject> pathMan, uint32_t side)
    {
        const int32_t state = side == 2 ? kPathManDodgeMirrored : kPathManDodge;
        if (setPathManState(pathMan, state, pathManBlendTime(pathMan)))
            game::tail(sayPathManPhrase, pathMan, kPhraseHit);
    }
    READABLE_FUNCTION_WAITS(pathManDodges, Verified);

    // riderLingersByPathMan: 1 when a rider has stayed within touching
    // range of a path man in the state after a hit for kLingerFrames.
    uint32_t riderLingersByPathMan(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->animation.state != kPathManAfterHit)
            return 0;
        return record->riderNearFrames >= kLingerFrames ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(riderLingersByPathMan, Verified);

    // sayRiderLingers: the "sit" phrase, and the count of frames starts
    // again.
    void sayRiderLingers(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        sayPathManPhrase(pathMan, kPhraseSit);
        record->riderNearFrames = 0;
    }
    READABLE_FUNCTION_WAITS(sayRiderLingers, Draft);

    // pathManMayGreet: 1 when its wait for a phrase has run out, a rider is
    // within greeting range and it is on its path or in the state after a
    // dodge.
    uint32_t pathManMayGreet(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->phraseFrames != 0)
            return 0;
        if (pathManRiderWithin(pathMan, kGreetRangeSquared) == 0)
            return 0;
        const int32_t state = record->animation.state;
        return state == kPathManOnPath || state == kPathManAfterDodge ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(pathManMayGreet, Verified);

    // pathManGreets: it waits a random time before its next phrase and
    // speaks to the rider: "proximity", or for a path man with an objective
    // the phrase for how far the objective has come ("proximity" before it
    // is active, "proximitypre" while it is, "proximitypost" once it is
    // done).
    void pathManGreets(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        record->phraseFrames = static_cast<uint16_t>(pathManRandom(pathMan, kPhraseWaitSpread) + kLeastPhraseWait);
        if (!record->objective)
        {
            game::tail(sayPathManPhrase, pathMan, kPhraseProximity);
            return;
        }
        const uint32_t state = zge::objectiveState(record->objective);
        if (state < 2)
            game::tail(sayPathManPhrase, pathMan, kPhraseProximity);
        else if (state < 4)
            game::tail(sayPathManPhrase, pathMan, kPhraseProximityPre);
        else if (state < 6)
            game::tail(sayPathManPhrase, pathMan, kPhraseProximityPost);
        else
            game::tail(sayPathManPhrase, pathMan, kPhraseProximity);
    }
    READABLE_FUNCTION_WAITS(pathManGreets, Verified);

    // pathManOutOfPlay: 1 when the path man is hidden, or its level node is
    // not drawn and no rider is within its awake range.
    uint32_t pathManOutOfPlay(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        const ee::Ptr<Node> node = objectLevelNode(pathMan);
        if ((pathMan->flags & kObjectShown) == 0)
            return 1;
        if (nodeNotDrawn(node) == 0)
            return 0;
        return pathManRiderWithin(pathMan, record->awakeRangeSquared) != 0 ? 0 : 1;
    }
    READABLE_FUNCTION_WAITS(pathManOutOfPlay, Verified);
}
