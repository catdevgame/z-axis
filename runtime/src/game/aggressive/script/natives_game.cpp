// The natives that were still only registered: the rest of the message data
// getters, objectives, the follow camera's scripted paths, timers, NPCs,
// Animal Rescue ("parrot") and the level's effects. Same conventions as
// library.cpp (native_parts.h). The strings in the frames are debug prints
// the retail game compiled out; their formatting stays.

#include "child_states.h"
#include "native_parts.h"

#include "aggressive/camera.h"
#include "aggressive/hud/hud.h"
#include "aggressive/skater/action_tricks.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/scene/animation.h"
#include "zge/scene/query.h"
#include "zge/text/strings.h"
#include "sym/aggressive/script/natives_game.cpp.h"
#include "zge/scene/animator.h"
#include "ee/plain.h"
#include "zge/scene/scene.h"
#include "zge/scene/message.h"
#include "aggressive/skater/skater.h"
#include "aggressive/timers.h"
#include "aggressive/fx/library.h"
#include "aggressive/npc.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/props.h"
#include "aggressive/level/animal_rescue.h"
#include "aggressive/level/dynamics.h"
#include "aggressive/level/objectives.h"
#include "aggressive/level/traffic_car.h"

namespace aggressive::script
{
    using namespace parts;

    namespace
    {
        constexpr uint32_t kStopObjectiveScript = sym::stopObjectiveScript; // "SET InGameUI.OverlayToggle.LabelObj.FadeOut.StartDelay {0}"
        constexpr uint32_t kNpcToLockNotFound = sym::npcToLockNotFound;   // "Could not find the NPC whose name hash is 0x%x for the request to lock it."
        constexpr uint32_t kNpcToUnlockNotFound = sym::npcToUnlockNotFound; // ".. to unlock it."
        constexpr uint32_t kStreamPositionalSpeechEvent = 0x50u; // soundEvent's streamed speech at an object
        constexpr uint32_t kCameraFlashColour = 0x80FFFFFFu;   // white, as the original reads it from gp data
        inline constexpr ee::Float kCameraFlashSeconds = 0.25f;
        constexpr uint32_t kLevelStateScriptedCamera = 10u;    // the level state while a scripted camera runs (library.cpp)
        constexpr uint32_t kCharacterTypeMask = 0x1Fu;
        constexpr uint32_t kTimerFinishedName = sym::g_textTimerFinished;   // "TimerFinished"
        constexpr uint32_t kTimerHurryName = sym::g_textTimerHurry;      // "TimerHurry"

        inline ee::Global<uint32_t, sym::g_timerFinishedHashPending> g_timerFinishedHashPending;
        inline ee::Global<uint32_t, sym::g_timerHurryHashPending> g_timerHurryHashPending;
        inline ee::Global<uint32_t, sym::g_timerFinishedHash> g_timerFinishedHash;
        inline ee::Global<uint32_t, sym::g_timerHurryHash> g_timerHurryHash;

        // A word and a float a skater event hands the script.
        struct WordAndFloat
        {
            int32_t word;
            ee::Float value;
        };
        // A word and two floats (a wall ride or wall tap's end).
        struct WordAndTwoFloats
        {
            int32_t word;
            ee::Float first, second;
        };

        // The word and float getters' shape: two variables by address.
        int32_t copyWordAndFloat()
        {
            const ee::Ptr<WordAndFloat> data(scriptPopMessageData());
            const ee::Ptr<int32_t> word(static_cast<uint32_t>(popInt()));
            const ee::Ptr<ee::Float> value(static_cast<uint32_t>(popInt()));
            *word = data->word;
            *value = data->value;
            return 0;
        }

        int32_t copyWordAndTwoFloats()
        {
            const ee::Ptr<WordAndTwoFloats> data(scriptPopMessageData());
            const ee::Ptr<int32_t> word(static_cast<uint32_t>(popInt()));
            const ee::Ptr<ee::Float> first(static_cast<uint32_t>(popInt()));
            const ee::Ptr<ee::Float> second(static_cast<uint32_t>(popInt()));
            *word = data->word;
            *first = data->first;
            *second = data->second;
            return 0;
        }

