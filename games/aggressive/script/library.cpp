// The level scripts' natives (script.h, natives.cpp has their names): each
// pops its arguments off the script stack, calls into the game and pushes
// its result, returning 1 when it pushed one. The ones the Tutorial and the
// Movie Lot career run.

#include "native_parts.h"

#include "child_states.h"

#include "aggressive/npc.h"
#include "zge/scene/node.h"
#include "zge/zgui/script.h"
#include "script.h"

#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "aggressive/sound/events.h"
#include "aggressive/timers.h"
#include "aggressive/hud/hud.h"
#include "aggressive/fx/library.h"
#include "aggressive/level/animated_objects.h"
#include "aggressive/level/closers.h"
#include "aggressive/level/objectives.h"
#include "aggressive/level/props.h"
#include "aggressive/level/scene_objects.h"
#include "sym/aggressive/script/library.cpp.h"

namespace aggressive::script
{
    using namespace parts;

    namespace
    {
        // The class names nativePlayAnim tells nodes apart by, hashed once.
        struct AnimClassHashes
        {
            uint32_t animObj, animPropDef;
        };
        inline ee::Global<AnimClassHashes, sym::hashes> g_animClassHashes;
        inline ee::Global<uint32_t, sym::firstCall> g_animClassHashesPending;
        inline ee::Global<uint32_t, sym::g_touchedBreakableCount> g_touchedBreakableCount;
        // Level::flags' low bits as playAnim leaves them: no reader of either
        // is known, so they keep the name of what they do here.
        constexpr uint16_t kLevelAnimationWatch = 0x2, kLevelAnimationPoked = 0x1;
    }

    namespace
    {
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameTypeByte;
        inline ee::Global<uint32_t, sym::g_levelNumber> g_levelWord;
        constexpr uint8_t kTimedRun = 3;

    }

    // getOBBData (and its twins): copies the message's first word to a variable.
    int32_t nativeGetObbData()
    {
        const ee::Ptr<uint32_t> data(scriptPopMessageData());
        const ee::Ptr<uint32_t> out(static_cast<uint32_t>(popInt()));
        *out = *data;
        return 0;
    }
    READABLE_FUNCTION(nativeGetObbData, Verified);

    // setObjectiveState(objective, state)
    int32_t nativeSetObjectiveState()
    {
        const int32_t objective = popCondition();
        const int32_t state = popInt();
        aggressive::level::completeChallenge(objective, state); // hooked by the restart override: by address
        return 0;
    }
    READABLE_FUNCTION(nativeSetObjectiveState, Verified);

    // setPowerUpState(powerUp, state) -> result
    int32_t nativeSetPowerUpState()
    {
        const int32_t powerUp = popInt();
        const int32_t state = popInt();
        pushInt(aggressive::level::setPropPowerUpState(powerUp, state));
        return 1;
    }
    READABLE_FUNCTION(nativeSetPowerUpState, Verified);

