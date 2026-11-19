// The level state's enter and exit (action.h): a level set up for play, a
// demo or a replay, and taken down again. DM2's own code (action.c).

#include "action.h"
#include "action_state.h"
#include "../camera/camera.h"
#include "../editor/editor_state.h"
#include "../fx/library.h"
#include "../game/npcs.h"
#include "../game/players.h"
#include "../game/round.h"
#include "../level/level_collision.h"
#include "../menu/memory_card_menus.h"
#include "../menu/menu_mode.h"
#include "../overlay/overlay.h"
#include "../replay/replays.h"
#include "../rider/rider.h"
#include "../sound/sound_mode.h"
#include "dm2/level/object_sounds.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/collision/dynamics.h"
#include "zge/game.h"
#include "zge/input/pads.h"
#include "zge/platform/tasks.h"
#include "zge/render/display.h"
#include "zge/scene/follower.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/sound/sound.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/action/level_state.cpp.h"

namespace zge
{
    ChunkProgressHook chunkProgressHook();                                                          // assets/chunk_reader.cpp
    uint32_t findZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, uint32_t group, uint32_t asset); // dm2/zfile_library.cpp
    void restoreLevelObjects(uint32_t save);                                                        // scene/objects.cpp
}

namespace game::dm2
{
    namespace rider
    {
        void riderSetup();                 // rider_state.cpp
        PadInput *padInput(uint32_t port); // input.cpp
        namespace collision
        {
            uint32_t linkLevelFaces();   // tags.cpp
            uint32_t unlinkLevelFaces(); // tags.cpp
        }
    }

    namespace
    {
        // The demo (a level started with no players): its level, which of
        // the level's two replays in the game's library it plays, and how
        // many demo replays have been read.
        inline ee::Global<uint8_t, sym::g_demoLevel> g_demoLevel;
        inline ee::Global<uint8_t, sym::g_demoReplayChoice> g_demoReplayChoice;
        inline ee::Global<uint8_t, sym::g_demoReplaysRead> g_demoReplaysRead;
        constexpr uint32_t kReplaysPerLevel = 2;
        constexpr ee::Ptr<const char> kReplayLibrary{sym::replayLibraryPath}; // "assets\replay.zal"
        // The bytes of recording a watched replay may take.
        constexpr uint32_t kWatchedReplayBytes = 0x3800;

        constexpr ee::Ptr<const char> kLevelNameFormat{sym::levelNameFormat}; // "Level%02d"
        constexpr uint32_t kLevelNameBytes = 0x20;

        // Whether the level's objects have state to save, and where the
        // level's set-up saves it.
        inline ee::Global<uint32_t, sym::g_levelObjectsSaved> g_levelObjectsSaved;
        constexpr uint32_t kLevelObjectsSave = sym::g_levelObjectsSave;

        // The action's task (made as the level starts, deleted as it ends):
        // its frame does nothing; its source file and the line it is made at.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_actionTask> g_actionTask;
        constexpr uint32_t kActionTaskFrame = sym::actionTaskFrame;
        constexpr uint32_t kActionSourcePath = sym::actionSourcePath; // "./src/com/action.c"
        constexpr uint32_t kLevelEnterLine = 412;

        // The last step of the loading screen's progress.
        constexpr ee::Ptr<const char> kLoadingFinal{sym::loadingFinalName}; // "FINAL"

        // The screen the game draws to.
        constexpr uint32_t kScreenWidth = 512, kScreenHeight = 448;


        // Where the two blocks are in a character's record: the one after
        // its tricks (players.h has no fields there yet), and its bike model
        // and bike (two bytes).
        constexpr uint32_t kCharacterBlockOffset = 0xBE;
        constexpr uint32_t kCharacterPairOffset = offsetof(RiderCharacter, bikeModel);
        constexpr uint32_t kCharacterPairBytes = 2;
        constexpr uint8_t kNoRider = 0xFF;
    }

    namespace
    {
        // The small models' draw distances by size (four classes): a model
        // smaller than `size` every way has its parts' last level of detail
        // drawn no further than `distance`.
        struct SmallModelDistance
        {
            ee::Float size;
            ee::Float distance;
        };
        constexpr uint32_t kSmallModelClasses = 4;
        inline ee::Global<SmallModelDistance[kSmallModelClasses], sym::g_smallModelDistances> g_smallModelDistances;
        constexpr float kSmallModelSize = 20.0f;

    }

