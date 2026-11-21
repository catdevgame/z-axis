// The collection's shell (collection/menu.h): its states (menu, game,
// quit), the menu's selection and input, the scripted menu input, and the
// pictures and font it reads from the games' discs.
#include "MiniTest.h"

#include "collection/menu.h"
#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    namespace col = zx::collection;

    col::MenuInput key(const char *name)
    {
        return col::menuInput({ps2_host_input::hostCodeByName(name)}, 0);
    }

    col::MenuInput pad(int button)
    {
        return col::menuInput({}, static_cast<uint16_t>(1u << button));
    }

    void put16(std::vector<uint8_t> &out, size_t at, uint16_t value)
    {
        out[at] = static_cast<uint8_t>(value);
        out[at + 1] = static_cast<uint8_t>(value >> 8);
    }

    void put32(std::vector<uint8_t> &out, size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            out[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    // An 8-bit BMP, w x h, bottom-up rows, palette entries given as B G R A.
    std::vector<uint8_t> bmp8(int w, int h, const std::vector<uint32_t> &paletteBgra, const std::vector<uint8_t> &topDown)
    {
        const size_t stride = ((static_cast<size_t>(w) * 8 + 31) / 32) * 4;
        const size_t pixels = 14 + 40 + 4 * 256;
        std::vector<uint8_t> out(pixels + stride * h, 0);
        out[0] = 'B';
        out[1] = 'M';
        put32(out, 10, static_cast<uint32_t>(pixels));
        put32(out, 14, 40);
        put32(out, 18, static_cast<uint32_t>(w));
        put32(out, 22, static_cast<uint32_t>(h));
        put16(out, 28, 8);
        for (size_t i = 0; i < paletteBgra.size(); ++i)
            put32(out, 54 + 4 * i, paletteBgra[i]);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out[pixels + stride * (h - 1 - y) + x] = topDown[static_cast<size_t>(y) * w + x];
        return out;
    }

    // A font file of one page: header, page record, glyph records.
    std::vector<uint8_t> fontFile(int height, const std::vector<std::vector<int>> &glyphs)
    {
        std::vector<uint8_t> out(6 + 0x28 + 12 * glyphs.size(), 0);
        out[0] = static_cast<uint8_t>(height);
        put16(out, 2, static_cast<uint16_t>(glyphs.size()));
        put16(out, 4, 1);
        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            const std::vector<int> &g = glyphs[i]; // code, top, u, v, w, h
            const size_t at = 6 + 0x28 + 12 * i;
            out[at] = static_cast<uint8_t>(g[0]);
            put16(out, at + 2, static_cast<uint16_t>(g[1]));
            put16(out, at + 4, static_cast<uint16_t>(g[2]));
            put16(out, at + 6, static_cast<uint16_t>(g[3]));
            put16(out, at + 8, static_cast<uint16_t>(g[4]));
            put16(out, at + 10, static_cast<uint16_t>(g[5]));
        }
        return out;
    }

    col::Image solid(int w, int h, uint32_t colour)
    {
        col::Image image;
        image.width = w;
        image.height = h;
        image.pixels.assign(static_cast<size_t>(w) * h, colour);
        return image;
    }
}

