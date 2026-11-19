// The app's own files: where they live, its settings.ini (the app's rows and
// each game's options), the collection's files, and what a game's boot is
// given (collection/settings.h).
#include "collection/settings.h"
#include "collection/maps.h"
#include "collection/mods.h"
#include "strings.h"

#include "runtime/play_recording.h"
#include "runtime/ps2_game_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace ps2x::app
{
    std::string envString(const GetEnv &getEnv, const char *name)
    {
        const char *value = getEnv ? getEnv(name) : nullptr;
        return (value && value[0]) ? std::string(value) : std::string();
    }

    std::string trim(std::string_view text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    std::string lower(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    namespace
    {
        bool parseBool(const std::string &value, bool &out)
        {
            const std::string v = lower(value);
            if (v == "true" || v == "yes" || v == "on" || v == "1")
            {
                out = true;
                return true;
            }
            if (v == "false" || v == "no" || v == "off" || v == "0")
            {
                out = false;
                return true;
            }
            return false;
        }

        // One of the names (any case): its index.
        template <size_t N>
        bool parseName(const std::string &value, const char *const (&names)[N], int &out)
        {
            const std::string v = lower(value);
            for (size_t i = 0; i < N; ++i)
                if (v == names[i])
                {
                    out = static_cast<int>(i);
                    return true;
                }
            return false;
        }

        // A window size: at least `least` (the app keeps a window of at least
        // 320 x 240 and saves its size so: app.cpp), at most 16384.
        bool parseSize(const std::string &value, int &out, long least)
        {
            char *end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || parsed < least || parsed > 16384)
                return false;
            out = static_cast<int>(parsed);
            return true;
        }

        const char *boolText(bool value)
        {
            return value ? "true" : "false";
        }

        // A [graphics] key of a picture quality option (gs_quality), or -1.
        int qualityKey(const std::string &key)
        {
            for (const ps2x::gs_quality::OptionInfo &q : ps2x::gs_quality::options())
                if (key == q.iniKey)
                    return static_cast<int>(q.option);
            return -1;
        }
    }

    AppPaths resolveAppPaths(const std::string &appName, const GetEnv &getEnv)
    {
        AppPaths paths;
        const std::string overrideDir = envString(getEnv, "PS2X_APP_SUPPORT_DIR");
        const std::string home = envString(getEnv, "HOME");
        std::filesystem::path logDir;
        if (!overrideDir.empty())
        {
            paths.supportDir = overrideDir;
            logDir = paths.supportDir / "Logs";
        }
        else
        {
#if defined(_WIN32)
            paths.supportDir = std::filesystem::path(envString(getEnv, "APPDATA")) / appName;
            logDir = paths.supportDir / "Logs";
#elif defined(__APPLE__)
            paths.supportDir = std::filesystem::path(home) / "Library" / "Application Support" / appName;
            logDir = std::filesystem::path(home) / "Library" / "Logs" / appName;
#else
            const std::string xdg = envString(getEnv, "XDG_CONFIG_HOME");
            paths.supportDir = (xdg.empty() ? std::filesystem::path(home) / ".config" : std::filesystem::path(xdg)) / appName;
            const std::string state = envString(getEnv, "XDG_STATE_HOME");
            logDir = (state.empty() ? std::filesystem::path(home) / ".local" / "state" : std::filesystem::path(state)) / appName;
#endif
        }
        if (const std::string logOverride = envString(getEnv, "PS2X_APP_LOG_DIR"); !logOverride.empty())
            logDir = logOverride;

        paths.settingsFile = paths.supportDir / "settings.ini";
        paths.controlsFile = paths.supportDir / "controls.ini";
        paths.memcardSlot1 = paths.supportDir / "memcards" / "slot1";
        paths.memcardSlot2 = paths.supportDir / "memcards" / "slot2";
        paths.savesDir = paths.supportDir / "Saves";
        paths.hostRoot = paths.supportDir / "host";
        paths.logDir = logDir;
        return paths;
    }

    Settings parseSettings(std::string_view text, std::vector<std::string> *warnings)
    {
        Settings settings;
        parseSettingsInto(text, settings, warnings);
        return settings;
    }

    void parseSettingsInto(std::string_view text, Settings &settings, std::vector<std::string> *warnings)
    {
        auto warn = [&](const std::string &message)
        {
            if (warnings)
                warnings->push_back(message);
        };

        std::string section;
        std::istringstream in{std::string(text)};
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line))
        {
            ++lineNumber;
            std::string content = trim(line);
            if (content.empty() || content[0] == '#' || content[0] == ';')
                continue;
            if (content.front() == '[' && content.back() == ']')
            {
                section = lower(trim(std::string_view(content).substr(1, content.size() - 2)));
                continue;
            }
            const size_t equals = content.find('=');
            if (equals == std::string::npos)
            {
                warn("line " + std::to_string(lineNumber) + ": expected key = value");
                continue;
            }
            const std::string where = "line " + std::to_string(lineNumber) + ": ";
            if (section == "mod order")
            {
                // N = <order key>: the mods by place, first read first.
                const std::string number = trim(std::string_view(content).substr(0, equals));
                const std::string key = trim(std::string_view(content).substr(equals + 1));
                if (key.empty() || number.empty() || !std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c); }))
                    warn(where + "expected <number> = <mod> in [mod order]");
                else if (std::find(settings.modOrder.begin(), settings.modOrder.end(), key) == settings.modOrder.end())
                    settings.modOrder.push_back(key);
                continue;
            }
            if (section == "mods")
            {
                // A mod by its folder's name as it is (the case kept; a name
                // with '=' in it ends at the last one): on | off.
                const size_t last = content.rfind('=');
                const std::string name = trim(std::string_view(content).substr(0, last));
                const std::string value = trim(std::string_view(content).substr(last + 1));
                bool on = true;
                if (name.empty() || !parseBool(value, on))
                    warn(where + "bad value for mod " + name + ": " + value);
                else
                    setModOn(settings, name, on);
                continue;
            }
            const std::string key = lower(trim(std::string_view(content).substr(0, equals)));
            const std::string value = trim(std::string_view(content).substr(equals + 1));
            bool ok = true;
            if (section == "disc") // earlier versions: the disc image
                continue;
            else if (section == "display" && key == "fullscreen")
                ok = parseBool(value, settings.fullscreen);
            else if (section == "display" && key == "window_width")
                ok = parseSize(value, settings.windowWidth, 320);
            else if (section == "display" && key == "window_height")
                ok = parseSize(value, settings.windowHeight, 240);
            else if (section == "display" && key == "scaling")
            {
                const std::string v = lower(value);
                if (v == "smooth")
                    settings.scaling = Scaling::Smooth;
                else if (v == "integer")
                    settings.scaling = Scaling::Integer;
                else
                    ok = false;
            }
            else if (section == "display" && key == "widescreen")
                ok = parseBool(value, settings.widescreen);
            else if (section == "display" && key == "aspect")
                ok = parseName(value, kWideAspects, settings.wideAspect);
            else if (section == "graphics" && key == "full_height")
                ok = parseBool(value, settings.fullHeight);
            else if (section == "graphics" && key == "motion_blur")
                ok = parseName(value, kMotionBlurLevels, settings.motionBlur);
            else if (section == "graphics" && key == "draw_distance") // (an earlier version's, app-wide)
                ok = parseName(value, kDrawDistances, settings.legacyDrawDistance);
            else if (section == "gameplay" && key == "draw_distance")
                ok = parseName(value, kDrawDistances, settings.drawDistance);
            else if (section == "display" && key == "fullscreen_mode")
            {
                const std::string v = lower(value);
                if (v == "native")
                    settings.fullscreenMode = FullscreenMode::Native;
                else if (v == "borderless")
                    settings.fullscreenMode = FullscreenMode::Borderless;
                else
                    ok = false;
            }
            else if (section == "display" && key == "xbox_textures")
                ok = parseBool(value, settings.xboxTextures);
            else if (section == "display" && key == "xbox_movies")
                ok = parseBool(value, settings.xboxMovies);
            else if (section == "display" && key == "texture_dump")
                ok = parseBool(value, settings.textureDump);
            else if (section == "display" && key == "xbox_skaters")
                ok = parseBool(value, settings.xboxSkaters);
            else if (section == "display" && key == "vsync")
                ok = parseBool(value, settings.vsync);
            else if (section == "display" && key == "variable_refresh")
                ok = parseBool(value, settings.variableRefresh);
            else if (section == "display" && key == "frame_interpolation")
                ok = parseBool(value, settings.frameInterpolation);
            else if (section == "display" && key == "show_fps")
                ok = parseBool(value, settings.showFps);
            else if (section == "display" && key == "map_window")
                ok = parseBool(value, settings.mapWindow);
            else if (section == "display" && key == "map_window_x")
                ok = parseSize(value, settings.mapWindowX, -16384); // a second display can be to the left
            else if (section == "display" && key == "map_window_y")
                ok = parseSize(value, settings.mapWindowY, -16384);
            else if (section == "display" && key == "map_window_width")
                ok = parseSize(value, settings.mapWindowWidth, 160);
            else if (section == "display" && key == "map_window_height")
                ok = parseSize(value, settings.mapWindowHeight, 140);
            else if (section == "audio" && key == "volume")
            {
                char *end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > 100)
                    ok = false;
                else
                    settings.volume = static_cast<int>(parsed);
            }
            else if (section == "audio" && key == "movie_volume")
            {
                char *end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > 100)
                    ok = false;
                else
                    settings.movieVolume = static_cast<int>(parsed);
            }
            else if (section == "audio" && key == "mute_on_focus_loss")
                ok = parseBool(value, settings.muteOnFocusLoss);
            else if (section == "general" && key == "skip_intro_movies")
                ok = parseBool(value, settings.skipIntroMovies);
            else if (section == "general" && key == "language")
            {
                const std::string v = lower(value);
                if (v == "us" || v == "uk")
                    settings.language = 0;
                else
                    ok = parseName(value, kLanguages, settings.language);
            }
            else if (section == "general" && key == "french_voices")
            {
                bool on = false;
                if (lower(value) == "auto")
                    settings.frenchVoices = -1;
                else if ((ok = parseBool(value, on)))
                    settings.frenchVoices = on ? 1 : 0;
            }
            else if (section == "general" && key == "developer_menu")
                ok = parseBool(value, settings.developerMenu);
            else if (section == "gameplay" && key == "tuning")
                ok = parseName(value, kTunings, settings.tuning);
            else if (section == "gameplay" && key == "career_scripts")
                ok = parseName(value, kTunings, settings.careerScripts);
            else if (section == "extras" && key == "zoo")
                ok = parseBool(value, settings.zoo);
            else if (section == "extras" && key == "zoo_unlock")
                ok = parseName(value, kZooUnlocks, settings.zooUnlock);
            else if (section == "extras" && key == "estate")
                ok = parseBool(value, settings.estate);
            // The cross-game combos' settings, gone: an older settings.ini keeps them, unread.
            else if (section == "extras" && (key == "dm2_levels" || key == "dm2_rider" || key == "dm2_rider_split" ||
                                             key == "ai_levels"))
                ok = true;
            else if (section == "extras" && key == "dm2_xbox_levels")
                ok = parseBool(value, settings.dm2XboxLevels);
            else if (section == "extras" && key == "ape")
                ok = parseBool(value, settings.ape);
            else if (section == "extras" && key == "xbox_free_skate")
                ok = parseBool(value, settings.xboxFreeSkate);
            else if (section == "extras" && key == "create_player")
                ok = parseBool(value, settings.dm2CreatePlayer);
            else if (section == "extras" && key == "xbox_themes")
                ok = parseBool(value, settings.dm2XboxThemes);
            else if (section == "extras" && key == "retro_movie")
                ok = parseBool(value, settings.dm2RetroMovie);
            else if (section == "extras" && key == "attract_demo")
                ok = parseBool(value, settings.dm2AttractDemo);
            else if (section == "extras" && key == "park_editor")
                ok = parseBool(value, settings.bmxParkEditor);
            else if (section == "extras" && key == "xbox_challenges")
                ok = parseBool(value, settings.bmxXboxChallenges);
            else if (section == "extras" && key == "editor_pieces")
                ok = parseBool(value, settings.bmxEditorPieces);
            else if (section == "extras" && key == "level_previews")
                ok = parseBool(value, settings.bmxLevelPreviews);
            else if (section == "extras" && key == "logo_movie")
                ok = parseBool(value, settings.bmxLogoMovie);
            else if (section == "extras" && key == "intro_movie")
                ok = parseBool(value, settings.bmxIntroMovie);
            else if (section == "extras" && key == "gc_levels")
                ok = parseBool(value, settings.bmxGcLevels);
            else if (section == "extras" && key == "menu_lines")
                ok = parseBool(value, settings.bmxMenuLines);
            else if (section == "extras" && key == "all_mp_modes")
                ok = parseBool(value, settings.bmxAllMpModes);
            else if (section == "graphics" && key == "renderer")
            {
                const std::string v = lower(value);
                if (v == "cpu")
                    settings.gpuRenderer = false;
                else if (v == "gpu")
                    settings.gpuRenderer = true;
                else
                    ok = false;
            }
            else if (section == "graphics" && key == "render_path")
            {
                const std::string v = lower(value);
                if (v == "native" || v == "records")
                {
                    settings.nativeMeshes = true;
                    settings.recordsMeshes = v == "records";
                }
                else if (v == "strict")
                    settings.nativeMeshes = false;
                else
                    ok = false;
            }
            else if (section == "host" && key == "disc")
            {
                const std::string v = lower(value);
                if (v == "native")
                    settings.hostDisc = true;
                else if (v == "ps2")
                    settings.hostDisc = false;
                else
                    ok = false;
            }
            else if (section == "graphics" && qualityKey(key) >= 0)
            {
                const int q = qualityKey(key);
                ok = ps2x::gs_quality::parseValue(static_cast<ps2x::gs_quality::Option>(q), value, settings.quality[q]);
            }
            else if (section == "graphics" && key == "internal_resolution")
            {
                char *end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end != '\0' || parsed < 1 || parsed > 4)
                    ok = false;
                else
                    settings.internalResolution = static_cast<int>(parsed);
            }
            else if (section == "general" && key == "pause_on_focus_loss")
                ok = parseBool(value, settings.pauseOnFocusLoss);
            else if (section == "general" && key == "log_file")
                ok = parseBool(value, settings.logFile);
            else if (section == "general" && key == "record_play")
                ok = parseBool(value, settings.recordPlay);
            else if (section == "general" && key == "custom_freeplay")
                ok = parseBool(value, settings.customFreeplay);
            else if (section == "menu" && key == "intro_movie")
            {
                ok = value == "logo" || value == "off";
                if (ok)
                    settings.introLogo = value == "logo";
            }
            else if (section == "menu" && key == "menu_music")
                ok = parseBool(value, settings.menuMusic);
            else if (section == "menu" && key == "intro_turn")
            {
                char *end = nullptr;
                const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
                ok = end != value.c_str() && *end == '\0' && parsed <= 0xFFFFFFFFul;
                if (ok)
                    settings.introTurn = static_cast<uint32_t>(parsed);
            }
            else if (section == "saves" && key == "profile")
            {
                if (value.empty())
                    ok = false;
                else
                    settings.saveProfile = value;
            }
            else if (section == "memcard" && key == "pcsx2_import_offered")
                ok = parseBool(value, settings.pcsx2ImportOffered);
            else
            {
                warn(where + "unknown setting [" + section + "] " + key);
                continue;
            }
            if (!ok)
                warn(where + "bad value for " + key + ": " + value);
        }
    }

    void setModOn(Settings &settings, const std::string &name, bool on)
    {
        std::vector<std::string> &off = settings.modsOff;
        const auto at = std::lower_bound(off.begin(), off.end(), name);
        const bool listed = at != off.end() && *at == name;
        if (!on && !listed)
            off.insert(at, name);
        else if (on && listed)
            off.erase(at);
    }

    // ---- each game's options --------------------------------------------------

    bool GameSettingsProfile::has(GameOption option) const
    {
        return std::find(options.begin(), options.end(), option) != options.end();
    }

    int drawDistanceOf(const Settings &settings, const GameSettingsProfile &game)
    {
        if (settings.drawDistance >= 0)
            return std::clamp(settings.drawDistance, 0, 2);
        if (settings.legacyDrawDistance >= 0 && settings.legacyDrawDistance != 1) // (Far: the earlier default, no pick)
            return std::clamp(settings.legacyDrawDistance, 0, 2);
        return game.drawDistanceChangesGame ? 0 : 1;
    }

    namespace
    {
        using O = GameOption;

        // A game (runtime/ps2_games.h) with its options.
        GameSettingsProfile profile(const char *id, std::vector<GameOption> options)
        {
            return {ps2x::games::game(id), std::move(options)};
        }

        // Each game's options (in the collection's order).
        const std::vector<GameSettingsProfile> &profiles()
        {
            static const std::vector<GameSettingsProfile> all = {
                // Aggressive Inline: the baseline, with what its Xbox and
                // GameCube releases add.
                profile("ai", {O::Language, O::FrenchVoices, O::DeveloperMenu, O::Tuning, O::CareerScripts, O::Zoo,
                               O::ZooUnlock, O::Estate, O::Ape, O::XboxFreeSkate, O::SaveProfile, O::Pcsx2Import,
                               O::GameplayPages, O::SkipIntroMovies, O::Widescreen, O::MotionBlur, O::DrawDistance,
                               O::MovieVolume}),
                // Dave Mirra Freestyle BMX 2: its Xbox release's two levels
                // and their career (game_overrides/dave_mirra_xboxlevel.cpp,
                // dave_mirra_xcareer.cpp), the intro movies' skip and the
                // movie volume (dave_mirra_movies.cpp), widescreen and draw
                // distance (dave_mirra_view.cpp), the gameplay pages
                // (dave_mirra_gameplay.cpp), save profiles and the PCSX2
                // save import (dave_mirra_saves.cpp), the developer menu
                // (dave_mirra_devtools.cpp: the game's debug display), Create
                // Player (dave_mirra_creator.cpp: only with the readable
                // newRider running, which makes a created rider's model); what
                // its Xbox release adds: the park editor's Desert and Highway
                // (dave_mirra_xboxthemes.cpp), the gallery's Retro movie
                // (dave_mirra_gallery.cpp), the attract demo (dave_mirra_demo.cpp).
                profile("dm2", {O::Dm2XboxLevels, O::Dm2CreatePlayer, O::Dm2XboxThemes, O::Dm2RetroMovie, O::Dm2AttractDemo,
                                O::DeveloperMenu, O::SaveProfile, O::Pcsx2Import,
                                O::GameplayPages, O::SkipIntroMovies, O::Widescreen, O::DrawDistance, O::MovieVolume}),
                // BMX XXX: the movie volume (its movies on the host,
                // bmx_xxx_native.cpp), the gameplay pages (bmx_xxx_gameplay.cpp), the
                // developer menu (bmx_xxx_devtools.cpp: its debug display),
                // widescreen, motion blur and draw distance (bmx_xxx_view.cpp),
                // save profiles and the PCSX2 save import (bmx_xxx_saves.cpp),
                // the park editor in the main menu (bmx_xxx_exit.cpp); what its
                // Xbox and GameCube releases add (bmx_xxx_releases.cpp): the
                // fuller French, German and Spanish, their challenge scripts
                // (with Syracuse's timers by the tuning), the level select's
                // preview movies, the Z-Axis logo movie in the gallery, the
                // intro movie at boot (and so the intro movies' skip), the
                // GameCube's levels; the menus' left-out lines (bmx_xxx_exit.cpp);
                // every multiplayer mode in SELECT GAME (bmx_xxx_modes.cpp).
                profile("bmx", {O::Language, O::DeveloperMenu, O::Tuning, O::BmxParkEditor, O::BmxXboxChallenges,
                                O::BmxEditorPieces, O::BmxLevelPreviews, O::BmxLogoMovie, O::BmxIntroMovie,
                                O::BmxGcLevels, O::BmxMenuLines, O::BmxAllMpModes, O::SaveProfile,
                                O::Pcsx2Import, O::GameplayPages, O::SkipIntroMovies, O::Widescreen, O::MotionBlur,
                                O::DrawDistance, O::MovieVolume}),
            };
            return all;
        }

        GameSettingsProfile makeEveryOption()
        {
            // (every game's draw distance changes the game: ps2_games.cpp)
            GameSettingsProfile every{{"", "", "Aggressive Inline", true}, {}};
            for (int i = 0; i < static_cast<int>(GameOption::Count); ++i)
                every.options.push_back(static_cast<GameOption>(i));
            return every;
        }
    }

    const GameSettingsProfile &gameSettingsProfile(std::string_view idOrElf)
    {
        const std::string_view id = ps2x::games::game(idOrElf).id;
        for (const GameSettingsProfile &game : profiles())
            if (id == game.id)
                return game;
        return profiles().front();
    }

    const GameSettingsProfile &everyOptionProfile()
    {
        static const GameSettingsProfile every = makeEveryOption();
        return every;
    }

    bool appWideKey(std::string_view section, std::string_view key)
    {
        if (section == "display" || section == "graphics" || section == "audio" || section == "host")
            return true;
        if (section == "mod order" || section == "menu")
            return true;
        return section == "general" && (key == "pause_on_focus_loss" || key == "skip_intro_movies" || key == "log_file" ||
                                        key == "record_play" || key == "custom_freeplay");
    }

    std::vector<std::string> settingsKeys(std::string_view text)
    {
        std::vector<std::string> keys;
        std::string section;
        std::istringstream in{std::string(text)};
        std::string line;
        while (std::getline(in, line))
        {
            const std::string content = trim(line);
            if (content.empty() || content[0] == '#' || content[0] == ';')
                continue;
            if (content.front() == '[' && content.back() == ']')
            {
                section = lower(trim(std::string_view(content).substr(1, content.size() - 2)));
                continue;
            }
            const size_t equals = content.find('=');
            if (equals != std::string::npos)
                keys.push_back(section + "." + lower(trim(std::string_view(content).substr(0, equals))));
        }
        return keys;
    }

    std::string formatSettings(const Settings &settings)
    {
        return formatSettings(settings, everyOptionProfile(), SettingsScope::All);
    }

    std::string formatSettings(const Settings &settings, const GameSettingsProfile &game, SettingsScope scope)
    {
        const bool app = scope != SettingsScope::Game;  // the app's own keys
        const bool own = scope != SettingsScope::App;  // the game's options
        // An app-wide value only some games act on: in the collection's
        // root always (every game shares it), in a single game's file if
        // the game has it.
        auto shared = [&](GameOption option) { return scope == SettingsScope::App || (app && game.has(option)); };
        auto option = [&](GameOption o) { return own && game.has(o); };

        std::ostringstream out;
        if (scope == SettingsScope::App)
            out << "# The collection's settings, the same in every game: display, graphics, sound.\n"
                << "# Each game's own options are in games/<game>/settings.ini, its keys and\n"
                << "# buttons in games/<game>/controls.ini. Edit while no game is running.\n";
        else if (scope == SettingsScope::Game)
            out << "# " << game.title << " settings: the game's own options. The display, graphics\n"
                << "# and sound are the collection's (settings.ini two folders up), the keys and\n"
                << "# buttons in controls.ini here. Edit while the game is not running.\n";
        else
            out << "# " << game.title << " settings. Edit while the game is not running.\n";

        if (app)
        {
            out << "\n[display]\n"
                << "fullscreen = " << boolText(settings.fullscreen) << "\n"
                << "# native: the system's full screen (its own Space on macOS) | borderless: a window covering the display\n"
                << "fullscreen_mode = " << (settings.fullscreenMode == FullscreenMode::Borderless ? "borderless" : "native") << "\n"
                << "window_width = " << settings.windowWidth << "\n"
                << "window_height = " << settings.windowHeight << "\n"
                << "# smooth: fill the window at 4:3, filtered | integer: whole multiples of 640x480, sharp pixels\n"
                << "scaling = " << (settings.scaling == Scaling::Integer ? "integer" : "smooth") << "\n";
            if (shared(GameOption::Widescreen))
                out << "# 16:9: the game draws a wider view, stretched to 16:9 (movies and 2D screens stay 4:3).\n"
                    << "# Drawn only: the game plays as at 4:3, so what is only at the sides stays still.\n"
                    << "widescreen = " << boolText(settings.widescreen) << "\n"
                    << "# Widescreen shape: 16:9 | 16:10 | 21:9 | 32:9 | window (follows the window's shape).\n"
                    << "aspect = " << kWideAspects[std::clamp(settings.wideAspect, 0, 4)] << "\n";
            out << "# Wait for the display's refresh (no tearing). false: lower latency.\n"
                << "vsync = " << boolText(settings.vsync) << "\n"
                << "# A G-Sync, FreeSync or adaptive-sync display: each game frame is shown when it\n"
                << "# is ready and the display follows (false: at the display's fixed refresh).\n"
                << "variable_refresh = " << boolText(settings.variableRefresh) << "\n"
                << "# On a display faster than the game's 60 frames a second (120, 144 Hz, ProMotion):\n"
                << "# pictures between the game's frames at the display's rate, drawn by the GPU renderer\n"
                << "# (the game runs as ever; what it shows is a frame later, less one refresh).\n"
                << "frame_interpolation = " << boolText(settings.frameInterpolation) << "\n"
                << "# Frame rate counter in the top right corner.\n"
                << "show_fps = " << boolText(settings.showFps) << "\n"
                << "# The bottom screen (the level's map, goals and tricks) in a second window, where it\n"
                << "# was last (-1: where the system puts it) and its size.\n"
                << "map_window = " << boolText(settings.mapWindow) << "\n"
                << "map_window_x = " << settings.mapWindowX << "\n"
                << "map_window_y = " << settings.mapWindowY << "\n"
                << "map_window_width = " << settings.mapWindowWidth << "\n"
                << "map_window_height = " << settings.mapWindowHeight << "\n"
                << "# When the app was built with your Xbox disc (see README): the Xbox release's\n"
                << "# sharper textures (GPU renderer) and its 640x480 movies in place of the PS2 ones.\n"
                << "xbox_textures = " << boolText(settings.xboxTextures) << "\n"
                << "xbox_movies = " << boolText(settings.xboxMovies) << "\n"
                << "# The Xbox release's more detailed characters (from the next level load).\n"
                << "xbox_skaters = " << boolText(settings.xboxSkaters) << "\n"
                << "# Texture packs: every texture the game draws written once to the Texture Dumps\n"
                << "# folder beside this file, named as a pack names it (from the next start).\n"
                << "texture_dump = " << boolText(settings.textureDump) << "\n"
                << "\n[graphics]\n"
#if defined(_WIN32)
                << "# cpu: the accurate software renderer. gpu: the hardware renderer (Direct3D 12),\n"
                << "# sharper at a higher internal resolution, less exact.\n"
#else
                << "# cpu: the accurate software renderer. gpu: the hardware renderer (Metal), sharper\n"
                << "# at a higher internal resolution, less exact.\n"
#endif
                << "renderer = " << (settings.gpuRenderer ? "gpu" : "cpu") << "\n"
                << "# GPU renderer: 1 (native 640x448) to 4 times the resolution.\n"
                << "internal_resolution = " << settings.internalResolution << "\n"
                << "# GPU renderer: the game draws 224-line frames (shown line-doubled); true renders\n"
                << "# them at twice the vertical resolution, as sharp as horizontally.\n"
                << "full_height = " << boolText(settings.fullHeight) << "\n"
                << "# GPU renderer: native draws level meshes from GPU buffers; strict sends\n"
                << "# them through the PS2's VIF, VU1 and GS as on the console; records\n"
                << "# draws them natively from the engine's draw records (no VU1 packets).\n"
                << "render_path = " << (!settings.nativeMeshes ? "strict" : settings.recordsMeshes ? "records" : "native")
                << "\n";
            if (shared(GameOption::MotionBlur))
                out << "# The game's blend of the previous frame in levels: off | low | original.\n"
                    << "motion_blur = " << kMotionBlurLevels[std::clamp(settings.motionBlur, 0, 2)] << "\n";
            out << "# GPU renderer picture quality, beyond the PS2 (native render path):\n"
                << "# anti_aliasing off | fxaa; texture_filtering off | 2x | 4x | 8x | 16x (anisotropic);\n"
                << "# fog original (eight bands) | smooth; shadows original | sharp; depth_precision\n"
                << "# original (16-bit) | high; perspective_correct false | true (shading, character textures).\n";
            for (const ps2x::gs_quality::OptionInfo &q : ps2x::gs_quality::options())
            {
                const int v = settings.quality[static_cast<size_t>(q.option)];
                out << q.iniKey << " = "
                    << (q.choices.size() == 2u && q.choices[1] == "on" ? boolText(v != 0) : ps2x::gs_quality::valueName(q.option, v))
                    << "\n";
            }
            out << "\n[audio]\n"
                << "# Master volume in percent (0-100).\n"
                << "volume = " << settings.volume << "\n"
                << "# Silent while the window is in the background.\n"
                << "mute_on_focus_loss = " << boolText(settings.muteOnFocusLoss) << "\n";
            if (shared(GameOption::MovieVolume))
                out << "# Movie sound in percent (0-100). Music, effects and speech are the game's\n"
                    << "# own volumes (Options, Sound), kept in its save.\n"
                    << "movie_volume = " << settings.movieVolume << "\n";
            out << "\n[host]\n"
                << "# native: the game's files read straight from the host, the Mods folder over them.\n"
                << "# ps2: the game's own CD/DVD client over the emulated IOP, as on the console.\n"
                << "disc = " << (settings.hostDisc ? "native" : "ps2") << "\n";
        }

        const bool gameGeneral = option(GameOption::Language) || option(GameOption::FrenchVoices) ||
                                 option(GameOption::DeveloperMenu);
        if (app || gameGeneral)
            out << "\n[general]\n";
        if (app)
        {
            out << "# Stop the game while its window is in the background.\n"
                << "pause_on_focus_loss = " << boolText(settings.pauseOnFocusLoss) << "\n";
            if (shared(GameOption::SkipIntroMovies))
                out << "# Start at the title screen, without the logo and intro movies.\n"
                    << "skip_intro_movies = " << boolText(settings.skipIntroMovies) << "\n";
        }
        const bool bmx = std::string_view(game.id) == "bmx";
        if (option(GameOption::Language) && bmx)
            out << "# english | french | german | spanish: the game's own, its French, German and\n"
                << "# Spanish with what the GameCube and Xbox releases translate further, when the\n"
                << "# app was built with those discs (tools/make-app.sh). Applies at the next start.\n";
        else if (option(GameOption::Language))
            out << "# english | french | german | spanish. The other languages are the Xbox\n"
                << "# release's text and fonts, so they need the Xbox files bundled at build time\n"
                << "# (tools/make-app.sh XBOX_ISO). Applies at the next start.\n";
        if (option(GameOption::Language))
            out << "language = " << kLanguages[std::clamp(settings.language, 0, 3)] << "\n";
        if (option(GameOption::FrenchVoices))
            out << "# The Xbox release's French speech: auto (with French) | true | false.\n"
                << "french_voices = " << (settings.frenchVoices < 0 ? "auto" : boolText(settings.frenchVoices != 0)) << "\n";
        if (option(GameOption::DeveloperMenu) && option(GameOption::Tuning))
            out << "# Developers and modders: F1 in a level opens the game's hidden debug menu\n"
                << "# (tuning table editor, free camera, respawn, debug text), and Mods gets a\n"
                << "# tuning-reference.ini. Applies at the next start.\n";
        else if (option(GameOption::DeveloperMenu))
            out << "# Developers and modders: F1 turns the game's debug display on and off (its\n"
                << "# frame rate, profiler and debug drawing). Applies at the next start.\n";
        if (option(GameOption::DeveloperMenu))
            out << "developer_menu = " << boolText(settings.developerMenu) << "\n";
        if (app)
            out << "# Write a log to the Logs folder (see README).\n"
                << "log_file = " << boolText(settings.logFile) << "\n"
                << "# Record each game's play (inputs and frame times) to its Recordings folder,\n"
                << "# to hand over for a replay. Applies at the next start.\n"
                << "record_play = " << boolText(settings.recordPlay) << "\n"
                << "# Custom Freeplay: a fourth tile on the menu, a player from any game on a custom\n"
                << "# map installed in a Mods folder (a mod folder with a map.ini). Off: the menu as\n"
                << "# it was, and no game is given a map.\n"
                << "custom_freeplay = " << boolText(settings.customFreeplay) << "\n";

        if (option(GameOption::Tuning) || option(GameOption::CareerScripts) || option(GameOption::DrawDistance))
            out << "\n[gameplay]\n";
        // (every game's draw distance changes the game: ps2_games.h drawDistanceChangesGame)
        if (option(GameOption::DrawDistance))
            out << "# How far the view reaches before the fog: original | far | farthest. Beyond\n"
                << "# original it changes the game (the cameras' far clip is the game's own state" << (bmx ? ",\n# and the models' levels of detail with it" : "")
                << "). Applies at the next start.\n"
                << "draw_distance = " << kDrawDistances[drawDistanceOf(settings, game)] << "\n";
        if (option(GameOption::Tuning) && bmx)
            out << "# ps2 | xbox: with the Xbox challenge scripts (xbox_challenges), Syracuse's two\n"
                << "# timed challenges at the Xbox and GameCube releases' 60 seconds (the PS2's: 45).\n"
                << "# Applies at the next start.\n";
        else if (option(GameOption::Tuning))
            out << "# ps2 | xbox: the Xbox release's balance (special tricks give 300,000 experience,\n"
                << "# key loot none; Civic Center's 350,000 point trick passes at 350,000). Applies\n"
                << "# at the next start.\n";
        if (option(GameOption::Tuning))
            out << "tuning = " << kTunings[std::clamp(settings.tuning, 0, 1)] << "\n";
        if (option(GameOption::CareerScripts))
            out << "# xbox | ps2: the career's level scripts, the Xbox release's with its bug fixes\n"
                << "# (challenges that could stick, wrong previews, texts) or the PS2's. A level a\n"
                << "# skater has already played keeps the script its progress was saved with.\n"
                << "career_scripts = " << kTunings[std::clamp(settings.careerScripts, 0, 1)] << "\n";

        if (option(GameOption::Zoo) || option(GameOption::ZooUnlock) || option(GameOption::Estate) ||
            option(GameOption::Dm2XboxLevels) || option(GameOption::Dm2CreatePlayer) || option(GameOption::Dm2XboxThemes) ||
            option(GameOption::Dm2RetroMovie) || option(GameOption::Dm2AttractDemo) || option(GameOption::Ape) || option(GameOption::XboxFreeSkate) ||
            option(GameOption::BmxParkEditor) || option(GameOption::BmxXboxChallenges) || option(GameOption::BmxEditorPieces) ||
            option(GameOption::BmxLevelPreviews) || option(GameOption::BmxLogoMovie) || option(GameOption::BmxIntroMovie) ||
            option(GameOption::BmxGcLevels) || option(GameOption::BmxMenuLines) ||
            option(GameOption::BmxAllMpModes))
            out << "\n[extras]\n";
        if (option(GameOption::Zoo))
            out << "# The Zoo, the level cut from the PS2 release, in Free Skate and Timed Run: the\n"
                << "# Xbox release's when the app was built with its disc image, else the PS2\n"
                << "# disc's unfinished one. Applies at the next start.\n"
                << "zoo = " << boolText(settings.zoo) << "\n";
        if (option(GameOption::ZooUnlock))
            out << "# museum | always: the Zoo opens for a skater with the Museum's eighth challenge\n"
                << "# (the Xbox release's rule; the cheats open it too), or is always open.\n"
                << "zoo_unlock = " << kZooUnlocks[std::clamp(settings.zooUnlock, 0, 1)] << "\n";
        if (option(GameOption::Estate))
            out << "# TUT/Estate, an earlier Tutorial left on the GameCube disc, as its own level in\n"
                << "# Free Skate, when the app was built with that disc's image. Applies at the next start.\n"
                << "estate = " << boolText(settings.estate) << "\n";
        if (option(GameOption::Dm2XboxLevels))
            out << "# The Xbox release's Venice, Italy and Greenville, NC: in Free Ride's level select\n"
                << "# and in ProQuest with their goals, gaps and rewards, when the app was built with\n"
                << "# the game's Xbox disc image. Applies at the next start.\n"
                << "dm2_xbox_levels = " << boolText(settings.dm2XboxLevels) << "\n";
        if (option(GameOption::Dm2CreatePlayer))
            out << "# Create Player, the menu the game has the code for but never shows: a rider\n"
                << "# made from the riders' heads, bodies and legs and their looks, in four slots,\n"
                << "# ridden in Free Ride and kept in the game save. Applies at the next start.\n"
                << "create_player = " << boolText(settings.dm2CreatePlayer) << "\n";
        if (option(GameOption::Dm2XboxThemes))
            out << "# The park editor's Desert and Highway, two themes on the Xbox disc that no\n"
                << "# release lists, after the nine others, lit as the Beach theme. Needs that disc\n"
                << "# at build time; applies at the next start.\n"
                << "xbox_themes = " << boolText(settings.dm2XboxThemes) << "\n";
        if (option(GameOption::Dm2RetroMovie))
            out << "# The Retro movie, the Xbox release's, as the movie gallery's last line. Needs that\n"
                << "# disc at build time; applies at the next start.\n"
                << "retro_movie = " << boolText(settings.dm2RetroMovie) << "\n";
        if (option(GameOption::Dm2AttractDemo))
            out << "# The attract demo, as on the Xbox and GameCube: left at the title screen, the game\n"
                << "# plays one of its own replays; a button goes back. Applies at the next start.\n"
                << "attract_demo = " << boolText(settings.dm2AttractDemo) << "\n";
        if (option(GameOption::Ape))
            out << "# The Ape, the Xbox release's secret skater (unlocked with the Mummy), when the\n"
                << "# app was built with the Xbox disc image. Applies at the next start.\n"
                << "ape = " << boolText(settings.ape) << "\n";
        if (option(GameOption::XboxFreeSkate))
            out << "# The Boardwalk's and the Cannery's free skate as on the Xbox (their doors open\n"
                << "# as in the career, coaster cars, the cutter), when the app was built with the\n"
                << "# Xbox disc image. Applies at the next start.\n"
                << "xbox_free_skate = " << boolText(settings.xboxFreeSkate) << "\n";
        if (option(GameOption::BmxParkEditor))
            out << "# The game's own park editor, which no menu of the retail game leads to, as the\n"
                << "# main menu's PARK EDITOR: a new park built on one of its four themes, a saved\n"
                << "# park edited again, or ridden. Applies at the next start.\n"
                << "park_editor = " << boolText(settings.bmxParkEditor) << "\n";
        if (option(GameOption::BmxXboxChallenges))
            out << "# Las Vegas's, NASA's, Sheep Hills' and Syracuse's challenge scripts as the Xbox\n"
                << "# and GameCube releases have them: the challenges' success and failure lines\n"
                << "# (the PS2's speech), Sheep Hills' pickup tethers, two steps fixed. A timer's\n"
                << "# length follows tuning. Needs those discs at build time; applies at the next start.\n"
                << "xbox_challenges = " << boolText(settings.bmxXboxChallenges) << "\n";
        if (option(GameOption::BmxEditorPieces))
            out << "# The park editor's Pool (in BOWLS) and Receptacle (a dynamic trash can), the\n"
                << "# GameCube and Xbox releases' pieces. A park saved with them turns them on by itself.\n"
                << "# Needs those discs at build time; applies at the next start.\n"
                << "editor_pieces = " << boolText(settings.bmxEditorPieces) << "\n";
        if (option(GameOption::BmxLevelPreviews))
            out << "# The level select's preview movies, the Xbox release's. Needs its disc at build\n"
                << "# time; applies at the next start.\n"
                << "level_previews = " << boolText(settings.bmxLevelPreviews) << "\n";
        if (option(GameOption::BmxLogoMovie))
            out << "# The Z-Axis logo movie, which its reward text sends the player to, in the movie\n"
                << "# gallery (the Xbox release's movie). Needs its disc at build time; applies at the next start.\n"
                << "logo_movie = " << boolText(settings.bmxLogoMovie) << "\n";
        if (option(GameOption::BmxIntroMovie))
            out << "# The intro movie at boot, as the GameCube release plays it (Skip Intro Movies\n"
                << "# still skips it). Applies at the next start.\n"
                << "intro_movie = " << boolText(settings.bmxIntroMovie) << "\n";
        if (option(GameOption::BmxGcLevels))
            out << "# The Secret Level and the flat, grind and vert test levels, whose files only the\n"
                << "# GameCube disc has, as the level select's last four. Needs that disc at build\n"
                << "# time; applies at the next start.\n"
                << "gc_levels = " << boolText(settings.bmxGcLevels) << "\n";
        if (option(GameOption::BmxMenuLines))
            out << "# The menu lines the game has but never puts up: TRICK LIST, CHANGE RIDER and\n"
                << "# LEVEL SELECT in the pause menu, CHANGE RIDER, VIEW REPLAY and TRICK LIST at the\n"
                << "# game over, and the main menu's PARK EDITOR once the BULLETPOINT cheat is\n"
                << "# entered (the PS2's feature 5, THE PARK EDITOR). Applies at the next start.\n"
                << "menu_lines = " << boolText(settings.bmxMenuLines) << "\n";
        if (option(GameOption::BmxAllMpModes))
            out << "# Every multiplayer mode whose rules work in SELECT GAME, not the game's three: HIGH\n"
                << "# FIVE, RANDOM SPOT, WIPEOUT and the eight three-chance contests besides SKILLZ,\n"
                << "# PAINTBALL and STRIP CHALLENGE. Applies at the next start.\n"
                << "all_mp_modes = " << boolText(settings.bmxAllMpModes) << "\n";
        if (option(GameOption::SaveProfile))
            out << "\n[saves]\n"
                << "# Saves/<profile>.sav in this folder; another name starts (or picks) another career.\n"
                << "profile = " << settings.saveProfile << "\n";
        if (option(GameOption::Pcsx2Import))
            out << "\n[memcard]\n"
                << "# Set to false to be offered a PCSX2 save import again.\n"
                << "pcsx2_import_offered = " << boolText(settings.pcsx2ImportOffered) << "\n";
        if (scope == SettingsScope::App)
            out << "\n[menu]\n"
                << "# What plays at launch: logo (the Z-Axis logo from a game's own intro, each game's\n"
                << "# in turn) | off.\n"
                << "intro_movie = " << (settings.introLogo ? "logo" : "off") << "\n"
                << "# The chosen game's menu music under the menu.\n"
                << "menu_music = " << boolText(settings.menuMusic) << "\n"
                << "# Whose logo plays next (counts up at each launch).\n"
                << "intro_turn = " << settings.introTurn << "\n";
        if (scope == SettingsScope::App && !settings.modOrder.empty())
        {
            out << "\n[mod order]\n"
                << "# The order the games read the mods in (Settings > Mods), first read first: a later\n"
                << "# mod's file over an earlier one's. Mods not listed come after, by name.\n";
            for (size_t i = 0; i < settings.modOrder.size(); ++i)
                out << (i + 1) << " = " << settings.modOrder[i] << "\n";
        }
        if (own)
        {
            out << "\n[mods]\n"
                << "# The folders in the Mods folder here are mods, read in name order (a later one's\n"
                << "# files over an earlier one's). Each is on unless switched off here, by its\n"
                << "# folder's name: <name> = off (in the collection, also its menu's Settings > Mods,\n"
                << "# which switches the collection's Mods folder's mods here too, by their ids).\n"
                << "# Applies at the next start.\n";
            for (const std::string &name : settings.modsOff)
                out << name << " = off\n";
        }
        if (scope == SettingsScope::Game && std::none_of(game.options.begin(), game.options.end(), [](GameOption o)
                                                         { return o < GameOption::GameplayPages; })) // (the pages are gameplay.ini's)
            out << "\n# The game has no options of its own yet.\n";
        return out.str();
    }

    bool readTextFile(const std::filesystem::path &file, std::string &text)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
        return true;
    }

    bool writeTextFile(const std::filesystem::path &file, const std::string &text)
    {
        std::error_code ec;
        if (file.has_parent_path())
            std::filesystem::create_directories(file.parent_path(), ec);
        std::filesystem::path temp = file;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out << text;
            if (!out.good())
                return false;
        }
        std::filesystem::rename(temp, file, ec);
        return !ec;
    }

    bool loadSettings(const std::filesystem::path &file, Settings &settings, std::vector<std::string> *warnings)
    {
        std::string text;
        if (!readTextFile(file, text))
            return false;
        settings = parseSettings(text, warnings);
        return true;
    }

    bool saveSettings(const std::filesystem::path &file, const Settings &settings)
    {
        return writeTextFile(file, formatSettings(settings));
    }

    // ---- a collection's files -------------------------------------------------

    namespace
    {
        // file -> file.bak (an older .bak replaced).
        bool putAside(const std::filesystem::path &file, std::vector<std::string> *log)
        {
            std::error_code ec;
            std::filesystem::path aside = file;
            aside += ".bak";
            std::filesystem::remove(aside, ec);
            std::filesystem::rename(file, aside, ec);
            if (log)
                log->push_back(file.string() + " -> " + aside.filename().string() + (ec ? " failed: " + ec.message() : ""));
            return !ec;
        }
    }

    namespace
    {
        // The root's [graphics] draw_distance (an earlier version's, the
        // same for every game) to the games' own [gameplay] draw_distance:
        // to each game that has none, when it is not Far (the earlier
        // default, which the app wrote whether picked or not: a user who
        // picked Far picks it again in the game's settings). The root's line
        // goes (rootText too).
        void moveDrawDistance(const std::filesystem::path &root, std::string &rootText, const std::vector<std::string> &gameIds,
                              std::vector<std::string> *log)
        {
            const std::vector<std::string> keys = settingsKeys(rootText);
            if (std::find(keys.begin(), keys.end(), "graphics.draw_distance") == keys.end())
                return;
            const int picked = parseSettings(rootText).legacyDrawDistance;
            for (const std::string &id : gameIds)
            {
                const GameSettingsProfile &game = gameSettingsProfile(id);
                if (picked < 0 || picked == 1 || !game.has(GameOption::DrawDistance))
                    continue;
                const std::filesystem::path file = root / "games" / id / "settings.ini";
                std::string text;
                readTextFile(file, text);
                const std::vector<std::string> own = settingsKeys(text);
                if (std::find(own.begin(), own.end(), "gameplay.draw_distance") != own.end())
                    continue;
                text += std::string(text.empty() || text.back() == '\n' ? "" : "\n") + "\n[gameplay]\ndraw_distance = " +
                        kDrawDistances[std::clamp(picked, 0, 2)] + "\n";
                const bool ok = writeTextFile(file, text);
                if (log)
                    log->push_back(std::string("the draw distance ") + kDrawDistances[std::clamp(picked, 0, 2)] + " in " +
                                   (root / "settings.ini").string() + " -> " + file.string() + (ok ? "" : " failed"));
            }
            // The line itself and its comment (the comment lines right above
            // it, as formatSettings writes a key's), the rest of the file as
            // it was.
            std::istringstream in(rootText);
            std::vector<std::string> kept;
            std::string line, section;
            auto isComment = [](const std::string &text)
            {
                const std::string content = trim(text);
                return !content.empty() && (content[0] == '#' || content[0] == ';');
            };
            while (std::getline(in, line))
            {
                const std::string content = trim(line);
                if (!content.empty() && content.front() == '[' && content.back() == ']')
                    section = lower(trim(std::string_view(content).substr(1, content.size() - 2)));
                const size_t equals = content.find('=');
                if (section == "graphics" && equals != std::string::npos && content[0] != '#' && content[0] != ';' &&
                    lower(trim(std::string_view(content).substr(0, equals))) == "draw_distance")
                {
                    while (!kept.empty() && isComment(kept.back()))
                        kept.pop_back();
                    continue;
                }
                kept.push_back(line);
            }
            rootText.clear();
            for (const std::string &each : kept)
                rootText += each + "\n";
            const bool ok = writeTextFile(root / "settings.ini", rootText);
            if (log)
                log->push_back("the draw distance left " + (root / "settings.ini").string() + " (each game's own now)" +
                               (ok ? "" : ": failed"));
        }
    }

    void migrateCollectionFiles(const std::filesystem::path &root, const std::string &keeperId,
                                const std::vector<std::string> &gameIds, std::vector<std::string> *log)
    {
        std::error_code ec;
        const std::filesystem::path shared = root / "controls.ini";
        if (std::filesystem::is_regular_file(shared, ec))
        {
            for (const std::string &id : gameIds)
            {
                const std::filesystem::path own = root / "games" / id / "controls.ini";
                if (id != keeperId && std::filesystem::exists(own, ec))
                    putAside(own, log);
            }
            const bool haveKeeper = std::find(gameIds.begin(), gameIds.end(), keeperId) != gameIds.end();
            if (haveKeeper)
            {
                const std::filesystem::path own = root / "games" / keeperId / "controls.ini";
                if (std::filesystem::exists(own, ec))
                    putAside(own, log);
                std::filesystem::create_directories(own.parent_path(), ec);
                ec.clear();
                std::filesystem::rename(shared, own, ec);
                if (log)
                    log->push_back(shared.string() + " -> " + own.string() + (ec ? " failed: " + ec.message() : ""));
            }
            else
            {
                putAside(shared, log);
            }
        }

        const std::filesystem::path rootSettings = root / "settings.ini";
        std::string rootText;
        if (!readTextFile(rootSettings, rootText))
            return;
        moveDrawDistance(root, rootText, gameIds, log);
        bool gameKeys = false;
        for (const std::string &key : settingsKeys(rootText))
        {
            const size_t dot = key.find('.');
            if (!appWideKey(std::string_view(key).substr(0, dot), std::string_view(key).substr(dot + 1)))
                gameKeys = true;
        }
        if (!gameKeys)
            return;
        const Settings fromRoot = parseSettings(rootText);
        Settings keeper = fromRoot;
        const std::filesystem::path keeperFile = root / "games" / keeperId / "settings.ini";
        std::string keeperText;
        if (readTextFile(keeperFile, keeperText))
            parseSettingsInto(keeperText, keeper);
        const GameSettingsProfile &profile = gameSettingsProfile(keeperId);
        const bool keeperOk = writeTextFile(keeperFile, formatSettings(keeper, profile, SettingsScope::Game));
        const bool rootOk = keeperOk && writeTextFile(rootSettings, formatSettings(fromRoot, profile, SettingsScope::App));
        if (log)
            log->push_back("the game options in " + rootSettings.string() + " -> " + keeperFile.string() +
                           (keeperOk && rootOk ? "" : " failed"));
    }

    bool loadCollectionSettings(const std::filesystem::path &rootFile, const std::filesystem::path &gameFile,
                                Settings &settings, std::vector<std::string> *warnings)
    {
        std::string game, app;
        const bool haveGame = readTextFile(gameFile, game);
        const bool haveApp = readTextFile(rootFile, app);
        settings = Settings{};
        parseSettingsInto(game, settings, warnings);
        if (haveApp)
            parseSettingsInto(app, settings, warnings);
        return haveGame && haveApp;
    }

    bool saveCollectionSettings(const std::filesystem::path &rootFile, const std::filesystem::path &gameFile,
                                const Settings &settings, const GameSettingsProfile &game)
    {
        const bool gameOk = writeTextFile(gameFile, formatSettings(settings, game, SettingsScope::Game));
        const bool appOk = writeTextFile(rootFile, formatSettings(settings, game, SettingsScope::App));
        return gameOk && appOk;
    }

    std::vector<std::pair<std::string, std::string>> rendererValues(const Settings &settings)
    {
        return {{"PS2X_GS_BACKEND", settings.gpuRenderer ? "gpu" : "cpu"},
                {"PS2X_GS_SCALE", std::to_string(settings.internalResolution)},
                {"PS2X_GS_FULL_HEIGHT", settings.fullHeight ? "1" : "0"}};
    }

    namespace
    {
        // The boot's log of its mods: each read (or why not), and where two
        // have the same file, which one's the game reads.
        void logModsRead(const std::string &gameId, const std::vector<Mod> &mods)
        {
            if (mods.empty())
                return;
            for (const Mod &mod : mods)
                std::cout << "[mods] " << gameId << ": " << mod.orderKey() << (mod.version.empty() ? "" : " " + mod.version)
                          << (mod.read ? " read" : " not read: " + mod.why) << (mod.read && mod.changesLogic() ? " (changes game logic)" : "")
                          << std::endl;
            for (const std::filesystem::path &pack : texturePacks(mods, gameId))
                std::cout << "[mods] " << gameId << ": texture pack " << pack.string() << std::endl;
            const std::vector<ModWinners> winners = modWinners(mods, gameId);
            for (size_t i = 0; i < mods.size(); ++i)
                for (const ModOverlap &over : winners[i].winsOver)
                    std::cout << "[mods] " << gameId << ": " << mods[i].orderKey() << " over " << mods[over.mod].orderKey() << ": "
                              << over.count << (over.count == 1 ? " thing (" : " things (") << over.example
                              << (over.count > 1 ? ", ...)" : ")") << std::endl;
        }
    }

    namespace
    {
        // The other releases' content's folders, by the boot name each is read by.
        struct ReleaseFolder
        {
            const char *name;
            const char *folder;
        };
        constexpr ReleaseFolder kReleaseFolders[] = {
            {"PS2X_XBOX_DIR", "xbox"},                     // AI's Xbox files (runtime/ps2_xbox_disc.h)
            {"PS2X_XBOX_CONVERTED", "xbox-converted"},     // the Zoo and the Ape made from them
            {"PS2X_XBOX_PACK", "xbox-pack"},               // their textures, movies and skaters
            {"PS2X_ESTATE_DIR", "estate"},                 // AI's GameCube Estate
            {"PS2X_DM2_XBOX_LEVELS_DIR", "xbox-levels"},   // DM2's Xbox levels, themes and movie
            {"PS2X_BMX_RELEASES_DIR", "releases"},         // BMX XXX's Xbox and GameCube content
        };
    }

    std::vector<std::pair<std::string, std::filesystem::path>> releaseFolders(const BootFolders &folders)
    {
        std::vector<std::pair<std::string, std::filesystem::path>> out;
        std::error_code ec;
        for (const ReleaseFolder &release : kReleaseFolders)
            for (const std::filesystem::path &root : {folders.extras, folders.resources})
                if (!root.empty() && std::filesystem::is_directory(root / release.folder, ec))
                {
                    out.emplace_back(release.name, root / release.folder);
                    break;
                }
        return out;
    }

    ps2x::BootConfig gameBootConfig(const std::string &gameId, const Settings &settings, const AppPaths &paths,
                                    const GameSettingsProfile &profile, const BootFolders &folders,
                                    const GetEnv &getEnv)
    {
        ps2x::BootConfig config;
        config.game = gameId;
        auto has = [&profile](GameOption option) { return profile.has(option); };
        auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
        // An extra's folder (releaseFolders): the one the setup made from the
        // player's discs, else the one bundled with the app (a workshop build).
        const std::vector<std::pair<std::string, std::filesystem::path>> releases = releaseFolders(folders);
        auto bundled = [&](const char *name)
        {
            for (const auto &[each, dir] : releases)
                if (each == name)
                    config.set(name, dir.string());
        };

        if (!folders.gamepadMappings.empty())
            config.set("PS2X_GAMEPAD_MAPPINGS", folders.gamepadMappings);
        // The game's saves (native save files, see the game override).
        config.set("PS2X_SAVES_DIR", paths.savesDir.string());
        // The game's mods (collection/mods.h): the game's Mods folder's loose
        // files, then each mod it reads (its own folder's and the
        // collection's), in order, as the engine's file system reads them
        // (runtime/ps2_game_files.h); their texture folders for the GPU
        // renderer.
        {
            const std::filesystem::path modsDir = paths.supportDir / "Mods";
            const std::vector<Mod> mods =
                modsForGame(gameId,
                            listMods(modsDir, folders.resources.empty() ? std::filesystem::path() : folders.resources / "disc",
                                     settings.modsOff, gameId),
                            listSharedMods(folders.sharedMods), settings.modOrder, settings.modsOff);
            if (folders.customMap.empty())
                config.set("PS2X_MODS_DIR", ps2x::game_files::modsText(modsRoots(modsDir, mods)));
            else
            {
                // Custom Freeplay (collection/maps.h): the map's folder read
                // last (not at all when its files are converted ones, in
                // memory), the other maps left out, and the game started
                // into the slot's free play with the entry it needs on.
                CustomMap map;
                map.folder = folders.customMap;
                config.set("PS2X_MODS_DIR",
                           ps2x::game_files::modsText(freeplayModsRoots(modsDir, mods, map, folders.customMapConverted)));
            }
            if (const std::vector<std::filesystem::path> packs = texturePacks(mods, gameId); !packs.empty())
                config.set("PS2X_TEXTURE_PACKS", ps2x::game_files::modsText(packs));
            logModsRead(gameId, mods);
        }
        // The GPU renderer's pipelines this game has used, made ahead at the
        // next start (GSGpuBackendOptions::pipelineFile).
        config.set("PS2X_GS_PIPELINE_CACHE", (paths.supportDir / "gpu_pipelines.txt").string());
        // Files from the Xbox release, extracted when the app was built
        // (tools/make-app.sh; runtime/ps2_xbox_disc.h), and what the build
        // made from them in the PS2's formats (the Zoo, the Ape:
        // ps2xXboxConvert). Optional.
        bundled("PS2X_XBOX_DIR");
        bundled("PS2X_XBOX_CONVERTED");
        config.set("PS2X_SAVE_PROFILE", settings.saveProfile);
        // The game's own menus get the Settings screen (game override).
        config.set("PS2X_GAME_SETTINGS_MENUS", "1");
        // The game's string table and the developer menu (game override).
        if (has(GameOption::Language))
            config.set("PS2X_LANGUAGE", kLanguages[std::clamp(settings.language, 0, 3)]);
        if (has(GameOption::FrenchVoices) && settings.frenchVoices >= 0)
            config.set("PS2X_FRENCH_VOICES", flag(settings.frenchVoices != 0));
        if (has(GameOption::DeveloperMenu))
            config.set("PS2X_DEV_MENU", flag(settings.developerMenu));
        // The cut Zoo level (game override).
        if (has(GameOption::Zoo))
            config.set("PS2X_ZOO", flag(settings.zoo));
        // TUT/Estate made at build time from the GameCube disc
        // (ps2xGcConvert from tools/make-app.sh). Optional.
        bundled("PS2X_ESTATE_DIR");
        if (has(GameOption::Estate))
            config.set("PS2X_ESTATE", flag(settings.estate));
        // Dave Mirra BMX 2's Xbox-only levels made into its PS2 formats at
        // build time (tools/dm2_xbox.py from tools/make-app.sh),
        // bundled with DM2's files. Optional.
        bundled("PS2X_DM2_XBOX_LEVELS_DIR");
        if (has(GameOption::Dm2XboxLevels))
            config.set("PS2X_DM2_XBOX_LEVELS", flag(settings.dm2XboxLevels));
        // From the same folder (game override): the park editor's Desert and
        // Highway, the gallery's Retro movie; and the attract demo.
        if (has(GameOption::Dm2XboxThemes))
            config.set("PS2X_DM2_XBOX_THEMES", flag(settings.dm2XboxThemes));
        if (has(GameOption::Dm2RetroMovie))
            config.set("PS2X_DM2_RETRO_MOVIE", flag(settings.dm2RetroMovie));
        if (has(GameOption::Dm2AttractDemo))
            config.set("PS2X_DM2_ATTRACT_DEMO", flag(settings.dm2AttractDemo));
        if (has(GameOption::ZooUnlock))
            config.set("PS2X_ZOO_UNLOCK", kZooUnlocks[std::clamp(settings.zooUnlock, 0, 1)]);
        // The Xbox release's gameplay balance and career scripts (game override).
        if (has(GameOption::Tuning))
            config.set("PS2X_TUNING", kTunings[std::clamp(settings.tuning, 0, 1)]);
        if (has(GameOption::CareerScripts))
            config.set("PS2X_CAREER_SCRIPTS", kTunings[std::clamp(settings.careerScripts, 0, 1)]);
        // The draw distance (zge_view.cpp, dave_mirra_view.cpp): the game's
        // far clips from the boot on (PS2Runtime::initialize), so a game it
        // changes runs with one value throughout.
        if (has(GameOption::DrawDistance))
            config.set("PS2X_DRAW_DISTANCE", std::to_string(drawDistanceOf(settings, profile)));
        // The Xbox release's free skate scripts and its Ape skater (game override).
        if (has(GameOption::XboxFreeSkate))
            config.set("PS2X_XBOX_FREESKATE", flag(settings.xboxFreeSkate));
        if (has(GameOption::Ape))
            config.set("PS2X_APE", flag(settings.ape));
        // DM2's cut Create Player menu (game override). It takes effect only with the readable newRider running
        // (dave_mirra_creator.cpp checks at install): the original makes no model for a created rider, and a ride
        // with one hangs at END RUN, so with PS2X_READABLE turning newRider off the menu is not added.
        if (has(GameOption::Dm2CreatePlayer))
            config.set("PS2X_DM2_CREATE_PLAYER", flag(settings.dm2CreatePlayer));
        // BMX XXX's park editor in its main menu (game override).
        if (has(GameOption::BmxParkEditor))
            config.set("PS2X_BMX_PARK_EDITOR", flag(settings.bmxParkEditor));
        // BMX XXX's Xbox and GameCube releases' content made into its PS2
        // formats at build time (tools/bmx_releases.py from tools/make-app.sh),
        // bundled with its files, each behind its setting (game override). Optional.
        bundled("PS2X_BMX_RELEASES_DIR");
        if (has(GameOption::BmxXboxChallenges))
            config.set("PS2X_BMX_XBOX_CHALLENGES", flag(settings.bmxXboxChallenges));
        if (has(GameOption::BmxEditorPieces))
            config.set("PS2X_BMX_EDITOR_PIECES", flag(settings.bmxEditorPieces));
        if (has(GameOption::BmxLevelPreviews))
            config.set("PS2X_BMX_LEVEL_PREVIEWS", flag(settings.bmxLevelPreviews));
        if (has(GameOption::BmxLogoMovie))
            config.set("PS2X_BMX_LOGO_MOVIE", flag(settings.bmxLogoMovie));
        if (has(GameOption::BmxIntroMovie))
            config.set("PS2X_BMX_INTRO_MOVIE", flag(settings.bmxIntroMovie));
        if (has(GameOption::BmxGcLevels))
            config.set("PS2X_BMX_GC_LEVELS", flag(settings.bmxGcLevels));
        if (has(GameOption::BmxMenuLines))
            config.set("PS2X_BMX_MENU_LINES", flag(settings.bmxMenuLines));
        if (has(GameOption::BmxAllMpModes))
            config.set("PS2X_BMX_ALL_MP_MODES", flag(settings.bmxAllMpModes));
        // The game's gameplay settings (Camera, Assists, Cheats pages) and
        // where its screenshot key saves (game override).
        if (has(GameOption::GameplayPages))
            config.set("PS2X_GAMEPLAY_INI", (paths.supportDir / "gameplay.ini").string());
        config.set("PS2X_SCREENSHOT_DIR", folders.screenshots.string());
        // Play recordings (runtime/play_recording.h), when the player turned them on.
        if (settings.recordPlay)
            config.set("ZAXIS_RECORD", play_recording::folderFor(paths.supportDir).string());
        // The readable code without the held-register table (ee/abi.h leanParts), in the games whose A/B with it
        // off is identical in everything compared (AI: lean1). DM2's and BMX XXX's still differ in words the
        // original leaves from registers and stack it never set (lean1's report): on there once those are proven
        // unused. The checks run the runner with an ELF and keep the table.
        if (std::string_view(profile.id) == "ai")
            config.set("PS2X_READABLE_LEAN", "1");
        // The host input layer reads controls.ini from the same folder.
        config.set("PS2X_CONTROLS_INI", paths.controlsFile.string());
        // The Xbox release's textures and movies, converted from the user's
        // Xbox disc when the app was built (tools/make-app.sh):
        // Resources/xbox-pack. The game override reads them from there.
        bundled("PS2X_XBOX_PACK");
        config.set("PS2X_XBOX_TEXTURES", flag(settings.xboxTextures));
        config.set("PS2X_XBOX_MOVIES", flag(settings.xboxMovies));
        config.set("PS2X_XBOX_MODELS", flag(settings.xboxSkaters));
        // Texture packs' dump mode (runtime/ps2_texture_pack.h): off unless asked for.
        if (settings.textureDump)
            config.set("PS2X_TEXTURE_DUMP", (paths.supportDir / "Texture Dumps").string());
        // Native meshes on the GPU renderer unless strict is asked for.
        config.set("PS2X_RENDER_PATH", !settings.nativeMeshes ? "strict" : settings.recordsMeshes ? "records" : "auto");
        // The disc device's host form, or the game's CD/DVD client over the IOP (zge/host/disc_device.cpp).
        config.set("ZAXIS_DISC", settings.hostDisc ? "native" : "ps2");
        // Custom Freeplay's start into the map's slot (collection/maps.h
        // mapSlots), after the game's own options: the entry the start needs
        // (AI: the Estate's) is on whatever its setting says.
        if (!folders.customMap.empty())
            if (const MapSlot *slot = mapSlot(gameId, folders.customMapSlot))
            {
                config.set("PS2X_DEV_START_LEVEL", slot->startLevel);
                config.set("PS2X_DEV_START_SKATER", "ask");
                if (slot->needs[0])
                    config.set(slot->needs, "1");
                // The slot level's own NPCs, kept outside its folder: only
                // with a map that keeps them.
                if (slot->slotNpcs[0])
                    config.set(slot->slotNpcs, folders.customMapSlotNpcs ? "1" : "0");
            }
        // The renderer settings, unless the environment picks one: then all
        // three are the environment's.
        const char *backend = getEnv ? getEnv("PS2X_GS_BACKEND") : nullptr;
        if (!backend || !backend[0])
            for (const auto &[name, value] : rendererValues(settings))
                config.setOverEnvironment(name, value);
        return config;
    }
}