    // limitSmallModelDistance (forEachWorldObject's visit): a model smaller
    // than 20 every way has each part's last level of detail drawn no
    // further than its size class's distance. Always 1 (the walk goes on).
    uint32_t limitSmallModelDistance(ee::Ptr<zge::GeoObject> model)
    {
        ee::Local<zge::Vec4> extent;
        const ee::Ptr<zge::Float4> bounds = zge::geoObjectBounds(model);
        *extent = zge::quadwordOf(bounds + 1) - zge::quadwordOf(bounds);
        const ee::Float largest = ee::max(ee::max(extent->y, extent->x), extent->z);
        if (!(largest < ee::Float(kSmallModelSize)))
            return 1;
        const int32_t parts = zge::modelPartCount(model);
        if (parts == 0)
            return 1;
        for (int32_t index = parts - 1; index != -1; --index)
        {
            const ee::Ptr<zge::CollisionFrame> part = zge::classModel(model, static_cast<uint32_t>(index));
            const ee::Ptr<zge::FramePart> last = zge::frameLevelPart(part, zge::frameLevels(part) - 1);
            for (uint32_t size = 0; size < kSmallModelClasses; ++size)
            {
                const SmallModelDistance &sizeClass = (*g_smallModelDistances)[size];
                if (largest < sizeClass.size)
                {
                    if (!(last->distance < sizeClass.distance))
                        last->distance = sizeClass.distance;
                    break;
                }
            }
        }
        return 1;
    }
    READABLE_FUNCTION(limitSmallModelDistance, Verified);

    // limitSmallModelDistances: limitSmallModelDistance over the world's models.
    uint32_t limitSmallModelDistances()
    {
        return game::tail(zge::forEachWorldObject, sym::limitSmallModelDistance, ee::Address());
    }
    READABLE_FUNCTION_WAITS(limitSmallModelDistances, Verified);

    uint32_t levelStateEnter()
    {
        ee::Local<char[kLevelNameBytes]> levelName;
        ee::Local<NewRider> request;
        ee::Local<ReplaySetup> replaySetup;
        ee::Local<uint32_t> savedPart;
        ee::Local<uint32_t> savedBytes;

        clearReplays();
        if (*g_multiplayerPlayers == 0)
            *g_sessionLevel = *g_demoLevel;
        // The demo and a viewed replay bring their own level: the replay is
        // made and filled first, and says which level and game it was.
        if (*g_multiplayerPlayers == 0 || menuMode() == kMenuViewReplay)
        {
            uint8_t *const watched = menuMode() == kMenuViewReplay ? g_viewedReplay.get() : g_demoReplay.get();
            replaySetup->dataBytes = kWatchedReplayBytes;
            replaySetup->flags = *g_multiplayerPlayers == 0 ? kReplayPlaysOnly : kReplayMultiplayerSetUp;
            replaySetup->rider = 0;
            const uint8_t replay = newReplay(replaySetup);
            *watched = replay;
            replaySavedPart(replay, savedPart, savedBytes);
            if (menuMode() == kMenuViewReplay)
                zge::memoryCopy(*savedPart, loadedReplay(), *savedBytes);
            else
            {
                const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kReplayLibrary);
                if (zge::findZfileLibraryAsset(library, *g_sessionLevel * kReplaysPerLevel + *g_demoReplayChoice, 0))
                {
                    readLibraryReplay(library, *savedPart);
                    ++*g_demoReplaysRead;
                }
                zge::closeZfileLibrary(library);
            }
            *g_sessionLevel = replayLevel(*watched);
            *g_sessionSinglePlayerMode = replaySinglePlayerMode(*watched);
        }

        setGameMode(kSoundModeLevel);
        if (*g_sessionLevel == kParkLevel)
        {
            if (*g_sessionSinglePlayerMode != kParkTest)
                enterParkLevel();
        }
        else
        {
            zge::formatString(levelName, kLevelNameFormat, static_cast<uint32_t>(*g_sessionLevel));
            if (!zge::enterScene(levelName.address()))
                return 0;
        }
        if (rider::collision::linkLevelFaces() != 1)
            return 0;
        if (zge::loadLevelPaths() != 1)
            return 0;
        if (setupLevelContacts() != 1)
            return 0;
        if (zge::spawnWorldMeshObjects() != 1)
            return 0;
        if (zge::levelObjectsSaveSize() != 0)
        {
            zge::saveLevelObjects(kLevelObjectsSave);
            *g_levelObjectsSaved = 1;
        }
        else
            *g_levelObjectsSaved = 0;

