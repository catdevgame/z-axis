// BMX XXX's icon roll (icon_roll.h). BMX XXX's own code (bmx.c).

#include "icon_roll.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/ui/icon_roll.cpp.h"

namespace game::bmx
{
    using zge::zgui::Message;

    namespace
    {
        constexpr uint32_t kDrawHandler = sym::drawZguiIconRoll, kRefreshHandler = sym::refreshZguiIconRoll;
        // The property and type names.
        constexpr uint32_t kRadiusAName = sym::g_textIconRollRadiusA;     // "RadiusA"
        constexpr uint32_t kRadiusBName = sym::g_textIconRollRadiusB;     // "RadiusB"
        constexpr uint32_t kAngleName = sym::g_textIconRollAngle;         // "Angle"
        constexpr uint32_t kRollAngleName = sym::g_textIconRollRollAngle; // "RollAngle"
        constexpr uint32_t kTotalIconName = sym::g_textIconRollTotalIcon; // "TotalIcon"
        constexpr uint32_t kFontName = sym::g_textIconRollFont;           // "Font"
        constexpr uint32_t kStringName = sym::g_textIconRollString;       // "String" (its type's name too)
        constexpr uint32_t kFloatType = sym::g_textIconRollZFloat;        // "ZFloat"
        constexpr uint32_t kIntType = sym::g_textIconRollSint32;          // "Sint32"
        constexpr uint32_t kAssetType = sym::g_textIconRollZAssetID;      // "ZAssetID"
        constexpr ee::Float kDefaultRadiusA = 320.0f, kDefaultRadiusB = 160.0f;
        constexpr int32_t kDefaultTotalIcon = 19;
        // An icon nearer than this (its size) is highlighted.
        inline ee::Global<ee::Float, sym::g_iconRollNearest> g_iconRollNearest; // 1.48
        constexpr ee::Float kSizeAtCentre = 0.5f;
        constexpr uint8_t kGold[4] = {0xB0, 0x8E, 0x15, 0xFF};
        constexpr uint8_t kGrey[4] = {0xA0, 0xA0, 0xA0, 0xFF};
        // A turn of angle units.
        constexpr uint32_t kTurn = 0x10000;
    }

    uint32_t refreshZguiIconRoll(ee::Ptr<IconRoll> roll)
    {
        roll->font = zge::zgui::findGuiAsset(roll->fontName, zge::zgui::kAssetFont);
        return 1;
    }
    READABLE_FUNCTION(refreshZguiIconRoll, Verified);

    uint32_t drawZguiIconRoll(ee::Ptr<IconRoll> roll)
    {
        ee::Local<zge::TextStyle> style;
        ee::Local<char[2]> glyph; // one icon's character
        if (roll->fontName && !roll->font)
            roll->font = zge::zgui::findGuiAsset(roll->fontName, zge::zgui::kAssetFont);
        if (!roll->string || roll->totalIcon == 0)
            return 1;
        const int32_t count = zge::stringLength(roll->string);
        roll->totalIcon = count;
        // The icons evenly round the ellipse (an empty String: the
        // original's division traps).
        const int16_t spacing = static_cast<int16_t>(static_cast<int32_t>(kTurn) / count);
        style->reserved &= ~0xFu; // its low four bits (ZGE4 keeps monospaced in bit 4)
        style->lineSpacing = 1.0f;
        style->letterSpacing = 1;
        style->align = 1;
        style->angle = ee::Float::fromBits(0);
        if (count <= 0)
            return 1;
        for (int16_t index = 0; index < roll->totalIcon; index = static_cast<int16_t>(index + 1))
        {
            // Its place on the ellipse, turned by RollAngle, tilted by Angle
            // about x, about the widget's place on the screen.
            const ee::Float around = ee::toFloat(spacing * index);
            const ee::Float cosine = zge::angleCos(static_cast<zge::Angle>(static_cast<int16_t>(ee::toInt(roll->rollAngle + around))));
            style->position.y = 0.0f;
            style->position.x = cosine * roll->radiusA;
            const ee::Float sine = zge::angleSin(static_cast<zge::Angle>(static_cast<int16_t>(ee::toInt(roll->rollAngle + around))));
            style->position.z = sine * roll->radiusB;
            zge::vec3RotateX(&style->position.xyz(), &style->position.xyz(), static_cast<zge::Angle>(static_cast<int16_t>(roll->angle)));
            style->position = style->position + roll->widget.node.screenPosition;
            if (!(ee::Float(0.0f) <= style->position.z))
                continue;
            // Nearer icons larger; the largest highlighted.
            const ee::Float size = (style->position.z - roll->widget.node.screenPosition.z) / roll->radiusB + kSizeAtCentre;
            glyph[1] = 0;
            glyph[0] = roll->string[index];
            style->scaleExtra[1] = 1.0f;
            style->scaleExtra[0] = 1.0f;
            style->scaleX = size;
            style->scaleY = size;
            std::memcpy(style->colour, *g_iconRollNearest < size ? kGold : kGrey, 4);
            style->position.z = 0.0f;
            if (style->position.z < style->position.y)
            {
                if (const ee::Ptr<zge::zgui::UiAsset> asset = roll->font)
                    zge::drawText(ee::Ptr<zge::Font>(asset->object), style, ee::Ptr<const int8_t>(ee::Address(glyph)));
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawZguiIconRoll, Verified);

    uint32_t defineZguiIconRollProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<IconRoll> roll(node);
        zge::zgui::defineWidgetProperties(node, text);
        roll->angle = 0;
        roll->fontName = 0;
        roll->font = {};
        roll->rollAngle = 0.0f;
        roll->string = {};
        roll->radiusA = kDefaultRadiusA;
        roll->radiusB = kDefaultRadiusB;
        roll->totalIcon = kDefaultTotalIcon;
        zge::zgui::addListener(node, Message::Draw, kDrawHandler);
        zge::zgui::addListener(node, Message::Refresh, kRefreshHandler);
        zge::zgui::defineTypedProperty(node, kRadiusAName, kFloatType, &roll->radiusA);
        zge::zgui::defineTypedProperty(node, kRadiusBName, kFloatType, &roll->radiusB);
        zge::zgui::defineTypedProperty(node, kAngleName, kIntType, &roll->angle);
        zge::zgui::defineTypedProperty(node, kRollAngleName, kFloatType, &roll->rollAngle);
        zge::zgui::defineTypedProperty(node, kTotalIconName, kIntType, &roll->totalIcon);
        zge::zgui::defineTypedProperty(node, kFontName, kAssetType, &roll->fontName);
        zge::zgui::defineTypedProperty(node, kStringName, kStringName, &roll->string);
        return node;
    }
    READABLE_FUNCTION(defineZguiIconRollProperties, Verified);
}
