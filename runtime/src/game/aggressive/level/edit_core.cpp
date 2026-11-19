// The park editor's core (g_parkEditorCore): the park project's themes, the
// park, its piece catalog and the editor's tools; building a park from saved
// data or opening it in the editor; saving and loading it; the editor's
// script functions (EditCoreEnterTest and the rest) and the callbacks the game
// sets for them.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_core.cpp.

#include "aggressive/level/edit_core.h"
#include "zge/editor/edit_core.h"
#include "aggressive/conversions.h"
#include "aggressive/level/edit_records.h"
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
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/edit_tools.h"
#include "aggressive/level/park_ui.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_core.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        inline ee::Global<ee::Ptr<EditCore>, sym::g_parkEditorCore> g_editCore;
        // The selection a park only played is placed with (buildParkLevel).
        inline ee::Global<uint32_t, sym::g_parkLevelObject> g_parkSelection;
        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;

        // The screen sizes: the editor's, and its high resolution one.
        constexpr uint32_t kScreenWidth = 0x200, kHighResolutionWidth = 0x280, kScreenHeight = 0x1C0;

        // A saved piece wider than a cell goes through the selection.
        constexpr uint32_t kCellPiece = 2;
    }

    namespace
    {
        namespace tail
        {
            int32_t freeBreakables() { return ee::tailCall<int32_t>(sym::freeBreakables); }
        }
    }

    int32_t setParkEditorViewport(uint32_t width, uint32_t height);
    uint32_t parkProjectTheme(ee::Ptr<ParkProject> project, uint32_t index);

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
    // of its own places the pieces.
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
        resetParkCells((*g_editCore)->park);
        game::tail(setParkEditorViewport, kScreenWidth, kScreenHeight);
    }
    READABLE_FUNCTION_WAITS(buildParkLevel, Verified);

    // The editor opened on a theme's park: its tools made (with the UI loaded
    // or not), the park's cells built.
    void openParkEditor(uint32_t themeIndex, uint32_t uiLoaded)
    {
        prepareParkTheme(themeIndex);
        const ee::Ptr<EditCore> core = *g_editCore;
        if (core->toolSet == 0)
            core->toolSet = newEditToolSet(core->theme, core->park, core->pieceCatalog, uiLoaded);
        buildParkCells(core->park, core->pieceCatalog);
        if (themeHighResolution(core->theme) != 0)
            setParkEditorViewport(kHighResolutionWidth, kScreenHeight);
        core->clearedOnOpen = 0;
    }
    READABLE_FUNCTION_WAITS(openParkEditor, Verified);

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
        return game::tail(zge::zgui::registerScriptFunction, sym::g_textEditCoreClear, sym::editCoreClear);
    }
    READABLE_FUNCTION_WAITS(registerEditCoreFunctions, Verified);

    // The callbacks, by event (the quit callback's slot is the last).
    
    namespace
    {
        ee::Ptr<EditCoreCallback> callbackSlot(uint32_t event) { return ee::Ptr<EditCoreCallback>(&(*g_editCore)->callbacks[0]) + event; }
    }

    // Draft: never called.
    void clearEditCoreCallback(EditCoreEvent event)
    {
        *callbackSlot(event) = EditCoreCallback();
    }
    READABLE_FUNCTION(clearEditCoreCallback, Draft);

    // The pieces the in-game editor placed before, back in the park.
    void showEditCorePark()
    {
        const ee::Ptr<EditCore> core = *g_editCore;
        game::tail(restoreBreakables, core->park, core->pieceCatalog);
    }
    READABLE_FUNCTION_WAITS(showEditCorePark, Verified);

    // A loaded piece put in place on `anchor`: one cell across and deep goes
    // straight on the anchor, a bigger one through the selection (the
    // editor's, or the played park's own).
    void placeEditPiece(uint32_t anchor, ee::Ptr<EditObjectPart> object)
    {
        ee::Local<int32_t[3]> cells; // across, up, deep (as a CellIndex holds them)
        const uint32_t piece = editObjectDef(object);
        uint32_t selection = *g_parkSelection;
        if ((*g_editCore)->toolSet != 0)
            selection = toolSetSelection((*g_editCore)->toolSet);
        pieceDefFootprint(piece, &cells[0], &cells[1], &cells[2]);
        if (static_cast<uint32_t>(cells[0]) < kCellPiece && static_cast<uint32_t>(cells[2]) < kCellPiece)
            attachToAnchor(anchor, object);
        else
            placePieceWithSelection(selection, object, anchor);
    }
    READABLE_FUNCTION_WAITS(placeEditPiece, Verified);
}
