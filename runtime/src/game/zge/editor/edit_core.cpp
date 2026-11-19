// The park editor's core (g_parkEditorCore): the park project's themes, the
// park, its piece catalog and the editor's tools; building a park from saved
// data or opening it in the editor; saving and loading it; the editor's
// script functions (EditCoreEnterTest and the rest) and the callbacks the game
// sets for them.

#include "zge/editor/edit_core.h"
#include "zge/editor/game.h"
#include "aggressive/conversions.h"
#include "zge/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/memory/heaps.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/render/display.h"
#include "zge/zgui/script.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_display.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/edit_tools.h"
#include "zge/editor/park_ui.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_core.cpp.h"

namespace zge::editor
{
    namespace
    {
        inline ee::Global<ee::Ptr<EditCore>, sym::g_parkEditorCore> g_editCore;
        // The selection a park only played is placed with (buildParkLevel).
        inline ee::Global<uint32_t, sym::g_parkLevelObject> g_parkSelection;
        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;
        inline ee::Global<ee::Float, sym::g_defaultFieldOfView> g_defaultHorizontalScale;
        inline ee::Global<ee::Float, sym::g_defaultAspect> g_defaultVerticalScale;

        // The screen sizes: the editor's, and its high resolution one.
        constexpr uint32_t kScreenWidth = 0x200, kHighResolutionWidth = 0x280, kScreenHeight = 0x1C0;
    }

    namespace
    {
        // The UI camera's scene: its camera.
        ee::Ptr<zge::Camera> uiCameraScene() { return zge::sceneOf(ee::Ptr<zge::SceneObject>(*g_uiCamera)).cast<zge::Camera>(); }
        namespace tail
        {
            int32_t freeBreakables() { return ee::tailCall<int32_t>(sym::freeBreakables); }
        }
    }

    void runEditCoreCallback(EditCoreEvent event);
    int32_t setParkEditorViewport(uint32_t width, uint32_t height);
    void freeParkProject(ee::Ptr<ParkProject> project);
    void readParkProject(ee::Ptr<ParkProject> project, uint32_t path);
    uint32_t parkProjectTheme(ee::Ptr<ParkProject> project, uint32_t index);
    ee::Ptr<ParkProject> newParkProject(uint32_t path);
    void clearEditCoreCallbacks();

    // The core made: the edit classes registered, the project read from
    // `path`, the view made; no theme, park, catalog or tools yet.
    void startEditCore(uint32_t path)
    {
        resetEditAllocations();
        initEditClasses();
        registerEditSelectionClass();
        registerEditAnchorClass();
        registerEditObjectClasses();
        registerEditToolClass();
        *g_editCore = ee::Ptr<EditCore>(editAllocate(sizeof(EditCore)));
        (*g_editCore)->project = newParkProject(path);
        const uint32_t view = static_cast<uint32_t>(newEditView());
        const ee::Ptr<EditCore> core = *g_editCore;
        core->started = 1;
        core->editView = view;
        core->themeIndex = -1;
        core->toolSet = 0;
        core->theme = 0;
        core->park = 0;
        core->pieceCatalog = 0;
        game::tail(clearEditCoreCallbacks);
    }
    READABLE_FUNCTION_WAITS(startEditCore, Verified);

    // The core freed with all it holds (a park built stays in the level).
    void endEditCore()
    {
        zge::reportMemoryStats();
        if (*g_parkSelection != 0)
        {
            deleteEditObject(*g_parkSelection);
            *g_parkSelection = 0;
        }
        const ee::Ptr<EditCore> core = *g_editCore;
        if (core->toolSet != 0)
        {
            deleteEditor(core->toolSet);
            core->toolSet = 0;
        }
        if (core->park != 0)
        {
            freeEditPark(core->park);
            core->park = 0;
        }
        if (core->pieceCatalog != 0)
        {
            freePieceCatalog(core->pieceCatalog);
            core->pieceCatalog = 0;
        }
        if (core->project)
        {
            freeParkProject(core->project);
            core->project = ee::Ptr<ParkProject>();
        }
        if (core->editView != 0)
        {
            freeEditView(core->editView);
            core->editView = 0;
        }
        hideTowerDisplay();
        editFree(core);
        *g_editCore = ee::Ptr<EditCore>();
        freeEditClasses();
        game::tail(checkEditAllocations);
    }
    READABLE_FUNCTION_WAITS(endEditCore, Verified);

