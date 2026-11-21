#pragma once

// DM2's icon roll (icon_roll.cpp): a zgui widget kind of DM2's own,
// "zguiIconRoll", the editor's roll of tool icons: TotalIcon icons of
// SizeX by SizeY round an ellipse of RadiusA by RadiusB, turned by Angle
// and RollAngle, drawn with the Font asset's characters of String.

#include "zge/scene/message.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    struct IconRoll
    {
        zge::zgui::WidgetRecord widget;
        ee::Float radiusA;   // +0x60 "RadiusA" (320)
        ee::Float radiusB;   // +0x64 "RadiusB" (160)
        int32_t angle;       // +0x68 "Angle"
        int32_t totalIcon;   // +0x6C "TotalIcon" (19)
        uint8_t _70[4];
        int32_t sizeX;       // +0x74 "SizeX" (32)
        int32_t sizeY;       // +0x78 "SizeY" (32)
        ee::Float rollAngle; // +0x7C "RollAngle"
        uint32_t string;     // +0x80 "String"
        ee::Ptr<zge::zgui::UiAsset> font; // +0x84 found by Font (refreshZguiIconRoll)
        uint32_t fontName;   // +0x88 "Font"
        uint8_t _8c[4];
    };
    static_assert(offsetof(IconRoll, radiusA) == 0x60 && offsetof(IconRoll, rollAngle) == 0x7C && sizeof(IconRoll) == 0x90);

    // The kind registered with zgui (its record IconRoll), its define (the
    // widget's record, its Draw and Refresh, the defaults and the nine
    // properties) and its Refresh (the font found again).
    uint32_t registerZguiIconRoll();
    uint32_t defineZguiIconRollProperties(uint32_t address, uint32_t name, ee::Address object);
    int32_t refreshZguiIconRoll(ee::Ptr<zge::EngineMessage> message);
}
