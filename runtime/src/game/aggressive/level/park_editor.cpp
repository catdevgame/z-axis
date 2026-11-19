// The park editor's game side (gamehand12; were the sweep's drafts of
// blocks_001bec40 and the three natives natives_planned.cpp held): the
// level scripts' natives, the game's state of the editor, the editor's
// game mode (7: its enter, exit and frame), its events from the editor's
// core (the editCore functions of blocks_001f1528: a test run started or
// ended, editing left, paused, a park saved or loaded on the card, the
// editor quit) and the parks played or edited from the front end.
//
// Two editors share it: the Park Editor (the front end's, game type 5: a
// park of one of four themes built from the pieces file "Inline.Pep" and
// saved on the card) and the in-game editor (the career levels' that have
// one, entered from their scripts: activateParkEditor; its pieces stay
// in the level after editing).
//

#include "aggressive/level/park_editor.h"
#include "aggressive/skater/action_tricks.h"
#include "ee/vu0.h"
#include "zge/assets/file.h"
#include "zge/collision/collision.h"
#include "zge/container/list.h"
#include "zge/memory/heaps.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include <cstring>
#include "aggressive/camera.h"
#include "aggressive/conversions.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/script/native_parts.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "aggressive/level/edit_records.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/scene/locators.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/render/display.h"
#include "aggressive/front_end.h"
#include "aggressive/game_frame.h"
#include "aggressive/game_start.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/double_rails.h"
#include "aggressive/level/edit_core.h"
#include "aggressive/level/edit_tool_crane.h"
#include "aggressive/level/edit_tool_object.h"
#include "aggressive/level/edit_tool_pisser.h"
#include "aggressive/level/edit_tool_rail.h"
#include "aggressive/level/edit_tool_start.h"
#include "aggressive/level/edit_tool_terrain.h"
#include "aggressive/level/edit_tool_test_run.h"
#include "aggressive/level/edit_tool_vacuum.h"
#include "aggressive/level/park_buffer.h"
#include "aggressive/level/park_save.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_selection.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_editor.cpp.h"

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        // The engine's (zge).
        constexpr uint32_t kSetCameraScales = sym::setCameraScales;      // (camera, f12 horizontal, f13 vertical): Camera's screen scales
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage;    // (EngineMessage *) (hooked by a game override: by address)

        // The game's.
        constexpr uint32_t kSetSpawnPosition = sym::setSpawnPosition;     // (name hash, Vec4 *): the named "Spawn" locator's position (below)
        constexpr uint32_t kSetSpawnOrientation = sym::setSpawnOrientation;  // (name hash, EulerAngles *): its orientation (below)
        constexpr uint32_t kFollowCameras = sym::followCameras;        // () -> the follow cameras
        constexpr uint32_t kSetFollowCameraTarget = sym::setFollowCameraTarget; // (cameras, target)

        // The editor's core (editCore, blocks_001f1528 ..).
        constexpr uint32_t kReadPark = sym::loadParkData;             // (data, bytes): the park loaded from them

        // This file's, handed on by address.
        constexpr uint32_t kActivateParkEditor = sym::activateParkEditor;
        constexpr uint32_t kDeactivateParkEditor = sym::deactivateParkEditor;
        constexpr uint32_t kCleanParkEditor = sym::cleanParkEditor;
        constexpr uint32_t kPlaceSkaterAtStart = sym::placeSkaterAtStart;
        constexpr uint32_t kEnterParkEditor = sym::enterParkEditor;
        constexpr uint32_t kSetEditorEventHandlers = sym::setEditorEventHandlers;
        constexpr uint32_t kSetUiViewport = sym::setUiViewport;
        constexpr EditCoreCallback kTestRunStarted(sym::testRunStarted);
        constexpr EditCoreCallback kTestRunEnded(sym::testRunEnded);
        constexpr EditCoreCallback kEditorPaused(sym::editorPaused);
        constexpr EditCoreCallback kEditorResumed(sym::editorResumed);
        constexpr EditCoreCallback kEditingEnded(sym::editingEnded);
        constexpr EditCoreCallback kSaveParkToCard(sym::saveParkToCard);
        constexpr EditCoreCallback kLoadParkFromCard(sym::loadParkFromCard);
        constexpr EditCoreCallback kQuitParkEditor(sym::quitParkEditor);
        constexpr uint32_t kParkSaved = sym::parkSaved;
        constexpr uint32_t kParkLoaded = sym::parkLoaded;

        // The game's state of the editor (g_parkEditor).
        struct ParkEditorState
        {
            uint32_t editing;      // +0x00 in the editor (enterParkEditor .. quitParkEditor)
            uint32_t started;      // +0x04 set at the level's setup (registerParkEditorNatives), cleared at its end
            uint32_t testRun;      // +0x08 a test run on: the objects and skaters move
            uint32_t paused;       // +0x0C the editor's pause menu up: nothing moves
            uint32_t inGameEditor; // +0x10 the in-game editor (a career level's), not the Park Editor
            uint32_t cardBusy;     // +0x14 a save or load dialog up
            uint32_t uiLoaded;     // +0x18 ParkEditor.zsc loaded (the levels with an in-game editor)
            ee::Ptr<ParkBuffer> cardBlock;    // +0x1C the DataBlock the dialog saves or loads (0: none)
        };
        static_assert(offsetof(ParkEditorState, inGameEditor) == 0x10 && offsetof(ParkEditorState, cardBlock) == 0x1C);
        inline ee::Global<ParkEditorState, sym::g_parkEditor> g_parkEditor;

        // A DataBlock (12 bytes from the heap): a buffer and its size.
        constexpr uint32_t kDataBlockBytes = 0x2000;      // a park's save
        constexpr uint32_t kParkDataKind = 0;

        // The level's first player: the one the editor's test runs use.
        ee::Ptr<skater::Player> playerOne() { return &currentLevel()->players[0]; }

        // The camera object zgui draws its 3D with (the front end's follow
        // camera), and the display's default field of view and aspect.
        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;
        inline ee::Global<ee::Float, sym::g_defaultFieldOfView> g_defaultFieldOfView;
        inline ee::Global<ee::Float, sym::g_defaultAspect> g_defaultAspect;

        // The whole screen, as a test run shows it (the editor keeps a panel at its side).
        constexpr uint32_t kScreenWidth = 512, kScreenHeight = 448;

        // The levels with an in-game editor: Cannery, Airfield, Industrial.
        constexpr uint32_t kCannery = 2, kAirfield = 3, kIndustrial = 4;
        inline ee::Global<uint32_t, sym::g_levelNumber> g_levelWord; // g_levelIndex read as a word

        constexpr uint32_t kGameModePlay = 2, kGameModePaused = 3; // GameMode
        constexpr uint32_t kModeParkEditor = 7;                    // the level's game mode record of the editor
        constexpr uint32_t kModePlay = 2;
        constexpr uint32_t kTopStateFrontEnd = 1;
        constexpr uint32_t kEntryMainMenu = 1;
        constexpr uint32_t kSceneShown = 1;
        constexpr uint32_t kCardLoaded = 2; // the load dialog's result

        // Strings (.data and .sdata).
        constexpr uint32_t kActivateName = sym::activateName;         // "activateParkEditor"
        constexpr uint32_t kDeactivateName = sym::deactivateName;       // "deactivateParkEditor"
        constexpr uint32_t kCleanName = sym::cleanName;            // "cleanParkEditor"
        constexpr uint32_t kParkEditorScript = sym::parkEditorScript;     // "ParkEditor.zsc"
        constexpr uint32_t kParkEditorVisible = sym::parkEditorVisible;    // "Root.ParkEditor.Visible"
        constexpr uint32_t kOnDestroyParkEditor = sym::onDestroyParkEditor;  // "PLAY Root.ParkEditor.OnDestroyParkEditor"
        constexpr uint32_t kOnExitEdit = sym::onExitEdit;           // "PLAY Root.ParkEditor.OnExitEdit"
        constexpr uint32_t kInGamePiecesFile = sym::inGamePiecesFile;     // "assets\editor\Inline.Pep"
        constexpr uint32_t kPlayerOne = sym::playerOne2;            // "PlayerOne"
        constexpr ee::Ptr<char> kSetParkEditor{sym::setParkEditor};        // "PLAY InGameUI.SetParkEditor"
        constexpr ee::Ptr<char> kSetIngameEditor{sym::setIngameEditor};      // "PLAY InGameUI.SetIngameEditor"
        constexpr uint32_t kParkEditorObject = sym::parkEditorObject;     // "ScriptObj_ParkEditor"
        constexpr uint32_t kParkEditExit = sym::parkEditExit;         // "ParkEditExit"
        constexpr uint32_t kShowAll = sym::showAll;              // "PLAY InGameUI.ShowAll"
        constexpr uint32_t kSetTestRun = sym::setTestRun;           // "PLAY InGameUI.SetTestRun"
        constexpr uint32_t kPauseMenuOnInit = sym::pauseMenuOnInit;      // "PLAY ParkEditor.PauseMenu.OnInit"

        // The skater's scene shown or hidden (hidden while editing).
        ee::Ptr<zge::Scene> skaterScene()
        {
            return zge::sceneOf(ee::Ptr<zge::SceneObject>(playerOne()->node));
        }
    }

    // activateParkEditor (the native activateParkEditor(theme)): the in-game
    // editor entered with the level's pieces file.
    uint32_t activateParkEditor()
    {
        const int32_t theme = script::parts::popInt();
        enterParkEditor(theme, kInGamePiecesFile, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(activateParkEditor, Verified);

    // deactivateParkEditor (the native deactivateParkEditor): in the editor, its
    // UI told to leave editing ("OnExitEdit").
    uint32_t deactivateParkEditor()
    {
        if (g_parkEditor->editing != 0)
            zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kOnExitEdit), 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(deactivateParkEditor, Verified);

    // cleanParkEditor (the native cleanParkEditor): editing left and the kept
    // pieces released.
    uint32_t cleanParkEditor()
    {
        deactivateParkEditor();
        cleanEditCore();
        return 0;
    }
    READABLE_FUNCTION_WAITS(cleanParkEditor, Verified);

    // registerParkEditorNatives (the level's setup): the park editor's natives
    // registered and its state reset; in the levels with an in-game editor
    // its UI script loaded, hidden.
    void registerParkEditorNatives()
    {
        ee::Local<uint32_t> value2;
        script::scriptRegisterNative(ee::Ptr<const char>(kActivateName), kActivateParkEditor);
        script::scriptRegisterNative(ee::Ptr<const char>(kDeactivateName), kDeactivateParkEditor);
        script::scriptRegisterNative(ee::Ptr<const char>(kCleanName), kCleanParkEditor);
        const uint32_t level = *g_levelWord;
        ParkEditorState &editor = *g_parkEditor;
        editor.started = 1;
        editor.editing = 0;
        editor.paused = 0;
        editor.testRun = 0;
        editor.inGameEditor = 0;
        editor.cardBusy = 0;
        editor.cardBlock = 0;
        if (level != kCannery && level != kIndustrial && level != kAirfield)
        {
            g_parkEditor->uiLoaded = 0;
            return;
        }
        const ee::Ptr<zgui::Node> root = *g_uiRoot;
        *value2 = 0; // the variable's value: hidden
        zge::zgui::loadZguiScript(kParkEditorScript, root);
        zge::zgui::setNodeValueByPath(kParkEditorVisible, value2);
        g_parkEditor->uiLoaded = 1;
    }
    READABLE_FUNCTION_WAITS(registerParkEditorNatives, Verified);

    // shutdownParkEditor (the level's end): the kept pieces released; the UI
    // script told the editor is gone when it was loaded.
    void shutdownParkEditor()
    {
        cleanEditCore();
        if (g_parkEditor->uiLoaded != 0)
        {
            zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kOnDestroyParkEditor), 0);
            g_parkEditor->uiLoaded = 0;
        }
        g_parkEditor->started = 0;
    }
    READABLE_FUNCTION_WAITS(shutdownParkEditor, Verified);

    // loadParkIntoEditor (the front end's Load Park, after enterParkEditor): the
    // park emptied and the saved one read from the card's block, the block
    // freed.
    void loadParkIntoEditor(uint32_t /*theme*/, uint32_t /*piecesFile*/, ee::Ptr<ParkBuffer> block)
    {
        clearEditCorePark();
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        loadParkData(data, bytes);
        game::tail(freeParkBuffer, block);
    }
    READABLE_FUNCTION_WAITS(loadParkIntoEditor, Verified);

    int32_t setSpawnPosition(uint32_t name, ee::Ptr<const zge::Vec4> position);

    int32_t setSpawnOrientation(uint32_t name, ee::Ptr<const zge::EulerAngles> angles);

    // placeSkaterAtStart: player one put at the park's StartObject (the "Spawn"
    // point PlayerOne moved there) and respawned at it; without one,
    // respawned where the level's locator puts it.
    void placeSkaterAtStart()
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::EulerAngles> angles;
        const uint32_t found = parkStartPosition(position, angles);
        if (found != 0)
        {
            const uint32_t name = zge::nameHash(ee::Ptr<const char>(kPlayerOne));
            setSpawnPosition(name, position);
            const uint32_t sameName = zge::nameHash(ee::Ptr<const char>(kPlayerOne));
            setSpawnOrientation(sameName, angles);
            playerOne()->flags |= skater::kRespawnAtLocator;
            aggressive::skater::respawn(playerOne(), 1, 0);
            return;
        }
        playerOne()->flags |= skater::kRespawnAtLocator;
        aggressive::skater::respawn(playerOne(), 0, 0);
    }
    READABLE_FUNCTION_WAITS(placeSkaterAtStart, Verified);

    // playSavedPark (Free Skate's Editor Level: a saved park played): the
    // pieces read, the park of its theme built from the block, the block
    // freed, the skater at its start, the editor's core freed, the grind
    // edges paired.
    void playSavedPark(uint32_t theme, uint32_t piecesFile, ee::Ptr<ParkBuffer> block)
    {
        startEditCore(piecesFile);
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        buildParkLevel(theme, data, bytes);
        freeParkBuffer(block);
        placeSkaterAtStart();
        endEditCore();
        game::tail(aggressive::level::setupLevelRails);
    }
    READABLE_FUNCTION_WAITS(playSavedPark, Verified);

    void setEditorEventHandlers();

    // enterParkEditor: the editor entered (the front end's Park Editor, or a
    // level's in-game editor): its core started with the pieces file and
    // its tools registered, the park of the theme made, the editor's
    // camera, the skater hidden, the events' handlers, the game in the
    // editor's mode, and the UI's look for the one or the other.
    void enterParkEditor(uint32_t theme, uint32_t piecesFile, uint32_t inGameEditor)
    {
        startEditCore(piecesFile);
        registerEditRailToolClass();
        registerEditObjectToolClass();
        registerEditPisserToolClass();
        registerEditCraneToolClass();
        registerEditVacuumToolClass();
        registerEditStartToolClass();
        registerEditTerrainToolClass();
        registerEditTestRunToolClass();
        openParkEditor(theme, g_parkEditor->uiLoaded);
        aggressive::setFollowCameraMode(followCameras(), static_cast<uint32_t>(CameraMode::Editor));
        zge::changeSceneState(skaterScene(), kSceneShown, 0);
        setEditorEventHandlers();
        ParkEditorState &editor = *g_parkEditor;
        editor.paused = 0;
        editor.editing = 1;
        editor.inGameEditor = inGameEditor;
        aggressive::enterGameMode(kModeParkEditor);
        if (inGameEditor == 0)
        {
            setupLevelRails();
            return game::tail(zge::zgui::runScript, *g_uiRoot, kSetParkEditor, 0);
        }
        showEditCorePark();
        return game::tail(zge::zgui::runScript, *g_uiRoot, kSetIngameEditor, 0);
    }
    READABLE_FUNCTION_WAITS(enterParkEditor, Verified);

    // enterParkEditorMode: the editor's game mode entered (nothing to do).
    uint32_t enterParkEditorMode()
    {
        return 1;
    }
    READABLE_FUNCTION(enterParkEditorMode, Verified);

    // parkEditorFrame: the editor's game mode's frame: the editor's core,
    // then unless paused the level's effects, and in a test run the
    // objects and the skaters before them.
    uint32_t parkEditorFrame(uint32_t /*mode*/, ee::Float seconds)
    {
        updateEditCore(seconds);
        if (g_parkEditor->paused != 0)
            return 1;
        if (g_parkEditor->testRun != 0)
        {
            zge::updateObjects(seconds);
            updatePlayers(seconds);
        }
        levelEffectsFrame(seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(parkEditorFrame, Verified);

    // exitParkEditorMode: the editor's game mode left (nothing to do).
    void exitParkEditorMode()
    {
    }
    READABLE_FUNCTION(exitParkEditorMode, Verified);

    // quitParkEditor (the editor's quit event): the Park Editor goes back to
    // the front end's main menu, the in-game editor back to play.
    void quitParkEditor()
    {
        if (g_parkEditor->inGameEditor == 0)
        {
            aggressive::setExitDestination(kEntryMainMenu);
            aggressive::switchGameState(kTopStateFrontEnd);
            g_parkEditor->editing = 0;
            return;
        }
        aggressive::enterGameMode(kModePlay);
        g_parkEditor->editing = 0;
    }
    READABLE_FUNCTION_WAITS(quitParkEditor, Verified);

    // editingEnded (the editor's editing-ended event): the level script's
    // ScriptObj_ParkEditor told "ParkEditExit", the camera following
    // again, the skater shown, the in-game UI back; the in-game editor's
    // pieces kept.
    void editingEnded()
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint32_t> names;
        *names = zge::nameHash(ee::Ptr<const char>(kParkEditorObject));
        const ee::Ptr<ee::Ptr<zge::SceneObject>> slot = zge::findObjects(names, 1);
        if (slot)
        {
            const ee::Ptr<zge::SceneObject> object = *slot;
            if (object)
            {
                message->target = ee::Address(object); // the message's word
                message->sender = 0;
                message->type = zge::nameHash(ee::Ptr<const char>(kParkEditExit));
                message->flags = zge::kMessageNow;
                message->data = 0;
                zge::sendEngineMessage(message);
            }
        }
        aggressive::setFollowCameraMode(followCameras(), static_cast<uint32_t>(CameraMode::Follow));
        zge::changeSceneState(skaterScene(), 0, kSceneShown);
        zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kShowAll), 0);
        if (g_parkEditor->inGameEditor != 0)
            hideEditCorePark();
    }
    READABLE_FUNCTION_WAITS(editingEnded, Verified);

    void setUiViewport(uint32_t width, uint32_t height);

    // testRunStarted (the editor's test-run-started event): the UI's
    // viewport full screen, the skater at the start, the camera following
    // it, the in-game UI for a test run; things move.
    uint32_t testRunStarted()
    {
        setUiViewport(kScreenWidth, kScreenHeight);
        placeSkaterAtStart();
        aggressive::setFollowCameraMode(followCameras(), static_cast<uint32_t>(CameraMode::Follow));
        setFollowCameraTarget(followCameras(), playerOne());
        zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kSetTestRun), 0);
        g_parkEditor->testRun = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(testRunStarted, Verified);

    // testRunEnded (the editor's test-run-ended event): nothing moves, the
    // editor's camera, the skater hidden, the Park Editor's UI.
    void testRunEnded()
    {
        g_parkEditor->testRun = 0;
        aggressive::setFollowCameraMode(followCameras(), static_cast<uint32_t>(CameraMode::Editor));
        zge::changeSceneState(skaterScene(), kSceneShown, 0);
        return game::tail(zge::zgui::runScript, *g_uiRoot, kSetParkEditor, 0);
    }
    READABLE_FUNCTION_WAITS(testRunEnded, Verified);

    // editorPaused (the editor's pause event): paused, the game mode's
    // sounds the pause's.
    void editorPaused()
    {
        g_parkEditor->paused = 1;
        game::tail(aggressive::setGameMode, kGameModePaused);
    }
    READABLE_FUNCTION_WAITS(editorPaused, Verified);

    // editorResumed (the editor's resume event).
    void editorResumed()
    {
        g_parkEditor->paused = 0;
        game::tail(aggressive::setGameMode, kGameModePlay);
    }
    READABLE_FUNCTION_WAITS(editorResumed, Verified);

    // parkSaved (the save dialog's done): the pause menu shown again,
    // the block freed.
    void parkSaved()
    {
        g_parkEditor->cardBusy = 0;
        zgui::runScript(ee::Ptr<zgui::Node>(0u), ee::Ptr<char>(kPauseMenuOnInit), 0);
        freeParkBuffer(g_parkEditor->cardBlock);
        g_parkEditor->cardBlock = 0;
    }
    READABLE_FUNCTION_WAITS(parkSaved, Verified);

    // parkLoaded (the load dialog's done, `result` the dialog's): a park
    // loaded replaces the one in the editor; the pause menu shown again,
    // the block freed.
    void parkLoaded(uint32_t result)
    {
        uint32_t data = 0;
        if (result == kCardLoaded)
        {
            clearEditCorePark();
            data = parkBufferData(g_parkEditor->cardBlock);
            const uint32_t bytes = parkBufferSize(g_parkEditor->cardBlock);
            loadParkData(data, bytes);
        }
        g_parkEditor->cardBusy = 0;
        zgui::runScript(ee::Ptr<zgui::Node>(0u), ee::Ptr<char>(kPauseMenuOnInit), 0);
        freeParkBuffer(g_parkEditor->cardBlock);
        g_parkEditor->cardBlock = 0;
    }
    READABLE_FUNCTION_WAITS(parkLoaded, Verified);

    // saveParkToCard (the editor's save event): the park written into a
    // card block of 0x2000 bytes and the card's save dialog opened.
    uint32_t saveParkToCard()
    {
        const ee::Ptr<ParkBuffer> block = newParkBuffer(kDataBlockBytes);
        setParkBufferClient(block, kParkDataKind);
        g_parkEditor->cardBlock = block;
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        saveParkData(data, bytes);
        startParkSave(block, kParkSaved);
        g_parkEditor->cardBusy = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveParkToCard, Verified);

    // loadParkFromCard (the editor's load event): a card block of 0x2000 bytes
    // and the card's load dialog opened.
    uint32_t loadParkFromCard()
    {
        const ee::Ptr<ParkBuffer> block = newParkBuffer(kDataBlockBytes);
        setParkBufferClient(block, kParkDataKind);
        g_parkEditor->cardBlock = block;
        startParkLoad(block, kParkLoaded);
        g_parkEditor->cardBusy = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadParkFromCard, Verified);

    // setEditorEventHandlers (enterParkEditor's): the editor core's events given the
    // game's handlers.
    void setEditorEventHandlers()
    {
        setEditCoreCallback(kEditCoreTestStarted, kTestRunStarted);
        setEditCoreCallback(kEditCoreTestEnded, kTestRunEnded);
        setEditCoreCallback(kEditCorePaused, kEditorPaused);
        setEditCoreCallback(kEditCoreResumed, kEditorResumed);
        setEditCoreCallback(kEditCoreEditEnded, kEditingEnded);
        setEditCoreCallback(kEditCoreSaving, kSaveParkToCard);
        setEditCoreCallback(kEditCoreLoading, kLoadParkFromCard);
        game::tail(setEditCoreCallback, kEditCoreQuit, kQuitParkEditor);
    }
    READABLE_FUNCTION(setEditorEventHandlers, Verified);

    namespace
    {
        // A camera object's record: its camera.
        ee::Ptr<zge::Camera> cameraOf(uint32_t object) { return zge::sceneOf(ee::Ptr<zge::SceneObject>(object)).cast<zge::Camera>(); }
    }

    // setUiViewport: every camera given the display's viewport, then zgui's
    // camera the viewport (0, 0, width, height) and the display's default
    // field of view and aspect.
    void setUiViewport(uint32_t width, uint32_t height)
    {
        ee::Local<ee::Float[4]> viewport; // x, y, width, height
        viewport[0] = 0.0f;
        viewport[1] = 0.0f;
        viewport[2] = unsignedToFloat(width);
        viewport[3] = unsignedToFloat(height);
        zge::resetCameraViewports(width, height);
        zge::setCameraViewport(cameraOf(*g_uiCamera), viewport);
        zge::setCameraScales(cameraOf(*g_uiCamera), *g_defaultFieldOfView, *g_defaultAspect);
    }
    READABLE_FUNCTION_WAITS(setUiViewport, Verified);
}

