// BMX XXX (SLUS-20415): Exit to Collection in the main menu and the pause
// menu, in the collection only (zge_host.h); the pause menu's RESTART line
// (unless PS2X_RESTART=0) and PHOTO MODE line (with photo mode); and the main
// menu's PARK EDITOR line (PS2X_BMX_PARK_EDITOR=1).
//
// Both menus get their lines from the game's code: a builder writes one
// "$KSTR_..." line per entry of an action-id list into a text and sets the
// menu's Items and VisibleCount (findNode, setVariable). For the main menu
// its builder and setVariable are wrapped: while the builder runs, its Items
// get an EXIT TO COLLECTION line at the end and its VisibleCount one more
// (the variables known by their names: both builders are readable code,
// whose calls from the host carry no return address of the original's);
// the action ids stay the game's (the line has none).
//
//   Main menu (UILIB.ZAL UI_menu.CommonBackground.FrameMainMenu.Menu,
//   built by mainMenuFrame from its list of ids):
//
//   PARK EDITOR: the game's own park editor menu (FrameParkEditor: NEW
//   PARK, LOAD PARK, RIDE PARK; front_end_menu.cpp kMenuParkEditor) is
//   whole in the retail game, and the handler takes the main menu's choice
//   kChoiceParkEditor to it, but the builder leaves that choice out (the
//   script's own Items list it as the third of six, $KSTR_COM_MENU_MAIN_OPT02,
//   with VisibleCount 6). The line is put back there: after MULTIPLAYER, its
//   text the game's (g_mainMenuTexts), its choice in g_mainMenuChoices with
//   the later ones moved down one. From there on it is the game's: NEW PARK
//   and a theme leave the front end for the editor (setGameState of
//   kGameStateParkEditor, as the Art Viewer's Park Editor Test), RIDE PARK
//   loads a saved park and rides it (kGameStateRidePark).
//
//   EXIT TO COLLECTION: its
//   OnItemSelect and OnChange are scripts that call bmxuiHandler; they get
//   a first line that sends the new line elsewhere (Select: CALL
//   pcExitToCollection, registered with the game's script functions after
//   bmxuiHandler, zge_exit.cpp; Change: nothing, as the line has no
//   picture or text of its own). Without those scripts (another UI_menu),
//   no line there.
//
//   Pause menu (UI_overlay.FramePause.Menu, built by its handler
//   pauseScreenMessage as the screen comes up: the lines' choices in
//   g_menuChoices, their KSTR_PAUSE_* names in g_pauseLineTexts): the
//   handler is wrapped; once it has made the menu, its Items are read back
//   (copyVariable), the new lines added and Items and VisibleCount set
//   again, so nothing is hooked that runs while no pause menu comes up. On
//   a line chosen with the new line selected the game is left instead.
//
//   The new lines come as one block after the game's last line, Exit Game,
//   which keeps its choice: RESTART, PHOTO MODE, SETTINGS, EXIT TO
//   COLLECTION; none has a choice of its own (its index is kept here).
//
//   RESTART (game/bmx/overlay/screen_pause.cpp): while a turn runs (the
//   round in its running phase), the first of them. Chosen, it does what the
//   game's own Play Again does, the turn again where Play Again starts the
//   game again: back to play (showGameScreen(kScreenPlay)) with the resume
//   action kResumeTurnStart, which sets the round's phase to a turn's
//   start as play comes back in (riders at the start, the turn's records
//   cleared, the clock set again).
//
//   PHOTO MODE (with photo mode, bmx_xxx_photo.cpp): after RESTART. Chosen,
//   photo mode starts in the pause and the pause menu is back after it.
//
// The lines the menus' builders leave out (PS2X_BMX_MENU_LINES=1, [extras]
// menu_lines): lines with the game's own texts and choices, whose handlers
// are whole, which no release's builder puts up. Each goes in among the
// game's lines where its choice comes in the handler's order, its choice in
// g_menuChoices (the later ones moved down), so the game's handler does
// what it says when it is chosen (game/bmx/overlay/screen_pause.cpp,
// screen_game_over.cpp):
//
//   Pause menu (pauseScreenMessage's kScreenEnter, as the other new lines):
//   TRICK LIST (kPauseTrickList: the trick list, back to the pause); in a
//   one-player game of the tour (g_sessionKind's kTourSession), CHANGE
//   RIDER after RECORDS and LEVEL SELECT before EXIT GAME (kPauseChangeRider,
//   kPauseLevelSelect: the level left, as the exit asks first, for the front
//   end's rider select or level select).
//
//   Game over (gameOverScreenMessage, wrapped; while it runs its Items and
//   VisibleCount, as the main menu's): in the tour, CHANGE RIDER after
//   LEVEL SELECT (kGameOverChangeRider, with the pause's text: the game has
//   no game over text for it), VIEW REPLAY (kGameOverViewReplay: the turn's
//   replay, the game over screen after it) and TRICK LIST after SAVE
//   (kGameOverTrickList), each with the game's KSTR_GAMEOVER_ text.
//   SAVE REPLAY (kGameOverSaveReplay) stays out: its screen
//   (saveReplayScreenMessage) starts the game's save, not the replay's.
//
//   Main menu (as the PARK EDITOR line above): PARK EDITOR once the
//   BULLETPOINT cheat has opened the saved game's feature 5, which the PS2's
//   STRING.ZAL calls THE PARK EDITOR (KSTRSP_REWARD_FEATURE05,
//   KSTR_FEATURENAME_05) and no code of the game reads; the Xbox's builder
//   puts its PARK EDITOR in on its own park editor feature, 7, which its
//   BULLETPOINT opens. With the park editor extra the line is there anyway.
#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_exit.cpp.h"
#include "zge_host.h"
#include "zge_settings_open.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_zaxis_ui.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace bmx_xxx::exit_menu
{
    namespace zx = ps2x::zaxis;

    constexpr zge_host::ScriptFunctions kScriptFunctions{
        "bmx-xxx",
        sym::bmx::registerScriptFunction,
        "bmxuiHandler",  // the menus' message handler, registered by the UI's setup (setUpUi)
        points::bmx::setUpUi::scriptFunctionSlots, // inside that setup, its registration calls: no entry points
        points::bmx::setUpUi::scriptFunctionSlotsEnd,
    };

    constexpr uint32_t kSetVariable = sym::bmx::setVariable;   // (node, name, value)
    constexpr uint32_t kCopyVariable = sym::bmx::copyVariable; // (node, name, out)
    constexpr uint32_t kPauseHandler = sym::bmx::pauseScreenMessage; // (message)
    constexpr uint32_t kMainMenuFrame = sym::bmx::mainMenuFrame; // the main menu's builder
    constexpr uint32_t kSelectedIndexName = sym::bmx::g_textSelectedIndex; // "SelectedIndex"
    // The builders' text buffers (their stack frames' first bytes).
    constexpr size_t kMainItemsSize = 0x100u;
    constexpr size_t kPauseItemsSize = 0x200u;
    constexpr uint32_t kMessageSelect = 0u; // a line chosen (game/bmx/overlay/screens.h kScreenChosen)
    constexpr uint32_t kMessageEnter = 10u; // the screen comes up: its lines are made (kScreenEnter)
    // The pause menu's RESTART (game/bmx/overlay/screens.h, game/bmx/game/round.h).
    constexpr uint32_t kScreenPlay = 1u;
    constexpr uint32_t kResumeTurnStart = 4u;
    constexpr uint8_t kTurnRunningPhase = 3u;
    // The round's state (RoundState), as far as its phase.
    struct RoundState
    {
        uint32_t flags;
        uint32_t phaseFramesLeft;
        uint32_t turnEndFrames;
        uint8_t phase;
    };
    static_assert(offsetof(RoundState, phase) == 0x0C);
    constexpr uint32_t kRoundPhase = sym::bmx::g_round + offsetof(RoundState, phase);
    constexpr size_t kMenuChoices = 12;
    // The main menu's choices (game/bmx/states/front_end_menus.h
    // MainMenuChoice) and the room of their table (g_mainMenuChoices, up to
    // the next global).
    constexpr uint8_t kChoiceMultiplayer = 1, kChoiceParkEditor = 2;
    constexpr size_t kMainMenuChoiceSlots = 16;
    // The saved game (game/bmx/profile/players.h SavedGame), as far as its
    // features: a bit per feature opened. The PS2's feature 5 is THE PARK
    // EDITOR (STRING.ZAL KSTRSP_REWARD_FEATURE05, KSTR_FEATURENAME_05), which
    // the BULLETPOINT cheat opens and no code of the game reads.
    struct SavedGame
    {
        uint16_t features;
        uint16_t featuresOn;
    };
    static_assert(offsetof(SavedGame, featuresOn) == 0x02);
    constexpr uint16_t kFeatureParkEditor = 1u << 5;
    // The lines the builders leave out (game/bmx/overlay/screen_pause.cpp
    // PauseChoice, screen_game_over.cpp GameOverChoice), their texts the
    // game's, and the menus' orders of choices.
    constexpr uint8_t kPauseChangeRider = 5, kPauseTrickList = 7, kPauseLevelSelect = 9;
    constexpr uint8_t kPauseOrder[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    constexpr uint8_t kGameOverChangeRider = 2, kGameOverViewReplay = 5, kGameOverTrickList = 6;
    constexpr uint8_t kGameOverOrder[] = {0, 1, 2, 3, 4, 5, 10, 6, 7, 8};
    constexpr const char *kGameOverChangeRiderText = "$KSTR_PAUSE_CHANGERIDER";
    constexpr const char *kGameOverViewReplayText = "$KSTR_GAMEOVER_VIEWREPLAY";
    constexpr const char *kGameOverTrickListText = "$KSTR_GAMEOVER_TRICKLIST";
    constexpr uint32_t kGameOverHandler = sym::bmx::gameOverScreenMessage; // (message)
    constexpr size_t kGameOverItemsSize = 0x200u; // its Items text buffer
    // g_sessionKind: a game of the tour (one player, a level from the select).
    constexpr uint8_t kTourSession = 0;
    constexpr const char *kRestartLine = "RESTART";
    constexpr const char *kPhotoLine = "PHOTO MODE";
    constexpr const char *kSettingsLine = "SETTINGS";
    constexpr int kRegA0 = 4, kRegA1 = 5, kRegA2 = 6, kRegSp = 29, kRegRa = 31;

    // One boot's originals and the pause menu as last built.
    struct State
    {
        PS2Runtime::RecompiledFunction setVariable = nullptr;
        PS2Runtime::RecompiledFunction pauseHandler = nullptr;
        bool mainMenuScript = false; // the main menu's scripts patched: its line can be chosen
        PS2Runtime::RecompiledFunction mainMenuFrame = nullptr;
        bool building = false;  // the main menu's builder is running
        bool mainMenuHooked = false; // the main menu's builder and setVariable wrapped
        bool setVariableHooked = false; // setVariable wrapped (the main menu's and the game over's builders)
        bool menuLines = false;  // the lines the builders leave out wanted (PS2X_BMX_MENU_LINES=1)
        PS2Runtime::RecompiledFunction gameOverHandler = nullptr;
        bool gameOverBuilding = false; // gameOverScreenMessage is making its menu
        int gameOverAdded = 0;  // lines it got
        bool parkEditor = false; // the PARK EDITOR line wanted (PS2X_BMX_PARK_EDITOR=1)
        bool restart = false;   // the RESTART line wanted (PS2X_RESTART is not 0)
        bool collection = false; // Exit to Collection wanted (in the collection)
        bool photo = false;     // the Photo Mode line wanted (photo mode installed)
        bool settings = false;  // the Settings line wanted (the Settings page installed)
        bool pauseHooked = false; // the pause handler wrapped
        uint32_t pauseMenu = 0; // its node
        int pauseRestartLine = -1; // the lines' indexes there, -1: none
        int pauseExitLine = -1;
        int pausePhotoLine = -1;
        int pauseSettingsLine = -1;
        int countAdded = 0;     // lines added to the menu being built
    };
    State &state() { return ps2x::session::state<State>(); }

    int addExitLine(char *p, size_t size)
    {
        if (!p)
            return -1;
        const size_t length = strnlen(p, size);
        const std::string line = std::string(length == 0 ? "" : "\\n") + zge_host::kExitItem;
        if (length >= size || length + line.size() >= size)
            return -1;
        int lines = length == 0 ? 0 : 1;
        for (const char *at = std::strstr(p, "\\n"); at; at = std::strstr(at + 2, "\\n"))
            ++lines;
        std::memcpy(p + length, line.c_str(), line.size() + 1);
        return lines;
    }

    MainMenuLines addMainMenuLines(char *p, size_t size, uint8_t *choices, const char *parkEditorText, bool exit)
    {
        MainMenuLines out;
        if (!p)
            return out;
        const size_t length = strnlen(p, size);
        if (length >= size)
            return out;
        std::string text(p, length);
        int lines = length == 0 ? 0 : 1;
        for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 2))
            ++lines;
        // PARK EDITOR after MULTIPLAYER, where the script's own list has it.
        int multiplayer = -1;
        for (int line = 0; choices && line < lines && line < static_cast<int>(kMainMenuChoiceSlots); ++line)
            if (choices[line] == kChoiceMultiplayer)
                multiplayer = line;
        const std::string parkLine = std::string("\\n") + (parkEditorText ? parkEditorText : "");
        if (parkEditorText && parkEditorText[0] && multiplayer >= 0 && lines < static_cast<int>(kMainMenuChoiceSlots) &&
            text.size() + parkLine.size() < size)
        {
            size_t at = 0;
            for (int line = 0; line <= multiplayer; ++line)
            {
                at = text.find("\\n", line == 0 ? 0 : at + 2);
                if (at == std::string::npos)
                    break;
            }
            text.insert(at == std::string::npos ? text.size() : at, parkLine);
            for (int line = lines; line > multiplayer + 1; --line)
                choices[line] = choices[line - 1];
            choices[multiplayer + 1] = kChoiceParkEditor;
            out.parkEditorLine = multiplayer + 1;
            ++lines;
            ++out.added;
        }
        const std::string exitLine = std::string(text.empty() ? "" : "\\n") + zge_host::kExitItem;
        if (exit && text.size() + exitLine.size() < size)
        {
            text += exitLine;
            out.exitLine = lines;
            ++out.added;
        }
        if (out.added != 0)
            std::memcpy(p, text.c_str(), text.size() + 1);
        return out;
    }

    PauseLines addPauseLines(char *p, size_t size, bool restart, bool exit, bool photo, bool settings)
    {
        PauseLines out;
        if (!p)
            return out;
        const size_t length = strnlen(p, size);
        if (length >= size)
            return out;
        std::string text(p, length);
        int lines = length == 0 ? 0 : 1;
        for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 2))
            ++lines;
        // Ours after the game's last line, Exit Game, which keeps its choice:
        // RESTART first (no choice of its own: its index is kept, as the
        // other new lines' are).
        const std::string restartLine = std::string(text.empty() ? "" : "\\n") + kRestartLine;
        if (restart && lines > 0 && text.size() + restartLine.size() < size)
        {
            text += restartLine;
            out.restartLine = lines;
            ++lines;
            ++out.added;
        }
        // PHOTO MODE after RESTART (no choice: its index is kept).
        const std::string photoLine = std::string(text.empty() ? "" : "\\n") + kPhotoLine;
        if (photo && text.size() + photoLine.size() < size)
        {
            text += photoLine;
            out.photoLine = lines;
            ++lines;
            ++out.added;
        }
        // SETTINGS after those (no choice: its index is kept).
        const std::string settingsLine = std::string(text.empty() ? "" : "\\n") + kSettingsLine;
        if (settings && text.size() + settingsLine.size() < size)
        {
            text += settingsLine;
            out.settingsLine = lines;
            ++lines;
            ++out.added;
        }
        const std::string exitLine = std::string(text.empty() ? "" : "\\n") + zge_host::kExitItem;
        if (exit && text.size() + exitLine.size() < size)
        {
            text += exitLine;
            out.exitLine = lines;
            ++out.added;
        }
        if (out.added != 0)
            std::memcpy(p, text.c_str(), text.size() + 1);
        return out;
    }

    int insertMenuLines(std::string &items, uint8_t *choices, int count, size_t room, const std::vector<MenuLine> &added,
                        const uint8_t *order, size_t orderCount)
    {
        if (!choices || count < 0 || static_cast<size_t>(count) > room)
            return 0;
        std::vector<std::string> lines;
        if (count > 0)
        {
            size_t at = 0;
            for (size_t next = items.find("\\n"); next != std::string::npos; next = items.find("\\n", at))
            {
                lines.push_back(items.substr(at, next - at));
                at = next + 2;
            }
            lines.push_back(items.substr(at));
        }
        if (lines.size() != static_cast<size_t>(count))
            return 0; // (not one line a choice: left as it is)
        auto rank = [&](uint8_t choice)
        {
            for (size_t i = 0; i < orderCount; ++i)
                if (order[i] == choice)
                    return static_cast<int>(i);
            return static_cast<int>(orderCount);
        };
        int inserted = 0;
        for (const MenuLine &line : added)
        {
            bool there = false;
            for (int i = 0; i < count; ++i)
                there = there || choices[i] == line.choice;
            if (there || static_cast<size_t>(count) >= room)
                continue;
            int at = count;
            for (int i = 0; i < count; ++i)
                if (rank(choices[i]) > rank(line.choice))
                {
                    at = i;
                    break;
                }
            for (int i = count; i > at; --i)
                choices[i] = choices[i - 1];
            choices[at] = line.choice;
            lines.insert(lines.begin() + at, line.text);
            ++count;
            ++inserted;
        }
        if (inserted == 0)
            return 0;
        items.clear();
        for (size_t i = 0; i < lines.size(); ++i)
            items += (i == 0 ? "" : "\\n") + lines[i];
        return inserted;
    }

    char *guestText(uint8_t *rdram, uint32_t address) { return reinterpret_cast<char *>(getMemPtr(rdram, address)); }

    void addToCount(uint8_t *rdram, uint32_t address, int added)
    {
        if (uint8_t *count = getMemPtr(rdram, address))
        {
            int32_t n = 0;
            std::memcpy(&n, count, sizeof(n));
            n += added;
            std::memcpy(count, &n, sizeof(n));
        }
    }

    uint8_t readByte(const uint8_t *rdram, uint32_t address)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        return p ? *p : 0u;
    }

    // A zgui variable's name (setVariable's second word) is this text.
    bool variableNamed(const uint8_t *rdram, uint32_t name, const char *text)
    {
        const size_t length = std::strlen(text) + 1u;
        const uint8_t *p = name ? getConstMemPtr(rdram, name) : nullptr;
        return p && getConstMemPtr(rdram, name + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
               std::memcmp(p, text, length) == 0;
    }

    // The PARK EDITOR line is wanted: with the park editor extra, or with the
    // left-out lines once BULLETPOINT has opened feature 5.
    bool wantParkEditorLine(const uint8_t *rdram)
    {
        const State &st = state();
        if (st.parkEditor)
            return true;
        uint16_t features = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_savedGame + offsetof(SavedGame, features)))
            std::memcpy(&features, p, sizeof(features));
        return st.menuLines && (features & kFeatureParkEditor) != 0;
    }

    // The main menu's Items and VisibleCount while its builder runs.
    void setVariable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        const uint32_t name = getRegU32(ctx, kRegA1);
        if (st.building && (st.mainMenuScript || st.parkEditor || st.menuLines) && variableNamed(rdram, name, "Items"))
        {
            // The PARK EDITOR line's text, the game's own for its choice.
            std::string parkEditorText;
            uint32_t textAt = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_mainMenuTexts + 4u * kChoiceParkEditor))
                std::memcpy(&textAt, p, sizeof(textAt));
            if (const char *t = textAt ? reinterpret_cast<const char *>(getConstMemPtr(rdram, textAt)) : nullptr)
                parkEditorText.assign(t, strnlen(t, 64));
            const MainMenuLines added =
                addMainMenuLines(guestText(rdram, getRegU32(ctx, kRegA2)), kMainItemsSize,
                                 getMemPtr(rdram, sym::bmx::g_mainMenuChoices),
                                 wantParkEditorLine(rdram) ? parkEditorText.c_str() : nullptr, st.mainMenuScript);
            st.countAdded = added.added;
        }
        else if (st.building && st.countAdded != 0 && variableNamed(rdram, name, "VisibleCount"))
        {
            addToCount(rdram, getRegU32(ctx, kRegA2), st.countAdded);
            st.countAdded = 0;
        }
        else if (st.gameOverBuilding && variableNamed(rdram, name, "Items"))
        {
            // The game over's lines and choices as the game made them, the
            // left-out lines put in (the count of lines read from the text).
            char *items = guestText(rdram, getRegU32(ctx, kRegA2));
            uint8_t *choices = getMemPtr(rdram, sym::bmx::g_menuChoices);
            if (items && choices)
            {
                std::string text(items, strnlen(items, kGameOverItemsSize));
                int lines = text.empty() ? 0 : 1;
                for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 2))
                    ++lines;
                const std::vector<MenuLine> wanted = {{kGameOverChangeRider, kGameOverChangeRiderText},
                                                      {kGameOverViewReplay, kGameOverViewReplayText},
                                                      {kGameOverTrickList, kGameOverTrickListText}};
                const int added = insertMenuLines(text, choices, lines, kMenuChoices, wanted, kGameOverOrder,
                                                  sizeof(kGameOverOrder));
                if (added != 0 && text.size() < kGameOverItemsSize)
                {
                    std::memcpy(items, text.c_str(), text.size() + 1);
                    st.gameOverAdded = added;
                }
            }
        }
        else if (st.gameOverBuilding && st.gameOverAdded != 0 && variableNamed(rdram, name, "VisibleCount"))
        {
            addToCount(rdram, getRegU32(ctx, kRegA2), st.gameOverAdded);
            st.gameOverAdded = 0;
        }
        st.setVariable(rdram, ctx, runtime);
    }

    // The game over screen's handler: as it comes up in a game of the tour,
    // its menu made with the left-out lines.
    void gameOverHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        const bool building = getRegU32(ctx, kRegA0) == kMessageEnter && readByte(rdram, sym::bmx::g_sessionKind) == kTourSession;
        st.gameOverBuilding = building;
        st.gameOverAdded = 0;
        st.gameOverHandler(rdram, ctx, runtime);
        st.gameOverBuilding = false;
    }

    // The main menu's builder, the menu being built while it runs.
    void mainMenuFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        st.building = true;
        st.countAdded = 0;
        st.mainMenuFrame(rdram, ctx, runtime);
        st.building = false;
    }

    // The pause menu just made by the game (its handler's kScreenEnter): its
    // Items read back, the new lines added, and Items and VisibleCount set
    // again, as the handler set them (zgui's copyVariable and setVariable).
    void addToPauseMenu(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        st.pauseRestartLine = st.pauseExitLine = st.pausePhotoLine = st.pauseSettingsLine = -1;
        // RESTART while a turn runs (the game's Play Again stands in its place once it is over).
        const bool restart = st.restart && readByte(rdram, kRoundPhase) == kTurnRunningPhase;
        if (!restart && !st.collection && !st.photo && !st.settings && !st.menuLines)
            return;
        // Guest memory below the stack for the calls, the text and the count.
        const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x600u) & ~0xFu;
        const uint32_t text = sp + 0x100u, count = sp + 0x100u + static_cast<uint32_t>(kPauseItemsSize);
        uint32_t overlay = 0, frame = 0, menu = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_overlayUi))
            std::memcpy(&overlay, p, sizeof(overlay));
        if (!overlay ||
            !runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {overlay, sym::bmx::g_textFramePause, 1u}, sp, &frame) ||
            !frame ||
            !runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {frame, sym::bmx::g_textMenu, 1u}, sp, &menu) ||
            !menu)
            return;
        // The Items text as the handler made it: a "$KSTR_PAUSE_..." line per
        // choice (its "%s$%s" with no separator first, then "\\n"), from the
        // game's choices and their names. (A menu's Items cannot be read
        // back whole: copyVariable gives its first line.)
        int32_t visible = 0;
        if (!runtime->callGuestFunction(rdram, ctx, kCopyVariable, {menu, sym::bmx::g_textVisibleCount, count}, sp))
            return;
        if (const uint8_t *p = getConstMemPtr(rdram, count))
            std::memcpy(&visible, p, sizeof(visible));
        uint8_t *choices = getMemPtr(rdram, sym::bmx::g_menuChoices);
        if (!choices || visible <= 0 || visible > static_cast<int32_t>(kMenuChoices))
            return;
        // A choice's line: "$" and its KSTR_PAUSE_ name (g_pauseLineTexts).
        auto lineOf = [&](uint8_t choice) -> std::string
        {
            uint32_t name = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_pauseLineTexts + 4u * choice))
                std::memcpy(&name, p, sizeof(name));
            const char *key = name ? reinterpret_cast<const char *>(getConstMemPtr(rdram, name)) : nullptr;
            return key ? "$" + std::string(key, strnlen(key, 64)) : std::string();
        };
        std::string made;
        for (int32_t line = 0; line < visible; ++line)
        {
            const std::string key = lineOf(choices[line]);
            if (key.empty())
                return;
            made += std::string(line == 0 ? "" : "\\n") + key;
        }
        // The left-out lines among the game's (their choices the game's).
        int inserted = 0;
        if (st.menuLines)
        {
            std::vector<MenuLine> wanted;
            const bool tour = readByte(rdram, sym::bmx::g_sessionKind) == kTourSession;
            for (uint8_t choice : {kPauseChangeRider, kPauseTrickList, kPauseLevelSelect})
                if (choice == kPauseTrickList || tour)
                    wanted.push_back({choice, lineOf(choice)});
            inserted = insertMenuLines(made, choices, visible, kMenuChoices, wanted, kPauseOrder, sizeof(kPauseOrder));
        }
        char *items = guestText(rdram, text);
        if (!items || made.size() >= kPauseItemsSize)
            return;
        std::memcpy(items, made.c_str(), made.size() + 1);
        PauseLines added = addPauseLines(items, kPauseItemsSize, restart, st.collection, st.photo, st.settings);
        if (added.added == 0 && inserted == 0)
            return;
        if (uint8_t *p = getMemPtr(rdram, count))
        {
            const int32_t lines = visible + inserted;
            std::memcpy(p, &lines, sizeof(lines));
        }
        st.pauseMenu = menu;
        st.pauseRestartLine = added.restartLine;
        st.pauseExitLine = added.exitLine;
        st.pausePhotoLine = added.photoLine;
        st.pauseSettingsLine = added.settingsLine;
        addToCount(rdram, count, added.added);
        runtime->callGuestFunction(rdram, ctx, kSetVariable, {menu, sym::bmx::g_textItems, text}, sp);
        runtime->callGuestFunction(rdram, ctx, kSetVariable, {menu, sym::bmx::g_textVisibleCount, count}, sp);
        if (added.restartLine >= 0)
            std::cout << "[bmx-xxx] pause menu: RESTART is line " << added.restartLine << std::endl;
        if (added.photoLine >= 0)
            std::cout << "[bmx-xxx] pause menu: PHOTO MODE is line " << added.photoLine << std::endl;
    }

    void pauseHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        if (getRegU32(ctx, kRegA0) == kMessageEnter)
        {
            st.pauseHandler(rdram, ctx, runtime);
            addToPauseMenu(rdram, ctx, runtime);
            // The menu is up: a Settings... pause (bmx_xxx_restart.cpp) gets its page in the menu's place.
            uint32_t overlay = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_overlayUi))
                std::memcpy(&overlay, p, sizeof(overlay));
            zge_host::settings_open::pauseMenuShown(rdram, ctx, runtime, overlay);
            return;
        }
        if (getRegU32(ctx, kRegA0) == kMessageSelect &&
            (st.pauseExitLine >= 0 || st.pauseRestartLine >= 0 || st.pausePhotoLine >= 0 || st.pauseSettingsLine >= 0) &&
            st.pauseMenu != 0u)
        {
            const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
            const uint32_t slot = sp + 0x100u;
            int32_t selected = -1;
            if (runtime->callGuestFunction(rdram, ctx, kCopyVariable, {st.pauseMenu, kSelectedIndexName, slot}, sp))
                if (const uint8_t *q = getConstMemPtr(rdram, slot))
                    std::memcpy(&selected, q, sizeof(selected));
            if (selected >= 0 && selected == st.pausePhotoLine)
            {
                // Photo mode in this pause, the menu back after it.
                photo::requestFromPause();
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            if (selected >= 0 && selected == st.pauseSettingsLine)
            {
                uint32_t overlay = 0;
                if (const uint8_t *p = getConstMemPtr(rdram, sym::bmx::g_overlayUi))
                    std::memcpy(&overlay, p, sizeof(overlay));
                settings_menu::openFromPause(rdram, ctx, runtime, overlay);
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            if (selected >= 0 && selected == st.pauseExitLine)
            {
                zge_host::exitToCollection("bmx-xxx", "the pause menu");
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
            if (selected >= 0 && selected == st.pauseRestartLine)
            {
                // As the game's Play Again, with a turn's start for a new game's.
                std::cout << "[bmx-xxx] restart (the pause menu)" << std::endl;
                runtime->callGuestFunction(rdram, ctx, sym::bmx::showGameScreen, {kScreenPlay}, sp);
                if (uint8_t *resume = getMemPtr(rdram, sym::bmx::g_resumeAction))
                    std::memcpy(resume, &kResumeTurnStart, sizeof(kResumeTurnStart));
                ctx->pc = getRegU32(ctx, kRegRa);
                return;
            }
        }
        st.pauseHandler(rdram, ctx, runtime);
    }

    // The main menu's select and change scripts: the new line first.
    bool patchMainMenu(std::string &text)
    {
        std::string edited = text;
        zx::Span menu = zx::findObject(edited, "UI_menu/CommonBackground/FrameMainMenu/Menu");
        if (!menu.found())
            return false;
        const std::string p(static_cast<size_t>(zx::indentAt(edited, menu.begin) + 1), '\t');
        const std::string t = p + "\t";
        const std::string isExit = t + "IF SelectedText == {\"" + zge_host::kExitItem + "\"} GOTO ";
        const bool ok =
            zx::replaceIn(edited, menu,
                          p + "OnItemSelect\t{\r\n" + t + "SET Root.MsgType {0}\r\n" + t + "CALL bmxuiHandler\r\n" + t + "}\r\n",
                          p + "OnItemSelect\t{\r\n" + isExit + "PcExit\r\n" + t + "SET Root.MsgType {0}\r\n" + t +
                              "CALL bmxuiHandler\r\n" + t + "GOTO PcEnd\r\n" + t + ":PcExit\r\n" + t + "CALL " +
                              zge_host::kExitFunction + "\r\n" + t + ":PcEnd\r\n" + t + "}\r\n") &&
            zx::replaceIn(edited, menu,
                          p + "OnChange\t{\r\n" + t + "SET Root.MsgType {2}\r\n" + t + "CALL bmxuiHandler\r\n" + t + "}\r\n",
                          p + "OnChange\t{\r\n" + isExit + "PcEnd\r\n" + t + "SET Root.MsgType {2}\r\n" + t +
                              "CALL bmxuiHandler\r\n" + t + ":PcEnd\r\n" + t + "}\r\n");
        if (!ok)
            return false;
        text = std::move(edited);
        return true;
    }

    bool patchUiLibrary(std::vector<uint8_t> &bytes)
    {
        std::vector<zx::ArchiveEntry> entries;
        if (!zx::readArchive(bytes, entries))
            return false;
        bool changed = false;
        for (zx::ArchiveEntry &entry : entries)
        {
            if (zx::scriptName(entry) != "UI_menu")
                continue;
            std::string text(entry.data.begin(), entry.data.end());
            if (patchMainMenu(text))
            {
                entry.data.assign(text.begin(), text.end());
                changed = true;
                state().mainMenuScript = true;
                std::cout << "[bmx-xxx] main menu: " << zge_host::kExitItem << std::endl;
            }
            else
            {
                std::cerr << "[bmx-xxx] UI_menu is not the expected script: no " << zge_host::kExitItem
                          << " in the main menu" << std::endl;
            }
        }
        if (changed)
            bytes = zx::writeArchive(entries);
        return changed;
    }

    // setVariable wrapped, once (the builders' Items and VisibleCount).
    bool installSetVariableHook(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.setVariableHooked)
            return true;
        st.setVariable = runtime.lookupFunction(kSetVariable);
        if (!st.setVariable || !runtime.replaceFunction(kSetVariable, &setVariable))
            return false;
        st.setVariableHooked = true;
        return true;
    }

    // The main menu's builder and setVariable wrapped, once.
    bool installMainMenuHooks(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.mainMenuHooked)
            return true;
        st.mainMenuFrame = runtime.lookupFunction(kMainMenuFrame);
        if (!st.mainMenuFrame || !installSetVariableHook(runtime) || !runtime.replaceFunction(kMainMenuFrame, &mainMenuFrame))
            return false;
        st.mainMenuHooked = true;
        return true;
    }

    // The game over's handler and setVariable wrapped, once.
    bool installGameOverHook(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.gameOverHandler)
            return true;
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kGameOverHandler);
        if (!original || !installSetVariableHook(runtime) || !runtime.replaceFunction(kGameOverHandler, &gameOverHandler))
            return false;
        st.gameOverHandler = original;
        return true;
    }

    // The pause menu's handler wrapped, once.
    bool installPauseHook(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.pauseHooked)
            return true;
        st.pauseHandler = runtime.lookupFunction(kPauseHandler);
        if (!st.pauseHandler || !runtime.replaceFunction(kPauseHandler, &pauseHandler))
            return false;
        st.pauseHooked = true;
        return true;
    }

    void addSettingsLine(PS2Runtime &runtime)
    {
        state().settings = true;
        if (!installPauseHook(runtime))
            std::cerr << "[bmx-xxx] pause menu hooks not installed: no SETTINGS line" << std::endl;
        // With the page in the pause menu, the app's Settings... reaches it.
        restart::installSettingsOpen(runtime);
    }

    void addPhotoLine(PS2Runtime &runtime)
    {
        state().photo = true;
        if (!installPauseHook(runtime))
            std::cerr << "[bmx-xxx] pause menu hooks not installed: no PHOTO MODE line" << std::endl;
    }
}

