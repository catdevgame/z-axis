// The level's critters and birds (inlineFx.c): each "pathhead" locator is
// a rat, squirrel, roach, pelican, pigeon or seagull (or the models its
// tags name) on a path of "pathpoint" locators, each naming the next. The
// rats, squirrels and roaches run along their paths and are splatted by
// the skaters that reach them; the birds wait for a skater to come near
// and fly off.

#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/skater/skater.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "zge/scene/scene_database.h"
#include "zge/memory/regions.h"
#include "aggressive/sound/events.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/fx/movers.cpp.h"

namespace aggressive
{
}

namespace aggressive
{
    namespace
    {
        using zge::SceneObject;
        using zge::Vec4;
        using skater::Player;

        // A path's waypoint: where it is, and the leg from it to the next.
        struct Waypoint
        {
            Vec4 position;              // +0x00
            uint32_t last;              // +0x10 nonzero: the path's last (its next is the first)
            ee::Float speed;            // +0x14 along the leg from here
            ee::Float wait;             // +0x18 seconds a mover waits on arriving here
            uint16_t heading;           // +0x1C the leg's, about y (a zge::Angle)
            uint8_t _1e[2];
            // +0x20 the leg's, of length 1. Its y is 0 except on a waiting
            // path (setLeg).
            Vec4 direction;
            ee::Float length;           // +0x30 the leg's
            ee::Ptr<Waypoint> next;     // +0x34
            ee::Ptr<Waypoint> previous; // +0x38
        };
        static_assert(offsetof(Waypoint, heading) == 0x1C && offsetof(Waypoint, direction) == 0x20);
        static_assert(offsetof(Waypoint, length) == 0x30 && offsetof(Waypoint, previous) == 0x38 && sizeof(Waypoint) == 0x40);

        // The kinds (Mover::kind): the critters, which have a splat model
        // and can be hit, and the birds.
        constexpr uint8_t kRat = 0, kSquirrel = 1, kRoach = 2, kPelican = 3, kPigeon = 4, kSeagull = 5;
        constexpr uint32_t kMoverKinds = 6;

        // Mover::path: what a mover does at its path's last waypoint. Any
        // other value goes back to the first.
        constexpr uint8_t kPathLoop = 1;         // on round to the first
        constexpr uint8_t kPathBackAndForth = 2; // back along the path
        // A bird: it waits at each waypoint for a skater to come near, and
        // at the end is hidden and goes back to the first.
        constexpr uint8_t kPathWaitForSkater = 3;

        // A critter or a bird of the level (a "pathhead" locator's,
        // setupMovers).
        struct Mover
        {
            uint8_t kind;                       // +0x00 kRat .. kSeagull; past them, its tags name its models
            uint8_t path;                       // +0x01 kPathLoop, kPathBackAndForth, kPathWaitForSkater
            uint8_t _02[2];
            ee::Float speed;                    // +0x04 along a leg whose waypoint has none
            uint32_t hittable;                  // +0x08 a skater that reaches it splats it
            uint32_t hidden;                    // +0x0C splatted, until the wait is over
            uint32_t triggered;                 // +0x10 a waiting bird a skater came near: it flies
            uint32_t forward;                   // +0x14 toward the next waypoint (else the previous)
            ee::Float left;                     // +0x18 of the leg
            ee::Float wait;                     // +0x1C seconds
            ee::Ptr<SceneObject> model;         // +0x20
            ee::Ptr<SceneObject> hitModel;      // +0x24 its splat, shown where it was hit (a critter's)
            ee::Ptr<Waypoint> first;            // +0x28
            ee::Ptr<Waypoint> current;          // +0x2C the leg's start
            ee::Ptr<Mover> next;                // +0x30 the list's next
        };
        static_assert(offsetof(Mover, model) == 0x20 && offsetof(Mover, next) == 0x30 && sizeof(Mover) == 0x34);

        inline ee::Global<ee::Ptr<Mover>, sym::g_movers> g_movers; // the list's first
        inline ee::Global<ee::Ptr<Mover>, sym::g_mover> g_mover;   // the one being set up, moved or freed

