#pragma once
// zgui as BMX XXX's UI code calls it: the engine's own functions by name
// (zge/zgui/script.h, widget.h, widget_parts.h).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/bmx/ui/ui_calls.h.h"

namespace game::bmx::ui::calls
{
    using Node = ee::Ptr<zge::zgui::Node>;
}
