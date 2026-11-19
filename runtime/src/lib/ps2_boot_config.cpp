#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <unordered_set>

namespace ps2x
{
    namespace
    {
        // The names a BootConfig carries: what the app passes for each game,
        // and the check switches a run gives each game it boots.
        const std::vector<std::string> kNames = {
            // The app's renderer settings (read when a GS renderer is made).
            "PS2X_GS_BACKEND",
            "PS2X_GS_SCALE",
            "PS2X_GS_FULL_HEIGHT",
            "PS2X_RENDER_PATH",
            // A game's own renderer need (DM2's override: its mip levels in
            // a render target, from GS memory).
            "PS2X_GS_MIP_TARGET_LEVELS",
            // BMX XXX's override: its 16-bit Z buffers may be D16 targets.
            "PS2X_GS_D16_TARGETS",
            // The game's folders and files.
            "PS2X_CONTROLS_INI",
            "PS2X_GAMEPAD_MAPPINGS",
            "PS2X_GAMEPLAY_INI",
            "PS2X_SAVES_DIR",
            "PS2X_MODS_DIR",
            // Texture packs (runtime/ps2_texture_pack.h): the folders, and dump mode's.
            "PS2X_TEXTURE_PACKS",
            "PS2X_TEXTURE_DUMP",
            "PS2X_SCREENSHOT_DIR",
            "PS2X_GS_PIPELINE_CACHE",
            "PS2X_XBOX_DIR",
            "PS2X_XBOX_PACK",
            "PS2X_XBOX_CONVERTED",
            "PS2X_ESTATE_DIR",
            "PS2X_DM2_XBOX_LEVELS_DIR",
            "PS2X_BMX_RELEASES_DIR",
            // The game's settings.ini.
            "PS2X_SAVE_PROFILE",
            "PS2X_GAME_SETTINGS_MENUS",
            "PS2X_LANGUAGE",
            "PS2X_FRENCH_VOICES",
            "PS2X_DEV_MENU",
            "PS2X_ZOO",
            "PS2X_ZOO_UNLOCK",
            "PS2X_ESTATE",
            "PS2X_DM2_XBOX_LEVELS",
            "PS2X_TUNING",
            "PS2X_CAREER_SCRIPTS",
            "PS2X_DRAW_DISTANCE",
            "PS2X_XBOX_FREESKATE",
            "PS2X_APE",
            "PS2X_DM2_CREATE_PLAYER",
            "PS2X_DM2_XBOX_THEMES",
            "PS2X_DM2_RETRO_MOVIE",
            "PS2X_DM2_ATTRACT_DEMO",
            "PS2X_BMX_PARK_EDITOR",
            "PS2X_BMX_XBOX_CHALLENGES",
            "PS2X_BMX_EDITOR_PIECES",
            "PS2X_BMX_LEVEL_PREVIEWS",
            "PS2X_BMX_LOGO_MOVIE",
            "PS2X_BMX_INTRO_MOVIE",
            "PS2X_XBOX_TEXTURES",
            "PS2X_XBOX_MOVIES",
            "PS2X_XBOX_MODELS",
            // [host]: the disc device's host form or the PS2's (native | ps2).
            "ZAXIS_DISC",
            // Check switches, each boot its own: scripted input, the state
            // probe's dumps and log, the cards.
            "PS2X_PAD_SCRIPT",
            "PS2X_PAD_SCRIPT_CLOCK",
            "PS2X_PAD_STICK_CENTER",
            "PS2X_HOST_INPUT",
            "PS2X_HOST_INPUT_SCRIPT",
            "PS2X_POKE_SCRIPT",
            "PS2X_DUMP_WHEN",
            "PS2X_DUMP_PREFIX",
            "PS2X_DUMP_EXTENDED",
            "PS2X_DUMP_AT_FRAME",
            "PS2X_SHOT_AT",
            "PS2X_STATE_LOG",
            "PS2X_STATE_LOG_FILE",
            "PS2X_PAD_DUMP",
            "PS2X_PAD_LOG",
            "PS2X_SAVES",
            "PS2X_MC_FD_BASE0",
            "PS2X_MC_FIXED_TIME",
            "PS2X_DEV_UNLOCK_ALL",
            // Aggressive Inline's start straight into a free skate level
            // (aggressive_inline_devstart.cpp): developer runs, and the
            // collection's Custom Freeplay starting a game into its map.
            "PS2X_DEV_START_LEVEL",
            "PS2X_DEV_START_SKATER",
            "PS2X_DEV_START_SKATE",
            "PS2X_DEV_START_QUIT",
            // DM2's park read from a host file instead of the memory card
            // (dave_mirra_saves.cpp): the Custom Freeplay map's.
            "PS2X_DM2_PARK_FILE",
            // BMX XXX's start keeping the row's level goal counts for the
            // player's record (bmx_xxx_devstart.cpp): the Custom Freeplay map's.
            "PS2X_BMX_KEEP_LEVEL_GOALS",
            // DM2's level-point camera from the level's own camera points
            // (game/dm2/camera/camera.cpp levelCameraPoint): a Custom
            // Freeplay map's.
            "PS2X_DM2_LEVEL_CAMERA_POINTS",
            // BMX XXX's levels' own NPC riders left out (bmx_xxx_npcs.cpp):
            // a Custom Freeplay map that does not keep its slot's.
            "PS2X_BMX_LEVEL_NPCS",
            // The readable code without its proof scaffolding (ee/abi.h leanParts).
            "PS2X_READABLE_LEAN",
            // Play recordings (runtime/play_recording.h): the app's setting
            // records each boot; a replay reads one.
            "ZAXIS_RECORD",
            "ZAXIS_RECORD_CHECKSUMS",
            "ZAXIS_REPLAY",
            "ZAXIS_REPLAY_STOP",
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
