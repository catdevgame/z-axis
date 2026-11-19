// BMX XXX's "GRIND" objective type: the rider grinding (or stalling on) a
// list of edges (EDGE1 .. EDGE8, each an edge tag's word), in any order or
// one after the other (SEQUENTIAL), each for MIN_DISTANCE at least, and as
// an alley-oop (ALLEYOOP). BMX XXX's own code (its game block); the names
// are the script's keywords and the type's callbacks' places in its
// registration.

#include "objective_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/grind_type.cpp.h"

namespace game::bmx::objectives
{
    using rider::Rider;
    namespace collision = rider::collision;

    namespace
    {
        constexpr uint32_t kMaxEdgeTags = 8;
        // The block's keys: NUMEDGES, SEQUENTIAL, ALLEYOOP, MIN_DISTANCE, EDGE1 .. EDGE8.
        constexpr uint32_t kGrindKeys = 4 + kMaxEdgeTags;
        constexpr uint32_t kEdgeNameBytes = 0x40;
        constexpr uint32_t kMaxEdges = 32;

        // A GRIND objective's record (0x138 bytes, from the type's pool).
        struct GrindObjective
        {
            uint8_t flags;                    // +0x00 kSequential, kAlleyOop
            uint8_t _01;
            uint8_t tagCount;                 // +0x02 NUMEDGES (0 when the block was not read whole)
            uint8_t done;                     // +0x03 how many tags are done: the next tag's index when sequential
            uint8_t doneBits;                 // +0x04 a bit per tag done
            uint8_t _05[3];
            ee::Float minDistance;            // +0x08 MIN_DISTANCE in metres (2 when the script gives none)
            uint32_t tags[kMaxEdgeTags];      // +0x0C EDGE1 ..: each name's hash, an edge tag's word
            uint8_t _2c[4];
            uint64_t edges[kMaxEdges + 1];    // +0x30 every edge of the level with one of the tags, ended by a 0
        };
        static_assert(offsetof(GrindObjective, minDistance) == 0x08 && offsetof(GrindObjective, tags) == 0x0C);
        static_assert(offsetof(GrindObjective, edges) == 0x30 && sizeof(GrindObjective) == 0x138);
        constexpr uint8_t kSequential = 0x1; // SEQUENTIAL
        constexpr uint8_t kAlleyOop = 0x2;   // ALLEYOOP

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_grindObjectives> g_grindObjectives;

        constexpr ee::Float kDefaultMinDistance = 2.0f;
        // 0.3048 as the game has it (the float just under it).
        constexpr ee::Float kMetresPerFoot = std::bit_cast<float>(0x3E9C0EBEu); // not an address
        // The highlight's colour for an edge still to grind.
        constexpr uint32_t kEdgeHighlight = highlightColour(0x48, 0xE0, 0x60);
        // The bounds start inside out and end this much wider than the edges.
        constexpr ee::Float kFarAway = 99999.9f;
        constexpr ee::Float kFarBelow = -99999.9f;
        constexpr ee::Float kBoundsMargin = 20.0f;
        // The rider's contact kind on an edge (Rider::contactKind).
        // Scaffolding (unnamed-field): the rider lane has no name for it.
        constexpr uint8_t kContactEdge = 5;

        bool isDone(const GrindObjective &grind, uint32_t tag)
        {
            return (((&grind.doneBits)[tag >> 3] >> (tag & 7)) & 1) != 0;
        }
    }

    void readGrindParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readGrindBlock(ee::Ptr<const char> beginKeyword);
    void freeGrind(Objective objective);
    void resetGrind(Objective objective);
    void grindEvent(Objective objective, ee::Ptr<ObjectiveEvent> event);
    void grindCompleted();
    void highlightGrind(Objective objective, ee::Ptr<Rider> rider);
    void updateGrindBounds(Objective objective);
    uint32_t grindFrame(Objective objective, ee::Ptr<ObjectiveEvent> event);
    int32_t grindStatusText(Objective objective, ee::Ptr<char> text);

