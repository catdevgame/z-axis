// DM2's park editor as a game state (editor_state.h): the state's enter,
// exit and frame (the game's state table holds them beside the front end's
// and the level's), the three ways the editor opens, the editor's script
// functions and the small entries other code reaches the editor through.
// DM2's own code (the game block between the dynamics objects and fx.c).

#include "editor_state.h"

#include "copy_tool.h"
#include "edit_camera.h"
#include "edit_core.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "edit_gui.h"
#include "edit_lights.h"
#include "edit_objects.h"
#include "edit_pools.h"
#include "edit_tags.h"
#include "edit_themes.h"
#include "gap_tool.h"
#include "icon_roll.h"
#include "park_editor.h"
#include "park_grid.h"
#include "rail_tool.h"
#include "ground_towers.h"
#include "park_scores.h"
#include "start_tool.h"

#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "dm2/action/ui_frame.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/level/level_load_progress.h"
#include "dm2/fx/effects_clock.h"
#include "dm2/fx/library.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/math/conversions.h"
#include "zge/input/pads.h"
#include "zge/memory/dmm.h"
#include "zge/render/display.h"
#include "zge/scene/camera.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/editor_state.cpp.h"

namespace zge
{
    uint32_t setChunkProgressHook(ChunkProgressHook hook); // assets/chunk_reader.cpp
}

namespace game::dm2
{
    namespace rider
    {
        PadInput *padInput(uint32_t port); // input.cpp
    }
}

namespace game::dm2
{
    namespace
    {
        // The editor's one record.
        inline ee::Global<ParkEditor, sym::g_parkEditor> g_parkEditor;
        // How the editor state opens the editor next (the front end's menu
        // variable "ParkEntryType"): kOpenNewPark, kOpenLoadedPark (the park
        // in g_parkBuffer), kReturnFromTest (back from a test run of the
        // park, which editTestLevel sets).
        inline ee::Global<int32_t, sym::g_parkEntryType> g_parkEntryType;
        constexpr int32_t kOpenNewPark = 0, kOpenLoadedPark = 1, kReturnFromTest = 2;
        // The park read from the memory card, while the editor opens it.
        inline ee::Global<ee::Ptr<ParkStream>, sym::g_parkBuffer> g_parkBuffer;
        // A dirty flag of the editor's GUI and the editor's first entry
        // (edit_lights.cpp names them).
        inline ee::Global<uint32_t, sym::g_editGuiReady> g_editGuiReady;
        inline ee::Global<uint32_t, sym::g_editorEntering> g_editorEntering;
        inline ee::Global<uint32_t, sym::g_relightRunning> g_relightRunning;
        inline ee::Global<uint32_t, sym::g_editSceneEntered> g_editSceneEntered;
        inline ee::Global<ee::Float, sym::g_parkCapacityUsed> g_parkCapacityUsed;
        // The display's default camera scales.
        inline ee::Global<ee::Float, sym::g_defaultFieldOfView> g_defaultFieldOfView;
        inline ee::Global<ee::Float, sym::g_defaultAspect> g_defaultAspect;
        // The whole screen as a viewport: x, y, width, height.
        inline ee::Global<ee::Float[4], sym::uiViewportRectangle> g_screenViewport;
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;

        constexpr uint32_t kEditorWidgetName = sym::editorWidgetName;   // "ParkEditor"
        constexpr uint32_t kEditorStartScript = sym::editorStartScript; // "PLAY Root.ParkEditor.Start"
        constexpr uint32_t kTerrainToolName = sym::terrainToolName;     // "Terrain"

        // The game's mode globals a park test sets (round.cpp keeps them).
        inline ee::Global<uint8_t, sym::g_multiplayerMode> g_multiplayerMode;
        inline ee::Global<uint8_t, sym::g_singlePlayerMode> g_singlePlayerMode;
        // The editor's load bar's parts while the editor loads
        // (editorLoadProgress brings them up).
        constexpr uint32_t kEditorLoadParts = 6;
        inline ee::Global<ee::Float[kEditorLoadParts], sym::g_editorLoadParts> g_editorLoadParts;

        // A park's file in memory: what newParkStream is asked for.
        constexpr uint32_t kParkFileBytes = 0x2565;
        constexpr uint32_t kPauseMenuUnhideScript = sym::pauseMenuUnhideScript; // "PLAY Root.ParkEditor.MainUI.PauseMenu.UnHide"
        constexpr uint32_t kLoadParkScript = sym::loadParkScript;               // "PLAY Root.ParkEditor.LoadPark"