    // The editor's frame: its tools, then the flashing of what is selected.
    void updateEditCore(ee::Float seconds)
    {
        const ee::Ptr<EditCore> core = *g_editCore;
        if (!core)
            return;
        if (core->toolSet != 0)
            updateEditor(core->toolSet, seconds);
        game::tail(updateEditFlash, seconds);
    }
    READABLE_FUNCTION_WAITS(updateEditCore, Verified);

    // Every cell of the park emptied.
    void clearEditCorePark()
    {
        game::tail(deleteParkPieces, (*g_editCore)->park);
    }
    READABLE_FUNCTION_WAITS(clearEditCorePark, Verified);

    // The script functions.
    void editCoreEnterEdit()
    {
        game::tail(runEditCoreCallback, kEditCoreEditStarted);
    }
    READABLE_FUNCTION_WAITS(editCoreEnterEdit, Verified);

    // Editing over: the core freed, then the game's quit callback.
    void editCoreExitEdit()
    {
        runEditCoreCallback(kEditCoreEditEnded);
        const ee::FunctionPtr<void()> onQuit = (*g_editCore)->onQuit;
        endEditCore();
        if (onQuit)
            onQuit();
    }
    READABLE_FUNCTION_WAITS(editCoreExitEdit, Verified);

    // A test run: its tool on top of the editor's.
    void editCoreEnterTest()
    {
        const uint32_t testRun = newEditObject(sym::g_editCoreEditTestRunToolName, 0); // "EditTestRunTool"
        pushEditorTool((*g_editCore)->toolSet, editObjectPart(testRun, sym::g_editCoreEditToolName)); // "EditTool"
        game::tail(runEditCoreCallback, kEditCoreTestStarted);
    }
    READABLE_FUNCTION_WAITS(editCoreEnterTest, Verified);

    void editCoreExitTest()
    {
        deleteEditObject(popEditorTool((*g_editCore)->toolSet));
        if (themeHighResolution((*g_editCore)->theme) != 0)
            setParkEditorViewport(kHighResolutionWidth, kScreenHeight);
        game::tail(runEditCoreCallback, kEditCoreTestEnded);
    }
    READABLE_FUNCTION_WAITS(editCoreExitTest, Verified);

    void editCoreEnterPlay()
    {
        game::tail(runEditCoreCallback, kEditCorePlayStarted);
    }
    READABLE_FUNCTION_WAITS(editCoreEnterPlay, Draft);

    void editCoreExitPlay()
    {
        game::tail(runEditCoreCallback, kEditCorePlayEnded);
    }
    READABLE_FUNCTION_WAITS(editCoreExitPlay, Draft);

    // Paused: the editor's help page goes.
    void editCoreEnterPause()
    {
        runEditCoreCallback(kEditCorePaused);
        game::tail(hideEditHelpPage, toolSetUi((*g_editCore)->toolSet), 0);
    }
    READABLE_FUNCTION_WAITS(editCoreEnterPause, Verified);

    void editCoreExitPause()
    {
        runEditCoreCallback(kEditCoreResumed);
        game::tail(showEditHelpPage, toolSetUi((*g_editCore)->toolSet), 0);
    }
    READABLE_FUNCTION_WAITS(editCoreExitPause, Verified);

    void editCoreLoad()
    {
        game::tail(runEditCoreCallback, kEditCoreLoading);
    }
    READABLE_FUNCTION_WAITS(editCoreLoad, Verified);

    void editCoreSave()
    {
        game::tail(runEditCoreCallback, kEditCoreSaving);
    }
    READABLE_FUNCTION_WAITS(editCoreSave, Verified);

    // The park emptied, and the first tool chosen.
    void editCoreClear()
    {
        clearParkPieces((*g_editCore)->park);
        game::tail(selectFirstTool, (*g_editCore)->toolSet);
    }
    READABLE_FUNCTION_WAITS(editCoreClear, Verified);

    // The callbacks, by event (the quit callback's slot is the last).
    
    namespace
    {
        ee::Ptr<EditCoreCallback> callbackSlot(uint32_t event) { return ee::Ptr<EditCoreCallback>(&(*g_editCore)->callbacks[0]) + event; }
    }

