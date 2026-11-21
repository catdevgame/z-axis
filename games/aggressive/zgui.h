#pragma once

// The engine's UI library (zge/zgui: its scripts, widgets and libraries; DM2
// and BMX XXX run the same one) by the name AI's code calls it: zgui::.

namespace zge::zgui
{
}

namespace aggressive
{
    namespace zgui = ::zge::zgui;

    // registerZguiIconRoll: AI's icon roll widget kind registered with zgui (zgui_icon_roll.cpp).
    void registerZguiIconRoll();
}
