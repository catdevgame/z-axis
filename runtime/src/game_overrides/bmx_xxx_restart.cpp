// BMX XXX (SLUS-20415): the Restart key, Aggressive Inline's and DM2's at
// BMX XXX's code.
//
// The key (bmx_xxx_gameplay.cpp asks, requestKey) starts the turn again,
// done in place of one play frame (playStateFrame, game/bmx/action/
// level_frame.cpp) while a turn runs (the round in its running phase) and
// play's Start is live (past its first frames): the round's phase is set to
// a turn's start (setRoundPhase), as the pause menu's RESTART line and the
// game's own resume action for it do (bmx_xxx_exit.cpp): the riders at the
// start, the turn's records cleared, the clock set again. Nothing is asked
// of the game otherwise.
//
// Settings... (the app menu, Cmd+,; zge_settings_open.h): asked with play
// running, the next play frame pauses as the game's Start does
// (playStateFrame's pauseGame: the sound's paused mode, the pause screen,
// the modal player) and the pause menu the game builds gets the page in its
// place (bmx_xxx_exit.cpp's pause handler), as its SETTINGS line would.
// Asked otherwise, a notice and nothing else.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_restart.cpp.h"
#include "zge_settings_open.h"

#include "ps2_runtime.h"
#include "runtime/ps2_session.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace bmx_xxx::restart
{
    namespace
    {
        // The round's state (game/bmx/game/round.h RoundState), as far as
        // its phase.
        struct RoundState
        {
            uint32_t flags;
            uint32_t phaseFramesLeft;
            uint32_t turnEndFrames;
            uint8_t phase;
        };
        static_assert(offsetof(RoundState, phase) == 0x0C);
        constexpr uint8_t kTurnStartPhase = 2, kTurnRunningPhase = 3;
        // Play's first frames, in which Start does not pause (level_frame.cpp).
        constexpr uint32_t kPauseWaitFrames = 2;
        // The level is the game state run (game/bmx/states/game_state.h); the
        // screen up is play's, or the pause menu's (game/bmx/overlay/
        // screens.h); the sound's paused mode (sound_mode.h).
        constexpr uint8_t kGameStateLevel = 1;
        constexpr uint32_t kScreenPlay = 1, kScreenPause = 7;
        constexpr uint32_t kSoundModePaused = 3;
        constexpr int kRegSp = 29, kRegRa = 31;

        struct State
        {
            PS2Runtime::RecompiledFunction playStateFrame = nullptr;
            bool keyPending = false; // VBlank and the play frame: both the EE thread
        };
        State &state() { return ps2x::session::state<State>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // ---- Settings... by the engine's own path (zge_settings_open.h) ------

        bool inLevel(const uint8_t *rdram)
        {
            const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_currentGameState);
            return p && *p == kGameStateLevel;
        }

        // Play runs: play's own screen is the one up (not the pause menu's,
        // a replay's or the round over's).
        bool playing(const uint8_t *rdram)
        {
            return readWord(rdram, sym::bmx::g_gameScreen) == kScreenPlay;
        }

        // Play is past its first frames: the game's own Start check
        // (level_frame.cpp), wherever Start would pause.
        bool pausable(const uint8_t *rdram)
        {
            return readWord(rdram, sym::bmx::g_playFrames) >= kPauseWaitFrames;
        }

        // What the game's Start does (playStateFrame's pauseGame for player
        // one): the sound's paused mode, the pause screen, the modal player.
        void pauseAsStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
            runtime->callGuestFunction(rdram, ctx, sym::bmx::setGameMode, {kSoundModePaused}, sp);
            runtime->callGuestFunction(rdram, ctx, sym::bmx::showGameScreen, {kScreenPause}, sp);
            if (uint8_t *modal = getMemPtr(rdram, sym::bmx::g_modalPlayerNum))
            {
                const int32_t player = 0;
                std::memcpy(modal, &player, sizeof(player));
            }
        }

        constexpr zge_host::settings_open::Hooks kSettingsOpenHooks{"bmx-xxx", &inLevel,  &playing,
                                                                    nullptr,   &pausable, &pauseAsStart,
                                                                    &settings_menu::openFromPause};

        void playStateFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            if (st.keyPending)
            {
                st.keyPending = false;
                const uint8_t *phase = getConstMemPtr(rdram, sym::bmx::g_round + offsetof(RoundState, phase));
                uint32_t playFrames = 0;
                if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_playFrames))
                    std::memcpy(&playFrames, p, sizeof(playFrames));
                if (phase && *phase == kTurnRunningPhase && playFrames >= kPauseWaitFrames)
                {
                    std::cout << "[bmx-xxx] restart (the Restart key, play frame " << playFrames << ")" << std::endl;
                    const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
                    runtime->callGuestFunction(rdram, ctx, sym::bmx::setRoundPhase, {kTurnStartPhase}, sp);
                    setReturnU32(ctx, 0u);
                    ctx->pc = getRegU32(ctx, kRegRa);
                    return;
                }
            }
            if (zge_host::settings_open::consumeAtPlayFrame(rdram, ctx, runtime))
            {
                // As the game's Start: the frame ends here (1).
                setReturnU32(ctx, 1u);
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            st.playStateFrame(rdram, ctx, runtime);
        }
    }

    void requestKey()
    {
        state().keyPending = true;
    }

    void installSettingsOpen(PS2Runtime &runtime)
    {
        zge_host::settings_open::install(runtime, kSettingsOpenHooks);
    }
}

namespace bmx_xxx
{
    bool installRestartKey(PS2Runtime &runtime)
    {
        restart::State &st = restart::state();
        st.playStateFrame = runtime.lookupFunction(sym::bmx::playStateFrame);
        if (!st.playStateFrame || !runtime.replaceFunction(sym::bmx::playStateFrame, &restart::playStateFrame))
        {
            std::cerr << "[bmx-xxx] restart key hook not installed" << std::endl;
            return false;
        }
        return true;
    }
}
