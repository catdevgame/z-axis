// BMX XXX's level entities: the level object types' registration, the
// script commands that enable, disable and activate objects by name, and
// what the object types share (an object's kind name, the sound of a rider
// touching one). BMX XXX's own code (its game block); no other build names
// its functions, so the names are from what each does, the script commands
// and the strings.

#include "entities.h"
#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/game/player_riders.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/conversions.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/animation_event_commands.h"
#include "zge/scene/follower.h"
#include "zge/scene/locators.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/entities.cpp.h"

namespace game::bmx
{
    using rider::collision::nodeFaceCount;

    namespace
    {
        // One level object type's registration, and the sessions it is left out of.
        struct TypeRegistrar
        {
            ee::FunctionPtr<uint32_t()> registerType; // +0x00
            uint32_t flags;                           // +0x04 kOnlySession0, kNotSession3
        };
        static_assert(sizeof(TypeRegistrar) == 8);
        constexpr uint32_t kOnlySession0 = 1u << 0; // registered only when the session's kind is 0
        constexpr uint32_t kNotSession3 = 1u << 1;  // not registered when the session's kind is 3
        constexpr uint32_t kTypeRegistrars = 20;
        constexpr uint8_t kSession3 = 3;
        inline ee::Global<TypeRegistrar[kTypeRegistrars], sym::g_levelObjectTypeRegistrars> g_levelObjectTypeRegistrars;
    }

    // registerLevelObjectTypes: every level object type the session's kind
    // has registered with the engine. Always 1.
    uint32_t registerLevelObjectTypes()
    {
        for (uint32_t type = 0; type < kTypeRegistrars; ++type)
        {
            const TypeRegistrar &registrar = (*g_levelObjectTypeRegistrars)[type];
            if ((registrar.flags & kOnlySession0) != 0 && *g_sessionKind != 0)
                continue;
            if ((registrar.flags & kNotSession3) != 0 && *g_sessionKind == kSession3)
                continue;
            registrar.registerType();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerLevelObjectTypes, Verified);

    // unregisterLevelObjectTypes: the engine's table of types cleared.
    int32_t unregisterLevelObjectTypes()
    {
        return game::tail(zge::clearLevelObjectTypes);
    }
    READABLE_FUNCTION(unregisterLevelObjectTypes, Verified);

    namespace
    {
        // Where entityKindName leaves its answer.
        constexpr uint32_t kEntityKindName = sym::g_entityKindName;
    }

    // entityKindName: the kind in an object's name, in capitals: what
    // follows the name's first '_', without its last one or two digits
    // ("pu_health01" gives "HEALTH"). Empty for a name with nothing to take;
    // 0 for an object with no node. The answer is one shared buffer.
    ee::Ptr<char> entityKindName(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<const char> name = calls::objectLevelNodeName(object);
        if (!name)
            return ee::Ptr<char>();
        const ee::Ptr<char> kind(kEntityKindName);
        kind[0] = 0;
        const uint32_t length = zge::stringLength(name);
        ee::Ptr<const char> from = name;
        for (;;)
        {
            const char c = *from;
            if (c == 0)
                return kind;
            ++from;
            if (c == '_')
                break;
        }
        if (*from == 0)
            return kind;
        ee::Ptr<const char> last = name + (length - 1);
        if (!(from < last))
            return kind;
        uint32_t digits = 0;
        while (static_cast<uint8_t>(*last) - '0' < 10u)
        {
            last = last - 1;
            if (!(from < last))
                return kind;
            ++digits;
            if (digits >= 2)
                break;
        }
        uint32_t written = 0;
        if (!(last < from))
        {
            do
            {
                uint8_t c = static_cast<uint8_t>(*from);
                if (c - 'a' < 26u)
                    c = static_cast<uint8_t>(c - 0x20);
                kind[written] = static_cast<char>(c);
                ++from;
                ++written;
            } while (!(last < from));
        }
        kind[written] = 0;
        return kind;
    }
    READABLE_FUNCTION(entityKindName, Verified);

    namespace
    {
        // The sound names of the touch kinds (an empty one: no sound).
        inline ee::Global<ee::Ptr<const char>[kTouchKinds], sym::touchSoundNames> g_touchSoundNames;
        constexpr uint32_t kTouchSoundFormat = sym::bmx::touchSoundFormat; // "%s.%s"
        // A frame's share of a second (1 / 60): the touched object's velocity is a second's.
        inline ee::Global<ee::Float, sym::g_touchFrameSeconds> g_touchFrameSeconds;
        // The slowest touch that sounds.
        inline ee::Global<ee::Float, sym::g_touchSoundMinSpeed> g_touchSoundMinSpeed;
    }

    // playTouchSound (a rider's touch message, the object's sound name):
    // the object's sound for the touch's kind ("<name>.bump", ".tap",
    // ".grind", ".crash"), when the rider meets it fast enough: the speed
    // into the touched face (the rider's velocity less the object's own,
    // where the object answers the velocity message), into the touch's own
    // normal when it has no face, or the rider's speed. 1 when a sound
    // started.
    uint32_t playTouchSound(ee::Ptr<zge::EngineMessage> message, ee::Address name)
    {
        ee::Local<zge::EngineMessage> velocityQuery;
        ee::Local<uint32_t> pad;
        ee::Local<zge::Vec4> objectVelocity;
        ee::Local<zge::Vec4> relative;
        ee::Local<zge::Vec4> normal;
        ee::Local<char[0x40]> soundName;
        ee::Local<ee::Ptr<const char>[kTouchKinds]> kindNames;
        const ee::Ptr<RiderTouch> touch(message->data);
        for (uint32_t kind = 0; kind < kTouchKinds; ++kind)
            kindNames[kind] = (*g_touchSoundNames)[kind];

        ee::Float speed;
        if (touch->node != 0)
        {
            if (zge::findSubscription(ee::Ptr<zge::MessageTarget>(message->target), zge::kVelocityMessage) != 0)
            {
                velocityQuery->target = message->target;
                velocityQuery->sender = message->sender;
                velocityQuery->type = zge::kVelocityMessage;
                velocityQuery->flags = zge::kMessageNow;
                velocityQuery->data = objectVelocity;
                zge::sendEngineMessage(velocityQuery);
                *objectVelocity = *objectVelocity * *g_touchFrameSeconds;
                *relative = touch->velocity - *objectVelocity;
            }
            else
            {
                *objectVelocity = zge::kZero;
                *relative = touch->velocity;
            }
            const uint32_t node = touch->node;
            const uint32_t face = touch->face;
            const uint64_t faceHandle = nodeFaceCount(node) == 0 ? 0 : (static_cast<uint64_t>(face) << 32) | node;
            rider::collision::faceNormal(faceHandle, &normal->x);
            speed = -zge::dot(*normal, *relative);
        }
        else if (touch->hasNormal != 0)
        {
            speed = -zge::dot(touch->normal, touch->velocity);
        }
        else
        {
            speed = zge::vec3Length(&touch->velocity.xyz());
        }
        if (!(*g_touchSoundMinSpeed <= speed))
            return 0;
        const ee::Ptr<const char> kindName = kindNames[touch->kind];
        if (*kindName == 0)
            return 0;
        zge::formatString(soundName, kTouchSoundFormat, name, kindName);
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(message->target));
        return soundEvent(kObjectSoundEvent, position, kObjectSoundOnce, soundName.address(), kFullVolume) != 0;
    }
    READABLE_FUNCTION_WAITS(playTouchSound, Verified);