        void runRootScript(uint32_t script)
        {
            zge::zgui::runScriptLines(*zge::zgui::g_root, ee::Ptr<char>(script));
        }

        // Each step of the editor's frame is told a frame's time.
        constexpr ee::Float kFrameSeconds = 0.03f;

        ee::Ptr<ParkEditor> editorRecord() { return &*g_parkEditor; }

        // The camera a camera object is (its scene).
        ee::Ptr<zge::Camera> cameraOf(ee::Ptr<zge::SceneObject> object) { return zge::sceneOf(object).cast<zge::Camera>(); }
    }

    ee::Ptr<ParkEditor> parkEditor()
    {
        return editorRecord();
    }
    READABLE_FUNCTION(parkEditor, Verified);

    uint8_t editTool()
    {
        return g_parkEditor->tool;
    }
    READABLE_FUNCTION(editTool, Verified);

    void markStopFrameOpen()
    {
        g_parkEditor->userLocked = 1;
    }
    READABLE_FUNCTION(markStopFrameOpen, Verified);

    void editUnlockUser()
    {
        g_parkEditor->userLocked = 0;
    }
    READABLE_FUNCTION(editUnlockUser, Verified);

    void editGapCancel()
    {
        g_parkEditor->toolStep = 1;
        editUnlockUser();
        game::tail(clearNewGapEnd);
    }
    READABLE_FUNCTION(editGapCancel, Verified);

    void setEditTool(uint8_t tool)
    {
        const ee::Ptr<ParkEditor> editor = editorRecord();
        // The tool in hand put away.
        switch (editor->tool)
        {
        case kEditToolTerrain:
        {
            // The terrain tool's tape frame closed (its DeInit ACTION).
            const ee::Ptr<zge::zgui::Node> root = zge::zgui::g_zgui->root;
            const ee::Ptr<zge::zgui::Node> tapeFrame = zge::zgui::findNode(root, sym::editStateTapeFramePath, 0);
            const ee::Ptr<zge::zgui::ListedAction> deInit = widgetAction(tapeFrame, ee::Ptr<const char>(sym::editStateDeInitName));
            zge::sendNewMessage(0, zge::zgui::g_zgui->root, static_cast<uint32_t>(zge::zgui::Message::PlayAction), deInit, 1);
            break;
        }
        case kEditToolPiece: hideHeldObject(); break;
        case kEditToolRail: releaseRailPreview(); break;
        case kEditToolStart: releaseStartMarkers(); break;
        case kEditToolGap: hideGapMarkers(); break;
        case kEditToolCopy: clearCopiedCells(editor); break;
        default: break;
        }
        editor->tool = tool;
        // The new one taken up.
        switch (tool)
        {
        case kEditToolTerrain:
            editor->toolStep = 0;
            game::tail(resetSelection, editor);
            break;
        case kEditToolPiece: game::tail(showHeldObject, 0); break;
        case kEditToolRail:
            resetSelection(editor);
            game::tail(markRailPreviewStale);
            break;
        case kEditToolStart: game::tail(newStartMarkers, editor); break;
        case kEditToolGap:
            editor->toolStep = 0;
            game::tail(beginNewGap, editor);
            break;
        default: break;
        }
    }
    READABLE_FUNCTION(setEditTool, Verified);

    void setUiViewport()
    {
        ee::Local<ee::Float[4]> viewport;
        for (uint32_t i = 0; i < 4; ++i)
            viewport[i] = (*g_screenViewport)[i];
        zge::resetCameraViewports(kScreenWidth, kScreenHeight);
        const ee::Ptr<zge::SceneObject> camera = g_parkEditor->viewCamera;
        zge::setCameraViewport(cameraOf(camera), viewport.address());
        zge::setCameraScales(cameraOf(camera), *g_defaultFieldOfView, *g_defaultAspect);
    }
    READABLE_FUNCTION(setUiViewport, Verified);

    void editorStartStub(ee::Ptr<ParkEditor>)
    {
    }
    READABLE_FUNCTION(editorStartStub, Verified);

    void editorCloseStub(ee::Ptr<ParkEditor>)
    {
    }
    READABLE_FUNCTION(editorCloseStub, Verified);

