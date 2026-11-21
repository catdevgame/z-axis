// The park editor's object classes and its themes (were the sweep's
// blocks_001f23c8).
//
// An edit object is one allocation: a part per class from the object's
// own to the root (EditBaseClass has none), each with a header and the
// class's data. A class names its parent, so a part holds the data its
// class adds, as in C++ inheritance; the pointer handed about is the data of
// the object's own class, and the header is just before it.
//
// A theme is a text file ("ThemeDefBegin" .. "ThemeDefEnd" in the park
// project file): the park's names, its files and the tools it offers.
//
// Everything the editor allocates goes through editAllocate (zeroed, and
// counted for a leak check whose report is compiled out).
// Aggressive Inline's own part of it; what both games have is zge/editor/park_classes.cpp.

#include "aggressive/level/park_classes.h"
#include "zge/editor/park_classes.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "aggressive/skater/skater.h"
#include "aggressive/level/park_catalog.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_classes.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;

    namespace
    {
        constexpr uint32_t kEditAllocate = sym::editAllocate;
        constexpr uint32_t kEditFree = sym::editFree;
        constexpr uint32_t kAlignTo16 = sym::alignTo16;
        constexpr uint32_t kInheritEditClass = sym::inheritEditClass;
        constexpr uint32_t kInitEditObjectParts = sym::initEditObjectParts;
        constexpr uint32_t kConstructEditObjectParts = sym::constructEditObjectParts;
        constexpr uint32_t kDestructEditObjectParts = sym::destructEditObjectParts;
        constexpr uint32_t kEditPartHeader = sym::editPartHeader;
        constexpr uint32_t kEditPartData = sym::editPartData;
        constexpr uint32_t kFindEditClass = sym::findEditClass;
        constexpr uint32_t kFreeEditClassSlot = sym::freeEditClassSlot;
        constexpr uint32_t kNewPieceCategories = sym::newPieceCategories;
        constexpr uint32_t kNewPieceDefTable = sym::newPieceDefTable;
        constexpr uint32_t kHeldPage = ee::high(sym::g_editBaseClassName); // the page of those names, held in saved registers
        inline ee::Global<ee::Ptr<EditClassTable>, sym::g_editClasses> g_editClasses;
    }

    uint32_t findEditClass(uint32_t className, ee::Ptr<uint32_t> index);

    uint32_t freeEditClassSlot();

    int32_t inheritEditClass(ee::Address added, ee::Address parent);

    void editNoop() {}
    READABLE_FUNCTION(editNoop, Verified);

    void initEditObjectParts(uint32_t object, uint32_t index);

    void constructEditObjectParts(ee::Address partAddress, uint32_t pieceDef);

    int32_t editPartData(ee::Address part);

    int32_t editPartHeader(uint32_t data);

    void destructEditObjectParts(ee::Address first);

    int32_t isEditObjectData(uint32_t data)
    {
        const ee::Ptr<EditPart> part(static_cast<uint32_t>(editPartHeader(data)));
        return part->data == data ? 1 : 0;
    }
    READABLE_FUNCTION(isEditObjectData, Verified);

    uint32_t editClassSize(uint32_t className)
    {
        ee::Local<uint32_t> index2;
        const int32_t found = static_cast<int32_t>(findEditClass(className, index2));
        const uint32_t index = *index2;
        if (found == 0)
            return 0;
        const uint32_t bytes = (*g_editClasses)->classes[index].objectBytes;
        return bytes;
    }
    READABLE_FUNCTION(editClassSize, Verified);

    uint32_t themeStartTool(uint32_t theme)
    {
        return ee::Ptr<ParkTheme>(theme)->startTool;
    }
    READABLE_FUNCTION(themeStartTool, Verified);

    uint32_t themeLightSystem(uint32_t theme)
    {
        return ee::Ptr<ParkTheme>(theme)->lightSystem;
    }
    READABLE_FUNCTION(themeLightSystem, Verified);

    uint32_t themeFlagA28(uint32_t theme)
    {
        return ee::Ptr<ParkTheme>(theme)->flagA28;
    }
    READABLE_FUNCTION(themeFlagA28, Verified);

    uint32_t newPieceCatalog()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(PieceCatalog)));
        const ee::Ptr<PieceCatalog> catalog(address);
        catalog->pieceCount = 0;
        catalog->categoryCount = 0;
        catalog->categories = newPieceCategories();
        catalog->pieces = ee::Ptr<uint32_t>(static_cast<uint32_t>(newPieceDefTable()));
        catalog->ready = 1;
        aggressive::skater::resetParkCost();
        return address;
    }
    READABLE_FUNCTION_WAITS(newPieceCatalog, Verified);
}