        // Finds the object the name hash at `hashAddress` names (a slot, its
        // first word the object).
        ee::Ptr<ee::Ptr<zge::SceneObject>> findNamedObject(ee::Address hashAddress)
        {
            return zge::findObjects(hashAddress, 1);
        }

        // lockNPCFromAI and unlockNPCFromAI: the NPC named by the hash told
        // (lockNpc when `lock`, else unlockNpc), its result pushed (0 when there
        // is no such NPC: a message is formatted, and nothing shows it).
        int32_t changeNpcLock(bool lock, uint32_t notFoundFormat)
        {
            ee::Local<char[0x400]> message;
            ee::Local<int32_t> nameHash;
            int32_t done = 0;
            *nameHash = popInt();
            const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(nameHash);
            if (!slot)
                formatUnread(message.address(), notFoundFormat, *nameHash);
            else
                done = lock ? lockNpc(*slot) : unlockNpc(*slot);
            pushInt(done);
            return 1;
        }
    }

    // ---- animation ----------------------------------------------------------

    // stopAnim(object)
    int32_t nativeStopAnim()
    {
        ee::Local<int32_t> popIntSlot;
        *popIntSlot = popInt();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(popIntSlot);
        zge::stopAnimation(*slot);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeStopAnim, Verified);

    // extractFloatFromTagData(event, field) -> the animation event's float.
    int32_t nativeExtractFloatFromTagData()
    {
        const int32_t event = popInt();
        const int32_t field = popInt();
        const ee::Float value = zge::eventFloat(ee::Ptr<zge::AnimationEvent>(static_cast<uint32_t>(event)), field);
        pushFloat(value);
        return 1;
    }
    READABLE_FUNCTION(nativeExtractFloatFromTagData, Verified);

    // ---- message data -------------------------------------------------------

    // getGrindEndData, getManualEndData, getAcidDropLandData: the trick and
    // its worth.
    int32_t nativeGetGrindEndData() { return copyWordAndFloat(); }
    READABLE_FUNCTION(nativeGetGrindEndData, Verified);

    // getStallEndData
    int32_t nativeGetStallEndData() { return copyWordAndFloat(); }
    READABLE_FUNCTION(nativeGetStallEndData, Verified);

    // getCollideData
    int32_t nativeGetCollideData() { return copyWordAndFloat(); }
    READABLE_FUNCTION(nativeGetCollideData, Verified);

    // getWaitSuccessData(second word *, first word *)
    int32_t nativeGetWaitSuccessData()
    {
        const ee::Ptr<int32_t> data(scriptPopMessageData());
        const ee::Ptr<int32_t> second(static_cast<uint32_t>(popInt()));
        const ee::Ptr<int32_t> first(static_cast<uint32_t>(popInt()));
        *second = data[1];
        *first = data[0];
        return 0;
    }
    READABLE_FUNCTION(nativeGetWaitSuccessData, Verified);

    // getWallRideEndData
    int32_t nativeGetWallRideEndData() { return copyWordAndTwoFloats(); }
    READABLE_FUNCTION(nativeGetWallRideEndData, Verified);

    // getWallTapEndData
    int32_t nativeGetWallTapEndData() { return copyWordAndTwoFloats(); }
    READABLE_FUNCTION(nativeGetWallTapEndData, Verified);

    // getOBBPlayer(player *): the second word of a box event.
    int32_t nativeGetObbPlayer()
    {
        const ee::Ptr<int32_t> data(scriptPopMessageData());
        const ee::Ptr<int32_t> player(static_cast<uint32_t>(popInt()));
        *player = data[1];
        return 0;
    }
    READABLE_FUNCTION(nativeGetObbPlayer, Verified);

    // getMessageDataTimerID() -> the message data itself.
    int32_t nativeGetMessageDataTimerId()
    {
        pushInt(scriptPopMessageData());
        return 1;
    }
    READABLE_FUNCTION(nativeGetMessageDataTimerId, Verified);

    // ---- objectives -----------------------------------------------------------

    // displayObjectiveText(objective)
    int32_t nativeDisplayObjectiveText()
    {
        aggressive::level::showObjectiveText(popCondition());
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeDisplayObjectiveText, Verified);

