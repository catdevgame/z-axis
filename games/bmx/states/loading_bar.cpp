// BMX XXX's loading bars (game_state.h): the chunk reader's progress hook
// while a state loads in steps, the level's (action.c) and the front end's
// (the menu block). BMX XXX's own code: the two are one routine over their
// own tables and screens.
//
// Each step of a loading has a share of the bar: the bar runs from the
// share the step before reached to the step's own, by the bytes the step
// has read against the bytes it is expected to read (a step with none
// expected stands at its share at once).

#include "game_state.h"

#include "bmx/ui/ui_calls.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/render/display.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/states/loading_bar.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The loading's counts (game_state.cpp's).
        inline ee::Global<uint32_t, sym::g_loadStep> g_loadStep;
        inline ee::Global<int32_t, sym::g_loadStepBytes> g_loadStepBytes;
        inline ee::Global<char, sym::g_loadingFile> g_loadingFile;
        inline ee::Global<int32_t, sym::g_loadingFileBytes> g_loadingFileBytes;

        // A step's share of the bar: the bytes it is expected to read (0:
        // none) and how far along the bar it ends.
        struct LoadStepShare
        {
            int32_t bytes;
            ee::Float share;
        };
        static_assert(sizeof(LoadStepShare) == 8);
        constexpr ee::Ptr<LoadStepShare> kLevelLoadShares{sym::g_levelLoadShares};
        constexpr ee::Ptr<LoadStepShare> kFrontEndLoadShares{sym::g_frontEndLoadShares};

        // The bar's widget (a widget of the loading frame) and its colour.
        constexpr ee::Ptr<const char> kLevelBar{sym::g_textLevelLoadingBar};       // "UI_common.FrameLoading.LOADING.Bar"
        constexpr ee::Ptr<const char> kFrontEndBar{sym::g_textFrontEndLoadingBar}; // the same
        constexpr ee::Ptr<const char> kLevelBarWidth{sym::g_textLevelLoadingBarWidth};       // "Width"
        constexpr ee::Ptr<const char> kFrontEndBarWidth{sym::g_textFrontEndLoadingBarWidth}; // "Width"
        inline ee::Global<uint8_t[4], sym::g_levelLoadingBarColour> g_levelLoadingBarColour;       // grey, a quarter seen
        inline ee::Global<uint8_t[4], sym::g_frontEndLoadingBarColour> g_frontEndLoadingBarColour; // the same
        constexpr uint32_t kFrameNode = 1;
        // The screens' widths: the level's, the front end's.
        constexpr ee::Float kLevelScreenWidth = 512.0f;
        constexpr ee::Float kFrontEndScreenWidth = 640.0f;

        // The bar's widget: its height, at the word past the widget's
        // scripts (inferred: the rectangle the bar clears is that tall).
        struct BarWidget
        {
            uint8_t _00[0x4C];
            ee::Float height; // +0x4C
        };
        // The bar's node as that widget.
        ee::Ptr<BarWidget> barWidgetOf(ee::Ptr<zge::zgui::Node> node) { return node.cast<BarWidget>(); }
        static_assert(offsetof(BarWidget, height) == 0x4C);

        // How far along the bar the step is.
        ee::Float barShare(ee::Ptr<LoadStepShare> shares)
        {
            const uint32_t step = *g_loadStep;
            if (shares[step].bytes == 0)
                return shares[step].share;
            ee::Float done = 1.0f;
            if (ee::toFloat(*g_loadStepBytes) / ee::toFloat(shares[*g_loadStep].bytes) <= ee::Float(1.0f))
                done = ee::toFloat(*g_loadStepBytes) / ee::toFloat(shares[*g_loadStep].bytes);
            if (*g_loadStep == 0)
                return done * shares[*g_loadStep].share;
            const ee::Float from = shares[*g_loadStep - 1].share;
            return from + done * (shares[*g_loadStep].share - shares[*g_loadStep - 1].share);
        }

        // The bar for a file's progress: the bytes of a new file count from
        // 0; the bar's width is its share of the screen's width less its
        // margins, and its rectangle is cleared in its colour.
        void showLoadingBar(const char *name, int32_t bytesDone, ee::Ptr<LoadStepShare> shares, ee::Ptr<const char> barName,
                            ee::Ptr<const char> widthName, ee::Float screenWidth, ee::Local<uint8_t[4]> &colour,
                            ee::Local<zge::Vec4> &place, ee::Local<ee::Float> &width)
        {
            if (zge::compareStrings(ee::Address(name), sym::g_loadingFile) != 0)
            {
                *g_loadingFileBytes = 0;
                zge::stringCopy(sym::g_loadingFile, ee::Address(name));
            }
            *g_loadStepBytes = *g_loadStepBytes + (bytesDone - *g_loadingFileBytes);
            *g_loadingFileBytes = bytesDone;
            const ee::Float share = barShare(shares);

            const ee::Ptr<zge::zgui::Node> bar = zge::zgui::findNode(g_bmxZgui->root, barName, kFrameNode);
            *place = bar->screenPosition;
            *width = (screenWidth - (place->x + place->x)) * share;
            zge::zgui::setVariable(bar, widthName, width);
            zge::clearDisplayRectangle(static_cast<uint32_t>(ee::toInt(place->x)), static_cast<uint32_t>(ee::toInt(place->y)),
                                       static_cast<uint32_t>(ee::toInt(*width)),
                                       static_cast<uint32_t>(ee::toInt(barWidgetOf(bar)->height)), ee::Ptr<uint8_t>(colour.address()));
        }
    }

    // levelLoadingBar: the level's loading bar (the chunk reader's progress
    // hook while the level loads).
    void levelLoadingBar(const char *name, int32_t bytesDone, uint32_t totalBytes, uint32_t context)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> place;
        ee::Local<ee::Float> width;
        for (uint32_t i = 0; i < 4; ++i)
            colour[i] = (*g_levelLoadingBarColour)[i];
        showLoadingBar(name, bytesDone, kLevelLoadShares, kLevelBar, kLevelBarWidth, kLevelScreenWidth, colour, place, width);
    }
    READABLE_FUNCTION(levelLoadingBar, Verified);

    // frontEndLoadingBar: the front end's.
    void frontEndLoadingBar(const char *name, int32_t bytesDone, uint32_t totalBytes, uint32_t context)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> place;
        ee::Local<ee::Float> width;
        for (uint32_t i = 0; i < 4; ++i)
            colour[i] = (*g_frontEndLoadingBarColour)[i];
        showLoadingBar(name, bytesDone, kFrontEndLoadShares, kFrontEndBar, kFrontEndBarWidth, kFrontEndScreenWidth, colour, place, width);
    }
    READABLE_FUNCTION(frontEndLoadingBar, Verified);
}
