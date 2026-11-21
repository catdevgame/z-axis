// zgui's icon roll: a String's characters as icons from a Font, spaced round
// an ellipse (RadiusA by RadiusB) turned RollAngle and tilted by Angle. AI's
// own widget (its game block; DM2 and BMX XXX have their own versions):
// moved from zge/zgui/icon_roll.cpp, where it sat as engine code (layer2).

#include "aggressive/zgui.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/text/font.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/zgui_icon_roll.cpp.h"

namespace aggressive
{
    using namespace zge;
    using namespace zge::zgui;

    namespace
    {
        constexpr uint32_t kAngleSin = sym::angleSin; // by address: a game override hooks it

        struct ZguiIconRoll
        {
            Widget widget;
            ee::Float radiusA;   // +0x0F0
            ee::Float radiusB;   // +0x0F4
            int16_t angle;       // +0x0F8
            uint8_t _0fa[2];
            uint32_t totalIcon;  // +0x0FC
            uint8_t _100[4];
            uint32_t sizeX;      // +0x104
            uint32_t sizeY;      // +0x108
            ee::Float rollAngle; // +0x10C
            ee::Ptr<const char> string; // +0x110 its icons, a character each
            ee::Ptr<UiAsset> uiAsset; // +0x114 Font found
            uint32_t font;       // +0x118
        };
        static_assert(offsetof(ZguiIconRoll, radiusA) == 0xF0 && offsetof(ZguiIconRoll, font) == 0x118);

        constexpr uint32_t kDrawHandler = sym::drawZguiIconRoll, kRefreshHandler = sym::refreshZguiIconRoll;
        constexpr Message kDrawMessage = Message::Draw, kRefreshMessage = Message::Refresh;
        constexpr uint32_t kFontAssetKind = 1;
        constexpr uint32_t kIconRollKindName = sym::g_textZguiIconRoll; // "zguiIconRoll"
        constexpr uint32_t kIconRollBytes = 0x120u;
        constexpr uint32_t kDefineIconRollProperties = sym::defineZguiIconRollProperties;
        // The property and type names.
        constexpr ee::Ptr<const char> kTotalIconName{sym::g_textTotalIcon};
        constexpr ee::Ptr<const char> kStringName{sym::g_textString2};
        constexpr ee::Ptr<const char> kSizeYName{sym::g_textSizeY};
        constexpr ee::Ptr<const char> kSizeXName{sym::g_textSizeX};
        constexpr ee::Ptr<const char> kRollAngleName{sym::g_textRollAngle};
        constexpr ee::Ptr<const char> kRadiusBName{sym::g_textRadiusB};
        constexpr ee::Ptr<const char> kRadiusAName{sym::g_textRadiusA};
        constexpr ee::Ptr<const char> kFontName{sym::g_textFont};
        constexpr ee::Ptr<const char> kAngleName{sym::g_textAngle};
        constexpr uint32_t kFloatType = sym::g_textZFloat2, kIntType = sym::g_textSint322, kAssetType = sym::g_textZAssetID3;
        inline constexpr ee::Float kDefaultRadiusA = 320.0f, kDefaultRadiusB = 160.0f;
        constexpr uint32_t kDefaultIconSize = 0x20, kDefaultTotalIcon = 19;
    }

    // refreshZguiIconRoll: the font found again after a refresh.
    uint32_t refreshZguiIconRoll(ee::Ptr<ZguiIconRoll> roll)
    {
        roll->uiAsset = findGuiAsset(roll->font, kFontAssetKind);
        return 1;
    }
    READABLE_FUNCTION(refreshZguiIconRoll, Verified);

    // defineZguiIconRollProperties: a new roll's defaults, handlers and properties.
    uint32_t defineZguiIconRollProperties(uint32_t node, uint32_t text)
    {
        const ee::Ptr<ZguiIconRoll> roll(node);
        defineWidgetProperties(node, text);
        roll->angle = 0;
        roll->font = 0;
        roll->uiAsset = 0;
        roll->rollAngle = 0;
        roll->string = ee::Ptr<const char>();
        roll->radiusA = kDefaultRadiusA;
        roll->radiusB = kDefaultRadiusB;
        roll->sizeY = kDefaultIconSize;
        roll->totalIcon = kDefaultTotalIcon;
        roll->sizeX = kDefaultIconSize;
        addListener(node, kDrawMessage, kDrawHandler);
        addListener(node, kRefreshMessage, kRefreshHandler);
        defineTypedProperty(roll, kRadiusAName, kFloatType, &roll->radiusA);
        defineTypedProperty(roll, kRadiusBName, kFloatType, &roll->radiusB);
        defineTypedProperty(roll, kAngleName, kIntType, &roll->angle);
        defineTypedProperty(roll, kRollAngleName, kFloatType, &roll->rollAngle);
        defineTypedProperty(roll, kTotalIconName, kIntType, &roll->totalIcon);
        defineTypedProperty(roll, kSizeXName, kIntType, &roll->sizeX);
        defineTypedProperty(roll, kSizeYName, kIntType, &roll->sizeY);
        defineTypedProperty(roll, kFontName, kAssetType, &roll->font);
        defineTypedProperty(roll, kStringName, kStringName, &roll->string);
        return node;
    }
    READABLE_FUNCTION(defineZguiIconRollProperties, Verified);

