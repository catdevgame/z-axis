// DM2's in-game overlay: the gauge (kOverlayGauge): an upright bar at the
// screen's left, red at its ends and green at its middle, with a marker
// that sits at the middle for 0 and goes down as the value sent grows (the
// bar's half is a value of about 0.9). A value of 0 hides it. It has no
// widget: it is zoverlay rectangles. DM2's own code (overlay.c).

#include "frames.h"
#include "overlay_calls.h"
#include "overlay_items.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/frame_gauge.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The value shown (0: none).
        inline ee::Global<ee::Float, sym::g_gaugeValue> g_gaugeValue;

        // The gauge's items: the bar's two halves, its frame's four edges and the marker.
        constexpr uint32_t kGaugeItems = 7;
        constexpr uint32_t kGaugeHalves = 2;
        constexpr uint32_t kGaugeEdges = 4;
        constexpr uint32_t kGaugeMarkerItem = 6;
        constexpr uint16_t kNoItem = 0xFFFF;
        inline ee::Global<uint16_t[kGaugeItems], sym::g_gaugeItems> g_gaugeItems;

        // Where the gauge is, in pixels.
        constexpr int32_t kBarLeft = 171, kBarTop = 150, kBarWidth = 22, kHalfHeight = 73;
        constexpr uint32_t kEndColour = 0x18B02010, kMiddleColour = 0x1820B010;
        constexpr int32_t kFrameLeft = 170, kFrameRight = 193, kFrameTop = 149, kFrameBottom = 297;
        constexpr int32_t kFrameWidth = 24, kFrameHeight = 149;
        constexpr uint32_t kFrameColour = 0x18000000;
        constexpr int32_t kMarkerHeight = 4;
        constexpr uint32_t kMarkerColour = 0x0FFFFFFF;
        constexpr int32_t kMarkerMiddle = 220, kMarkerHighest = 146, kMarkerLowest = 294;
        // How far the marker goes from the middle, and the pixels a value of 1 moves it.
        constexpr int32_t kMarkerReach = 74;
        constexpr ee::Float kPixelsPerUnit = 82.857147f;

        void setGaugeItemsVisible(uint32_t visible)
        {
            for (uint32_t i = 0; i < kGaugeItems; ++i)
                setOverlayItemVisible((*g_gaugeItems)[i], visible);
        }
    }

    // As the overlay starts, the gauge's items are made, hidden, and it
    // shows no value; as the overlay ends they are given up.
    void overlayGaugeSetUp(uint32_t starting)
    {
        if (starting == 0)
        {
            for (uint32_t i = 0; i < kGaugeItems; ++i)
            {
                uint16_t &item = (*g_gaugeItems)[i];
                if (item != kNoItem)
                {
                    removeOverlayItem(item);
                    item = kNoItem;
                }
            }
            return;
        }
        for (uint32_t i = 0; i < kGaugeItems; ++i)
            (*g_gaugeItems)[i] = kNoItem;
        for (uint32_t half = 0; half < kGaugeHalves; ++half)
        {
            // The upper half runs from an end's colour down to the
            // middle's, the lower half back.
            const int32_t y = kBarTop + static_cast<int32_t>(half) * kHalfHeight;
            const uint16_t item = static_cast<uint16_t>(newOverlayGradient(
                0, kBarLeft, y, kBarWidth, kHalfHeight, half == 0 ? kEndColour : kMiddleColour,
                half == 0 ? kMiddleColour : kEndColour));
            (*g_gaugeItems)[half] = item;
            setOverlayItemVisible(item, 0);
        }
        for (uint32_t edge = 0; edge < kGaugeEdges; ++edge)
        {
            // The left and right edges, then the top and bottom ones.
            const bool upright = edge < 2;
            const int32_t x = edge == 1 ? kFrameRight : kFrameLeft;
            const int32_t y = edge == 3 ? kFrameBottom : kFrameTop;
            const uint16_t item = static_cast<uint16_t>(newOverlayRectangle(
                0, x, y, upright ? 1 : kFrameWidth, upright ? kFrameHeight : 1, kFrameColour));
            (*g_gaugeItems)[kGaugeHalves + edge] = item;
            setOverlayItemVisible(item, 0);
        }
        const uint16_t marker = static_cast<uint16_t>(
            newOverlayRectangle(0, kBarLeft, kMarkerMiddle, kBarWidth, kMarkerHeight, kMarkerColour));
        (*g_gaugeItems)[kGaugeMarkerItem] = marker;
        setOverlayItemVisible(marker, 0);
        *g_gaugeValue = 0.0f;
    }
    READABLE_FUNCTION(overlayGaugeSetUp, Verified);

    void overlayGaugeFrame() {}
    READABLE_FUNCTION(overlayGaugeFrame, Verified);

    // The gauge shown or hidden; hidden while there is no value.
    void overlayGaugeShow(uint32_t shown)
    {
        for (uint32_t i = 0; i < kGaugeItems; ++i)
        {
            uint32_t visible = 0;
            if (shown != 0 && *g_gaugeValue != 0.0f)
                visible = 1;
            setOverlayItemVisible((*g_gaugeItems)[i], visible);
        }
    }
    READABLE_FUNCTION(overlayGaugeShow, Verified);

    // A value sent. 0 hides the gauge; the first value after none shows it
    // (unless the frame is hidden); the marker goes to the value, stopping
    // at the bar's ends.
    void overlayGaugeMessage(ee::Ptr<const ee::Float> value)
    {
        if (*value == *g_gaugeValue)
            return;
        if (*value == 0.0f)
        {
            setGaugeItemsVisible(0);
            zge::memoryCopy(sym::g_gaugeValue, value, sizeof(ee::Float));
            return;
        }
        if (*g_gaugeValue == 0.0f && !overlayFrameHidden(kOverlayGauge))
            setGaugeItemsVisible(1);
        zge::memoryCopy(sym::g_gaugeValue, value, sizeof(ee::Float));
        const int32_t down = ee::toInt(*g_gaugeValue * kPixelsPerUnit);
        int32_t y;
        if (down > kMarkerReach)
            y = kMarkerLowest;
        else if (down < -kMarkerReach)
            y = kMarkerHighest;
        else
            y = kMarkerMiddle + down;
        game::tail(setOverlayItemRectangle, (*g_gaugeItems)[kGaugeMarkerItem], kBarLeft, y, kBarWidth, kMarkerHeight);
    }
    READABLE_FUNCTION(overlayGaugeMessage, Verified);
}
