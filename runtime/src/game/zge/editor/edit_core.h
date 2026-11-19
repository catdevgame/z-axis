#pragma once

// The park editor's core (edit_core.cpp): building a park from saved data or
// opening it in the editor, saving and loading it, and the events the game
// sets a callback for (park_editor.cpp).

#include "ee/route.h"
#include "zge/assets/file.h"
#include "zge/math/matrix.h"
#include "zge/memory/heaps.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/render/display.h"
#include "zge/zgui/script.h"
#include <cstddef>
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/editor/edit_records.h"

#include <cstdint>

namespace zge::editor
{
    // The events the core runs a callback for (the game sets them:
    // park_editor.cpp), by the script function that raises them; the quit
    // callback runs after the core is freed.
    enum EditCoreEvent : uint32_t
    {
        kEditCoreTestStarted = 0, // editCoreEnterTest
        kEditCoreTestEnded = 1,   // editCoreExitTest
        kEditCoreEditStarted = 2,
        kEditCoreEditEnded = 3,   // editCoreExitEdit
        kEditCorePlayStarted = 4,
        kEditCorePlayEnded = 5,
        kEditCorePaused = 6,      // editCoreEnterPause
        kEditCoreResumed = 7,     // editCoreExitPause
        kEditCoreSaving = 8,      // editCoreSave
        kEditCoreLoading = 9,     // editCoreLoad
        kEditCoreLoadingPreset = 10, // BMX XXX's alone: its handler reads a preset park (assets\editor\presets)
        kEditCoreQuit = game::readable::perGame(10u, 10u, 11u), // the last: BMX XXX's is after its preset event
    };
    constexpr uint32_t kEditCoreEvents = kEditCoreQuit + 1;

    // A callback gets its slot's offset (the event times 4): the save
    // callback hands it on as a size.
    using EditCoreCallback = ee::FunctionPtr<void(uint32_t)>;
    int32_t cleanEditCore();
    void clearEditCorePark();
    int32_t editCoreTheme();
    void endEditCore();
    void hideEditCorePark();
    void loadParkData(uint32_t data, uint32_t bytes);
    int32_t parkStartPosition(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::EulerAngles> angles);
    void readEditPiece(uint32_t anchor, uint32_t stream);
    void saveParkData(uint32_t data, uint32_t bytes);
    void setEditCoreCallback(EditCoreEvent event, EditCoreCallback callback);
    void startEditCore(uint32_t path);
    void updateEditCore(ee::Float seconds);

    // The park project ("ParkEditorProjectFile"): its version and the
    // themes it lists.
    constexpr uint32_t kMaxParkThemes = 16;
    struct ParkProject
    {
        char version[0x80];
        uint32_t themeCount;               // +0x80
        uint32_t themes[kMaxParkThemes];   // +0x84 newParkTheme's records
    };
    static_assert(sizeof(ParkProject) == 0xC4);

    // The core (editAllocate'd, 0x90 bytes).
    struct EditCore
    {
        uint32_t started;           // +0x00 1 once made; read nowhere found
        uint32_t park;              // +0x04 the edit park
        ee::Ptr<ParkProject> project;  // +0x08
        uint32_t theme;             // +0x0C the project's theme in use
        uint32_t pieceCatalog;      // +0x10
        ee::Ptr<EditToolSet> toolSet;           // +0x14 the editor's tools (EditToolSet), 0 while a park is only played
        uint32_t editView;          // +0x18
        int32_t themeIndex;         // +0x1C -1 before a park is built or opened
        uint32_t clearedOnOpen;     // +0x20 made 0 when the editor opens; read nowhere found
        char author[0x20];          // +0x24 saved and loaded with the park (the card's save menus write the saved copy: park_save.cpp)
        char name[0x20];            // +0x44 the park's name, the same way
        EditCoreCallback callbacks[kEditCoreQuit]; // +0x64 by EditCoreEvent
        ee::FunctionPtr<void()> onQuit;    // +0x8C (BMX XXX: +0x90, after its one more event)
    };
    static_assert(sizeof(EditCore) == game::readable::perGame(0x90u, 0x90u, 0x94u) && offsetof(EditCore, themeIndex) == 0x1C);
    static_assert(offsetof(EditCore, name) == 0x44 && offsetof(EditCore, callbacks) == 0x64 && offsetof(EditCore, onQuit) == game::readable::perGame(0x8Cu, 0x8Cu, 0x90u));

    // The camera's rectangle on screen.
    struct ViewportRectangle
    {
        ee::Float x, y, width, height;
    };

    // (edit_core.cpp, for the games' own parts)
    void editCoreClear();
    void editCoreEnterEdit();
    void editCoreEnterPause();
    void editCoreEnterPlay();
    void editCoreEnterTest();
    void editCoreExitEdit();
    void editCoreExitPause();
    void editCoreExitPlay();
    void editCoreExitTest();
    void editCoreLoad();
    void editCoreSave();
    uint32_t parkProjectTheme(ee::Ptr<ParkProject> project, uint32_t index);
    int32_t setParkEditorViewport(uint32_t width, uint32_t height);
    void runEditCoreCallback(EditCoreEvent event);
}
