#pragma once

// The action tricks: the game objects the skater's action
// states make (scoreStateChange's table, skater/state_tricks.cpp): kind 6
// for Wall Ride and Wall Tap (wall_tricks.cpp), 7 for Vault
// (vault_trick.cpp), 8 for the pole states (pole_trick.cpp) and 11 for
// Skitch (skitch_trick.cpp). Each kind is a module of the same shape: its
// class's message handlers (aggressive/objects.h's messages), a table of
// its tricks (one a state or a stance: the animation by name, found at the
// class's setup, and the trick's text from the string table), the setup
// that registers the class and its handlers, the shutdown, and the create
// function the trick starters call. The modules are copies of one code, kept
// here as helpers.

#include "skater.h"

#include "aggressive/conversions.h"
#include "aggressive/objects.h"
#include "zge/scene/animator.h"
#include "ee/function_ptr.h"
#include "aggressive/tuning.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/scene/animation.h"
#include "sce/libgcc.h"
#include "aggressive/hud/hud.h"
#include "aggressive/sound/events.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/skater/action_tricks.h.h"

namespace aggressive::hud
{
}

namespace aggressive
{
}

namespace aggressive::skater
{
    ee::Float storedPendingScore(ee::Ptr<Player> player);                        // score.cpp
    int32_t trickMultiplier(ee::Ptr<Player> player);
    int32_t setTrickMultiplier(ee::Ptr<Player> player, uint32_t multiplier);
    int32_t setPendingScore(ee::Ptr<Player> player, ee::Float score);
    int32_t requestMove(ee::Ptr<Player> player, int32_t move);
    ee::Ptr<GameObject> skaterCurrentObject(ee::Ptr<Player> player);                        // (trick_objects.cpp)

    namespace action_tricks
    {
        // The calls every trick class makes.
        // The object of the slot for the skater's state made current.
        constexpr uint32_t kHalfMirrored = 13;             // playAnimation's mode for a trick on the skater's other side (5 | 8)
        inline constexpr ee::Float kPlayOnce = -1.0f;       // playAnimation's speed: the animation's own, once

        // A trick's id: its object kind in the top five bits, the trick's
        // variant below.
        constexpr uint32_t trickId(uint32_t kind)
        {
            return kind << 27;
        }

        // Control: Animation Blend Time (tuning category 5, entry 2).
        inline ee::Float animationBlendTime()
        {
            return (*g_tuningCategories)[5].entries[2].value;
        }

        // A trick of the table of kinds 6 and 7 (0x30 bytes): its id (the
        // variant its state block holds), its animation by name (found at
        // the class's setup) and the trick's text (the string table's text
        // for the key, else the key).
        struct TrickEntry
        {
            uint32_t id;            // +0x00
            uint32_t animationName; // +0x04 "global_assets.cedwards.wallride"
            ee::Ptr<zge::Motion> animation;     // +0x08
            uint32_t textKey;       // +0x0C "WallRide"
            char text[0x20];        // +0x10
        };
        static_assert(sizeof(TrickEntry) == 0x30 && offsetof(TrickEntry, text) == 0x10);

        // A trick's start worth banked: when it is worth something, the line's
        // pending score gets it, and the multiplier one more when the line had
        // more than a point pending. The pending score counts in whole points
        // from here on.
        inline void bankStartWorth(ee::Ptr<Player> player, const ee::Float &worth)
        {
            if (!(ee::Float(0.0f) < worth))
                return;
            const uint32_t pending = sce::libgcc::fptoui(storedPendingScore(player));
            if (ee::Float(1.0f) < unsignedToFloat(pending))
                setTrickMultiplier(player, static_cast<uint32_t>(trickMultiplier(player) + 1));
            setPendingScore(player, unsignedToFloat(pending) + worth);
        }

        // The table's entry of `id`, none as 0.
        template <typename Entry, size_t Count>
        ee::Ptr<Entry> findTrick(Entry (&table)[Count], uint32_t id)
        {
            for (Entry &entry : table)
            {
                if (entry.id == id)
                    return &entry;
            }
            return ee::Ptr<Entry>();
        }

        // The same, by the entry's own id field.
        template <typename Entry, size_t Count>
        ee::Ptr<Entry> findTrick(Entry (&table)[Count], uint32_t Entry::*idField, uint32_t id)
        {
            for (Entry &entry : table)
            {
                if (entry.*idField == id)
                    return &entry;
            }
            return ee::Ptr<Entry>();
        }

