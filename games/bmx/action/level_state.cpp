// BMX XXX's level game state (level_state.h): BMX XXX's own code (action.c),
// DM2's line (games/dm2/action/level_state.cpp).

#include "level_state.h"

#include "camera_state.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/round.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/states/front_end_menus.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/ui_calls.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/ui/zgui_globals.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/text/cstring.h"
#include "bmx/fx/library.h"
#include "bmx/game/game_setup.h"
#include "bmx/game/level_globals.h"
#include "bmx/game/npcs.h"
#include "bmx/game/player_riders.h"
#include "bmx/level/entities.h"
#include "bmx/level/level_collision.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/overlay/screens.h"
#include "bmx/profile/players.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/weapon/player_weapon.h"
#include "bmx/weapon/weapon.h"
#include "zge/collision/dynamics.h"
#include "zge/memory/regions.h"
#include "zge/game.h"
#include "zge/platform/tasks.h"
#include "zge/render/display.h"
#include "zge/render/texture.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/fast_paths.h"
#include "zge/scene/cut_scenes.h"
#include "zge/scene/follower.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/sound/sound.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/bmx/action/level_state.cpp.h"

namespace game::bmx
{
    // actionTaskFrame (DM2's name): the action task's frame does nothing.
    uint32_t actionTaskFrame()
    {
        return 1;
    }
    READABLE_FUNCTION(actionTaskFrame, Verified);

    uint32_t checkRiderModelReady(uint32_t riderIndex, ee::Ptr<uint32_t> allReady)
    {
        if (!rider::riderModelReady(&*rider::riderOf(riderIndex & 0xFF)))
            *allReady = 0;
        return 1;
    }
    READABLE_FUNCTION(checkRiderModelReady, Verified);

    namespace
    {
        // The action task (the level's loading makes it), the camera state
        // set last, and the level's animation archives ("anims.zal",
        // "cutscene.zal" in its folder), all the level's loading's.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_actionTask> g_actionTask;
        inline ee::Global<uint32_t, sym::g_cameraState> g_cameraState;
        inline ee::Global<int32_t, sym::g_levelAnimations> g_levelAnimations;
        inline ee::Global<int32_t, sym::g_levelCutSceneAnimations> g_levelCutSceneAnimations;
        inline ee::Global<uint32_t, sym::g_levelObjectsSaved> g_levelObjectsSaved;
        // Each player's rider, weapon and camera, and the ones every player
        // shares in a session with one rider (player_riders.cpp's).
        inline ee::Global<uint8_t[2], sym::g_playerRiders> g_playerRiders;
        inline ee::Global<uint8_t, sym::g_singlePlayerRider> g_singlePlayerRider;
        inline ee::Global<uint32_t[2], sym::g_playerWeapons> g_playerWeapons;
        inline ee::Global<uint32_t, sym::g_singlePlayerWeapon> g_singlePlayerWeapon;
        inline ee::Global<ee::Ptr<ActionCamera>[2], sym::g_playerCameras> g_playerCameras;
        constexpr uint8_t kNoRider = 0xFF;
        // The park editor's level (no level of its own to leave), and the
        // levels' table (which keep their textures in multiplayer).
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        constexpr uint8_t kParkLevel = 12;
        constexpr uint8_t kParkTestSession = 4; // g_sessionKind (inferred: DM2 keeps its scene for its park test here)
        struct LevelRow
        {
            uint8_t _00[0x0C];
            uint16_t flags; // +0x0C kLevelFullTextures
            uint8_t _0e[2];
        };
        static_assert(offsetof(LevelRow, flags) == 0x0C && sizeof(LevelRow) == 0x10);
        inline ee::Global<LevelRow[kParkLevel], sym::g_levels> g_levels;
        constexpr uint16_t kLevelFullTextures = 0x2; // the level keeps its textures in multiplayer
        // The GS mode sceGsResetGraph was given (3: the one that keeps only
        // two mip levels): the texture settings put back as the renderer
        // starts them (zge/render/texture_levels.cpp).
        inline ee::Global<uint32_t, sym::g_gsVideoMode> g_gsVideoMode;
        constexpr uint32_t kGsModeFewLevels = 3;
        constexpr uint32_t kLastTextureLevel = 3, kLastTextureLevelFewLevels = 1;
        constexpr uint32_t kSoundModeOff = 0; // setGameMode's

