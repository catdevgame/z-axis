#pragma once
// BMX XXX's icon roll (icon_roll.cpp): its own version of the zgui widget
// kind "zguiIconRoll" the menus use (AI's aggressive/zgui_icon_roll.cpp,
// DM2's games/dm2/editor/icon_roll.cpp): a String's characters as icons from
// a Font, spaced round an ellipse (RadiusA by RadiusB) turned by RollAngle
// and tilted by Angle. ZGE4's record has no SizeX and SizeY. Its
// registration (registerZguiIconRoll) is AI's identical body. BMX XXX's own
// code (bmx.c).

#include "ee/ee.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    struct IconRoll
    {
        zge::zgui::Widget widget;
        ee::Float radiusA;   // +0x0F0 "RadiusA" (320)
        ee::Float radiusB;   // +0x0F4 "RadiusB" (160)
        int32_t angle;       // +0x0F8 "Angle" (the tilt; read as its low half)
        int32_t totalIcon;   // +0x0FC "TotalIcon" (19; the draw sets it to the String's length)
        uint8_t _100[4];
        ee::Float rollAngle; // +0x104 "RollAngle"
        ee::Ptr<const char> string; // +0x108 "String": its icons, a character each
        ee::Ptr<zge::zgui::UiAsset> font; // +0x10C found by "Font"
        uint32_t fontName;   // +0x110 "Font"
    };
    static_assert(offsetof(IconRoll, radiusA) == 0xF0 && offsetof(IconRoll, rollAngle) == 0x104 && offsetof(IconRoll, fontName) == 0x110);

    // The font found again (the Refresh message). Always 1.
    uint32_t refreshZguiIconRoll(ee::Ptr<IconRoll> roll);
    // Each icon at its place round the ellipse, the near half only, bigger
    // and gold when nearest (the Draw message). Always 1.
    uint32_t drawZguiIconRoll(ee::Ptr<IconRoll> roll);
    // A new roll's defaults, its two handlers and its properties.
    uint32_t defineZguiIconRollProperties(uint32_t node, uint32_t text);
}