    void setUpParkEditor()
    {
        const ee::Ptr<zge::Pad> pad = zge::padAt(0);
        destroyEditPools();
        const ee::Ptr<ParkEditor> editor = editorRecord();
        initEditorCore(editor, pad);
        initEditThemes();
        startEditGui(editor);
        setUpEditGapTags();
        addEditToolEntry(kEditToolTerrain, kToolEntryNoCategory, ee::Ptr<const char>(kTerrainToolName), 1);
        editorStartStub(editor);
        initRails();
        loadEditObjects();
        setUpGaps();
        updateEditorCore(editor);
        selectFirstEditToolEntry();
        *g_editSceneEntered = 0;
    }
    READABLE_FUNCTION(setUpParkEditor, Verified);

    void openNewPark()
    {
        setUpParkEditor();
        const ee::Ptr<ParkEditor> editor = editorRecord();
        openParkTheme();
        applyThemeLighting(editor->theme);
        relightWorld();
        centreCursor(editor);
        initInlineFx();
        *g_parkCapacityUsed = 0.0f;
        game::tail(setUiViewport);
    }
    READABLE_FUNCTION(openNewPark, Verified);

    void openLoadedPark()
    {
        *g_parkCapacityUsed = 0.0f;
        setUpParkEditor();
        addEditToolEntry(kEditToolRail, kToolEntryNoCategory, ee::Ptr<const char>(sym::railsToolName), 1);
        addEditToolEntry(kEditToolStart, kToolEntryNoCategory, ee::Ptr<const char>(sym::startToolName), 1);
        addEditToolEntry(kEditToolGap, kToolEntryNoCategory, ee::Ptr<const char>(sym::gapsToolName), 1);
        const ee::Ptr<ParkEditor> editor = editorRecord();
        loadPark(editor, *g_parkBuffer);
        applyThemeLighting(editor->theme);
        relightWorld();
        selectEditToolEntry();
        centreCursor(editor);
        freeParkStream(*g_parkBuffer);
        initInlineFx();
        game::tail(setUiViewport);
    }
    READABLE_FUNCTION(openLoadedPark, Verified);

    // editLoadFromPark (Park.zsc's): the park in the park buffer opened in
    // the editor already running: the editor's parts set up again for it
    // (its lights, themes, rails, objects and gaps), the park loaded with
    // its tools, lit, the cursor centred and the tool roll rewound; then the
    // editor's Start ACTION played, the user unlocked and the inline
    // effects started.
    void editLoadFromPark()
    {
        *g_parkCapacityUsed = 0.0f;
        initParkLights();
        addEditToolEntry(kEditToolTerrain, kToolEntryNoCategory, ee::Ptr<const char>(kTerrainToolName), 1);
        const ee::Ptr<ParkEditor> editor = editorRecord();
        initEditThemes();
        initRails();
        loadEditObjects();
        setUpGaps();
        loadPark(editor, *g_parkBuffer);
        addEditToolEntry(kEditToolRail, kToolEntryNoCategory, ee::Ptr<const char>(sym::railsToolName), 1);
        addEditToolEntry(kEditToolStart, kToolEntryNoCategory, ee::Ptr<const char>(sym::startToolName), 1);
        addEditToolEntry(kEditToolGap, kToolEntryNoCategory, ee::Ptr<const char>(sym::gapsToolName), 1);
        applyThemeLighting(editor->theme);
        relightWorld();
        selectEditToolEntry();
        centreCursor(editor);
        rewindToolRoll(editor);
        freeParkStream(*g_parkBuffer);
        zge::zgui::runScriptLines(*zge::zgui::g_root, ee::Ptr<char>(kEditorStartScript));
        editUnlockUser();
        game::tail(initInlineFx);
    }
    READABLE_FUNCTION(editLoadFromPark, Draft);

