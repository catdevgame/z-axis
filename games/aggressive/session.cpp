// A level's session: what ends it. shutdownLevelSession undoes what entering
// a level built (play back to the front end); endTimedRun stops a run whose
// clock ran out; loadSkaterDynamics reads the skater's rigid body file.

#include "aggressive/camera.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "zge/scene/animator.h"
#include "zge/scene/carried_objects.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/collision/sweep.h"
#include "zge/scene/scene.h"
#include "zge/assets/file.h"
#include "zge/collision/dynamics.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/follower.h"
#include "zge/render/texture_effects.h"
#include "aggressive/level/level_collision.h"

#include <cstdint>
#include "aggressive/fx/library.h"
#include "aggressive/objects.h"
#include "aggressive/timers.h"
#include "aggressive/card_game.h"
#include "aggressive/game_save.h"
#include "aggressive/script/child_states.h"
#include "aggressive/sound/events.h"
#include "aggressive/zone_memory.h"
#include "aggressive/hud/hud.h"
#include "aggressive/game_start.h"
#include "aggressive/level/double_rails.h"
#include "aggressive/level/load.h"
#include "aggressive/level/park_editor.h"
#include "aggressive/level/props.h"
#include "aggressive/level/zones.h"
#include "aggressive/level/animal_rescue.h"
#include "aggressive/level/closers.h"

#include "sym/aggressive/session.cpp.h"

namespace aggressive
{
    namespace
    {

        constexpr uint32_t kMultiplayerUIName = sym::multiplayerUIName, kInGameUIName = sym::inGameUIName;
        constexpr uint32_t kSkaterDynamicsFile = sym::skaterDynamicsFile;  // "assets\dynamics\edwards.zad"

        inline ee::Global<int32_t, sym::g_skaterDynamicsSize> g_skaterDynamicsSize;

        // The best-run rows a timed run's end records into.
        constexpr uint32_t kMoneyStat = 12, kSplitStat = 13;
        // Level::timeUpKind values that keep the stop reason already set.
        constexpr uint8_t kKeepsReasonA = 3, kKeepsReasonB = 5;
    }

    // shutdownLevelSession: the level left: the players' careers saved, the UI, the
    // animators, the scripts, the sounds, the effects, the players and what
    // they hold released, the scene left, the session's memory freed, in the
    // original's order. Returns the new top state.
    int32_t shutdownLevelSession()
    {
        const ee::Ptr<Level> session = currentLevel();

        script::scriptStopObjects();
        if (g_gameState->type == GameType::Career)
        {
            for (int32_t index = 0; index < playerCount(); ++index)
                saveCareerProgress(levelPlayer(session, index));
        }
        aggressive::hud::freeHudMessages();
        const uint32_t uiName = isSplitScreen() ? kMultiplayerUIName : kInGameUIName;
        zgui::destroyNode(zgui::findNode(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<const char>(uiName), 1));

        level::unhookAnimationEvents();
        zge::shutdownAnimators();
        level::shutdownParkEditor();
        aggressive::script::unloadLevelScript();
        aggressive::level::freeCloserQueries();
        stopSoundEmitters();
        fx::shutdownInlineFx();
        zge::freeTextureEffects();
        zge::resetDynamicsDefinitions();

        for (int32_t index = 0; index < playerCount(); ++index)
            skater::removePlayer(levelPlayer(session, index));

        level::freeLevelZones();
        skater::freeSkaterClasses();
        freeKeyedValues();
        level::freeLevelRails();
        setFollowCameraTarget(&g_gameState->cameras[0], ee::Ptr<skater::Player>(0u));
        if (isSplitScreen())
            setFollowCameraTarget(&g_gameState->cameras[1], ee::Ptr<skater::Player>(0u));
        zge::removeLevelObjects();
        freeLevelContacts();
        zge::clearPathRecords();
        zge::freeCarriedAnimals();
        shutdownTimers();
        freeZoneMemory();
        zge::leaveScene();
        releaseFollowCameras();
        zge::heapFree(session);
        g_gameState->topStateId = kTopStateNone;
        g_gameState->topState->level = ee::Ptr<Level>(0u);
        return kTopStateNone;
    }
    READABLE_FUNCTION_WAITS(shutdownLevelSession, Verified);

    // loadSkaterDynamics: the skater's rigid body file copied to the heap and its
    // size handed out; 0 if it could not be read.
    uint32_t loadSkaterDynamics(ee::Ptr<uint32_t> bytesOut)
    {
        const uint32_t file = zge::loadFile(kSkaterDynamicsFile, ee::Ptr<int32_t>(g_skaterDynamicsSize.get()));
        uint32_t copy = 0;
        if (file)
        {
            copy = zge::heapAllocate(*g_skaterDynamicsSize);
            zge::memoryCopy(copy, file, *g_skaterDynamicsSize);
            zge::freeLoadedFile(file);
        }
        if (bytesOut)
            *bytesOut = *g_skaterDynamicsSize;
        return copy;
    }
    READABLE_FUNCTION_WAITS(loadSkaterDynamics, Verified);

    // endTimedRun: the run's timer ran out: the players stop and go back to
    // their start, the round's own end runs, and play stops. A split-screen
    // game decides the first; a timed run and a free skate record the score;
    // anything else asks to retry unless the level's timeUpKind keeps the
    // reason it has.
    void endTimedRun()
    {
        const ee::Ptr<Level> level = currentLevel();
        const auto stopPlayer = [&](int32_t index) {
            const ee::Ptr<skater::Player> player = levelPlayer(level, index);
            player->flags |= skater::kRespawnAtLocator;
            skater::respawn(player, 1, 1);
        };
        const auto stopPlay = [&](uint16_t reason) {
            setLevelStopReason(level, reason);
            enterGameMode(kModePaused);
        };

        const GameType type = g_gameState->type;
        if (type == GameType::SplitScreen)
        {
            stopPlayer(0);
            stopPlayer(1);
            if (level->splitGame == kSplitGameCards)
            {
                endCardGame();
                stopPlay(kStopRunOver);
            }
            else if (level->splitGame == kSplitGameAnimalRescue)
            {
                stopPlay(kStopRunOver);
                dropAnimals(levelPlayer(level, 0), 1);
                dropAnimals(levelPlayer(level, 1), 1);
                level::setAnimalRescue(0);
            }
            else
                stopPlay(kStopRunOver);
        }
        else if (type == GameType::FreeSkate || type == GameType::TimedRun)
        {
            stopPlayer(0);
            const ee::Ptr<skater::Player> player = levelPlayer(level, 0);
            const char *name = skater::skaterProfile(player)->name;
            recordBestScore(kMoneyStat, g_gameState->level, skater::skaterScore(player), name);
            recordBestScore(kSplitStat, g_gameState->level, skater::splitScore(player), name);
            stopPlay(kStopRunResults);
        }
        else
        {
            const uint8_t kind = level->timeUpKind;
            if (kind < 6 && kind != kKeepsReasonA && kind != kKeepsReasonB)
                setLevelStopReason(level, kStopRetry);
            enterGameMode(kModePaused);
        }
        releaseTimer(level->timer);
        level->timer = 0;
    }
    READABLE_FUNCTION_WAITS(endTimedRun, Verified);
}