    void clearEditCoreCallbacks()
    {
        for (uint32_t event = 0; event < kEditCoreEvents; ++event)
            *callbackSlot(event) = EditCoreCallback();
    }
    READABLE_FUNCTION(clearEditCoreCallbacks, Verified);

    void runEditCoreCallback(EditCoreEvent event)
    {
        const EditCoreCallback callback = *callbackSlot(event);
        if (callback)
            callback(event * 4);
    }
    READABLE_FUNCTION(runEditCoreCallback, Verified);

    void setEditCoreCallback(EditCoreEvent event, EditCoreCallback callback)
    {
        *callbackSlot(event) = callback;
    }
    READABLE_FUNCTION(setEditCoreCallback, Verified);

    // The in-game editor's placed pieces kept for the level.
    void hideEditCorePark()
    {
        game::tail(collectBreakables, (*g_editCore)->park);
    }
    READABLE_FUNCTION_WAITS(hideEditCorePark, Verified);

    namespace
    {
        // setCameraScales: the camera's projection scales. By address: the caller returns
        // what the (void) function leaves in the result register.
        int32_t setCameraScalesKeepingResult(ee::Ptr<zge::Camera> camera, ee::Float horizontal, ee::Float vertical) { return ee::game<int32_t>(sym::setCameraScales, camera, horizontal, vertical); }
    }

    // The screen size for the editor: the cameras' viewports, and the UI
    // camera's rectangle and default scales.
    int32_t setParkEditorViewport(uint32_t width, uint32_t height)
    {
        ee::Local<ViewportRectangle> rectangle;
        rectangle->x = ee::Float(0.0f);
        rectangle->y = ee::Float(0.0f);
        rectangle->width = unsignedToFloat(width);
        rectangle->height = unsignedToFloat(height);
        zge::resetCameraViewports(width, height);
        zge::setCameraViewport(uiCameraScene(), rectangle);
        return setCameraScalesKeepingResult(uiCameraScene(), *g_defaultHorizontalScale, *g_defaultVerticalScale);
    }
    READABLE_FUNCTION_WAITS(setParkEditorViewport, Verified);

    // Where the park's "StartObject" stands and how it turns; 0 when the park
    // has none.
    int32_t parkStartPosition(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::EulerAngles> angles)
    {
        const uint32_t start = findPieceDef((*g_editCore)->pieceCatalog, sym::g_textStartObject);
        const ee::Ptr<EditObjectPart> piece = findParkPiece((*g_editCore)->park, start);
        if (piece == 0)
            return 0;
        const ee::Ptr<zge::Matrix> matrix(editObjectMatrix(piece));
        *position = matrix->translation();
        zge::matrixToEuler(angles, matrix);
        return 1;
    }
    READABLE_FUNCTION_WAITS(parkStartPosition, Verified);

    // The park saved into `data`: its theme, the two saved blocks, the view,
    // the pieces, the tools' camera.
    void saveParkData(uint32_t data, uint32_t bytes)
    {
        const uint32_t stream = openParkStream(data, bytes, 1);
        parkStreamWrite(stream, &(*g_editCore)->themeIndex, sizeof(int32_t));
        parkStreamWrite(stream, (*g_editCore)->author, sizeof(EditCore::author));
        parkStreamWrite(stream, (*g_editCore)->name, sizeof(EditCore::name));
        saveEditView((*g_editCore)->editView, stream);
        saveEditPark((*g_editCore)->park, stream);
        const ee::Ptr<EditToolSet> toolSet = (*g_editCore)->toolSet;
        if (toolSet != 0)
            saveToolSetCamera(toolSet, stream);
        game::tail(closeParkStream, stream);
    }
    READABLE_FUNCTION_WAITS(saveParkData, Verified);

    // The park loaded from `data` (the saved theme is read past: the park's
    // theme is the one already chosen), the tool in use put away meanwhile.
    void loadParkData(uint32_t data, uint32_t bytes)
    {
        ee::Local<int32_t> savedTheme;
        if ((*g_editCore)->toolSet != 0)
            deactivateEditorTool((*g_editCore)->toolSet);
        const uint32_t stream = openParkStream(data, bytes, 0);
        parkStreamRead(stream, savedTheme.address(), sizeof(int32_t));
        parkStreamRead(stream, (*g_editCore)->author, sizeof(EditCore::author));
        parkStreamRead(stream, (*g_editCore)->name, sizeof(EditCore::name));
        loadEditView((*g_editCore)->editView, stream);
        buildParkCells((*g_editCore)->park, (*g_editCore)->pieceCatalog);
        loadEditPark((*g_editCore)->park, stream);
        if ((*g_editCore)->toolSet != 0)
        {
            loadToolSetCamera((*g_editCore)->toolSet, stream);
            activateEditorTool((*g_editCore)->toolSet);
        }
        closeParkStream(stream);
    }
    READABLE_FUNCTION_WAITS(loadParkData, Verified);