    // showObjectsNamed: every object of the name shown, or hidden; one that
    // says it is not enabled stays hidden.
    void showObjectsNamed(uint32_t name, uint32_t shown)
    {
        ee::Local<uint32_t> shownSlot;
        *shownSlot = shown;
        zge::forEachObjectNamed(name, sym::showObjectIfEnabled, shownSlot.address());
    }
    READABLE_FUNCTION_WAITS(showObjectsNamed, Verified);

    namespace
    {
        constexpr uint32_t kPathManTypeName = sym::pathManTypeName;         // "PathMan"
        constexpr uint32_t kFastPathManTypeName = sym::fastPathManTypeName; // "FastPathMan"
    }

    // refreshPathMen: every "PathMan" and "FastPathMan" sent its enabled
    // state again.
    int32_t refreshPathMen()
    {
        zge::forEachObjectNamed(kPathManTypeName, sym::resendEnabledState, 0);
        return game::tail(zge::forEachObjectNamed, kFastPathManTypeName, static_cast<uint32_t>(sym::resendEnabledState), 0u);
    }
    READABLE_FUNCTION_WAITS(refreshPathMen, Verified);

    namespace
    {
        // What newBuffer is asked for: a tag table of `records` records of
        // `recordBytes` each.
        struct TagTableRequest
        {
            uint32_t kind : 4;
            uint32_t recordBytes : 10;
            uint32_t records : 18;
            uint32_t reserved;
        };
        static_assert(sizeof(TagTableRequest) == 8);
        constexpr uint32_t kTagTableKind = 3;
    }

