// DM2's icon roll widget kind (icon_roll.h). DM2's own code (its editor
// block), in ZGE2's widget form (zge/zgui/widget_parts.h).

#include "icon_roll.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstring>
#include <cstdint>
#include "sym/dm2/editor/icon_roll.cpp.h"

namespace zge::zgui
{
    // (system.cpp; ZGE2 passes the class of the kind's widget objects)
    uint32_t registerWidgetKind(uint32_t name, uint32_t bytes, uint32_t flags, uint32_t define, uint32_t objectClass);
}

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kIconRollFlags = 1;
        constexpr ee::Float kDefaultRadiusA = 320.0f, kDefaultRadiusB = 160.0f;
        constexpr int32_t kDefaultTotalIcon = 19, kDefaultIconSize = 32;

        constexpr zge::zgui::ListedProperty kIconRollProperties[] = {
            {sym::iconRollRadiusAName, sym::iconRollFloatType, offsetof(IconRoll, radiusA)},
            {sym::iconRollRadiusBName, sym::iconRollFloatType, offsetof(IconRoll, radiusB)},
            {sym::iconRollAngleName, sym::iconRollSint32Type, offsetof(IconRoll, angle)},
            {sym::iconRollRollAngleName, sym::iconRollFloatType, offsetof(IconRoll, rollAngle)},
            {sym::iconRollTotalIconName, sym::iconRollSint32Type, offsetof(IconRoll, totalIcon)},
            {sym::iconRollSizeXName, sym::iconRollSint32Type, offsetof(IconRoll, sizeX)},
            {sym::iconRollSizeYName, sym::iconRollSint32Type, offsetof(IconRoll, sizeY)},
            {sym::iconRollFontName, sym::iconRollAssetIdType, offsetof(IconRoll, fontName)},
            {sym::iconRollStringName, sym::iconRollStringName, offsetof(IconRoll, string)},
        };
    }

    uint32_t registerZguiIconRoll()
    {
        return game::tail(zge::zgui::registerWidgetKind, sym::iconRollKindName, sizeof(IconRoll), kIconRollFlags,
                          sym::defineZguiIconRollProperties, sym::g_widgetObjectClass);
    }
    READABLE_FUNCTION(registerZguiIconRoll, Verified);

    // Kept from the original: of Angle only the low half is cleared.
    uint32_t defineZguiIconRollProperties(uint32_t address, uint32_t name, ee::Address object)
    {
        using namespace zge::zgui;
        const ee::Ptr<IconRoll> roll(address);
        defineWidgetRecord(address, name);
        *ee::Ptr<uint16_t>(address + offsetof(IconRoll, angle)) = 0;
        roll->fontName = 0;
        roll->font = ee::Ptr<UiAsset>();
        roll->rollAngle = 0.0f;
        roll->string = 0;
        roll->radiusA = kDefaultRadiusA;
        roll->radiusB = kDefaultRadiusB;
        roll->sizeY = kDefaultIconSize;
        roll->totalIcon = kDefaultTotalIcon;
        roll->sizeX = kDefaultIconSize;
        subscribeWidget(object, Message::Draw, sym::drawZguiIconRoll);
        subscribeWidget(object, Message::Refresh, sym::refreshZguiIconRoll);
        addListedProperties(roll, kIconRollProperties);
        return address;
    }
    READABLE_FUNCTION(defineZguiIconRollProperties, Verified);

    namespace
    {
        // An icon this near the front (its scale past this) is drawn in the
        // highlight colour; the others in white, fading out towards the back.
        constexpr ee::Float kHighlightScale = 1.48f;
        constexpr uint8_t kHighlightColour[4] = {200, 137, 0, 255};
        // An icon at the roll's centre depth is drawn at this scale.
        constexpr ee::Float kCentreScale = 0.5f;
        constexpr uint32_t kAlignCentre = 1; // zge::TextStyle::align
    }

    // drawZguiIconRoll: the roll's Draw. Its font found when it has none
    // yet; then, with a String, one icon for each of its characters
    // (TotalIcon becomes the string's length), an equal turn apart from
    // RollAngle on, round an ellipse RadiusA across and RadiusB deep,
    // tilted by Angle about x, at the widget's place: an icon in the front
    // half is drawn, if it is above the centre, scaled by how near the
    // front it is. 1.
    int32_t drawZguiIconRoll(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<zge::TextStyle> style;
        ee::Local<char[2]> icon;
        const ee::Ptr<IconRoll> roll = zge::zgui::messageRecord<IconRoll>(message);
        if (roll->fontName && !roll->font)
            roll->font = zge::zgui::findGuiAsset(roll->fontName, zge::zgui::kAssetFont);
        if (!roll->string || !roll->totalIcon)
            return 1;
        const ee::Ptr<const char> string(roll->string);
        roll->totalIcon = static_cast<int32_t>(zge::stringLength(string));
        const zge::Angle step = static_cast<zge::Angle>(0x10000 / roll->totalIcon);
        std::memset(&style->reserved, 0, 1); // its low byte only
        style->letterSpacing = 1;
        style->align = kAlignCentre;
        *zge::styleOrientation(*style) = zge::Quaternion{0.0f, 0.0f, 0.0f, 1.0f};
        style->lineSpacing = 1.0f;
        int32_t turn = 0;
        for (int16_t i = 0; i < roll->totalIcon; ++i, turn += step)
        {
            const zge::Angle around = static_cast<zge::Angle>(ee::toInt(roll->rollAngle + ee::toFloat(turn)));
            style->position.x = zge::angleCos(around) * roll->radiusA;
            style->position.y = 0.0f;
            style->position.z = zge::angleSin(around) * roll->radiusB;
            zge::vec3RotateX(&style->position.xyz(), &style->position.xyz(), static_cast<zge::Angle>(roll->angle));
            style->position = style->position + roll->widget.position;
            if (!(0.0f <= style->position.z))
                continue;
            icon[1] = 0;
            icon[0] = string[i];
            const ee::Float depth = (style->position.z - roll->widget.position.z) / roll->radiusB;
            const ee::Float scale = depth + kCentreScale;
            style->scaleExtra[0] = 1.0f;
            style->scaleExtra[1] = 1.0f;
            style->scaleX = scale;
            style->scaleY = scale;
            if (kHighlightScale < scale)
                std::memcpy(style->colour, kHighlightColour, sizeof(kHighlightColour));
            else
            {
                style->colour[0] = 255;
                style->colour[1] = 255;
                style->colour[2] = 255;
                style->colour[3] = static_cast<uint8_t>(ee::toInt(depth * 255.0f));
            }
            style->position.z = 0.0f;
            if (0.0f < style->position.y && roll->font)
                zge::drawText(ee::Ptr<zge::Font>(roll->font->object), style, ee::Ptr<const int8_t>(icon.address()));
        }
        return 1;
    }
    READABLE_FUNCTION(drawZguiIconRoll, Verified);

    int32_t refreshZguiIconRoll(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<IconRoll> roll = zge::zgui::messageRecord<IconRoll>(message);
        roll->font = zge::zgui::findGuiAsset(roll->fontName, zge::zgui::kAssetFont);
        return 1;
    }
    READABLE_FUNCTION(refreshZguiIconRoll, Verified);
}
