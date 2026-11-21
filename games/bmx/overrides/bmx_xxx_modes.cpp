// BMX XXX (SLUS-20415): every multiplayer mode whose rules work, in the
// multiplayer SELECT GAME menu (ZX_BMX_ALL_MP_MODES=1, [extras]
// all_mp_modes; off by default).
//
// The game has 20 modes (g_gameModes: rounds, turn time, scoring; names and
// descriptions in every release's STRING.ZAL, KSTRMP_MODENAME_MODE%02d and
// KSTRMP_DESC_MODE%02d), 16 of them multiplayer (4 to 19). Its SELECT GAME
// (games/bmx/game/game_screens.cpp listMpGameModes) lists the three of
// g_mpGameSelectModes, SKILLZ, PAINTBALL and STRIP CHALLENGE, then EXIT, in
// every release. With the setting the menu lists kOfferedModes instead, in
// mode order, then EXIT; each was played with two riders to its game over.
// Two are never offered (dead ends: neither can be played as it says):
//
//   B-M-X (6): its word screen (horseWordScreenMessage) looks for
//   "KeyboardFrame" in UI_overlay, where no release's UILIB has one (the
//   keyboard is UI_common's, which the record name screen finds), so it
//   shows an empty frame with no way on.
//   GHOSTRIDE DUEL (19): "the first to take out 10 pedestrians by ghost
//   riding a bike". Its record has the 10 (g_gameModes +0x10) and a turn
//   end of its own (ghostRideTurnEndFrame: the clock waits for a ghost ride
//   to end), but no code found counts a pedestrian: its one 60 s turn scores trick
//   lines as SKILLZ does, and played it ends on the clock with the trick
//   scores, not the rules it gives.
//
// The screen's handler (mpGameSelectScreenMessage) is wrapped. As the
// screen comes up the game's handler makes its menu, then its Items are set
// again with the offered modes and EXIT (the game's own texts: "$KSTRMP_
// MODENAME_MODE%02d" and "$KSTR_GAMEOVER_EXIT"), four lines in view as the
// game's menu shows (it scrolls). A line chosen or moved to
// does what the game's handler does with its own lines: a mode chosen starts
// that game (startGame, then the game's start phase), EXIT asks for the exit
// confirmation over this screen,
// and the description shows the mode's KSTRMP_DESC_MODE%02d. The game's
// table and its code are not changed.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_modes.cpp.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace bmx_xxx::mp_modes
{
    // The modes offered, in order (games/bmx/game/round.h): SKILLZ, HIGH FIVE,
    // RANDOM SPOT, WIPEOUT, the eight three-chance contests (LONGEST GRIND,
    // HIGHEST WALLTAP, BIG AIR CONTEST, HUGE COMBO CONTEST, SUPER SPIN
    // CONTEST, FURTHEST JUMP, LONGEST MANUAL, LONGEST NOSE WHEELIE), PAINTBALL
    // and STRIP CHALLENGE.
    constexpr uint8_t kOfferedModes[] = {4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};

    // The screen's messages (games/bmx/overlay/screens.h).
    constexpr int32_t kScreenChosen = 0;
    constexpr int32_t kScreenSelectionChanged = 1;
    constexpr int32_t kScreenEnter = 10;
    // The screens and the round's phase a choice leads to.
    constexpr uint32_t kScreenExitLevel = 18;
    constexpr uint32_t kScreenAfterGame = 37;
    constexpr uint32_t kGameStartPhase = 1;
    constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
    constexpr int32_t kVisibleLines = 4;
    constexpr const char *kModeNameKey = "$KSTRMP_MODENAME_MODE%02d";
    constexpr const char *kModeDescriptionKey = "$KSTRMP_DESC_MODE%02d";
    constexpr const char *kExitText = "$KSTR_GAMEOVER_EXIT";
    constexpr int kRegA0 = 4, kRegSp = 29, kRegRa = 31;

    struct State
    {
        PS2Machine::RecompiledFunction handler = nullptr; // the game's mpGameSelectScreenMessage
    };
    State &state() { return zx::session::state<State>(); }

    std::string itemsText()
    {
        std::string items;
        char line[64];
        for (uint8_t mode : kOfferedModes)
        {
            std::snprintf(line, sizeof(line), kModeNameKey, mode);
            items += std::string(items.empty() ? "" : "\\n") + line;
        }
        return items + "\\n" + kExitText;
    }

    // A text into guest memory at `at` (below the stack: the calls' scratch).
    void putText(uint8_t *rdram, uint32_t at, const std::string &text)
    {
        if (uint8_t *p = getMemPtr(rdram, at))
            std::memcpy(p, text.c_str(), text.size() + 1);
    }

    uint32_t readWord(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, address))
            std::memcpy(&value, p, sizeof(value));
        return value;
    }

    void handler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        State &st = state();
        const int32_t message = static_cast<int32_t>(getRegU32(ctx, kRegA0));
        if (message != kScreenEnter && message != kScreenChosen && message != kScreenSelectionChanged)
        {
            st.handler(rdram, ctx, runtime);
            return;
        }
        if (message == kScreenEnter)
            st.handler(rdram, ctx, runtime); // the game's menu first (its focus, its Init)
        // Guest memory below the stack for the calls and the texts.
        const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu;
        const uint32_t text = sp + 0x100u, value = sp + 0x380u;
        uint32_t frame = 0, menu = 0;
        const uint32_t overlay = readWord(rdram, sym::bmx::g_overlayUi);
        const bool found =
            overlay &&
            runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {overlay, sym::bmx::g_textFrameMPGameSelect, kFrameNode}, sp,
                                       &frame) &&
            frame &&
            runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {frame, sym::bmx::g_textFrameMenuMenu, kFrameNode}, sp, &menu) &&
            menu;
        if (message == kScreenEnter)
        {
            if (found)
            {
                putText(rdram, text, itemsText());
                runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariable, {menu, sym::bmx::g_textItems, text}, sp);
                // Four lines at a time, as many as the game's menu shows (its
                // VisibleCount, 6, would run past the frame onto the prompt).
                if (uint8_t *p = getMemPtr(rdram, value))
                    std::memcpy(p, &kVisibleLines, sizeof(kVisibleLines));
                runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariable, {menu, sym::bmx::g_textVisibleCount, value}, sp);
            }
            return;
        }
        if (!found)
        {
            st.handler(rdram, ctx, runtime);
            return;
        }
        ctx->pc = getRegU32(ctx, kRegRa);
        int32_t selected = -1;
        if (runtime->callGuestFunction(rdram, ctx, sym::bmx::copyVariable, {menu, sym::bmx::g_textSelectedIndex, value}, sp))
            selected = static_cast<int32_t>(readWord(rdram, value));
        constexpr int32_t kModes = static_cast<int32_t>(sizeof(kOfferedModes));
        if (selected < 0 || selected > kModes)
            return;
        const bool exit = selected == kModes;
        if (message == kScreenSelectionChanged)
        {
            // The description: the mode's; EXIT's is mode 0's (an empty
            // line), as the game's table has 0 for its EXIT.
            char line[64];
            std::snprintf(line, sizeof(line), kModeDescriptionKey, exit ? 0 : kOfferedModes[selected]);
            putText(rdram, text, line);
            uint32_t description = 0;
            if (runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {frame, sym::bmx::g_textFrameDescriptionText, kFrameNode}, sp,
                                           &description) &&
                description)
                runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariable, {description, sym::bmx::g_textText, text}, sp);
            return;
        }
        if (exit)
        {
            // The exit confirmation, which comes back here.
            if (uint8_t *p = getMemPtr(rdram, sym::bmx::g_screenBehindExitConfirm))
                std::memcpy(p, &kScreenAfterGame, sizeof(kScreenAfterGame));
            runtime->callGuestFunction(rdram, ctx, sym::bmx::showGameScreen, {kScreenExitLevel}, sp);
            if (uint8_t *p = getMemPtr(rdram, sym::bmx::g_gameScreenChanged))
                std::memset(p, 0, sizeof(uint32_t));
            return;
        }
        const uint8_t mode = kOfferedModes[selected];
        std::cout << "[bmx-xxx] multiplayer: mode " << static_cast<int>(mode) << std::endl;
        runtime->callGuestFunction(rdram, ctx, sym::bmx::startGame, {mode}, sp);
        runtime->callGuestFunction(rdram, ctx, sym::bmx::setRoundPhase, {kGameStartPhase}, sp);
    }
}

namespace bmx_xxx
{
    bool installAllMultiplayerModes(PS2Machine &runtime)
    {
        using namespace mp_modes;
        const char *on = zx::boot::text("ZX_BMX_ALL_MP_MODES");
        if (!on || on[0] != '1')
            return false;
        State &st = state();
        if (st.handler)
            return true;
        PS2Machine::RecompiledFunction original = runtime.lookupFunction(sym::bmx::mpGameSelectScreenMessage);
        if (!original || !runtime.replaceFunction(sym::bmx::mpGameSelectScreenMessage, &handler))
        {
            std::cerr << "[bmx-xxx] SELECT GAME's handler not found: the game's three modes only" << std::endl;
            return false;
        }
        st.handler = original;
        std::cout << "[bmx-xxx] SELECT GAME: " << sizeof(kOfferedModes) << " multiplayer modes" << std::endl;
        return true;
    }
}
