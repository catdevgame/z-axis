// The Settings page's table (game_overrides/zge_settings.cpp, kPage): every
// key the three games register today has a place, and no key has two.

#include "MiniTest.h"
#include "overrides/zge_settings.h"
#include "runtime/settings_registry.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace zs = zge_host::settings;

    // The keys the games' pages show today (collection/app.cpp
    // registerSettings, lib/gs/gs_quality.cpp, lib/pad.cpp
    // registerControlSettings, the overrides' *_gameplay.cpp, *_sound.cpp,
    // dave_mirra_volumes.cpp, *_native.cpp, zge_xbox_textures.cpp,
    // _disc.cpp, _movies.cpp). The key and pad rows are one per game action
    // (controls.key.jump, controls.pad.jump): one of each stands for them.
    const char *const kRegisteredKeys[] = {
        // the app: display
        "display.window", "display.renderer", "display.resolution", "display.full_height", "display.widescreen",
        "display.aspect", "display.motion_blur", "display.draw_distance", "display.anti_aliasing",
        "display.texture_filtering", "display.fog", "display.shadows", "display.depth_precision",
        "display.perspective_correct", "display.character_lighting", "display.xbox_textures", "display.xbox_skaters",
        "display.xbox_movies", "display.scaling", "display.vsync", "display.show_fps", "display.pause_in_background",
        "display.native_meshes", "display.variable_refresh", "display.depth_test", "display.texture_dump",
        // sound
        "audio.volume", "audio.mute_in_background", "audio.movie_volume", "audio.french_voices", "audio.music_volume",
        "audio.effects_volume", "audio.speech_volume", "audio.shuffle",
        // controls
        "controls.mouse_sensitivity", "controls.invert_y", "controls.mouse_mode", "controls.stick_deadzone",
        "controls.gamepad", "controls.gamepads", "controls.test_rumble", "controls.swap_players", "controls.prompts",
        "controls.key.jump", "controls.gamepad_layout", "controls.pad.jump", "controls.reset",
        // gameplay
        "camera.fov", "camera.distance", "camera.height", "camera.style", "assist.juice_runs_out", "assist.juice_drain",
        "assist.crash_juice_loss", "assist.grind_balance_loss", "assist.manual_balance_loss",
        "cheat.low_gravity_wall_ride", "cheat.juice_regeneration", "cheat.invulnerability", "cheat.perfect_grind",
        "cheat.perfect_handplant", "cheat.super_spin", "cheat.perfect_manual", "cheat.unlock_all",
        // game
        "game.skip_intro_movies", "game.language", "game.developer_menu", "game.zoo", "game.estate",
        "game.dm2_xbox_levels", "game.create_player", "game.xbox_themes", "game.retro_movie", "game.attract_demo",
        "game.park_editor", "game.xbox_challenges", "game.editor_pieces",
        "game.level_previews", "game.logo_movie", "game.intro_movie", "game.gc_levels", "game.menu_lines",
        "game.all_mp_modes", "game.logo_splash", "game.trailers", "game.car_textures", "game.green_skin",
        "game.topless_girl", "game.xbox_songs",
        "game.zoo_unlock", "game.tuning",
        "game.career_scripts", "game.ape", "game.xbox_free_skate", "game.record_play", "game.recordings", "game.practice",
        "game.custom_freeplay", "game.restart",
    };

    // The page cf196d42b showed for those keys (game.restart aside, which it
    // had not), with Gamepad Layout first under BUTTONS (uxgame1, landed beside
    // this), section by section: "<title>: <row> <row> ...", a heading as "#"
    // and its word. With nothing waiting for a restart the page is still this,
    // row for row.
    const char *const kPageBefore[] = {
        "Display: display.window display.scaling display.vsync display.variable_refresh display.widescreen display.aspect display.show_fps display.pause_in_background",
        "Graphics: display.renderer display.resolution display.full_height display.anti_aliasing display.texture_filtering display.fog display.shadows display.depth_precision display.depth_test display.perspective_correct display.character_lighting display.native_meshes display.motion_blur display.draw_distance display.xbox_textures display.xbox_skaters display.xbox_movies display.texture_dump",
        "Sound: audio.volume audio.music_volume audio.effects_volume audio.speech_volume audio.shuffle audio.movie_volume audio.french_voices audio.mute_in_background",
        "Controls: controls.prompts controls.gamepad controls.stick_deadzone controls.gamepads controls.test_rumble controls.swap_players controls.mouse_mode controls.mouse_sensitivity controls.invert_y #KEYS controls.key.jump #BUTTONS controls.gamepad_layout controls.pad.jump controls.reset",
        "Gameplay: #CAMERA camera.fov camera.distance camera.height camera.style #ASSISTS assist.grind_balance_loss assist.manual_balance_loss assist.juice_runs_out assist.juice_drain assist.crash_juice_loss #CHEATS cheat.perfect_grind cheat.perfect_manual cheat.perfect_handplant cheat.super_spin cheat.low_gravity_wall_ride cheat.juice_regeneration cheat.invulnerability cheat.unlock_all",
        "Game: game.language game.skip_intro_movies game.tuning game.career_scripts game.developer_menu game.practice game.record_play game.recordings game.custom_freeplay #EXTRAS game.zoo game.zoo_unlock game.estate game.ape game.xbox_free_skate game.dm2_xbox_levels game.create_player game.xbox_themes game.retro_movie game.attract_demo game.park_editor game.xbox_challenges game.editor_pieces game.level_previews game.logo_movie game.intro_movie game.gc_levels game.menu_lines game.all_mp_modes game.logo_splash game.trailers game.car_textures game.green_skin game.topless_girl game.xbox_songs",
    };

    bool namedBy(const zs::PageTable &table, const std::string &key)
    {
        for (size_t s = 0; s < table.count; ++s)
            for (const zs::Entry &entry : table.sections[s].entries)
                if ((entry.key && key == entry.key) || (entry.prefix && key.rfind(entry.prefix, 0) == 0))
                    return true;
        return false;
    }
}

