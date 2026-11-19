// The park editor's core (g_parkEditorCore): the park project's themes, the
// park, its piece catalog and the editor's tools; building a park from saved
// data or opening it in the editor; saving and loading it; the editor's
// script functions (EditCoreEnterTest and the rest) and the callbacks the game
// sets for them.
// BMX XXX's own part of it; what both games have is zge/editor/edit_core.cpp.

#include "bmx/editor/edit_core.h"
#include "zge/editor/edit_core.h"
#include "bmx/editor/conversions.h"
#include "bmx/editor/edit_records.h"
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
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_display.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/edit_tools.h"
#include "bmx/editor/park_ui.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_core.cpp.h"

namespace zge::editor
{
    namespace
    {
        // A piece of fewer cells than this, across and deep, covers one cell.
        constexpr uint32_t kCellPiece = 2;

        inline ee::Global<ee::Ptr<EditCore>, sym::g_parkEditorCore> g_editCore;
        inline ee::Global<uint32_t, sym::g_parkLevelObject> g_parkSelection;
        // The selection a park only played is placed with (buildParkLevel).
        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;

        // The screen sizes: the editor's, and its high resolution one.
        constexpr uint32_t kHighResolutionWidth = 0x280, kScreenHeight = 0x1C0;
    }

    namespace
    {
        namespace tail
        {
            int32_t freeBreakables() { return ee::tailCall<int32_t>(sym::freeBreakables); }
        }
    }

    void runEditCoreCallback(EditCoreEvent event);
    int32_t setParkEditorViewport(uint32_t width, uint32_t height);
    uint32_t parkProjectTheme(ee::Ptr<ParkProject> project, uint32_t index);

    // The script's "editCoreLoadPreset": its argument, a preset park's name, becomes the
    // park's name, and the game's loading-preset callback runs.
    void editCoreLoadPreset(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments)
    {
        zge::zgui::variableToText(arguments[0], ee::Ptr<char>(&(*g_editCore)->name[0]));
        game::tail(runEditCoreCallback, kEditCoreLoadingPreset);
    }
    READABLE_FUNCTION_WAITS(editCoreLoadPreset, Draft);

