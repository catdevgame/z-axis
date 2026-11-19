// The park editor's tool set: the tools a theme names (its catalog's
// categories among them, one tool each), which one is in use, stepping through
// them, and putting one aside under another; the editor's update and its end.
// Also what every tool uses: a sound at the selection, and the selection
// brought up to date.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tools.cpp.

#include "aggressive/level/edit_tools.h"
#include "zge/editor/edit_tools.h"
#include "aggressive/level/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "aggressive/sound/events.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_cells.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tools.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kHelperEditToolClass = sym::g_toolHelperEditToolName;      // "EditTool"

        // soundEvent's sound sources: a sound of the source bank played once at
        // a position, at full level.
        constexpr uint32_t kSoundSourceEvent = 57, kSourcePlayOnce = 3, kFullLevel = 127;
    }

    // A tool's sound (of the source bank) where its selection is.
    void playToolSound(ee::Address part, uint32_t sound)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(part, kHelperEditToolClass));
        ee::Local<zge::Vec4> position;
        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(position));
        soundEvent(kSoundSourceEvent, position.address(), kSourcePlayOnce, sound, kFullLevel);
    }
    READABLE_FUNCTION_WAITS(playToolSound, Verified);
}
