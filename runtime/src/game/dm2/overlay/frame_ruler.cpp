// DM2's in-game overlay: the ruler (kOverlayRuler): a measure in feet shown
// on a ruler at the screen's right, twenty feet of it at a time, with a
// marker at the measure and a mark at the game's record of the same kind.
// The trick scoring sends it a measure in metres as a trick goes on, and 0
// when it is over. It has no widget: it is zoverlay rectangles and numbers
// in the overlay's tiny font. DM2's own code (overlay.c).

#include "frames.h"
#include "overlay_calls.h"
#include "overlay_items.h"

#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/quaternion.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/overlay/frame_ruler.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The measure shown (0: none), as it was sent and in feet.
        inline ee::Global<OverlayRuler, sym::g_ruler> g_ruler;
        // The foot at the ruler's bottom: 0 until the measure passes
        // kFeetBelowMarker, then that far below it.
        inline ee::Global<ee::Float, sym::g_rulerBottomFoot> g_rulerBottomFoot;

        // The ruler's items: one off the screen, its frame's four edges and the marker.
        constexpr uint32_t kRulerItems = 6;
        constexpr uint32_t kRulerEdges = 4;
        constexpr uint32_t kRulerMarkerItem = 5;
        constexpr uint16_t kNoItem = 0xFFFF;
        inline ee::Global<uint16_t[kRulerItems], sym::g_rulerItems> g_rulerItems;

        // Where the ruler is, in pixels: its frame, the ticks inside it, the
        // marker to its left and the numbers' right edge.
        constexpr int32_t kFrameLeft = 438, kFrameRight = 469, kFrameTop = 81, kFrameBottom = 356;
        constexpr int32_t kFrameWidth = 32, kFrameHeight = 276;
        constexpr uint32_t kFrameColour = 0x18000000;
        constexpr int32_t kTickLeft = 439, kTickHeight = 2;
        constexpr int32_t kFootTickWidth = 15, kFiveFootTickWidth = 30;
        constexpr uint32_t kFootTickColour = 0x18404040, kFiveFootTickColour = 0x18202020;
        constexpr int32_t kMarkerLeft = 422, kMarkerWidth = 16, kMarkerHeight = 4;
        constexpr uint32_t kMarkerColour = 0x0FFFFFFF;
        constexpr int32_t kRecordMarkWidth = 30, kRecordMarkHeight = 6;
        constexpr uint32_t kRecordMarkColour = 0x0F703020;
        constexpr int32_t kZeroLine = 355; // where a measure at the bottom foot is drawn
        constexpr ee::Float kFirstTickY = 356.0f;
        constexpr ee::Float kNumberX = 467.0f;
        constexpr int32_t kTopNumberLowest = 88, kTopNumberRise = 3;

        // A foot in metres as the game stores it: 0.30479997, one float step
        // below the nearest float to 0.3048 (every feet shown is divided by it).
        constexpr ee::Float kMetresPerFoot = 0.30479997f;
        constexpr ee::Float kPixelsPerFoot = 13.35f;
        constexpr uint32_t kFeetShown = 20;
        constexpr ee::Float kFeetBelowMarker = 15.0f;
        constexpr uint32_t kFeetPerNumber = 5;

        // The numbers: every fifth foot, right-aligned, amber; from 1000
        // feet red and without their first digit.
        constexpr ee::Ptr<const char> kFootFormat{sym::g_textCountFormat}; // "%ld"
        constexpr uint32_t kFourDigits = 1000;
        constexpr uint32_t kAlignRight = 2; // zge::TextStyle::align
        inline ee::Global<ee::Ptr<zge::Font>, sym::g_overlayTinyFont> g_overlayTinyFont;
        // How far above the ruler's bottom a number must be to be drawn.
        // (A byte the game never sets: 0.)
        inline ee::Global<uint8_t, sym::g_rulerNumberClearance> g_rulerNumberClearance;

        // A foot's number drawn with its right edge at the ruler's and its
        // top at `y`.
        void drawFootNumber(ee::Ptr<char> text, ee::Ptr<zge::TextStyle> style, uint32_t foot, ee::Float y)
        {
            zge::formatString(text, kFootFormat, foot);
            const bool short_ = foot < kFourDigits;
            style->position.x = kNumberX;
            style->position.y = y;
            style->position.z = 0.0f;
            style->colour[0] = short_ ? 0xEE : 0xFF;
            style->colour[1] = short_ ? 0xB5 : 0x00;
            style->colour[2] = short_ ? 0x2C : 0x00;
            style->colour[3] = 0xFF;
            if (*g_overlayTinyFont)
                zge::drawText(*g_overlayTinyFont, style, ee::Ptr<const int8_t>((short_ ? text : text + 1).address));
        }

        void setRulerItemsVisible(uint32_t visible)
        {
            for (uint32_t i = 0; i < kRulerItems; ++i)
                setOverlayItemVisible((*g_rulerItems)[i], visible);
        }
    }

    // As the overlay starts, the ruler's items are made (its frame's edges
    // hidden) and it shows no measure; as the overlay ends they are given up.
    void overlayRulerSetUp(uint32_t starting)
    {
        if (starting == 0)
        {
            for (uint32_t i = 0; i < kRulerItems; ++i)
            {
                uint16_t &item = (*g_rulerItems)[i];
                if (item != kNoItem)
                {
                    removeOverlayItem(item);
                    item = kNoItem;
                }
            }
            return;
        }
        for (uint32_t i = 0; i < kRulerItems; ++i)
            (*g_rulerItems)[i] = kNoItem;
        (*g_rulerItems)[0] = static_cast<uint16_t>(newOverlayRectangle(0, -10, -10, 1, 1, 0));
        for (uint32_t edge = 0; edge < kRulerEdges; ++edge)
        {
            // The left and right edges, then the top and bottom ones.
            const bool upright = edge < 2;
            const int32_t x = edge == 1 ? kFrameRight : kFrameLeft;
            const int32_t y = edge == 3 ? kFrameBottom : kFrameTop;
            const uint16_t item = static_cast<uint16_t>(newOverlayRectangle(
                0, x, y, upright ? 1 : kFrameWidth, upright ? kFrameHeight : 1, kFrameColour));
            (*g_rulerItems)[1 + edge] = item;
            setOverlayItemVisible(item, 0);
        }
        (*g_rulerItems)[kRulerMarkerItem] = static_cast<uint16_t>(newOverlayRectangle(
            0, kMarkerLeft, kZeroLine, kMarkerWidth, kMarkerHeight, kMarkerColour));
        *g_rulerBottomFoot = 0.0f;
        g_ruler->metres = 0.0f;
    }
    READABLE_FUNCTION(overlayRulerSetUp, Verified);

    // Each frame while a measure is shown: the record's mark when it is on
    // the ruler, then a tick a foot from the bottom foot up, every fifth
    // foot's longer and numbered.
    void overlayRulerFrame()
    {
        ee::Local<char[16]> text;
        ee::Local<zge::TextStyle> style;
        style->scaleX = 1.5f;
        style->scaleY = 1.2f;
        style->scaleExtra[0] = 0.75f;
        zge::quaternionIdentity(calls::textStyleRotation(style));
        style->align = kAlignRight;
        style->lineSpacing = 1.0f;
        style->letterSpacing = 0;
        std::memset(&style->reserved, 0, 1); // its low byte only
        if (g_ruler->metres == 0.0f)
            return;

        // The first tick: the bottom foot when it is a whole one, else the
        // next foot, that much higher.
        const ee::Float bottomFoot = *g_rulerBottomFoot;
        uint32_t foot = static_cast<uint32_t>(ee::toInt(bottomFoot));
        ee::Float y = kFirstTickY;
        const ee::Float part = bottomFoot - ee::toFloat(ee::toInt(bottomFoot));
        if (part != 0.0f)
        {
            ++foot;
            y = kFirstTickY - (ee::Float(1.0f) - part) * kPixelsPerFoot;
        }
        uint32_t feetToNumber = foot % kFeetPerNumber != 0 ? kFeetPerNumber - foot % kFeetPerNumber : 0;

        const uint8_t mode = multiplayerMode();
        const uint8_t single = singlePlayerMode();
        const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, single, g_ruler->recordKind, 0));
        const ee::Float recordFoot = record->value / kMetresPerFoot;
        if (recordFoot < *g_rulerBottomFoot + ee::toFloat(kFeetShown) && *g_rulerBottomFoot <= recordFoot)
        {
            const int32_t up = ee::toInt((recordFoot - *g_rulerBottomFoot) * kPixelsPerFoot);
            drawOverlayRectangle(kTickLeft, kZeroLine - up, kRecordMarkWidth, kRecordMarkHeight, kRecordMarkColour);
        }

        for (uint32_t tick = 0; tick < kFeetShown; ++tick)
        {
            const bool numbered = feetToNumber == 0;
            drawOverlayRectangle(kTickLeft, static_cast<int16_t>(ee::toInt(y)), numbered ? kFiveFootTickWidth : kFootTickWidth,
                                 kTickHeight, numbered ? kFiveFootTickColour : kFootTickColour);
            if (numbered && ee::toInt(y) < kZeroLine - *g_rulerNumberClearance)
                drawFootNumber(text, style, foot, ee::toFloat(ee::toInt(y)));
            y = y - kPixelsPerFoot;
            ++foot;
            feetToNumber = numbered ? kFeetPerNumber - 1 : feetToNumber - 1;
        }
        // The foot above the last tick, when it is a fifth one and still
        // inside the frame: its number, a little higher.
        if (feetToNumber == 0 && ee::toInt(y) >= kTopNumberLowest)
            drawFootNumber(text, style, foot, ee::toFloat(ee::toInt(y) - kTopNumberRise));
    }
    READABLE_FUNCTION(overlayRulerFrame, Verified);

    // The ruler shown or hidden; hidden while there is no measure.
    void overlayRulerShow(uint32_t shown)
    {
        for (uint32_t i = 0; i < kRulerItems; ++i)
        {
            uint32_t visible = 0;
            if (shown != 0 && g_ruler->metres != 0.0f)
                visible = 1;
            setOverlayItemVisible((*g_rulerItems)[i], visible);
        }
    }
    READABLE_FUNCTION(overlayRulerShow, Verified);

    // A measure sent. 0 hides the ruler; the first measure after none shows
    // it (unless the frame is hidden). The marker goes to the measure, and
    // the ruler's bottom foot keeps the marker kFeetBelowMarker up at most.
    void overlayRulerMessage(ee::Ptr<const OverlayRuler> measure)
    {
        OverlayRuler &ruler = *g_ruler;
        if (ruler.metres == 0.0f && measure->metres == 0.0f)
        {
            setRulerItemsVisible(0);
            *g_rulerBottomFoot = 0.0f;
            zge::memoryCopy(sym::g_ruler, measure, sizeof(OverlayRuler));
        }
        if (measure->metres == ruler.metres)
            return;
        if (measure->metres == 0.0f)
        {
            setRulerItemsVisible(0);
            *g_rulerBottomFoot = 0.0f;
            zge::memoryCopy(sym::g_ruler, measure, sizeof(OverlayRuler));
            return;
        }
        if (ruler.metres == 0.0f && !overlayFrameHidden(kOverlayRuler))
            setRulerItemsVisible(1);
        zge::memoryCopy(sym::g_ruler, measure, sizeof(OverlayRuler));
        ruler.feet = ruler.metres / kMetresPerFoot;
        if (ruler.feet < kFeetBelowMarker)
            *g_rulerBottomFoot = 0.0f;
        else
            *g_rulerBottomFoot = ruler.feet - kFeetBelowMarker;
        const int32_t up = ee::toInt((ruler.feet - *g_rulerBottomFoot) * kPixelsPerFoot);
        setOverlayItemRectangle((*g_rulerItems)[kRulerMarkerItem], kMarkerLeft, kZeroLine - up, kMarkerWidth,
                                                  kMarkerHeight);
        ruler.recordKind = measure->recordKind;
    }
    READABLE_FUNCTION(overlayRulerMessage, Verified);
}
