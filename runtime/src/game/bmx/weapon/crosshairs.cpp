// BMX XXX's weapons (weapon.h): the crosshairs and the scope's picture laid
// out over a camera's rectangle of the screen (./src/com/action/weapon.c).
//
// "UI_Weapon" holds a "MuzzleFlash", a "Weapon_<name>.Crosshair" for each
// weapon type and the "Scope". A crosshair is two hairlines ("Horizontal",
// "Vertical") through a "Dot" with four reticle bars ("Top", "Bottom",
// "Left", "Right") round it; the scope is four borders ("Border_L", "_R",
// "_T", "_B") round a picture of four corners.

#include "weapon.h"

#include "bmx/camera/action_camera.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/weapon/crosshairs.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using zge::zgui::Node;

    namespace
    {
        inline ee::Global<ee::Ptr<Node>, sym::g_weaponUi> g_weaponUi;
        inline ee::Global<ee::Ptr<char>[kWeaponTypes], sym::g_weaponNames> g_weaponNames;
        // The crosshair's bars and the scope's corners, by name.
        inline ee::Global<uint32_t[4], sym::g_crosshairBarNames> g_crosshairBarNames; // "Top", "Bottom", "Left", "Right"
        inline ee::Global<uint32_t[4], sym::g_scopeCornerNames> g_scopeCornerNames;   // "Corner_TL", "_TR", "_BL", "_BR"

        constexpr uint32_t kCrosshairName = sym::bmx::g_weaponCrosshairFormat; // "Weapon_%s.Crosshair"

        // The scope's picture is this much wider than high, and so is the
        // square the crosshair's reticle is measured in.
        constexpr ee::Float kScopeAspect = std::bit_cast<float>(0x3F5BFA81u); // 0.859291

        // A widget's "Position".
        struct WidgetPosition
        {
            ee::Float x, y;
            uint32_t z;
        };

        // A widget moved and sized.
        void placeWidget(ee::Ptr<Node> widget, ee::Ptr<WidgetPosition> position, ee::Ptr<ee::Float> width, ee::Ptr<ee::Float> height)
        {
            zge::zgui::setVariable(widget, sym::g_weaponPosition, position); // "Position"
            zge::zgui::setVariable(widget, sym::g_weaponWidth, width);       // "Width"
            zge::zgui::setVariable(widget, sym::g_weaponHeight, height);     // "Height"
        }
    }

    // layoutWeaponCrosshairs: the weapons' widgets laid out over a rectangle
    // of the screen: the muzzle flash fills it, every weapon type's
    // crosshair is centred in it, and so is the scope at full size.
    void layoutWeaponCrosshairs(const ScreenRectangle *rectangle)
    {
        ee::Local<char[0x40]> name;
        ee::Local<WidgetPosition> position;
        const ee::Ptr<Node> flash = zge::zgui::findNode(*g_weaponUi, sym::g_weaponMuzzleFlashName, 1); // "MuzzleFlash"
        position->x = rectangle->x;
        position->y = rectangle->y;
        position->z = 0;
        zge::zgui::setVariable(flash, sym::g_weaponPosition, position);
        zge::zgui::setVariable(flash, sym::g_weaponWidth, &rectangle->width);
        zge::zgui::setVariable(flash, sym::g_weaponHeight, &rectangle->height);
        for (uint32_t type = 0; type < kWeaponTypes; ++type)
        {
            formatString(name, kCrosshairName, (*g_weaponNames)[type]);
            const ee::Ptr<Node> crosshair = zge::zgui::findNode(*g_weaponUi, name.address(), 1);
            if (crosshair != 0)
                layoutCrosshair(crosshair, rectangle);
        }
        layoutScope(zge::zgui::findNode(*g_weaponUi, sym::g_weaponScopeName, 1), rectangle, ee::Float(1.0f)); // "Scope"
    }
    READABLE_FUNCTION(layoutWeaponCrosshairs, Verified);

    // layoutCrosshair: a crosshair centred in the rectangle: the hairlines
    // one unit thick across the whole rectangle, the dot at the centre, and
    // the four reticle bars between the crosshair's "ReticleInside" and
    // "ReticleOutside" (parts of half the reticle's square) from the centre,
    // "ReticleThickness" (a part of the square, made an odd number of units)
    // thick. The square is as high as the rectangle, or as high as the
    // rectangle's width allows.
    void layoutCrosshair(ee::Ptr<Node> crosshair, const ScreenRectangle *rectangle)
    {
        ee::Local<WidgetPosition> centre;
        ee::Local<WidgetPosition> position;
        ee::Local<ee::Float> width;
        ee::Local<ee::Float> height;
        ee::Local<ee::Float> inside;
        ee::Local<ee::Float> outside;
        ee::Local<ee::Float> thickness;

        const ee::Float widthAsHeight = rectangle->width / kScopeAspect;
        *width = widthAsHeight;
        const ee::Float halfHeight = (rectangle->height <= widthAsHeight ? rectangle->height : widthAsHeight) * ee::Float(0.5f);
        centre->y = rectangle->y + rectangle->height * ee::Float(0.5f);
        centre->x = rectangle->x + rectangle->width * ee::Float(0.5f);
        centre->z = 0;
        const ee::Float halfWidth = halfHeight * kScopeAspect;

        const ee::Ptr<Node> horizontal = zge::zgui::findNode(crosshair, sym::g_crosshairHorizontalName, 1); // "Horizontal"
        if (horizontal != 0)
        {
            position->x = rectangle->x;
            position->y = centre->y;
            *width = rectangle->width;
            *height = ee::Float(1.0f);
            position->z = 0;
            placeWidget(horizontal, position, width, height);
        }
        const ee::Ptr<Node> vertical = zge::zgui::findNode(crosshair, sym::g_crosshairVerticalName, 1); // "Vertical"
        if (vertical != 0)
        {
            position->x = centre->x;
            position->y = rectangle->y;
            *width = ee::Float(1.0f);
            *height = rectangle->height;
            position->z = 0;
            placeWidget(vertical, position, width, height);
        }
        const ee::Ptr<Node> dot = zge::zgui::findNode(crosshair, sym::g_crosshairDotName, 1); // "Dot"
        if (dot != 0)
        {
            position->z = 0;
            position->x = centre->x - ee::Float(1.0f);
            position->y = centre->y - ee::Float(1.0f);
            zge::zgui::setVariable(dot, sym::g_weaponPosition, position);
        }

        zge::zgui::copyVariable(crosshair, sym::g_crosshairReticleInside, inside);       // "ReticleInside"
        zge::zgui::copyVariable(crosshair, sym::g_crosshairReticleOutside, outside);     // "ReticleOutside"
        zge::zgui::copyVariable(crosshair, sym::g_crosshairReticleThickness, thickness); // "ReticleThickness"
        const ee::Float squareWidth = halfWidth + halfWidth;
        const ee::Float squareHeight = halfHeight + halfHeight;
        for (uint32_t bar = 0; bar < 4; ++bar)
        {
            const ee::Ptr<Node> widget = zge::zgui::findNode(crosshair, (*g_crosshairBarNames)[bar], 1);
            if (widget == 0)
                continue;
            if ((bar & 2) == 0)
            {
                // Top and bottom: upright bars above and below the centre.
                *width = ee::toFloat(ee::toInt(squareWidth * *thickness) | 1);
                const ee::Float halfBar = *width * ee::Float(0.5f);
                *height = halfHeight * (*outside - *inside);
                position->x = centre->x - ee::toFloat(ee::toInt(halfBar));
                if ((bar & 1) == 0)
                    position->y = centre->y - *outside * halfHeight;
                else
                    position->y = centre->y + *inside * halfHeight;
                position->z = 0;
            }
            else
            {
                // Left and right: level bars beside the centre.
                *width = halfWidth * (*outside - *inside);
                *height = ee::toFloat(ee::toInt(squareHeight * *thickness) | 1);
                if ((bar & 1) == 0)
                    position->x = centre->x - *outside * halfWidth;
                else
                    position->x = centre->x + *inside * halfWidth;
                position->z = 0;
                position->y = centre->y - ee::toFloat(ee::toInt(*height * ee::Float(0.5f)));
            }
            placeWidget(widget, position, width, height);
        }
    }
    READABLE_FUNCTION(layoutCrosshair, Verified);

    // layoutScope: the scope's picture centred in the rectangle, `scale`
    // times the largest that fits, and the four borders that cover the rest
    // of the rectangle round it (each overlapping the picture by a unit).
    // The picture's four corners are quarters of it.
    void layoutScope(ee::Ptr<Node> scope, const ScreenRectangle *rectangle, ee::Float scale)
    {
        ee::Local<WidgetPosition> centre;
        ee::Local<WidgetPosition> position;
        ee::Local<ee::Float> width;
        ee::Local<ee::Float> height;

        const ee::Float widthAsHeight = rectangle->width / kScopeAspect;
        *width = widthAsHeight;
        const ee::Float halfFit = (rectangle->height <= widthAsHeight ? rectangle->height : widthAsHeight) * ee::Float(0.5f);
        const ee::Float halfHeight = scale * halfFit;
        const ee::Float halfWidth = halfHeight * kScopeAspect;
        const ee::Float centreX = rectangle->x + rectangle->width * ee::Float(0.5f);
        const ee::Float centreY = rectangle->y + rectangle->height * ee::Float(0.5f);
        centre->z = 0;
        *height = rectangle->height;
        centre->y = centreY;
        centre->x = centreX;

        // Left and right of the picture, the rectangle's whole height.
        *width = ee::max(centreX - halfWidth + ee::Float(1.0f) - rectangle->x, ee::Float(0.0f));
        const ee::Ptr<Node> left = zge::zgui::findNode(scope, sym::g_scopeBorderLeftName, 1); // "Border_L"
        position->x = rectangle->x;
        position->y = rectangle->y;
        position->z = 0;
        placeWidget(left, position, width, height);
        const ee::Ptr<Node> right = zge::zgui::findNode(scope, sym::g_scopeBorderRightName, 1); // "Border_R"
        position->z = 0;
        position->y = rectangle->y;
        position->x = centre->x + halfWidth - ee::Float(1.0f);
        placeWidget(right, position, width, height);

        // Above and below it, the picture's width.
        *width = halfWidth + halfWidth;
        *height = ee::max(centre->y - halfHeight + ee::Float(1.0f) - rectangle->y, ee::Float(0.0f));
        const ee::Ptr<Node> top = zge::zgui::findNode(scope, sym::g_scopeBorderTopName, 1); // "Border_T"
        position->z = 0;
        position->y = rectangle->y;
        position->x = centre->x - halfWidth;
        placeWidget(top, position, width, height);
        const ee::Ptr<Node> bottom = zge::zgui::findNode(scope, sym::g_scopeBorderBottomName, 1); // "Border_B"
        position->z = 0;
        position->x = centre->x - halfWidth;
        position->y = centre->y + halfHeight - ee::Float(1.0f);
        placeWidget(bottom, position, width, height);

        *width = halfWidth;
        *height = halfHeight;
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            const ee::Ptr<Node> widget = zge::zgui::findNode(scope, (*g_scopeCornerNames)[corner], 1);
            ee::Float x = centre->x;
            if ((corner & 1) == 0)
                x = x - halfWidth;
            ee::Float y = centre->y;
            if ((corner & 2) == 0)
                y = y - halfHeight;
            position->x = x;
            position->y = y;
            position->z = 0;
            placeWidget(widget, position, width, height);
        }
    }
    READABLE_FUNCTION(layoutScope, Verified);
}
