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
// BMX XXX's own part of it; what both games have is zge/editor/park_classes.cpp.

#include "bmx/editor/park_classes.h"
#include "zge/editor/park_classes.h"
#include "bmx/editor/edit_tools.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "bmx/editor/park_catalog.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/park_classes.cpp.h"

namespace zge::editor
{
    uint32_t newPieceCatalog()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(PieceCatalog)));
        const ee::Ptr<PieceCatalog> catalog(address);
        catalog->pieceCount = 0;
        catalog->categoryCount = 0;
        catalog->categories = newPieceCategories();
        catalog->pieces = ee::Ptr<uint32_t>(static_cast<uint32_t>(newPieceDefTable()));
        catalog->ready = 1;
        resetParkCost();
        return address;
    }
    READABLE_FUNCTION_WAITS(newPieceCatalog, Verified);
}
