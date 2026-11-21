#pragma once

// The park editor's object classes, its allocations and its themes
// (park_classes.cpp).

#include "zge/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (park_classes.cpp)
    void checkEditAllocations();
    int32_t deleteEditObject(ee::Address data);
    uint32_t editAllocate(uint32_t bytes);
    uint32_t editAllocationCount();
    int32_t editFree(ee::Address block);
    ee::Ptr<const char> editObjectClassName(ee::Address data);
    uint32_t editObjectPart(ee::Address data, uint32_t className);
    int32_t freeEditClasses();
    void freeParkTheme(uint32_t theme);
    void initEditClasses();
    uint32_t newEditObject(uint32_t className, ee::Address pieceDef);
    int32_t newParkTheme();
    void readParkThemeDef(uint32_t address);
    int32_t registerEditClass(uint32_t className, uint32_t dataBytes, uint32_t parentName, uint32_t construct, uint32_t destruct, uint32_t message);
    void resetEditAllocations();
    void sendEditMessage(uint32_t data, ee::Address message);
    uint32_t themeHighResolution(uint32_t theme);
    int32_t themeMapFileName(uint32_t theme);
    int32_t themeObjectDetailFileName(uint32_t theme);
    int32_t themeObjectGeomFileName(uint32_t theme);
    int32_t themeStartScript(uint32_t theme);
    uint32_t themeToolCount(uint32_t theme);
    int32_t themeToolName(uint32_t theme, uint32_t index);
    int32_t themeUiScript(uint32_t theme);

    constexpr uint32_t kEditClasses = 32;
    struct EditClass
    {
        uint32_t used;        // +0x00
        char name[64];        // +0x04
        uint32_t index;       // +0x44 in the table
        uint32_t parent;      // +0x48 its index (0: the root: no part of its own)
        uint32_t partBytes;   // +0x4C its data, aligned, and the part's header
        uint32_t live;        // +0x50 objects made and not yet deleted
        uint32_t objectBytes; // +0x54 its part and its ancestors'
        ee::FunctionPtr<void(uint32_t, uint32_t)> construct; // +0x58 (data, piece definition), root first
        ee::FunctionPtr<void(uint32_t)> destruct;            // +0x5C (data), its own first
        ee::FunctionPtr<void(uint32_t, uint32_t)> message;   // +0x60 (data, message)
    };
    static_assert(sizeof(EditClass) == 0x64);
    struct EditClassTable
    {
        uint32_t ready;       // +0x00
        EditClass classes[kEditClasses]; // +0x04
    };
    static_assert(sizeof(EditClassTable) == 3204);

    // A part's header, just before its data.
    struct EditPart
    {
        ee::Ptr<EditPart> object; // +0x00 the object's first part: its allocation
        ee::Ptr<EditPart> next;   // +0x04 its parent class's part
        ee::Ptr<EditClass> type;  // +0x08
        uint32_t data;            // +0x0C
    };
    static_assert(sizeof(EditPart) == 0x10);

    // A park theme (ThemeDefBegin).
    constexpr uint32_t kThemeTools = 32;
    struct ParkTheme
    {
        char name[0x20];               // +0x000 "Name"
        char description[0x80];        // +0x020 "Description"
        char mapFile[0x40];            // +0x0A0 "MapFileName"
        char objectDetailFile[0x40];   // +0x0E0 "ObjectDetailFileName"
        char objectGeomFile[0x40];     // +0x120 "ObjectGeomFileName"
        char apronFile[0x40];          // +0x160 "ApronFileName"
        char uiScript[0x80];           // +0x1A0 "UIScript"
        char toolNames[kThemeTools][0x20]; // +0x220 "ToolName"s
        uint32_t toolCount;            // +0x620
        char startScript[0x400];       // +0x624 "StartScript"
        uint32_t startTool;            // +0xA24 "StartTool"
        uint32_t flagA28;              // +0xA28 0: no text sets it
        uint32_t highResolution;       // +0xA2C "HighResolution" 1 (1 by default)
        uint32_t lightSystem;          // +0xA30 "LightSystem" 1 (1 by default)
        uint32_t _a34;
    };
    static_assert(sizeof(ParkTheme) == 2616 && offsetof(ParkTheme, toolCount) == 0x620 && offsetof(ParkTheme, lightSystem) == 0xA30);

    // (park_classes.cpp, for the games' own parts)
    uint32_t alignTo16(uint32_t bytes);
    void constructEditObjectParts(ee::Address partAddress, uint32_t pieceDef);
    void destructEditObjectParts(ee::Address first);
    int32_t editPartData(ee::Address part);
    int32_t editPartHeader(uint32_t data);
    uint32_t findEditClass(uint32_t className, ee::Ptr<uint32_t> index);
    uint32_t freeEditClassSlot();
    int32_t inheritEditClass(ee::Address added, ee::Address parent);
    void initEditObjectParts(uint32_t object, uint32_t index);
}