    // registerGrindType: the "GRIND" type and its callbacks.
    void registerGrindType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::grindTypeName);
        setup->hooks.readParameters = sym::readGrindParameters;
        setup->hooks.freeData = sym::freeGrind;
        setup->hooks.resetProgress = sym::resetGrind;
        setup->hooks.onEvent = sym::grindEvent;
        setup->hooks.onComplete = sym::grindCompleted;
        setup->hooks.drawForRider = sym::highlightGrind;
        setup->hooks.moveBounds = sym::updateGrindBounds;
        setup->hooks.inProgress = sym::grindFrame;
        setup->hooks.statusText = sym::grindStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerGrindType, Verified);

    // readGrindParameters (the type's readParameters): the block
    // GRIND_BEGIN .. GRIND_END is made readable inside the objective's
    // block, with the objective as its owner.
    void readGrindParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kGrindKeys]> keyNames;
        ee::Local<uint32_t[kGrindKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::grindKeyNames);
        for (uint32_t i = 0; i < kGrindKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kGrindKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::grindBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::grindEndKeyword);
        setup->keyCount = kGrindKeys;
        setup->onEnd = sym::readGrindBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::grindBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readGrindParameters, Verified);

    // readGrindBlock (the block's handler): the objective's record made
    // from the block's values. Each EDGE is a tag's name: its hash is kept,
    // and every edge of the level with that tag word joins the objective's
    // edges (32 in all at most). A block with no NUMEDGES, or with an EDGE
    // missing or naming a tag no edge has, leaves the objective with no
    // tags. Always 1.
    uint32_t readGrindBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> key;
        ee::Local<char[kMaxEdgeTags][kEdgeNameBytes]> tagNames;
        ee::Local<Objective> owner;
        bool whole = true;
        *owner = Objective(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<GrindObjective> grind(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_grindObjectives), sizeof(GrindObjective)));
        zge::memoryFill(grind, 0, sizeof(GrindObjective));
        grind->minDistance = kDefaultMinDistance;

        zge::formatString(key, sym::bmx::grindKeyNumEdges);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, key);
        bool counted = false;
        if (!value)
            whole = false;
        else
        {
            grind->tagCount = static_cast<uint8_t>(zge::scriptValueInteger(value));
            counted = grind->tagCount != 0;
        }
        if (counted)
        {
            uint32_t edgeCount = 0;
            for (uint32_t i = 0; i < grind->tagCount; ++i)
            {
                zge::formatString(key, sym::bmx::grindEdgeKeyFormat, i + 1);
                value = zge::findScriptValue(values, key);
                if (!value)
                {
                    whole = false;
                    continue;
                }
                const ee::Ptr<char> name(tagNames.address() + i * kEdgeNameBytes);
                zge::scriptValueText(value, name, kEdgeNameBytes);
                grind->tags[i] = zge::nameHash(name);
                const ee::Ptr<uint64_t> found = rider::collision::edgesTagged(&grind->tags[i], 1);
                if (!found)
                    whole = false;
                else if (found[0] != 0 && edgeCount < kMaxEdges)
                {
                    uint32_t next = 0;
                    do
                    {
                        grind->edges[edgeCount] = found[next];
                        ++next;
                        ++edgeCount;
                    } while (found[next] != 0 && edgeCount < kMaxEdges);
                }
                grind->edges[edgeCount] = 0;
            }

            zge::formatString(key, sym::bmx::grindKeySequential);
            value = zge::findScriptValue(values, key);
            if (value)
            {
                const uint32_t on = zge::scriptValueSwitch(value, grind->flags & kSequential);
                grind->flags = static_cast<uint8_t>((grind->flags & ~kSequential) | (on & 1));
            }

            zge::formatString(key, sym::bmx::grindKeyAlleyOop);
            value = zge::findScriptValue(values, key);
            if (value)
            {
                const uint32_t on = zge::scriptValueSwitch(value, (grind->flags >> 1) & 1);
                grind->flags = static_cast<uint8_t>((grind->flags & ~kAlleyOop) | ((on & 1) << 1));
            }

            zge::formatString(key, sym::bmx::grindKeyMinDistance);
            value = zge::findScriptValue(values, key);
            if (value)
                grind->minDistance = zge::scriptValueFloat(value) * kMetresPerFoot;
        }
        if (!whole)
            grind->tagCount = 0;
        zge::setObjectiveData(*owner, ee::Address(grind));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::grindBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readGrindBlock, Verified);

    // freeGrind (the type's free): the record back to the pool.
    void freeGrind(Objective objective)
    {
        const ee::Address grind = zge::objectiveData(objective);
        if (grind)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_grindObjectives), grind);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeGrind, Draft);

    // resetGrind (the type's reset): no tag done.
    void resetGrind(Objective objective)
    {
        const ee::Ptr<GrindObjective> grind(zge::objectiveData(objective));
        grind->done = 0;
        zge::memoryFill(&grind->doneBits, 0, sizeof(uint8_t));
    }
    READABLE_FUNCTION_WAITS(resetGrind, Verified);

    // grindEvent (the type's event), on the frame's event: while the
    // player's rider grinds or stalls on an edge, the tag of that edge is
    // done once the rider's first kind score (the grind's distance) has
    // reached MIN_DISTANCE, unless it is done already, is not the next one
    // of a sequence, or an alley-oop is asked and the newest trick line's
    // first trick is not marked as one. With every tag done the objective
    // is done.
    void grindEvent(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        const ee::Ptr<GrindObjective> grind(zge::objectiveData(objective));
        if (event->kind != kFrameEvent)
            return;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        if (rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionGrind &&
            rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionStall)
            return;
        if (rider->contactKind != kContactEdge)
            return;
        for (uint32_t i = 0; i < grind->tagCount; ++i)
        {
            const uint32_t tag = collision::edgeTagWord(rider::contactReference(*rider));
            if (tag != grind->tags[i])
                continue;
            if (isDone(*grind, i))
                return;
            if ((grind->flags & kSequential) != 0 && i != grind->done)
                return;
            if (!(grind->minDistance <= rider->score.kindScores[0]))
                return;
            if ((grind->flags & kAlleyOop) != 0)
            {
                const uint32_t lines = rider->score.lineCount;
                if (lines == 0)
                    return;
                const ee::Ptr<ScoreTrick> first = rider->score.lines[lines - 1].tricks;
                if ((first->flags & kScoreTrickAlleyOop) == 0)
                    return;
            }
            (&grind->doneBits)[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
            grind->done = static_cast<uint8_t>(grind->done + 1);
            if (grind->done == grind->tagCount)
                zge::setObjectiveState(objective, kObjectiveDone);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(grindEvent, Verified);

    // grindCompleted (the type's completed): nothing to do.
    void grindCompleted()
    {
    }
    READABLE_FUNCTION(grindCompleted, Verified);

    // highlightGrind (the type's highlight): every edge whose tag is still
    // to do is highlighted for the rider (when sequential, the next tag's
    // edges alone). The edges come tag by tag, so a tag is looked up only
    // when it changes from one edge to the next; an edge whose tag is none
    // of the objective's is judged by the bit after the last tag's.
    void highlightGrind(Objective objective, ee::Ptr<Rider> rider)
    {
        const ee::Ptr<GrindObjective> grind(zge::objectiveData(objective));
        uint32_t lastTag = 0;
        uint32_t which = 0;
        for (uint32_t edge = 0; grind->edges[edge] != 0;)
        {
            const uint32_t tag = collision::edgeTagWord(grind->edges[edge]);
            if (tag != lastTag)
            {
                const uint32_t count = grind->tagCount;
                for (which = 0; which < count; ++which)
                {
                    if (grind->tags[which] == tag)
                    {
                        lastTag = tag;
                        break;
                    }
                }
            }
            if (!isDone(*grind, which) && ((grind->flags & kSequential) == 0 || which == grind->done))
                highlightEdge(grind->edges[edge], &rider->position, kEdgeHighlight);
            ++edge;
            if (edge > kMaxEdges)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(highlightGrind, Verified);

    // updateGrindBounds (the type's updateBounds): the objective's bounds
    // made the box round its edges' ends, 20 units wider each way.
    void updateGrindBounds(Objective objective)
    {
        ee::Local<ee::Float[4]> point;
        const ee::Ptr<GrindObjective> grind(zge::objectiveData(objective));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        bounds->low.z = kFarAway;
        bounds->low.x = kFarAway;
        bounds->low.y = kFarAway;
        bounds->high.x = kFarBelow;
        bounds->high.z = kFarBelow;
        bounds->high.y = kFarBelow;
        for (uint32_t i = 0; grind->edges[i] != 0; ++i)
        {
            for (uint32_t end = 0; end < 2; ++end)
            {
                collision::edgePoint(grind->edges[i], end, point);
                growBounds(bounds, point);
            }
        }
        const ee::Float minX = bounds->low.x - kBoundsMargin;
        const ee::Float minY = bounds->low.y - kBoundsMargin;
        const ee::Float minZ = bounds->low.z - kBoundsMargin;
        const ee::Float maxX = bounds->high.x + kBoundsMargin;
        const ee::Float maxY = bounds->high.y + kBoundsMargin;
        const ee::Float maxZ = bounds->high.z + kBoundsMargin;
        bounds->low.x = minX;
        bounds->low.y = minY;
        bounds->low.z = minZ;
        bounds->high.x = maxX;
        bounds->high.z = maxZ;
        bounds->high.y = maxY;
    }
    READABLE_FUNCTION_WAITS(updateGrindBounds, Verified);

    // grindFrame (the type's frame): whether the player's rider is inside
    // the objective's bounds.
    uint32_t grindFrame(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        ee::Local<zge::Vec4> at;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        *at = *rider::vec4(rider->position);
        if (!(bounds->low.x <= at->x) || !(bounds->low.y <= at->y) || !(bounds->low.z <= at->z))
            return 0;
        if (!(at->x <= bounds->high.x) || !(at->y <= bounds->high.y) || !(at->z <= bounds->high.z))
            return 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(grindFrame, Verified);

    // grindStatusText (the type's statusText): the string table's text
    // "<STRINGID>_STATUS", a format, written with how many tags are done
    // and how many there are. 0 when the table has no such text.
    int32_t grindStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<GrindObjective> grind(zge::objectiveData(objective));
        zge::formatString(key, sym::bmx::grindStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        return zge::formatString(text, format, grind->done, grind->tagCount);
    }
    READABLE_FUNCTION_WAITS(grindStatusText, Verified);
}