        using TrickObjectFunction = ee::FunctionPtr<uint32_t(ee::Ptr<GameObject> object)>;
        // A state block that starts with the table's id.
        inline uint32_t trickIdOf(ee::Ptr<GameObject> object)
        {
            return *ee::Ptr<uint32_t>(gameObjectState(object));
        }

        // The trick's text copied into the class's buffer (31 characters).
        template <typename Entry>
        uint32_t trickText(ee::Ptr<GameObject> object, uint32_t buffer, ee::FunctionPtr<ee::Ptr<Entry>(uint32_t id)> find)
        {
            const ee::Ptr<char> out(buffer);
            *out = 0;
            const ee::Ptr<Entry> entry = find(trickIdOf(object));
            zge::stringCopyN(out, entry->text, 31);
            return buffer;
        }

        // The trick's animation, 0 when its entry has none.
        inline ee::Ptr<zge::Motion> trickAnimation(ee::Ptr<GameObject> object, ee::FunctionPtr<ee::Ptr<TrickEntry>(uint32_t id)> find)
        {
            const ee::Ptr<TrickEntry> entry = find(trickIdOf(object));
            return entry ? entry->animation : ee::Ptr<zge::Motion>{};
        }

        // What a trick table's setup does to each row: animations found by
        // name ("Could not find the '%s' animation." made in `message` when
        // not, never shown), and the text: the string table's for the key,
        // else the key, an empty key left out.
        struct TableResolver
        {
            ee::Ptr<uint8_t> message;
            uint32_t notFoundFormat;
            uint32_t empty;
            uint32_t found = 0; // the last find

            void animation(uint32_t name, ee::Ptr<zge::Motion> &slot)
            {
                if (!name)
                    return;
                found = zge::findAnimation(name, &slot);
                if (!found)
                    zge::formatString(message, notFoundFormat, name);
            }

            void text(uint32_t key, char (&text)[0x20])
            {
                if (!key || zge::compareStrings(key, empty) == 0)
                    return;
                const char *translated = zge::stringTableText(zge::nameHash(ee::Ptr<const char>(key)));
                if (translated && zge::compareStrings(translated, empty) != 0)
                    zge::stringCopyN(&text, translated, sizeof(text));
                else
                    zge::stringCopyN(&text, key, sizeof(text));
            }
        };

        // The table of kinds 6 and 7 resolved at the class's setup. The last
        // animation's find.
        template <size_t Count>
        uint32_t resolveTricks(TrickEntry (&table)[Count], uint32_t notFoundFormat, uint32_t empty)
        {
            ee::Local<uint8_t[0x180]> message;
            TableResolver resolve{message, notFoundFormat, empty};
            for (TrickEntry &entry : table)
            {
                resolve.animation(entry.animationName, entry.animation);
                resolve.text(entry.textKey, entry.text);
            }
            return resolve.found;
        }

        // A kind's class registered with its handlers. Returns the last
        // handler's registration.
        template <size_t N>
        uint32_t registerClassHandlers(uint32_t kind, const uint32_t (&messages)[N], const uint32_t (&handlers)[N])
        {
            registerObjectClass(kind);
            uint32_t registered = 0;
            for (size_t i = 0; i < N; ++i)
                registered = setClassHandler(uint64_t(kind), messages[i], handlers[i]);
            return registered;
        }

        // A kind's class registered with its handlers, then its table resolved
        // (`resolve`, none: the caller's own).
        template <size_t N>
        uint32_t registerTrickClass(uint32_t kind, const uint32_t (&messages)[N], const uint32_t (&handlers)[N],
                                    ee::FunctionPtr<uint32_t()> resolve)
        {
            registerClassHandlers(kind, messages, handlers);
            return resolve ? resolve() : 0;
        }

        // A kind's class unregistered once its pool is freed. 0 when the pool
        // was not freed.
        template <typename Pool>
        int32_t unregisterTrickClass(Pool &pool, uint32_t kind)
        {
            if (!aggressive::freePool(*pool))
                return 0;
            *pool = 0;
            return unregisterObjectClass(uint64_t(kind));
        }

        // The handlers every kind has the same code for.

        // getScore: the points, as an unsigned integer, into the answer.
        template <typename State>
        uint32_t answerScore(ee::Ptr<ObjectMessageNote> note, ee::Float State::*worth)
        {
            const ee::Ptr<uint32_t> out(note->argument);
            const ee::Ptr<State> state(gameObjectState(note->object));
            *out = sce::libgcc::fptoui((*state).*worth);
            return 1;
        }

