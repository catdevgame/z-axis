// A game boot's BootConfig (runtime/boot_config.h): what the app gives
// each game it boots, read in place of the environment, the environment
// still winning; the app's settings made into one (zx::app::gameBootConfig).
#include "MiniTest.h"

#include "collection/settings.h"
#include "runtime/boot_config.h"

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
            zx::boot::end();
        }
    };

    std::string textOf(const char *name)
    {
        const char *value = zx::boot::text(name);
        return value ? value : "(null)";
    }

    int g_reads = 0;
    std::string readZoo()
    {
        ++g_reads;
        return textOf("ZX_ZOO");
    }
}

void register_boot_config_tests()
{
    MiniTest::Case("BootConfig", [](TestCase &tc)
    {
        tc.Run("carries the app's names and the check switches, nothing else", [](TestCase &t)
        {
            zx::BootConfig config;
            t.IsTrue(config.set("ZX_ZOO", "1"), "a game setting");
            t.IsTrue(config.set("ZX_SAVES_DIR", "/saves"), "a game folder");
            t.IsTrue(config.set("ZX_PAD_SCRIPT", "0:start"), "a check switch");
            t.IsFalse(config.set("ZX_GS_NULL", "1"), "a process switch is not carried");
            t.IsFalse(config.set("HOME", "/x"), "nor anything else");
            t.IsTrue(config.find("ZX_GS_NULL") == nullptr, "and not kept");
            t.Equals(*config.find("ZX_ZOO"), std::string("1"), "kept");
            for (const char *name : {"ZX_GS_BACKEND", "ZX_GS_SCALE", "ZX_GS_FULL_HEIGHT", "ZX_GAMEPAD_MAPPINGS",
                                     "ZX_SAVES_DIR", "ZX_MODS_DIR", "ZX_XBOX_DIR", "ZX_XBOX_CONVERTED",
                                     "ZX_SAVE_PROFILE", "ZX_GAME_SETTINGS_MENUS", "ZX_LANGUAGE",
                                     "ZX_FRENCH_VOICES", "ZX_DEV_MENU", "ZX_ZOO", "ZX_ESTATE_DIR", "ZX_ESTATE",
                                     "ZX_DM2_XBOX_LEVELS_DIR", "ZX_DM2_XBOX_LEVELS", "ZX_ZOO_UNLOCK", "ZX_TUNING",
                                     "ZX_CAREER_SCRIPTS", "ZX_XBOX_FREESKATE", "ZX_APE", "ZX_GAMEPLAY_INI",
                                     "ZX_SCREENSHOT_DIR", "ZX_CONTROLS_INI", "ZX_XBOX_PACK", "ZX_XBOX_TEXTURES",
                                     "ZX_XBOX_MOVIES", "ZX_XBOX_MODELS", "ZX_RENDER_PATH", "ZX_DM2_CREATE_PLAYER", "ZX_DM2_XBOX_THEMES",
                                     "ZX_DM2_RETRO_MOVIE", "ZX_DM2_ATTRACT_DEMO", "ZX_BMX_PARK_EDITOR",
                                     "ZX_BMX_RELEASES_DIR", "ZX_BMX_XBOX_CHALLENGES", "ZX_BMX_EDITOR_PIECES",
                                     "ZX_BMX_LEVEL_PREVIEWS", "ZX_BMX_LOGO_MOVIE", "ZX_BMX_INTRO_MOVIE",
                                     "ZX_BMX_GC_LEVELS", "ZX_BMX_MENU_LINES", "ZX_BMX_ALL_MP_MODES",
                                     "ZX_BMX_LOGO_SPLASH", "ZX_BMX_TRAILERS", "ZX_BMX_CAR_TEXTURES",
                                     "ZX_BMX_GREEN_SKIN", "ZX_BMX_TOPLESS", "ZX_BMX_XBOX_SONGS",
                                     "ZX_GS_PIPELINE_CACHE", "ZX_DRAW_DISTANCE"})
                t.IsTrue(zx::BootConfig::carries(name), std::string("the app's ") + name);
        });

        tc.Run("the environment wins when set and not empty; then the boot's; a value set over it wins", [](TestCase &t)
        {
            EnvGuard env({"ZX_ZOO", "ZX_LANGUAGE", "ZX_TUNING", "ZX_GS_SCALE", "ZX_APE"});
            zx::BootConfig config;
            config.set("ZX_ZOO", "1");
            config.set("ZX_LANGUAGE", "french");
            config.set("ZX_TUNING", "xbox");
            config.setOverEnvironment("ZX_GS_SCALE", "3");
            zx::boot::begin(config);
            setEnv("ZX_LANGUAGE", "german");
            setEnv("ZX_TUNING", "");
            setEnv("ZX_GS_SCALE", "2");
            t.Equals(textOf("ZX_ZOO"), std::string("1"), "not in the environment: the boot's");
            t.Equals(textOf("ZX_LANGUAGE"), std::string("german"), "in the environment: the environment's");
            t.Equals(textOf("ZX_TUNING"), std::string("xbox"), "empty in the environment: the boot's");
            t.Equals(textOf("ZX_GS_SCALE"), std::string("3"), "set over the environment: the boot's");
            t.Equals(textOf("ZX_APE"), std::string("(null)"), "in neither: none");
            t.IsFalse(zx::boot::has("ZX_APE"), "has: none");
            // An empty value: POSIX keeps it, Windows' _putenv_s removes the
            // variable. Either way text() gives what getenv gives.
            setEnv("ZX_APE", "");
            const char *raw = std::getenv("ZX_APE");
            t.Equals(textOf("ZX_APE"), std::string(raw ? raw : "(null)"), "empty in the environment alone: as getenv");
            t.Equals(config.text("ZX_LANGUAGE"), std::string("german"), "BootConfig::text: the same order");
            t.Equals(config.text("ZX_ZOO"), std::string("1"), "BootConfig::text: its own");
            t.Equals(config.text("ZX_GS_SCALE"), std::string("3"), "BootConfig::text: over the environment");
            t.IsTrue(zx::boot::change("ZX_LANGUAGE", "spanish"), "changed while it runs");
            t.Equals(textOf("ZX_LANGUAGE"), std::string("spanish"), "a change wins over the environment");
            t.IsFalse(zx::boot::change("ZX_GS_NULL", "1"), "a process switch cannot be changed");
        });

        tc.Run("each boot its own: values, game, and what is read once per boot", [](TestCase &t)
        {
            EnvGuard env({"ZX_ZOO"});
            static zx::boot::PerBoot<std::string> zoo(&readZoo);
            g_reads = 0;
            zx::BootConfig first;
            first.game = "ai";
            first.set("ZX_ZOO", "1");
            const char *kept = nullptr;
            {
                zx::boot::Scope boot(first);
                kept = zx::boot::text("ZX_ZOO");
                t.Equals(zx::boot::game(), std::string("ai"), "first boot: its game");
                t.Equals(zoo.get(), std::string("1"), "first boot: read");
                t.Equals(zoo.get(), std::string("1"), "first boot: kept");
                t.Equals(g_reads, 1, "read once in the boot");
            }
            t.Equals(textOf("ZX_ZOO"), std::string("(null)"), "between boots: nothing");
            t.Equals(zx::boot::game(), std::string(""), "between boots: no game");
            zx::BootConfig second;
            second.game = "dm2";
            {
                zx::boot::Scope boot(second);
                t.Equals(zx::boot::game(), std::string("dm2"), "second boot: its game");
                t.Equals(zoo.get(), std::string("(null)"), "second boot: not the first's");
                t.Equals(g_reads, 2, "read again for the new boot");
                t.Equals(std::string(kept), std::string("1"), "the first boot's text is still good");
            }
            setEnv("ZX_ZOO", "0");
            {
                zx::boot::Scope boot(first);
                t.Equals(zoo.get(), std::string("0"), "third boot: the environment over its value");
            }
        });

        tc.Run("the app's settings to a game's BootConfig: its options, folders and renderer", [](TestCase &t)
        {
            namespace app = zx::app;
            const std::filesystem::path resources =
                std::filesystem::temp_directory_path() / ("zx-bootcfg-" + std::to_string(std::rand()));
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

            const zx::BootConfig ai =
                app::gameBootConfig("ai", settings, paths, app::gameSettingsProfile("ai"), folders, noEnv);
            auto value = [](const zx::BootConfig &c, const char *name)
            {
                const std::string *v = c.find(name);
                return v ? *v : std::string("(none)");
            };
            t.Equals(ai.game, std::string("ai"), "its game");
            t.Equals(value(ai, "ZX_ZOO"), std::string("1"), "AI: the Zoo");
            t.Equals(value(ai, "ZX_LANGUAGE"), std::string("german"), "AI: its language");
            t.Equals(value(ai, "ZX_DM2_XBOX_LEVELS"), std::string("(none)"), "AI: no DM2 option");
            t.Equals(value(ai, "ZX_SAVE_PROFILE"), std::string("Mine"), "the save profile");
            t.Equals(value(ai, "ZX_SAVES_DIR"), paths.savesDir.string(), "its saves");
            t.Equals(value(ai, "ZX_MODS_DIR"), (paths.supportDir / "Mods").string(), "its mods");
            t.Equals(value(ai, "ZX_GS_PIPELINE_CACHE"), (paths.supportDir / "gpu_pipelines.txt").string(),
                     "its GPU pipelines");
            t.Equals(value(ai, "ZX_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "its gameplay.ini");
            t.Equals(value(ai, "ZX_CONTROLS_INI"), paths.controlsFile.string(), "its controls");
            t.Equals(value(ai, "ZX_SCREENSHOT_DIR"), std::string("/pictures/ai"), "its screenshots");
            t.Equals(value(ai, "ZX_GAMEPAD_MAPPINGS"), folders.gamepadMappings, "the collection's mappings");
            t.Equals(value(ai, "ZX_XBOX_PACK"), (resources / "xbox-pack").string(), "a bundled folder");
            t.Equals(value(ai, "ZX_XBOX_DIR"), std::string("(none)"), "a folder not bundled");
            t.Equals(value(ai, "ZX_GAME_SETTINGS_MENUS"), std::string("1"), "the settings in the game's menus");
            t.Equals(value(ai, "ZX_RENDER_PATH"), std::string("strict"), "native meshes off");
            t.Equals(value(ai, "ZX_GS_BACKEND"), std::string("gpu"), "the renderer");
            t.Equals(value(ai, "ZX_GS_SCALE"), std::string("3"), "its resolution");
            t.Equals(value(ai, "ZX_GS_FULL_HEIGHT"), std::string("1"), "full height");
            t.IsTrue(ai.overEnvironment("ZX_GS_SCALE"), "the renderer settings over the environment's");
            t.IsFalse(ai.overEnvironment("ZX_ZOO"), "the rest under it");

            const zx::BootConfig dm2 =
                app::gameBootConfig("dm2", settings, paths, app::gameSettingsProfile("dm2"), folders, noEnv);
            t.Equals(value(dm2, "ZX_ZOO"), std::string("(none)"), "DM2: no Zoo");
            t.Equals(value(dm2, "ZX_DM2_XBOX_LEVELS"), std::string("1"), "DM2: its Xbox levels");
            t.Equals(value(dm2, "ZX_DM2_XBOX_LEVELS_DIR"), (resources / "xbox-levels").string(), "DM2: their folder");
            t.Equals(value(dm2, "ZX_DM2_CREATE_PLAYER"), std::string("0"), "DM2: Create Player off");
            t.Equals(value(dm2, "ZX_DM2_XBOX_THEMES") + value(dm2, "ZX_DM2_RETRO_MOVIE") + value(dm2, "ZX_DM2_ATTRACT_DEMO"),
                     std::string("000"), "DM2: the Xbox themes, Retro movie and attract demo off");
            t.Equals(value(dm2, "ZX_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "DM2: its gameplay.ini");
            const zx::BootConfig bmx =
                app::gameBootConfig("bmx", settings, paths, app::gameSettingsProfile("bmx"), folders, noEnv);
            t.Equals(value(bmx, "ZX_GAMEPLAY_INI"), (paths.supportDir / "gameplay.ini").string(), "BMX XXX: its gameplay.ini");
            t.Equals(value(bmx, "ZX_BMX_PARK_EDITOR"), std::string(settings.bmxParkEditor ? "1" : "0"),
                     "BMX XXX: its park editor line as set");
            t.Equals(value(bmx, "ZX_BMX_XBOX_CHALLENGES"), std::string("0"), "BMX XXX: the Xbox challenge scripts off");
            t.Equals(value(bmx, "ZX_BMX_INTRO_MOVIE"), std::string("0"), "BMX XXX: the intro movie off");
            t.Equals(value(bmx, "ZX_LANGUAGE"), std::string("german"), "BMX XXX: its language");
            t.Equals(value(bmx, "ZX_BMX_RELEASES_DIR"), std::string("(none)"), "BMX XXX: no other releases' files bundled");
            t.Equals(value(dm2, "ZX_BMX_XBOX_CHALLENGES"), std::string("(none)"), "DM2: no BMX XXX option");
            // The draw distance at boot: it changes each game, Original unless picked.
            t.Equals(value(ai, "ZX_DRAW_DISTANCE"), std::string("0"), "AI: Original by default");
            t.Equals(value(dm2, "ZX_DRAW_DISTANCE"), std::string("0"), "DM2: Original by default");
            t.Equals(value(bmx, "ZX_DRAW_DISTANCE"), std::string("0"), "BMX XXX: Original by default");
            app::Settings farthest = settings;
            farthest.drawDistance = 2;
            t.Equals(value(app::gameBootConfig("bmx", farthest, paths, app::gameSettingsProfile("bmx"), folders, noEnv),
                           "ZX_DRAW_DISTANCE"),
                     std::string("2"), "BMX XXX: as picked");

            auto cpuEnv = [](const char *name) -> const char * { return std::strcmp(name, "ZX_GS_BACKEND") == 0 ? "cpu" : nullptr; };
            const zx::BootConfig picked =
                app::gameBootConfig("ai", settings, paths, app::gameSettingsProfile("ai"), folders, cpuEnv);
            t.IsTrue(picked.find("ZX_GS_BACKEND") == nullptr && picked.find("ZX_GS_SCALE") == nullptr &&
                         picked.find("ZX_GS_FULL_HEIGHT") == nullptr,
                     "the environment picks the renderer: all three its own");
            std::error_code ec;
            std::filesystem::remove_all(resources, ec);
        });
    });
}
