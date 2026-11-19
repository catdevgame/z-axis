// Aggressive Inline (SLUS-20327): Settings... by the engine's own path, the
// game's side (zge_settings_open.h has the mechanism).
//
// The play mode's update (updatePlayMode) calls checkPauseButtons each frame
// while the level's mode is playing; a Start there clears the level's stop
// reason, notes the player who stopped it and enters the paused mode
// (game/aggressive/level/pause.cpp). checkPauseButtons is wrapped here: the
// original runs to its end, then a pending Settings... request makes the
// same pause for player one. The paused mode's Pause.Show (or the
// Tutorial's) calls pcSettingsPauseShown (aggressive_inline_settings.cpp),
// which plays the menu's GoToPcSettings for the request.
#include "aggressive_inline_settings_open.h"
#include "sym/game_overrides/aggressive_inline_settings_open.cpp.h"
#include "zge_settings.h"
#include "zge_settings_open.h"

#include "runtime/ps2_session.h"

#include <cstring>
#include <iostream>

namespace aggressive_inline::settings_open
{
    namespace
    {
        // The game's mode word: the front end, play, paused (the pause
        // menu, a tip or a message box).
        constexpr uint32_t kGameModePlay = 2u, kGameModePaused = 3u;
        // game/aggressive/game_state.h: GameState::topState, TopState::level,
        // Level::flags and Level::mode, PlayMode::id.
        constexpr uint32_t kTopStateOffset = 0x0Cu, kLevelOffset = 0x10u, kModeOffset = 0x04u;
        constexpr int32_t kModePlaying = 2, kModePaused = 3; // PlayModeId
        // Level::flags: why play stopped, and the player who stopped it
        // (game/aggressive/level/level_object.h).
        constexpr uint16_t kLevelStopReason = 0x3F00u, kLevelStoppedBy = 0xC000u;
        constexpr int kRegSp = 29, kRegRa = 31;

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = address ? getConstMemPtr(rdram, address) : nullptr)
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // The level being played (the top state's block), 0 outside one.
        uint32_t currentLevel(const uint8_t *rdram)
        {
            const uint32_t topState = readWord(rdram, sym::g_debugDisplay + kTopStateOffset);
            return topState ? readWord(rdram, topState + kLevelOffset) : 0u;
        }

        int32_t levelModeId(const uint8_t *rdram)
        {
            const uint32_t level = currentLevel(rdram);
            const uint32_t mode = level ? readWord(rdram, level + kModeOffset) : 0u;
            return mode ? static_cast<int32_t>(readWord(rdram, mode)) : -1;
        }

        struct State
        {
            PS2Runtime::RecompiledFunction checkPauseButtons = nullptr;
        };
        State &state() { return ps2x::session::state<State>(); }

        void checkPauseButtonsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // The original to its end (a Start of the player's own pauses
            // there), then the request: with play still on, its pause.
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, state().checkPauseButtons, sym::checkPauseButtons, {},
                                       getRegU32(ctx, kRegSp), &v0);
            zge_host::settings_open::consumeAtPlayFrame(rdram, ctx, runtime);
            setReturnU32(ctx, v0);
            ctx->pc = getRegU32(ctx, kRegRa);
        }
    }

    bool inLevel(const uint8_t *rdram)
    {
        const uint32_t mode = readWord(rdram, sym::g_gameMode);
        return mode == kGameModePlay || mode == kGameModePaused;
    }

    bool playing(const uint8_t *rdram)
    {
        return readWord(rdram, sym::g_gameMode) == kGameModePlay;
    }

    bool pausable(const uint8_t *rdram)
    {
        return levelModeId(rdram) == kModePlaying;
    }

    void pause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t level = currentLevel(rdram);
        uint8_t *flags = level ? getMemPtr(rdram, level) : nullptr;
        if (!flags)
            return;
        // As checkPauseButtons for player one's Start: no stop reason, the
        // player noted, the paused mode.
        uint16_t value = 0;
        std::memcpy(&value, flags, sizeof(value));
        value = static_cast<uint16_t>(value & ~(kLevelStopReason | kLevelStoppedBy));
        std::memcpy(flags, &value, sizeof(value));
        const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
        runtime->callGuestFunction(rdram, ctx, sym::enterGameMode, {static_cast<uint32_t>(kModePaused)}, sp);
    }

    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pauseGroup)
    {
        zge_host::settings::runScript(rdram, ctx, runtime, pauseGroup, {"PLAY GoToPcSettings"});
    }

    bool installPlayFrameHook(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.checkPauseButtons)
            return true;
        st.checkPauseButtons = runtime.lookupFunction(sym::checkPauseButtons);
        if (!st.checkPauseButtons || !runtime.replaceFunction(sym::checkPauseButtons, &checkPauseButtonsHook))
        {
            st.checkPauseButtons = nullptr;
            std::cerr << "[aggressive-inline] settings: the play frame hook not installed (Settings... stays in the menus)"
                      << std::endl;
            return false;
        }
        return true;
    }
}
