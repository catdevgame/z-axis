#pragma once

// The park editor's test run tool (edit_tool_test_run.cpp): its class.

#include "zge/editor/edit_records.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tool_test_run.cpp)
    int32_t registerEditTestRunToolClass();

    // (edit_tool_test_run.cpp, for the games' own parts)
    int32_t testRunToolPause(ee::Ptr<EditUiButton> button);
}
