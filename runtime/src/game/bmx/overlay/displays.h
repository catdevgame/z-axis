#pragma once

// BMX XXX's in-game overlay (./src/com/action/overlay.c): the displays the
// game code shows, hides and sends values to. DM2's overlay frames
// (game/dm2/overlay), with a widget per player.
//
// The overlay keeps a table of fifteen displays, each with its handlers (a
// set-up, a per-frame update, a show/hide, a message, a state query); the
// game code names a display by its index. The names are the widgets' each
// display's set-up looks up ("Overlay.DisplayTimer" and the others); a
// display with no widget string has no name here.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    enum OverlayDisplayIndex : uint32_t
    {
        kOverlayTimer = 0,     // Overlay.DisplayTimer
        kOverlaySongInfo = 1,  // OverlayReplay.Demo.LabelSongInfo
        kOverlaySubtitle = 3,  // OverlayCutScene.DisplaySubtitle
        kOverlayScore = 5,     // Overlay.DisplayScore
        kOverlayTrick = 7,     // Overlay.DisplayTrick
        kOverlayBalance = 9,   // Overlay.DisplayBalance
        kOverlayHealth = 10,   // Overlay.DisplayHealth
        kOverlayHint = 11,     // Overlay.DisplayHint
        kOverlayObjective = 12, // Overlay.DisplayObjctv
        kOverlayReward = 13,   // Overlay.DisplayReward
        kOverlayExitWarning = 14, // Overlay.DisplayExitWarn
    };
    constexpr uint32_t kOverlayDisplays = 15;
    constexpr uint32_t kOverlayPlayers = 2;
    constexpr uint32_t kOverlayDisplayHidden = 0x1;

    // A display of the table: its handlers (0: none) and, for each player,
    // whether it is hidden and its widget.
    struct OverlayDisplay
    {
        ee::FunctionPtr<void(ee::Ptr<OverlayDisplay>, uint32_t)> setUp;      // +0x00 (display, 0) as the overlay ends
        ee::FunctionPtr<void(ee::Ptr<OverlayDisplay>)> frame;                // +0x04 every frame while shown
        ee::FunctionPtr<void(ee::Ptr<OverlayDisplay>, uint32_t)> show;       // +0x08 (display, shown) as it is shown or hidden
        ee::FunctionPtr<void(ee::Ptr<OverlayDisplay>, ee::Address)> message; // +0x0C a value for it (overlayMessage)
        ee::FunctionPtr<void(ee::Ptr<OverlayDisplay>, ee::Address)> state;   // +0x10 its state copied out (overlayDisplayState)
        struct Player
        {
            uint32_t flags;                    // kOverlayDisplayHidden
            ee::Ptr<zge::zgui::Node> widget;
        } players[kOverlayPlayers];            // +0x14
    };
    static_assert(sizeof(OverlayDisplay) == 0x24 && offsetof(OverlayDisplay, message) == 0x0C && offsetof(OverlayDisplay, players) == 0x14);

    // The overlay's widget groups (showOverlayPart).
    enum OverlayPart : uint32_t
    {
        kOverlayAllParts = 0,   // every group but the replay's demo
        kOverlayShared = 1,     // "Overlay" of the shared tree
        kOverlayPlayersOwn = 2, // "Overlay" of each player's tree
        kOverlayReplay = 3,     // "OverlayReplay"
        kOverlayReplayDemo = 4, // "OverlayReplay.Demo"
        kOverlayCutScene = 5,   // "OverlayCutScene"
    };
    constexpr uint32_t kOverlayEveryPartButDemo = 0x2E;
    // The fades playOverlayFade plays (the "Fade" widget's actions).
    enum OverlayFade : uint32_t
    {
        kOverlayFadeIn = 0,           // "In"
        kOverlayFadeOut = 1,          // "Out"
        kOverlayFadeInCB = 2,         // "In_CB"
        kOverlayFadeOutCB = 3,        // "Out_CB"
        kOverlayFadeSetOut = 4,       // "SetOut"
        kOverlayFadeReset = 5,        // "Reset"
        kOverlayFadeLoadingSet = 6,   // "LoadingSet"
        kOverlayFadeLoadingReset = 7, // "LoadingReset"
    };
    // playOverlayFade's player for the whole screen's fade.
    constexpr uint32_t kOverlayWholeScreen = 0xFFFFFFFFu;

    // (displays.cpp)
    // One of the overlay's widget groups shown or hidden.
    void showOverlayPart(uint32_t part, uint32_t shown);
    // A fade played over a player's part of the screen, or the whole screen.
    void playOverlayFade(uint32_t player, uint32_t fade);
    // Every display told the overlay ends, and the overlay's widgets freed.
    void closeOverlayDisplays();
    // Every shown display's frame.
    void updateOverlayDisplays();
    // A display shown or hidden (its handler told only on a change).
    void showOverlayDisplay(uint32_t display, uint32_t shown);
    // A value sent to a display's message handler (what each takes is its own).
    void overlayMessage(uint32_t display, ee::Address message);
    // A display's state copied to `out`.
    void overlayDisplayState(uint32_t display, ee::Address out);
}