    // parkEditorAllEditorPiecesTouched() -> 1 once every breakable of the
    // level was touched (fx/skater_effects.cpp).
    int32_t parkEditorAllEditorPiecesTouched()
    {
        const uint32_t breakables = aggressive::fx::countBreakables();
        pushInt(*g_touchedBreakableCount == breakables ? 1 : 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(parkEditorAllEditorPiecesTouched, Verified);

    // parkEditorResetTouchesPiece(): none touched.
    int32_t parkEditorResetTouchesPiece()
    {
        aggressive::fx::clearTouchedBreakables(); // clearTouchedBreakables
        return 0;
    }
    READABLE_FUNCTION_WAITS(parkEditorResetTouchesPiece, Verified);

    // displayOverlayText(text, seconds): seconds <= 0 keeps it up.
    int32_t nativeDisplayOverlayText()
    {
        const ee::Ptr<char> text = scriptPopString(ee::Ptr<uint32_t>());
        const ee::Float seconds = popFloat();
        aggressive::hud::showOverlayText(text, seconds <= 0.0f ? 1 : 0, seconds);
        return 0;
    }
    READABLE_FUNCTION(nativeDisplayOverlayText, Verified);

    // destroyNPCBubble(bubble)
    int32_t nativeDestroyNpcBubble()
    {
        aggressive::destroyNpcBubble(popInt());
        return 0;
    }
    READABLE_FUNCTION(nativeDestroyNpcBubble, Verified);

    // createNPCBubble(a, b, c, d, e, f): six words as the bubble code takes them.
    int32_t nativeCreateNpcBubble()
    {
        const int32_t a = popInt();
        const int32_t b = popInt();
        const int32_t c = popInt();
        const int32_t d = popInt();
        const int32_t e = popInt();
        const int32_t f = popInt();
        aggressive::createNpcBubble(a, b, c, d, e, f);
        return 0;
    }
    READABLE_FUNCTION(nativeCreateNpcBubble, Verified);

    // releaseTimer(timer)
    int32_t nativeReleaseTimer()
    {
        aggressive::releaseTimer(popInt());
        return 0;
    }
    READABLE_FUNCTION(nativeReleaseTimer, Verified);

    // setTimer(timer, seconds)
    int32_t nativeSetTimer()
    {
        const int32_t timer = popInt();
        const ee::Float seconds = popFloat();
        aggressive::setTimer(timer, seconds);
        return 0;
    }
    READABLE_FUNCTION(nativeSetTimer, Verified);

    // createTimer(seconds, object, interval): a timer that messages the
    // object; an interval of -1 has no tick.
    int32_t nativeCreateTimer()
    {
        ee::Local<int32_t> names;
        const ee::Float seconds = popFloat();
        *names = popInt(); // the object's name hash, looked up by address
        const ee::Float interval = popFloat();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(names, 1);
        constexpr uint32_t kTimerExpired = sym::timerFinishedCallback, kTimerTick = sym::timerHurryCallback;
        const ee::Address object = *found;
        const uint32_t timer = interval == -1.0f
                                   ? aggressive::createTimer(kTimerExpired, object, 0, 0, seconds, interval)
                                   : aggressive::createTimer(kTimerExpired, object, kTimerTick, object, seconds, interval);
        pushInt(timer);
        return 1;
    }
    READABLE_FUNCTION(nativeCreateTimer, Verified);

    // destroySkidMarks()
    int32_t nativeDestroySkidMarks()
    {
        aggressive::fx::destroySkidMarks();
        return 0;
    }
    READABLE_FUNCTION(nativeDestroySkidMarks, Verified);

    // getScore() -> player one's score
    int32_t nativeGetScore()
    {
        pushInt(static_cast<uint32_t>(skater::skaterScore(levelPlayer(0))));
        return 1;
    }
    READABLE_FUNCTION(nativeGetScore, Verified);

    // setEmitterState(emitter, state)
    int32_t nativeSetEmitterState()
    {
        const int32_t emitter = popInt();
        const int32_t state = popInt();
        aggressive::fx::setUserEffectState(emitter, state);
        return 0;
    }
    READABLE_FUNCTION(nativeSetEmitterState, Verified);

    // lockSkaters(on): the players held or let go.
    int32_t nativeLockSkaters()
    {
        const bool on = (static_cast<uint32_t>(popInt()) & 1u) != 0;
        const ee::Ptr<Level> level = currentLevel();
        level->flags = static_cast<uint16_t>(on ? level->flags | kLevelPlayersHeld : level->flags & ~kLevelPlayersHeld);
        return 0;
    }
    READABLE_FUNCTION(nativeLockSkaters, Verified);

    // isFakie(skater) -> whether the skater rides fakie.
    int32_t nativeIsFakie()
    {
        ee::Local<char[kComplaintBytes]> complaint;
        const int32_t index = popCondition();
        const ee::Ptr<skater::Player> player = levelPlayer(index);
        complainIfNoSkater(player, complaint, index);
        pushInt(player->fakie);
        return 1;
    }
    READABLE_FUNCTION(nativeIsFakie, Verified);

    // isKeyAwarded(skater) -> whether this level's key is among the skater's
    // loot.
    int32_t nativeIsKeyAwarded()
    {
        const int32_t index = popCondition();
        const uint64_t loot = skater::skaterProfile(levelPlayer(index))->lootFlags;
        const uint32_t key = (g_gameState->level + 9u) & 63u;
        pushInt(((loot >> key) & 1u) ? 1u : 0u);
        return 1;
    }
    READABLE_FUNCTION(nativeIsKeyAwarded, Verified);

    // playSound(sound, flag, c, d, e): sound 0x30 plays only with flag 1, as 0x6B.
    int32_t nativePlaySound()
    {
        const int32_t sound = popCondition();
        int32_t flag = popCondition();
        const int32_t c = popCondition();
        const int32_t d = popCondition();
        const int32_t e = popCondition();
        if (sound == 0x30)
        {
            if (flag != 1)
                return 0;
            flag = 0x6B;
        }
        aggressive::soundEvent(sound, flag, c, d, e);
        return 0;
    }
    READABLE_FUNCTION(nativePlaySound, Verified);

    // setChildVisible(object, child, visible): the object's scene nodes
    // walked with the visibility (setChildShownByHash), then the object told.
    int32_t nativeSetChildVisible()
    {
        ee::Local<ChildProperties> properties;
        ee::Local<int32_t> object;
        *object = popInt();
        properties->childHash = popInt();
        const int32_t visible = popInt();
        properties->shown = visible;
        properties->collides = visible;
        properties->wholeTree = 1;
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(object, 1);
        if (!found)
            return 0;
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(*found);
        if (node)
        {
            zge::forEachNodeInSubtree(node, sym::setChildShownByHash, properties.address());
            recordChildState(*object, properties);
        }
        return 0;
    }
    READABLE_FUNCTION(nativeSetChildVisible, Verified);

    // setObjectVisible(object, visible, keep): shows or hides the object's
    // node (and its collision); keep 0 puts the node's own flags back after.
    int32_t nativeSetObjectVisible()
    {
        ee::Local<int32_t> names;
        *names = popInt();
        const int32_t visible = popInt();
        const int32_t keep = popInt();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(names, 1);
        if (!found)
            return 0;
        const ee::Ptr<zge::Node> node(zge::objectLevelNode(*found));
        if (node)
        {
            const uint32_t wasVisible = node->flags & zge::kNodeShown;
            zge::setNodeShown(node, visible, 1);
            zge::setNodeCollides(node, visible, 1);
            if (!keep)
            {
                zge::setNodeShown(node, wasVisible, 0);
                zge::setNodeCollides(node, wasVisible, 0);
            }
            queueObjectVisibility(*names, visible, keep);
        }
        return 0;
    }
    READABLE_FUNCTION(nativeSetObjectVisible, Verified);

    // resetJuice(skater): the carton back to half its size, not jizzed.
    int32_t nativeResetJuice()
    {
        ee::Local<char[kComplaintBytes]> complaint;
        ee::Local<int32_t> cartonSize;
        const int32_t index = popCondition();
        const ee::Ptr<skater::Player> player = levelPlayer(index);
        complainIfNoSkater(player, complaint, index);
        *cartonSize = -1;
        ee::Float capacity = 0.0f;
        if (zge::zgui::copyVariable(player->juiceMeter, sym::g_textCartonSize4, cartonSize))
            capacity = ee::toFloat(*cartonSize);
        skater::setJuiceMeter(player->juiceMeter, capacity / 2.0f);
        aggressive::skater::setJizzed(player, 0);
        return 0;
    }
    READABLE_FUNCTION(nativeResetJuice, Verified);

    // playCameraAnim(name) -> the camera's result: a scripted camera; the
    // level state 4 in a Timed Run, else 10, while it runs.
    int32_t nativePlayCameraAnim()
    {
        const ee::Ptr<char> name = scriptPopString(ee::Ptr<uint32_t>());
        const SavedMachine saved = saveMachine();
        setLevelState(*g_gameTypeByte == kTimedRun ? 4u : 10u);
        const uint32_t result = aggressive::playCameraAnimation(&*g_followCamera, name, 0, 0);
        restoreMachine(saved);
        pushInt(result);
        return 1;
    }
    READABLE_FUNCTION(nativePlayCameraAnim, Verified);

    // respawnSkaterName(skater, locator): the skater back at a named locator.
    int32_t nativeRespawnSkaterName()
    {
        ee::Local<char[kComplaintBytes]> complaint;
        const int32_t index = popCondition();
        const ee::Ptr<char> locator = scriptPopString(ee::Ptr<uint32_t>());
        const ee::Ptr<skater::Player> player = levelPlayer(index);
        complainIfNoSkater(player, complaint, index);
        const SavedMachine saved = saveMachine();
        player->flags |= skater::kRespawnAtLocator;
        aggressive::skater::placeSkaterAtLevelStart(player, zge::nameHash(locator.get()), 1);
        restoreMachine(saved);
        return 0;
    }
    READABLE_FUNCTION(nativeRespawnSkaterName, Verified);

    // playAnim(object, animation, speed, loop) -> started: an AnimObj /
    // AnimPropDef node plays it itself (playObjectAnimation); else an object with an
    // animation controller (findCloserListener) gets the animation found by name;
    // else the object is just poked (objectNode) and the level marked.
    int32_t nativePlayAnim()
    {
        ee::Local<int32_t> objectName;
        ee::Local<ee::Ptr<zge::Motion>> animationId;
        *objectName = 0;
        *objectName = popInt();
        const ee::Ptr<char> animation = scriptPopString(ee::Ptr<uint32_t>());
        const ee::Float speed = popFloat();
        const int32_t loop = popCondition();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(objectName, 1);
        uint32_t started = 0;
        const ee::Ptr<zge::TagRecord> node = zge::objectComponent(*found);
        if (node)
        {
            // The class names' hashes are made on the first call.
            if (*g_animClassHashesPending)
            {
                g_animClassHashes->animObj = zge::nameHash(ee::Ptr<const char>(sym::g_textAnimObj2));         // "AnimObj"
                g_animClassHashes->animPropDef = zge::nameHash(ee::Ptr<const char>(sym::g_textAnimPropDef)); // "AnimPropDef"
                *g_animClassHashesPending = 0;
            }
            const uint32_t nodeClass = zge::tagWord(node, 8, 0);
            if (nodeClass == g_animClassHashes->animObj || nodeClass == g_animClassHashes->animPropDef)
            {
                pushInt(aggressive::level::playObjectAnimation(*found, animation, loop, speed));
                return 1;
            }
        }
        if (aggressive::level::findCloserListener(*found))
        {
            const ee::Ptr<Npc> npc(aggressive::pickupOf(*found));
            *animationId = {};
            if (!npc->invisible)
            {
                started = zge::findAnimation(animation, animationId);
                if (started)
                {
                    if (npc->locked)
                    {
                        npc->scriptAnimationMode = loop;
                        npc->scriptAnimation = *animationId;
                    }
                    started = zge::playAnimation(*found, *animationId, loop, speed, npc->animationSpeed);
                }
            }
        }
        else
        {
            aggressive::objectNode(*found);
            const ee::Ptr<Level> level = currentLevel();
            if (level->flags & kLevelAnimationWatch)
                level->flags |= kLevelAnimationPoked;
        }
        pushInt(started);
        return 1;
    }
    READABLE_FUNCTION(nativePlayAnim, Verified);

    // soundSourceSetState(source, state): a level sound source on or off
    // (the free skate scripts' ambience).
    int32_t nativeSoundSourceSetState()
    {
        const int32_t source = popInt();
        const int32_t state = popInt();
        aggressive::setSoundEmitterState(source, state);
        return 0;
    }
    READABLE_FUNCTION(nativeSoundSourceSetState, Verified);
}