        // A kind's animations, found by name (setupMovers): the birds'
        // "pigeonIdle" and "pigeonFly", none for the critters (""). The idle
        // one is never played: where a bird starts one, its idle animation
        // is tested and its fly animation played.
        struct MoverKind
        {
            uint32_t idleName; // +0x00 -> its name
            ee::Ptr<zge::Motion> idle; // +0x04 the animation, 0: none
            uint32_t flyName;  // +0x08
            ee::Ptr<zge::Motion> fly;  // +0x0C
        };
        inline ee::Global<MoverKind[kMoverKinds], sym::g_moverKinds> g_moverKinds;

        // A mover's kind's animations. (A kind past the table reads on past
        // it, as the original.)
        const MoverKind &kindOf(const Mover &mover)
        {
            const ee::Ptr<MoverKind> kinds = *g_moverKinds;
            return kinds[mover.kind];
        }

        inline ee::Global<ee::Float, sym::g_waitBlend> g_waitBlend;       // 0.1: a bird placed at its start
        inline ee::Global<ee::Float, sym::g_showBlend> g_showBlend;       // 0.1: a bird whose wait is over
        inline ee::Global<ee::Float, sym::g_triggerBlend> g_triggerBlend; // 0.1: a bird a skater came near
        inline ee::Global<ee::Float, sym::g_hitDistance> g_hitDistance;   // 1.0: a skater nearer splats a critter

        constexpr uint32_t kLoopAnimation = 1; // playAnimation's mode
        constexpr uint16_t kHalfTurn = 0x8000;
        constexpr ee::Float kNearBird = 3.0f;  // a skater this near sends a waiting bird off
        // soundEvent 42: the main bank's sample 0x32 + a. A splatted
        // critter's: a 1 for a rat or squirrel, 0 for a roach.
        constexpr uint32_t kSplatSound = 42;
    }

    namespace
    {
        // The mover's model turned to `heading` about y, through the
        // caller's angles on its frame.
        void face(const ee::Local<zge::EulerAngles> &angles, ee::Ptr<SceneObject> model, uint16_t heading)
        {
            *angles = zge::EulerAngles{0, static_cast<zge::Angle>(heading), 0};
            zge::setObjectAngles(model, angles);
        }

        // How far the skater is from `at`, the difference kept in the
        // caller's `offset`.
        ee::Float distanceFrom(const Player &skater, const Vec4 &at, Vec4 &offset)
        {
            offset.x = skater.drawnPosition.x - at.x;
            offset.y = skater.drawnPosition.y - at.y;
            offset.z = skater.drawnPosition.z - at.z;
            return zge::vec3Length(&offset.xyz());
        }
    }

