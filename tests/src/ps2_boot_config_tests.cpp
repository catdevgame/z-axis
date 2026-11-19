// A game boot's BootConfig (runtime/ps2_boot_config.h): what the app gives
// each game it boots, read in place of the environment, the environment
// still winning; the app's settings made into one (ps2x::app::gameBootConfig).
#include "MiniTest.h"

#include "collection/settings.h"
#include "runtime/ps2_boot_config.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    void setEnv(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            ::setenv(name, value, 1);
        else
            ::unsetenv(name);
#endif
    }

    // The environment as it was for the names a test sets, put back after.
    struct EnvGuard
    {
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;
        explicit EnvGuard(std::initializer_list<const char *> names)
        {
            for (const char *name : names)
            {
                const char *value = std::getenv(name);
                saved.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
                setEnv(name, nullptr);
            }
        }
        ~EnvGuard()
        {
            for (const auto &[name, value] : saved)
                setEnv(name.c_str(), value ? value->c_str() : nullptr);
            ps2x::boot::end();
        }
    };

    std::string textOf(const char *name)
    {
        const char *value = ps2x::boot::text(name);
        return value ? value : "(null)";
    }

    int g_reads = 0;
    std::string readZoo()
    {
        ++g_reads;
        return textOf("PS2X_ZOO");
    }
}

