// BMX XXX's game code (bmx.c's side of the park editor): Aggressive Inline's
// game/aggressive/level/park_editor.cpp, copied for BMX XXX: the functions the two games have word for
// word. The editor paused and resumed, a park saved to the memory card from the editor and one loaded
// into it. The rest is BMX XXX's own: entering and leaving the editor, the test run, the frame, and the
// two game states that load a park (the editor's, and the one that plays a saved park) with their
// loading screen.

#include "bmx/editor/park_editor.h"
#include "bmx/editor/edit_core.h"
#include "bmx/editor/edit_tool_dynamics.h"
#include "bmx/editor/edit_tool_object.h"
#include "bmx/editor/edit_tool_rail.h"
#include "bmx/editor/edit_tool_start.h"
#include "bmx/editor/edit_tool_terrain.h"
#include "bmx/editor/edit_tool_test_run.h"
#include "bmx/editor/edit_tools.h"
#include "zge/assets/file.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/render/display.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/memory/regions.h"
#include "zge/scene/message.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "bmx/save/park_buffer.h"
#include "bmx/save/park_save.h"
#include "bmx/action/level_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/spawn_points.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/states/intro.h"
#include "bmx/ui/ui_nodes.h"
#include "zge/assets/chunk_reader.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/park_editor.cpp.h"