void register_ps2_collection_tests()
{
    MiniTest::Case("CollectionShell", [](TestCase &tc)
    {
        tc.Run("menu -> game -> menu -> another game -> menu -> quit", [](TestCase &t)
        {
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            shell.start();
            t.IsTrue(shell.state() == col::State::Menu, "the app opens on the menu");
            t.Equals(shell.selection(), 0, "on the first game");
            t.Equals(shell.lastGame(), -1, "none played yet");

            col::MenuAction action = shell.input(key("Enter"), layout);
            t.IsTrue(action.kind == col::MenuAction::Boot && action.game == 0, "Enter boots the first game");
            t.IsTrue(shell.boot(action.game), "booted");
            t.IsTrue(shell.state() == col::State::Game && shell.game() == 0, "in game 0");
            t.IsTrue(!shell.boot(1), "no second boot while a game runs");
            t.IsTrue(shell.input(key("Enter"), layout).kind == col::MenuAction::None, "no menu input in a game");

            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.IsTrue(shell.state() == col::State::Menu, "back on the menu");
            t.Equals(shell.game(), -1, "no game running");
            t.Equals(shell.selection(), 0, "on the game just played");

            shell.input(key("Right"), layout);
            t.Equals(shell.selection(), 1, "Right: the next game");
            action = shell.input(pad(ps2_host_input::kCross), layout);
            t.IsTrue(action.kind == col::MenuAction::Boot && action.game == 1, "Cross boots it");
            t.IsTrue(shell.boot(1), "booted");
            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.Equals(shell.selection(), 1, "back on the second game");

            shell.input(pad(ps2_host_input::kRight), layout);
            action = shell.input(pad(ps2_host_input::kStart), layout);
            t.IsTrue(action.kind == col::MenuAction::Boot && action.game == 2, "D-pad right and Start: the third");
            shell.boot(2);
            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.Equals(shell.boots(), 3, "three boots");

            shell.input(key("Escape"), layout);
            t.Equals(shell.selection(), shell.quitItem(), "Escape: Quit");
            action = shell.input(key("Enter"), layout);
            t.IsTrue(action.kind == col::MenuAction::Quit, "Enter on Quit quits");
            shell.quit();
            t.IsTrue(shell.state() == col::State::Quit, "the app ends");
        });

        tc.Run("a window close in a game quits; a failed boot comes back to the menu", [](TestCase &t)
        {
            col::Shell shell(3);
            shell.start(2);
            t.IsTrue(shell.state() == col::State::Game && shell.game() == 2, "--game: straight into the game");
            shell.gameEnded(col::GameEnd::Failed);
            t.IsTrue(shell.state() == col::State::Menu, "a game that does not boot: the menu");
            t.Equals(shell.selection(), 2, "on that game");
            shell.boot(0);
            shell.gameEnded(col::GameEnd::Quit);
            t.IsTrue(shell.state() == col::State::Quit, "window closed in a game: quit");
            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.IsTrue(shell.state() == col::State::Quit, "and it stays so");
            col::Shell bad(3);
            bad.start(7);
            t.IsTrue(bad.state() == col::State::Menu, "an unknown game: the menu");
        });

        tc.Run("the selection stays on the menu's items", [](TestCase &t)
        {
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            shell.start();
            shell.input(key("Left"), layout);
            t.Equals(shell.selection(), 0, "Left on the first stays");
            shell.input(key("Right"), layout);
            shell.input(key("Right"), layout);
            shell.input(key("Right"), layout);
            t.Equals(shell.selection(), 2, "Right on the last stays");
            shell.input(key("Escape"), layout);
            t.Equals(shell.selection(), shell.quitItem(), "Escape: to Quit, not quitting");
            shell.input(key("Right"), layout);
            t.Equals(shell.selection(), shell.quitItem(), "Right on Quit stays");
            shell.input(key("Up"), layout);
            t.Equals(shell.selection(), shell.quitItem(), "Up on Quit stays (it is at the top)");
            shell.input(key("Down"), layout);
            t.Equals(shell.selection(), 0, "Down from Quit before any game: the first");
            shell.boot(1);
            shell.gameEnded(col::GameEnd::ExitToMenu);
            shell.input(pad(ps2_host_input::kCircle), layout);
            shell.input(pad(ps2_host_input::kDown), layout);
            t.Equals(shell.selection(), 1, "Down from Quit: the game last played");
            shell.input(pad(ps2_host_input::kUp), layout);
            t.Equals(shell.selection(), shell.settingsItem(), "Up from a card: Settings above them");
        });

        tc.Run("the pointer chooses what it is over; a click boots it", [](TestCase &t)
        {
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            shell.start();
            col::MenuInput in;
            in.pointerMoved = true;
            in.pointerX = static_cast<float>(layout.tiles[2].x + 5);
            in.pointerY = static_cast<float>(layout.tiles[2].y + 5);
            t.IsTrue(shell.input(in, layout).kind == col::MenuAction::None, "moving: no action");
            t.Equals(shell.selection(), 2, "over the third tile");
            in.pointerMoved = false;
            in.click = true;
            const col::MenuAction action = shell.input(in, layout);
            t.IsTrue(action.kind == col::MenuAction::Boot && action.game == 2, "a click on it boots it");
            in.pointerX = static_cast<float>(layout.quit.x + 1);
            in.pointerY = static_cast<float>(layout.quit.y + 1);
            t.IsTrue(shell.input(in, layout).kind == col::MenuAction::Quit, "a click on Quit quits");
            in.pointerX = 1.0f;
            in.pointerY = 1.0f;
            t.IsTrue(shell.input(in, layout).kind == col::MenuAction::None, "a click on nothing does nothing");
            t.Equals(shell.selection(), shell.quitItem(), "and keeps the selection");
        });

        tc.Run("the layout: a cabinet, the cards in a row under the name, Settings and Quit at the top", [](TestCase &t)
        {
            for (const size_t games : {1u, 2u, 3u, 4u, 5u})
            {
                const col::Layout layout = col::layout(960, 720, games);
                t.Equals(layout.tiles.size(), games, "a tile per game");
                for (size_t i = 0; i < games; ++i)
                {
                    const col::Rect &r = layout.tiles[i];
                    t.IsTrue(r.x >= 0 && r.x + r.width <= 960 && r.width > 0, "inside, across");
                    t.IsTrue(std::abs(r.width * 10 - r.height * 17) <= 17, "the title pictures' shape");
                    // (the chosen card grows by 1.2: it stays inside, clear of the hint and of its neighbours)
                    t.IsTrue(r.x - r.width / 10 >= 0 && r.x + r.width + r.width / 10 <= 960, "grown, still inside");
                    t.IsTrue(r.y + r.height + r.height / 10 < layout.hint.y, "grown, above the hint");
                    if (i > 0)
                        t.IsTrue(layout.tiles[i - 1].x + layout.tiles[i - 1].width + r.width / 10 < r.x, "apart, grown too");
                }
                t.IsTrue(layout.name.y > layout.title.y + layout.title.height && layout.lastPlayed.y >= layout.name.y + layout.name.height &&
                             layout.lastPlayed.y + layout.lastPlayed.height <= layout.tiles[0].y - layout.tiles[0].height / 10,
                         "the name and Last played between the title and the cards");
                t.IsTrue(layout.hint.y + layout.hint.height <= 720, "the hint inside");
                t.IsTrue(layout.settings.y == layout.title.y && layout.quit.y == layout.title.y &&
                             layout.settings.x + layout.settings.width < layout.quit.x && layout.quit.x + layout.quit.width <= 960 &&
                             layout.title.x + layout.title.width <= layout.settings.x + layout.settings.width,
                         "Settings then Quit at the right of the title's line, inside");
                t.IsTrue(layout.riderLeft.x + layout.riderLeft.width < layout.riderRight.x && layout.riderRight.x + layout.riderRight.width <= 960 &&
                             layout.riderLeft.y + layout.riderLeft.height <= layout.tiles[0].y,
                         "the riders at the sides, above the cards");
            }
        });

        // The menu's settings page: three rows on a small Settings-like
        // struct, as the app gives them (SettingsRow's functions).
        struct Rows
        {
            bool vsync = true;
            int scaling = 0;
            int volume = 100;
            std::vector<col::SettingsRow> rows;
            Rows()
            {
                col::SettingsRow a;
                a.label = "VSync";
                a.current = [this] { return vsync ? 1 : 0; };
                a.apply = [this](int v) { vsync = v != 0; };
                col::SettingsRow b;
                b.kind = col::SettingsRow::Choice;
                b.label = "Scaling";
                b.choices = {"Smooth", "Integer"};
                b.current = [this] { return scaling; };
                b.apply = [this](int v) { scaling = v; };
                col::SettingsRow c;
                c.kind = col::SettingsRow::Range;
                c.label = "Master Volume";
                c.minValue = 0;
                c.maxValue = 100;
                c.step = 5;
                c.unit = "%";
                c.current = [this] { return volume; };
                c.apply = [this](int v) { volume = v; };
                rows = {a, b, c};
            }
        };

        tc.Run("Settings beside Quit opens the page; Back comes back to it", [](TestCase &t)
        {
            Rows r;
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            const col::PageLayout page = col::pageLayout(960, 720, r.rows.size());
            shell.start();
            shell.input(key("Escape"), layout, r.rows, page);
            shell.input(key("Left"), layout, r.rows, page);
            t.Equals(shell.selection(), shell.settingsItem(), "Left from Quit: Settings");
            shell.input(key("Left"), layout, r.rows, page);
            t.Equals(shell.selection(), shell.settingsItem(), "Left on Settings stays");
            shell.input(key("Right"), layout, r.rows, page);
            t.Equals(shell.selection(), shell.quitItem(), "Right from Settings: Quit");
            shell.input(key("Left"), layout, r.rows, page);
            shell.input(key("Down"), layout, r.rows, page);
            t.Equals(shell.selection(), 0, "Down from Settings: a game");
            shell.input(key("Up"), layout, r.rows, page);
            const int before = shell.revision();
            const col::MenuAction open = shell.input(key("Enter"), layout, r.rows, page);
            t.IsTrue(open.kind == col::MenuAction::None && shell.page() == col::MenuPage::Settings && shell.pageRow() == 0,
                     "Enter on Settings opens the page on its first row, no boot or quit");
            t.IsTrue(shell.revision() > before, "a change to show");
            t.IsTrue(shell.input(key("Enter"), layout, r.rows, page).kind == col::MenuAction::None && shell.state() == col::State::Menu,
                     "Enter on the page does not boot");
            shell.input(key("Escape"), layout, r.rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Tiles && shell.selection() == shell.settingsItem(),
                     "Escape closes the page, back on Settings");
            shell.input(key("Enter"), layout, r.rows, page);
            for (int i = 0; i < 5; ++i)
                shell.input(key("Down"), layout, r.rows, page);
            t.Equals(shell.pageRow(), 3, "Down stops on Back, after the rows");
            shell.input(pad(ps2_host_input::kCross), layout, r.rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Tiles, "Cross on Back closes the page");
        });

        tc.Run("the page's rows: Left and Right change them through their functions", [](TestCase &t)
        {
            Rows r;
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            const col::PageLayout page = col::pageLayout(960, 720, r.rows.size());
            shell.start();
            shell.input(key("Up"), layout, r.rows, page);
            shell.input(key("Enter"), layout, r.rows, page);
            shell.input(key("Right"), layout, r.rows, page);
            t.IsTrue(!r.vsync, "Right on a toggle flips it");
            shell.input(key("Left"), layout, r.rows, page);
            t.IsTrue(r.vsync, "Left flips it back");
            shell.input(key("Enter"), layout, r.rows, page);
            t.IsTrue(!r.vsync, "Enter on a row is a change too");
            shell.input(key("Down"), layout, r.rows, page);
            shell.input(key("Left"), layout, r.rows, page);
            t.Equals(r.scaling, 0, "Left on the first choice stays");
            shell.input(key("Right"), layout, r.rows, page);
            shell.input(key("Right"), layout, r.rows, page);
            t.Equals(r.scaling, 1, "Right stops on the last choice");
            shell.input(pad(ps2_host_input::kDown), layout, r.rows, page);
            const int rev = shell.revision();
            shell.input(pad(ps2_host_input::kRight), layout, r.rows, page);
            t.IsTrue(r.volume == 100 && shell.revision() == rev, "Right at the range's top: no change, nothing to show");
            shell.input(pad(ps2_host_input::kLeft), layout, r.rows, page);
            shell.input(pad(ps2_host_input::kLeft), layout, r.rows, page);
            t.IsTrue(r.volume == 90 && shell.revision() > rev, "Left: a step down each, shown");
            // The pointer: over a row chooses it, a click on the chosen row changes it, a click on Back closes.
            col::MenuInput in;
            in.pointerMoved = true;
            in.pointerX = static_cast<float>(page.rows[0].x + 5);
            in.pointerY = static_cast<float>(page.rows[0].y + 5);
            shell.input(in, layout, r.rows, page);
            t.Equals(shell.pageRow(), 0, "over the first row");
            in.pointerMoved = false;
            in.click = true;
            shell.input(in, layout, r.rows, page);
            t.IsTrue(r.vsync, "a click on it: the toggle flips");
            in.pointerX = static_cast<float>(page.back.x + 1);
            in.pointerY = static_cast<float>(page.back.y + 1);
            shell.input(in, layout, r.rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Tiles, "a click on Back closes the page");
            in.pointerX = static_cast<float>(layout.settings.x + 1);
            in.pointerY = static_cast<float>(layout.settings.y + 1);
            shell.input(in, layout, r.rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Settings, "a click on Settings opens it");
        });

        tc.Run("Settings > Mods: the Mods page, a mod's page, its switches and moves; Back comes back to each row", [](TestCase &t)
        {
            Rows r;
            using Game = col::ModEntry::Game;
            auto mods = std::make_shared<std::vector<col::ModEntry>>();
            {
                col::ModEntry park;
                park.title = "Park";
                park.where = "games/ai/Mods/10-park";
                park.contents = "game files";
                park.logic = true;
                park.games = {Game{"Aggressive Inline", true, true, "", "wins 2 over HD Rails"}};
                col::ModEntry rails;
                rails.title = "HD Rails";
                rails.version = "1.2";
                rails.where = "Mods/hd-rails.zip";
                rails.contents = "textures";
                rails.games = {Game{"Aggressive Inline", true, true, "", "loses 2 to Park"}, Game{"BMX XXX", false, false, "Off", ""}};
                rails.notes = {"Textures/dm2 not read, the mod is not for Dave Mirra BMX 2."};
                col::ModEntry loose;
                loose.title = "loose";
                loose.where = "Mods/loose";
                loose.problem = "its mod.ini names no game";
                col::ModEntry map;
                map.title = "Test Park";
                map.where = "Mods/test-park";
                map.map = true;
                *mods = {park, rails, loose, map};
            }
            const std::vector<col::SettingsRow> list = col::modsPageRows(*mods, "Mods");
            t.Equals(list.size(), size_t(4), "a row per mod");
            t.IsTrue(list[0].kind == col::SettingsRow::Page && list[0].opens == col::MenuPage::Mod && list[1].label == "2. HD Rails 1.2",
                     "each opens its page, numbered in the order, with its version");
            t.IsTrue(list[0].valueText(0) == "On" && list[1].valueText(0) == "On in Aggressive Inline" && list[2].valueText(0) == "Not read" &&
                         list[3].valueText(0) == "Map",
                     "its state: on, on in some games, not read, a map");
            t.IsTrue(list[0].help == "Game files." && list[1].help == "Textures.", "its help: what it holds");
            t.Equals(col::modsSummary(*mods), std::string("2 of 3 on"), "the summary: maps not counted");
            {
                col::ModEntry three;
                three.title = "Songs";
                three.games = {Game{"Aggressive Inline", true, true, "", ""}, Game{"Dave Mirra BMX 2", false, false, "Off", ""},
                               Game{"BMX XXX", true, true, "", ""}};
                t.Equals(col::modsPageRows({three}, "Mods")[0].valueText(0), std::string("On in 2 of 3"), "on in some of its games: how many");
            }
            t.IsTrue(col::modsPageRows({}, "Mods").size() == 1 && col::modsPageRows({}, "Mods")[0].kind == col::SettingsRow::Note,
                     "no mods: a note");

            std::vector<std::pair<size_t, bool>> switched;
            std::vector<int> moved;
            const std::vector<col::SettingsRow> page =
                col::modPageRows(mods, 1, [&](size_t g, bool on) { switched.emplace_back(g, on); }, [&](int steps) { moved.push_back(steps); });
            t.IsTrue(page.size() >= 6 && page[0].kind == col::SettingsRow::Heading && page[0].label == "HD Rails 1.2", "its name over it");
            int toggles = 0, actions = 0;
            bool winners = false, note = false;
            for (const col::SettingsRow &row : page)
            {
                toggles += row.kind == col::SettingsRow::Toggle;
                actions += row.kind == col::SettingsRow::Action;
                winners = winners || row.label == "Aggressive Inline: loses 2 to Park";
                note = note || row.label == "Textures/dm2 not read, the mod is not for Dave Mirra BMX 2.";
            }
            t.IsTrue(toggles == 2 && actions == 2 && winners && note, "a switch per game, Move Up and Down, what it loses, its notes");
            const std::vector<col::SettingsRow> mapPage = col::modPageRows(mods, 3, {}, {});
            t.IsTrue(std::none_of(mapPage.begin(), mapPage.end(), [](const col::SettingsRow &row) { return row.kind == col::SettingsRow::Toggle; }),
                     "a map's page has no game's switch");

            col::SettingsRow modsRow;
            modsRow.kind = col::SettingsRow::Page;
            modsRow.label = "Mods";
            modsRow.opens = col::MenuPage::Mods;
            modsRow.summary = [mods] { return col::modsSummary(*mods); };
            r.rows.push_back(modsRow);
            col::Shell shell(3);
            const col::Layout layout = col::layout(960, 720, 3);
            const col::PageLayout settingsPage = col::pageLayout(960, 720, r.rows.size());
            const col::PageLayout modsPage = col::pageLayout(960, 720, list.size());
            const col::PageLayout modPage = col::pageLayout(960, 720, page.size());
            shell.start();
            shell.input(key("Up"), layout, r.rows, settingsPage);
            shell.input(key("Enter"), layout, r.rows, settingsPage);
            for (int i = 0; i < 3; ++i)
                shell.input(key("Down"), layout, r.rows, settingsPage);
            shell.input(key("Enter"), layout, r.rows, settingsPage);
            t.IsTrue(shell.page() == col::MenuPage::Mods, "Enter on the Mods row opens the Mods page");
            shell.input(pad(ps2_host_input::kDown), layout, list, modsPage);
            shell.input(pad(ps2_host_input::kCross), layout, list, modsPage);
            t.IsTrue(shell.page() == col::MenuPage::Mod && shell.modsRow() == 1, "the pad's Cross on HD Rails opens its page");
            t.Equals(shell.chosenRow(page), 3, "on its first switch, past the name and the notes");
            shell.input(key("Left"), layout, page, modPage);
            t.IsTrue(!(*mods)[1].games[0].on && switched.size() == 1 && switched[0] == std::make_pair(size_t(0), false),
                     "Left switches it off in Aggressive Inline, the app told");
            // A click on Move Down.
            int later = -1;
            for (size_t i = 0; i < page.size(); ++i)
                if (page[i].label == "Move Down")
                    later = static_cast<int>(i);
            t.IsTrue(later >= 0 && static_cast<size_t>(later) < modPage.rows.size(), "Move Down is on the page");
            if (later >= 0 && static_cast<size_t>(later) < modPage.rows.size())
            {
                col::MenuInput in;
                in.pointerMoved = true;
                in.pointerX = static_cast<float>(modPage.rows[static_cast<size_t>(later)].x + 5);
                in.pointerY = static_cast<float>(modPage.rows[static_cast<size_t>(later)].y + 5);
                shell.input(in, layout, page, modPage);
                in.pointerMoved = false;
                in.click = true;
                shell.input(in, layout, page, modPage);
                t.IsTrue(moved.size() == 1 && moved[0] == 1, "a click (or a touch) on Move Down moves it one later");
            }
            shell.followMod(2);
            shell.input(key("Escape"), layout, page, modPage);
            t.IsTrue(shell.page() == col::MenuPage::Mods && shell.pageRow() == 2, "Escape: back on the mod's row, where it now is");
            shell.input(key("Escape"), layout, list, modsPage);
            t.IsTrue(shell.page() == col::MenuPage::Settings && shell.pageRow() == 3, "Escape: back on the Mods row");
        });

        tc.Run("a long page: its rows come closer, then scroll with the selection", [](TestCase &t)
        {
            const col::PageLayout ten = col::pageLayout(960, 720, 10), eleven = col::pageLayout(960, 720, 11);
            t.IsTrue(ten.rows.size() == 10 && ten.rows[1].y - ten.rows[0].y == 720 / 18, "ten at the usual spacing");
            t.IsTrue(eleven.rows.size() == 11 && eleven.rows[1].y - eleven.rows[0].y < 720 / 18 &&
                         eleven.rows[10].y + eleven.rows[10].height <= eleven.help.y &&
                         eleven.help.y + eleven.help.height <= eleven.back.y,
                     "eleven closer, all shown, the help between them and Back");
            std::vector<col::SettingsRow> rows(30);
            std::vector<int> values(30, 0);
            for (size_t i = 0; i < rows.size(); ++i)
            {
                rows[i].label = "Row " + std::to_string(i);
                rows[i].current = [&values, i] { return values[i]; };
                rows[i].apply = [&values, i](int v) { values[i] = v; };
            }
            const col::PageLayout page = col::pageLayout(960, 720, rows.size());
            const int shown = static_cast<int>(page.rows.size());
            t.IsTrue(shown > 10 && shown < 30 && page.rows.back().y + page.rows.back().height <= page.help.y,
                     "thirty: as many lines as fit");
            col::Shell shell(1);
            const col::Layout layout = col::layout(960, 720, 1);
            shell.start();
            shell.input(key("Up"), layout, rows, page);
            shell.input(key("Enter"), layout, rows, page);
            for (int i = 0; i < shown; ++i)
                shell.input(key("Down"), layout, rows, page);
            t.IsTrue(shell.pageRow() == shown && shell.pageFirst() == 1, "Down past the last line: the page moves one");
            col::MenuInput in;
            in.pointerMoved = true;
            in.click = true;
            in.pointerX = static_cast<float>(page.rows[0].x + 5);
            in.pointerY = static_cast<float>(page.rows[0].y + 5);
            shell.input(in, layout, rows, page);
            t.IsTrue(shell.pageRow() == 1 && values[1] == 1, "a click on the top line: the row shown there");
            for (int i = 0; i < 40; ++i)
                shell.input(key("Down"), layout, rows, page);
            t.IsTrue(shell.pageRow() == 30 && shell.pageFirst() == 30 - shown, "Back: the last rows shown");
            for (int i = 0; i < 40; ++i)
                shell.input(key("Up"), layout, rows, page);
            t.IsTrue(shell.pageRow() == 0 && shell.pageFirst() == 0, "Up to the top: the first rows again");
        });

        tc.Run("a row's value text and its moves; the last played line", [](TestCase &t)
        {
            Rows r;
            t.IsTrue(r.rows[0].valueText(0) == "Off" && r.rows[0].valueText(1) == "On", "a toggle: Off, On");
            t.IsTrue(r.rows[1].valueText(1) == "Integer" && r.rows[1].valueText(7) == "Integer", "a choice: its name, the last past the end");
            t.IsTrue(r.rows[2].valueText(35) == "35%", "a range: the number and the unit");
            t.IsTrue(r.rows[0].moved(1, 1) == 0 && r.rows[0].moved(0, -1) == 1, "a toggle flips either way");
            t.IsTrue(r.rows[1].moved(0, -1) == 0 && r.rows[1].moved(0, 1) == 1 && r.rows[1].moved(1, 1) == 1, "a choice stops at its ends");
            t.IsTrue(r.rows[2].moved(100, 1) == 100 && r.rows[2].moved(100, -1) == 95 && r.rows[2].moved(2, -1) == 0,
                     "a range steps and stops at its ends");
            std::tm day{};
            day.tm_year = 2026 - 1900;
            day.tm_mon = 10;
            day.tm_mday = 3;
            day.tm_hour = 12;
            t.IsTrue(col::lastPlayedText(std::mktime(&day)) == "Last played 3 November 2026", "the day, month and year, no leading space");
        });

        tc.Run("the settings page draws, with the rows' values", [](TestCase &t)
        {
            Rows r;
            col::Canvas canvas(320, 240);
            const col::Layout layout = col::layout(320, 240, 3);
            const col::PageLayout page = col::pageLayout(320, 240, r.rows.size());
            t.Equals(page.rows.size(), r.rows.size(), "a line per row");
            t.IsTrue(page.rows[2].y + page.rows[2].height <= page.help.y && page.help.y < page.back.y &&
                         page.back.y + page.back.height <= 240,
                     "the help under the rows, Back under it, inside");
            std::vector<col::Tile> tiles{{"One", solid(64, 48, col::rgba(255, 0, 0)), {}, "Last played 3 November 2026"},
                                         {"Two", col::Image{}, {}}, {"Three", col::Image{}, {}}};
            col::BitmapFont font;
            col::readFont(fontFile(8, {{'O', 0, 0, 0, 4, 8}}), solid(8, 8, col::rgba(255, 255, 255)), font);
            col::Shell shell(3);
            shell.start();
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", r.rows, page);
            shell.input(key("Up"), layout, r.rows, page);
            shell.input(key("Enter"), layout, r.rows, page);
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", r.rows, page);
            const col::Rect &row = page.rows[0];
            const uint32_t band = canvas.image().at(row.x + 1, row.y + 1);
            t.IsTrue((band & 0xFF) > 10, "the chosen row's band is lighter than the background");
            t.IsTrue((canvas.image().at(0, 0) >> 24) == 0xFF, "opaque");
        });
    });

    MiniTest::Case("CollectionMenuScript", [](TestCase &tc)
    {
        tc.Run("steps come one every kFramesPerStep frames, across visits", [](TestCase &t)
        {
            col::MenuScript script;
            std::string error;
            t.IsTrue(script.parse("Right, pad:cross,wait,Enter", &error), "parsed");
            t.Equals(script.size(), static_cast<size_t>(4), "four steps");
            std::vector<int> keys;
            uint16_t buttons = 0;
            int given = 0;
            script.beginVisit();
            for (int frame = 1; frame <= col::MenuScript::kFramesPerStep; ++frame)
                given += script.next(keys, buttons) ? 1 : 0;
            t.Equals(given, 1, "one step in the first kFramesPerStep frames");
            t.IsTrue(keys.size() == 1 && keys[0] == ps2_host_input::hostCodeByName("Right"), "Right");
            keys.clear();
            for (int frame = 1; frame <= col::MenuScript::kFramesPerStep; ++frame)
                script.next(keys, buttons);
            t.IsTrue(keys.empty() && buttons == (1u << ps2_host_input::kCross), "then the gamepad's Cross");
            const col::MenuInput in = col::menuInput(keys, buttons);
            t.IsTrue(in.confirm && !in.right, "as menu input: confirm");
            // A new visit (a game ran in between) starts its count again.
            script.beginVisit();
            buttons = 0;
            for (int frame = 1; frame < col::MenuScript::kFramesPerStep; ++frame)
                t.IsTrue(!script.next(keys, buttons), "nothing before the step's frame");
            t.IsTrue(script.next(keys, buttons) && keys.empty() && buttons == 0, "wait: a step with no input");
            for (int frame = 1; frame <= col::MenuScript::kFramesPerStep; ++frame)
                script.next(keys, buttons);
            t.IsTrue(keys.size() == 1 && keys[0] == ps2_host_input::hostCodeByName("Enter"), "Enter last");
            for (int frame = 1; frame <= 3 * col::MenuScript::kFramesPerStep; ++frame)
                t.IsTrue(!script.next(keys, buttons), "nothing after the last step");
        });

        tc.Run("game:<id> steps to that game's card by id, wherever the menu shows it", [](TestCase &t)
        {
            // The cards in release order (dm2, ai, bmx) with Custom Freeplay, the menu as the app drives it.
            col::Shell shell(3, true);
            const col::Layout layout = col::layout(960, 720, 4);
            shell.start();
            col::MenuScript script;
            std::string error;
            t.IsTrue(script.parse("game:bmx,Enter", &error), "parsed");
            t.IsTrue(!script.parse("game:", &error) && error.find("game") != std::string::npos, "game: with no id refused");
            script.parse("game:bmx,game:DM2,wait,game:ai,game:freeplay,game:thps", &error);
            const std::vector<std::string> ids = {"dm2", "ai", "bmx", "freeplay"};
            std::vector<std::string> problems;
            std::vector<int> seen;
            for (int frame = 0; frame < 60 * col::MenuScript::kFramesPerStep; ++frame)
            {
                col::MenuScript::Cards cards;
                cards.games = ids;
                cards.chosen = shell.selection() < 4 ? shell.selection() : -1;
                std::vector<int> keys;
                uint16_t buttons = 0;
                std::string problem;
                if (script.next(keys, buttons, cards, &problem) && keys.empty())
                    seen.push_back(shell.selection()); // a step that moved nothing: a game: step passed (or wait)
                if (!problem.empty())
                    problems.push_back(problem);
                shell.input(col::menuInput(keys, buttons), layout);
            }
            t.IsTrue(seen.size() >= 6 && seen[0] == 2 && seen[1] == 0 && seen[3] == 1 && seen[4] == 3,
                     "on BMX XXX, then DM2 (any case), a wait, AI, Custom Freeplay");
            t.IsTrue(problems.size() == 1 && problems[0].find("thps") != std::string::npos, "a game the menu lacks is said and passed over");
            // From Settings and Quit (no card chosen): Down first, back to the cards.
            col::Shell top(3);
            top.start();
            top.input(key("Escape"), col::layout(960, 720, 3));
            col::MenuScript down;
            down.parse("game:ai", &error);
            for (int frame = 0; frame < 4 * col::MenuScript::kFramesPerStep; ++frame)
            {
                col::MenuScript::Cards cards;
                cards.games = {"dm2", "ai", "bmx"};
                cards.chosen = top.selection() < 3 ? top.selection() : -1;
                std::vector<int> keys;
                uint16_t buttons = 0;
                down.next(keys, buttons, cards);
                top.input(col::menuInput(keys, buttons), col::layout(960, 720, 3));
            }
            t.Equals(top.selection(), 1, "from Quit: down to the cards, then to AI's");
        });

        tc.Run("the bottom screen's buttons: a card, a page straight from the tiles", [](TestCase &t)
        {
            col::Shell shell(3, true);
            shell.start();
            shell.choose(2);
            t.Equals(shell.selection(), 2, "a card chooses its game");
            shell.showPage(col::MenuPage::Mods);
            t.IsTrue(shell.page() == col::MenuPage::Mods, "Mods opens");
            const std::vector<col::SettingsRow> rows;
            const col::PageLayout page = col::pageLayout(960, 720, 0);
            shell.input(key("Escape"), col::layout(960, 720, 4), rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Settings, "its Back: Settings");
            shell.input(key("Escape"), col::layout(960, 720, 4), rows, page);
            t.IsTrue(shell.page() == col::MenuPage::Tiles && shell.selection() == shell.settingsItem(), "then the tiles, on Settings");
            shell.showPage(col::MenuPage::Settings);
            t.IsTrue(shell.page() == col::MenuPage::Settings, "Settings opens");
            shell.input(key("Escape"), col::layout(960, 720, 4), rows, page);
            shell.showPage(col::MenuPage::FreeplayPlayer);
            t.IsTrue(shell.page() == col::MenuPage::FreeplayPlayer, "Custom Freeplay's player page");
            shell.choose(1);
            t.Equals(shell.freeplayGame(), 1, "a card there chooses the player");
            col::Shell off(3);
            off.start();
            off.showPage(col::MenuPage::FreeplayPlayer);
            t.IsTrue(off.page() == col::MenuPage::Tiles, "no player page with Custom Freeplay off");
        });

        tc.Run("a drop: step drops its path; the Games page opens from Settings too", [](TestCase &t)
        {
            col::MenuScript script;
            std::string error;
            t.IsTrue(script.parse("drop:/Discs/ROMs,Enter", &error), "parses: " + error);
            t.IsFalse(script.parse("drop:", &error), "a drop: with no path refused");
            t.IsTrue(script.parse("drop:/Discs/ROMs,Enter", &error), "again");
            std::vector<std::string> dropped;
            std::vector<int> keys;
            uint16_t pad = 0;
            for (int frame = 1; frame <= 2 * col::MenuScript::kFramesPerStep; ++frame)
                script.next(keys, pad, {}, nullptr, &dropped);
            t.IsTrue(dropped == std::vector<std::string>{"/Discs/ROMs"}, "the path, as written");
            t.Equals(keys.size(), size_t{1}, "then Enter");

            col::Shell shell(3);
            shell.start();
            shell.showPage(col::MenuPage::Settings);
            shell.showPage(col::MenuPage::Games);
            t.IsTrue(shell.page() == col::MenuPage::Games, "Games over Settings");
            const std::vector<col::SettingsRow> rows;
            shell.input(key("Escape"), col::layout(960, 720, 3), rows, col::pageLayout(960, 720, 0));
            t.IsTrue(shell.page() == col::MenuPage::Settings, "its Back: Settings");
        });

        tc.Run("tick boxes, marks and bars on a page", [](TestCase &t)
        {
            int ticked = 1;
            std::vector<col::SettingsRow> rows(3);
            rows[0].kind = col::SettingsRow::Check;
            rows[0].label = "PS2 Disc";
            rows[0].summary = []() { return std::string("ai.iso"); };
            rows[0].current = [&]() { return ticked; };
            rows[0].apply = [&](int value) { ticked = value; };
            rows[1].kind = col::SettingsRow::Note;
            rows[1].label = "Xbox Disc";
            rows[1].summary = []() { return std::string("ai-xbox.iso"); };
            rows[1].mark = [&]() { return ticked ? col::SettingsRow::Mark::Tick : col::SettingsRow::Mark::Box; };
            rows[2].kind = col::SettingsRow::Note;
            rows[2].label = "Not set up";
            rows[2].progress = []() { return 0.5f; };
            t.Equals(rows[0].valueText(1), std::string("ai.iso"), "a tick box's value: its summary");
            t.Equals(rows[1].valueText(0), std::string("ai-xbox.iso"), "a note's too");
            t.IsTrue(rows[0].moved(1, 1) == 0 && rows[0].moved(0, -1) == 1, "a tick box flips");
            t.IsTrue(rows[0].choosable() && !rows[1].choosable(), "chosen, a note not");

            col::Shell shell(3);
            shell.start();
            shell.showPage(col::MenuPage::Games);
            const col::Layout layout = col::layout(320, 240, 3);
            const col::PageLayout page = col::pageLayout(320, 240, rows.size());
            shell.input(key("Enter"), layout, rows, page);
            t.Equals(ticked, 0, "Enter takes the tick off");
            shell.input(key("Right"), layout, rows, page);
            t.Equals(ticked, 1, "Right puts it back");

            col::Canvas canvas(320, 240);
            std::vector<col::Tile> tiles{{"One", col::Image{}, {}}, {"Two", col::Image{}, {}}, {"Three", col::Image{}, {}}};
            tiles[0].colour = col::rgba(0, 255, 0);
            col::BitmapFont font;
            col::readFont(fontFile(8, {{'O', 0, 0, 0, 4, 8}}), solid(8, 8, col::rgba(255, 255, 255)), font);
            col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", "", rows, page);
            const col::Rect &bar = page.rows[2];
            bool lit = false, unlit = false;
            for (int x = page.labelX; x < page.valueX; ++x)
            {
                const uint32_t c = canvas.image().at(x, bar.y + bar.height - 2);
                (((c & 0xFF) < 40 && ((c >> 8) & 0xFF) > 200) ? lit : unlit) = true;
            }
            t.IsTrue(lit && unlit, "the bar half in the game's colour");

            // A short label and a long value (a game's name, its disc's file): the label whole, the value cut short.
            const col::RowText laid = col::layRowText(font, "OOOO", std::string(200, 'O'), page.labelX, page.valueX, 1.0f, 6);
            t.IsTrue(laid.label.text == "OOOO" && laid.value.text.size() > 3 &&
                         laid.value.text.substr(laid.value.text.size() - 3) == "..." &&
                         laid.labelX + laid.label.width + 6 <= laid.valueX && laid.valueX + laid.value.width <= page.valueX,
                     "the label whole, the value cut, side by side");

            // "%" is drawn whatever the font has: some of its pixels lit, inside its width.
            col::Canvas sign(64, 32);
            sign.clear(col::rgba(0, 0, 0));
            const int wide = col::Canvas::textWidth(font, "%", 2.0f);
            sign.text(font, 4, 4, "%", 2.0f, col::rgba(255, 255, 255));
            int inked = 0, outside = 0;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x)
                    if ((sign.image().at(x, y) & 0xFF) > 128)
                        (x >= 4 && x <= 4 + wide ? inked : outside) += 1;
            t.IsTrue(wide > 0 && inked > 10 && outside == 0, "a percent sign drawn in its own width");
        });

        tc.Run("a step that is no key, button or wait is refused", [](TestCase &t)
        {
            col::MenuScript script;
            std::string error;
            t.IsTrue(!script.parse("Right,Jump", &error), "Jump refused");
            t.IsTrue(error.find("Jump") != std::string::npos, "named in the reason");
            t.IsTrue(!script.parse("pad:nothing", &error), "an unknown button refused");
            t.IsTrue(script.parse("", &error) && script.empty(), "an empty script");
        });

        tc.Run("keys and buttons as menu input", [](TestCase &t)
        {
            t.IsTrue(key("Left").left && key("Up").up && key("Down").down, "arrows");
            t.IsTrue(key("Space").confirm && key("KP_Enter").confirm, "Space, keypad Enter");
            t.IsTrue(key("Escape").back && pad(ps2_host_input::kCircle).back, "Escape, Circle: back");
            t.IsTrue(pad(ps2_host_input::kLeft).left && pad(ps2_host_input::kDown).down, "the D-pad");
            t.IsTrue(!key("F8").any(), "other keys: nothing");
        });
    });

    MiniTest::Case("CollectionPictures", [](TestCase &tc)
    {
        tc.Run("raw pictures, 24 and 32 bits, and a field's rows doubled", [](TestCase &t)
        {
            col::Image image;
            const std::vector<uint8_t> rgb{1, 2, 3, 4, 5, 6};
            t.IsTrue(col::readRawImage(rgb, 2, 1, 24, image), "24 bits");
            t.IsTrue(image.at(0, 0) == col::rgba(1, 2, 3) && image.at(1, 0) == col::rgba(4, 5, 6), "R G B, opaque");
            const std::vector<uint8_t> rgbx{1, 2, 3, 0x80, 4, 5, 6, 0};
            t.IsTrue(col::readRawImage(rgbx, 1, 2, 32, image), "32 bits");
            t.IsTrue(image.at(0, 1) == col::rgba(4, 5, 6), "the PS2 alpha ignored");
            t.IsTrue(!col::readRawImage(rgb, 2, 2, 24, image), "a short file refused");
            const col::Image doubled = col::doubleRows(image);
            t.IsTrue(doubled.height == 4 && doubled.at(0, 1) == image.at(0, 0) && doubled.at(0, 2) == image.at(0, 1),
                     "every row twice");
        });

        tc.Run("an 8-bit BMP with its palette's alpha, bottom-up rows", [](TestCase &t)
        {
            // index 0: opaque red, 1: half-transparent blue (bytes B G R A, little-endian words)
            const std::vector<uint8_t> file = bmp8(2, 2, {0xFFFF0000u, 0x800000FFu}, {0, 1, 1, 0});
            col::Image image;
            t.IsTrue(col::readBmpImage(file, image), "read");
            t.IsTrue(image.width == 2 && image.height == 2, "its size");
            t.IsTrue(image.at(0, 0) == col::rgba(255, 0, 0, 255), "top-left red");
            t.IsTrue(image.at(1, 0) == col::rgba(0, 0, 255, 0x80), "top-right blue, its alpha kept");
            t.IsTrue(image.at(0, 1) == col::rgba(0, 0, 255, 0x80), "the bottom row is the file's first");
            std::vector<uint8_t> bad = file;
            bad[0] = 'X';
            t.IsTrue(!col::readBmpImage(bad, image), "not a BMP");
        });

        tc.Run("the engine's font file and text on the canvas", [](TestCase &t)
        {
            const col::Image page = solid(16, 8, col::rgba(255, 255, 255));
            col::BitmapFont font;
            t.IsTrue(col::readFont(fontFile(8, {{'A', 0, 0, 0, 4, 8}, {'B', 1, 4, 0, 6, 7}}), page, font), "read");
            t.Equals(font.height, 8, "its line height");
            t.IsTrue(font.glyphs['A'].present && font.glyphs['B'].top == 1 && font.glyphs['B'].width == 6, "its glyphs");
            t.IsTrue(!font.glyphs['C'].present, "and no others");
            t.Equals(col::Canvas::textWidth(font, "AB", 1.0f), 4 + 1 + 6 + 1, "each glyph's width and a texel between");
            t.Equals(col::Canvas::textWidth(font, "A A", 2.0f), 2 * (5 + 4 + 5), "a space: half a line");
            col::Canvas canvas(32, 16);
            canvas.clear(col::rgba(0, 0, 0));
            canvas.text(font, 2, 2, "A", 1.0f, col::rgba(255, 0, 0));
            t.IsTrue(canvas.image().at(3, 5) == col::rgba(255, 0, 0), "drawn in the colour asked");
            t.IsTrue(canvas.image().at(7, 5) == col::rgba(0, 0, 0), "only where the glyph is");
            col::BitmapFont none;
            t.IsTrue(!col::readFont(fontFile(8, {{'A', 0, 12, 0, 8, 8}}), page, none), "a glyph outside its page refused");
            t.IsTrue(!col::readFont(fontFile(8, {}), col::Image{}, none), "no page refused");
        });

        tc.Run("drawing: cover fills, contain fits whole, fill blends", [](TestCase &t)
        {
            col::Image picture = solid(2, 1, col::rgba(200, 0, 0));
            col::Canvas canvas(4, 4);
            canvas.clear(col::rgba(0, 0, 0));
            canvas.draw(picture, {0, 0, 2, 1}, {0, 0, 4, 4}, col::Fit::Contain);
            t.IsTrue(canvas.image().at(0, 0) == col::rgba(0, 0, 0) && canvas.image().at(0, 3) == col::rgba(0, 0, 0),
                     "contain: bars above and below");
            t.IsTrue(canvas.image().at(1, 1) == col::rgba(200, 0, 0) && canvas.image().at(2, 2) == col::rgba(200, 0, 0),
                     "and the picture across the middle");
            canvas.clear(col::rgba(0, 0, 0));
            canvas.draw(picture, {0, 0, 2, 1}, {0, 0, 4, 4}, col::Fit::Cover, 0.5f);
            bool all = true;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    all = all && canvas.image().at(x, y) == col::rgba(100, 0, 0);
            t.IsTrue(all, "cover: every pixel, at the brightness asked");
            canvas.fill({0, 0, 4, 4}, col::rgba(0, 0, 255, 128));
            const uint32_t blended = canvas.image().at(0, 0);
            t.IsTrue((blended & 0xFF) == 50 && ((blended >> 16) & 0xFF) == 128, "fill: blended by its alpha");
            canvas.fill({-10, -10, 5, 5}, col::rgba(255, 255, 255));
            t.IsTrue(true, "a rectangle off the canvas is cut");
        });

        tc.Run("an archive's files from its ZDIR tables", [](TestCase &t)
        {
            // ZDIR { OFST: 40, 44 ; DLEN: 4 | flags, 3 } then the two files.
            std::vector<uint8_t> file(48, 0);
            std::memcpy(file.data(), "ZDIR", 4);
            put32(file, 4, 32);
            std::memcpy(file.data() + 8, "OFST", 4);
            put32(file, 12, 8);
            put32(file, 16, 40);
            put32(file, 20, 44);
            std::memcpy(file.data() + 24, "DLEN", 4);
            put32(file, 28, 8);
            put32(file, 32, 0x60000004u);
            put32(file, 36, 3);
            const auto files = col::archiveFiles(file);
            t.IsTrue(files.size() == 2 && files[0] == std::make_pair<size_t, size_t>(40, 4) &&
                         files[1] == std::make_pair<size_t, size_t>(44, 3),
                     "each file's offset and length, the length's flags dropped");
            put32(file, 36, 9);
            t.Equals(col::archiveFiles(file).size(), static_cast<size_t>(1), "a file past the end left out");
            const std::vector<uint8_t> plain{'T', 'X', 'R', 'C', 0, 0, 0, 0};
            t.IsTrue(col::archiveFiles(plain) == std::vector<std::pair<size_t, size_t>>{{0, 8}}, "no archive: one file");
        });

        tc.Run("each game's picture on its disc", [](TestCase &t)
        {
            const col::ArtSource *ai = col::artSource("ai");
            const col::ArtSource *dm2 = col::artSource("dm2");
            const col::ArtSource *bmx = col::artSource("bmx");
            t.IsTrue(ai && ai->kind == col::ArtSource::Raw && ai->width == 640 && ai->height == 448, "AI's loading screen");
            t.IsTrue(dm2 && dm2->kind == col::ArtSource::RawField && dm2->bits == 32, "DM2's legal screen, a field");
            t.IsTrue(bmx && bmx->kind == col::ArtSource::LibraryImage && std::string(bmx->texture) == "start_center_04",
                     "BMX XXX's start screen");
            t.IsTrue(col::artSource("pong") == nullptr, "none for another game");
        });

        tc.Run("Custom Freeplay: its tile when on, the player page, the map page, a map's Boot", [](TestCase &t)
        {
            col::Shell shell(3);
            t.Equals(shell.freeplayItem(), -1, "off: no tile");
            t.Equals(shell.quitItem(), 3, "Quit after the games");
            shell.setFreeplay(true);
            t.Equals(shell.freeplayItem(), 3, "on: the tile after the games");
            t.IsTrue(shell.quitItem() == 4 && shell.settingsItem() == 5, "Quit and Settings move by one");
            const col::Layout layout = col::layout(960, 720, 4), players = col::layout(960, 720, 3);
            const col::PageLayout playerPage = col::pageLayout(960, 720, 0);
            t.Equals(layout.tiles.size(), size_t(4), "four tiles laid out");
            shell.start();
            for (int i = 0; i < 3; ++i)
                shell.input(key("Right"), layout);
            t.Equals(shell.selection(), 3, "Right three times: on the Custom Freeplay tile");
            shell.input(key("Right"), layout);
            t.Equals(shell.selection(), 3, "the last tile");
            shell.input(key("Escape"), layout);
            t.Equals(shell.selection(), shell.quitItem(), "Escape: Quit");
            shell.input(key("Left"), layout);
            t.Equals(shell.selection(), shell.settingsItem(), "Left: Settings");
            shell.input(key("Down"), layout);
            shell.input(key("Right"), layout);
            shell.input(key("Right"), layout);
            shell.input(key("Right"), layout);
            t.IsTrue(shell.input(key("Enter"), layout).kind == col::MenuAction::None && shell.page() == col::MenuPage::FreeplayPlayer,
                     "Enter on the tile opens the player page, no boot");
            t.Equals(shell.freeplayGame(), 0, "the first game's player");
            shell.input(key("Right"), players, {}, playerPage);
            t.Equals(shell.freeplayGame(), 1, "Right: the second game's player");
            shell.input(key("Down"), players, {}, playerPage);
            t.IsTrue(shell.freeplayOnBack(), "Down: Back");
            shell.input(key("Right"), players, {}, playerPage);
            t.Equals(shell.freeplayGame(), 1, "Right on Back: the player stays");
            shell.input(key("Enter"), players, {}, playerPage);
            t.IsTrue(shell.page() == col::MenuPage::Tiles && shell.selection() == 3, "Enter on Back: the tiles, on Custom Freeplay");
            shell.input(key("Enter"), layout);
            shell.input(key("Left"), players, {}, playerPage);
            t.Equals(shell.freeplayGame(), 0, "the player page again: Left to the first");
            shell.input(key("Enter"), players, {}, playerPage);
            t.IsTrue(shell.page() == col::MenuPage::FreeplayMap, "Enter on a player: the map page");

            // A game whose maps do not start (AI's without the Estate) has no
            // tile there: the page's tiles are the players offered.
            {
                col::Shell offered(3, true);
                offered.start();
                offered.setFreeplayPlayers({1, 2});
                t.IsTrue(offered.freeplayPlayers() == std::vector<int>({1, 2}), "the players offered");
                offered.showPage(col::MenuPage::FreeplayPlayer);
                t.Equals(offered.freeplayGame(), 1, "the page opens on the first player offered");
                const col::Layout two = col::layout(960, 720, 2);
                offered.input(key("Left"), two, {}, playerPage);
                t.Equals(offered.freeplayGame(), 1, "Left on the first: no tile before it");
                offered.input(key("Right"), two, {}, playerPage);
                t.Equals(offered.freeplayGame(), 2, "Right: the next one offered");
                offered.input(key("Right"), two, {}, playerPage);
                t.Equals(offered.freeplayGame(), 2, "the last one");
                offered.choose(0);
                t.Equals(offered.freeplayGame(), 2, "a card of a game not offered chooses nothing");
                const col::Rect first = two.tiles[0];
                col::MenuInput point;
                point.pointerMoved = true;
                point.pointerX = first.x + 2.0f;
                point.pointerY = first.y + 2.0f;
                offered.input(point, two, {}, playerPage);
                t.Equals(offered.freeplayGame(), 1, "the first tile is the first player offered");
                offered.setFreeplayPlayers({2});
                t.Equals(offered.freeplayGame(), 2, "a player no longer offered: the first one that is");
                offered.setFreeplayPlayers({});
                t.IsTrue(offered.freeplayOnBack(), "none offered: Back");
                offered.input(key("Up"), col::layout(960, 720, 0), {}, playerPage);
                t.IsTrue(offered.freeplayOnBack(), "Up: still Back, no player to go to");
                offered.input(key("Enter"), col::layout(960, 720, 0), {}, playerPage);
                t.IsTrue(offered.page() == col::MenuPage::Tiles, "Enter: Back to the tiles");
            }

            // The map page's rows, one per map whatever game it was made in:
            // a playable map's Enter is a Boot with it, its help the same line
            // whatever game it was made in, one that did not convert is a
            // Note with the converter's reason.
            std::vector<col::MapChoice> maps(3);
            maps[0].name = "Test Park";
            maps[0].builtFor = "Aggressive Inline";
            maps[0].author = "custom2";
            maps[0].playable = true;
            maps[0].game = 0;
            maps[0].map = 7;
            maps[0].preview = std::make_shared<const col::Image>(solid(8, 6, col::rgba(0, 0, 255)));
            maps[1].name = "Mullaly Mix";
            maps[1].builtFor = "BMX XXX";
            maps[1].author = "someone";
            maps[1].playable = true;
            maps[1].game = 0;
            maps[1].map = 8;
            maps[2].name = "Huge Park";
            maps[2].builtFor = "Dave Mirra Freestyle BMX 2";
            maps[2].why = "too many parts to draw (3000, 1083 at most)";
            const std::vector<col::SettingsRow> rows = col::freeplayMapRows(maps, "Aggressive Inline", "Maps", true);
            t.Equals(rows.size(), size_t(4), "a heading and three maps");
            t.IsTrue(rows[0].kind == col::SettingsRow::Heading && rows[1].kind == col::SettingsRow::Action &&
                         rows[2].kind == col::SettingsRow::Action && rows[3].kind == col::SettingsRow::Note,
                     "the playable maps Actions (the converted one too), the failed one a Note");
            t.IsTrue(rows[1].label == "Test Park" && rows[1].help == "From Aggressive Inline, by custom2." && rows[1].picture &&
                         rows[1].picture->width == 8,
                     "the map's name, where it was made and its author in the help line, its preview");
            t.IsTrue(rows[2].help == "From BMX XXX, by someone.",
                     "a map made in another game: the same line as one made in the player's game");
            t.IsTrue(rows[3].label == "Huge Park: too many parts to draw (3000, 1083 at most)",
                     "a map that did not convert says why, never a silent \"not startable\"");
            // The bottom line names the buttons of the device last used, by
            // position (Nintendo's choose button is B), and nothing on touch.
            t.Equals(col::buttonHint(col::HintDevice::Keyboard, "Select", "Back"), std::string("Enter Select   Esc Back"), "keyboard");
            t.Equals(col::buttonHint(col::HintDevice::PlayStation, "Start", "Back"), std::string("Cross Start   Circle Back"), "PlayStation");
            t.Equals(col::buttonHint(col::HintDevice::Xbox, "Select"), std::string("A Select"), "Xbox, no back part");
            t.Equals(col::buttonHint(col::HintDevice::Nintendo, "Change", "Back"), std::string("B Change   A Back"), "Nintendo");
            t.Equals(col::buttonHint(col::HintDevice::Touch, "Select", "Back"), std::string(), "touch: no hint");
            t.Equals(col::buttonHint(col::HintDevice::PlayStation, nullptr, "Stop"), std::string("Circle Stop"), "back only");
            const std::vector<col::SettingsRow> none = col::freeplayMapRows({}, "Aggressive Inline", "Maps", true);
            t.IsTrue(none.size() == 2 && none[1].kind == col::SettingsRow::Note && none[1].label.find("Maps") != std::string::npos,
                     "no maps: a note saying where they go");
            const std::vector<col::SettingsRow> noSlot = col::freeplayMapRows(maps, "BMX XXX", "Maps", false);
            t.IsTrue(noSlot.size() == 2 && noSlot[1].label.find("lends no level slot") != std::string::npos, "a game lending no slot: one note");
            const std::vector<col::SettingsRow> needs = col::freeplayMapRows(maps, "Aggressive Inline", "Maps", false, "Add its GameCube disc.");
            t.IsTrue(needs.size() == 2 && needs[1].kind == col::SettingsRow::Note && needs[1].label == "Add its GameCube disc.",
                     "a slot the setup cannot start: no maps offered, the note says what it needs");
            // Measured by the rectangles at the app's menu size (960 x 720)
            // and 640 x 480: a long map name and its value, the game's name
            // over the rows and the notice never meet each other, the picture
            // or Back.
            for (const auto &[w, h] : {std::pair{960, 720}, std::pair{640, 480}})
            {
                col::BitmapFont font;
                col::readFont(fontFile(8, {{'O', 0, 0, 0, 4, 8}}), solid(8, 8, col::rgba(255, 255, 255)), font);
                const col::PageLayout page = col::pageLayout(w, h, 5, true);
                const float unit = h / 448.0f, scale = 1.1f * unit;
                const int gap = static_cast<int>(12 * unit);
                const col::Rect &row = page.rows[1];
                const int rowRight = row.x + row.width;
                const col::RowText laid =
                    col::layRowText(font, std::string("The Underground ") + std::string(90, 'O'), "start >", page.labelX, page.valueX, scale, gap);
                const std::string at = " at " + std::to_string(w) + "x" + std::to_string(h);
                t.IsTrue(laid.labelX >= row.x && laid.labelX + laid.label.width + gap <= laid.valueX &&
                             laid.valueX + laid.value.width <= page.valueX && page.valueX <= rowRight,
                         "a long label and its value side by side, inside the row" + at);
                t.IsTrue(rowRight < page.picture.x, "the rows end left of the picture" + at);
                t.IsTrue(laid.label.text.size() > 3 && laid.label.text.substr(laid.label.text.size() - 3) == "...",
                         "a label too long even smaller is cut short" + at);
                const col::RowText fits = col::layRowText(font, "Woodward", "start >", page.labelX, page.valueX, scale, gap);
                t.IsTrue(fits.label.text == "Woodward" && fits.label.scale == scale, "a short one as it is" + at);
                const int headingX = page.labelX - static_cast<int>(8 * unit);
                const col::FittedLine heading = col::fitLine(font, "Dave Mirra Freestyle BMX 2", rowRight - headingX, scale, 0.6f * scale);
                t.IsTrue(headingX + heading.width <= rowRight, "the game's name over the rows stays left of the picture" + at);
                t.IsTrue(page.notice.y >= page.help.y + page.help.height && page.notice.y + page.notice.height <= page.back.y &&
                             page.help.y >= page.picture.y + page.picture.height,
                         "the help under the picture, the notice between it and Back" + at);
            }
            const col::PageLayout mapsPage = col::pageLayout(960, 720, rows.size(), true);
            t.IsTrue(!mapsPage.picture.empty() && mapsPage.rows[0].x + mapsPage.rows[0].width <= mapsPage.picture.x,
                     "a page with pictures: the rows left of the picture");
            t.Equals(shell.chosenRow(rows), 1, "on the first map, past the heading");
            shell.input(key("Down"), players, rows, mapsPage);
            t.Equals(shell.pageRow(), 2, "Down: the converted map");
            shell.input(key("Down"), players, rows, mapsPage);
            t.Equals(shell.pageRow(), 4, "Down: past the note to Back");
            shell.input(key("Up"), players, rows, mapsPage);
            shell.input(key("Up"), players, rows, mapsPage);
            const col::MenuAction action = shell.input(key("Enter"), players, rows, mapsPage);
            t.IsTrue(action.kind == col::MenuAction::Boot && action.game == 0 && action.map == 7, "Enter on the map: a Boot with it");
            t.IsTrue(shell.boot(action.game, action.map), "booted");
            t.IsTrue(shell.map() == 7 && shell.page() == col::MenuPage::Tiles, "the map remembered, the page closed");
            shell.gameEnded(col::GameEnd::ExitToMenu);
            t.Equals(shell.selection(), 3, "back on Custom Freeplay after a map");
            shell.input(key("Enter"), layout);
            shell.input(key("Escape"), players, {}, playerPage);
            t.IsTrue(shell.page() == col::MenuPage::Tiles && shell.selection() == 3, "Escape on the player page: the tiles");
            shell.setFreeplay(false);
            t.IsTrue(shell.freeplayItem() == -1 && shell.selection() == 0 && shell.quitItem() == 3, "off again: the selection on a game, Quit back");

            // Drawn: the four tiles, the player page, the map page with a picture.
            col::Canvas canvas(320, 240);
            std::vector<col::Tile> tiles{{"One", solid(64, 48, col::rgba(255, 0, 0)), {}, "", "Inline skater"},
                                         {"Two", solid(64, 32, col::rgba(0, 255, 0)), {0, 0, 64, 16}, "", "BMX rider"},
                                         {"Three", col::Image{}, {}, "", "BMX rider"}};
            col::BitmapFont font;
            col::readFont(fontFile(8, {{'O', 0, 0, 0, 4, 8}}), solid(8, 8, col::rgba(255, 255, 255)), font);
            shell.setFreeplay(true);
            const col::Layout small = col::layout(320, 240, 4), smallPlayers = col::layout(320, 240, 3);
            for (int i = 0; i < 3; ++i)
                shell.input(key("Right"), small);
            col::drawMenu(canvas, small, tiles, font, shell, "Z-Axis Collection", "");
            const col::Rect tile = small.tiles[3];
            const uint32_t left = canvas.image().at(tile.x + tile.width / 6, tile.y + 4);
            t.IsTrue((left & 0xFF) > 100 && ((left >> 8) & 0xFF) < 60, "the Custom Freeplay tile: the first game's picture in its left third");
            shell.input(key("Enter"), small);
            col::drawMenu(canvas, smallPlayers, tiles, font, shell, "Z-Axis Collection", "", {}, col::pageLayout(320, 240, 0));
            shell.input(key("Enter"), smallPlayers, {}, col::pageLayout(320, 240, 0));
            const col::PageLayout smallMaps = col::pageLayout(320, 240, rows.size(), true);
            col::drawMenu(canvas, smallPlayers, tiles, font, shell, "Z-Axis Collection", "", rows, smallMaps);
            const uint32_t preview = canvas.image().at(smallMaps.picture.x + smallMaps.picture.width / 2, smallMaps.picture.y + smallMaps.picture.height / 2);
            t.IsTrue(((preview >> 16) & 0xFF) > 200 && (preview & 0xFF) < 40, "the chosen map's preview drawn in the picture box");
        });

        tc.Run("a 24-bit BMP reads as a picture", [](TestCase &t)
        {
            // 2 x 2, bottom-up: the top row red then green, the bottom blue then white.
            std::vector<uint8_t> file(54 + 2 * 8, 0);
            file[0] = 'B';
            file[1] = 'M';
            put32(file, 10, 54);
            put32(file, 14, 40);
            put32(file, 18, 2);
            put32(file, 22, 2);
            put16(file, 28, 24);
            const uint8_t bottom[8] = {255, 0, 0, 255, 255, 255, 0, 0}, top[8] = {0, 0, 255, 0, 255, 0, 0, 0};
            std::memcpy(file.data() + 54, bottom, 8);
            std::memcpy(file.data() + 62, top, 8);
            col::Image image;
            t.IsTrue(col::readBmpImage(file, image) && image.width == 2 && image.height == 2, "read");
            t.IsTrue(image.at(0, 0) == col::rgba(255, 0, 0) && image.at(1, 0) == col::rgba(0, 255, 0) && image.at(0, 1) == col::rgba(0, 0, 255) &&
                         image.at(1, 1) == col::rgba(255, 255, 255),
                     "the rows top down, B G R read as R G B");
            put32(file, 30, 1); // RLE: not read
            t.IsTrue(!col::readBmpImage(file, image), "a compressed BMP is refused");
        });

        tc.Run("the whole menu draws for every selection", [](TestCase &t)
        {
            col::Canvas canvas(320, 240);
            const col::Layout layout = col::layout(320, 240, 3);
            std::vector<col::Tile> tiles{{"One", solid(64, 48, col::rgba(255, 0, 0)), {}},
                                         {"Two", solid(64, 32, col::rgba(0, 255, 0)), {0, 0, 64, 16}},
                                         {"Three", col::Image{}, {}}};
            col::BitmapFont font;
            col::readFont(fontFile(8, {{'O', 0, 0, 0, 4, 8}}), solid(8, 8, col::rgba(255, 255, 255)), font);
            col::Shell shell(3);
            shell.start();
            for (int i = 0; i <= shell.settingsItem(); ++i)
            {
                col::drawMenu(canvas, layout, tiles, font, shell, "Z-Axis Collection", i == 1 ? "a notice" : "");
                shell.input(i < 2 ? key("Right") : i == 2 ? key("Escape") : key("Left"), layout);
            }
            t.Equals(shell.selection(), shell.settingsItem(), "drawn on every item up to Settings");
            const col::Rect tile = layout.tiles[0];
            const uint32_t centre = canvas.image().at(tile.x + tile.width / 2, tile.y + tile.height / 2);
            t.IsTrue((centre & 0xFF) > 0 && (centre & 0xFF) < 255, "a tile not chosen: its picture, dimmed");
            t.IsTrue((canvas.image().at(0, 0) >> 24) == 0xFF, "opaque");
        });
    });
}