    // displayObjectiveTitle(objective)
    int32_t nativeDisplayObjectiveTitle()
    {
        aggressive::level::objectiveTitle(popCondition());
        return 0;
    }
    READABLE_FUNCTION(nativeDisplayObjectiveTitle, Verified);

    // stopObjective(): the overlay's label fades now.
    int32_t nativeStopObjective()
    {
        zgui::runScript(*zgui::g_root, ee::Ptr<char>(kStopObjectiveScript), 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeStopObjective, Verified);

    // setBonusObjectiveState(objective, state)
    int32_t nativeSetBonusObjectiveState()
    {
        const int32_t objective = popCondition();
        const int32_t state = popInt();
        aggressive::level::awardObjective(objective, state);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeSetBonusObjectiveState, Verified);

    // ---- the scripted camera --------------------------------------------------

    // playCameraAnimWithFOV(name, fov) -> the camera's result.
    int32_t nativePlayCameraAnimWithFov()
    {
        ee::Local<ee::Float> fov;
        const ee::Ptr<char> name = popString();
        *fov = popFloat();
        pushInt(playCameraAnimation(&*g_followCamera, name, fov, 0));
        return 1;
    }
    READABLE_FUNCTION_WAITS(nativePlayCameraAnimWithFov, Verified);

    // playCameraAnimTarget(name) -> the camera's result: a scripted camera
    // that keeps the skater in view; the game carries on under it.
    int32_t nativePlayCameraAnimTarget()
    {
        const ee::Ptr<char> name = popString();
        setLevelState(kLevelStateScriptedCamera);
        const uint32_t result = playCameraAnimation(&*g_followCamera, name, ee::Ptr<const ee::Float>(), 1);
        pushInt(result);
        return 1;
    }
    READABLE_FUNCTION_WAITS(nativePlayCameraAnimTarget, Verified);

    // ---- the skater -----------------------------------------------------------

    // isSkaterJizzed(skater) -> whether the juice meter is at its top.
    int32_t nativeIsSkaterJizzed()
    {
        const int32_t index = popCondition();
        const uint32_t jizzed = skater::isJizzed(ee::Ptr<skater::Player>(skaterHandle(index)));
        pushInt(jizzed);
        return 1;
    }
    READABLE_FUNCTION(nativeIsSkaterJizzed, Verified);

    // getNum180s(skater) -> half turns of the trick, both ways.
    int32_t nativeGetNum180s()
    {
        const ee::Ptr<skater::Player> player(skaterHandle(popCondition()));
        const int32_t clockwise = skater::clockwiseSpins(player);
        const int32_t counterClockwise = skater::counterClockwiseSpins(player);
        pushResult(clockwise + counterClockwise);
        return 1;
    }
    READABLE_FUNCTION(nativeGetNum180s, Verified);

    // getChallengeScore(skater) -> the skater's points.
    int32_t nativeGetChallengeScore()
    {
        const int32_t index = popCondition();
        pushResult(skater::skaterPoints(ee::Ptr<skater::Player>(skaterHandle(index))));
        return 1;
    }
    READABLE_FUNCTION(nativeGetChallengeScore, Verified);

    // awardInstantaneousPoints(points): to player one's score, shown.
    int32_t nativeAwardInstantaneousPoints()
    {
        const ee::Ptr<skater::Player> player(skaterHandle(0));
        const int32_t points = popInt();
        const int32_t score = skater::skaterScore(player);
        skater::setSkaterScore(player, score + points, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeAwardInstantaneousPoints, Verified);

    // addBonusTrickToTrickQueue(skater, text, points, multiplier) -> queued.
    // An argument out of range pushes a 0 and carries on (what looks like an
    // early return lost in the retail build), so a bad call pushes twice. No
    // shipped script calls it.
    int32_t nativeAddBonusTrickToTrickQueue()
    {
        const int32_t index = popCondition();
        const ee::Ptr<char> text = popString();
        const int32_t pointsBits = popCondition();
        const int32_t multiplier = popCondition();
        if (static_cast<uint32_t>(index) >= 2u)
            pushInt(0);
        if (!text)
            pushInt(0);
        if (pointsBits < 0)
            pushInt(0);
        if (multiplier < 0)
            pushInt(0);
        const ee::Ptr<GameObject> trick = skater::newTextObject(text, multiplier, ee::toFloat(pointsBits));
        if (trick)
        {
            const uint32_t queued = aggressive::beginEndAndQueueObject(ee::Ptr<skater::Player>(skaterHandle(index)), trick);
            pushInt(queued);
            return 1;
        }
        pushInt(0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(nativeAddBonusTrickToTrickQueue, Verified);

    // getCurrentTrickUID(skater) -> the trick being pulled's id, 0 for none.
    int32_t nativeGetCurrentTrickUid()
    {
        const int32_t index = popCondition();
        const ee::Ptr<GameObject> trick = skater::skaterCurrentObject(ee::Ptr<skater::Player>(skaterHandle(index)));
        if (trick)
        {
            pushInt(aggressive::skater::trickKeyOf(trick));
            return 1;
        }
        pushInt(0);
        return 1;
    }
    READABLE_FUNCTION(nativeGetCurrentTrickUid, Verified);

    // respawnSkater(skater): back at the level's "Respawn" or the locator
    // setRespawnLocation left. The respawn can run handlers, so the machine
    // is kept on the stack meanwhile.
    int32_t nativeRespawnSkater()
    {
        ee::Local<char[kComplaintBytes]> complaint;
        const int32_t index = popCondition();
        const ee::Ptr<skater::Player> player = levelPlayer(index);
        complainIfNoSkater(player, complaint, index);
        const SavedMachine saved = saveMachine();
        player->flags |= skater::kRespawnAtLocator;
        skater::respawn(player, 0, 1);
        restoreMachine(saved);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeRespawnSkater, Verified);

    // setRespawnLocation(skater, locator): where the next respawn goes.
    int32_t nativeSetRespawnLocation()
    {
        ee::Local<uint8_t[0x140]> buffer;
        const int32_t index = popCondition();
        const ee::Ptr<char> locator = popString();
        const ee::Ptr<skater::Player> player(skaterHandle(index));
        if (!player)
            formatUnread(buffer.address(), kNullSkaterFormat, index);
        player->respawnLocator = zge::nameHash(locator.get());
        return 0;
    }
    READABLE_FUNCTION(nativeSetRespawnLocation, Verified);

    // awardSecretTrick(skater, trick): the trick's bit in the unlocked set.
    int32_t nativeAwardSecretTrick()
    {
        const int32_t index = popCondition();
        const int32_t trick = popCondition();
        const ee::Ptr<skater::Player> player(skaterHandle(index));
        player->unlockedTricks = static_cast<uint16_t>(player->unlockedTricks | 1 << (trick & 0x1F));
        return 0;
    }
    READABLE_FUNCTION(nativeAwardSecretTrick, Verified);

    // getCharacterType(skater) -> which skater it is.
    int32_t nativeGetCharacterType()
    {
        const ee::Ptr<skater::Player> player(skaterHandle(popCondition()));
        pushInt(player->skaterIndex & kCharacterTypeMask);
        return 1;
    }
    READABLE_FUNCTION(nativeGetCharacterType, Verified);

    // ---- objects and children -------------------------------------------------

    // setChildProperties(object, child, shown, collides, whole tree): the
    // named child of the object's geometry, kept in the level's child states.
    int32_t nativeSetChildProperties()
    {
        ee::Local<ChildProperties> properties;
        ee::Local<int32_t> popIntSlot;
        constexpr uint32_t kProperties = 0x00, kObjectHash = 0x10;
        *popIntSlot = popInt();
        properties->childHash = static_cast<uint32_t>(popInt());
        properties->shown = static_cast<uint32_t>(popInt());
        properties->collides = static_cast<uint32_t>(popInt());
        properties->wholeTree = static_cast<uint32_t>(popInt());
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(popIntSlot);
        if (slot)
        {
            const ee::Ptr<zge::Node> geometry = zge::objectLevelNode(ee::Ptr<zge::SceneObject>(*slot));
            if (geometry)
            {
                zge::forEachNodeInSubtree(geometry, kSetChildShownByHash, properties);
                recordChildState(*popIntSlot, properties);
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeSetChildProperties, Verified);

    // ---- NPCs -----------------------------------------------------------------

    // createNPCPointer(npc, player)
    int32_t nativeCreateNpcPointer()
    {
        const int32_t npc = popInt();
        const int32_t player = popInt();
        aggressive::createNpcPointerNode(npc, player);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeCreateNpcPointer, Verified);

    // destroyNPCPointer(npc, player)
    int32_t nativeDestroyNpcPointer()
    {
        const int32_t npc = popInt();
        const int32_t player = popInt();
        aggressive::destroyNpcPointerNode(npc, player);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeDestroyNpcPointer, Verified);

    // lockNPCFromAI(npc) -> done
    int32_t nativeLockNpcFromAi() { return changeNpcLock(true, kNpcToLockNotFound); }
    READABLE_FUNCTION_WAITS(nativeLockNpcFromAi, Verified);

    // unlockNPCFromAI(npc) -> done
    int32_t nativeUnlockNpcFromAi() { return changeNpcLock(false, kNpcToUnlockNotFound); }
    READABLE_FUNCTION_WAITS(nativeUnlockNpcFromAi, Verified);

    // ---- timers ---------------------------------------------------------------

    // getTimerTime(timer) -> the seconds left.
    int32_t nativeGetTimerTime()
    {
        const int32_t timer = popInt();
        pushFloat(timerTime(timer));
        return 1;
    }
    READABLE_FUNCTION(nativeGetTimerTime, Verified);

    // setTimerState(timer, running)
    int32_t nativeSetTimerState()
    {
        const int32_t timer = popInt();
        const int32_t running = popCondition();
        setTimerRunning(timer, running);
        return 0;
    }
    READABLE_FUNCTION(nativeSetTimerState, Verified);

    // getTimerState(timer) -> running
    int32_t nativeGetTimerState()
    {
        const int32_t timer = popInt();
        pushResult(timerRunning(timer));
        return 1;
    }
    READABLE_FUNCTION(nativeGetTimerState, Verified);

    // getTheVisibleTimer() -> the timer on the HUD
    int32_t nativeGetTheVisibleTimer()
    {
        pushInt(visibleTimer());
        return 1;
    }
    READABLE_FUNCTION(nativeGetTheVisibleTimer, Verified);

    // extendTimer(timer, seconds)
    int32_t nativeExtendTimer()
    {
        const int32_t timer = popInt();
        const ee::Float seconds = popFloat();
        stepTimer(timer, seconds);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeExtendTimer, Verified);

    namespace
    {
        int32_t sendTimerMessage(uint32_t target, uint32_t hash, uint32_t data) { return ee::tailCall<int32_t>(kSendMessage, 0, target, hash, data, 1); }
    }

    // timerFinishedCallback: a timer ran out: its target hears "TimerFinished", with
    // the timer's data.
    int32_t timerFinishedCallback(uint32_t data, uint32_t /*timer*/, uint32_t target)
    {
        if (*g_timerFinishedHashPending)
        {
            *g_timerFinishedHash = zge::nameHash(ee::Ptr<const char>(kTimerFinishedName));
            *g_timerFinishedHashPending = 0;
        }
        return sendTimerMessage(target, *g_timerFinishedHash, data);
    }
    READABLE_FUNCTION_WAITS(timerFinishedCallback, Verified);

    // timerHurryCallback: a timer is about to run out: its target hears
    // "TimerHurry"; the timer the HUD shows starts to shake.
    int32_t timerHurryCallback(uint32_t data, uint32_t stop, uint32_t target, uint32_t /*unused*/, ee::Float seconds)
    {
        if (*g_timerHurryHashPending)
        {
            *g_timerHurryHash = zge::nameHash(ee::Ptr<const char>(kTimerHurryName));
            *g_timerHurryHashPending = 0;
        }
        zge::sendNewMessage(0, target, *g_timerHurryHash, data, 1);
        const uint32_t shown = visibleTimer();
        if (data == shown)
            return game::tail(hud::timerFreakOut, data, stop);
        return static_cast<int32_t>(shown);
    }
    READABLE_FUNCTION_WAITS(timerHurryCallback, Verified);

    // ---- Animal Rescue --------------------------------------------------------

    // parrotHitBase(skater): the animals carried are dropped at the base.
    int32_t nativeParrotHitBase()
    {
        const int32_t index = popInt();
        aggressive::dropAnimals(levelPlayer(index), 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeParrotHitBase, Verified);

    // parrotStartGame()
    int32_t nativeParrotStartGame()
    {
        level::setAnimalRescue(1);
        ee::Ptr<skater::Player>(skaterHandle(0))->droppedAnimals = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeParrotStartGame, Verified);

    // parrotEndGame()
    int32_t nativeParrotEndGame()
    {
        aggressive::dropAnimals(levelPlayer(0), 0);
        level::setAnimalRescue(0);
        ee::Ptr<skater::Player>(skaterHandle(0))->droppedAnimals = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeParrotEndGame, Verified);

    // parrotGetScore(score *): the animals player one brought back.
    int32_t nativeParrotGetScore()
    {
        const ee::Ptr<int32_t> score(static_cast<uint32_t>(popInt()));
        *score = ee::Ptr<skater::Player>(skaterHandle(0))->droppedAnimals;
        return 0;
    }
    READABLE_FUNCTION(nativeParrotGetScore, Verified);

    // ---- sound and effects ----------------------------------------------------

    // createCameraFlash()
    int32_t nativeCreateCameraFlash()
    {
        fx::startScreenFlash(1, kCameraFlashColour, kCameraFlashSeconds);
        return 0;
    }
    READABLE_FUNCTION(nativeCreateCameraFlash, Verified);

    // streamPositionalSpeech(speech, object): played at the object.
    int32_t nativeStreamPositionalSpeech()
    {
        ee::Local<int32_t> popIntSlot;
        *popIntSlot = 0;
        const int32_t speech = popInt();
        *popIntSlot = popInt();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(popIntSlot);
        soundEvent(kStreamPositionalSpeechEvent, speech, *slot, 0, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeStreamPositionalSpeech, Verified);

    // initializeAudioSync(track, object): the object's animation follows the
    // sound track's marks.
    int32_t nativeInitializeAudioSync()
    {
        ee::Local<int32_t> popIntSlot;
        *popIntSlot = 0;
        const int32_t track = popInt();
        *popIntSlot = popInt();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(popIntSlot);
        loadAudioSync(track & 0xFFu, *slot);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeInitializeAudioSync, Verified);

    // createExplosion(object, with debris, size)
    int32_t nativeCreateExplosion()
    {
        ee::Local<int32_t> popIntSlot;
        *popIntSlot = popInt();
        const int32_t withDebris = popInt();
        const ee::Float size = popFloat();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = findNamedObject(popIntSlot);
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(ee::Ptr<zge::SceneObject>(*slot));
        fx::spawnExplosion(position, withDebris, size);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeCreateExplosion, Verified);

    // createSplash(object, z-less direction y, x, size)
    int32_t nativeCreateSplash()
    {
        ee::Local<zge::Vec4> direction;
        ee::Local<int32_t> objectName;
        *objectName = popInt();
        direction->y = popFloat();
        direction->x = popFloat();
        direction->z = 0.0f;
        const ee::Float size = popFloat();
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(objectName, 1);
        fx::splashWith(zge::objectPosition(*found), direction, size);
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeCreateSplash, Verified);

    // setTrafficVisible(visible): the cars and the dynamics objects.
    int32_t nativeSetTrafficVisible()
    {
        if (popInt())
        {
            aggressive::showCars();
            aggressive::level::showDynamicsObjects();
            return 0;
        }
        aggressive::hideCars();
        aggressive::level::hideDynamicsObjects();
        return 0;
    }
    READABLE_FUNCTION_WAITS(nativeSetTrafficVisible, Verified);

    // setVehicleVolume(car, engine level)
    int32_t nativeSetVehicleVolume()
    {
        const int32_t car = popInt();
        const int32_t engineLevel = popCondition();
        aggressive::setTrafficCarVolume(car, engineLevel);
        return 0;
    }
    READABLE_FUNCTION(nativeSetVehicleVolume, Verified);
}