    // registerZguiIconRoll
    void registerZguiIconRoll()
    {
        game::tail(registerWidgetKind, kIconRollKindName, kIconRollBytes, 1, kDefineIconRollProperties);
    }
    READABLE_FUNCTION(registerZguiIconRoll, Verified);

    namespace
    {
        void drawText(uint32_t font, ee::Address style, ee::Address text) { ee::game(kDrawTextAndMeasure, font, style, text); }
    }

    // drawZguiIconRoll: each character at its place round the ellipse, the near
    // half only, bigger and gold when nearest.
    uint32_t drawZguiIconRoll(uint32_t node)
    {
        ee::Local<zge::TextStyle> style;
        ee::Local<char[16]> glyph; // one icon's character
        const ee::Ptr<ZguiIconRoll> roll(node);
        const uint32_t name = roll->font;
        if (name && !roll->uiAsset)
            roll->uiAsset = findGuiAsset(name, kAssetFont);
        const ee::Ptr<const char> text = roll->string;
        if (!text || roll->totalIcon == 0)
            return 1;
        const int32_t count = zge::stringLength(text);
        roll->totalIcon = count;
        // The icons evenly round the ellipse (a turn is 0x10000).
        const int32_t spacing = static_cast<int16_t>(static_cast<int32_t>(0x00010000u / static_cast<uint32_t>(count)));
        std::memset(&style->reserved, 0, 1); // its low byte only
        style->lineSpacing = 1.0f;
        style->letterSpacing = 1;
        style->align = 1;
        style->angle = ee::Float::fromBits(0);
        if (count <= 0)
            return 1;
        for (int32_t index = 0; index < static_cast<int32_t>(roll->totalIcon); index = static_cast<int16_t>(index + 1))
        {
            // Its place on the ellipse, turned by RollAngle, tilted by Angle
            // about x, about the widget's position.
            const ee::Float around = ee::toFloat(spacing * index);
            const ee::Float cosine = zge::angleCos(static_cast<zge::Angle>(static_cast<int16_t>(ee::toInt(roll->rollAngle + around))));
            const ee::Float radiusA = roll->radiusA;
            const ee::Float angle = roll->rollAngle + around;
            style->position.y = 0.0f;
            style->position.x = cosine * radiusA;
            const ee::Float sine = zge::angleSin(static_cast<int32_t>(static_cast<int16_t>(ee::toInt(angle))));
            const int16_t tilt = roll->angle;
            style->position.z = sine * roll->radiusB;
            zge::vec3RotateX(&style->position.xyz(), &style->position.xyz(), static_cast<zge::Angle>(tilt));
            style->position = style->position + roll->widget.node.screenPosition;
            const ee::Float depth = style->position.z;
            if (!(ee::Float(0.0f) <= depth))
                continue;
            // Nearer icons larger; the largest (past 1.48) highlighted.
            const ee::Float radiusB = roll->radiusB;
            const ee::Float nearness = depth - roll->widget.node.screenPosition.z;
            glyph[1] = 0;
            style->scaleExtra[1] = 1.0f;
            glyph[0] = text[index];
            style->scaleExtra[0] = 1.0f;
            const ee::Float size = nearness / radiusB + ee::Float(0.5f);
            style->scaleX = size;
            style->scaleY = size;
            const uint32_t colour = ee::Float(1.48f) < size ? 0xFF158EB0u : 0xFFA0A0A0u; // ABGR
            std::memcpy(style->colour, &colour, 4);
            style->position.z = 0.0f;
            if (style->position.z < style->position.y)
            {
                if (const ee::Ptr<UiAsset> asset = roll->uiAsset)
                    drawText(asset->object, style, glyph);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawZguiIconRoll, Verified);
}