    uint32_t registerEditCoreFunctions()
    {
        zge::zgui::registerScriptFunction(sym::g_textEditCoreEnterTest, sym::editCoreEnterTest);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreExitTest, sym::editCoreExitTest);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreEnterPlay, sym::editCoreEnterPlay);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreExitPlay, sym::editCoreExitPlay);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreEnterEdit, sym::editCoreEnterEdit);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreExitEdit, sym::editCoreExitEdit);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreEnterPause, sym::editCoreEnterPause);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreExitPause, sym::editCoreExitPause);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreSave, sym::editCoreSave);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreLoad, sym::editCoreLoad);
        zge::zgui::registerScriptFunction(sym::g_textEditCoreLoadPreset, sym::editCoreLoadPreset);
        return game::tail(zge::zgui::registerScriptFunction, sym::g_textEditCoreClear, sym::editCoreClear);
    }
    READABLE_FUNCTION_WAITS(registerEditCoreFunctions, Verified);

    // The park shown for the editor again: its dynamics get their displays anew.
    void showEditCorePark()
    {
        const ee::Ptr<EditCore> core = *g_editCore;
        game::tail(remakeParkDynamics, core->park, core->pieceCatalog);
    }
    READABLE_FUNCTION_WAITS(showEditCorePark, Verified);

    // A piece put on an anchor. On a tower's anchor it goes through the selection, which
    // places it over every cell it covers; on any other anchor it is simply attached.
    // (The original asks twice whether the anchor is a tower's, the second time only for a
    // piece one cell across and deep, and would attach such a piece if the answers differed.)
    void placeEditPiece(uint32_t anchor, ee::Ptr<EditObjectPart> object)
    {
        ee::Local<int32_t[3]> cells; // across, up, deep (as a CellIndex holds them)
        const uint32_t piece = editObjectDef(object);
        uint32_t selection = *g_parkSelection;
        if ((*g_editCore)->toolSet != 0)
            selection = toolSetSelection((*g_editCore)->toolSet);
        pieceDefFootprint(piece, &cells[0], &cells[1], &cells[2]);
        if (zge::compareStrings(editObjectClassName(anchorOwner(anchor)), sym::g_placePieceEditTowerObjectName) != 0)
        {
            attachToAnchor(anchor, object);
            return;
        }
        if (static_cast<uint32_t>(cells[0]) < kCellPiece && static_cast<uint32_t>(cells[2]) < kCellPiece &&
            zge::compareStrings(editObjectClassName(anchorOwner(anchor)), sym::g_placePieceEditTowerObjectName) != 0)
            attachToAnchor(anchor, object);
        else
            placePieceWithSelection(selection, object, anchor);
    }
    READABLE_FUNCTION_WAITS(placeEditPiece, Draft);

    namespace
    {
        // The theme chosen, and the catalog and the park made for it if they
        // are not yet: what building a park and opening the editor share.
        void prepareParkTheme(uint32_t themeIndex)
        {
            const ee::Ptr<EditCore> core = *g_editCore;
            if (core->themeIndex != static_cast<int32_t>(themeIndex))
            {
                core->theme = parkProjectTheme(core->project, themeIndex);
                core->themeIndex = static_cast<int32_t>(themeIndex);
            }
            if (core->pieceCatalog == 0)
            {
                core->pieceCatalog = newPieceCatalog();
                addPieceSceneFile(core->pieceCatalog, static_cast<uint32_t>(themeObjectGeomFileName(core->theme)));
                readPieceFile(core->pieceCatalog, static_cast<uint32_t>(themeObjectDetailFileName(core->theme)));
            }
            if (core->park == 0)
            {
                core->park = newEditPark();
                readParkMap(core->park, static_cast<uint32_t>(themeMapFileName(core->theme)));
                measurePieceDefs(core->pieceCatalog, core->park);
            }
        }
    }

    // A saved park of a theme built for play: without the editor, a selection
    // of its own places the pieces. (BMX XXX leaves the viewport as it is.)
    void buildParkLevel(uint32_t themeIndex, uint32_t data, uint32_t bytes)
    {
        prepareParkTheme(themeIndex);
        if ((*g_editCore)->toolSet == 0)
        {
            const uint32_t selection = newEditObject(sym::g_editCoreEditSelectionName, 0); // "EditSelection"
            *g_parkSelection = selection;
            attachSelectionToPark(selection, (*g_editCore)->park);
        }
        loadParkData(data, bytes);
        game::tail(resetParkCells, (*g_editCore)->park);
    }
    READABLE_FUNCTION_WAITS(buildParkLevel, Verified);

    // The editor opened on a theme's park: the park's cells built, then its tools made
    // (with the UI loaded or not).
    void openParkEditor(uint32_t themeIndex, uint32_t uiLoaded)
    {
        prepareParkTheme(themeIndex);
        buildParkCells((*g_editCore)->park, (*g_editCore)->pieceCatalog);
        const ee::Ptr<EditCore> core = *g_editCore;
        if (core->toolSet == 0)
            core->toolSet = newEditToolSet(core->theme, core->park, core->pieceCatalog, uiLoaded);
        if (themeHighResolution(core->theme) != 0)
            setParkEditorViewport(kHighResolutionWidth, kScreenHeight);
        (*g_editCore)->clearedOnOpen = 0;
    }
    READABLE_FUNCTION_WAITS(openParkEditor, Verified);

    // The editor's camera takes the UI's scene camera as it is now (BMX XXX).
    void editCoreUseUiCamera()
    {
        game::tail(toolSetUseUiCamera, (*g_editCore)->toolSet);
    }
    READABLE_FUNCTION(editCoreUseUiCamera, Verified);

    // The park's start object: the piece of the catalog's "StartObject" in the park, 0 for none.
    ee::Ptr<EditObjectPart> findParkStartObject()
    {
        const uint32_t piece = findPieceDef((*g_editCore)->pieceCatalog, sym::g_textStartObject);
        return findParkPiece((*g_editCore)->park, piece);
    }
    READABLE_FUNCTION_WAITS(findParkStartObject, Verified);
}