        // A word answered (getMultiplierBonus 0, GetSpaceNeededForSaving the
        // state block's size).
        inline uint32_t answerWord(ee::Ptr<ObjectMessageNote> note, uint32_t word)
        {
            *ee::Ptr<uint32_t>(note->argument) = word;
            return 1;
        }

        // A word of the state block answered.
        template <typename State>
        uint32_t answerStateWord(ee::Ptr<ObjectMessageNote> note, uint32_t State::*word)
        {
            const ee::Ptr<uint32_t> out(note->argument);
            const ee::Ptr<State> state(gameObjectState(note->object));
            *out = (*state).*word;
            return 1;
        }

        // getName: the trick's text into the request, `fallback` when there
        // is none.
        inline uint32_t answerName(ee::Ptr<ObjectMessageNote> note, TrickObjectFunction text, uint32_t fallback)
        {
            const ee::Ptr<NameRequest> request(note->argument);
            const uint32_t name = text(note->object);
            zge::stringCopyN(request->out, name ? name : fallback, request->size);
            return 1;
        }

        // areYouPossible: not while the skater is down, and only in the
        // kind's states.
        template <typename InState>
        int32_t answerPossible(ee::Ptr<ObjectMessageNote> note, InState inState)
        {
            const ee::Ptr<Player> player(note->argument);
            gameObjectState(note->object);
            if (player->flags & kDown)
                return 0;
            return inState(static_cast<uint32_t>(player->state)) ? 1 : 0;
        }

        // areYouFinished: the state block's word.
        template <typename State>
        uint32_t answerFinished(ee::Ptr<ObjectMessageNote> note, uint32_t State::*finished)
        {
            const ee::Ptr<State> state(gameObjectState(note->object));
            return (*state).*finished;
        }

        // SaveToBuffer: the state block (`bytes`) copied to the buffer.
        inline uint32_t saveState(ee::Ptr<ObjectMessageNote> note, uint32_t bytes)
        {
            const ee::Ptr<SaveRequest> request(note->argument);
            const uint32_t state = gameObjectState(note->object);
            zge::memoryCopy(request->buffer, state, bytes);
            return 1;
        }

        // LoadFromBuffer: a state block from the pool, the buffer copied in,
        // made the object's.
        inline uint32_t loadState(ee::Ptr<ObjectMessageNote> note, uint32_t pool, uint32_t bytes)
        {
            const ee::Ptr<SaveRequest> request(note->argument);
            const uint32_t state = aggressive::poolTake(pool);
            zge::memoryCopy(state, request->buffer, bytes);
            setGameObjectState(note->object, state, 0);
            return 1;
        }

        // TrickFinished: the trick's name added to the line's.
        inline uint32_t finishTrick(ee::Ptr<ObjectMessageNote> note)
        {
            hud::addCompletedTrick(ee::Ptr<Player>(note->argument), note->object); // TrickFinished's argument: the player
            return 1;
        }

        // TrickDroppedFromQueue: the state block back to the pool.
        template <typename Pool>
        uint32_t dropTrick(ee::Ptr<ObjectMessageNote> note, Pool &pool)
        {
            const uint32_t state = gameObjectState(note->object);
            setGameObjectState(note->object, 0, 0);
            aggressive::poolGive(*pool, state);
            return 1;
        }

        // The stance flips when an animation's "FrameFlag" event says so: the
        // skater ended it the other way round.
        inline void flipStanceAtEnd(ee::Ptr<AnimationEnd> ended, uint32_t frameFlagName)
        {
            constexpr uint32_t kStanceFlipped = 5;
            if (zge::eventInt(ee::Ptr<zge::AnimationEvent>(ended->event), zge::nameHash(ee::Ptr<const char>(frameFlagName))) != kStanceFlipped)
                return;
            const ee::Ptr<Player> player(ended->player);
            player->fakie = player->fakie < 1 ? 1 : 0;
            player->drawnRefresh = 1;
            zge::stopAnimation(player->node);
        }

        // A kind's new object: a state block from the pool, the object made
        // with it, the block cleared.
        template <typename State, typename Pool>
        ee::Ptr<GameObject> newTrickObject(Pool &pool, uint32_t kind, ee::Ptr<State> &state)
        {
            state = ee::Ptr<State>(aggressive::poolTake(*pool));
            const ee::Ptr<GameObject> object = newGameObject(uint64_t(kind), state);
            zge::memoryFill(state, 0, sizeof(State));
            return object;
        }
    }
}