// The "Spawn" locators set (gamehand13; were the sweep's drafts of
// setSpawnPosition and setSpawnOrientation): where a player respawns, the editor's start.
namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kSpawnKind = sym::g_textSpawn;         // "Spawn"
    }

    namespace
    {
        // freeLocators: the list's locators back to their pool. By address: the caller
        // returns what the (void) function leaves in the result register.
        int32_t freeLocatorsKeepingResult(ee::Ptr<zge::LocatorList> list) { return ee::game<int32_t>(sym::freeLocators, list); }
    }

    // setSpawnPosition (name hash, Vec4 *): the first "Spawn" locator of that
    // name moved there. Returns freeLocators'.
    int32_t setSpawnPosition(uint32_t name, ee::Ptr<const zge::Vec4> position)
    {
        ee::Local<zge::LocatorList> list;
        zge::findLocators(list, zge::nameHash(ee::Ptr<const char>(kSpawnKind)), name);
        const ee::Ptr<zge::ScenePoint> spawn(list->head->point);
        zge::aligned(ee::Ptr<zge::Vec4>(&spawn->position)) = zge::quadwordOf(position);
        return freeLocatorsKeepingResult(list);
    }
    READABLE_FUNCTION_WAITS(setSpawnPosition, Verified);

    // setSpawnOrientation (name hash, EulerAngles *): the first "Spawn" locator of
    // that name turned so (the angles' matrix as its quaternion). Returns
    // freeLocators'.
    int32_t setSpawnOrientation(uint32_t name, ee::Ptr<const zge::EulerAngles> angles)
    {
        ee::Local<zge::LocatorList> list;
        ee::Local<zge::Matrix> matrix;
        zge::findLocators(list, zge::nameHash(ee::Ptr<const char>(kSpawnKind)), name);
        const ee::Ptr<zge::Locator> first = list->head;
        zge::matrixIdentity(matrix);
        zge::matrixRotateXYZ(matrix, matrix, angles);
        zge::matrixToQuaternion(&ee::Ptr<zge::ScenePoint>(first->point)->orientation, matrix);
        return freeLocatorsKeepingResult(list);
    }
    READABLE_FUNCTION_WAITS(setSpawnOrientation, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_parkEditorCore> g_parkEditorCore;
        inline ee::Global<int32_t, sym::g_parkLevelObject> g_parkLevelObject; // g_parkLevelObject; its name is a low-confidence guess (readable_names.tsv)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(readPark, ee::game, kReadPark);
            GAME_TABLE_ROUTE(setCameraScalesint32_t, ee::game<int32_t>, kSetCameraScales);
        }
    }
}

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    // parkEditorTheme: the park's theme (the list of saved parks shows only
    // the ones of it while in the editor, addParkSaveFile).
    int32_t parkEditorTheme()
    {
        return editCoreTheme();
    }
    READABLE_FUNCTION(parkEditorTheme, Verified);

    // inParkEditor: whether the Park Editor is in use (ParkEditorState::editing).
    uint32_t inParkEditor()
    {
        return g_parkEditor->editing;
    }
    READABLE_FUNCTION(inParkEditor, Verified);
}
