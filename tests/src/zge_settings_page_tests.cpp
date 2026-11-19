// The Settings page's table (game_overrides/zge_settings.cpp, kPage): every
// key the three games register today has a place, and no key has two.

#include "MiniTest.h"
#include "zge_settings.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace zs = zge_host::settings;

    // The keys the games' pages show today (runtime/src/collection/app.cpp
    // registerSettings, lib/gs/gs_quality.cpp, lib/ps2_pad.cpp
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
        "display.native_meshes", "display.variable_refresh",
        // sound
        "audio.volume", "audio.mute_in_background", "audio.movie_volume", "audio.french_voices", "audio.music_volume",
        "audio.effects_volume", "audio.speech_volume",
        // controls
        "controls.mouse_sensitivity", "controls.invert_y", "controls.mouse_mode", "controls.stick_deadzone",
        "controls.gamepad", "controls.gamepads", "controls.test_rumble", "controls.swap_players", "controls.prompts",
        "controls.key.jump", "controls.pad.jump", "controls.reset",
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
        "game.all_mp_modes",
        "game.zoo_unlock", "game.tuning",
        "game.career_scripts", "game.ape", "game.xbox_free_skate", "game.record_play", "game.recordings",
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
