// The debug menu (reached from a dev build's input; its page is in the level
// scripts): a game mode of its own, one list.

#include "aggressive/camera.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/level/pause_parts.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/sound/sound.h"
#include "zge/debug/profiler.h"
#include "zge/debug/debug.h"
#include "aggressive/sound/sound_mode.h"
#include "aggressive/sound/events.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/debug_menu.cpp.h"

namespace aggressive
{
    namespace
    {
        using pause::run;

        constexpr uint32_t kUnlockInputZero = sym::g_textUnlockinput4, kUnlockInputOne = sym::g_textUnlockinput5; // "UNLOCKINPUT 0", "UNLOCKINPUT 1"
        constexpr uint32_t kLockInputZero = sym::g_textLockinput3, kLockInputOne = sym::g_textLockinput4;     // "LOCKINPUT 0", "LOCKINPUT 1"
        constexpr uint32_t kShowMultiplayerDebug = sym::g_textPlayMultiplayerUIShowDebug;  // Play MultiplayerUI.ShowDebug
        constexpr uint32_t kShowDebug = sym::g_textPlayInGameUIShowDebug;             // Play InGameUI.ShowDebug
        constexpr uint32_t kDebugMenuState = sym::g_textDebugMenuState;        // "DebugMenu State"
        constexpr uint32_t kMultiplayerSelected = sym::g_textMultiplayerUIDebugDebugMenu;   // "MultiplayerUI.Debug.DebugMenu.SelectedIndex"
        constexpr uint32_t kSelected = sym::g_textInGameUIDebugDebugMenuSelectedInde;              // "InGameUI.Debug.DebugMenu.SelectedIndex"
        constexpr uint32_t kPaused = 3;                          // setGameMode's

        inline ee::Global<uint32_t, sym::g_uiRoot> g_uiRootNode;
        inline ee::Global<int32_t, sym::g_gameModeBeforeDebugMenu> g_gameModeBeforeDebugMenu;

        // GameState::debugDisplay's bits 0-1: the profiler's display, cycled here
        // (off, the overlay with the selection on, the overlay with it off).
        constexpr uint8_t kProfilerDisplayBits = 3;
        constexpr uint8_t kProfilerDisplays = 3;
    }

    // enterDebugMenuMode.
    uint32_t enterDebugMenuMode()
    {
        run(*g_uiRootNode, kUnlockInputZero);
        run(*g_uiRootNode, kUnlockInputOne);
        if (*g_gameType == GameType::SplitScreen)
            run(*g_uiRootNode, kShowMultiplayerDebug);
        else
            run(*g_uiRootNode, kShowDebug);
        *g_gameModeBeforeDebugMenu = gameModeOf();
        setGameMode(kPaused);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterDebugMenuMode, Verified);

    // updateDebugMenuMode: the follow cameras take the tuning's camera type each frame.
    uint32_t updateDebugMenuMode()
    {
        zge::debugPrint(0, 0, kDebugMenuState);
        const auto cameraType = [] { return static_cast<uint32_t>(ee::toInt(cameraTuning(CameraTuning::Type))) & 0xFFu; };
        setFollowCameraMode(&(*g_followCameras)[0], cameraType());
        if (*g_gameType == GameType::SplitScreen)
            setFollowCameraMode(&(*g_followCameras)[1], cameraType());
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateDebugMenuMode, Verified);

    // exitDebugMenuMode.
    uint32_t exitDebugMenuMode()
    {
        run(*g_uiRootNode, kLockInputZero);
        run(*g_uiRootNode, kLockInputOne);
        return game::tail(aggressive::setGameMode, *g_gameModeBeforeDebugMenu);
    }
    READABLE_FUNCTION_WAITS(exitDebugMenuMode, Verified);

    // inlineDebugMenuSelect (zgui's CALL; nothing reads a result): the list's
    // selected line: back to play, the free camera, a line that does nothing,
    // respawn the skater, and the profiler's display (then back to play).
    void inlineDebugMenuSelect()
    {
        ee::Local<uint32_t> selected;
        const ee::Ptr<skater::Player> playerOne = &currentLevel()->players[0];
        if (*g_gameType == GameType::SplitScreen)
            zgui::copyVariable(ee::Ptr<zgui::Node>(*g_uiRootNode), ee::Ptr<const char>(kMultiplayerSelected), selected);
        else
            zgui::copyVariable(ee::Ptr<zgui::Node>(*g_uiRootNode), ee::Ptr<const char>(kSelected), selected);
        switch (*selected)
        {
        case 0:
            enterGameMode(kModePlaying);
            aggressive::skater::debugMenuHook();
            break;
        case 1:
            enterGameMode(kModeFreeCamera);
            break;
        case 2:
            break;
        case 3:
            playerOne->flags |= skater::kRespawnAtLocator;
            skater::respawn(playerOne, 0, 0);
            enterGameMode(kModePlaying);
            break;
        case 4:
        {
            uint8_t &display = g_gameState->debugDisplay;
            display = static_cast<uint8_t>((display & ~kProfilerDisplayBits) | ((display + 1) & kProfilerDisplayBits));
            if ((display & kProfilerDisplayBits) >= kProfilerDisplays)
                display = static_cast<uint8_t>(display & ~kProfilerDisplayBits);
            switch (display & kProfilerDisplayBits)
            {
            case 0:
                zge::setProfileDisplayMode(0);
                zge::setDebugMenuSelection(0);
                break;
            case 1:
                zge::setProfileDisplayMode(1);
                zge::setDebugMenuSelection(1);
                break;
            case 2:
                zge::setProfileDisplayMode(1);
                zge::setDebugMenuSelection(0);
                break;
            }
            enterGameMode(kModePlaying);
            break;
        }
        }
    }
    READABLE_FUNCTION_WAITS(inlineDebugMenuSelect, Verified);
}