namespace bmx_xxx
{
    bool installExitToCollection(PS2Runtime &runtime)
    {
        using namespace exit_menu;
        installParkEditorLine(runtime);
        installMenuLines(runtime);
        State &st = state();
        const char *restart = std::getenv("PS2X_RESTART");
        st.restart = !(restart && restart[0] == '0');
        st.collection = zge_host::inCollection();
        if (!st.restart && !st.collection)
            return false;
        // (the main menu's builder and setVariable only for its Exit to Collection line)
        if (!installPauseHook(runtime) || (st.collection && !installMainMenuHooks(runtime)))
        {
            std::cerr << "[bmx-xxx] pause menu hooks not installed" << std::endl;
            return false;
        }
        if (!st.collection)
            return true;
        // The pause line works without a script function; the main menu's
        // needs one.
        if (zge_host::installExitFunction(runtime, kScriptFunctions))
            ps2x::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        return true;
    }
    const zge_host::ScriptFunctions &scriptFunctions() { return exit_menu::kScriptFunctions; }

    bool installParkEditorLine(PS2Runtime &runtime)
    {
        using namespace exit_menu;
        const char *on = ps2x::boot::text("PS2X_BMX_PARK_EDITOR");
        if (!on || on[0] != '1')
            return false;
        if (!installMainMenuHooks(runtime))
        {
            std::cerr << "[bmx-xxx] main menu hooks not installed: no PARK EDITOR line" << std::endl;
            return false;
        }
        state().parkEditor = true;
        std::cout << "[bmx-xxx] main menu: PARK EDITOR" << std::endl;
        return true;
    }

    bool installMenuLines(PS2Runtime &runtime)
    {
        using namespace exit_menu;
        const char *on = ps2x::boot::text("PS2X_BMX_MENU_LINES");
        if (!on || on[0] != '1')
            return false;
        if (!installPauseHook(runtime) || !installGameOverHook(runtime) || !installMainMenuHooks(runtime))
        {
            std::cerr << "[bmx-xxx] pause or game over hooks not installed: no left-out menu lines" << std::endl;
            return false;
        }
        state().menuLines = true;
        std::cout << "[bmx-xxx] pause menu: TRICK LIST, CHANGE RIDER, LEVEL SELECT; game over: CHANGE RIDER, VIEW REPLAY, "
                     "TRICK LIST; main menu: PARK EDITOR once BULLETPOINT is entered"
                  << std::endl;
        return true;
    }
}