void register_ps2_boot_config_tests()
{
    MiniTest::Case("BootConfig", [](TestCase &tc)
    {
        tc.Run("carries the app's names and the check switches, nothing else", [](TestCase &t)
        {
            ps2x::BootConfig config;
            t.IsTrue(config.set("PS2X_ZOO", "1"), "a game setting");
            t.IsTrue(config.set("PS2X_SAVES_DIR", "/saves"), "a game folder");
            t.IsTrue(config.set("PS2X_PAD_SCRIPT", "0:start"), "a check switch");
            t.IsFalse(config.set("PS2X_GS_NULL", "1"), "a process switch is not carried");
            t.IsFalse(config.set("HOME", "/x"), "nor anything else");
            t.IsTrue(config.find("PS2X_GS_NULL") == nullptr, "and not kept");
            t.Equals(*config.find("PS2X_ZOO"), std::string("1"), "kept");
            for (const char *name : {"PS2X_GS_BACKEND", "PS2X_GS_SCALE", "PS2X_GS_FULL_HEIGHT", "PS2X_GAMEPAD_MAPPINGS",
                                     "PS2X_SAVES_DIR", "PS2X_MODS_DIR", "PS2X_XBOX_DIR", "PS2X_XBOX_CONVERTED",
                                     "PS2X_SAVE_PROFILE", "PS2X_GAME_SETTINGS_MENUS", "PS2X_LANGUAGE",
                                     "PS2X_FRENCH_VOICES", "PS2X_DEV_MENU", "PS2X_ZOO", "PS2X_ESTATE_DIR", "PS2X_ESTATE",
                                     "PS2X_DM2_XBOX_LEVELS_DIR", "PS2X_DM2_XBOX_LEVELS", "PS2X_ZOO_UNLOCK", "PS2X_TUNING",
                                     "PS2X_CAREER_SCRIPTS", "PS2X_XBOX_FREESKATE", "PS2X_APE", "PS2X_GAMEPLAY_INI",
                                     "PS2X_SCREENSHOT_DIR", "PS2X_CONTROLS_INI", "PS2X_XBOX_PACK", "PS2X_XBOX_TEXTURES",
                                     "PS2X_XBOX_MOVIES", "PS2X_XBOX_MODELS", "PS2X_RENDER_PATH", "PS2X_DM2_CREATE_PLAYER", "PS2X_DM2_XBOX_THEMES",
                                     "PS2X_DM2_RETRO_MOVIE", "PS2X_DM2_ATTRACT_DEMO", "PS2X_BMX_PARK_EDITOR",
                                     "PS2X_BMX_RELEASES_DIR", "PS2X_BMX_XBOX_CHALLENGES", "PS2X_BMX_EDITOR_PIECES",
                                     "PS2X_BMX_LEVEL_PREVIEWS", "PS2X_BMX_LOGO_MOVIE", "PS2X_BMX_INTRO_MOVIE",
                                     "PS2X_GS_PIPELINE_CACHE", "PS2X_DRAW_DISTANCE"})
                t.IsTrue(ps2x::BootConfig::carries(name), std::string("the app's ") + name);
        });

        tc.Run("the environment wins when set and not empty; then the boot's; a value set over it wins", [](TestCase &t)
        {
            EnvGuard env({"PS2X_ZOO", "PS2X_LANGUAGE", "PS2X_TUNING", "PS2X_GS_SCALE", "PS2X_APE"});
            ps2x::BootConfig config;
            config.set("PS2X_ZOO", "1");
            config.set("PS2X_LANGUAGE", "french");
            config.set("PS2X_TUNING", "xbox");
            config.setOverEnvironment("PS2X_GS_SCALE", "3");
            ps2x::boot::begin(config);
            setEnv("PS2X_LANGUAGE", "german");
            setEnv("PS2X_TUNING", "");
            setEnv("PS2X_GS_SCALE", "2");
            t.Equals(textOf("PS2X_ZOO"), std::string("1"), "not in the environment: the boot's");
            t.Equals(textOf("PS2X_LANGUAGE"), std::string("german"), "in the environment: the environment's");
            t.Equals(textOf("PS2X_TUNING"), std::string("xbox"), "empty in the environment: the boot's");
            t.Equals(textOf("PS2X_GS_SCALE"), std::string("3"), "set over the environment: the boot's");
            t.Equals(textOf("PS2X_APE"), std::string("(null)"), "in neither: none");
            t.IsFalse(ps2x::boot::has("PS2X_APE"), "has: none");
            // An empty value: POSIX keeps it, Windows' _putenv_s removes the
            // variable. Either way text() gives what getenv gives.
            setEnv("PS2X_APE", "");
            const char *raw = std::getenv("PS2X_APE");
            t.Equals(textOf("PS2X_APE"), std::string(raw ? raw : "(null)"), "empty in the environment alone: as getenv");
            t.Equals(config.text("PS2X_LANGUAGE"), std::string("german"), "BootConfig::text: the same order");
            t.Equals(config.text("PS2X_ZOO"), std::string("1"), "BootConfig::text: its own");
            t.Equals(config.text("PS2X_GS_SCALE"), std::string("3"), "BootConfig::text: over the environment");
            t.IsTrue(ps2x::boot::change("PS2X_LANGUAGE", "spanish"), "changed while it runs");
            t.Equals(textOf("PS2X_LANGUAGE"), std::string("spanish"), "a change wins over the environment");
            t.IsFalse(ps2x::boot::change("PS2X_GS_NULL", "1"), "a process switch cannot be changed");
        });

        tc.Run("each boot its own: values, game, and what is read once per boot", [](TestCase &t)
        {
            EnvGuard env({"PS2X_ZOO"});
            static ps2x::boot::PerBoot<std::string> zoo(&readZoo);
            g_reads = 0;
            ps2x::BootConfig first;
            first.game = "ai";
            first.set("PS2X_ZOO", "1");
            const char *kept = nullptr;
            {
                ps2x::boot::Scope boot(first);
                kept = ps2x::boot::text("PS2X_ZOO");
                t.Equals(ps2x::boot::game(), std::string("ai"), "first boot: its game");
                t.Equals(zoo.get(), std::string("1"), "first boot: read");
                t.Equals(zoo.get(), std::string("1"), "first boot: kept");
                t.Equals(g_reads, 1, "read once in the boot");
            }
            t.Equals(textOf("PS2X_ZOO"), std::string("(null)"), "between boots: nothing");
            t.Equals(ps2x::boot::game(), std::string(""), "between boots: no game");
            ps2x::BootConfig second;
            second.game = "dm2";
            {
                ps2x::boot::Scope boot(second);
                t.Equals(ps2x::boot::game(), std::string("dm2"), "second boot: its game");
                t.Equals(zoo.get(), std::string("(null)"), "second boot: not the first's");
                t.Equals(g_reads, 2, "read again for the new boot");
                t.Equals(std::string(kept), std::string("1"), "the first boot's text is still good");
            }
            setEnv("PS2X_ZOO", "0");
            {
                ps2x::boot::Scope boot(first);
                t.Equals(zoo.get(), std::string("0"), "third boot: the environment over its value");
            }
        });

        tc.Run("the app's settings to a game's BootConfig: its options, folders and renderer", [](TestCase &t)
        {
            namespace app = ps2x::app;
            const std::filesystem::path resources =
                std::filesystem::temp_directory_path() / ("ps2x-bootcfg-" + std::to_string(std::rand()));
            std::filesystem::create_directories(resources / "xbox-pack");
            std::filesystem::create_directories(resources / "xbox-levels");
            app::Settings settings;
            settings.zoo = true;
            settings.dm2XboxLevels = true;
            settings.language = 2;
            settings.saveProfile = "Mine";
            settings.gpuRenderer = true;
            settings.internalResolution = 3;
            settings.fullHeight = true;
            settings.nativeMeshes = false;
            app::AppPaths paths;
            paths.supportDir = "/support/games/ai";
            paths.savesDir = "/support/games/ai/Saves";
            paths.controlsFile = "/support/games/ai/controls.ini";
            app::BootFolders folders;
            folders.resources = resources;
            folders.screenshots = "/pictures/ai";
            folders.gamepadMappings = "/support/gamecontrollerdb.txt";
            auto noEnv = [](const char *) -> const char * { return nullptr; };

            const ps2x::BootConfig ai =
                app::gameBootConfig("ai", settings, paths, app::gameSettingsProfile("ai"), folders, noEnv);
            auto value = [](const ps2x::BootConfig &c, const char *name)
            {
                const std::string *v = c.find(name);
                return v ? *v : std::string("(none)");
            };
            t.Equals(ai.game, std::string("ai"), "its game");
            t.Equals(value(ai, "PS2X_ZOO"), std::string("1"), "AI: the Zoo");
            t.Equals(value(ai, "PS2X_LANGUAGE"), std::string("german"), "AI: its language");
            t.Equals(value(ai, "PS2X_DM2_XBOX_LEVELS"), std::string("(none)"), "AI: no DM2 option");
            t.Equals(value(ai, "PS2X_SAVE_PROFILE"), std::string("Mine"), "the save profile");
            t.Equals(value(ai, "PS2X_SAVES_DIR"), paths.savesDir.string(), "its saves");
            t.Equals(value(ai, "PS2X_MODS_DIR"), (paths.supportDir / "Mods").string(), "its mods");
            t.Equals(value(ai, "PS2X_GS_PIPELINE_CACHE"), (paths.supportDir / "gpu_pipelines.txt").string(),
                     "its GPU pipelines");
            t.Equals(value(ai, "PS2X_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "its gameplay.ini");
            t.Equals(value(ai, "PS2X_CONTROLS_INI"), paths.controlsFile.string(), "its controls");
            t.Equals(value(ai, "PS2X_SCREENSHOT_DIR"), std::string("/pictures/ai"), "its screenshots");
            t.Equals(value(ai, "PS2X_GAMEPAD_MAPPINGS"), folders.gamepadMappings, "the collection's mappings");
            t.Equals(value(ai, "PS2X_XBOX_PACK"), (resources / "xbox-pack").string(), "a bundled folder");
            t.Equals(value(ai, "PS2X_XBOX_DIR"), std::string("(none)"), "a folder not bundled");
            t.Equals(value(ai, "PS2X_GAME_SETTINGS_MENUS"), std::string("1"), "the settings in the game's menus");
            t.Equals(value(ai, "PS2X_RENDER_PATH"), std::string("strict"), "native meshes off");
            t.Equals(value(ai, "PS2X_GS_BACKEND"), std::string("gpu"), "the renderer");
            t.Equals(value(ai, "PS2X_GS_SCALE"), std::string("3"), "its resolution");
            t.Equals(value(ai, "PS2X_GS_FULL_HEIGHT"), std::string("1"), "full height");
            t.IsTrue(ai.overEnvironment("PS2X_GS_SCALE"), "the renderer settings over the environment's");
            t.IsFalse(ai.overEnvironment("PS2X_ZOO"), "the rest under it");

            const ps2x::BootConfig dm2 =
                app::gameBootConfig("dm2", settings, paths, app::gameSettingsProfile("dm2"), folders, noEnv);
            t.Equals(value(dm2, "PS2X_ZOO"), std::string("(none)"), "DM2: no Zoo");
            t.Equals(value(dm2, "PS2X_DM2_XBOX_LEVELS"), std::string("1"), "DM2: its Xbox levels");
            t.Equals(value(dm2, "PS2X_DM2_XBOX_LEVELS_DIR"), (resources / "xbox-levels").string(), "DM2: their folder");
            t.Equals(value(dm2, "PS2X_DM2_CREATE_PLAYER"), std::string("0"), "DM2: Create Player off");
            t.Equals(value(dm2, "PS2X_DM2_XBOX_THEMES") + value(dm2, "PS2X_DM2_RETRO_MOVIE") + value(dm2, "PS2X_DM2_ATTRACT_DEMO"),
                     std::string("000"), "DM2: the Xbox themes, Retro movie and attract demo off");
            t.Equals(value(dm2, "PS2X_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "DM2: its gameplay.ini");
            const ps2x::BootConfig bmx =
                app::gameBootConfig("bmx", settings, paths, app::gameSettingsProfile("bmx"), folders, noEnv);
            t.Equals(value(bmx, "PS2X_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "BMX XXX: its gameplay.ini");
            t.Equals(value(bmx, "PS2X_BMX_PARK_EDITOR"), std::string(settings.bmxParkEditor ? "1" : "0"),
                     "BMX XXX: its park editor line as set");
            t.Equals(value(bmx, "PS2X_BMX_XBOX_CHALLENGES"), std::string("0"), "BMX XXX: the Xbox challenge scripts off");
            t.Equals(value(bmx, "PS2X_BMX_INTRO_MOVIE"), std::string("0"), "BMX XXX: the intro movie off");
            t.Equals(value(bmx, "PS2X_LANGUAGE"), std::string("german"), "BMX XXX: its language");
            t.Equals(value(bmx, "PS2X_BMX_RELEASES_DIR"), std::string("(none)"), "BMX XXX: no other releases' files bundled");
            t.Equals(value(dm2, "PS2X_BMX_XBOX_CHALLENGES"), std::string("(none)"), "DM2: no BMX XXX option");
            // The draw distance at boot: it changes each game, Original unless picked.
            t.Equals(value(ai, "PS2X_DRAW_DISTANCE"), std::string("0"), "AI: Original by default");
            t.Equals(value(dm2, "PS2X_DRAW_DISTANCE"), std::string("0"), "DM2: Original by default");
            t.Equals(value(bmx, "PS2X_DRAW_DISTANCE"), std::string("0"), "BMX XXX: Original by default");
            app::Settings farthest = settings;
            farthest.drawDistance = 2;
            t.Equals(value(app::gameBootConfig("bmx", farthest, paths, app::gameSettingsProfile("bmx"), folders, noEnv),
                           "PS2X_DRAW_DISTANCE"),
                     std::string("2"), "BMX XXX: as picked");

            auto cpuEnv = [](const char *name) -> const char * { return std::strcmp(name, "PS2X_GS_BACKEND") == 0 ? "cpu" : nullptr; };
            const ps2x::BootConfig picked =
                app::gameBootConfig("ai", settings, paths, app::gameSettingsProfile("ai"), folders, cpuEnv);
            t.IsTrue(picked.find("PS2X_GS_BACKEND") == nullptr && picked.find("PS2X_GS_SCALE") == nullptr &&
                         picked.find("PS2X_GS_FULL_HEIGHT") == nullptr,
                     "the environment picks the renderer: all three its own");
            std::error_code ec;
            std::filesystem::remove_all(resources, ec);
        });
    });
}
