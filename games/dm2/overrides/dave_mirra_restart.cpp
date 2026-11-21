// Dave Mirra Freestyle BMX 2 (SLUS-20159): Restart and, in the collection,
// Exit to Collection in the pause menu.
//
// The pause menu (InGameUI.FramePause.Menu) is built by showPauseScreen: an
// Items text of "$KSTR_PAUSEMENU_..." lines (Continue, [Goals], Options,
// Trick List, Records, End Run) and, per line, an action id in
// g_pauseChoices (End Run = 5), then VisibleCount; bmxoverlayPauseHandler
// maps the selected line's id to the screen to show (showGameScreen).
// Here the Items text gets a RESTART line after End Run (showPauseScreen's
// setVariable call for Items), with id 6, and VisibleCount one more (its
// call for the count); the collection's lines follow it as one block
// (RESTART, PHOTO MODE, SETTINGS, EXIT TO COLLECTION), the game's lines
// first. Choosing it does what the round-over menu's "next
// run" does (bmxoverlayRoundOverHandler): close the screen
// (showGameScreen(0)) and start the round again (setRoundPhase(3), which
// resets every rider's run state and places them at the start).
// ZX_RESTART=0: no Restart line.
//
// With photo mode (dave_mirra_photo.cpp), a PHOTO MODE line after RESTART,
// whose index is kept as Exit to Collection's is: choosing it starts photo
// mode from the pause menu, back to it after.
//
// The Restart key (dave_mirra_gameplay.cpp asks, requestKey): the same
// restart, done in place of one play frame (actionFrame, the play mode's
// frame) once a turn is on (a free ride's or a timed one: round phase 4 or
// 5) and play's Start wait is over (the action step is pausable), as the
// pause item would. Nothing is asked of the game otherwise.
//
// With the Settings page (dave_mirra_settings.cpp), a SETTINGS line after
// those, its index kept the same way: choosing it shows the page in the pause
// menu's place, back to it after.
//
// Settings... (the app menu, Cmd+,; zge_settings_open.h): asked with play
// running, the next play frame pauses as the game's Start does (actionFrame's
// pauseGame: the sound's paused mode, then the pause screen) and the pause
// menu it builds (showPauseScreen) gets the page in its place, as its
// SETTINGS line would. Asked otherwise, a notice and nothing else.
//
// In the collection (zge_host::inCollection), an EXIT TO COLLECTION line
// last: the game's menu draws it like the others; choosing it
// leaves the game for the collection's menu. Its action id is not in the
// table (the line's index is kept here), so the game's own state is as
// without it but for the Items text and VisibleCount.
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_restart.cpp.h"
#include "overrides/zge_host.h"
#include "overrides/zge_settings_open.h"
#include "overrides/zge_sites.h"