        // The script natives' end, with no body in BMX XXX's instance.
        // Scaffolding (address-call): until it has one.
        namespace calls
        {
            void scriptFreeNatives() { ee::game(sym::scriptFreeNatives); }
        }
    }

    // emptyQuitHook: the level's exit's hook for the game, empty. Always 1.
    int32_t emptyQuitHook()
    {
        return 1;
    }
    READABLE_FUNCTION(emptyQuitHook, Verified);

    // levelStateExit (DM2's name): the level state's exit. The voices
    // released, the action task deleted, the game's records put back; each
    // player's weapon and rider freed (the shared ones in a one-rider
    // session) and their cameras; the weapons, the NPCs, the riders'
    // physics, the replays, the overlay, the cameras, the level's objects,
    // animation archives, contacts, effects, paths and scripts; the level's
    // scene left (but in the park editor's test ride); a multiplayer
    // level's lowered texture settings put back, and the sound's mode off.
    int32_t levelStateExit()
    {
        zge::releaseAllVoices();
        zge::deleteTask(*g_actionTask);
        gameCleanup();
        if (!sessionHasOneRider())
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                freePlayerWeapon((*g_playerWeapons)[player]);
                rider::freeRider((*g_playerRiders)[player]);
                (*g_playerRiders)[player] = kNoRider;
            }
            for (uint32_t player = 0; player < *g_playerCount; ++player)
                freeActionCamera(&*(*g_playerCameras)[player]);
        }
        else
        {
            freePlayerWeapon(*g_singlePlayerWeapon);
            rider::freeRider(*g_singlePlayerRider);
            *g_singlePlayerRider = kNoRider;
            freeActionCamera(&*(*g_playerCameras)[0]);
        }
        closeWeapons();
        closePlayerWeapons();
        freeNpcs();
        rider::physicsShutdown();
        freeReplays();
        playOverlayFade(kOverlayWholeScreen, kOverlayFadeReset);
        closeOverlay();
        freeActionCameras();
        freeEntityModelCache();
        zge::removeLevelObjects();
        unregisterLevelObjectTypes();
        *g_levelObjectsSaved = 0;
        if (*g_levelCutSceneAnimations)
        {
            zge::freeAnimationLibrary(static_cast<uint32_t>(*g_levelCutSceneAnimations));
            *g_levelCutSceneAnimations = 0;
        }
        if (*g_levelAnimations)
        {
            zge::freeAnimationLibrary(static_cast<uint32_t>(*g_levelAnimations));
            *g_levelAnimations = 0;
        }
        freeLevelContacts();
        if (*g_currentLevel != kParkLevel)
            zge::resetDynamicsDefinitions();
        objectives::freeObjectiveIcons();
        shutdownEffects();
        zge::clearPathRecords();
        emptyQuitHook();
        zge::freeFastPaths();
        calls::scriptFreeNatives();
        if (*g_sessionKind != kParkTestSession)
        {
            zge::leaveScene();
            zge::resetFramePackets();
        }
        if (*g_playerCount >= 2 && *g_currentLevel < kParkLevel && ((*g_levels)[*g_currentLevel].flags & kLevelFullTextures) == 0)
        {
            zge::setTextureSetting(zge::kTextureSettingDropLargeLevel, 0);
            zge::setTextureSetting(zge::kTextureSettingLastLevel,
                                   *g_gsVideoMode != kGsModeFewLevels ? kLastTextureLevel : kLastTextureLevelFewLevels);
        }
        const int32_t result = setGameMode(kSoundModeOff);
        *g_cameraState = 0;
        return result;
    }
    READABLE_FUNCTION(levelStateExit, Verified);

    namespace
    {
        // The loading's steps (loadStep), a frame or more each.
        enum LevelLoadStep : uint32_t
        {
            kLoadShowLoading = 0, // the loading frame up (72 frames)
            kLoadScene = 1,       // the level's scene entered
            kLoadFirstText = 2,   // the loading frame's first text (72 frames)
            kLoadLevel = 3,       // the level's edges, light, paths and contacts
            kLoadEffects = 4,
            kLoadArchives = 5,    // the level's animations, cut scenes' motions and models' library
            kLoadRiders = 6,      // the riders, weapons, objective icons and NPCs
            kLoadSecondText = 7,  // the loading frame's second text (72 frames)
            kLoadRiderModels = 8, // until every rider's model is ready
            kLoadObjects = 9,     // the level's objects
            kLoadScripts = 10,    // the cut scenes' scripts
            kHideLoading = 11,    // the loading frame shut down (72 frames), then the cameras
            kLoadOverlay = 12,
            kLoadGame = 13,       // the game set up and started for the session
            kCloseModels = 14,    // the models' library closed
            kLoadDone = 15,       // play begins: 0
        };
        constexpr uint32_t kLoadingFrames = 72;
        constexpr uint32_t kTextFrame = 36;   // the frame of a text step the text is written
        constexpr uint32_t kShowFrame = 1;    // the frame a step plays its frame's part

        // The loading frame and its parts, and what the loading plays on
        // them.
        constexpr ee::Ptr<const char> kLoadingFrame{sym::g_textLevelLoadingFrame};  // "UI_common.FrameLoading"
        constexpr ee::Ptr<const char> kPosition{sym::g_textLevelLoadPosition};      // "Position"
        constexpr ee::Ptr<const char> kInit{sym::g_textLevelLoadInit};              // "Init"
        constexpr ee::Ptr<const char> kSplashPicture{sym::g_textLevelLoadSplashPicture}; // "SplashPicture"
        constexpr ee::Ptr<const char> kSplashCenter{sym::g_textLevelLoadSplashCenter};   // "SplashCenter"
        constexpr ee::Ptr<const char> kSplashInfo{sym::g_textLevelLoadSplashInfo};       // "SplashCenter.Info"
        constexpr ee::Ptr<const char> kMove{sym::g_textLevelLoadMove};              // "Move"
        constexpr ee::Ptr<const char> kText{sym::g_textLevelLoadText};              // "Text"
        constexpr ee::Ptr<const char> kShutdown{sym::g_textLevelLoadShutdown};      // "Shutdown"
        constexpr uint32_t kFrameNode = 1;
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_levelLoadingFrame> g_levelLoadingFrame;
        constexpr zge::ChunkProgressHook kLevelLoadingBar{sym::levelLoadingBar};
        // The loading frame's texts, by the session: the hardcore tour's
        // nine, the park editor's three, the multiplayer one's (one for each
        // text step), the rest's six. A level loaded picks the next pair.
        constexpr uint32_t kLoadingText = sym::g_textLevelLoadingText;               // "$KSTR_LOADING_%02d"
        constexpr uint32_t kTourLoadingText = sym::g_textTourLoadingText;       // "$KSTR_LOADING_HT_%02d"
        constexpr uint32_t kParkLoadingText = sym::g_textParkLoadingText;       // "$KSTR_LOADING_PARK_%02d"
        constexpr uint32_t kMultiplayerLoadingText = sym::g_textMultiplayerLoadingText; // "$KSTR_LOADING_MULTIPLAYER_%02d"
        constexpr uint32_t kLoadingTexts = 6, kTourLoadingTexts = 9, kParkLoadingTexts = 3;
        inline ee::Global<uint8_t, sym::g_levelsLoaded> g_levelsLoaded;
        // The session kinds the texts go by (g_sessionKind; inferred from
        // the texts): 0 the hardcore tour, 3 multiplayer, 4 the park
        // editor's test ride.
        constexpr uint8_t kTourSession = 0, kMultiplayerSession = 3;

        // Where the level's files come from (a byte the game is started
        // with): the art's exports A, B or C on the developers' drive, else
        // the disc's levels folder; the scene's name takes a prefix to match.
        inline ee::Global<uint8_t, sym::g_levelArtSource> g_levelArtSource;
        constexpr uint8_t kArtExportsA = 1, kArtExportsB = 2, kArtExportsC = 3;
        constexpr uint32_t kLocalScene = sym::g_textLocalScene;    // "local.%s"
        constexpr uint32_t kSceneA = sym::g_textSceneA;            // "A.%s"
        constexpr uint32_t kSceneB = sym::g_textSceneB;            // "B.%s"
        constexpr uint32_t kSceneC = sym::g_textSceneC;            // "C.%s"
        constexpr uint32_t kLevelFolder = sym::g_textLevelFolder;   // "assets\\levels\\%s\\"
        constexpr uint32_t kExportsFolderA = sym::g_textExportsFolderA; // "s:\\bmx3\\Art_In_Progress\\Level_Exports\\A\\%s\\"
        constexpr uint32_t kExportsFolderB = sym::g_textExportsFolderB;
        constexpr uint32_t kExportsFolderC = sym::g_textExportsFolderC;
        // The level's folder (its files' prefix) and the levels' table: each
        // level's folder name.
        constexpr uint32_t kLevelFilePrefix = sym::g_levelFilePrefix;
        struct LevelEntry
        {
            uint8_t _00[4];
            ee::Ptr<const char> folder; // +0x04
            uint8_t _08[6];
            uint16_t startKind;         // +0x0E 1: the hardcore tour starts it with startGame(1)
        };
        static_assert(offsetof(LevelEntry, folder) == 0x04 && offsetof(LevelEntry, startKind) == 0x0E && sizeof(LevelEntry) == 0x10);
        inline ee::Global<LevelEntry[kParkLevel + 1], sym::g_levels> g_levelEntries;

        // The level's archives in its folder.
        constexpr uint32_t kAnimationsFile = sym::g_textLevelAnimationsFile;     // "%sanims.zal"
        constexpr uint32_t kCutSceneFile = sym::g_textLevelCutSceneFile;         // "%scutscene.zal"
        constexpr uint32_t kCutSceneAnimationsFile = sym::g_textLevelCutCompFile; // "%sCutComp.zal"
        constexpr uint32_t kModelsFile = sym::g_textLevelModelsFile;             // "%smodels.zal"
        constexpr uint32_t kCutSceneBytes = 0x29810;
        inline ee::Global<uint32_t, sym::g_entityModelLibrary> g_entityModelLibrary;

        // The level's light: its ambient colour (the level's scene point's,
        // else this grey).
        inline ee::Global<uint8_t[4], sym::g_defaultLevelLight> g_defaultLevelLight;
        inline ee::Global<uint32_t, sym::g_levelLight> g_levelLight;
        // The heap's free bytes after each step (nothing reads it).
        inline ee::Global<uint32_t, sym::g_levelLoadFreeBytes> g_levelLoadFreeBytes;
        constexpr uint32_t kChunkTraceOff = 2;

        // The replay a one-rider session's rider is made from (the demo's).
        inline ee::Global<uint8_t, sym::g_stateReplay> g_stateReplay;
        constexpr uint8_t kDemoSession = 5; // the one-rider session whose rider is not the first player's too
        // The game's own: the camera state's step, the play's flags, the
        // replay's pad player, the game's camera.
        inline ee::Global<uint8_t, sym::g_cameraStateStep> g_cameraStateStep;
        inline ee::Global<uint32_t, sym::g_actionFlags> g_actionFlags;
        inline ee::Global<uint8_t, sym::g_replayPadPlayer> g_replayPadPlayer;
        inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
        constexpr uint8_t kNoPadPlayer = 0xFF;

        // How each session starts its game (startGame's mode), or what it
        // shows first.
        constexpr uint32_t kStartTourFirst = 1, kStartTour = 0, kStartSession1 = 2, kStartOther = 3;
        constexpr uint16_t kTourStartsFirst = 1;
        constexpr uint8_t kPlayPhaseLoaded = 1;
        constexpr uint32_t kActionTaskStage = 1;
        constexpr uint32_t kActionSourceFile = sym::g_textActionSourceFile; // "./src/com/action/action.c"
        constexpr uint32_t kActionTaskLine = 760;
        constexpr uint32_t kActionTaskFrame = sym::actionTaskFrame;
        constexpr rider::RiderVisit kCheckRiderModelReady{sym::checkRiderModelReady};

        // The engine's parts of the loading with no body in BMX XXX's
        // instance: the particle system on or off (inferred: the word
        // emitSparks and spawnPolys test); every asset library's open file
        // closed.
        // Scaffolding (address-call): until each has its body.
        namespace calls
        {
            void setParticlesOn(uint32_t on) { ee::game(sym::setParticlesOn, on); }
            void closeLibraryFiles() { ee::game(sym::closeLibraryFiles); }
        }

        // The loading frame's text for this level: the session's set of
        // texts, the pair this level is at, its first or second.
        void showLoadingText(uint32_t second, ee::Local<char[0x40]> &text)
        {
            const uint8_t kind = *g_sessionKind;
            if (kind == kMultiplayerSession)
            {
                zge::formatString(text, kMultiplayerLoadingText, second);
            }
            else
            {
                const int32_t pair = *g_levelsLoaded * 2 + static_cast<int32_t>(second);
                if (kind == kTourSession)
                    zge::formatString(text, kTourLoadingText, pair % static_cast<int32_t>(kTourLoadingTexts));
                else if (kind == kParkTestSession)
                    zge::formatString(text, kParkLoadingText, pair % static_cast<int32_t>(kParkLoadingTexts));
                else
                    zge::formatString(text, kLoadingText, pair % static_cast<int32_t>(kLoadingTexts));
            }
            zge::zgui::setVariable(zge::zgui::findNode(*g_levelLoadingFrame, kSplashInfo, kFrameNode), kText, text);
            playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashCenter, kFrameNode), kInit);
        }

        // The scene of the level entered from where its files come from;
        // 0 when the scene fails.
        uint32_t enterLevelScene(ee::Local<char[0x40]> &scene)
        {
            const ee::Ptr<const char> folder = (*g_levelEntries)[*g_currentLevel].folder;
            switch (*g_levelArtSource)
            {
            case kArtExportsA:
                zge::formatString(scene, kSceneA, folder);
                zge::formatString(kLevelFilePrefix, kExportsFolderA, folder);
                break;
            case kArtExportsB:
                zge::formatString(scene, kSceneB, folder);
                zge::formatString(kLevelFilePrefix, kExportsFolderB, folder);
                break;
            case kArtExportsC:
                zge::formatString(scene, kSceneC, folder);
                zge::formatString(kLevelFilePrefix, kExportsFolderC, folder);
                break;
            default:
                zge::formatString(scene, kLocalScene, folder);
                zge::formatString(kLevelFilePrefix, kLevelFolder, folder);
                break;
            }
            const uint32_t trace = zge::setChunkTrace(kChunkTraceOff);
            if (zge::enterScene(scene.address()) != 1)
                return 0;
            zge::setChunkTrace(trace);
            return 1;
        }

        // The riders: each player's from the character the player picked
        // (with a weapon), or a one-rider session's from the demo's replay.
        void makeRiders(ee::Local<NewRider> &request, ee::Local<PlayerWeaponOwner> &owner)
        {
            zge::memoryFill(request, 0, sizeof(NewRider));
            *g_singlePlayerRider = kNoRider;
            if (!sessionHasOneRider())
            {
                for (uint32_t player = 0; player < *g_playerCount; ++player)
                {
                    const ee::Ptr<RiderCharacter> character = playerCharacter(player);
                    request->player = static_cast<uint8_t>(player);
                    zge::memoryCopy(request->savedModel, &character->savedModel, sizeof(request->savedModel));
                    zge::memoryCopy(request->characterBike, &character->bike, sizeof(request->characterBike));
                    request->model = playerCharacter(player)->model;
                    (*g_playerRiders)[player] = rider::newRider(request);
                    *g_levelLoadFreeBytes = zge::freeHeapBytes();
                    owner->player = static_cast<uint8_t>(player);
                    owner->rider = (*g_playerRiders)[player];
                    (*g_playerWeapons)[player] = newPlayerWeapon(owner);
                    *g_levelLoadFreeBytes = zge::freeHeapBytes();
                }
                return;
            }
            zge::memoryFill(request, 0, sizeof(NewRider));
            request->flags |= kRiderFromReplay;
            zge::memoryCopy(request->riderModel, replayRiderHead(*g_stateReplay), kReplayRiderHeadBytes);
            request->model = playerCharacter(0)->model;
            const uint8_t made = rider::newRider(request);
            owner->player = 0;
            owner->rider = made;
            *g_singlePlayerRider = made;
            *g_singlePlayerWeapon = newPlayerWeapon(owner);
            if (*g_sessionKind != kDemoSession)
                (*g_playerRiders)[0] = *g_singlePlayerRider;
        }

        // The players' cameras: each player's on its rider, with the
        // player's camera type; a one-rider session's one letterboxed.
        void makeCameras(ee::Local<CameraSetup> &setup)
        {
            setup->kind = 1;
            setup->viewportLayout = static_cast<uint32_t>(ViewportLayout::Full);
            if (!sessionHasOneRider())
            {
                for (uint32_t player = 0; player < *g_playerCount; ++player)
                {
                    setup->rider = (*g_playerRiders)[player];
                    (*g_playerCameras)[player] = newActionCamera(setup);
                    setCameraType(&*(*g_playerCameras)[player], playerSetup(player)->cameraType);
                }
                return;
            }
            setup->viewportLayout = static_cast<uint32_t>(ViewportLayout::Letterbox);
            setup->rider = *g_singlePlayerRider;
            (*g_playerCameras)[0] = newActionCamera(setup);
        }

        // The session's game started, or what it shows first.
        void startSession()
        {
            switch (*g_sessionKind)
            {
            case kTourSession:
                startGame((*g_levelEntries)[*g_currentLevel].startKind == kTourStartsFirst ? kStartTourFirst : kStartTour);
                break;
            case 1:
                startGame(kStartSession1);
                break;
            case kMultiplayerSession:
                showGameScreen(kScreenAfterGame);
                showCamera(&**g_gameCamera, 1);
                break;
            case kDemoSession:
                setCameraState(kCameraStateReplayDemo);
                break;
            case 6:
                setCameraState(kCameraState5);
                break;
            default:
                startGame(kStartOther);
                break;
            }
        }
    }

    namespace
    {
        // The screen the cameras' viewports are laid out over.
        constexpr uint32_t kScreenWidth = 512, kScreenHeight = 448;
        // A multiplayer level that does not keep its textures loads them
        // lowered (levelStateExit puts them back).
        constexpr uint32_t kLoweredTextureLevel = 2;
        // The two one-rider sessions that watch a replay: the front end's
        // demo (the game's library's) and a replay viewed (the one loaded:
        // DM2's view replay, made with the same flag).
        constexpr uint8_t kViewReplaySession = 6;
        constexpr uint32_t kWatchedReplayBytes = 0x3800;
        // The demos: a pair (character, level) each; which one plays (it
        // goes back to the first once one is read). The library's replay
        // of one is "demo.Lev<level>.<character's model>".
        struct DemoReplay
        {
            uint8_t character;
            uint8_t level;
        };
        inline ee::Global<DemoReplay[1], sym::g_demoReplays> g_demoReplays;
        inline ee::Global<uint8_t, sym::g_demoReplayChoice> g_demoReplayChoice;
        constexpr ee::Ptr<const char> kReplayLibrary{sym::g_textReplayLibrary}; // "assets\replay.zal"
        constexpr uint32_t kDemoReplayName = sym::g_textDemoReplayName;          // "demo.Lev%02d.%s"
        struct CharacterModelRow
        {
            uint8_t _00[0x10];
            ee::Ptr<const char> modelName; // +0x10 its model script's NAME
        };
        static_assert(sizeof(CharacterModelRow) == 0x14);
        inline ee::Global<CharacterModelRow[1], sym::g_characterNames> g_characterRows;
    }

    // levelStateEnter (DM2's name): the level state's enter. The cameras'
    // viewports reset; a multiplayer level that does not keep its textures
    // has them lowered; the replays emptied; a session that watches a
    // replay makes it and fills it (the demo from the game's library, a
    // viewed one from the loaded replay) and takes its level; the sound's
    // mode set for the level and the loading's first step. Always 1.
    int32_t levelStateEnter()
    {
        ee::Local<char[0x40]> demoName;
        ee::Local<ReplaySetup> replaySetup;
        ee::Local<uint32_t> savedPart;
        ee::Local<uint32_t> savedBytes;
        zge::resetCameraViewports(kScreenWidth, kScreenHeight);
        if (*g_playerCount >= 2 && *g_currentLevel < kParkLevel && ((*g_levels)[*g_currentLevel].flags & kLevelFullTextures) == 0)
        {
            zge::setTextureSetting(zge::kTextureSettingDropLargeLevel, 1);
            zge::setTextureSetting(zge::kTextureSettingLastLevel,
                                   *g_gsVideoMode != kGsModeFewLevels ? kLoweredTextureLevel : kLastTextureLevelFewLevels);
        }
        clearReplays();
        if (sessionHasOneRider())
        {
            if (*g_sessionKind == kDemoSession)
            {
                replaySetup->flags = kReplayPlaysOnly;
                *g_currentLevel = (*g_demoReplays)[*g_demoReplayChoice].level;
            }
            else
            {
                replaySetup->flags = kReplayMultiplayerSetUp;
            }
            replaySetup->dataBytes = kWatchedReplayBytes;
            replaySetup->format = kInputReplay;
            replaySetup->rider = 0;
            replaySetup->weapon = 0;
            *g_stateReplay = newReplay(replaySetup);
            replaySavedPart(*g_stateReplay, savedPart, savedBytes);
            if (*g_sessionKind == kDemoSession)
            {
                const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kReplayLibrary);
                const DemoReplay &demo = (*g_demoReplays)[*g_demoReplayChoice];
                zge::formatString(demoName, kDemoReplayName, *g_currentLevel, (*g_characterRows)[demo.character].modelName);
                if (zge::findAsset(ee::Address(library), zge::nameHash(demoName)))
                {
                    readLibraryReplay(library, *savedPart);
                    *g_demoReplayChoice = 0;
                }
                zge::closeZfileLibrary(library);
            }
            else
            {
                zge::memoryCopy(*savedPart, loadedReplay(), *savedBytes);
            }
            *g_currentLevel = replayLevel(*g_stateReplay);
        }
        setGameMode(kPlayMode);
        setLoadStep(0);
        return 1;
    }
    READABLE_FUNCTION(levelStateEnter, Verified);

    // levelStateLoad: the level's loading, a step a frame (LevelLoadStep);
    // the loading frame shows over the first steps with two texts. 0 once
    // play begins, or as soon as the scene, its edges, paths, contacts or
    // objects fail; else 1.
    int32_t levelStateLoad()
    {
        ee::Local<char[0x40]> text;
        ee::Local<uint8_t[4]> colour;
        ee::Local<NewRider> request;
        ee::Local<PlayerWeaponOwner> owner;
        ee::Local<CameraSetup> camera;
        ee::Local<zge::Vec4> position;
        ee::Local<uint32_t> allReady;

        const uint32_t step = loadStep();
        switch (step)
        {
        case kLoadShowLoading:
            zge::setChunkProgressHook(kLevelLoadingBar, 0);
            if (*g_loadStepFrames == kShowFrame)
            {
                *g_levelLoadingFrame = zge::zgui::findNode(g_bmxZgui->root, kLoadingFrame, kFrameNode);
                *position = zge::kZero;
                zge::zgui::setVariable(*g_levelLoadingFrame, kPosition, position);
                playUiNode(*g_levelLoadingFrame, kInit);
                playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashPicture, kFrameNode), kInit);
            }
            else if (*g_loadStepFrames == kLoadingFrames)
            {
                setLoadStep(loadStep() + 1);
            }
            break;
        case kLoadScene:
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            if (*g_currentLevel != kParkLevel && enterLevelScene(text) == 0)
                return 0;
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            calls::setParticlesOn(0);
            setLoadStep(loadStep() + 1);
            break;
        case kLoadFirstText:
        case kLoadSecondText:
        {
            const uint32_t second = step == kLoadSecondText ? 1 : 0;
            if (*g_loadStepFrames == kTextFrame)
                showLoadingText(second, text);
            else if (*g_loadStepFrames == kLoadingFrames)
                setLoadStep(loadStep() + 1);
            else if (*g_loadStepFrames == kShowFrame)
            {
                if (second == 0)
                    playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashPicture, kFrameNode), kMove);
                else
                    playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashCenter, kFrameNode), kShutdown);
            }
            break;
        }
        case kLoadLevel:
        {
            const int32_t scanned = rider::collision::scanNodesEdgeTags();
            if (scanned != 1)
                return 0;
            for (uint32_t i = 0; i < 4; ++i)
                colour[i] = (*g_defaultLevelLight)[i];
            levelAmbientColour(colour);
            *g_levelLight = zge::newSceneLight(0, 1, colour.address());
            const uint32_t paths = zge::loadLevelPaths();
            if (paths != static_cast<uint32_t>(scanned))
                return 0;
            if (setupLevelContacts() != paths)
                return 0;
            setLoadStep(loadStep() + 1);
            break;
        }
        case kLoadEffects:
            initEffects();
            setLoadStep(loadStep() + 1);
            break;
        case kLoadArchives:
        {
            zge::formatString(text, kAnimationsFile, kLevelFilePrefix);
            *g_levelAnimations = zge::loadAnimationArchive(text);
            zge::formatString(text, kCutSceneFile, kLevelFilePrefix);
            zge::setCutSceneLibrary(text, kCutSceneBytes);
            const uint32_t form = zge::rotationKeyForm();
            zge::setRotationKeyForm1();
            zge::formatString(text, kCutSceneAnimationsFile, kLevelFilePrefix);
            *g_levelCutSceneAnimations = zge::loadAnimationArchive(text);
            zge::setRotationKeyForm(form);
            zge::formatString(text, kModelsFile, kLevelFilePrefix);
            *g_entityModelLibrary = zge::openAssetLibrary(text);
            setLoadStep(loadStep() + 1);
            break;
        }
        case kLoadRiders:
            rider::riderSetup();
            setUpPlayerWeapons();
            setUpWeapons();
            objectives::loadObjectiveIcons();
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            makeRiders(request, owner);
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setupNpcs();
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setLoadStep(loadStep() + 1);
            break;
        case kLoadRiderModels:
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            *allReady = 1;
            rider::forEachRider(kCheckRiderModelReady, allReady);
            if (*allReady == 0)
                break;
            calls::closeLibraryFiles();
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setLoadStep(loadStep() + 1);
            break;
        case kLoadObjects:
        {
            registerLevelObjectTypes();
            calls::setParticlesOn(1);
            const uint32_t spawned = zge::spawnLevelObjects();
            if (spawned != 1)
                return 0;
            if (zge::levelObjectsSaveSize() != 0)
            {
                zge::saveLevelObjects(sym::g_levelObjectsSave);
                *g_levelObjectsSaved = spawned;
            }
            else
            {
                *g_levelObjectsSaved = 0;
            }
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setLoadStep(loadStep() + 1);
            break;
        }
        case kLoadScripts:
            zge::linkCutSceneScripts();
            zge::runCutSceneSetupScripts();
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setLoadStep(loadStep() + 1);
            break;
        case kHideLoading:
            if (*g_loadStepFrames == kShowFrame)
            {
                playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashPicture, kFrameNode), kShutdown);
                playUiNode(zge::zgui::findNode(*g_levelLoadingFrame, kSplashCenter, kFrameNode), kShutdown);
                playUiNode(*g_levelLoadingFrame, kShutdown);
            }
            else if (*g_loadStepFrames == kLoadingFrames)
            {
                zge::setChunkProgressHook(zge::ChunkProgressHook(), 0);
                if (g_bmxZgui->camera)
                {
                    zge::releaseSceneCamera(g_bmxZgui->camera);
                    g_bmxZgui->camera = {};
                }
                *g_levelLoadFreeBytes = zge::freeHeapBytes();
                initActionCameras();
                makeCameras(camera);
                *g_levelLoadFreeBytes = zge::freeHeapBytes();
                setLoadStep(loadStep() + 1);
            }
            break;
        case kLoadOverlay:
            openOverlay();
            if (*g_sessionKind == kParkTestSession)
                showOverlayDisplay(kOverlayHealth, 0);
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeSetOut);
            setLoadStep(loadStep() + 1);
            break;
        case kLoadGame:
            *g_cameraState = kCameraStatePlay;
            *g_replayPadPlayer = kNoPadPlayer;
            *g_cameraStateStep = 0;
            *g_actionFlags = 0;
            *g_currentPlayer = 0;
            gameSetup();
            startSession();
            *g_levelLoadFreeBytes = zge::freeHeapBytes();
            setLoadStep(loadStep() + 1);
            break;
        case kCloseModels:
            if (*g_entityModelLibrary)
            {
                zge::closeAssetLibrary(*g_entityModelLibrary);
                *g_entityModelLibrary = 0;
            }
            setLoadStep(loadStep() + 1);
            break;
        case kLoadDone:
            setPlayPhase(kPlayPhaseLoaded);
            *g_actionTask = zge::newTask(kActionTaskStage, kActionTaskFrame, 0, 1, kActionSourceFile, kActionTaskLine);
            playOverlayFade(kOverlayWholeScreen, kOverlayFadeIn);
            *g_levelsLoaded = static_cast<uint8_t>(*g_levelsLoaded + 1);
            return 0;
        default:
            break;
        }
        *g_loadStepFrames = *g_loadStepFrames + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(levelStateLoad, Verified);
}
