// The level's placed effects (inlineFx.c, readable rounds gamehand4 and
// gamehand15): the locators tagged as effects (kind hash 0x0678D345;
// loadUserEffects reads them at the level's load into a list of 0xC0-byte
// records) each run a particle emitter of one of the user classes
// ("userspark", "usersmoke", "userbubble", "userfirefly"; fx/particles.h)
// at a rate, in place or following an object's node.
//
// Field names past the offsets are inferred from what the code does.

#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/globals.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/assets/tag_table.h"
#include "zge/math/box.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"

#include "aggressive/level/level_object.h"
#include "zge/math/vec.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"
#include "zge/math/matrix.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/fx/user_effects.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_userEffects> g_userEffects;  // the list's first
        inline ee::Global<uint32_t, sym::g_userEffect> g_userEffect;   // the one being run (the walks' cursor)
        // The user classes' name hashes (initInlineFx sets them at the
        // effects library's load).
        inline ee::Global<uint32_t, sym::g_userBubbleHash> g_userBubble;
        inline ee::Global<uint32_t, sym::g_userFireflyHash> g_userFirefly;
        inline ee::Global<uint32_t, sym::g_userSmokeHash> g_userSmoke;
        inline ee::Global<uint32_t, sym::g_userSparkHash> g_userSpark;

        constexpr uint32_t kNodeMatrix = sym::nodeMatrix;       // (node, 1) -> its world matrix
        // (class name hash, a, b): the effect class of that name's +0x00
        // method, called with (a, b) (engine, zobjParticleEmitter.c: the
        // emitter told to spawn; its record's +0xA8 the count).

        ee::Ptr<UserEffect> running() { return ee::Ptr<UserEffect>(*g_userEffect); }
    }

    // updateUserEffects: the placed effects' frame (dt the frame's time). Each
    // effect enabled and found (in split screen, only those within their
    // range of player one) counts the particles due: its rate times the
    // time into this second, less those spawned this second; a node it
    // follows moves it (the offset turned by the node's frame); then its
    // emitter's class spawns the particles due. The list is walked with
    // the global cursor the original keeps (0 at the end).
    void updateUserEffects(ee::Float dt)
    {
        ee::Local<zge::Matrix> world;   // the followed node's frame
        ee::Local<zge::Vec4> turned;    // the offset in it
        ee::Local<zge::Vec4> at;        // the node's point
        ee::Local<zge::Vec4> toPlayer;
        const uint32_t list = *g_userEffects;
        const zge::Vec4 &viewer = parts::currentLevel()->players[0].drawnPosition;
        *g_userEffect = list;
        for (uint32_t effect = list; effect != 0;)
        {
            ee::Ptr<UserEffect> e = running();
            bool run = e->enabled != 0 && e->hidden == 0;
            if (run)
            {
                // In split screen only near player one (the original measures twice).
                bool far = false;
                if (*g_gameType == GameType::SplitScreen)
                {
                    *toPlayer = e->position - viewer;
                    far = running()->range < zge::vec3Length(&toPlayer->xyz());
                    if (far)
                    {
                        *toPlayer = running()->position - viewer;
                        if (zge::vec3Length(&toPlayer->xyz()) <= running()->range)
                            far = false;
                    }
                }
                run = !far;
            }
            if (run)
            {
                // The particles due: the rate times the time into this second
                // (truncated), less this second's so far.
                e = running();
                const ee::Float time = e->time + dt;
                e->time = time;
                const int32_t total = ee::toInt(ee::toFloat(static_cast<int32_t>(e->rate)) * time);
                e->due = static_cast<uint8_t>(total - static_cast<uint8_t>(e->spawned));
                e->spawned = static_cast<uint16_t>(total);
                if (ee::Float(1.0f) < e->time)
                {
                    const ee::Float rest = e->time - 1.0f;
                    e->spawned = static_cast<uint16_t>(total - e->rate);
                    e->time = rest;
                }

                // Following a node: its point plus the offset turned by its frame.
                if (running()->node != 0)
                {
                    zge::matrixIdentity(world);
                    *world = *zge::nodeMatrix(running()->node, 1);
                    *at = zge::matrixRow(*world, 3);
                    zge::matrixClearTranslation(world);
                    *turned = zge::transformed(*world, running()->offset);
                    running()->position = *at + *turned;
                }

                // The emitter's class spawns them.
                e = running();
                if (e->due != 0 && e->emitter != 0)
                {
                    uint32_t spawnClass;
                    switch (e->kind) // the original's jump table, sparks past it
                    {
                    case 1:
                    case 2:
                        spawnClass = *g_userSmoke;
                        break;
                    case 3:
                        spawnClass = *g_userBubble;
                        break;
                    case 4:
                        spawnClass = *g_userFirefly;
                        break;
                    default: // 0 (the table's) and past it (a branch)
                        spawnClass = *g_userSpark;
                        break;
                    }
                    // The class's spawn gets the record.
                    zge::spawnNamedEffect(spawnClass, e->emitter, e);
                }
            }
            effect = running()->next;
            *g_userEffect = effect;
        }
    }
    READABLE_FUNCTION_WAITS(updateUserEffects, Verified);

}