void register_zge_settings_page_tests()
{
    MiniTest::Case("Settings page table", [](TestCase &tc)
    {
        tc.Run("six sections in the page's order", [](TestCase &t)
        {
            const zs::PageTable table = zs::pageTable();
            t.Equals(table.count, static_cast<size_t>(6), "sections");
            const char *const titles[] = {"Display", "Graphics", "Sound", "Controls", "Gameplay", "Game"};
            for (size_t s = 0; s < table.count && s < 6; ++s)
                t.IsTrue(std::strcmp(table.sections[s].title, titles[s]) == 0, std::string("section ") + titles[s]);
        });

        tc.Run("Always on Top under Map Window, as the collection menu has it", [](TestCase &t)
        {
            const zs::PageTable table = zs::pageTable();
            std::vector<std::string> display;
            for (const zs::Entry &entry : table.sections[0].entries)
                if (entry.key)
                    display.push_back(entry.key);
            const auto window = std::find(display.begin(), display.end(), "display.map_window");
            t.IsTrue(window != display.end() && window + 1 != display.end() && *(window + 1) == "display.map_window_on_top",
                     "display.map_window_on_top the row after display.map_window");
        });

        tc.Run("every key the three games register has a place", [](TestCase &t)
        {
            const zs::PageTable table = zs::pageTable();
            for (const char *key : kRegisteredKeys)
                t.IsTrue(namedBy(table, key), std::string("named: ") + key);
        });

        tc.Run("no key twice, every entry one kind", [](TestCase &t)
        {
            const zs::PageTable table = zs::pageTable();
            std::set<std::string> seen;
            for (size_t s = 0; s < table.count; ++s)
                for (const zs::Entry &entry : table.sections[s].entries)
                {
                    const int kinds = (entry.key ? 1 : 0) + (entry.heading ? 1 : 0) + (entry.prefix ? 1 : 0);
                    t.Equals(kinds, 1, "one kind per entry");
                    if (entry.key)
                        t.IsTrue(seen.insert(entry.key).second, std::string("once: ") + entry.key);
                }
        });

        tc.Run("a key's group has a home section", [](TestCase &t)
        {
            // A key a game adds later lands in the section its group lives
            // in: every group named by a row entry is some section's home.
            const zs::PageTable table = zs::pageTable();
            std::set<std::string> homes;
            for (size_t s = 0; s < table.count; ++s)
                for (const char *prefix : table.sections[s].homePrefixes)
                    homes.insert(prefix);
            for (size_t s = 0; s < table.count; ++s)
                for (const zs::Entry &entry : table.sections[s].entries)
                    if (entry.key)
                    {
                        const std::string key = entry.key;
                        const std::string group = key.substr(0, key.find('.') + 1);
                        t.IsTrue(homes.count(group) == 1, "home for " + key);
                    }
        });

        tc.Run("L2 / R2: the top options, KEYS and BUTTONS in turn", [](TestCase &t)
        {
            // The Controls section as rows: one row per row entry, two per
            // prefix (a game's key and pad rows), a heading per heading.
            const zs::PageTable table = zs::pageTable();
            std::vector<bool> headings;
            std::vector<std::string> keys;
            for (size_t s = 0; s < table.count; ++s)
                if (std::strcmp(table.sections[s].title, "Controls") == 0)
                    for (const zs::Entry &entry : table.sections[s].entries)
                    {
                        const int count = entry.prefix ? 2 : 1;
                        for (int i = 0; i < count; ++i)
                        {
                            headings.push_back(entry.heading != nullptr);
                            keys.push_back(entry.key ? entry.key : entry.prefix ? entry.prefix : entry.heading);
                        }
                    }
            const auto keyAt = [&keys](int row) { return row >= 0 && row < static_cast<int>(keys.size()) ? keys[static_cast<size_t>(row)] : std::string(); };
            int row = 0;
            t.Equals(keyAt(row), std::string("controls.prompts"), "the top");
            row = zs::headingJump(headings, row, 1);
            t.Equals(keyAt(row), std::string("controls.key."), "R2: the first key row");
            row = zs::headingJump(headings, row + 1, 1);
            t.Equals(keyAt(row), std::string("controls.gamepad_layout"), "R2 again: BUTTONS' first row, Gamepad Layout");
            row = zs::headingJump(headings, row, 1);
            t.Equals(keyAt(row), std::string("controls.prompts"), "R2 at the last part: round to the top");
            row = zs::headingJump(headings, row, -1);
            t.Equals(keyAt(row), std::string("controls.gamepad_layout"), "L2 at the top: round to BUTTONS");
            row = zs::headingJump(headings, row + 2, -1);
            t.Equals(keyAt(row), std::string("controls.key."), "L2 inside BUTTONS: KEYS");
            t.Equals(zs::headingJump({false, false, false}, 2, 1), 0, "no headings: the first row");
            t.Equals(zs::headingJump({}, 0, 1), 0, "no rows: where it was");
        });

        tc.Run("with nothing waiting for a restart the page is cf196d42b's, row for row; Restart Game comes last in Game", [](TestCase &t)
        {
            namespace st = zx::settings;
            st::clear();
            for (const char *key : kRegisteredKeys)
                if (std::strcmp(key, "game.restart") != 0)
                {
                    st::Setting setting;
                    setting.key = key;
                    setting.label = key;
                    st::add(setting);
                }
            const zs::PageTable table = zs::pageTable();
            auto shown = [](size_t section, const char *title)
            {
                std::string line = title + std::string(":");
                for (const std::string &row : zs::shownRows(section))
                    line += " " + row;
                return line;
            };
            t.Equals(table.count, std::size(kPageBefore), "as many sections");
            for (size_t s = 0; s < table.count && s < std::size(kPageBefore); ++s)
                t.Equals(shown(s, table.sections[s].title), std::string(kPageBefore[s]), std::string("as before: ") + table.sections[s].title);
            st::Setting restart;
            restart.key = "game.restart";
            restart.label = "Restart Game";
            restart.kind = st::Kind::Action;
            st::add(restart);
            const size_t game = table.count - 1;
            t.Equals(shown(game, table.sections[game].title), std::string(kPageBefore[game]) + " game.restart",
                     "a change waiting: Restart Game after the rows as they were");
            st::clear();
        });

        tc.Run("the headings sit where the design puts them", [](TestCase &t)
        {
            const zs::PageTable table = zs::pageTable();
            std::vector<std::string> headings;
            for (size_t s = 0; s < table.count; ++s)
                for (const zs::Entry &entry : table.sections[s].entries)
                    if (entry.heading)
                        headings.push_back(std::string(table.sections[s].title) + "/" + entry.heading);
            const std::vector<std::string> expected = {"Controls/KEYS",      "Controls/BUTTONS", "Gameplay/CAMERA",
                                                       "Gameplay/ASSISTS",   "Gameplay/CHEATS",  "Game/EXTRAS"};
            t.IsTrue(headings == expected, "headings");
            // Unlock Everything is the last row of Gameplay.
            for (size_t s = 0; s < table.count; ++s)
                if (std::strcmp(table.sections[s].title, "Gameplay") == 0)
                {
                    const zs::Entry &last = table.sections[s].entries.back();
                    t.IsTrue(last.key && std::strcmp(last.key, "cheat.unlock_all") == 0, "Unlock Everything last");
                }
        });
    });
}
