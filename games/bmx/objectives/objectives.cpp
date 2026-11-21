// BMX XXX's objectives manager, its first half: the module's set-up, a
// level's objectives loaded and freed, a player's frame (the timed
// objective's countdown, the goals' visit, the engine's update), the events
// the game sends the objectives, and what the game asks about them. BMX
// XXX's own code (its game block); no build names these functions: the
// names are from what each one's code does.

#include "objectives.h"

#include "bmx/action/camera_state.h"
#include "bmx/game/round.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/debug/profiler.h"
#include "zge/math/types.h"
#include "zge/render/material.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/objectives/objectives.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        // The nine objective types and the three with no parameters: each
        // one's registration, in the order the game registers them.
        constexpr uint32_t kObjectiveTypeRegistrars = 11;

        // The profiler's bar for the objectives' frame.
        inline ee::Global<uint32_t, sym::g_objectivesProfileBar> g_objectivesProfileBar;

        // What kind of session is played: only kind 0 has objectives.
        inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
        inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // The material the highlights are drawn with, and the frames drawn
        // so far (the highlights pulse by it).
        inline ee::Global<ee::Ptr<zge::Material>, sym::g_highlightMaterial> g_highlightMaterial;
        inline ee::Global<uint32_t, sym::g_highlightFrame> g_highlightFrame;
        // The highlight material's states, by the engine's state numbers
        // (the engine's material code has no names for them yet).
        struct MaterialState
        {
            uint32_t state, value;
        };
        constexpr MaterialState kHighlightMaterialStates[] = {{3, 1}, {4, 0}, {5, 2}, {0xB, 1}};

        // The two icon models ("icon.NPR_Available", "icon.Checkpoint"),
        // each playing the animation of its own name over and over.
        constexpr uint32_t kObjectiveIcons = 2;
        inline ee::Global<ee::Ptr<zge::SceneObject>[kObjectiveIcons], sym::g_objectiveIcons> g_objectiveIcons;
        inline ee::Global<ee::Ptr<zge::Motion>[kObjectiveIcons], sym::g_objectiveIconAnimations> g_objectiveIconAnimations;
        inline ee::Global<ee::Float[kObjectiveIcons], sym::g_objectiveIconTimes> g_objectiveIconTimes;
        constexpr uint32_t kSceneModelKind = 1; // newSceneModelByName's first word, as the game gives it
        constexpr ee::Float kFrameSeconds = 1.0f / 60.0f;

        // The timed objective's countdown: a sound with the seconds left at
        // each of the last five seconds and at zero.
        constexpr uint32_t kTicksPerSecond = 60;
        constexpr uint32_t kCountdownSeconds = 5;
        constexpr uint32_t kCountdownSound = 0x38;
        // The sound played after the goals' visit when its soundAfterVisit
        // is set (nothing sets it).
        constexpr uint32_t kGoalVisitSound = 0x3D;

        // The objectives run in the game's first mode only.
        constexpr uint8_t kObjectivesMode = 0;

        // RiderObjectives::turnAwards bits 33 and 34: the game's end-of-turn
        // code sets each together with one of the level's own bits (2 and 4)
        // in the player's saved goals.
        constexpr uint64_t kTurnLevelAwards = 0x6ull << 32;

        // A wheel's contact that is an edge: the rider's contact kind 5
        // (its contact is read as an edge here; kContactObject's is the
        // object).
        constexpr uint8_t kContactEdge = 5;

        // A wheel's contact as a face or edge reference, and as an object.
        uint64_t contactRef(const rider::WheelContact &wheel)
        {
            uint64_t ref;
            std::memcpy(&ref, wheel.contact, sizeof(ref));
            return ref;
        }
        ee::Ptr<zge::SceneObject> contactObject(const rider::WheelContact &wheel)
        {
            return ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(contactRef(wheel)));
        }

        ee::Ptr<RiderObjectives> playerObjectives(uint32_t player)
        {
            return riderObjectives(rider::riderOf(riderIndexOf(player)));
        }
    }

    // setUpObjectives: the engine's objectives module set up with the game's
    // two hooks, every objective type registered, and the profiler's
    // "objectives" bar made.
    void setUpObjectives()
    {
        ee::Local<zge::ObjectiveGameHooks> hooks;
        ee::Local<uint32_t> barColour;
        *barColour = *ee::Ptr<uint32_t>(sym::objectivesBarColour);
        hooks->cue = sym::objectiveCue;
        hooks->objectAction = sym::objectiveObjectAction;
        zge::initObjectiveSystem(hooks);
        const ee::Ptr<ee::FunctionPtr<void()>> registrars(sym::g_objectiveTypeRegistrars);
        for (uint32_t i = 0; i < kObjectiveTypeRegistrars; ++i)
            registrars[i]();
        *g_objectivesProfileBar = zge::newProfileBar(0, 0, barColour, sym::objectivesBarLabel);
    }
    READABLE_FUNCTION_WAITS(setUpObjectives, Verified);

    // shutDownObjectives: the profiler's bar freed and the engine's types
    // forgotten.
    void shutDownObjectives()
    {
        zge::freeProfileBar(*g_objectivesProfileBar);
        calls::shutDownObjectiveSystemTail();
    }
    READABLE_FUNCTION_WAITS(shutDownObjectives, Verified);

    // loadLevelObjectives: in a session with objectives, the level's
    // "objctv.txt" is loaded and read (a level without the file has none),
    // then every objective gets its place in the goal slots and the current
    // player's goals of the level are counted again. In every session the
    // highlights' material is made.
    void loadLevelObjectives()
    {
        ee::Local<char[0x40]> path;
        ee::Local<GoalRegistration> registration;
        ee::Local<uint32_t> fileBytes;
        if (*g_sessionKind == 0)
        {
            zge::formatString(path, sym::bmx::objectivesFileFormat, sym::g_levelFilePrefix);
            const uint32_t data = zge::loadFile(path, fileBytes);
            if (data != 0)
            {
                const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(data, *fileBytes);
                if (file)
                {
                    zge::readObjectives(ee::Address(file));
                    zge::closeFile(file, 0);
                }
                zge::freeLoadedFile(data);
            }
            zge::memoryFill(sym::g_goalSlots, 0, sizeof(GoalSlot[kLevelObjectives]));
            registration->goal = 0;
            registration->goals = playerLevelGoals(*g_currentPlayer, *g_currentLevel);
            registration->goals->goals = 0;
            registration->goals->gaps = 0;
            calls::forEachObjective(sym::registerGoal, registration);
        }
        *g_highlightMaterial = zge::newMaterial();
        for (const MaterialState &state : kHighlightMaterialStates)
            zge::zgui::rebuildMaterialRegisters(*g_highlightMaterial, 0, state.state, state.value);
    }
    READABLE_FUNCTION_WAITS(loadLevelObjectives, Verified);

    // freeLevelObjectives: the level's objectives freed (in a session that
    // has them) and the highlights' material released.
    void freeLevelObjectives()
    {
        if (*g_sessionKind == 0)
            zge::freeObjectives();
        game::tail(zge::releaseMaterial, *g_highlightMaterial);
    }
    READABLE_FUNCTION_WAITS(freeLevelObjectives, Verified);

    // loadObjectiveIcons: each icon's model made, hidden, with an animator;
    // the animation of the model's name is started on it when there is one.
    // An icon with an empty name is none.
    void loadObjectiveIcons()
    {
        ee::Local<uint32_t[kObjectiveIcons]> names;
        const ee::Ptr<uint32_t> iconNames(sym::objectiveIconNames);
        for (uint32_t i = 0; i < kObjectiveIcons; ++i)
            names[i] = iconNames[i];
        for (uint32_t i = 0; i < kObjectiveIcons; ++i)
        {
            const ee::Ptr<const char> name(names[i]);
            if (*name == 0)
            {
                (*g_objectiveIcons)[i] = ee::Ptr<zge::SceneObject>();
                continue;
            }
            const ee::Ptr<zge::SceneObject> icon = zge::newSceneModelByName(kSceneModelKind, name);
            (*g_objectiveIcons)[i] = icon;
            calls::remakeObjectAnimator(icon);
            zge::setSceneObjectShown((*g_objectiveIcons)[i], 0);
            const ee::Ptr<zge::Motion> animation = calls::findAnimationNamed(ee::Ptr<const char>(names[i]));
            (*g_objectiveIconAnimations)[i] = animation;
            if (animation)
            {
                (*g_objectiveIconTimes)[i] = 0.0f;
                zge::startAnimation((*g_objectiveIcons)[i], animation, 0.0f);
            }
        }
    }
    READABLE_FUNCTION_WAITS(loadObjectiveIcons, Verified);

    // freeObjectiveIcons: each icon's animator and model freed.
    void freeObjectiveIcons()
    {
        for (uint32_t i = 0; i < kObjectiveIcons; ++i)
        {
            const ee::Ptr<zge::SceneObject> icon = (*g_objectiveIcons)[i];
            if (!icon)
                continue;
            zge::freeObjectAnimator(icon);
            zge::releaseSceneModel((*g_objectiveIcons)[i]);
            (*g_objectiveIcons)[i] = ee::Ptr<zge::SceneObject>();
        }
    }
    READABLE_FUNCTION_WAITS(freeObjectiveIcons, Verified);

    // updatePlayerObjectives: a player's frame of the objectives. While the
    // action camera is with the rider, in the game's first mode:
    //  - the player's timed objective counts down (an objective a script
    //    block timed out this frame has no time left at once), with the
    //    countdown's sound at each of the last five seconds. At zero the
    //    reward display is told that the objective timed out, the objective
    //    is timed out, and the objective display is told to show none;
    //  - every objective is visited for the player's goals, with the
    //    objective display's state;
    //  - the engine updates every objective;
    //  - the timer display gets the time left and shows only while a timed
    //    objective runs.
    // The icons' animations run in every mode.
    void updatePlayerObjectives(uint8_t player)
    {
        ee::Local<ObjectiveEvent> frameEvent;
        ee::Local<PlayerGoalVisit> visit;
        ee::Local<RewardMessage> reward;
        ee::Local<uint32_t> ticksShown;
        zge::startProfileBar(*g_objectivesProfileBar);
        if (cameraState() == 0 && gameMode() == kObjectivesMode)
        {
            const ee::Ptr<RiderObjectives> own = playerObjectives(player);
            if (own->timedObjective)
            {
                const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(own->timedObjective);
                if ((info->flags & kObjectiveTimedOutByScript) != 0)
                    own->ticksLeft = 0;
                else
                    own->ticksLeft = own->ticksLeft - 1;
                const uint32_t ticks = own->ticksLeft;
                if (ticks == 0)
                {
                    soundEvent(kCountdownSound, ticks / kTicksPerSecond, 0, 0, 0);
                    reward->what = ee::Address(info);
                    reward->line = kRewardObjectiveTimedOut;
                    reward->player = player;
                    overlayMessage(kOverlayReward, reward);
                    calls::timeOutObjective(own->timedObjective);
                    own->timedObjective = Objective();
                    visit->shown.goal = kNoGoalShown;
                    visit->shown.player = player;
                    overlayMessage(kOverlayObjective, &visit->shown);
                }
                else if (ticks % kTicksPerSecond == 0 && ticks / kTicksPerSecond <= kCountdownSeconds)
                {
                    soundEvent(kCountdownSound, ticks / kTicksPerSecond, 0, 0, 0);
                }
            }
            visit->player = player;
            visit->goal = 0;
            visit->shown.player = player;
            visit->soundAfterVisit = 0;
            overlayDisplayState(kOverlayObjective, &visit->shown);
            calls::forEachObjective(sym::updateGoal, visit);
            if (visit->soundAfterVisit != 0)
                soundEvent(kGoalVisitSound, 0, 0, 0, 0);
            frameEvent->player = player;
            frameEvent->kind = kFrameEvent;
            zge::updateObjectives(ee::Address(frameEvent));
            *ticksShown = own->ticksLeft;
            overlayMessage(kOverlayTimer, ticksShown);
            showOverlayDisplay(kOverlayTimer, own->timedObjective ? 1u : 0u);
        }
        for (uint32_t i = 0; i < kObjectiveIcons; ++i)
        {
            if (!(*g_objectiveIcons)[i])
                continue;
            if (zge::animationDone((*g_objectiveIcons)[i], (*g_objectiveIconTimes)[i]) != 0)
            {
                (*g_objectiveIconTimes)[i] = 0.0f;
                zge::startAnimation((*g_objectiveIcons)[i], (*g_objectiveIconAnimations)[i], 0.0f);
            }
            zge::updateAnimation((*g_objectiveIcons)[i], 0, (*g_objectiveIconTimes)[i]);
            (*g_objectiveIconTimes)[i] = (*g_objectiveIconTimes)[i] + kFrameSeconds;
        }
        zge::stopProfileBar(*g_objectivesProfileBar);
    }
    READABLE_FUNCTION_WAITS(updatePlayerObjectives, Verified);

    // drawObjectiveHighlights: in the game's first mode, the objectives in
    // progress highlight themselves for the current player's rider. The
    // highlights' frame count goes up in every mode.
    void drawObjectiveHighlights()
    {
        if (gameMode() == kObjectivesMode)
            zge::highlightObjectives(ee::Address(rider::riderOf(riderIndexOf(*g_currentPlayer))));
        *g_highlightFrame = *g_highlightFrame + 1;
    }
    READABLE_FUNCTION_WAITS(drawObjectiveHighlights, Verified);

    // sendRecordEvent: in the game's first mode, every running objective is
    // told a game record's new value for the player.
    void sendRecordEvent(uint8_t player, uint32_t record, ee::Float value)
    {
        ee::Local<RecordEvent> event;
        if (gameMode() != kObjectivesMode)
            return;
        event->player = player;
        event->kind = kRecordEvent;
        event->record = record;
        event->value = value;
        calls::forEachObjective(sym::sendEventToObjective, event);
    }
    READABLE_FUNCTION_WAITS(sendRecordEvent, Verified);

    // sendTrickEvent: every running objective is told that the player's
    // rider started or ended a trick on a surface. For a start, each wheel's
    // surface goes with it: the name hash of the object the wheel is on (its
    // contact is an object, or an edge of a node that is a scene object's),
    // else the tag word of its edge or face.
    void sendTrickEvent(uint8_t player, uint32_t started)
    {
        ee::Local<TrickEvent> event;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        event->player = player;
        event->started = started;
        event->kind = kTrickEvent;
        if (started != 0)
        {
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                const rider::WheelContact &contact = rider->wheels[wheel];
                if (contact.kind == kContactEdge)
                {
                    const uint64_t node = rider::collision::nodeOf(rider::collision::widen(static_cast<uint32_t>(contactRef(contact))));
                    const ee::Ptr<zge::SceneObject> object =
                        zge::nodeSceneObject(ee::Ptr<zge::Node>(static_cast<uint32_t>(node)), ee::Ptr<ee::Ptr<zge::SceneBone>>());
                    if (object)
                    {
                        event->sort[wheel] = kTrickOnObject;
                        event->surface[wheel] = zge::objectNameHash(object);
                    }
                    else
                    {
                        event->sort[wheel] = kTrickOnEdge;
                        event->surface[wheel] = rider::collision::edgeTagWord(contactRef(contact));
                    }
                }
                else if (contact.kind == rider::kContactObject)
                {
                    event->sort[wheel] = kTrickOnObject;
                    event->surface[wheel] = zge::objectNameHash(contactObject(contact));
                }
                else
                {
                    event->sort[wheel] = kTrickOnFace;
                    event->surface[wheel] = rider::collision::faceTagWord(contactRef(contact));
                }
            }
        }
        calls::forEachObjective(sym::sendEventToObjective, event);
    }
    READABLE_FUNCTION_WAITS(sendTrickEvent, Verified);

    // sendObjectActivatedEvent: every running objective is told that the
    // player's rider activated a level object, before the object gets the
    // activation. Non-zero when an objective took the object: the game then
    // leaves the object alone.
    uint32_t sendObjectActivatedEvent(uint8_t player, ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::EngineMessage> activation)
    {
        ee::Local<ObjectActivatedEvent> event;
        rider::riderOf(riderIndexOf(player));
        event->player = player;
        event->object = object;
        event->activation = activation;
        event->kind = kObjectActivatedEvent;
        event->taken = 0;
        calls::forEachObjective(sym::sendObjectActivatedToObjective, event);
        return event->taken;
    }
    READABLE_FUNCTION_WAITS(sendObjectActivatedEvent, Verified);

    // growBounds: the box grown to hold the point.
    void growBounds(ee::Ptr<zge::Box> bounds, ee::Address point)
    {
        const ee::Ptr<const zge::Vec3> at(point);
        bounds->low.x = ee::min(bounds->low.x, at->x);
        bounds->low.y = ee::min(bounds->low.y, at->y);
        bounds->low.z = ee::min(bounds->low.z, at->z);
        bounds->high.x = ee::max(bounds->high.x, at->x);
        bounds->high.y = ee::max(bounds->high.y, at->y);
        bounds->high.z = ee::max(bounds->high.z, at->z);
    }
    READABLE_FUNCTION(growBounds, Verified);

    // activatePlayerObjectiveByName: the objective of that name activated.
    void activatePlayerObjectiveByName(uint8_t player, ee::Ptr<const char> name)
    {
        const Objective objective = zge::findObjective(name);
        game::tail(activatePlayerObjective, player, objective);
    }
    READABLE_FUNCTION_WAITS(activatePlayerObjectiveByName, Verified);

    // activatePlayerObjective: the objective activated. Which player asks
    // makes no difference.
    void activatePlayerObjective(uint32_t, Objective objective)
    {
        calls::activateObjectiveTail(objective);
    }
    READABLE_FUNCTION_WAITS(activatePlayerObjective, Verified);

    // playerCanActivateObjectiveByName: playerCanActivateObjective for the
    // objective of that name; 0 when there is none.
    uint32_t playerCanActivateObjectiveByName(uint8_t player, ee::Ptr<const char> name)
    {
        const Objective objective = zge::findObjective(name);
        if (!objective)
            return 0;
        return playerCanActivateObjective(player, objective);
    }
    READABLE_FUNCTION_WAITS(playerCanActivateObjectiveByName, Verified);

    // playerCanActivateObjective: whether the objective can be activated
    // for the player now: it is ready (not active yet, what it waits for
    // complete), and it has no time limit or the player has no timed
    // objective running.
    uint32_t playerCanActivateObjective(uint8_t player, Objective objective)
    {
        if (!objective)
            return 0;
        if (zge::objectiveReadyToActivate(objective) == 0)
            return 0;
        const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(objective);
        if (!playerObjectives(player)->timedObjective)
            return 1;
        return info->timeout == 0.0f ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(playerCanActivateObjective, Verified);

    // objectiveSavedMark: whether the objective is one of the level's goals
    // in the player's saved game. An objective that is in no goal slot reads
    // the byte before the marks (the level's own bits).
    uint32_t objectiveSavedMark(uint8_t player, Objective objective)
    {
        const int32_t goal = goalIndexOf(player, zge::objectiveNameHash(objective));
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_currentLevel);
        const ee::Ptr<uint8_t> mark(&goals->marks[goal]);
        return *mark & kGoalSaved;
    }
    READABLE_FUNCTION_WAITS(objectiveSavedMark, Verified);

    // resetTurnGoals: a turn starts. The player's counts of goals and gaps
    // done start at zero and every objective is put back by its saved mark
    // (the count goes up again for each one done); the engine resets every
    // objective; the objectives active after that are marked so; and the
    // player has no timed objective.
    void resetTurnGoals(uint8_t player)
    {
        ee::Local<GoalRestore> restore;
        restore->goals = playerLevelGoals(player, *g_currentLevel);
        restore->goals->goalsDone = 0;
        restore->goals->gapsDone = 0;
        restore->goal = 0;
        calls::forEachObjective(sym::restoreGoal, restore);
        zge::resetObjectives();
        restore->goal = 0;
        calls::forEachObjective(sym::markActiveGoal, restore);
        clearPlayerObjective(player);
    }
    READABLE_FUNCTION_WAITS(resetTurnGoals, Verified);

    // clearPlayerObjective: the player's timed objective, if one runs, is
    // timed out; the rider has none and no time left, and the objective
    // display is told to show none.
    void clearPlayerObjective(uint8_t player)
    {
        ee::Local<ObjectiveDisplayMessage> message;
        const ee::Ptr<RiderObjectives> own = playerObjectives(player);
        if (own->timedObjective)
        {
            calls::timeOutObjective(own->timedObjective);
            own->timedObjective = Objective();
        }
        own->ticksLeft = 0;
        own->timedObjective = Objective();
        message->player = player;
        message->goal = kNoGoalShown;
        overlayMessage(kOverlayObjective, message);
    }
    READABLE_FUNCTION_WAITS(clearPlayerObjective, Verified);

    // unusedTurnStartHook: nothing (the game calls it as a turn starts).
    void unusedTurnStartHook()
    {
    }
    READABLE_FUNCTION(unusedTurnStartHook, Verified);

    // playerHasTurnAwards: in the game's first two modes, whether the
    // rider's end-of-turn bits have any of the low 32 set, or one of the two
    // level awards.
    uint32_t playerHasTurnAwards(uint8_t player)
    {
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        if (gameMode() >= 2)
            return 0;
        const uint64_t awards = riderObjectives(rider)->turnAwards;
        if (static_cast<uint32_t>(awards) != 0)
            return 1;
        return (awards & kTurnLevelAwards) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(playerHasTurnAwards, Verified);

    // goalNameHash: the name hash of the level's objective at that place.
    // (The player's goals are fetched and not used.)
    uint32_t goalNameHash(uint32_t player, uint32_t goal)
    {
        playerLevelGoals(player, *g_currentLevel);
        return ee::Ptr<GoalSlot>(sym::g_goalSlots)[goal].nameHash;
    }
    READABLE_FUNCTION_WAITS(goalNameHash, Verified);

    // goalIndexOf: the place of the objective of that name hash among the
    // level's, or kNoGoal. (The player's goals are fetched and not used.)
    int32_t goalIndexOf(uint32_t player, uint32_t nameHash)
    {
        playerLevelGoals(player, *g_currentLevel);
        for (uint32_t goal = 0; goal < kLevelObjectives; ++goal)
        {
            if ((*g_goalSlots)[goal].nameHash == nameHash)
                return static_cast<int32_t>(goal);
        }
        return kNoGoal;
    }
    READABLE_FUNCTION_WAITS(goalIndexOf, Verified);
}