    // setObjectTagsFromPoint: a spawn point's tags copied into a table of
    // one record, which becomes the object's own tag data.
    namespace
    {
        // An entity keeps its point's tag table where other objects keep an animator (setObjectTagsFromPoint).
        ee::Ptr<zge::Animator> tagTableAsAnimator(ee::Ptr<zge::TagTableHeader> table) { return table.cast<zge::Animator>(); }
    }

    void setObjectTagsFromPoint(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<TagTableRequest> request;
        request->kind = kTagTableKind;
        request->recordBytes = point->tagBytes;
        request->records = 1;
        request->reserved = 0;
        const ee::Ptr<zge::TagTableHeader> table = zge::newBuffer(request);
        zge::memoryCopy(table + 1, point->tags, point->tagBytes);
        object->animator = tagTableAsAnimator(table);
    }
    READABLE_FUNCTION_WAITS(setObjectTagsFromPoint, Verified);

    // showObjectIfEnabled (forEachObjectNamed's visit; the context is the
    // word wanted): the object shown only if it also says it is enabled
    // (an object that does not answer counts as enabled); hidden otherwise.
    uint32_t showObjectIfEnabled(ee::Ptr<zge::SceneObject> object, ee::Ptr<uint32_t> wanted)
    {
        ee::Local<zge::EngineMessage> query;
        ee::Local<uint32_t> pad;
        ee::Local<uint32_t> enabled;
        uint32_t shown = *wanted;
        if (shown != 0)
        {
            query->flags = zge::kMessageNow;
            query->type = kIsEnabledMessage;
            query->data = enabled;
            *enabled = 1;
            query->target = object;
            query->sender = 0;
            zge::sendEngineMessage(query);
            if (*enabled == 0)
                shown = 0;
        }
        zge::setSceneObjectShown(object, shown);
        return 1;
    }
    READABLE_FUNCTION_WAITS(showObjectIfEnabled, Verified);

    // resendEnabledState (forEachObjectNamed's visit): the object asked
    // whether it is enabled, then sent the enable or the disable message
    // that says so again.
    uint32_t resendEnabledState(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint32_t> pad;
        ee::Local<uint32_t> enabled;
        message->target = object;
        *enabled = 1;
        message->flags = zge::kMessageNow;
        message->type = kIsEnabledMessage;
        message->data = enabled;
        message->sender = 0;
        zge::sendEngineMessage(message);
        const uint32_t answer = *enabled;
        message->target = object;
        message->flags = zge::kMessageNow;
        message->type = answer != 0 ? kEnableMessage : kDisableMessage;
        message->sender = 0;
        message->data = 0;
        zge::sendEngineMessage(message);
        return 1;
    }
    READABLE_FUNCTION_WAITS(resendEnabledState, Verified);

    // entityEnableCommand (the script command ENTITY_ENABLE): the objects
    // the event names are enabled.
    void entityEnableCommand(uint32_t, ee::Address event)
    {
        ee::Local<zge::EngineMessage> message;
        message->flags = zge::kMessageNow;
        message->type = kEnableMessage;
        message->sender = 0;
        message->data = 0;
        zge::sendMessageToEventObjects(ee::Ptr<zge::AnimationEvent>(event), message);
    }
    READABLE_FUNCTION_WAITS(entityEnableCommand, Verified);

    // entityDisableCommand (the script command ENTITY_DISABLE).
    void entityDisableCommand(uint32_t, ee::Address event)
    {
        ee::Local<zge::EngineMessage> message;
        message->flags = zge::kMessageNow;
        message->type = kDisableMessage;
        message->sender = 0;
        message->data = 0;
        zge::sendMessageToEventObjects(ee::Ptr<zge::AnimationEvent>(event), message);
    }
    READABLE_FUNCTION_WAITS(entityDisableCommand, Verified);

    namespace
    {
        // The player whose turn it is (the round's).
        inline ee::Global<uint8_t, sym::g_currentPlayer> g_activatingPlayer;
    }

