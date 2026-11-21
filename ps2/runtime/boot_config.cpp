#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <unordered_set>

namespace zx
{
    namespace
    {
        // The names a BootConfig carries: what the app passes for each game,
        // and the check switches a run gives each game it boots.
        const std::vector<std::string> kNames = {
            // The app's renderer settings (read when a GS renderer is made).
            "ZX_GS_BACKEND",
            "ZX_GS_SCALE",
            "ZX_GS_FULL_HEIGHT",
            "ZX_RENDER_PATH",
            // A game's own renderer need (DM2's override: its mip levels in
            // a render target, from GS memory).
            "ZX_GS_MIP_TARGET_LEVELS",
            // BMX XXX's override: its 16-bit Z buffers may be D16 targets.
            "ZX_GS_D16_TARGETS",
            // The game's folders and files.
            "ZX_CONTROLS_INI",
            "ZX_GAMEPAD_MAPPINGS",
            "ZX_GAMEPLAY_INI",
            "ZX_SAVES_DIR",
            "ZX_MODS_DIR",
            // Texture packs (runtime/texture_pack.h): the folders, and dump mode's.
            "ZX_TEXTURE_PACKS",
            "ZX_TEXTURE_DUMP",
            "ZX_SCREENSHOT_DIR",
            "ZX_GS_PIPELINE_CACHE",
            "ZX_XBOX_DIR",
            "ZX_XBOX_PACK",
            "ZX_XBOX_CONVERTED",
            "ZX_ESTATE_DIR",
            "ZX_DM2_XBOX_LEVELS_DIR",
            "ZX_BMX_RELEASES_DIR",
            // The game's settings.ini.
            "ZX_SAVE_PROFILE",
            "ZX_GAME_SETTINGS_MENUS",
            "ZX_LANGUAGE",
            "ZX_FRENCH_VOICES",
            "ZX_DEV_MENU",
            "ZX_ZOO",
            "ZX_ZOO_UNLOCK",
            "ZX_ESTATE",
            "ZX_DM2_XBOX_LEVELS",
            "ZX_TUNING",
            "ZX_CAREER_SCRIPTS",
            "ZX_DRAW_DISTANCE",
            "ZX_XBOX_FREESKATE",
            "ZX_APE",
            "ZX_DM2_CREATE_PLAYER",
            "ZX_DM2_XBOX_THEMES",
            "ZX_DM2_RETRO_MOVIE",
            "ZX_DM2_ATTRACT_DEMO",
            "ZX_BMX_PARK_EDITOR",
            "ZX_BMX_XBOX_CHALLENGES",
            "ZX_BMX_EDITOR_PIECES",
            "ZX_BMX_LEVEL_PREVIEWS",
            "ZX_BMX_LOGO_MOVIE",
            "ZX_BMX_INTRO_MOVIE",
            "ZX_BMX_GC_LEVELS",
            "ZX_BMX_MENU_LINES",
            "ZX_BMX_ALL_MP_MODES",
            "ZX_BMX_LOGO_SPLASH",
            "ZX_BMX_TRAILERS",
            "ZX_BMX_CAR_TEXTURES",
            "ZX_BMX_GREEN_SKIN",
            "ZX_BMX_TOPLESS",
            "ZX_BMX_XBOX_SONGS",
            "ZX_XBOX_TEXTURES",
            "ZX_XBOX_MOVIES",
            "ZX_XBOX_MODELS",
            // [host]: the disc device's host form or the PS2's (native | ps2).
            "ZX_DISC",
            // Check switches, each boot its own: scripted input, the state
            // probe's dumps and log, the cards.
            "ZX_PAD_SCRIPT",
            "ZX_PAD_SCRIPT_CLOCK",
            "ZX_PAD_STICK_CENTER",
            "ZX_HOST_INPUT",
            "ZX_HOST_INPUT_SCRIPT",
            "ZX_POKE_SCRIPT",
            "ZX_DUMP_WHEN",
            "ZX_DUMP_PREFIX",
            "ZX_DUMP_EXTENDED",
            "ZX_DUMP_AT_FRAME",
            "ZX_SHOT_AT",
            "ZX_STATE_LOG",
            "ZX_STATE_LOG_FILE",
            "ZX_PAD_DUMP",
            "ZX_PAD_LOG",
            "ZX_SAVES",
            "ZX_MC_FD_BASE0",
            "ZX_MC_FIXED_TIME",
            "ZX_DEV_UNLOCK_ALL",
            // Aggressive Inline's start straight into a free skate level
            // (aggressive_inline_devstart.cpp): developer runs, and the
            // collection's Custom Freeplay starting a game into its map.
            "ZX_DEV_START_LEVEL",
            "ZX_DEV_START_SKATER",
            "ZX_DEV_START_SKATE",
            "ZX_DEV_START_QUIT",
            // DM2's park read from a host file instead of the memory card
            // (dave_mirra_saves.cpp): the Custom Freeplay map's.
            "ZX_DM2_PARK_FILE",
            // BMX XXX's start keeping the row's level record for the
            // player's save (bmx_xxx_devstart.cpp): the Custom Freeplay map's.
            "ZX_BMX_KEEP_LEVEL_GOALS",
            // Aggressive Inline's start keeping the level's records for the
            // player's save (aggressive_inline_devstart.cpp): the Custom
            // Freeplay map's.
            "ZX_AI_KEEP_LEVEL_RECORDS",
            // DM2's start keeping the row's level's saved records for the
            // player's save (dave_mirra_devstart.cpp): the Custom Freeplay map's.
            "ZX_DM2_KEEP_LEVEL_RECORDS",
            // DM2's level-point camera from the level's own camera points
            // (games/dm2/camera/camera.cpp levelCameraPoint): a Custom
            // Freeplay map's.
            "ZX_DM2_LEVEL_CAMERA_POINTS",
            // BMX XXX's levels' own NPC riders left out (bmx_xxx_npcs.cpp):
            // a Custom Freeplay map that does not keep its slot's.
            "ZX_BMX_LEVEL_NPCS",
            // The readable code without its proof scaffolding (ee/abi.h leanParts).
            "ZX_READABLE_LEAN",
            // Play recordings (runtime/play_recording.h): the app's setting
            // records each boot; a replay reads one.
            "ZX_RECORD",
            "ZX_RECORD_CHECKSUMS",
            "ZX_REPLAY",
            "ZX_REPLAY_STOP",
        };