    // A saved piece read from the stream: its catalog index, the object made
    // and placed, then its own data.
    void readEditPiece(uint32_t anchor, uint32_t stream)
    {
        ee::Local<uint16_t> index;
        parkStreamRead(stream, index.address(), sizeof(uint16_t));
        const uint32_t piece = catalogPieceDef((*g_editCore)->pieceCatalog, *index);
        const ee::Ptr<EditObjectPart> object(newEditObject(static_cast<uint32_t>(pieceDefClassName(piece)), piece));
        makeEditObjectDisplay(object, 0);
        placeEditPiece(anchor, object);
        loadEditObject(object, stream);
    }
    READABLE_FUNCTION_WAITS(readEditPiece, Verified);

    int32_t editCoreTheme()
    {
        return (*g_editCore)->themeIndex;
    }
    READABLE_FUNCTION(editCoreTheme, Verified);

    // The project read from `path` (no themes before).
    ee::Ptr<ParkProject> newParkProject(uint32_t path)
    {
        const ee::Ptr<ParkProject> project(editAllocate(sizeof(ParkProject)));
        project->themeCount = 0;
        project->version[0] = '\0';
        for (uint32_t i = 0; i < kMaxParkThemes; ++i)
            project->themes[i] = 0;
        readParkProject(project, path);
        return project;
    }
    READABLE_FUNCTION_WAITS(newParkProject, Verified);

    uint32_t parkProjectTheme(ee::Ptr<ParkProject> project, uint32_t index)
    {
        return project->themes[index];
    }
    READABLE_FUNCTION(parkProjectTheme, Verified);

    void freeParkProject(ee::Ptr<ParkProject> project)
    {
        for (uint32_t i = 0; i < project->themeCount; ++i)
        {
            freeParkTheme(project->themes[i]);
            project->themes[i] = 0;
        }
        project->themeCount = 0;
        game::tail(editFree, project);
    }
    READABLE_FUNCTION_WAITS(freeParkProject, Verified);

    // The project file read whole into memory and read as a script: "Version"
    // and its value, each "ThemeDefBegin" a theme, up to
    // "ParkEditorProjectFileEnd".
    void readParkProject(ee::Ptr<ParkProject> project, uint32_t path)
    {
        ee::Local<char[0x100]> token;
        ee::Local<uint32_t> fileBytes;
        const ee::Ptr<zge::ZFile> file = zge::openFile(path, 1, 0).file();
        zge::fileSize(file, fileBytes.address(), 0);
        const uint32_t text = editAllocate(*fileBytes);
        zge::fileRead(file, text, *fileBytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memoryFile = zge::openMemoryFile(text, *fileBytes);
        zge::beginScript(zge::filePath(memoryFile));
        zge::setTokenCapitalize(0);
        zge::nextToken(token.address(), sizeof(char[0x100]));
        do
        {
            zge::nextToken(token.address(), sizeof(char[0x100]));
            if (zge::stringCompareNoCase(token.address(), sym::g_textVersion) == 0)
                zge::nextToken(project->version, sizeof(ParkProject::version));
            else if (zge::stringCompareNoCase(sym::g_textThemeDefBegin, token.address()) == 0)
            {
                project->themes[project->themeCount] = static_cast<uint32_t>(newParkTheme());
                readParkThemeDef(project->themes[project->themeCount]);
                project->themeCount = project->themeCount + 1;
            }
        } while (zge::stringCompareNoCase(token.address(), sym::g_textParkEditorProjectFileEnd) != 0);
        zge::endScript();
        zge::closeFile(memoryFile, 0);
        editFree(text);
    }
    READABLE_FUNCTION_WAITS(readParkProject, Verified);

    // The level's breakables freed with the editor's.
    int32_t cleanEditCore()
    {
        return tail::freeBreakables();
    }
    READABLE_FUNCTION_WAITS(cleanEditCore, Verified);
}