    // moveMovers: the level's critters and birds moved along their paths
    // by the frame's `seconds`. A wait uses up the time it covers and what
    // is left goes on to the next mover. A critter a skater reaches is
    // splatted: its splat shown where it was hit, it goes back to its
    // path's start, hidden until that waypoint's wait is over (with none
    // there, for good). A waiting bird starts when a skater comes within 3.
    //
    // (A mover left no time by its wait is still tested for hits, at the
    // place, leg and step of the last mover that moved or was a waiting
    // bird: bug kept.)
    void moveMovers(ee::Float seconds)
    {
        ee::Local<zge::EulerAngles> facing; // face's
        ee::Local<Vec4> offset;             // distanceFrom's
        ee::Local<Vec4> hitAt;              // where the splat goes

        ee::Float time = seconds;
        // Kept from one mover to the next (the bug above).
        ee::Ptr<Waypoint> leg;
        ee::Float step = 0.0f;
        ee::Ptr<Vec4> position;

        for (*g_mover = *g_movers; *g_mover; *g_mover = (*g_mover)->next)
        {
            Mover &m = **g_mover;
            if (m.wait != 0.0f)
            {
                if (time < m.wait)
                {
                    m.wait = m.wait - time;
                    time = 0.0f;
                }
                else
                {
                    time = time - m.wait;
                    m.wait = 0.0f;
                    if (m.hidden)
                    {
                        // A splatted critter back on its path, its splat gone.
                        m.hidden = 0;
                        zge::setSceneObjectShown(m.model, 1);
                        zge::setSceneObjectShown(m.hitModel, 0);
                    }
                    if (m.path == kPathWaitForSkater)
                    {
                        // A bird waits for a skater again.
                        m.triggered = 0;
                        zge::setSceneObjectShown(m.model, 1);
                        const MoverKind &kind = kindOf(m);
                        if (kind.idle)
                            zge::playAnimation(m.model, kind.fly, kLoopAnimation, *g_showBlend, ee::Float(1.0f));
                    }
                }
            }
            if (m.hidden)
                continue;

            if (m.path == kPathWaitForSkater && !m.triggered)
            {
                // A waiting bird: a skater within kNearBird sends it off.
                // (Player two's place is read in a one-player game too.)
                position = zge::objectPosition(m.model);
                const ee::Ptr<LevelObject> level = parts::currentLevel();
                bool near = distanceFrom(level->players[0], *position, offset) <= kNearBird;
                if (!near)
                    near = distanceFrom(level->players[1], *position, offset) <= kNearBird;
                if (near)
                {
                    m.triggered = 1;
                    if (const ee::Ptr<zge::Motion> fly = kindOf(m).fly)
                        zge::playAnimation(m.model, fly, kLoopAnimation, *g_triggerBlend, ee::Float(1.0f));
                }
                continue;
            }

            if (time > 0.0f)
            {
                leg = m.current;
                if (m.forward)
                {
                    const ee::Ptr<Waypoint> to = leg->next;
                    position = zge::objectPosition(m.model);
                    step = leg->speed * time;
                    if (m.left <= step)
                    {
                        // At the next waypoint: on, back, or round again
                        // from the first (a bird hidden first).
                        if (!to->last || m.path == kPathLoop)
                        {
                            zge::setObjectPosition(m.model, &to->position);
                            face(facing, m.model, to->heading);
                            m.current = to;
                            m.left = to->length;
                            m.wait = to->wait;
                        }
                        else if (m.path == kPathBackAndForth)
                        {
                            m.forward = 0;
                            zge::setObjectPosition(m.model, &to->position);
                            face(facing, m.model, static_cast<uint16_t>(leg->heading + kHalfTurn));
                            m.current = leg;
                            m.left = leg->length;
                            m.wait = to->wait;
                        }
                        else
                        {
                            if (m.path == kPathWaitForSkater)
                                zge::setSceneObjectShown(m.model, 0);
                            const ee::Ptr<Waypoint> first = m.first;
                            zge::setObjectPosition(m.model, &first->position);
                            face(facing, m.model, first->heading);
                            m.current = first;
                            m.left = first->length;
                            m.wait = first->wait;
                        }
                    }
                    else
                    {
                        Vec4 &p = *position;
                        p.x = p.x + leg->direction.x * step;
                        p.z = p.z + leg->direction.z * step;
                        if (m.path == kPathWaitForSkater)
                            p.y = p.y + leg->direction.y * step;
                        zge::setObjectPosition(m.model, &p);
                        m.left = m.left - step;
                    }
                }
                else
                {
                    // Going back: the leg is the previous waypoint's.
                    const ee::Ptr<Waypoint> to = leg->previous;
                    position = zge::objectPosition(m.model);
                    step = to->speed * time;
                    if (m.left <= step)
                    {
                        if (to == m.first)
                        {
                            // Back at the start: forward again.
                            m.forward = 1;
                            zge::setObjectPosition(m.model, &to->position);
                            face(facing, m.model, to->heading);
                            m.current = m.first;
                            m.left = to->length;
                        }
                        else
                        {
                            zge::setObjectPosition(m.model, &to->position);
                            const ee::Ptr<Waypoint> before = to->previous;
                            face(facing, m.model, static_cast<uint16_t>(before->heading + kHalfTurn));
                            m.current = to;
                            m.left = before->length;
                        }
                        m.wait = to->wait;
                    }
                    else
                    {
                        // (Only a back-and-forth path goes back, so the y
                        // part, added the wrong way, never moves.)
                        Vec4 &p = *position;
                        p.x = p.x - to->direction.x * step;
                        p.z = p.z - to->direction.z * step;
                        if (m.path == kPathWaitForSkater)
                            p.y = p.y + to->direction.y * step;
                        zge::setObjectPosition(m.model, &p);
                        m.left = m.left - step;
                    }
                }
            }

            if (!m.hittable)
                continue;
            // A skater nearer than this frame's step or g_hitDistance hits it.
            const ee::Ptr<LevelObject> level = parts::currentLevel();
            Player &one = level->players[0];
            const ee::Float firstDistance = distanceFrom(one, *position, offset);
            const bool firstHit = firstDistance < step || firstDistance < *g_hitDistance;
            int32_t hits = firstHit ? 1 : 0;
            ee::Float secondDistance = 0.0f;
            if (*g_gameType == GameType::SplitScreen)
            {
                secondDistance = distanceFrom(level->players[1], *position, offset);
                if (secondDistance < step || secondDistance < *g_hitDistance)
                    ++hits;
            }
            if (!hits)
                continue;

            // The splat goes where the skater is (the nearer of two), at the
            // leg's height; one player's splats are counted in the profile.
            if (*g_gameType == GameType::SplitScreen)
            {
                const bool byOne = hits == 2 ? firstDistance < secondDistance : firstHit;
                *hitAt = level->players[byOne ? 0 : 1].drawnPosition;
                hitAt->y = leg->position.y;
            }
            else
            {
                *hitAt = one.drawnPosition;
                hitAt->y = leg->position.y;
                ++skater::skaterProfile(&one)->crittersExterminated;
            }
            m.current = m.first;
            m.left = m.first->length;
            m.forward = 1;
            m.wait = m.first->wait;
            zge::setObjectOrientation(m.hitModel, zge::objectOrientation(m.model));
            zge::setObjectPosition(m.model, &m.first->position);
            face(facing, m.model, m.first->heading);
            zge::setSceneObjectShown(m.model, 0);
            zge::setObjectPosition(m.hitModel, hitAt);
            zge::setSceneObjectShown(m.hitModel, 1);
            if (m.kind <= kRoach)
                soundEvent(kSplatSound, m.kind < kRoach ? 1 : 0, 0, 0, 0);
            m.hidden = 1;
        }
    }
    READABLE_FUNCTION_WAITS(moveMovers, Verified);

