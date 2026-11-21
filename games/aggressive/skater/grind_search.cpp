// Finding a rail and getting onto it: a grind trick asked of the Ground
// or Air state, the rail search under the skater (ordinary rails, and the
// two edges of a double rail for the Double Grind), and the step onto a
// rail (the grind's enter, and every rail joined on from the end of the
// one ground).
//
// A rail is an edge of a scene object's grind-edge stream
// (zge/collision/collision.h, TGFE); the search is a sphere query about
// the skater whose filter (railFilter) picks the edge and writes the
// rail record (Rail below, Player::rail).

#include "aggressive/level/double_rails.h"
#include "moves.h"
#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/closest.h"
#include "zge/collision/collision.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "aggressive/level/objectives.h"

#include "sym/aggressive/skater/grind_search.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        using zge::Vec4;

        // closestRailEdge's kinds and tuning.
        constexpr uint32_t kVerticalPoleEdge = 3;
        constexpr uint32_t kVPoleMinSpeed = 13; // Action tuning: VPole Min SPeed

        // Grind-edge tags (the rail filter's third word): 0 an ordinary
        // rail, 5 and 6 the two edges of a double rail.
        constexpr uint32_t kOrdinaryRail = 0;
        constexpr uint32_t kDoubleRailEdgeA = 5;
        constexpr uint32_t kDoubleRailEdgeB = 6;

        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;
        inline ee::Global<ee::Float, sym::g_stillSpeed> g_stillSpeed;      // 1e-5
        inline ee::Global<ee::Float, sym::g_degreesToAngle4> g_degreesToAngle;  // 182.04445
        inline ee::Global<ee::Float, sym::g_samePoint> g_samePoint;       // 1e-5: a rail with its ends this close is none
        inline ee::Global<ee::Float, sym::g_joinStillSpeed> g_joinStillSpeed;  // 1e-5
        inline ee::Global<ee::Float, sym::g_squareToRail> g_squareToRail;    // 1e-5
        inline ee::Global<ee::Float, sym::g_joinDegreesToAngle> g_joinDegreesToAngle; // 182.04445
        inline ee::Global<ee::Float, sym::g_steepRail> g_steepRail;       // 0.97
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        enum GrindWallsEntry : uint32_t
        {
            kMinGrindAngle = 0,
            kStallSpeed = 1,
            kStallTimeout = 18,
            kGrindRadius = 23,
            kGrindRadiusGround = 24,
            kGrindRadiusQPipe = 25,
            kGrindApproachAngle = 27,
            kGrindSpeedLoss = 29,
        };
        ee::Float grindWalls(uint32_t entry) { return tuning(14, entry); }

        using Rail = RailRecord;

        // The rail filter's context (searchRails' frame).
        struct RailSearch
        {
            ee::Ptr<Player> player; // +0x00
            ee::Ptr<Rail> rail;     // +0x04 the record to fill
            ee::Float radius;       // +0x08 the nearest edge's distance so far (the filter lowers it)
            uint32_t tag;           // +0x0C the grind-edge tag wanted (0: any edge marked grindable)
            ee::Float minAngle;     // +0x10 the least |cos| between heading and edge (Min Grind Angle
                                    // going on from a rail's end; the vault's Vault Check Angle), else 0
            zge::Vec4 nearest;      // +0x20 the point of the rail ground nearest the skater (kZero without one)
            uint32_t vault;         // +0x30 1: the vault's search (actionRequest), else 0
        };
        static_assert(offsetof(RailSearch, nearest) == 0x20 && offsetof(RailSearch, vault) == 0x30);

        // The double rail of a double rail's edge (pairedRail;
        // Player::doubleGrindRails while on one; level/double_rails.h).
        using level::DoubleRail;

        // A grind edge's record (TGFE; docs/DEVTOOLS.md): +0x04 the edge,
        // +0x08 its tag, +0x09 grindable (cleared for an edge with no length).
        // The tag table's calls take an argument they do not read.
        constexpr uint32_t kEdgeId = 0x04, kEdgeTag = 0x08, kEdgeGrindable = 0x09;
        constexpr uint32_t kByteSize = 2u, kWordSize = 8u;
        uint32_t edgeTag(ee::Ptr<zge::TagRecord> record) { return zge::tagByte(record, kByteSize, kEdgeTag); }
        bool edgeGrindable(ee::Ptr<zge::TagRecord> record) { return zge::tagByte(record, kByteSize, kEdgeGrindable) != 0; }
        int32_t edgeId(ee::Ptr<zge::TagRecord> record) { return zge::tagWord(record, kWordSize, kEdgeId); }
        void markNotGrindable(ee::Ptr<zge::TagRecord> record) { zge::setTagByte(record, kByteSize, kEdgeGrindable, 0u); }
        bool isDoubleRailTag(uint32_t tag) { return tag == kDoubleRailEdgeA || tag == kDoubleRailEdgeB; }
        // Tags 8 and 9 (not met in the levels' records) are never vaulted.
        bool neverVaulted(uint32_t tag) { return tag == 8u || tag == 9u; }

        // Action tuning entries (docs/DEVTOOLS.md), the vault's.
        constexpr uint32_t kAction = 15;
        constexpr uint32_t kVaultMinHeight = 6, kVaultMaxHeight = 7, kVaultApproachAngle = 8;
        // Grind/Walls / Sloppy Vert Weld: two ends this close are one point.
        constexpr uint32_t kSloppyVertWeld = 49;
        ee::Float sloppyWeld() { return grindWalls(kSloppyVertWeld); }

        // 1e-5 each: a vector this short has no direction; an edge
        // this short has no length; the vault's nearest point this close to
        // the skater is under it.
        inline ee::Global<ee::Float, sym::g_noDirection> g_noDirection;
        inline ee::Global<ee::Float, sym::g_noLength> g_noLength;
        inline ee::Global<ee::Float, sym::g_underSkater> g_underSkater;

        // |a - b| < tolerance on x, then y, then z.
        bool samePoint(const zge::Vec4 &a, const zge::Vec4 &b, ee::Float tolerance)
        {
            return ee::abs(a.x - b.x) < tolerance && ee::abs(a.y - b.y) < tolerance && ee::abs(a.z - b.z) < tolerance;
        }

        // Where the skater heads: its motion, or where it faces when
        // (nearly) still.
        void headingOf(Player &player, zge::Vec4 &out, ee::Float stillSpeed)
        {
            if (ee::abs(player.speed) < stillSpeed)
                out = player.axes.forward;
            else
                out = zge::unit(player.velocity);
        }
        // The same, still by the velocity's length.
        void headingByVelocity(Player &player, zge::Vec4 &out)
        {
            if (ee::abs(vec3Length(player.velocity)) < *g_noDirection)
                out = player.axes.forward;
            else
                out = zge::unit(player.velocity);
        }
        // Grind Approach Angle's cosine.
        ee::Float approachCos(ee::Float degreesToAngle)
        {
            const ee::Float angle = grindWalls(kGrindApproachAngle) * degreesToAngle;
            return zge::angleCos(static_cast<zge::Angle>(ee::toInt(angle)));
        }

        // Whether the double rail the skater is on is the one this edge of it
        // makes (each end at one of the other's, either way round).
        bool sameDoubleRail(const DoubleRail &now, const DoubleRail &other)
        {
            const bool startMatches = samePoint(now.start, other.start, sloppyWeld()) || samePoint(now.start, other.end, sloppyWeld());
            return startMatches && (samePoint(now.end, other.start, sloppyWeld()) || samePoint(now.end, other.end, sloppyWeld()));
        }

        // One grind edge for railFilter below: the record it fills if this
        // is the nearest that can be taken so far.
        void considerEdge(RailSearch &search, ee::Ptr<zge::Node> node, ee::Ptr<zge::ZgePart> part, ee::Ptr<zge::TagTableHeader> edges, uint32_t index)
        {
            ee::Local<zge::Matrix> world;
            ee::Local<Vec4> a; // the edge's ends, in world space
            ee::Local<Vec4> b;
            ee::Local<Vec4> direction;
            ee::Local<Vec4> onEdge; // the edge's point nearest the skater
            ee::Local<Vec4> toEdge;
            ee::Local<Vec4> heading;
            ee::Local<Vec4> ahead;    // the vault: position + velocity
            ee::Local<Vec4> onMotion; // the vault: the motion's point nearest the edge
            ee::Local<ee::Ptr<uint16_t>> vertices;
            ee::Local<ee::Float> alongEdge; // how far along each segment those nearest points are
            ee::Local<ee::Float> alongMotion;

            Player &player = *search.player;
            const ee::Ptr<zge::TagRecord> record = zge::tagRecordAt(edges, index);
            const uint32_t tag = edgeTag(record);

            // A double rail's edge while on a double rail: not the pair now
            // ground (either way round).
            if (isDoubleRailTag(search.tag) && isDoubleRailTag(tag) && player.doubleGrindRails)
            {
                ee::Ptr<DoubleRail> other = aggressive::level::pairedRail(edgeId(record), node, kDoubleRailEdgeA);
                if (!other)
                    other = aggressive::level::pairedRail(edgeId(record), node, kDoubleRailEdgeB);
                if (sameDoubleRail(*player.doubleGrindRails, *other))
                    return;
            }

            // An ordinary search takes any grindable edge; a tagged one its tag.
            const bool wanted = search.tag == 0 ? edgeGrindable(record) : search.tag == tag;
            if (!wanted)
                return;

            // The edge's ends in world space.
            zge::elementEdgeVertices(part, edgeId(record), &*vertices);
            zge::vertexPosition(part, (*vertices)[0], a);
            zge::vertexPosition(part, (*vertices)[1], b);
            *world = *zge::nodeMatrix(node, 1);
            *a = zge::transformed(*world, *a);
            *b = zge::transformed(*world, *b);

            // An edge with no length is marked not grindable.
            if (samePoint(*b, *a, *g_noLength))
            {
                markNotGrindable(record);
                return;
            }

            // The rail now ground (an end at each of its ends) is not taken again.
            const Rail &current = player.rail;
            const bool atStart = samePoint(current.start, *a, sloppyWeld()) || samePoint(current.start, *b, sloppyWeld());
            if (atStart && (samePoint(current.end, *a, sloppyWeld()) || samePoint(current.end, *b, sloppyWeld())))
                return;

            *direction = *b - *a;
            *direction = zge::unit(*direction);
            if (search.vault != 1)
            {
                // The edge's point nearest the skater.
                zge::closestPointOnSegment(a, b, direction, &player.position, onEdge);
                *toEdge = *onEdge - player.position;
            }
            else
            {
                // The vault: the edge's point nearest the skater's motion this
                // frame, within the vault's heights below the skater (y grows
                // down), not under it, ahead within Vault Check Angle and the
                // edge across the way within Vault Approach Angle.
                if (neverVaulted(tag))
                    return;
                *ahead = player.position + player.velocity;
                if (!aggressive::level::closestBetweenSegments(a, b, &player.position, ahead, onEdge, onMotion, alongEdge, alongMotion))
                    return;
                const ee::Float drop = onEdge->y - onMotion->y;
                if (-tuning(kAction, kVaultMinHeight) < drop)
                    return;
                if (drop < -tuning(kAction, kVaultMaxHeight))
                    return;
                headingByVelocity(player, *heading);
                if (samePoint(*onMotion, player.position, *g_underSkater))
                    return;
                *toEdge = *onMotion - player.position;
                *toEdge = zge::unit(*toEdge);
                if (zge::dot(*heading, *toEdge) < search.minAngle)
                    return;
                *direction = *a - *b;
                *direction = zge::unit(*direction);
                if (tuning(kAction, kVaultApproachAngle) < ee::abs(zge::dot(*heading, *direction)))
                    return;
                *toEdge = *onEdge - player.position;
            }

            // Within the radius (the nearest so far), and ahead of the skater
            // or an end at the point of the rail now ground.
            const ee::Float distance = vec3Length(*toEdge);
            if (!(distance < search.radius))
                return;
            headingByVelocity(player, *heading);
            if (ee::abs(vec3Length(*toEdge)) < *g_noDirection)
                *toEdge = *heading;
            else
                *toEdge = zge::unit(*toEdge);
            if (!(ee::Float(0.0f) <= zge::dot(*heading, *toEdge)) &&
                !samePoint(*a, search.nearest, sloppyWeld()) && !samePoint(*b, search.nearest, sloppyWeld()))
                return;

            // Along the heading enough (the vault checked its own angles).
            if (search.vault != 1 && !(search.minAngle <= ee::abs(zge::dot(*heading, *direction))))
                return;
            search.radius = distance;

            // Taken (a nearer one may follow).
            const int32_t edge = edgeId(record);
            Rail &rail = *search.rail;
            rail.surface = static_cast<uint32_t>(edge);
            rail.node = node;
            rail.found = part;
            rail.start = *a;
            rail.end = *b;
            rail.direction = *direction;
            rail.joinPoint = *onEdge;
        }

        // A hit's face edges: its collision part's grind-edge stream.
        ee::Ptr<zge::TagTableHeader> grindEdgesOf(ee::Ptr<zge::ZgePart> part) { return part->faceEdges.cast<zge::TagTableHeader>(); }
    }

    // railFilter, the rail search's filter (searchRails' sphere query
    // calls it for each scene object it meets, with the search's context):
    // over the object's grind edges (its collision part's edge stream, +0x4C)
    // the nearest within the context's radius that the skater can get onto:
    // of the tag asked for (any grindable edge for tag 0; an edge with no
    // length is marked not grindable), not the rail or double rail now
    // ground, ahead of the skater (or joined to the rail now ground at its
    // nearest point), along its heading within the context's least angle;
    // for the vault (actionRequest) an edge below the skater's motion this
    // frame within the vault's heights and angles. The edge goes to the
    // context's Rail record, its distance becomes the radius. Returns 0.
    //
    int32_t railFilter(ee::Ptr<RailSearch> search, ee::Ptr<zge::Node> node)
    {
        const ee::Ptr<zge::ZgePart> part(collisionMeshOf(node));
        ee::Ptr<zge::TagTableHeader> edges;
        uint32_t count = 0;
        if (part)
            edges = grindEdgesOf(part);
        if (edges)
            count = zge::tagRecordCount(edges);
        for (uint32_t i = 0; i < count; ++i)
            considerEdge(*search, node, part, edges, i);
        return 0;
    }
    READABLE_FUNCTION(railFilter, Verified);

    namespace
    {
        constexpr uint32_t kQuerySphere = 0; // zge::CollisionKind::Sphere
    }

    // searchRails: the rails about the skater: a sphere query of `radius`
    // about its position whose filter takes a grind edge of `tag` and fills
    // the rail record (the found flag cleared first). The point of the rail
    // now ground nearest the skater goes to the filter (VF0 when that rail's
    // ends coincide). `vault` 1 is the vault's search (actionRequest, its
    // radius and angle Vault Check Radius and Angle). Returns whether a rail
    // was found (0 with no record).
    int32_t searchRails(ee::Ptr<Player> player, ee::Ptr<RailRecord> rail, uint32_t tag, uint32_t vault, ee::Float radius,
                        ee::Float minAngle)
    {
        if (!rail)
            return 0;
        ee::Local<zge::QuerySphere> sphere;
        ee::Local<zge::QueryResults> results;
        ee::Local<RailSearch> search;
        const Rail &current = player->rail;

        search->minAngle = minAngle;
        search->tag = tag;
        search->vault = vault;
        search->rail = rail;
        search->player = player;
        search->radius = radius;
        if (samePoint(current.start, current.end, *g_samePoint))
            search->nearest = zge::kZero;
        else
            zge::closestPointOnSegment(&current.start, &current.end, 0u, &player->position, &search->nearest);
        sphere->radius = radius;
        rail->found = {};
        sphere->centre = player->position;
        zge::collisionQuery(kQuerySphere, sphere, 0, sym::railFilter, search, 0, results, 0);
        return rail->found ? 1 : 0;
    }
    READABLE_FUNCTION(searchRails, Verified);

    // closestRailEdge: the rail edge a hand can take (actionRequest, over the
    // objects of the Action Check Radius sphere): over each face of each hit
    // its three edges, of kind `kind` (level/rails_setup.cpp's edge kinds: 3
    // a vertical pole, only at VPole Min SPeed or more, 4 a horizontal one,
    // 7 a car) or, for kind 0, any edge marked grindable; the nearest within
    // 6 units that the skater looks at (the cosine from its motion, or from
    // its forward standing still, to the edge's nearest point at least
    // `minFacing`). The edge fills the Rail record. Returns whether one did.
    uint32_t closestRailEdge(ee::Ptr<Player> player, ee::Ptr<RailRecord> edge, ee::Ptr<zge::QueryResults> hits, uint32_t kind,
                             ee::Float minFacing)
    {
        ee::Local<Vec4> start;
        ee::Local<Vec4> end;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> nearestPoint;
        ee::Local<Vec4> toEdge;
        ee::Local<Vec4> looking;
        ee::Local<zge::Matrix> matrix;
        ee::Local<ee::Ptr<uint16_t>> vertices;

        constexpr ee::Float kStill(1e-05f);
        ee::Float nearest(6.0f);

        if (!edge)
            return 0;
        edge->object = 0;
        edge->found = {};
        uint32_t found = 0;
        for (int32_t n = 0; n < hits->count; ++n)
        {
            const zge::HitNode &hit = *hits->nodes[n];
            for (uint32_t f = 0; f < hit.faceCount; ++f)
            {
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    const uint32_t face = hit.faces[f].face;
                    const uint32_t edgeIndex = static_cast<uint32_t>(zge::faceCornerIndex(hit.part, face, corner));
                    const bool wanted = kind == 0 ? railEdgeAnyKind(hit.node, edgeIndex) != 0 : railEdgeKind(hit.node, edgeIndex) == kind;
                    if (!wanted)
                        continue;
                    if (kind == kVerticalPoleEdge && player->speed < tuningValue(kTuningAction, kVPoleMinSpeed))
                        continue;

                    zge::edgeVertices(hit.part, face, corner, &*vertices);
                    zge::vertexPosition(hit.part, (*vertices)[0], start);
                    zge::vertexPosition(hit.part, (*vertices)[1], end);
                    *matrix = *zge::nodeMatrix(hit.node, 1);
                    *start = zge::transformed(*matrix, *start);
                    *end = zge::transformed(*matrix, *end);
                    *direction = *end - *start;
                    *direction = zge::unit(*direction);
                    zge::closestPointOnSegment(start, end, direction, &player->position, nearestPoint);
                    *toEdge = *nearestPoint - player->position;
                    const ee::Float distance = vec3Length(*toEdge);
                    if (!(distance < nearest))
                        continue;

                    const bool still = ee::abs(player->speed) < kStill;
                    *looking = zge::unit(still ? player->axes.forward : player->velocity);
                    *toEdge = zge::unit(*toEdge);
                    if (!(minFacing <= zge::dot(*looking, *toEdge)))
                        continue;

                    edge->surface = edgeIndex;
                    nearest = distance;
                    edge->node = hit.node;
                    edge->found = hit.part;
                    edge->start = *start;
                    edge->end = *end;
                    edge->direction = *direction;
                    edge->joinPoint = *nearestPoint;
                    found = 1;
                }
            }
        }
        return found;
    }
    READABLE_FUNCTION(closestRailEdge, Verified);

    // findGrindRail: a rail near the skater, into Player::rail. On the
    // ground the radius is Grind Radius Ground (Grind Radius QPipe on a
    // quarter pipe's lip); grinding (a Grind or a Double Grind going on
    // from the rail's end) Grind Radius with Min Grind Angle; else Grind
    // Radius. Returns whether one was found.
    int32_t findGrindRail(ee::Ptr<Player> player)
    {
        ee::Local<Rail> found;
        int32_t hit;
        switch (player->state)
        {
        case State::Ground:
        {
            bool onLip = false;
            if (player->object != 0)
                onLip = surfacePhysics(player->objectNode, player->surface) == 1 ||
                        surfacePhysics(player->objectNode, player->surface) == 2;
            const ee::Float radius = grindWalls(onLip ? kGrindRadiusQPipe : kGrindRadiusGround);
            hit = searchRails(player, found, kOrdinaryRail, 0u, radius, ee::Float(0.0f));
            break;
        }
        case State::Grind:
        case State::DoubleGrind:
            hit = searchRails(player, found, kOrdinaryRail, 0u, grindWalls(kGrindRadius), grindWalls(kMinGrindAngle));
            break;
        default:
            hit = searchRails(player, found, kOrdinaryRail, 0u, grindWalls(kGrindRadius), ee::Float(0.0f));
            break;
        }
        if (hit)
            zge::memoryCopy(&player->rail, found, sizeof(Rail));
        return hit;
    }
    READABLE_FUNCTION(findGrindRail, Verified);

    // findDoubleRail: the Double Grind's rails: an edge of a double rail (tag
    // 5, else 6) within Grind Radius into Player::rail, and the record
    // pairing it with the other edge into Player::doubleGrindRails.
    // Returns whether one was found.
    int32_t findDoubleRail(ee::Ptr<Player> player)
    {
        const ee::Ptr<Rail> rail = &player->rail;
        uint32_t tag = kDoubleRailEdgeA;
        int32_t hit = searchRails(player, rail, kDoubleRailEdgeA, 0u, grindWalls(kGrindRadius), grindWalls(kMinGrindAngle));
        if (!hit)
        {
            tag = kDoubleRailEdgeB;
            hit = searchRails(player, rail, kDoubleRailEdgeB, 0u, grindWalls(kGrindRadius), grindWalls(kMinGrindAngle));
            if (!hit)
                return 0;
        }
        player->doubleGrindRails = aggressive::level::pairedRail(player->grindSegment, player->grindRail, tag);
        return hit;
    }
    READABLE_FUNCTION(findDoubleRail, Verified);

    // grindTrickMove, a grind trick's move (4 Grind, 5 Hand Grind) asked of
    // the Ground or Air state: with a rail near, the grind. Heading across
    // the rail (beyond Grind Approach Angle), rising slower than Stall
    // Speed, with the stall input held Stall Timeout, on a quarter pipe's
    // lip (or in Air QPipe): a stall instead. *result gets the state
    // entered, or keeps its value when none was.
    void grindTrickMove(ee::Ptr<Player> player, int32_t move, ee::Ptr<int32_t> result)
    {
        const int32_t refused = *result;
        if (!findGrindRail(player))
        {
            *result = refused;
            return;
        }
        zge::Vec4 toward;
        headingOf(*player, toward, *g_stillSpeed);
        const ee::Float cosApproach = approachCos(*g_degreesToAngle);

        bool stall = false;
        if (ee::abs(zge::dot(toward, player->railDirection)) < cosApproach)
        {
            const ee::Float rise = player->velocity.y; // y grows downward
            if (-rise < grindWalls(kStallSpeed) && rise < 0.0f &&
                grindWalls(kStallTimeout) <= player->controls.stallTime)
            {
                if (player->state == State::AirQuarterPipe)
                    stall = true;
                else if (player->object != 0 && surfacePhysics(player->objectNode, player->surface) == 1)
                    stall = true;
                else if (player->object != 0 && surfacePhysics(player->objectNode, player->surface) == 2)
                    stall = true;
            }
        }
        const State target = stall ? State::Stall : move == kMoveGrind ? State::Grind : State::HandGrind;
        changeState(player, target);
        *result = player->state == target ? static_cast<int32_t>(target) : refused;
    }
    READABLE_FUNCTION_WAITS(grindTrickMove, Verified);

    // joinNextRail: onto Player::rail (the grind's enter, and
    // the next rail from the end of one). The rail is turned to run the way
    // the skater heads (a tie, square to it, by the end it came from); the
    // speed along it (Grind Speed Loss coming on across it beyond Grind
    // Approach Angle, at least 0.5); the skater faces along it, upright (on
    // a rail close to vertical, up square to it), at the point it gets on.
    // Another rail than the one ground (or another event word): "GrindEnd"
    // to the object left (and the skater's script), "GrindStart" to the
    // rail, the goals told. Returns 0 if GrindEnd's handlers changed the
    // skater's state, else 1.
    int32_t joinNextRail(ee::Ptr<Player> player)
    {
        ee::Local<GrindEndData> endData;
        ee::Local<Vec4> velocity;
        ee::Local<Vec4> toward;
        ee::Local<Vec4> start;
        ee::Local<uint32_t> startWord;
        Rail &rail = player->rail;
        PlayerAxes &axes = player->axes;

        headingOf(*player, *toward, *g_joinStillSpeed);
        const ee::Float along = zge::dot(rail.direction, *toward);
        bool turn = along < 0.0f;
        if (!turn)
        {
            const bool square = ee::abs(along) < *g_squareToRail;
            turn = square && samePoint(rail.joinPoint, rail.end, *g_squareToRail);
        }
        if (turn)
        {
            vec3Negate(rail.direction, rail.direction);
            *start = rail.start;
            rail.start = rail.end;
            rail.end = *start;
        }
        zge::matrixIdentity(&player->railLean);

        // The speed along the rail.
        const ee::Float cosApproach = approachCos(*g_joinDegreesToAngle);
        ee::Float speed = player->speed;
        if (ee::abs(zge::dot(*toward, rail.direction)) < cosApproach)
            speed = speed * grindWalls(kGrindSpeedLoss);
        if (speed < 0.5f)
            speed = 0.5f;
        *velocity = zge::scaledAfterMeasure(rail.direction, speed);
        player->velocity = *velocity;

        // Facing along the rail, the way the skater faced; upright.
        Vec4 &facing = axes.forward;
        if (zge::dot(rail.direction, facing) < 0.0f)
            vec3Negate(facing, rail.direction);
        else
            facing = rail.direction;
        if (*g_steepRail < ee::abs(zge::dot(*g_worldUp, facing)))
            axes.up = zge::cross(axes.right, *velocity);
        else
            axes.up = *g_worldUp;
        orthonormalize(&axes.right, &axes.up, &axes.forward);
        player->position = rail.joinPoint;

        // The same rail going on: no events.
        if (player->object == rail.object && player->railEvent == railEventWord(player->objectNode, rail.surface))
        {
            player->airTime = 0.0f;
            return 1;
        }
        if (player->object != rail.object)
            player->lastRail = player->objectNode;

        const ee::Ptr<zge::Node> left = player->objectNode;
        const State state = player->state;
        if (left)
        {
            endData->railEvent = player->railEvent;
            endData->distance = player->distance;
            sendObjectEvent(player, left, sym::grindEnd, endData);
            if (const uint32_t handle = player->script)
                sendScriptEvent(player, handle, sym::grindEnd, endData);
            if (player->state != state)
                return 0;
        }

        const uint32_t word = railEventWord(rail.node, rail.surface);
        player->railEvent = word;
        player->surface = kNoSurface;
        player->object = rail.object;
        player->distance = 0.0f;
        *startWord = word;
        sendObjectEvent(player, rail.node, sym::grindStart, startWord);
        if (const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(ee::Ptr<zge::Node>(rail.object), 0))
        {
            const uint32_t hash = zge::objectNameHash(owner);
            aggressive::level::goalGrind(hash, *startWord);
        }
        player->airTime = 0.0f;
        return 1;
    }
    READABLE_FUNCTION_WAITS(joinNextRail, Verified);
}
