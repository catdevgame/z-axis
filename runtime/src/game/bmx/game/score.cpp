// BMX XXX's turn score records (score.h). BMX XXX's own code (game.c's
// block); DM2 has the same two functions over its smaller block.

#include "score.h"

#include "overlay_messages.h"
#include "player_riders.h"
#include "records.h"

#include "bmx/level/entities.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/game/score.cpp.h"

namespace game::bmx
{
    // openScoreLine: the block's next line opened: counted, with no trick
    // yet. Nothing checks the room for it.
    void openScoreLine(ee::Ptr<ScoreBlock> score)
    {
        ScoreLine &line = score->lines[score->lineCount];
        score->lineCount = score->lineCount + 1;
        line.gap = rider::kNoGap;
        line.tricks = {};
        line.flags = 0;
        line.landingGrade = 0;
        line.spin = 0;
        line.points = 0;
    }
    READABLE_FUNCTION(openScoreLine, Verified);

    // addScoreTrick: the block's next trick record taken, empty (no trick
    // in it yet), and chained at the end of the last line's tricks. Nothing
    // checks the room for it, and with no line open it is chained under the
    // record before the first line.
    void addScoreTrick(ee::Ptr<ScoreBlock> score)
    {
        const ee::Ptr<ScoreTrick> added(&score->tricks[score->trickCount]);
        score->trickCount = score->trickCount + 1;
        added->modifier = rider::kNoTrick;
        added->flags = 0;
        added->trick = rider::kNoTrick;
        added->loops = 0;
        added->turns = 0;
        added->framesHeld = 0;
        added->distance = 0.0f;
        added->points = 0;
        added->next = {};
        ScoreLine &line = *(ee::Ptr<ScoreLine>(&score->lines[0]) + (static_cast<int32_t>(score->lineCount) - 1));
        ee::Ptr<ScoreTrick> last = line.tricks;
        if (!last)
        {
            line.tricks = added;
            return;
        }
        while (last->next)
            last = last->next;
        last->next = added;
    }
    READABLE_FUNCTION(addScoreTrick, Verified);

    namespace
    {
        // A spin in half turns (0x8000 of the yaw's units), at most seven.
        constexpr uint32_t kHalfTurn = 0x8000;
        constexpr uint32_t kMostHalfTurns = 7;
    }

    // riderHasMoveToScore: the rider has spun at least half a turn (not
    // while the alignment turns it), acted, or has a trick or a trick
    // motion going.
    uint32_t riderHasMoveToScore(const rider::Rider *rider)
    {
        if (!rider::hasFlag(rider, kFlagAlignAbout))
        {
            const uint32_t yaw = static_cast<uint32_t>(rider->yawTurned);
            const uint32_t turned = rider->yawTurned < 0 ? 0u - yaw : yaw;
            uint32_t halfTurns = (turned + kHalfTurn / 2) / kHalfTurn;
            if (halfTurns > kMostHalfTurns)
                halfTurns = kMostHalfTurns;
            if (halfTurns != 0)
                return 1;
        }
        if (rider::hasFlag(rider, kFlagActed))
            return 1;
        if (rider->trick != rider::kNoTrick)
            return 1;
        return rider->trickMotion != rider::kNoTrick ? 1 : 0;
    }
    READABLE_FUNCTION(riderHasMoveToScore, Verified);

    namespace
    {
        // The crowd's sound for a line's end (the game bank's), loud by the
        // line's points over 4000 (a level of 0 .. 5), or the groan of a
        // bail (level 2); and what the path men near the rider are told.
        constexpr uint32_t kSoundLineReaction = 0x3B;
        constexpr int32_t kCheeredPoints = 4000;
        inline ee::Global<ee::Float, sym::g_cheerPerPoint> g_cheerPerPoint; // 0.0005: a level each 2000 points
        constexpr int32_t kLoudestCheer = 5;
        constexpr uint32_t kBailGroan = 2;
        constexpr uint32_t kPathMenSeeLine = 0, kPathMenSeeBail = 1;
    }

    // lineReaction (inferred name): the crowd and the path men react to a
    // line that ends, landed (a cheer by its points) or in a bail.
    int32_t lineReaction(uint32_t landed, ee::Ptr<ScoreBlock> score)
    {
        if (landed)
        {
            const int32_t over = static_cast<int32_t>(score->linePoints) - kCheeredPoints;
            int32_t level = kLoudestCheer;
            if (ee::toInt(ee::toFloat(over) * *g_cheerPerPoint) <= kLoudestCheer)
                level = ee::toInt(ee::toFloat(over) * *g_cheerPerPoint);
            soundEvent(kSoundLineReaction, static_cast<uint32_t>(level), 1, 0, 0);
            return game::tail(tellPathMenNearRider, kPathMenSeeLine);
        }
        soundEvent(kSoundLineReaction, kBailGroan, 0, 0, 0);
        return game::tail(tellPathMenNearRider, kPathMenSeeBail);
    }
    READABLE_FUNCTION(lineReaction, Verified);