    // entityActivateCommand (the script command ENTITY_ACTIVATE): the
    // objects the event names are activated for the current player.
    void entityActivateCommand(uint32_t, ee::Address event)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint32_t> pad;
        ee::Local<ActivationEvent> activation;
        message->flags = zge::kMessageNow;
        message->type = kActivateMessage;
        message->data = activation;
        activation->player = *g_activatingPlayer;
        message->sender = 0;
        activation->quiet = 0;
        activation->velocity = zge::kZero;
        zge::sendMessageToEventObjects(ee::Ptr<zge::AnimationEvent>(event), message);
    }
    READABLE_FUNCTION_WAITS(entityActivateCommand, Draft);

    // pauseObjectAnimations: every object told to hold its animation.
    void pauseObjectAnimations()
    {
        game::tail(zge::forEachObject, static_cast<uint32_t>(sym::sendPauseAnimation), ee::Address(0u));
    }
    READABLE_FUNCTION(pauseObjectAnimations, Verified);

    // resumeObjectAnimations: every object told to let its animation go on.
    void resumeObjectAnimations()
    {
        game::tail(zge::forEachObject, static_cast<uint32_t>(sym::sendResumeAnimation), ee::Address(0u));
    }
    READABLE_FUNCTION(resumeObjectAnimations, Verified);

    // sendPauseAnimation (forEachObject's visit).
    uint32_t sendPauseAnimation(ee::Ptr<zge::SceneObject> object)
    {
        zge::sendNewMessage(0u, object, kPauseAnimationMessage, 0u, zge::kMessageNow);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendPauseAnimation, Verified);

    // sendResumeAnimation (forEachObject's visit).
    uint32_t sendResumeAnimation(ee::Ptr<zge::SceneObject> object)
    {
        zge::sendNewMessage(0u, object, kResumeAnimationMessage, 0u, zge::kMessageNow);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendResumeAnimation, Verified);

    // entitiesFrameNoop: the level entities' turn in the game's frame, right
    // after the objects' update: nothing in this build.
    void entitiesFrameNoop()
    {
    }
    READABLE_FUNCTION(entitiesFrameNoop, Verified);

    namespace
    {
        // The level's random numbers: a multiplicative generator stepped
        // without overflow (the modulus split by the multiplier into a
        // quotient and a remainder). The four numbers are the game's data:
        // 1588635695, 4294967291, 2 and 1117695901.
        inline ee::Global<uint32_t, sym::g_randomMultiplier> g_randomMultiplier;
        inline ee::Global<uint32_t, sym::g_randomModulus> g_randomModulus;
        inline ee::Global<uint32_t, sym::g_randomQuotient> g_randomQuotient;
        inline ee::Global<uint32_t, sym::g_randomRemainder> g_randomRemainder;
    }

    // seededRandom: the caller's generator state moved on one step and
    // answered as a fraction of the modulus, 0 .. 1. Each user keeps its own
    // state (a path man, a car at a junction), so they stay out of step.
    ee::Float seededRandom(ee::Ptr<uint32_t> state)
    {
        const uint32_t old = *state;
        const uint32_t quotient = *g_randomQuotient;
        const uint32_t next = *g_randomMultiplier * (old % quotient) - *g_randomRemainder * (old / quotient);
        *state = next;
        return zge::unsignedToFloat(next) / zge::unsignedToFloat(*g_randomModulus);
    }
    READABLE_FUNCTION(seededRandom, Verified);

    namespace
    {
        // An object's exit weights in its tag data, at the offset its type
        // gives: three floats (percentages in the level's data) and the
        // state of the object's own random numbers.
        struct ExitWeights
        {
            ee::Float first;      // +0x00 the weight of exit 0
            ee::Float unused;     // +0x04 counted in the total only
            ee::Float second;     // +0x08 the weight of exit 1
            uint32_t randomState; // +0x0C
        };
        static_assert(offsetof(ExitWeights, second) == 8 && offsetof(ExitWeights, randomState) == 0xC);
        constexpr uint32_t kTagFloat = 8; // the tag accessors' second argument
        // Weights that add up to nothing are replaced by thirds.
        inline ee::Global<ee::Float, sym::g_exitNoWeights> g_exitNoWeights;   // 0.00001
        inline ee::Global<ee::Float, sym::g_exitThirdWeight> g_exitThirdWeight; // 33.3
        inline ee::Global<ee::Float, sym::g_exitThirdsTotal> g_exitThirdsTotal; // 99.9
        constexpr ee::Float kPercent(100.0f);
        constexpr uint32_t kExits = 3;
    }

    // pickExitByWeight: 0, 1 or 2 by the object's exit weights and its next
    // random number: 0 up to the first weight's share, 1 up to the first and
    // second's, 2 past them. Each share is the weight over the total, times
    // 100 over the total again: a fraction when the weights add up to 100.
    uint32_t pickExitByWeight(ee::Ptr<zge::SceneObject> object, uint32_t exitTags)
    {
        ee::Float first = zge::tagFloat(zge::objectComponent(object), kTagFloat, exitTags + offsetof(ExitWeights, first));
        const ee::Float unused = zge::tagFloat(zge::objectComponent(object), kTagFloat, exitTags + offsetof(ExitWeights, unused));
        ee::Float second = zge::tagFloat(zge::objectComponent(object), kTagFloat, exitTags + offsetof(ExitWeights, second));
        ee::Float total = first + second + unused;
        if (ee::abs(total) < *g_exitNoWeights)
        {
            first = *g_exitThirdWeight;
            total = *g_exitThirdsTotal;
            second = first;
        }
        first = first / total;
        second = second / total;
        const ee::Float scale = kPercent / total;
        first = first * scale;
        second = second * scale;
        // The generator's state is the tag data's own word, stepped in place.
        const ee::Ptr<uint32_t> state = &zge::tagFieldAs<ExitWeights>(zge::objectComponent(object), exitTags)->randomState;
        second = second + first;
        const ee::Float random = seededRandom(state);
        if (random <= first)
            return 0;
        if (random <= second)
            return 1;
        return 2;
    }
    READABLE_FUNCTION_WAITS(pickExitByWeight, Draft);

    // pickSegmentExit: the segment a follower's object takes at the end of
    // its segment (`atStart`: the end it is at), by its name hash:
    //  - an exit whose start tags name the object is taken at once;
    //  - no exit: kNoSegmentExit; one exit: that one;
    //  - two: a pick by the object's weights (a pick of 2 is thrown again),
    //    0 the lower exit there is, 1 the higher;
    //  - three: the pick's own exit.
    uint32_t pickSegmentExit(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::Follower> follower, uint32_t atStart, uint32_t exitTags)
    {
        uint32_t names[kExits];
        uint32_t last = 0;
        const uint32_t objectName = zge::objectNameHash(object);
        for (uint32_t exit = 0; exit < kExits; ++exit)
        {
            const ee::Ptr<zge::PathRecord> segment = zge::segmentExit(zge::followerSegment(follower), exit, atStart);
            if (!segment)
            {
                names[exit] = 0;
                continue;
            }
            names[exit] = zge::pathNameHash(segment);
            if (zge::pathStartTagsInclude(segment, objectName) != 0)
                return names[exit];
        }
        uint32_t exits = 0;
        for (uint32_t exit = 0; exit < kExits; ++exit)
        {
            if (names[exit] != 0)
            {
                last = names[exit];
                ++exits;
            }
        }
        if (exits == 0)
            return kNoSegmentExit;
        if (exits == 1)
            return last;
        if (exits == 2)
        {
            uint32_t pick;
            do
                pick = pickExitByWeight(object, exitTags);
            while (pick == 2);
            if (pick == 0)
                return names[0] != 0 ? names[0] : names[1];
            return names[1] != 0 ? names[1] : names[2];
        }
        return names[pickExitByWeight(object, exitTags)];
    }
    READABLE_FUNCTION_WAITS(pickSegmentExit, Verified);

    namespace
    {
        // A path man hears of a rider's trick or crash within this distance
        // (squared) of the rider.
        constexpr ee::Float kPathManHearsSquared(1000.0f);
    }

    // tellPathMenNearRider: what the current player's rider just did
    // (kRiderDid...) told to the "PathMan" objects, then to the
    // "FastPathMan" ones, near the rider.
    int32_t tellPathMenNearRider(uint32_t what)
    {
        zge::forEachObjectNamed(kPathManTypeName, sym::tellPathManIfNear, what);
        return game::tail(zge::forEachObjectNamed, kFastPathManTypeName, static_cast<uint32_t>(sym::tellPathManIfNear), what);
    }
    READABLE_FUNCTION_WAITS(tellPathMenNearRider, Verified);

    // tellPathManIfNear (forEachObjectNamed's visit; the context is what the
    // rider did): the object sent the message when the current player's
    // rider is near it. Answers 0 once one was told, which ends the visit:
    // the first one found near the rider is the only one of its type told.
    uint32_t tellPathManIfNear(ee::Ptr<zge::SceneObject> object, uint32_t what)
    {
        ee::Local<zge::Vec4> toRider;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_activatingPlayer));
        *toRider = *rider::vec4(rider->position) - *zge::objectPosition(object);
        if (!(zge::dot(*toRider, *toRider) <= kPathManHearsSquared))
            return 1;
        zge::sendNewMessage(0u, object, kRiderDidMessage, what, zge::kMessageNow);
        return 0;
    }
    READABLE_FUNCTION_WAITS(tellPathManIfNear, Verified);
}