    void returnToParkEditor()
    {
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(0)->pad));
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(1)->pad));
        const ee::Ptr<ParkEditor> editor = editorRecord();
        startEditGui(editor);
        updateEditorCore(editor);
        collectThemeLights();
        applyThemeLighting(editor->theme);
        selectFirstEditToolEntry();
        centreCursor(editor);
        game::tail(setUiViewport);
    }
    READABLE_FUNCTION(returnToParkEditor, Verified);

    void shutDownEditCore()
    {
        freeCopyBuffers();
        shutDownParkLights();
        zge::deleteSceneObject(widgetObject(zge::zgui::findNode(*zge::zgui::g_root, kEditorWidgetName, 0)));
        *g_editGuiReady = 0;
    }
    READABLE_FUNCTION(shutDownEditCore, Verified);

    void registerEditCoreFunctions()
    {
        registerZguiIconRoll();
        zge::zgui::loadZguiScript(sym::parkScriptFile, *zge::zgui::g_root);
        *g_editGuiReady = 1;
        setUpEditView(editorRecord());
        // The editor's script functions, by the names Park.zsc calls them.
        using zge::zgui::registerScriptFunction;
        registerScriptFunction(sym::editSetModeSaveParkName, sym::editSetModeSavePark);
        registerScriptFunction(sym::editSetModeLoadParkName, sym::editSetModeLoadPark);
        registerScriptFunction(sym::editLoadFromParkName, sym::editLoadFromPark);
        registerScriptFunction(sym::editTestLevelName, sym::editTestLevel);
        registerScriptFunction(sym::editUnlockUserName, sym::editUnlockUser);
        registerScriptFunction(sym::editClearRecordsName, sym::editClearRecords);
        registerScriptFunction(sym::editGapOkName, sym::editGapOk);
        registerScriptFunction(sym::editBackToMainName, sym::editBackToMain);
        registerScriptFunction(sym::editLightProcessParkName, sym::editLightProcessPark);
        registerScriptFunction(sym::editLightResetName, sym::editLightReset);
        registerScriptFunction(sym::editMapClearAllName, sym::editMapClearAll);
        registerScriptFunction(sym::editGapCancelName, sym::editGapCancel);
        findEditGuiWidgets();
        initParkLights();
        allocateCopyBuffers();
        game::tail(editUnlockUser);
    }
    READABLE_FUNCTION(registerEditCoreFunctions, Verified);

    void closeParkEditor()
    {
        const ee::Ptr<ParkEditor> editor = editorRecord();
        clearGapsEntry(editor);
        freeRailList();
        editorCloseStub(editor);
        clearEditGapTags();
        stopEditGui();
        clearEditThemes(editor);
        freeEditObjects();
        freeEditToolEntries(editor);
        endEditView(editor, 0);
        game::tail(shutDownEditPools);
    }
    READABLE_FUNCTION(closeParkEditor, Verified);

    // enterParkLevel: the park in the park buffer built as the level a session rides (the park level, when it is no
    // test run from the editor): the editor's parts set up as the editor opens (no pad), the park loaded into them,
    // lit and freed from the buffer, its gaps registered, its spawn and its scores set and the world relit without
    // the sloped towers' trees; then everything the editor alone needs (its objects' list, rails, gaps entry,
    // lights, tool entries and pools) given back, so that what stays is the park's world.
    void enterParkLevel()
    {
        const ee::Ptr<ParkEditor> editor = editorRecord();
        initEditorCore(editor, ee::Ptr<zge::Pad>());
        destroyEditPools();
        initParkLights();
        initEditThemes();
        setUpEditGapTags();
        editorStartStub(editor);
        initRails();
        loadEditObjects();
        setUpGaps();
        loadPark(editor, *g_parkBuffer);
        applyThemeLighting(editor->theme);
        relightWorld();
        freeParkStream(*g_parkBuffer);
        clearRegisteredGaps();
        registerGaps();
        setParkSpawn(editor);
        applyParkScores(editor);
        relightWithoutTowerTrees();
        clearParkObjects(editor);
        editorCloseStub(editor);
        freeRailList();
        freeEditObjects();
        clearGapsEntry(editor);
        shutDownParkLights();
        freeEditToolEntries(editor);
        game::tail(shutDownEditPools);
    }
    READABLE_FUNCTION(enterParkLevel, Verified);

    void editBackToMain()
    {
        setEditTool(kEditToolNone);
        shutdownInlineFx();
        closeParkEditor();
        game::tail(setGameState, kGameStateFrontEnd);
    }
    READABLE_FUNCTION(editBackToMain, Verified);

    uint32_t relightWithoutTowerTrees()
    {
        freeSlopedTowerTrees(editorRecord());
        return game::tail(relightWorldEntry);
    }
    READABLE_FUNCTION(relightWithoutTowerTrees, Verified);

    void editTestLevel()
    {
        setEditTool(kEditToolNone);
        (*g_playerSetups)[0].character = 0;
        playerCharacter(0)->bike = 0;
        *g_multiplayerPlayers = 1;
        *g_sessionLevel = kParkLevel;
        relightWithoutTowerTrees();
        const ee::Ptr<ParkEditor> editor = editorRecord();
        stopEditGui();
        endEditView(editor, 1);
        setParkSpawn(editor);
        clearRegisteredGaps();
        registerGaps();
        *g_multiplayerMode = 0;
        *g_sessionSinglePlayerMode = kParkTest;
        *g_singlePlayerMode = kParkTest;
        setGameState(kGameStateLevel);
        // The way back is to the park as it stands; the level's load bar
        // starts with the park's geometry, textures and data loaded.
        *g_parkEntryType = kReturnFromTest;
        ee::Float *const parts = *g_levelLoadParts;
        for (uint32_t part = 0; part < kLevelLoadParts; ++part)
            parts[part] = part <= kLoadData ? 1.0f : 0.0f;
        game::tail(zge::setChunkProgressHook, zge::ChunkProgressHook(sym::levelLoadProgress));
    }
    READABLE_FUNCTION(editTestLevel, Verified);

    void bmxStartEditor()
    {
        setGameState(kGameStateEditor);
        for (uint32_t part = 0; part < kEditorLoadParts; ++part)
            (*g_editorLoadParts)[part] = 0.0f;
        game::tail(zge::setChunkProgressHook, zge::ChunkProgressHook(sym::editorLoadProgress));
    }
    READABLE_FUNCTION(bmxStartEditor, Verified);

    void editGapOk()
    {
        ee::Local<char[0x100]> name;
        ee::Local<uint32_t> scoreChoice;
        zge::zgui::copyVariable(zge::zgui::findNode(zge::zgui::g_zgui->root, sym::gapNameEditBoxPath, 0),
                                sym::editStateTextName, name.address());
        zge::zgui::copyVariable(zge::zgui::findNode(zge::zgui::g_zgui->root, sym::gapMenuPath, 0),
                                sym::editStateSelectedIndexName, scoreChoice.address());
        setNewGapScoreChoice(*scoreChoice);
        setNewGapName(name);
        const ee::Ptr<ParkEditor> editor = editorRecord();
        addNewGap(editor);
        registerNewGap(name);
        editUnlockUser();
        editor->toolStep = 0;
    }
    READABLE_FUNCTION(editGapOk, Verified);

    void editClearRecords()
    {
        game::tail(takeParkScores, editorRecord());
    }
    READABLE_FUNCTION(editClearRecords, Draft);

    void takeEditorParkScoresAgain()
    {
        game::tail(takeParkScoresAgain, editorRecord());
    }
    READABLE_FUNCTION(takeEditorParkScoresAgain, Verified);

    uint32_t newParkBuffer()
    {
        const ee::Ptr<ParkStream> stream = newParkStream(kParkFileBytes);
        *g_parkBuffer = stream;
        return ee::Address(stream->bytes);
    }
    READABLE_FUNCTION(newParkBuffer, Verified);

    void parkSaveFrameClosed(uint32_t, uint32_t stream)
    {
        freeParkStream(ee::Ptr<ParkStream>(stream));
        setUiFrame(0, 0, ee::Ptr<UiFrameRequest>());
        game::tail(runRootScript, kPauseMenuUnhideScript);
    }
    READABLE_FUNCTION(parkSaveFrameClosed, Verified);

    void parkLoadFrameClosed(uint32_t, uint32_t)
    {
        freeParkStream(*g_parkBuffer);
        setUiFrame(0, 0, ee::Ptr<UiFrameRequest>());
        game::tail(runRootScript, kPauseMenuUnhideScript);
    }
    READABLE_FUNCTION(parkLoadFrameClosed, Draft);

    void editorParkLoaded(uint32_t, uint32_t)
    {
        for (uint32_t part = 0; part < kEditorLoadParts; ++part)
            (*g_editorLoadParts)[part] = 0.0f;
        shutdownInlineFx();
        const ee::Ptr<ParkEditor> editor = editorRecord();
        clearGapsEntry(editor);
        freeEditObjects();
        freeRailList();
        clearEditThemes(editor);
        shutDownParkLights();
        game::tail(runRootScript, kLoadParkScript);
    }
    READABLE_FUNCTION(editorParkLoaded, Draft);

    void editSetModeSavePark()
    {
        ee::Local<UiFrameRequest> request;
        const ee::Ptr<ParkStream> stream = newParkStream(kParkFileBytes);
        request->dataHandle = ee::Address(stream);
        request->onDone = UiFrameCallback();
        savePark(editorRecord(), stream);
        request->onClosed = UiFrameCallback(sym::parkSaveFrameClosed);
        request->data = ee::Address(stream->bytes);
        setUiFrame(1, kUiFrameSavePark, request);
        setEditTool(kEditToolCardFrame);
    }
    READABLE_FUNCTION(editSetModeSavePark, Verified);

    // Kept from the original: a park stream is made first and dropped (it
    // stays allocated); the load's buffer is newParkBuffer's.
    void editSetModeLoadPark()
    {
        ee::Local<UiFrameRequest> request;
        newParkStream(kParkFileBytes);
        request->dataHandle = 0;
        request->onClosed = UiFrameCallback(sym::parkLoadFrameClosed);
        request->data = newParkBuffer();
        request->onDone = UiFrameCallback(sym::editorParkLoaded);
        setUiFrame(1, kUiFrameLoadPark, request);
        setEditTool(kEditToolCardFrame);
        markStopFrameOpen();
    }
    READABLE_FUNCTION(editSetModeLoadPark, Draft);

    namespace
    {
        // The editor's load bar's parts (g_editorLoadParts): the park
        // theme's four files by their extension, then two by their name.
        enum EditorLoadPart : uint32_t
        {
            kEditorLoadGeometry, // .GEO
            kEditorLoadTextures, // .TXR
            kEditorLoadData,     // .DAT
            kEditorLoadInstances,// .INS
            kEditorLoadFx,       // FX
            kEditorLoadLights,   // LIGHT
        };
        static_assert(kEditorLoadLights + 1 == kEditorLoadParts);
        constexpr uint64_t bits(double value) { return std::bit_cast<uint64_t>(value); }
        // What the parts weigh in the bar (they add up to 1), in double
        // precision as the original adds them.
        constexpr uint64_t kGeometryWeight = bits(0.48), kTexturesWeight = bits(0.38), kSmallFileWeight = bits(0.01);
        constexpr uint64_t kFxWeight = bits(0.02), kLightsWeight = bits(0.1);
        constexpr uint64_t kWholeBar = bits(1.0);
        // The bar on the load screen: its width the sum times 234, times 1.25.
        constexpr uint32_t kBarX = 290, kBarY = 101, kBarHeight = 20;
        constexpr ee::Float kBarWidth = 234.0f;
        constexpr uint64_t kBarStretch = bits(1.25);

        ee::Float fractionDone(uint32_t done, uint32_t total)
        {
            return zge::unsignedToFloat(done) / zge::unsignedToFloat(total);
        }
    }

    // Kept from the original: the lights' weight is given to the FX part a
    // second time; the LIGHT part is brought up to date but never counted.
    // FX and LIGHT are matched against the whole path, not its file name.
    void editorLoadProgress(ee::Ptr<const char> path, uint32_t bytesDone, uint32_t totalBytes)
    {
        ee::Local<char[0x10]> extension;
        ee::Local<char[0x40]> name;
        zge::pathExtension(path, extension);
        zge::pathBaseName(path, name);
        ee::Float *const parts = *g_editorLoadParts;
        if (zge::stringCompareNoCase(extension, sym::editorLoadGeoExtension) == 0)
            parts[kEditorLoadGeometry] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, sym::editorLoadTxrExtension) == 0)
            parts[kEditorLoadTextures] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, sym::editorLoadDatExtension) == 0)
            parts[kEditorLoadData] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, sym::editorLoadInsExtension) == 0)
            parts[kEditorLoadInstances] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(path, sym::editorLoadFxName) == 0)
            parts[kEditorLoadFx] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(path, sym::editorLoadLightName) == 0)
            parts[kEditorLoadLights] = fractionDone(bytesDone, totalBytes);
        else
            return;

        using namespace sce::libgcc;
        uint64_t sum = dpmul(fptodp(parts[kEditorLoadGeometry]), kGeometryWeight);
        sum = dpadd(sum, dpmul(fptodp(parts[kEditorLoadTextures]), kTexturesWeight));
        sum = dpadd(sum, dpmul(fptodp(parts[kEditorLoadData]), kSmallFileWeight));
        sum = dpadd(sum, dpmul(fptodp(parts[kEditorLoadInstances]), kSmallFileWeight));
        sum = dpadd(sum, dpmul(fptodp(parts[kEditorLoadFx]), kFxWeight));
        sum = dpadd(sum, dpmul(fptodp(parts[kEditorLoadFx]), kLightsWeight));
        ee::Float done = dptofp(sum);
        if (dpcmp(fptodp(done), kWholeBar) > 0)
            done = 1.0f;
        const uint32_t width = dptoul(dpmul(fptodp(done * kBarWidth), kBarStretch));
        zge::clearDisplayRectangle(kBarX, kBarY, width, kBarHeight, ee::Ptr<uint8_t>(sym::editorLoadBarColour));
    }
    READABLE_FUNCTION(editorLoadProgress, Verified);

    void editMapClearAll()
    {
        const ee::Ptr<ParkEditor> editor = editorRecord();
        for (uint32_t row = 0; row < editor->rows; ++row)
        {
            for (uint32_t column = 0; column < editor->columns; ++column)
            {
                const uint8_t cellColumn = static_cast<uint8_t>(column), cellRow = static_cast<uint8_t>(row);
                if (cellHasRail(editor, cellColumn, cellRow))
                    removeCellRails(editor, cellColumn, cellRow);
                const ee::Ptr<ParkCell> cell = parkCell(editor, static_cast<int32_t>(column), static_cast<int32_t>(row));
                // An object goes with its own cell, its cost given back.
                const ee::Ptr<PlacedObject> object = cell->object;
                if (object && object->column == column && object->row == row)
                {
                    removeNodeLights(object->node);
                    *g_parkCapacityUsed = *g_parkCapacityUsed - zge::unsignedToFloat(placedObjectCost(cell->object));
                    freePlacedObject(&cell->object);
                    cell->object = ee::Ptr<PlacedObject>();
                }
                cell->level = static_cast<int8_t>(themeCellHeight(editor->theme, cellColumn, cellRow));
                cell->object = ee::Ptr<PlacedObject>();
                cell->column = cellColumn;
                cell->row = cellRow;
            }
        }
        fitThemeTowers(editor);
        resetGapTool(editor);
        game::tail(removePlacedStart, editor);
    }
    READABLE_FUNCTION(editMapClearAll, Verified);

    uint32_t editorStateEnter()
    {
        *g_editorEntering = 1;
        registerEditCoreFunctions();
        switch (*g_parkEntryType)
        {
        case kOpenLoadedPark: openLoadedPark(); break;
        case kOpenNewPark: openNewPark(); break;
        case kReturnFromTest: returnToParkEditor(); break;
        default: break;
        }
        zge::zgui::runScriptLines(*zge::zgui::g_root, ee::Ptr<char>(kEditorStartScript));
        *g_editorEntering = 0;
        return 1;
    }
    READABLE_FUNCTION(editorStateEnter, Verified);

    uint32_t editorStateExit()
    {
        shutDownEditCore();
        return game::tail(zge::dmmCompact, zgui::heapRegion(zge::zgui::g_zgui->heap));
    }
    READABLE_FUNCTION(editorStateExit, Verified);

    uint32_t editorStateFrame()
    {
        stepPulse();
        if (*g_relightRunning)
        {
            continueRelight();
            return 1;
        }
        const ee::Ptr<ParkEditor> editor = editorRecord();
        if (!editor->userLocked)
        {
            updateEditorCore(editor);
            updateEditView(editor, kFrameSeconds);
        }
        showEditToolTexts(editor);
        switch (editor->tool)
        {
        case kEditToolTerrain:
        case kEditToolPiece: updateHeldObject(editor); break;
        case kEditToolRail: updateRailPreview(editor); break;
        case kEditToolStart: moveStartMarkerToCursor(editor); break;
        case kEditToolGap: moveGapMarkerToCursor(editor); break;
        case kEditToolCopy: showCopiedCellsAtCursor(editor); break;
        case kEditToolCardFrame: uiFrameUpdate(); break;
        default: break;
        }
        advanceEffectsClock();
        markCursorPieces(editor);
        tintMarkedPieces(editor);
        return 1;
    }
    READABLE_FUNCTION(editorStateFrame, Verified);
}
