#pragma once

// A gamepad picture drawn at run time from simple shapes (no console
// maker's art): two grips, the left stick above the d-pad, four face
// buttons above the right stick, two shoulder buttons with triggers
// behind them and two small buttons in the middle, in the slightly 3D,
// greyish look of the game's own pictures (ps2_glyph_art.h Shading). The
// face buttons are lettered by position: a Nintendo-style pad B south, A
// east, Y west, X north (L R ZL ZR, - and +); an Xbox-style pad A south, B
// east, X west, Y north (LB RB LT RT, back and start). Labels are drawn in
// a font passed in (one of the game's, read from the user's disc).
//
// Used where the game shows a picture of its DualShock 2 (the Tutorial's
// loading screens, the Controller options page), with leader lines or
// arrows to padAnchor().

#include "runtime/ps2_glyph_art.h"

#include <string>

namespace ps2x::glyphs
{
    // The controls of the pad, by place.
    enum class PadControl : int
    {
        South,
        East,
        West,
        North,
        L1, // shoulder buttons
        R1,
        L2, // triggers
        R2,
        Select, // the small middle buttons: - / back
        Start,  // + / start
        LeftStick,
        RightStick,
        DPad,
        Count,
    };

    enum class PadStyle
    {
        Nintendo,
        Xbox,
    };

    struct Point
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Width over height of the drawn pad; drawGamepad fits it into its
    // rectangle at this ratio, centred.
    constexpr float kGamepadAspect = 1.6f;

    // The middle of a control of the pad drawn into the rectangle (for
    // shoulder buttons and triggers: their visible part).
    Point padAnchor(PadControl control, float x, float y, float w, float h);
    // About how far the control reaches from its anchor (to stop an arrow
    // or put a dot on it).
    float padControlRadius(PadControl control, float x, float y, float w, float h);
    // The label printed on a control ("B", "ZR", "LB"); "-" and "+" for
    // Nintendo's middle buttons; empty for the sticks and the d-pad.
    std::string padControlLabel(PadControl control, PadStyle style);

    void drawGamepad(Image &image, float x, float y, float w, float h, PadStyle style, const Font &font);

    // A hand-drawn style arrow: a curve from `from` bent towards `bend`,
    // its head at `to`, with a dark outline.
    void drawArrow(Image &image, Point from, Point bend, Point to, float width, uint32_t color, uint32_t outline);
    // A four-way arrow cross (Forward / Left / Right / Brake).
    void drawCrossArrows(Image &image, float cx, float cy, float size, uint32_t color, uint32_t outline);
}
