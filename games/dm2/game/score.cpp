// DM2's trick scoring (game.c): the turn's trick lines.

#include "score.h"
#include "goals.h"
#include "gaps.h"
#include "players.h"
#include "records.h"

#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/collision/collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/dm2/game/score.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The contact kinds below this are faces; what markAirGap makes of
        // a contact: a face with its tag read, or nothing.
        constexpr uint32_t kFaceContactKinds = 5;
        constexpr uint32_t kNoContact = 0;
        // The two contact kinds before an object's count as none.
        constexpr uint32_t kIgnoredContactKinds = 6;
        // Rider::flags (the rider's files keep these two to themselves:
        // inferred there): the alignment turns the air directly, so its
        // yaw is no spin; an action was taken in the air.
        constexpr uint32_t kFlagAlignAbout = 0x4000;
        constexpr uint32_t kFlagActed = 0x100;
        // An air's spin counts in half turns (an Angle's 0x8000), rounded, seven at most.
        constexpr uint32_t kHalfTurn = 0x8000;
        constexpr uint32_t kMostHalfTurns = 7;
        // Rider::flags: the landing's travel along the ground was found
        // (Rider::landingTravelAngle is its angle; rider/physics/air_move.cpp).
        constexpr uint32_t kFlagTravelFound = 0x40000000;
        // The landing grades (ScoreLine::landingGrade) and the travel angles that give them.
        constexpr uint8_t kLandedStraight = 1, kLandedAskew = 2, kLandedBackwards = 3;
        constexpr int32_t kStraightBelow = 0x71C, kAskewFrom = 0x11C8, kBackwardsAbove = 0x4000;

    }

    void openScoreLine(rider::ScoreBlock *score)
    {
        rider::ScoreLine &line = score->lines[score->lineCount];
        ++score->lineCount;
        line.gap = kNoGap;
        line.tricks = ee::Ptr<rider::ScoreTrick>();
        line.flags = 0;
        line.landingGrade = 0;
        line.spinHalfTurns = 0;
        line.points = 0;
    }
    READABLE_FUNCTION(openScoreLine, Verified);

    void newScoreTrick(rider::ScoreBlock *score)
    {
        rider::ScoreTrick &trick = score->tricks[score->trickCount];
        ++score->trickCount;
        trick.trickMotion = rider::kNoTrick;
        trick.flags = 0;
        trick.trick = rider::kNoTrick;
        trick.loops = 0;
        trick.turns = 0;
        trick.framesHeld = 0;
        trick.distance = 0.0f;
        trick.points = 0;
        trick.next = ee::Ptr<rider::ScoreTrick>();

        rider::ScoreLine &line = score->lines[score->lineCount - 1];
        if (!line.tricks)
        {
            line.tricks = &trick;
            return;
        }
        ee::Ptr<rider::ScoreTrick> last = line.tricks;
        while (last->next)
            last = last->next;
        last->next = &trick;
    }
    READABLE_FUNCTION(newScoreTrick, Verified);

    void markAirGap(uint32_t player)
    {
        ee::Local<OverlayTrick> begun;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;
        if (rider::actionState(rider, rider::kLevelAction) != rider::kActionAir)
            return;

        // The back wheel's contact, then the rider's own.
        uint32_t kinds[2] = {rider->wheels[0].kind, rider->contactKind};
        uint32_t tags[2];
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (kinds[i] < kFaceContactKinds)
            {
                rider::collision::FaceRef face;
                std::memcpy(&face, i == 0 ? rider->wheels[0].contact : rider->contact, sizeof(face));
                tags[i] = rider::collision::faceTagWord(face);
                kinds[i] = kTaggedFace;
            }
            else if (kinds[i] >= kIgnoredContactKinds && kinds[i] < rider::kContactObject)
            {
                kinds[i] = kNoContact;
            }
        }
        if (kinds[0] != kTaggedFace || kinds[1] != kTaggedFace)
            return;
        const uint32_t gap = findGap(tags[0], tags[1]);
        if (gap == kNoGap)
            return;
        if (score.lineCount == 0)
        {
            openScoreLine(&score);
            newScoreTrick(&score);
            score.lineState = kLineTrick;
            begun->action = kTrickDisplayTrickBegun;
            begun->turn = ee::Address(&score.turnScore);
            overlayMessage(kOverlayTrick, begun);
        }
        rider::ScoreLine &line = score.lines[score.lineCount - 1];
        if (line.gap != gap)
            line.gap = static_cast<uint8_t>(gap);
    }
    READABLE_FUNCTION(markAirGap, Verified);

    uint32_t riderHasMoveToScore(const rider::Rider *rider)
    {
        if ((rider->flags & kFlagAlignAbout) == 0)
        {
            const uint32_t turned = static_cast<uint32_t>(rider->yawTurned < 0 ? -rider->yawTurned : rider->yawTurned);
            uint32_t halfTurns = (turned + kHalfTurn / 2) / kHalfTurn;
            if (halfTurns > kMostHalfTurns)
                halfTurns = kMostHalfTurns;
            if (halfTurns != 0)
                return 1;
        }
        if ((rider->flags & kFlagActed) != 0)
            return 1;
        if (rider->trick != rider::kNoTrick)
            return 1;
        return rider->trickMotion != rider::kNoTrick ? 1 : 0;
    }
    READABLE_FUNCTION(riderHasMoveToScore, Verified);

    void trickEvent(uint32_t player, uint32_t landed)
    {
        ee::Local<uint32_t[2]> tags;  // (a wheel with no contact leaves its tag as the stack had it)
        ee::Local<uint32_t[2]> kinds;
        ee::Local<zge::Vec4> between;
        ee::Local<OverlayTrick> text;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        rider::ScoreBlock &score = rider->score;
        if (landed != 0)
        {
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                const rider::WheelContact &contact = rider->wheels[wheel];
                uint32_t object;
                uint64_t ref;
                std::memcpy(&object, contact.contact, sizeof(object));
                std::memcpy(&ref, contact.contact, sizeof(ref));
                if (object == 0)
                    continue;
                uint32_t tag;
                uint32_t kind;
                switch (contact.kind)
                {
                case kContactKind6:
                case kContactKind9:
                case kContactKind10:
                    return;
                case rider::kContactObject:
                    tag = zge::objectNameHash(ee::Ptr<zge::SceneObject>(object));
                    kind = kTaggedObject;
                    break;
                case kContactRailEdge:
                {
                    // A rail on an object counts as the object.
                    const uint64_t node = rider::collision::nodeOf(rider::collision::widen(object));
                    const uint32_t owner = rider::collision::engine::nodeObject(node, 0u);
                    if (owner != 0)
                    {
                        tag = zge::objectNameHash(ee::Ptr<zge::SceneObject>(owner));
                        kind = kTaggedObject;
                    }
                    else
                    {
                        tag = rider::collision::edgeTagWord(ref);
                        kind = kTaggedEdge;
                    }
                    break;
                }
                default:
                    tag = rider::collision::faceTagWord(ref);
                    kind = kTaggedFace;
                    break;
                }
                tags[wheel] = tag;
                kinds[wheel] = kind;
            }

            const zge::Vec4 *const from = rider::vec4(rider->wheels[0].point);
            const zge::Vec4 *const to = rider::vec4(rider->wheels[1].point);
            *between = *to - *from;
            between->y = 0.0f;
            checkGameRecord(player, kContactDistanceRecord, kRewardContactDistance, 0, zge::vec3Length(&between->xyz()));

            if (score.lineCount != 0)
            {
                rider::ScoreLine &line = score.lines[score.lineCount - 1];
                line.landingGrade = 0;
                if ((rider->flags & kFlagTravelFound) != 0)
                {
                    const int32_t angle = rider->landingTravelAngle;
                    if (angle > kBackwardsAbove)
                        line.landingGrade = kLandedBackwards;
                    else if (angle < kStraightBelow)
                        line.landingGrade = kLandedStraight;
                    else if (angle >= kAskewFrom)
                        line.landingGrade = kLandedAskew;
                }
                const uint32_t gap = findGap(tags[0], tags[1]);
                if (gap != kNoGap && line.gap != gap)
                    line.gap = static_cast<uint8_t>(gap);
            }
            text->action = kTrickDisplayTrickText;
            text->turn = ee::Address(&score.turnScore);
            overlayMessage(kOverlayTrick, text);
        }
        goalObjectTrick(player, landed);
    }
    READABLE_FUNCTION(trickEvent, Verified);
}
