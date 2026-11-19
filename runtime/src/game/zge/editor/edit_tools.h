#pragma once

// The park editor's tool set (edit_tools.cpp): its tools, the one in use and
// those put aside, the editor's update and its end.

#include "zge/editor/edit_records.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tools.cpp)
    int32_t activateEditorTool(ee::Ptr<EditToolSet> toolSet);
    int32_t deactivateEditorTool(ee::Ptr<EditToolSet> toolSet);
    void deleteEditor(ee::Ptr<EditToolSet> toolSet);
    ee::Ptr<EditToolSet> newEditToolSet(uint32_t theme, uint32_t park, uint32_t catalog, uint32_t uiLoaded);
    int32_t popEditorTool(ee::Ptr<EditToolSet> toolSet);
    void pushEditorTool(ee::Ptr<EditToolSet> toolSet, uint32_t tool);
    int32_t refreshToolSelection(ee::Address part);
    int32_t selectFirstTool(ee::Ptr<EditToolSet> toolSet);
    int32_t selectNextTool(ee::Ptr<EditToolSet> toolSet);
    int32_t selectPreviousTool(ee::Ptr<EditToolSet> toolSet);
    void updateEditor(ee::Ptr<EditToolSet> toolSet, ee::Float seconds);
}