    namespace
    {
        // The locators' kinds (ScenePoint::kindHash).
        constexpr uint32_t kPathHead = 0x068CCC14u;  // nameHash("pathhead"): a mover, at its path's start
        constexpr uint32_t kPathPoint = 0x08D56A54u; // nameHash("pathpoint"): a waypoint

        // Their tags (the tag accessors' second argument: 2 for a byte, 8
        // for a word).
        constexpr uint32_t kTagBytes = 2, kTagWords = 8;
        enum PathHeadTag : uint32_t
        {
            kHeadKind = 0x00,      // byte: Mover::kind
            kHeadPath = 0x01,      // byte: Mover::path
            kHeadSpeed = 0x04,     // float: Mover::speed
            kHeadNext = 0x08,      // word: the first waypoint's name hash
            kHeadWait = 0x0C,      // float: the start's Waypoint::wait
            kHeadHittable = 0x10,  // byte: Mover::hittable, for a kind past kSeagull
            kHeadModel = 0x14,     // word: a kind past kSeagull's model's name hash
            kHeadHitModel = 0x18,  // word: and its splat's
        };
        enum PathPointTag : uint32_t
        {
            kPointLast = 0x00,  // byte: Waypoint::last
            kPointSpeed = 0x04, // float: 0 takes the mover's
            kPointNext = 0x08,  // word: the next waypoint's name hash
            kPointWait = 0x0C,  // float
        };

        // The kinds' models: the critters' own and their splats, the birds'.
        constexpr uint32_t kCritterModels[3][2] = {
            {sym::g_textRat, sym::g_textRatSplat},           // "Rat", "RatSplat"
            {sym::g_textSquirrel, sym::g_textSquirrelSplat}, // "Squirrel", "SquirrelSplat"
            {sym::g_textRoach, sym::g_textRoachSplat},       // "Roach", "RoachSplat"
        };
        constexpr uint32_t kBirdModels[3] = {sym::g_textPelican, sym::g_textPigeon, sym::g_textSeagull}; // "Pelican", "Pigeon", "Seagull"
    }

    namespace
    {
        uint32_t tagByte(ee::Ptr<zge::TagRecord> tags, uint32_t tag) { return zge::tagByte(tags, kTagBytes, tag); }
        uint32_t tagWord(ee::Ptr<zge::TagRecord> tags, uint32_t tag) { return static_cast<uint32_t>(zge::tagWord(tags, kTagWords, tag)); }
        ee::Float tagFloat(ee::Ptr<zge::TagRecord> tags, uint32_t tag) { return zge::tagFloat(tags, kTagWords, tag); }