        probeSpawnGround();
        rider::riderSetup();
        *g_replayViewRider = kNoRider;
        *g_singlePlayerRider = kNoRider;
        zge::memoryFill(request, 0, sizeof(NewRider));
        if (*g_multiplayerPlayers != 0 && menuMode() != kMenuViewReplay)
        {
            // Each player's rider, from the character the player picked.
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                const ee::Ptr<RiderCharacter> character = playerCharacter(player);
                request->player = static_cast<uint8_t>(player);
                zge::memoryCopy(&request->characterBlock, ee::Address(character) + kCharacterBlockOffset, sizeof(request->characterBlock));
                zge::memoryCopy(&request->bikeModel, ee::Address(character) + kCharacterPairOffset, kCharacterPairBytes);
                (*g_playerRiders)[player] = rider::newRider(request);
            }
        }
        else
        {
            // The watched replay's rider, as its recording started.
            uint8_t *const watched = menuMode() == kMenuViewReplay ? g_viewedReplay.get() : g_demoReplay.get();
            zge::memoryFill(request, 0, sizeof(NewRider));
            request->flags |= kRiderFromHead;
            zge::memoryCopy(ee::Address(request.address() + offsetof(NewRider, head)), replayRiderHead(*watched),
                            sizeof(request->head));
            if (menuMode() == kMenuViewReplay)
            {
                const uint8_t made = rider::newRider(request);
                (*g_playerRiders)[0] = made;
                *g_replayViewRider = made;
            }
            else
                *g_singlePlayerRider = rider::newRider(request);
        }

        setupNpcs();
        initOverlay();
        initCamera();
        showOverlayFrame(kOverlayFade, 1);
        *g_actionMode = kActionPlay;
        *g_actionRequest = kNoActionRequest;
        *g_actionStep = 0;
        *g_actionFlags = 0;
        *g_currentPlayer = 0;
        gameSetup();
        *g_actionTask = zge::newTask(1, kActionTaskFrame, 0, 1, kActionSourcePath, kLevelEnterLine);

        if (*g_multiplayerPlayers == 0)
            actionStart(kActionDemo);
        else if (*g_multiplayerPlayers == 1)
        {
            if (menuMode() == kMenuViewReplay)
            {
                setSinglePlayerMode(*g_sessionSinglePlayerMode);
                actionStart(kActionViewReplay);
            }
            else
            {
                setSinglePlayerMode(*g_sessionSinglePlayerMode);
                startGame(0);
            }
        }
        else
        {
            showGameScreen(kScreenMultiplayerGameSelect);
            setCameraPlayer(0);
            setCameraMode(0, 0, kCameraLevelPoint);
        }

        if (singlePlayerMode() != kParkTest)
            initInlineFx();
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(0)->pad));
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(1)->pad));
        zge::chunkProgressHook()(kLoadingFinal, 1, 1);
        if (*g_sessionLevel != kParkLevel)
            limitSmallModelDistances();
        zge::resetCameraViewports(kScreenWidth, kScreenHeight);
        return 1;
    }
    READABLE_FUNCTION(levelStateEnter, Verified);

    int32_t levelStateExit()
    {
        zge::releaseAllVoices();
        shutdownCamera();
        zge::deleteTask(*g_actionTask);
        gameCleanup();
        // A viewed replay's rider is not the players' to free.
        if (*g_actionMode != kActionViewReplay)
        {
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
            {
                rider::freeRider((*g_playerRiders)[player]);
                (*g_playerRiders)[player] = kNoRider;
            }
        }
        freeNpcs();
        freeReplays();
        shutdownOverlay();
        if (singlePlayerMode() != kParkTest)
            shutdownInlineFx();
        rider::physicsShutdown();
        *g_levelObjectsSaved = 0;
        freeObjectSounds();
        zge::removeLevelObjects();
        freeLevelContacts();
        zge::resetDynamicsDefinitions();
        zge::clearPathRecords();
        rider::collision::unlinkLevelFaces();
        if (singlePlayerMode() != kParkTest)
            zge::leaveScene();
        return game::tail(setGameMode, kSoundModeOff);
    }
    READABLE_FUNCTION(levelStateExit, Verified);

    // The action task's frame (levelStateEnter makes the task): nothing to
    // do. Always 1.
    uint32_t actionTaskFrame()
    {
        return 1;
    }
    READABLE_FUNCTION(actionTaskFrame, Verified);

    // restoreSavedLevelObjects: the level's objects put back as the level's
    // enter saved them (nothing when it saved none): a player's spawn calls
    // it.
    void restoreSavedLevelObjects()
    {
        if (*g_levelObjectsSaved)
            game::tail(zge::restoreLevelObjects, kLevelObjectsSave);
    }
    READABLE_FUNCTION(restoreSavedLevelObjects, Verified);
}