#include "machine.h"
#include "runtime/boot_config.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace dave_mirra::restart
{
    constexpr uint32_t kSetProperty = sym::dm2::setVariable;
    // The pause menu's builder: its setVariable calls for the menu's Items text
    // and VisibleCount are known by running inside it (it is wrapped) and by
    // the variables' names, never by a return address (a readable builder
    // gives none for a callee it calls from two places).
    constexpr uint32_t kShowPauseScreen = sym::dm2::showPauseScreen;
    constexpr uint32_t kPauseHandler = sym::dm2::bmxoverlayPauseHandler;
    constexpr uint32_t kGetProperty = sym::dm2::copyVariable;
    constexpr uint32_t kFind = sym::dm2::findNode;
    constexpr uint32_t kRootWidget = sym::dm2::g_uiRoot;
    constexpr uint32_t kPauseMenuName = sym::dm2::g_textFramePauseMenu; // as the handler finds it
    constexpr uint32_t kSelectedIndexName = sym::dm2::g_textSelectedIndex;
    constexpr uint32_t kActionIds = sym::dm2::g_pauseChoices;
    constexpr uint32_t kOpenOverlay = sym::dm2::showGameScreen;
    constexpr uint32_t kRound = sym::dm2::setRoundPhase;
    constexpr uint32_t kActionFrame = sym::dm2::actionFrame;
    constexpr uint32_t kRoundPhase = sym::dm2::g_roundPhase;   // byte
    constexpr uint32_t kActionStep = sym::dm2::g_actionStep;   // byte
    constexpr uint32_t kSoundSetGameMode = sym::dm2::setGameMode;        // the sound's mode (games/dm2/sound/sound_mode.h)
    constexpr uint32_t kGameScreen = sym::dm2::g_gameScreen;             // byte: the screen up (games/dm2/overlay/overlay.h)
    constexpr uint32_t kCurrentGameState = sym::dm2::g_currentGameState; // byte (games/dm2/action/action.h)
    constexpr uint8_t kGameStateLevel = 1u;
    constexpr uint32_t kSoundModePaused = 3u;
    constexpr uint32_t kScreenPlay = 0u, kScreenPause = 1u;
    constexpr uint8_t kPlayPausable = 1u;             // games/dm2/action/action_state.h
    constexpr uint8_t kFreeTurnPhase = 4u, kTurnRunningPhase = 5u; // games/dm2/game/round.h
    constexpr uint32_t kTurnStartPhase = 3u;
    constexpr uint8_t kEndRunId = 5u, kRestartId = 6u;
    constexpr const char *kRestartLine = "\\nRESTART";
    constexpr const char *kPhotoLine = "\\nPHOTO MODE";
    constexpr const char *kSettingsLine = "\\nSETTINGS";
    constexpr size_t kItemsSize = 480u; // what the menu's builder leaves for its text
    constexpr int kRegA0 = 4, kRegA1 = 5, kRegA2 = 6, kRegSp = 29, kRegRa = 31;

    // One boot's originals and whether its pause menu got the Restart line
    // (made at the boot's first use, the install: zx::boot::PerBoot).
    struct State
    {
        PS2Machine::RecompiledFunction setProperty = nullptr;
        PS2Machine::RecompiledFunction pauseHandler = nullptr;
        PS2Machine::RecompiledFunction actionFrame = nullptr;
        PS2Machine::RecompiledFunction showPauseScreen = nullptr;
        bool building = false; // the pause menu's builder is running
        bool keyPending = false; // the Restart key was pressed (VBlank and the play frame: both the EE thread)
        bool restart = false;    // the Restart line wanted (ZX_RESTART is not 0)
        bool collection = false; // the Exit to Collection line wanted
        bool photo = false;      // the Photo Mode line wanted
        bool settings = false;   // the Settings line wanted
        bool hooked = false;     // the pause menu's hooks are in place
        int added = 0;           // lines added to the menu being built
        int photoLine = -1;      // the Photo Mode line's index in it, -1: none
        int exitLine = -1;       // the Exit to Collection line's index in it, -1: none
        int settingsLine = -1;   // the Settings line's index in it, -1: none
    };

    State &state()
    {
        static zx::boot::PerBoot<State> ofBoot([] { return State{}; });
        return ofBoot.get();
    }

    PauseLines addPauseLines(std::string &items, uint8_t *ids, bool restart, bool exit, bool photo, bool settings)
    {
        PauseLines out;
        // Lines are separated by the two characters \n (the UI script's
        // escape).
        const size_t last = items.rfind("\\n");
        size_t lines = 1;
        for (size_t at = items.find("\\n"); at != std::string::npos; at = items.find("\\n", at + 2))
            ++lines;
        // Ours come after the game's last line, End Run: RESTART first,
        // with its own action id after End Run's.
        if (restart && last != std::string::npos && ids && lines < 15 && ids[lines - 1] == kEndRunId &&
            items.size() + std::strlen(kRestartLine) < kItemsSize)
        {
            items += kRestartLine;
            ids[lines] = kRestartId;
            ++lines;
            ++out.added;
        }
        if (photo && lines < 16 && items.size() + std::strlen(kPhotoLine) < kItemsSize)
        {
            items += kPhotoLine;
            out.photoLine = static_cast<int>(lines);
            ++lines;
            ++out.added;
        }
        if (settings && lines < 16 && items.size() + std::strlen(kSettingsLine) < kItemsSize)
        {
            items += kSettingsLine;
            out.settingsLine = static_cast<int>(lines);
            ++lines;
            ++out.added;
        }
        const std::string exitLine = std::string("\\n") + zge_host::kExitItem;
        if (exit && lines < 16 && items.size() + exitLine.size() < kItemsSize)
        {
            items += exitLine;
            out.exitLine = static_cast<int>(lines);
            ++out.added;
        }
        return out;
    }

    uint32_t stackBelow(R5900Context *ctx)
    {
        return (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
    }

    bool variableNamed(const uint8_t *rdram, uint32_t name, const char *text)
    {
        const size_t length = std::strlen(text) + 1u;
        const uint8_t *p = name ? getConstMemPtr(rdram, name) : nullptr;
        return p && getConstMemPtr(rdram, name + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
               std::memcmp(p, text, length) == 0;
    }

    // The pause menu's widget tree's root (g_uiRoot's value), 0 if none.
    uint32_t uiRoot(const uint8_t *rdram)
    {
        uint32_t v = 0;
        if (const uint8_t *q = getConstMemPtr(rdram, kRootWidget))
            std::memcpy(&v, q, sizeof(v));
        return v;
    }

    // ---- Settings... by the engine's own path (zge_settings_open.h) ----------

    bool inLevel(const uint8_t *rdram)
    {
        const uint8_t *p = getConstMemPtr(rdram, kCurrentGameState);
        return p && *p == kGameStateLevel;
    }

    // Play runs: no screen (the pause menu, the round over) is up.
    bool playing(const uint8_t *rdram)
    {
        const uint8_t *p = getConstMemPtr(rdram, kGameScreen);
        return p && *p == kScreenPlay;
    }

    // Play's Start wait is over: the game's own Start check (action_frame.cpp).
    bool pausable(const uint8_t *rdram)
    {
        const uint8_t *step = getConstMemPtr(rdram, kActionStep);
        return step && *step == kPlayPausable;
    }

    // What the game's Start does (actionFrame's pauseGame): the sound's
    // paused mode, the pause screen up.
    void pauseAsStart(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const uint32_t sp = stackBelow(ctx);
        runtime->callGuestFunction(rdram, ctx, kSoundSetGameMode, {kSoundModePaused}, sp);
        runtime->callGuestFunction(rdram, ctx, kOpenOverlay, {kScreenPause}, sp);
    }

    constexpr zge_host::settings_open::Hooks kSettingsOpenHooks{"dave-mirra", &inLevel,      &playing,
                                                                nullptr,      &pausable,     &pauseAsStart,
                                                                &settings_menu::openFromPause};

    void showPauseScreenHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        State &st = state();
        st.building = true;
        st.showPauseScreen(rdram, ctx, runtime);
        st.building = false;
        // The menu is up: a Settings... pause gets its page in the menu's place.
        zge_host::settings_open::pauseMenuShown(rdram, ctx, runtime, uiRoot(rdram));
    }

    void setProperty(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        State &st = state();
        const uint32_t name = getRegU32(ctx, kRegA1);
        if (st.building && variableNamed(rdram, name, "Items"))
        {
            zge_host::sites::seen("dm2.pause.items");
            st.added = 0;
            st.photoLine = -1;
            st.exitLine = -1;
            st.settingsLine = -1;
            const uint32_t text = getRegU32(ctx, kRegA2);
            char *p = reinterpret_cast<char *>(getMemPtr(rdram, text));
            if (p)
            {
                const size_t length = strnlen(p, kItemsSize);
                std::string items(p, length);
                if (length < kItemsSize)
                {
                    const PauseLines added =
                        addPauseLines(items, getMemPtr(rdram, kActionIds), st.restart, st.collection, st.photo,
                                      st.settings);
                    st.added = added.added;
                    st.photoLine = added.photoLine;
                    st.exitLine = added.exitLine;
                    st.settingsLine = added.settingsLine;
                }
                if (st.added != 0)
                    std::memcpy(p, items.c_str(), items.size() + 1);
                if (st.photoLine >= 0)
                    std::cout << "[dave-mirra] pause menu: PHOTO MODE is line " << st.photoLine << std::endl;
            }
        }
        else if (st.building && st.added != 0 && variableNamed(rdram, name, "VisibleCount"))
        {
            if (uint8_t *count = getMemPtr(rdram, getRegU32(ctx, kRegA2)))
            {
                int32_t n = 0;
                std::memcpy(&n, count, sizeof(n));
                n += st.added;
                std::memcpy(count, &n, sizeof(n));
            }
        }
        st.setProperty(rdram, ctx, runtime);
    }

    void pauseHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        // The selected line, as the handler reads it.
        const uint32_t sp = stackBelow(ctx);
        uint32_t menu = 0;
        const uint32_t root = uiRoot(rdram);
        int32_t selected = -1;
        const uint32_t slot = sp + 0x100u;
        if (runtime->callGuestFunction(rdram, ctx, kFind, {root, kPauseMenuName}, sp, &menu) && menu != 0u &&
            runtime->callGuestFunction(rdram, ctx, kGetProperty, {menu, kSelectedIndexName, slot}, sp))
            if (const uint8_t *q = getConstMemPtr(rdram, slot))
                std::memcpy(&selected, q, sizeof(selected));
        if (selected >= 0 && selected == state().photoLine)
        {
            photo::requestFromPause();
            ctx->pc = getRegU32(ctx, kRegRa);
            return;
        }
        if (selected >= 0 && selected == state().settingsLine)
        {
            settings_menu::openFromPause(rdram, ctx, runtime, root);
            ctx->pc = getRegU32(ctx, kRegRa);
            return;
        }
        if (selected >= 0 && selected == state().exitLine)
        {
            zge_host::exitToCollection("dave-mirra", "the pause menu");
            ctx->pc = getRegU32(ctx, kRegRa);
            return;
        }
        const uint8_t *ids = getConstMemPtr(rdram, kActionIds);
        if (selected < 0 || selected >= 16 || !ids || ids[selected] != kRestartId)
        {
            state().pauseHandler(rdram, ctx, runtime);
            return;
        }
        std::cout << "[dave-mirra] restart" << std::endl;
        runtime->callGuestFunction(rdram, ctx, kOpenOverlay, {0u}, sp);
        runtime->callGuestFunction(rdram, ctx, kRound, {kTurnStartPhase}, sp);
        ctx->pc = getRegU32(ctx, kRegRa);
    }

    // The play mode's frame: a Restart key press runs in its place when a
    // turn can be started again; otherwise it is dropped. A Settings...
    // request pauses in its place, as the game's own Start does.
    void actionFrame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        State &st = state();
        if (st.keyPending)
        {
            st.keyPending = false;
            const uint8_t *phase = getConstMemPtr(rdram, kRoundPhase);
            const uint8_t *step = getConstMemPtr(rdram, kActionStep);
            if (phase && step && *step == kPlayPausable && (*phase == kFreeTurnPhase || *phase == kTurnRunningPhase))
            {
                std::cout << "[dave-mirra] restart (the Restart key, round phase " << int(*phase) << ")" << std::endl;
                runtime->callGuestFunction(rdram, ctx, kRound, {kTurnStartPhase}, stackBelow(ctx));
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
        st.actionFrame(rdram, ctx, runtime);
    }

    void requestKey()
    {
        state().keyPending = true;
    }
}

namespace dave_mirra::restart
{
    bool installPauseHooks(PS2Machine &runtime)
    {
        State &st = state();
        if (st.hooked)
            return true;
        st.setProperty = runtime.lookupFunction(kSetProperty);
        st.pauseHandler = runtime.lookupFunction(kPauseHandler);
        st.showPauseScreen = runtime.lookupFunction(kShowPauseScreen);
        if (!st.setProperty || !st.pauseHandler || !st.showPauseScreen ||
            !runtime.replaceFunction(kSetProperty, &setProperty) || !runtime.replaceFunction(kPauseHandler, &pauseHandler) ||
            !runtime.replaceFunction(kShowPauseScreen, &showPauseScreenHook))
        {
            std::cerr << "[dave-mirra] pause menu hooks not installed" << std::endl;
            return false;
        }
        st.hooked = true;
        return true;
    }

    void addPhotoLine(PS2Machine &runtime)
    {
        state().photo = true;
        installPauseHooks(runtime);
    }

    void addSettingsLine(PS2Machine &runtime)
    {
        state().settings = true;
        installPauseHooks(runtime);
        // With the page in the pause menu, the app's Settings... reaches it.
        zge_host::settings_open::install(runtime, kSettingsOpenHooks);
    }
}

namespace dave_mirra
{
    bool installRestartKey(PS2Machine &runtime)
    {
        using namespace restart;
        State &st = state();
        st.actionFrame = runtime.lookupFunction(kActionFrame);
        if (!st.actionFrame || !runtime.replaceFunction(kActionFrame, &restart::actionFrame))
        {
            std::cerr << "[dave-mirra] restart key hook not installed" << std::endl;
            return false;
        }
        return true;
    }

    bool installRestart(PS2Machine &runtime)
    {
        using namespace restart;
        State &st = state();
        const char *v = std::getenv("ZX_RESTART");
        st.restart = !(v && v[0] == '0');
        st.collection = zge_host::inCollection();
        if (!st.restart && !st.collection)
            return false;
        return restart::installPauseHooks(runtime);
    }
}