        // The boot in progress: its config, and its values as texts kept for
        // the life of the process (a few dozen per boot), so a pointer text()
        // gave out never dangles and a read allocates nothing.
        struct Entry
        {
            const char *text = nullptr;
            bool overEnvironment = false;
        };
        struct Current
        {
            std::mutex mutex;
            std::string game;
            std::map<std::string, Entry, std::less<>> values;
            std::unordered_set<std::string> texts;
            std::atomic<uint64_t> generation{0};

            const char *keep(const std::string &text) { return texts.insert(text).first->c_str(); }
            void put(const std::string &name, const std::string &text, bool overEnvironment)
            {
                values[name] = Entry{keep(text), overEnvironment};
            }
        };

        Current &current()
        {
            static Current c;
            return c;
        }
    }

    bool BootConfig::set(const std::string &name, std::string value)
    {
        if (!carries(name))
            return false;
        m_values[name] = Value{std::move(value), false};
        return true;
    }

    bool BootConfig::setOverEnvironment(const std::string &name, std::string value)
    {
        if (!carries(name))
            return false;
        m_values[name] = Value{std::move(value), true};
        return true;
    }

    const std::string *BootConfig::find(const std::string &name) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? nullptr : &it->second.text;
    }

    std::string BootConfig::text(const std::string &name) const
    {
        const char *env = std::getenv(name.c_str());
        const std::string *own = find(name);
        if (own && (overEnvironment(name) || !env || !env[0]))
            return *own;
        return env ? env : "";
    }

    bool BootConfig::overEnvironment(const std::string &name) const
    {
        const auto it = m_values.find(name);
        return it != m_values.end() && it->second.overEnvironment;
    }

    const std::vector<std::string> &BootConfig::names() { return kNames; }

    bool BootConfig::carries(const std::string &name)
    {
        return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
    }

    namespace boot
    {
        void begin(BootConfig config)
        {
            // A session is one boot: what was made between two (the menu's
            // time) is not the new game's.
            session::end();
            Current &c = current();
            std::lock_guard<std::mutex> lock(c.mutex);
            c.game = config.game;
            c.values.clear();
            for (const std::string &name : BootConfig::names())
                if (const std::string *value = config.find(name))
                    c.put(name, *value, config.overEnvironment(name));
            c.generation.fetch_add(1u, std::memory_order_acq_rel);
        }

        // The game overrides' state of the boot goes with it (begin).
        void end() { begin(BootConfig{}); }

        const char *text(const char *name)
        {
            const char *env = std::getenv(name);
            Current &c = current();
            std::lock_guard<std::mutex> lock(c.mutex);
            const auto it = c.values.find(std::string_view(name));
            if (it == c.values.end())
                return env;
            if (it->second.overEnvironment || !env || !env[0])
                return it->second.text;
            return env;
        }

        bool has(const char *name)
        {
            const char *value = text(name);
            return value && value[0];
        }

        bool change(const std::string &name, std::string value)
        {
            if (!BootConfig::carries(name))
                return false;
            Current &c = current();
            std::lock_guard<std::mutex> lock(c.mutex);
            c.put(name, value, true);
            return true;
        }

        std::string game()
        {
            Current &c = current();
            std::lock_guard<std::mutex> lock(c.mutex);
            return c.game;
        }

        uint64_t generation() { return current().generation.load(std::memory_order_acquire); }
    }
}