namespace zge
{
}

namespace aggressive::fx
{
    namespace
    {
        constexpr uint32_t kMatchUserEffect = sym::matchUserEffect; // below: the subtree walk's visit

        constexpr uint32_t kEffectLocator = 0x0678D345u;   // a locator's kind hash: a placed effect

        // A level locator as the effects read it.
        struct Locator
        {
            zge::Vec4 position; // +0x00
            uint8_t _10[0x10];
            uint32_t kind;      // +0x20 its kind's name hash
            uint32_t name;      // +0x24 its name's hash
            uint32_t _28;
            ee::Ptr<zge::TagRecord> tags; // +0x2C its tag record (TGFX)
        };
        static_assert(offsetof(Locator, kind) == 0x20 && offsetof(Locator, tags) == 0x2C);

        // The placed effect's tag record (TGFX), by offset (tagByte with 2,
        // tagWord and tagFloat with 8).
        enum EffectTag : uint32_t
        {
            kTagKind = 0x00,          // byte: UserEffect::kind
            kTagRange = 0x04,         // float
            kTagVelocity = 0x08,      // three floats
            kTagVelocitySpread = 0x14,
            kTagPositionSpread = 0x20,
            kTagDrift = 0x2C,
            kTagTexture = 0x38,       // word: the texture's name hash (0: the class's)
            kTagRootObject = 0x3C,    // word: the script object it is on (its name hash)
            kTagLinkNode = 0x40,      // word: the node of that object it follows, or of the object found by this name
            kTagOffset = 0x44,        // three floats
            kTagSize = 0x50,          // float
            kTagRate = 0x54,          // byte
            kTagLifetime = 0x58,      // float seconds
            kTagLifetimeSpread = 0x5C,
            kTagColour = 0x60,        // four bytes
            kTagGrowth = 0x64,        // float (a second's)
            kTagFade = 0x68,          // float
        };

        // The region an effect in place belongs to: the box 5 either way of it.
        inline constexpr ee::Float kRegionReach = 5.0f;
        inline constexpr ee::Float kMilliseconds = 1000.0f;
        inline ee::Global<uint32_t, sym::g_smokeBallTexture> g_smokeTexture; // kFxID_ImgSmoke (spawns.cpp's first)
        enum : uint8_t { kSteam = 1, kSmoke = 2 }; // UserEffect::kind's smoke classes
    }

