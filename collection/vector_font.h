#pragma once

// The font the app ships for its own text before any game's disc is set up
// (the menu writes with Aggressive Inline's font, which is on AI's disc):
// Michroma (collection/resources/fonts: the unchanged TTF with its OFL 1.1
// licence), drawn with stb_truetype (pinned in third_party/cmake/zx_stb.cmake)
// into the menu's BitmapFont at one pixel size, so Canvas::text writes with
// it as with AI's font.
//
// Each character is a cell as wide as its advance less the one pixel
// Canvas::text puts after every glyph, with the outline drawn at its own
// left bearing inside it, every cell a line high (glyph top 0): the menu's
// spacing comes out as the font's.

#include "collection/menu.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace zx::collection
{
    // The shipped font's file below the app's resources folder.
    constexpr const char *kShippedFontFile = "fonts/Michroma-Regular.ttf";
    // Its size in place of AI's menu font: at 28 pixels a line of the
    // menu's words is as wide as in AI's 14pt font (19-pixel lines), so
    // the menu's text scales fit the same room (front1 measured "Aggressive
    // Inline Settings Custom Freeplay": AI 540 pixels, Michroma 546).
    constexpr float kShippedFontPixels = 28.0f;

    // The printable ASCII characters of a TrueType font at `pixelHeight`
    // (ascent to descent) into `out`: white, the coverage as alpha. False if
    // the file is not a font stb_truetype reads.
    bool bakeFont(const std::vector<uint8_t> &ttf, float pixelHeight, BitmapFont &out);
}