namespace game::bmx::editor
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        // The game's state of the editor (g_parkEditor): the fields the functions written so far use. BMX XXX
        // has one word more than Aggressive Inline before cardBusy.
        struct ParkEditorState
        {
            uint32_t editing;      // +0x00 in the editor
            uint32_t stateEntered; // +0x04 1 from either park state's enter to its exit
            uint32_t objectsRun;   // +0x08 the frame updates the scene's objects: nothing in the game sets it (the states' enter and a test run's end clear it), so the frame never does
            uint32_t paused;       // +0x0C the editor's pause menu up: nothing moves
            uint32_t inGame;       // +0x10 the park is being ridden: a test run, or the editor entered so
            uint32_t testRun;      // +0x14 a test run under way
            uint32_t cardBusy;     // +0x18 a save or load dialog up
            uint32_t uiLoaded;     // +0x1C the editor's UI pages are there
            ee::Ptr<ParkBuffer> cardBlock;    // +0x20 the buffer the dialog saves or loads (0: none)
        };
        inline ee::Global<ParkEditorState, sym::g_parkEditor> g_parkEditor;

        constexpr uint32_t kParkBufferBytes = 0x2000;     // a park's save
        constexpr uint32_t kParkDataKind = 0;
        constexpr uint32_t kCardLoaded = 2; // the load dialog's result
        constexpr uint32_t kGameModePlay = 2, kGameModePaused = 3; // the game's modes
        constexpr uint32_t kPauseMenuOnInit = sym::pauseMenuOnInit;      // "PLAY ParkEditor.PauseMenu.OnInit"

        // A line of the UI's script run on a widget (the root's scripts with none).
        void runScript(uint32_t text) { zge::zgui::runScript(ee::Ptr<zge::zgui::Node>(), ee::Ptr<char>(text), 0); }
        void runUiScript(uint32_t root, uint32_t text) { zge::zgui::runScript(ee::Ptr<zge::zgui::Node>(root), ee::Ptr<char>(text), 0); }
        // Where the original hands on what runScript (void) left in its result register: called at its address.
        int32_t runScriptResult(uint32_t node, ee::Address text) { return ee::game<int32_t>(sym::runScript, node, text, 0); }
        inline ee::Global<uint32_t, sym::g_uiRoot> g_uiRoot;
        // The UI's own camera (in BMX XXX's Zgui record): the editor gives it back when the park is ridden.
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_uiCamera> g_uiCamera;

        // Scaffolding (address-call): the original's tail jump to bmx.c's setGameState.
        void setGameStateTail(uint32_t state) { ee::tailCall<void>(sym::setGameState, state); }

        // The game's states, modes, session kinds and levels this file sets. The mode numbered 0 and
        // the state numbered 0 (the one with the game's menus) have no name read from bmx.c yet.
        constexpr uint32_t kGameStateMenus = 0, kGameStateLevel = 1;
        constexpr uint32_t kGameModeFirst = 0;
        constexpr uint8_t kSessionParkEditor = 2, kSessionTestRun = 4;
        constexpr uint8_t kLevelPark = 12;
        inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;

        // The editor's frame runs at this step whatever the frame took.
        constexpr float kFrameSeconds = 0.0167f;

        // The preset parks (assets\editor\presets): the one chosen, and each one's file name.
        constexpr uint32_t kPresetParks = 8;
        inline ee::Global<uint8_t, sym::g_presetParkIndex> g_presetParkIndex;
        inline ee::Global<ee::Ptr<char>[kPresetParks], sym::g_presetParkFiles> g_presetParkFiles;
    }

    // Scaffolding (address-call): the original's tail jump to the game's sound mode (bmx/sound/sound_mode.h).
    namespace calls
    {
        void setGameModeTail(uint32_t mode) { ee::tailCall<void>(sym::setGameMode, mode); }
    }

    void setEditorEventHandlers();
    void enterParkEditor(uint32_t theme, uint32_t piecesFile, uint32_t inGame);
    void playSavedPark(uint32_t theme, uint32_t piecesFile, ee::Ptr<ParkBuffer> block);
    void loadPresetPark();

    namespace
    {
        // The park states' loading: its step, and the calls of the loading step spent in it.
        inline ee::Global<uint32_t, sym::g_parkLoadStep> g_parkLoadStep;
        inline ee::Global<uint32_t, sym::g_parkLoadStepFrames> g_parkLoadStepFrames;
        // The steps. The editor's state starts at the first, or at the last when a test run
        // comes back to it; the state that plays a saved park starts at the first.
        constexpr uint32_t kLoadShowFrame = 0, kLoadScene = 1, kLoadShowText = 2, kLoadPark = 3, kLoadHideFrame = 4,
                           kLoadBackFromTestRun = 5, kLoadSteps = 6;
        // A step's frames: its screen's animations get this long; the text comes in half way.
        constexpr uint32_t kStepFrames = 72, kTextFrame = 36;
        // What a state's loading step answers (the game's state machine reads it).
        constexpr int32_t kLoadDone = 0, kLoadMore = 1, kLoadQuit = 2;

        // 1 in the state that plays a saved park, 0 in the editor's (each state's enter sets it).
        inline ee::Global<uint32_t, sym::g_playingSavedPark> g_playingSavedPark;
        // What the editor starts with: a park from the memory card (its buffer in g_parkToLoad),
        // a preset park, or neither (an empty park). The game's menus set these.
        inline ee::Global<uint8_t, sym::g_editorParkSource> g_editorParkSource;
        constexpr uint8_t kParkSourceCard = 1, kParkSourcePreset = 2;
        inline ee::Global<ee::Ptr<ParkBuffer>, sym::g_parkToLoad> g_parkToLoad;
        // The park's theme (the game's own byte), and each preset park's theme.
        inline ee::Global<uint8_t, sym::g_parkTheme> g_parkTheme;
        inline ee::Global<uint8_t[kPresetParks], sym::g_presetParkThemes> g_presetParkThemes;
        // The session kind of a saved park being played (bmx.c's own name for it is not read).
        constexpr uint8_t kSessionPlayPark = 1;

        // A theme's scene: its name ("PEMullaly") and its folder under assets\editor ("mullaly").
        constexpr uint32_t kParkThemes = 4;
        struct ParkScene
        {
            ee::Ptr<const char> name;
            ee::Ptr<const char> folder;
        };
        inline ee::Global<ParkScene[kParkThemes], sym::g_parkScenes> g_parkScenes;

        // The loading screen: the UI's "UI_common.FrameLoading" frame, found when it is shown.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_parkLoadingFrame> g_parkLoadingFrame;
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;
        // The loading bar's frame, as far as the bar's drawing reads it (BMX XXX's zgui frame).
        struct LoadingBarFrame
        {
            uint8_t _00[0x30];
            zge::Vec4 screenPosition; // +0x30
            uint8_t _40[0x0C];
            ee::Float height;         // +0x4C (named from its use: the drawn rectangle's height)
        };
        // The loading bar's frame node as that record.
        ee::Ptr<LoadingBarFrame> loadingBarFrameOf(ee::Ptr<zge::zgui::Node> node) { return node.cast<LoadingBarFrame>(); }
        inline ee::Global<uint8_t[4], sym::g_loadingBarColour> g_loadingBarColour; // light grey, alpha 100

        // The UI camera's far plane while the editor shows.
        constexpr float kEditorFarClip = 500.0f;
    }

    // The editor's frame (the game state's): the editor's core, then unless paused, and when
    // the objects run, the scene's objects.
    uint32_t parkEditorFrame()
    {
        updateEditCore(kFrameSeconds);
        if (g_parkEditor->paused == 0 && g_parkEditor->objectsRun != 0)
            zge::updateObjects(kFrameSeconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(parkEditorFrame, Verified);

    // The rider's spawn point "PlayerOne" put at the park's start object, when the park has one.
    void placeRiderAtStart()
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::EulerAngles> angles;
        ee::Local<zge::Matrix> matrix;
        if (parkStartPosition(position, angles) == 0)
            return;
        zge::matrixIdentity(matrix);
        zge::matrixRotateXYZ(matrix, matrix, angles);
        zge::matrixTranslated(matrix, matrix, &position->xyz());
        setSpawnPoint(matrix, ee::Ptr<const char>(sym::g_textPlayerOne));
    }
    READABLE_FUNCTION_WAITS(placeRiderAtStart, Verified);

    // A saved park played: the pieces read, the park of its theme built from the buffer, the
    // buffer freed, the UI's camera given back, the rider's spawn at the park's start, the
    // editor's core freed.
    void playSavedPark(uint32_t theme, uint32_t piecesFile, ee::Ptr<ParkBuffer> block)
    {
        startEditCore(piecesFile);
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        buildParkLevel(theme, data, bytes);
        freeParkBuffer(block);
        zge::releaseSceneCamera(*g_uiCamera);
        *g_uiCamera = {};
        placeRiderAtStart();
        game::tail(endEditCore);
    }
    READABLE_FUNCTION_WAITS(playSavedPark, Verified);

    // The editor entered: its core started with the pieces file and its tools registered, the
    // park of the theme made, the pause menu's volume script functions, the action cameras,
    // the events' handlers.
    void enterParkEditor(uint32_t theme, uint32_t piecesFile, uint32_t inGame)
    {
        startEditCore(piecesFile);
        registerEditDynamicsToolClass();
        registerEditRailToolClass();
        registerEditObjectToolClass();
        registerEditStartToolClass();
        registerEditTerrainToolClass();
        registerEditTestRunToolClass();
        openParkEditor(theme, g_parkEditor->uiLoaded);
        zge::zgui::registerScriptFunction(sym::g_textInlineMenuSetSFXVolume, sym::inlineMenuSetSFXVolume);
        zge::zgui::registerScriptFunction(sym::g_textInlineMenuGetSFXVolume, sym::inlineMenuGetSFXVolume);
        initActionCameras();
        setEditorEventHandlers();
        g_parkEditor->inGame = inGame;
        g_parkEditor->editing = 1;
        g_parkEditor->paused = 0;
    }
    READABLE_FUNCTION_WAITS(enterParkEditor, Verified);

    // The editor's quit event: the editor goes back to the game's menus (the scene left, unless
    // a test run is the session); with the park being ridden, the game is paused instead.
    // The UI's camera is given back either way.
    void quitParkEditor()
    {
        if (g_parkEditor->inGame == 0)
        {
            emptyQuitHook();
            if (*g_sessionKind != kSessionTestRun)
                zge::leaveScene();
            setGameState(kGameStateMenus);
            setGameMode(kGameModeFirst);
        }
        else
            setGameMode(kGameModePaused);
        zge::releaseSceneCamera(*g_uiCamera);
        *g_uiCamera = {};
        g_parkEditor->editing = 0;
    }
    READABLE_FUNCTION_WAITS(quitParkEditor, Verified);

    // The editor's editing-ended event: the in-game UI back; with the park being ridden its
    // editing display is hidden.
    void editingEnded()
    {
        if (g_parkEditor->inGame == 0)
            setGameMode(kGameModeFirst);
        runUiScript(*g_uiRoot, sym::g_textPlayInGameUiShowAll); // "PLAY InGameUI.ShowAll"
        if (g_parkEditor->inGame != 0)
            game::tail(hideEditCorePark);
    }
    READABLE_FUNCTION_WAITS(editingEnded, Verified);

    // The editor's test-run-started event: the start object hidden, the UI's camera given
    // back, the rider's spawn at the park's start, and the game sent to ride the park level
    // as a test run.
    void testRunStarted()
    {
        g_parkEditor->testRun = 1;
        showParkStartObject(0);
        zge::releaseSceneCamera(*g_uiCamera);
        *g_uiCamera = {};
        placeRiderAtStart();
        g_parkEditor->inGame = 1;
        *g_currentLevel = kLevelPark;
        *g_sessionKind = kSessionTestRun;
        setGameMode(kGameModeFirst);
        setGameStateTail(kGameStateLevel);
    }
    READABLE_FUNCTION_WAITS(testRunStarted, Verified);

    // The editor's test-run-ended event: the session is the editor's again, nothing is ridden
    // or run, the start object and the park's editing display shown.
    void testRunEnded()
    {
        *g_sessionKind = kSessionParkEditor;
        g_parkEditor->inGame = 0;
        g_parkEditor->testRun = 0;
        g_parkEditor->objectsRun = 0;
        showParkStartObject(1);
        game::tail(showEditCorePark);
    }
    READABLE_FUNCTION_WAITS(testRunEnded, Verified);

    // The edit core's loading-a-preset event: the chosen preset park's file read from the
    // disc and loaded in place of the editor's park.
    void loadPresetPark()
    {
        ee::Local<char[0x40]> path;
        ee::Local<uint32_t> fileBytes;
        const uint32_t data = zge::heapAllocate(kParkBufferBytes);
        zge::formatString(path, sym::bmx::g_textPresetParkPath, (*g_presetParkFiles)[*g_presetParkIndex]); // "assets\editor\presets\%s"
        const ee::Ptr<zge::ZFile> file = zge::openFile(path, 1, 0).file();
        zge::fileSize(file, fileBytes, 0);
        zge::fileRead(file, data, *fileBytes, 0);
        zge::closeFile(file, 0);
        clearEditCorePark();
        loadParkData(data, kParkBufferBytes);
        zge::heapFree(data);
    }
    READABLE_FUNCTION_WAITS(loadPresetPark, Draft);

    // A park's bytes read into the editor in place of the one it has; the buffer freed.
    void loadParkIntoEditor(uint32_t /*theme*/, uint32_t /*piecesFile*/, ee::Ptr<ParkBuffer> block)
    {
        clearEditCorePark();
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        loadParkData(data, bytes);
        game::tail(game::bmx::freeParkBuffer, block);
    }
    READABLE_FUNCTION_WAITS(loadParkIntoEditor, Draft);

    // The edit core's paused and resumed callbacks.
    void editorPaused()
    {
        g_parkEditor->paused = 1;
        calls::setGameModeTail(kGameModePaused);
    }
    READABLE_FUNCTION_WAITS(editorPaused, Verified);

    void editorResumed()
    {
        g_parkEditor->paused = 0;
        calls::setGameModeTail(kGameModePlay);
    }
    READABLE_FUNCTION_WAITS(editorResumed, Verified);

    // The card's job done with a save: the pause menu back, the buffer freed.
    void parkSaved()
    {
        g_parkEditor->cardBusy = 0;
        runScript(kPauseMenuOnInit);
        freeParkBuffer(g_parkEditor->cardBlock);
        g_parkEditor->cardBlock = {};
    }
    READABLE_FUNCTION_WAITS(parkSaved, Verified);

    // The card's job done with a load: a park that loaded is read into the editor in place of
    // its own; the pause menu back, the buffer freed.
    void parkLoaded(uint32_t result)
    {
        if (result == kCardLoaded)
        {
            clearEditCorePark();
            const uint32_t data = parkBufferData(g_parkEditor->cardBlock);
            const uint32_t bytes = parkBufferSize(g_parkEditor->cardBlock);
            loadParkData(data, bytes);
        }
        g_parkEditor->cardBusy = 0;
        runScript(kPauseMenuOnInit);
        freeParkBuffer(g_parkEditor->cardBlock);
        g_parkEditor->cardBlock = {};
    }
    READABLE_FUNCTION_WAITS(parkLoaded, Draft);

    // The edit core's saving callback: the park written into a new buffer, and its save started.
    uint32_t saveParkToCard()
    {
        const ee::Ptr<ParkBuffer> block = newParkBuffer(kParkBufferBytes);
        setParkBufferClient(block, kParkDataKind);
        g_parkEditor->cardBlock = block;
        const uint32_t data = parkBufferData(block);
        const uint32_t bytes = parkBufferSize(block);
        saveParkData(data, bytes);
        startParkSave(block, sym::parkSaved);
        g_parkEditor->cardBusy = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveParkToCard, Verified);

    int32_t parkEditorTheme()
    {
        return editCoreTheme();
    }
    READABLE_FUNCTION(parkEditorTheme, Draft);

    uint32_t inParkEditor()
    {
        return g_parkEditor->editing;
    }
    READABLE_FUNCTION(inParkEditor, Verified);

    // The edit core's loading callback: a new buffer, and a park's load into it started.
    uint32_t loadParkFromCard()
    {
        const ee::Ptr<ParkBuffer> block = newParkBuffer(kParkBufferBytes);
        setParkBufferClient(block, kParkDataKind);
        g_parkEditor->cardBlock = block;
        startParkLoad(block, sym::parkLoaded, 1);
        g_parkEditor->cardBusy = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadParkFromCard, Draft);

    // The saved game's sound effects volume (0 .. 127): the byte the game save keeps it in.
    
    namespace
    {
        constexpr ee::Ptr<uint8_t> kSavedSfxVolume{sym::g_savedGame + 0x20};
        constexpr uint32_t kSetVolume = 0;  // soundEvent's event: a channel's volume set
        constexpr uint32_t kVolumeSfx = 0;  // its channel
        constexpr int32_t kMaxVolume = 127;
        constexpr int32_t kMaxSliderValue = 128;
    }

    // The script function inlineMenuSetSFXVolume (slider, test): the slider's value (90 if it
    // has none), kept to 0 .. 127, becomes the saved sound effects volume and the channel's,
    // with a test sound unless the second argument says no.
    int32_t inlineMenuSetSFXVolume(uint32_t arguments)
    {
        ee::Local<ee::Float> value;
        ee::Local<int32_t> playTest;
        const ee::Ptr<ee::Ptr<zge::zgui::Node>> argument(arguments);
        *value = ee::Float(90.0f);
        *playTest = 1;
        zge::zgui::variableFloatValue(argument[0], value);
        zge::zgui::variableIntValueAgain(argument[1], playTest);
        const int32_t slider = ee::toInt(*value);
        const int32_t volume = slider < kMaxVolume ? (slider <= 0 ? 0 : slider) : kMaxVolume;
        *kSavedSfxVolume = static_cast<uint8_t>(volume);
        return static_cast<int32_t>(soundEvent(kSetVolume, kVolumeSfx, *kSavedSfxVolume, static_cast<uint32_t>(*playTest), 0));
    }
    READABLE_FUNCTION_WAITS(inlineMenuSetSFXVolume, Verified);

    // The script function inlineMenuGetSFXVolume (slider): the saved sound effects volume,
    // rounded up to even and at most 128, set as the slider's value; then the slider's
    // on-change script.
    int32_t inlineMenuGetSFXVolume(uint32_t arguments)
    {
        ee::Local<ee::Float> shown;
        const ee::Ptr<ee::Ptr<zge::zgui::Node>> argument(arguments);
        const int32_t volume = *kSavedSfxVolume;
        const int32_t even = volume + (volume & 1);
        *shown = ee::toFloat(even > kMaxSliderValue ? kMaxSliderValue : even);
        zge::zgui::setVariable(argument[0], sym::g_textVolumeCurValue, shown); // "CurValue"
        return runScriptResult(ee::Address(argument[0]), sym::g_textVolumePlayOnChange); // "PLAY OnChange"
    }
    READABLE_FUNCTION_WAITS(inlineMenuGetSFXVolume, Verified);

    // The loading's step set; its frames start over.
    void setParkLoadStep(uint32_t step)
    {
        *g_parkLoadStep = step;
        *g_parkLoadStepFrames = 0;
    }
    READABLE_FUNCTION(setParkLoadStep, Verified);

    uint32_t parkLoadStep()
    {
        return *g_parkLoadStep;
    }
    READABLE_FUNCTION(parkLoadStep, Verified);

    // A park state entered (both states' enter): nothing of the editor is up yet. The state
    // that plays a saved park just starts its loading. The editor's sets the game's play mode;
    // coming back from a test run its loading is the short one, else a preset park gives the
    // park its theme and the loading starts at its first step.
    int32_t enterParkState()
    {
        g_parkEditor->stateEntered = 1;
        g_parkEditor->editing = 0;
        g_parkEditor->uiLoaded = 0;
        g_parkEditor->paused = 0;
        g_parkEditor->objectsRun = 0;
        g_parkEditor->inGame = 0;
        g_parkEditor->testRun = 0;
        g_parkEditor->cardBusy = 0;
        g_parkEditor->cardBlock = {};
        if (*g_playingSavedPark != 0)
        {
            setParkLoadStep(kLoadShowFrame);
            return 1;
        }
        setGameMode(kGameModePlay);
        if (*g_sessionKind == kSessionTestRun)
        {
            setParkLoadStep(kLoadBackFromTestRun);
            return 1;
        }
        if (*g_editorParkSource == kParkSourcePreset)
            *g_parkTheme = (*g_presetParkThemes)[*g_presetParkIndex];
        setParkLoadStep(kLoadShowFrame);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterParkState, Verified);

    // The enter of the game state that plays a saved park, and of the editor's.
    int32_t enterPlayParkState()
    {
        *g_playingSavedPark = 1;
        return enterParkState();
    }
    READABLE_FUNCTION_WAITS(enterPlayParkState, Verified);

    int32_t enterParkEditorState()
    {
        *g_playingSavedPark = 0;
        return enterParkState();
    }
    READABLE_FUNCTION_WAITS(enterParkEditorState, Verified);

    // The exit of the state that plays a saved park.
    void exitPlayParkState()
    {
        g_parkEditor->stateEntered = 0;
        g_parkEditor->uiLoaded = 0;
    }
    READABLE_FUNCTION(exitPlayParkState, Verified);

    // The frame of the state that plays a saved park: the park is loaded, so the game goes on
    // to ride the park level.
    int32_t playParkFrame()
    {
        *g_currentLevel = kLevelPark;
        g_parkEditor->inGame = 1;
        *g_sessionKind = kSessionPlayPark;
        setGameState(kGameStateLevel);
        return 1;
    }
    READABLE_FUNCTION_WAITS(playParkFrame, Verified);

    // The park's scene entered: the theme's scene, with the level's files taken from the
    // theme's folder under assets\editor. Answers what entering the scene does.
    uint32_t loadParkScene()
    {
        ee::Local<char[0x40]> sceneName;
        ee::Local<ParkScene[kParkThemes]> scenes;
        for (uint32_t theme = 0; theme < kParkThemes; ++theme)
            scenes[theme] = (*g_parkScenes)[theme];
        zge::formatString(sceneName, scenes[*g_parkTheme].name); // the theme's scene name, a text with no format marks
        zge::formatString(sym::g_levelFilePrefix, sym::g_textParkFolderFormat, sym::g_textAssetsEditor, scenes[*g_parkTheme].folder); // "%s\%s\", "assets\editor"
        return zge::enterScene(sceneName.address());
    }
    READABLE_FUNCTION_WAITS(loadParkScene, Verified);

    // The chunk reader's progress hook while the park's scene loads: the loading screen's bar
    // drawn as wide as the load is far (the whole width at most), straight to the display.
    void drawParkLoadingBar(uint32_t /*name*/, int32_t bytesDone, int32_t totalBytes)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> corner;
        ee::Local<ee::Float> width;
        for (uint32_t i = 0; i < 4; ++i)
            colour[i] = (*g_loadingBarColour)[i];
        const ee::Float done = ee::toFloat(bytesDone) / ee::toFloat(totalBytes);
        ee::Float fraction(1.0f);
        if (done <= ee::Float(1.0f))
            fraction = done;
        const ee::Ptr<LoadingBarFrame> bar = loadingBarFrameOf(zge::zgui::findNode(ee::Ptr<zge::zgui::Node>(*g_uiRoot), sym::g_textLoadingBar, kFrameNode)); // "UI_common.FrameLoading.LOADING.Bar"
        *corner = bar->screenPosition;
        // The bar ends as far from the screen's right edge as it starts from the left.
        *width = (ee::Float(static_cast<float>(kScreenWidth)) - (corner->x + corner->x)) * fraction;
        zge::zgui::setVariable(bar, sym::g_textLoadingBarWidth, width); // "Width"
        zge::clearDisplayRectangle(static_cast<uint32_t>(ee::toInt(corner->x)), static_cast<uint32_t>(ee::toInt(corner->y)),
                                   static_cast<uint32_t>(ee::toInt(*width)), static_cast<uint32_t>(ee::toInt(bar->height)), ee::Ptr<uint8_t>(colour.address()));
    }
    READABLE_FUNCTION_WAITS(drawParkLoadingBar, Verified);

    namespace
    {
        // The loading screen put up: the UI's camera (made if there is none), the viewports
        // reset, the loading frame found and placed.
        void showLoadingFrame(ee::Local<zge::Vec4> &position)
        {
            if (!*g_uiCamera)
                *g_uiCamera = ee::Ptr<zge::SceneObject>(zge::newSceneCamera(1));
            zge::resetCameraViewports(kScreenWidth, kScreenHeight);
            const ee::Ptr<zge::zgui::Node> frame = zge::zgui::findNode(ee::Ptr<zge::zgui::Node>(*g_uiRoot), sym::g_textUiCommonFrameLoading, kFrameNode); // "UI_common.FrameLoading"
            // Only the three coordinates are set, as the original's.
            position->x = ee::Float(64.0f);
            *g_parkLoadingFrame = frame;
            position->y = ee::Float(0.0f);
            position->z = ee::Float(0.0f);
            zge::zgui::setVariable(frame, sym::g_textLoadingPosition, position); // "Position"
        }

        void nextParkLoadStep()
        {
            setParkLoadStep(parkLoadStep() + 1);
        }
    }

    // The park states' loading step, run each frame until it answers done (or quit, when the
    // scene or the park's edges fail). Step by step: the loading screen comes in; the theme's
    // scene loads behind its progress bar; the splash picture moves off and the "loading park"
    // text comes in; the park itself (the editor entered, with the card's park or a preset
    // read into it; or the saved park built to be ridden); the loading screen shuts down. The
    // last step is the editor's way back from a test run: its screen and camera again, then
    // the shutdown step.
    int32_t loadParkState()
    {
        ee::Local<char[0x40]> text;
        ee::Local<zge::Vec4> position;
        ee::Local<int32_t> visible;
        const uint32_t step = parkLoadStep();
        if (step < kLoadSteps)
        {
            const uint32_t frames = *g_parkLoadStepFrames;
            switch (step)
            {
            case kLoadShowFrame:
                if (frames == 1)
                {
                    showLoadingFrame(position);
                    playUiNode(*g_parkLoadingFrame, sym::g_textLoadingInit); // "Init"
                    const ee::Ptr<zge::zgui::Node> picture = zge::zgui::findNode(*g_parkLoadingFrame, sym::g_textSplashPicture, kFrameNode); // "SplashPicture"
                    playUiNode(picture, sym::g_textLoadingInit);
                }
                else if (frames == kStepFrames)
                    nextParkLoadStep();
                break;
            case kLoadScene:
                // A test run's park is already there.
                if (*g_playingSavedPark != 0 || *g_sessionKind != kSessionTestRun)
                {
                    zge::setChunkProgressHook(zge::ChunkProgressHook(sym::drawParkLoadingBar), 0);
                    const uint32_t entered = loadParkScene();
                    zge::setChunkProgressHook(zge::ChunkProgressHook(), 0);
                    if (entered != 1)
                        return kLoadQuit;
                }
                nextParkLoadStep();
                break;
            case kLoadShowText:
                if (frames == kTextFrame)
                {
                    zge::formatString(text, sym::g_textLoadingPark); // "$KSTR_LOADING_PARK"
                    const ee::Ptr<zge::zgui::Node> info = zge::zgui::findNode(*g_parkLoadingFrame, sym::g_textSplashCenterInfo, kFrameNode); // "SplashCenter.Info"
                    zge::zgui::setVariable(info, sym::g_textLoadingText, text); // "Text"
                    const ee::Ptr<zge::zgui::Node> center = zge::zgui::findNode(*g_parkLoadingFrame, sym::g_textSplashCenter, kFrameNode); // "SplashCenter"
                    playUiNode(center, sym::g_textLoadingInit);
                }
                else if (frames > kTextFrame)
                {
                    if (frames == kStepFrames)
                        nextParkLoadStep();
                }
                else if (frames == 1)
                {
                    const ee::Ptr<zge::zgui::Node> picture = zge::zgui::findNode(*g_parkLoadingFrame, sym::g_textSplashPicture, kFrameNode);
                    playUiNode(picture, sym::g_textLoadingMove); // "Move"
                }
                break;
            case kLoadPark:
                zge::formatString(text, sym::g_textPiecesFileFormat, sym::g_textAssetsEditor); // "%s\BMX3.Pep", "assets\editor"
                if (*g_playingSavedPark != 0)
                    playSavedPark(*g_parkTheme, text.address(), *g_parkToLoad);
                else
                {
                    if (*g_sessionKind != kSessionTestRun)
                    {
                        enterParkEditor(*g_parkTheme, text.address(), 0);
                        if (*g_editorParkSource == kParkSourceCard)
                            loadParkIntoEditor(*g_parkTheme, text.address(), *g_parkToLoad);
                        else if (*g_editorParkSource == kParkSourcePreset)
                            loadPresetPark();
                    }
                    runUiScript(*g_uiRoot, sym::g_textPlayParkEditorSetRes); // "PLAY Root.ParkEditor.SetRes"
                    g_parkEditor->uiLoaded = 1;
                }
                if (rider::collision::scanNodesEdgeTags() != 1)
                    return kLoadQuit;
                nextParkLoadStep();
                break;
            case kLoadHideFrame:
                if (frames == 1)
                {
                    playUiNode(*g_parkLoadingFrame, sym::g_textShutdownPE); // "ShutdownPE"
                    // The editor is ready at once; a park to ride waits the shutdown out.
                    if (*g_playingSavedPark == 0)
                        return kLoadDone;
                }
                else if (frames == kStepFrames)
                    return kLoadDone;
                break;
            case kLoadBackFromTestRun:
                if (frames > 1)
                    break;
                if (frames == 0)
                {
                    showLoadingFrame(position);
                    *visible = 1;
                    zge::zgui::setVariable(*g_parkLoadingFrame, sym::g_textLoadingVisible, visible); // "Visible"
                }
                editCoreUseUiCamera();
                setCameraNodeFarClip(*g_uiCamera, ee::Float(kEditorFarClip));
                runUiScript(*g_uiRoot, sym::g_textPlayParkEditorOnExitTest); // "PLAY Root.ParkEditor.OnExitTest"
                runUiScript(*g_uiRoot, sym::g_textPlayParkEditorSetRes);
                g_parkEditor->uiLoaded = 1;
                if (rider::collision::scanNodesEdgeTags() != 1)
                    return kLoadDone;
                setParkLoadStep(kLoadHideFrame);
                break;
            default:
                break;
            }
        }
        *g_parkLoadStepFrames = *g_parkLoadStepFrames + 1;
        return kLoadMore;
    }
    READABLE_FUNCTION_WAITS(loadParkState, Verified);

    // The exit of the editor's state: the edit core cleaned up; unless a test run is leaving
    // to ride the park, the editor's UI pages go too.
    void exitParkEditorState()
    {
        cleanEditCore();
        if (g_parkEditor->testRun == 0 && g_parkEditor->uiLoaded != 0)
        {
            zge::zgui::destroyNode(zge::zgui::findNode(ee::Ptr<zge::zgui::Node>(*g_uiRoot), sym::g_textParkEditorNode, kFrameNode)); // "ParkEditor"
            g_parkEditor->uiLoaded = 0;
        }
        g_parkEditor->stateEntered = 0;
    }
    READABLE_FUNCTION_WAITS(exitParkEditorState, Verified);

    // The game's handlers for the edit core's events. (BMX XXX has one more than Aggressive
    // Inline: the loading of a preset park.)
    void setEditorEventHandlers()
    {
        setEditCoreCallback(kEditCoreTestStarted, EditCoreCallback(sym::testRunStarted));
        setEditCoreCallback(kEditCoreTestEnded, EditCoreCallback(sym::testRunEnded));
        setEditCoreCallback(kEditCorePaused, EditCoreCallback(sym::editorPaused));
        setEditCoreCallback(kEditCoreResumed, EditCoreCallback(sym::editorResumed));
        setEditCoreCallback(kEditCoreEditEnded, EditCoreCallback(sym::editingEnded));
        setEditCoreCallback(kEditCoreSaving, EditCoreCallback(sym::saveParkToCard));
        setEditCoreCallback(kEditCoreLoadingPreset, EditCoreCallback(sym::loadPresetPark));
        setEditCoreCallback(kEditCoreLoading, EditCoreCallback(sym::loadParkFromCard));
        game::tail(setEditCoreCallback, kEditCoreQuit, EditCoreCallback(sym::quitParkEditor));
    }
    READABLE_FUNCTION(setEditorEventHandlers, Verified);
}