    // matchUserEffect (node, name hash *): forEachNodeInSubtree's visit: the
    // node of that name becomes the placed effect's (the one being read)
    // and the walk stops (0).
    uint32_t matchUserEffect(ee::Ptr<zge::Node> node, ee::Ptr<uint32_t> name)
    {
        const ee::Ptr<UserEffect> effect = running();
        if (node->nameHash == *name)
        {
            effect->node = node;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(matchUserEffect, Verified);

    // loadUserEffects: the level's placed effects read at its load: each
    // locator of the effect kind becomes a record on the list (its tags
    // the motion, spread, look and rate, its locator the position), with
    // a particle emitter of its own. One on an object (its root's name)
    // follows the node of the link's name below the object's geometry; one
    // with only a link follows that object's geometry; one found nowhere
    // is off and hidden; one in place belongs to the region around it.
    void loadUserEffects()
    {
        ee::Local<zge::LocatorList> locators;
        ee::Local<zge::Box> around;
        ee::Local<uint32_t> rootName;
        ee::Local<uint32_t> linkName;
        const ee::Float milliseconds = kMilliseconds;
        const ee::Float reach = kRegionReach;

        zge::initList(locators);
        zge::findLocators(locators, 0, 0);
        *g_userEffect = 0;
        *g_userEffects = 0;
        {
            for (ee::Ptr<zge::Locator> link = locators->head; link; link = link->next)
            {
                const Locator &locator = *ee::Ptr<Locator>(link->point);
                if (locator.kind == kEffectLocator)
                {
                    // A record on the end of the list.
                    if (!*g_userEffect)
                    {
                        const uint32_t made = zge::heapAllocate(sizeof(UserEffect));
                        *g_userEffect = made;
                        *g_userEffects = made;
                    }
                    else
                    {
                        const uint32_t made = zge::heapAllocate(sizeof(UserEffect));
                        running()->next = made;
                        *g_userEffect = made;
                    }
                    UserEffect &e = *running();
                    e.next = 0;
                    e.name = locator.name;
                    const auto tagFloat = [&](uint32_t offset)
                    {
                        return zge::tagFloat(locator.tags, 8u, offset);
                    };
                    const auto tagByte = [&](uint32_t offset)
                    {
                        return static_cast<uint8_t>(zge::tagByte(locator.tags, 2u, offset));
                    };
                    const auto tagWord = [&](uint32_t offset)
                    {
                        return static_cast<uint32_t>(zge::tagWord(locator.tags, 8u, offset));
                    };
                    e.kind = tagByte(kTagKind);
                    e.velocity.x = tagFloat(kTagVelocity);
                    e.velocity.y = tagFloat(kTagVelocity + 4);
                    e.velocity.z = tagFloat(kTagVelocity + 8);
                    e.velocitySpread.x = tagFloat(kTagVelocitySpread);
                    e.velocitySpread.y = tagFloat(kTagVelocitySpread + 4);
                    e.velocitySpread.z = tagFloat(kTagVelocitySpread + 8);
                    e.position = locator.position;
                    e.positionSpread.x = tagFloat(kTagPositionSpread);
                    e.positionSpread.y = tagFloat(kTagPositionSpread + 4);
                    e.positionSpread.z = tagFloat(kTagPositionSpread + 8);
                    e.drift.x = tagFloat(kTagDrift);
                    e.drift.y = tagFloat(kTagDrift + 4);
                    e.drift.z = tagFloat(kTagDrift + 8);
                    e.offset.x = tagFloat(kTagOffset);
                    e.offset.y = tagFloat(kTagOffset + 4);
                    e.offset.z = tagFloat(kTagOffset + 8);

                    // Its own texture by name, else its class's (the smoke
                    // classes' set here; the others' are the classes' own).
                    const uint32_t textureName = tagWord(kTagTexture);
                    e.ownTexture = textureName;
                    if (textureName)
                    {
                        const uint32_t texture = zge::findTextureByHash(textureName, 0);
                        e.texture = texture;
                        if (!texture)
                            e.ownTexture = 0;
                    }
                    else if (e.kind == kSteam)
                        e.texture = *g_steamBallTexture;
                    else if (e.kind == kSmoke)
                        e.texture = *g_smokeTexture;
                    e.size = tagFloat(kTagSize);
                    e.rate = tagByte(kTagRate);
                    e.lifetime = tagFloat(kTagLifetime) * milliseconds;
                    const ee::Float lifetimeSpread = tagFloat(kTagLifetimeSpread);
                    e.hidden = 0;
                    e.lifetimeSpread = lifetimeSpread * milliseconds;
                    e.enabled = 1;
                    e.colour[0] = tagByte(kTagColour);
                    e.colour[1] = tagByte(kTagColour + 1);
                    e.colour[2] = tagByte(kTagColour + 2);
                    e.colour[3] = tagByte(kTagColour + 3);
                    e.growth = tagFloat(kTagGrowth) / milliseconds;
                    e.fade = static_cast<uint32_t>(ee::toInt(tagFloat(kTagFade)));
                    e.range = tagFloat(kTagRange);
                    *rootName = tagWord(kTagRootObject);
                    *linkName = tagWord(kTagLinkNode);

                    // What it follows. (On an object whose geometry is
                    // missing its node is left as the heap gave it: bug kept.)
                    bool lost = false;
                    if (*rootName)
                    {
                        const ee::Ptr<ee::Ptr<zge::SceneObject>> objects = zge::findObjects(rootName, 1);
                        if (!objects)
                            lost = true;
                        else
                        {
                            const ee::Ptr<zge::Node> geometry = zge::objectLevelNode(*objects);
                            if (geometry)
                            {
                                const int32_t walked = zge::forEachNodeInSubtree(geometry, kMatchUserEffect, linkName);
                                if (walked)
                                {
                                    // The link is not below it.
                                    running()->hidden = 1;
                                    running()->node = {};
                                    running()->enabled = 0;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (*linkName)
                        {
                            const ee::Ptr<ee::Ptr<zge::SceneObject>> objects = zge::findObjects(linkName, 1);
                            ee::Ptr<zge::Node> geometry;
                            if (objects)
                                geometry = zge::objectLevelNode(*objects);
                            if (geometry)
                                running()->node = geometry;
                            else
                                lost = true;
                        }
                        else
                        {
                            // In place: the region of the box around it.
                            UserEffect &placed = *running();
                            placed.node = 0;
                            around->low.x = placed.position.x - reach;
                            around->low.y = placed.position.y - reach;
                            around->low.z = placed.position.z - reach;
                            around->high.x = placed.position.x + reach;
                            around->high.y = placed.position.y + reach;
                            around->high.z = placed.position.z + reach;
                            running()->area = zge::findRegion(around);
                        }
                    }
                    if (lost)
                    {
                        // Its object is not there: off, and hidden.
                        running()->hidden = 1;
                        running()->node = {};
                        running()->enabled = 0;
                    }

                    // Its emitter, whose class gets the record.
                    UserEffect &made = *running();
                    made.due = 0;
                    made.spawned = 0;
                    made.time = 0.0f;
                    const ee::Ptr<zge::SceneObject> emitter = zge::newSceneEmitter(0u);
                    running()->emitter = emitter;
                    zge::setEmitterOwner(emitter, running());
                }
            }
        }
        zge::freeLocators(locators);
    }
    READABLE_FUNCTION_WAITS(loadUserEffects, Verified);

    // freeUserEffects: the placed effects' records freed (the list walked with
    // the global cursor, 0 at the end).
    void freeUserEffects()
    {
        const uint32_t list = *g_userEffects;
        *g_userEffect = list;
        if (list)
        {
            for (;;)
            {
                const uint32_t effect = *g_userEffect;
                const uint32_t next = running()->next;
                zge::heapFree(effect);
                *g_userEffect = next;
                if (!next)
                    break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(freeUserEffects, Verified);

    // setUserEffectState: the first user effect of a name on or off.
    void setUserEffectState(uint32_t name, uint32_t enabled)
    {
        *g_userEffect = *g_userEffects;
        bool found = false;
        while (*g_userEffect != 0 && !found)
        {
            ee::Ptr<UserEffect> effect(*g_userEffect);
            if (effect->name == name)
            {
                effect->enabled = enabled;
                found = true;
            }
            *g_userEffect = effect->next;
        }
    }
    READABLE_FUNCTION(setUserEffectState, Verified);
}