        ee::Ptr<SceneObject> newModel(uint32_t nameHash)
        {
            return zge::newSceneModelByHash(0u, nameHash);
        }

        // A kind's animation `name` found into `animation`; "" is none. A
        // missing one has its message made in `message`, never shown.
        void findKindAnimation(uint32_t name, ee::Ptr<zge::Motion> &animation, const ee::Local<char[0x100]> &message)
        {
            if (!name || !zge::compareStrings(sym::emptyName, name))
                return;
            if (!zge::findAnimation(name, &animation))
                zge::formatString(message, sym::noAnimation, name); // "Could not find the '%s' animation."
        }

        // The leg from `from` to `to`: its direction (its y only on a
        // waiting path; elsewhere what it held), heading and length.
        void setLeg(Waypoint &from, const Waypoint &to, uint8_t path)
        {
            Vec4 &d = from.direction;
            d.x = to.position.x - from.position.x;
            d.z = to.position.z - from.position.z;
            from.heading = static_cast<uint16_t>(zge::angleAtan2(d.x, d.z) - kHalfTurn);
            if (path == kPathWaitForSkater)
                d.y = to.position.y - from.position.y;
            const ee::Float length = zge::vec3Length(&d.xyz());
            from.length = length;
            d.z = d.z / length;
            d.x = d.x / length;
            if (path == kPathWaitForSkater)
                d.y = d.y / length;
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        // freeLocators: the list's locators back to their pool. By address: the caller
        // returns what the (void) function leaves in the result register.
        int32_t freeLocatorsKeepingResult(ee::Ptr<zge::LocatorList> list) { return ee::game<int32_t>(sym::freeLocators, list); }
    }
    // setupMovers: the level's critters and birds set up at its load. The
    // kinds' animations are found by name. Each "pathhead" locator is made
    // a mover with its models (its splat hidden) at the locator, on a path
    // from there: each next waypoint the "pathpoint" its predecessor's tags
    // name (searched for from the list's start), up to one marked last,
    // which leads back to the first. A path whose next waypoint is not in
    // the level ends there (marked last, with no leg back) and its mover is
    // not placed at the start: its facing and leg length stay unset. A
    // waiting bird starts its fly animation. Returns freeLocators'.
    //
    // (The last waypoint's leg back to the first has its y set only on a
    // waiting path: elsewhere it is what the heap left, in its length too.
    // Bug kept.)
    int32_t setupMovers()
    {
        ee::Local<zge::LocatorList> locators;
        ee::Local<zge::EulerAngles> facing;  // face's
        ee::Local<char[0x100]> message;      // findKindAnimation's
        ee::Local<uint32_t> stranded;        // the path's next waypoint is not in the level

        for (MoverKind &kind : *g_moverKinds)
        {
            findKindAnimation(kind.idleName, kind.idle, message);
            findKindAnimation(kind.flyName, kind.fly, message);
        }

        zge::initList(locators);
        zge::findLocators(locators, 0, 0);
        *g_mover = ee::Ptr<Mover>();
        *g_movers = ee::Ptr<Mover>();
        for (ee::Ptr<zge::Locator> node = locators->head; node; node = node->next)
        {
            *stranded = 0;
            const ee::Ptr<zge::ScenePoint> head(node->point);
            if (head->kindHash != kPathHead)
                continue;

            // A mover on the end of the list.
            const ee::Ptr<Mover> made(zge::heapAllocate(sizeof(Mover)));
            if (*g_mover)
                (*g_mover)->next = made;
            else
                *g_movers = made;
            *g_mover = made;
            Mover &m = *made;
            m.next = ee::Ptr<Mover>();
            m.path = static_cast<uint8_t>(tagByte(head->tags, kHeadPath));
            m.speed = tagFloat(head->tags, kHeadSpeed);
            m.hittable = tagByte(head->tags, kHeadHittable);
            m.wait = 0.0f;
            m.hidden = 0;
            m.forward = 1;
            m.kind = static_cast<uint8_t>(tagByte(head->tags, kHeadKind));

            // Its models: a critter's own and its splat (it can be hit), a
            // bird's (it cannot), else the tags' two.
            uint32_t model, hitModel;
            if (m.kind <= kRoach)
            {
                model = zge::nameHash(ee::Ptr<const char>(kCritterModels[m.kind][0]));
                hitModel = zge::nameHash(ee::Ptr<const char>(kCritterModels[m.kind][1]));
                m.hittable = 1;
            }
            else if (m.kind <= kSeagull)
            {
                hitModel = 0;
                model = zge::nameHash(ee::Ptr<const char>(kBirdModels[m.kind - kPelican]));
                m.hittable = 0;
            }
            else
            {
                model = tagWord(head->tags, kHeadModel);
                hitModel = tagWord(head->tags, kHeadHitModel);
            }
            m.model = newModel(model);
            zge::setObjectPosition(m.model, &head->position);
            zge::setSceneObjectShown(m.model, 1);
            if (hitModel)
            {
                m.hitModel = newModel(hitModel);
                zge::setSceneObjectShown(m.hitModel, 0);
            }

            // The path's first waypoint: the locator itself.
            ee::Ptr<Waypoint> last(zge::heapAllocate(sizeof(Waypoint)));
            m.current = last;
            m.first = last;
            last->position = head->position;
            last->wait = tagFloat(head->tags, kHeadWait);
            last->last = 0;
            last->speed = m.speed;
            uint32_t nextName = tagWord(head->tags, kHeadNext);

            // Each next one, searched for from the list's start.
            for (;;)
            {
                bool found = false;
                for (ee::Ptr<zge::Locator> search = locators->head; search && !found; search = search->next)
                {
                    const ee::Ptr<zge::ScenePoint> point(search->point);
                    if (point->kindHash != kPathPoint || point->nameHash != nextName)
                        continue;
                    found = true;
                    const ee::Ptr<Waypoint> waypoint(zge::heapAllocate(sizeof(Waypoint)));
                    last->next = waypoint;
                    waypoint->position = point->position;
                    waypoint->wait = tagFloat(point->tags, kPointWait);
                    waypoint->speed = tagFloat(point->tags, kPointSpeed);
                    if (waypoint->speed == 0.0f)
                        waypoint->speed = m.speed;
                    waypoint->last = tagByte(point->tags, kPointLast);
                    waypoint->previous = last;
                    last->direction = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
                    setLeg(*last, *waypoint, m.path);
                    if (waypoint->last)
                    {
                        // The last: its leg back to the first.
                        waypoint->next = m.first;
                        m.first->previous = waypoint;
                        setLeg(*waypoint, *m.first, m.path);
                    }
                    else
                    {
                        nextName = tagWord(point->tags, kPointNext);
                    }
                    last = waypoint;
                }
                if (!found)
                {
                    *stranded = 1;
                    last->last = 1;
                }
                if (last->last || *stranded)
                    break;
            }
            if (*stranded)
                continue;

            // On the path's start, facing along its first leg.
            const ee::Ptr<Waypoint> start = m.first;
            zge::setObjectPosition(m.model, &start->position);
            face(facing, m.model, start->heading);
            m.current = start;
            m.left = start->length;
            m.wait = start->wait;
            if (m.path == kPathWaitForSkater)
            {
                m.triggered = 0;
                const MoverKind &kind = kindOf(m);
                if (kind.idle)
                    zge::playAnimation(m.model, kind.fly, kLoopAnimation, *g_waitBlend, ee::Float(1.0f));
            }
        }
        return freeLocatorsKeepingResult(locators);
    }
    READABLE_FUNCTION_WAITS(setupMovers, Verified);

    // freeMovers: the movers and their paths freed (each path's waypoints
    // up to its last; the list walked with the global cursor).
    void freeMovers()
    {
        *g_mover = *g_movers;
        if (!*g_mover)
            return;
        for (;;)
        {
            const ee::Ptr<Mover> mover = *g_mover;
            // Its waypoints, up to the one marked last.
            for (ee::Ptr<Waypoint> waypoint = mover->first; waypoint;)
            {
                const ee::Ptr<Waypoint> freed = waypoint;
                waypoint = freed->last ? ee::Ptr<Waypoint>() : freed->next;
                zge::heapFree(freed);
            }
            const ee::Ptr<Mover> next = mover->next;
            zge::heapFree(mover);
            *g_mover = next;
            if (!next)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(freeMovers, Verified);
}