    namespace
    {
        // The wheel contact kinds trickLineEvent tells apart (rider.h's):
        // a rail's edge, a kind whose node's object is taken, and three it
        // gives up on (BMX XXX's own, not named).
        constexpr uint8_t kContactRailEdge = 5, kContactNodeObject = 6;
        constexpr uint8_t kContactKind7 = 7, kContactKind10 = 10, kContactKind11 = 11;
        // What a wheel's contact was reduced to (kept beside its tag, read
        // by nothing).
        constexpr uint32_t kTaggedEdge = 5, kTaggedFace = 7, kTaggedObject = 9;
        // The landing grades (ScoreLine::landingGrade) and the travel angles
        // that give them (DM2's).
        constexpr uint8_t kLandedStraight = 1, kLandedAskew = 2, kLandedBackwards = 3;
        constexpr int32_t kStraightBelow = 0x71C, kAskewFrom = 0x11C8, kBackwardsAbove = 0x4000;
        // The jump's distance (the two wheels' contacts, flat) and its
        // reward display item.
        constexpr uint32_t kRewardJumpDistance = 0x19;

        // The scene object a contact's node belongs to (0: none).
        ee::Ptr<zge::SceneObject> nodeObject(uint32_t node)
        {
            const ee::Ptr<zge::Node> owner(static_cast<uint32_t>(rider::collision::nodeOf(rider::collision::widen(node))));
            return zge::nodeSceneObject(owner, ee::Ptr<ee::Ptr<zge::SceneBone>>());
        }
    }

    // trickLineEvent (r9bmxrider3's name; DM2's trickEvent): a landing
    // tags each wheel's contact (an object's name, a rail's or a face's tag;
    // a contact of kinds 7, 10 or 11, or a node with no object, ends it
    // there), checks the jump's flat distance against its record, grades
    // the last line's landing by the travel angle when it was found, scores
    // the lines and writes the trick display's text. Either way, the
    // objectives hear of it.
    void trickLineEvent(uint32_t player, uint32_t landed)
    {
        ee::Local<uint32_t[2]> tags; // (a wheel with no contact leaves its tag as the stack had it)
        ee::Local<uint32_t[2]> kinds;
        ee::Local<zge::Vec4> between;
        ee::Local<TrickDisplayMessage> message;

        player &= 0xFF;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        ScoreBlock &score = rider->score;
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
                case kContactRailEdge:
                {
                    // A rail on an object counts as the object.
                    const ee::Ptr<zge::SceneObject> owner = nodeObject(object);
                    if (owner)
                    {
                        tag = zge::objectNameHash(owner);
                        kind = kTaggedObject;
                    }
                    else
                    {
                        tag = rider::collision::edgeTagWord(ref);
                        kind = kTaggedEdge;
                    }
                    break;
                }
                case kContactNodeObject:
                {
                    const ee::Ptr<zge::SceneObject> owner = nodeObject(object);
                    if (!owner)
                        return;
                    tag = zge::objectNameHash(owner);
                    kind = kTaggedObject;
                    break;
                }
                case rider::kContactObject:
                    tag = zge::objectNameHash(ee::Ptr<zge::SceneObject>(object));
                    kind = kTaggedObject;
                    break;
                case kContactKind7:
                case kContactKind10:
                case kContactKind11:
                    return;
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
            checkGameRecord(player, kRecordJumpDistance, kRewardJumpDistance, 0, zge::vec3Length(&between->xyz()));

            if (score.lineCount != 0)
            {
                rider::ScoreLine &line = score.lines[score.lineCount - 1];
                line.landingGrade = 0;
                if (rider::hasFlag(&*rider, rider::kFlagTravelFound))
                {
                    const int32_t angle = rider->landingTravelAngle;
                    if (angle > kBackwardsAbove)
                        line.landingGrade = kLandedBackwards;
                    else if (angle < kStraightBelow)
                        line.landingGrade = kLandedStraight;
                    else if (angle >= kAskewFrom)
                        line.landingGrade = kLandedAskew;
                }
            }
            scoreLines(player);
            message->score = ee::Ptr<ScoreBlock>(&score);
            message->action = kTrickDisplayTrickText;
            message->player = static_cast<uint8_t>(player);
            overlayMessage(kOverlayTrick, message);
        }
        objectives::sendTrickEvent(static_cast<uint8_t>(player), landed);
    }
    READABLE_FUNCTION(trickLineEvent, Verified);
}
