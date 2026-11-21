#pragma once

// DM2's park editor as a game state (editor_state.cpp): the game's state
// table's enter, exit and frame for the editor, the three ways the editor
// opens (a new park, the park just loaded, back from a test run), and the
// entries other code reaches the editor's record and tool through.

#include "park_editor.h"

#include "ee/abi.h"

#include <cstdint>

namespace game::dm2
{
    // The editor's one record, and the tool in its hands (kEditTool...).
    ee::Ptr<ParkEditor> parkEditor();
    uint8_t editTool();
    // The tool in hand put away (the terrain tool's tape frame closed, the
    // held piece hidden, the rail preview, the start markers, the gap markers
    // or the copied cells released) and the new one taken up (the selection
    // reset for the terrain and rail tools, the piece shown, the start
    // markers made, a new gap begun).
    void setEditTool(uint8_t tool);

    // The editor's GUI has the player (the stop frame or a scroll of the
    // tool roll plays): the editor's frame moves neither cursor nor view.
    void markStopFrameOpen();
    // The script function editUnlockUser: the player has the editor again.
    void editUnlockUser();
    // The script function editGapCancel: the new gap's end taken back, the
    // gap tool at its first step again.
    void editGapCancel();

    // The cameras' viewports made the whole screen (640 by 448), the
    // editor's view camera given it with the display's default scales.
    void setUiViewport();

    // Park.zsc loaded, the view set up, the editor's script functions
    // registered, the GUI's widgets found, the park lights and the copy
    // buffers made; the player has the editor. And its counterpart: the
    // copy buffers and park lights freed, the "ParkEditor" widget deleted.
    void registerEditCoreFunctions();
    void shutDownEditCore();

    // What every way of opening the editor shares: the editor core on pad
    // 0, the themes, the GUI, the gap tags, the Terrain tool's entry, the
    // rails, the objects and the gaps set up, a first update, the first
    // tool entry chosen.
    void setUpParkEditor();
    // An empty park of the theme picked; the park read from the memory card
    // (g_parkBuffer, freed after); the editor again after a test run (the
    // park still built).
    void openNewPark();
    void openLoadedPark();
    void returnToParkEditor();
    // Two empty functions the editor's start-up and close call with the
    // editor's record.
    void editorStartStub(ee::Ptr<ParkEditor> editor);
    void editorCloseStub(ee::Ptr<ParkEditor> editor);

    // The editor's park state given back: the gaps, rails, gap tags, GUI,
    // themes, objects, tool entries, the view and the pools.
    void closeParkEditor();
    // The park in the park buffer built as the level a session rides (not a test run): the editor set up for it
    // and given back once the park's world stands.
    void enterParkLevel();
    // The script function editBackToMain: no tool, the effects and the
    // editor closed, the front end's state next.
    void editBackToMain();
    // The park's sloped towers' trees freed and the world relit (before the
    // park is ridden).
    uint32_t relightWithoutTowerTrees();
    // The script function editTestLevel: the park ridden as the park level
    // (one player, the park test), its spawn and gaps set; the level's
    // state next, its load bar starting with the park's geometry, textures
    // and data done; the editor opens again after it (kReturnFromTest).
    void editTestLevel();
    // The front end's script function BmxStartEditor: the editor's state next, its load bar's six parts at 0 and
    // editorLoadProgress the chunk reader's progress hook.
    void bmxStartEditor();
    // The script function editGapOk: the gap being made named (the
    // keyboard's EditBox) and scored (the gap menu's SelectedIndex), added
    // and registered; the player has the editor again.
    void editGapOk();
    // The script function editMapClearAll: every cell back to the theme's
    // ground, its rails and its object removed; the towers fitted again,
    // the gap tool reset, the placed start removed.
    void editMapClearAll();
    // The script function editClearRecords: the park's high-score tables
    // taken from the game's.
    void editClearRecords();
    // The script function editLoadFromPark: the park buffer's park opened in
    // the running editor.
    void editLoadFromPark();

    // A park's file buffer made (g_parkBuffer): its bytes.
    uint32_t newParkBuffer();
    // The script functions editSetModeSavePark and editSetModeLoadPark: the
    // park written to a stream and the memory card's save screen opened, or
    // the load screen opened on a new park buffer; the card frame has the
    // pad. The save screen closed: the stream freed, the pause menu shown
    // again. The load screen closed: the buffer freed, the pause menu
    // again. A park loaded: the editor's load bar cleared, the effects, the
    // gaps, objects, rails, themes and park lights given back, and
    // "PLAY Root.ParkEditor.LoadPark".
    void editSetModeSavePark();
    void editSetModeLoadPark();
    void parkSaveFrameClosed(uint32_t data, uint32_t stream);
    void parkLoadFrameClosed(uint32_t data, uint32_t dataHandle);
    void editorParkLoaded(uint32_t data, uint32_t dataHandle);

    // The chunk reader's progress hook while the editor loads: the part the
    // file belongs to brought up to date and the bar drawn as wide as the
    // parts' weighted sum.
    void editorLoadProgress(ee::Ptr<const char> path, uint32_t bytesDone, uint32_t totalBytes);

    // The editor's game state (the game's state table): entered by the way
    // g_parkEntryType says, then "PLAY Root.ParkEditor.Start"; left with
    // zgui's memory tidied; each frame: the relight while it runs, else the
    // cursor, the view, the tool texts, the tool in hand, the effects'
    // clock and the marked pieces.
    uint32_t editorStateEnter();
    uint32_t editorStateExit();
    uint32_t editorStateFrame();

    // The editor's copy of the park level's high-score tables taken from the
    // game's again (the editor's takeParkScoresAgain on the editor's record;
    // the game's clean-up calls it).
    void takeEditorParkScoresAgain();
}
