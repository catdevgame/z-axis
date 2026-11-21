#include "app.h"
#include "platform/platform.h"

#include "collection/settings.h"
#include "collection/disc_check.h"
#include "collection/front_art.h"
#include "collection/game_order.h"
#include "collection/menu_audio.h"
#include "collection/menu_bottom.h"
#include "collection/menu_scene.h"
#include "collection/vector_font.h"
#include "collection/game_code.h"
#include "collection/game_setup.h"
#include "collection/disc_finder.h"
#include "collection/extras/panel_maps.h"
#include "collection/extras/setup.h"
#include "collection/extras/source_files.h"
#include "collection/save_transfer.h"
#include "collection/card_import.h"
#include "collection/maps.h"
#include "collection/menu.h"
#include "collection/mods.h"
#include "collection/mod_install.h"
#include "collection/offered.h"
#if defined(__ANDROID__)
#include "platform/android/android_assets.h"
#endif
#include "collection/playback.h"
#include "collection/recordings.h"
#include "collection/show_folder.h"
#include "collection/picture_file.h"
#include "collection/save_profiles.h"
#include "game_overrides.h"
#include "dm2/overrides/dave_mirra.h"
#include "collection/panel.h"
#include "runtime/save_store.h"
#include "machine.h"
#if defined(ZX_ENABLE_DEBUG_UI)
#include "debug_panel.h"
#endif

#include "host_backend.h"
#include "host_sdl.h"
#include "runtime/audio_mix.h"
#include "runtime/boot_config.h"
#include "runtime/front_end.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/settings.h"
#include "runtime/play_recording.h"
#include "runtime/game_module.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/xbox_disc.h"
#include "runtime/texture_replace.h"
#include "gs/gs_interpolate.h"
#include "gs/gs_null_backend.h"
// The engine's texture library reader (the menu's BMX XXX picture).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <deque>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(ZX_PGO_GENERATE)
// Instrumented builds (clang -fprofile-instr-generate): write the profile
// before std::_Exit, which skips the runtime's own atexit writer.
extern "C" int __llvm_profile_write_file(void);
#endif

#ifndef ZX_APP_NAME
#error "app.cpp is built only with ZX_APP_NAME"
#endif
#ifndef ZX_APP_GAME_TITLE
#define ZX_APP_GAME_TITLE ZX_APP_NAME
#endif
#ifndef ZX_APP_ELF_SHA256
#define ZX_APP_ELF_SHA256 ""
#endif
#ifndef ZX_APP_SAVE_DIR
#define ZX_APP_SAVE_DIR ""
#endif
#ifndef ZX_APP_VERSION
#define ZX_APP_VERSION "dev"
#endif

namespace zx_app
{
    // The app's version (CMake ZX_APP_VERSION: the tree's VERSION.txt unless the configure gave another), as one
    // string in the binary, "zx-app-version 1.3.0", which platform/android/build-apk.sh finds in the native library to check
    // it is the APK's. appVersion(): the version alone ("" a build given none).
    namespace
    {
        constexpr char kVersionStamp[] = "zx-app-version " ZX_APP_VERSION;
        const char *appVersion()
        {
            return kVersionStamp + sizeof("zx-app-version ") - 1;
        }
    }

    // A game the app can run. The single-game build has one, from the
    // ZX_APP_* definitions. A build with several (CMake ZX_APP_GAMES,
    // zx_app_games.h) is a collection named ZX_APP_NAME: the game comes
    // from its menu (or --game <id>, ZX_APP_GAME); each keeps its
    // files in <collection support>/games/<id> and its disc in
    // Resources/games/<id>/disc.
    struct GameProfile
    {
        const char *id;         // "" in the single-game build
        const char *name;       // window title, its single-game app's folder name
        const char *title;      // in messages
        const char *elf;        // boot ELF
        const char *elfSha256;  // of the recompiled ELF ("" = not checked)
        const char *saveDir;    // PCSX2 card folder offered for import ("" = none); "FOLDER/FILE" when its
                                // data file is not named like the folder (DM2's BASLUS-20159BMX2GAME/BMX2SAVE.DAT);
                                // ",FOLDER" after it for each of the game's other save folders brought with it
                                // (DM2's BASLUS-20159BMX2PARK and BASLUS-20159BMX2RPLY: offerPcsx2Import)
    };
}
#if defined(ZX_APP_GAMES_HEADER)
#include ZX_APP_GAMES_HEADER
#else
namespace zx_app
{
    constexpr GameProfile kGames[] = {
        {"", ZX_APP_NAME, ZX_APP_GAME_TITLE, ZX_APP_ELF, ZX_APP_ELF_SHA256, ZX_APP_SAVE_DIR},
    };
}
#endif

namespace zx_app
{
    namespace
    {
        using namespace zx::app;

        constexpr size_t kGameCount = sizeof(kGames) / sizeof(kGames[0]);
        constexpr bool kCollection = kGameCount > 1 || kGames[0].id[0] != '\0';
        // A collection of several games opens on its menu
        // (collection/menu.h) and comes back to it from a game; one of a
        // single game boots it and quits with it, as a single-game app does.
        constexpr bool kMenu = kCollection && kGameCount > 1;
        // The build's games (kGames, in the order ZX_APP_GAMES lists them)
        // in the collection's order (collection/game_order.h: DM2, AI, BMX
        // XXX). The menu's cards and the shell's game numbers, the Games,
        // Mods and Custom Freeplay pages and the Thor's bottom screen all
        // count in this order; what is saved is keyed by the game's id.
        std::array<GameProfile, kGameCount> gamesInOrder()
        {
            std::vector<std::string_view> ids;
            for (const GameProfile &game : kGames)
                ids.push_back(game.id);
            const std::vector<size_t> order = zx::collection::inGameOrder(ids);
            std::array<GameProfile, kGameCount> out{};
            for (size_t i = 0; i < kGameCount; ++i)
                out[i] = kGames[order[i]];
            return out;
        }
        const std::array<GameProfile, kGameCount> kGamesInOrder = gamesInOrder();

        // The launch time's measure: from the program's start (its statics
        // made) to the menu's first picture.
        const std::chrono::steady_clock::time_point g_processStart = std::chrono::steady_clock::now();
        bool g_firstPictureLogged = false;
        const GameProfile *g_game = &kGamesInOrder[0];

        // Portable mode's support folder (applyPortableMode), "" when not
        // portable: ZX_APP_SUPPORT_DIR unless the environment sets it.
        std::string g_portableSupportDir;

        // The app's own lookups: the environment, portable mode's support
        // folder in place of an unset ZX_APP_SUPPORT_DIR. What the game
        // is given goes in its BootConfig, not the environment.
        const char *getEnv(const char *name)
        {
            const char *value = std::getenv(name);
            if ((!value || !value[0]) && !g_portableSupportDir.empty() && std::string(name) == "ZX_APP_SUPPORT_DIR")
                return g_portableSupportDir.c_str();
            return value;
        }

        bool envSet(const char *name)
        {
            const char *value = getEnv(name);
            return value && value[0];
        }

        // The player changed the renderer settings while the game runs: over
        // the environment, as at the start (the GS renderer made next, on
        // PS2Machine::requestRasterBackendReload, reads them).
        void applyRendererSettings(const Settings &settings)
        {
            for (const auto &[name, value] : rendererValues(settings))
                zx::boot::change(name, value);
        }

        // Settings::wideAspect as width / height (0: the window's).
        float wideAspectValue(int index)
        {
            constexpr float kAspects[] = {16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 9.0f, 0.0f};
            return kAspects[std::clamp(index, 0, 4)];
        }

        // The view settings the game override reads (PS2Machine), unless the
        // environment sets them (ZX_ASPECT, ZX_MOTION_BLUR: the runtime
        // read those already). The draw distance came with the boot
        // (gameBootConfig's ZX_DRAW_DISTANCE).
        void applyViewSettings(const Settings &settings, PS2Machine &runtime)
        {
            if (!envSet("ZX_ASPECT"))
                runtime.setWideAspect(wideAspectValue(settings.wideAspect));
            if (!envSet("ZX_MOTION_BLUR"))
                runtime.setMotionBlur(std::clamp(settings.motionBlur, 0, 2));
            // GPU renderer picture quality (gs/gs_quality.h).
            for (const zx::gs_quality::OptionInfo &q : zx::gs_quality::options())
                if (!envSet(q.env))
                    zx::gs_quality::set(q.option, settings.quality[static_cast<size_t>(q.option)]);
        }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string logFileStem()
        {
            // The collection with its menu has one log for every game it boots.
            std::string stem = lowerAscii(kCollection && !kMenu ? g_game->id : ZX_APP_NAME);
            std::replace(stem.begin(), stem.end(), ' ', '-');
            return stem;
        }

        // stdout and stderr to <logs>/<app>.log, the previous run's log kept
        // as <app>.previous.log.
        void openLogFile(const std::filesystem::path &logDir)
        {
            std::error_code ec;
            std::filesystem::create_directories(logDir, ec);
            const std::filesystem::path log = logDir / (logFileStem() + ".log");
            if (std::filesystem::exists(log, ec))
                std::filesystem::rename(log, logDir / (logFileStem() + ".previous.log"), ec);
            if (!std::freopen(log.string().c_str(), "w", stdout))
                return;
#if defined(_WIN32)
            // The MSVC CRT has no line buffering (and rejects _IOLBF with
            // size 0): unbuffered, so a crash loses nothing.
            std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
#if defined(_WIN32)
            _dup2(_fileno(stdout), _fileno(stderr));
#else
            dup2(fileno(stdout), fileno(stderr));
#endif
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }

        struct AppState
        {
            AppPaths paths;
            Settings settings;
            bool fullscreen = false;
            bool fullscreenApplied = false;
            bool focusPauseAllowed = true;
            bool pausedForFocus = false;
            uint64_t frames = 0;
            std::function<void(bool)> showFps; // the frame rate counter (debug UI overlay)
            float audioGain = -1.0f; // last applied
            bool vsyncApplied = true;
        };

        // A recording playing back (Settings > Recordings,
        // collection/recordings.h), for the boot's life: what the window's
        // input does to it (collection/playback.h) and what it shows. Nothing
        // of the player's is written while it plays (saveSettingsNow): the
        // setting changes it replays are the recording's, not the player's.
        struct Playback
        {
            bool active = false;
            bool paused = false;
            uint64_t endVsync = 0;
            bool toldDiffers = false; // the player has been told it no longer plays as recorded
            std::vector<uint16_t> padHeld; // per gamepad slot
            zx::collection::HintDevice device = zx::collection::HintDevice::Keyboard;
            std::chrono::steady_clock::time_point lineUntil{}; // the overlay's line shown until then (and while paused)
            std::string stripShown; // what the second screen's strip was handed last
            // ZX_APP_PLAYBACK_SCRIPT (developers, checks): the player's
            // commands at vsyncs, "pause@600/2.5,leave@1800" (a pause of
            // 2.5 s of host time), as the keys give them.
            struct Step
            {
                uint64_t vsync = 0;
                zx::collection::PlaybackCommand command = zx::collection::PlaybackCommand::None;
                double seconds = 0.0;
            };
            std::vector<Step> script;
            size_t nextStep = 0;
            std::chrono::steady_clock::time_point resumeAt{};
        };
        Playback g_playback;

        std::vector<Playback::Step> playbackScript(const char *spec)
        {
            std::vector<Playback::Step> steps;
            std::stringstream items(spec ? spec : "");
            for (std::string item; std::getline(items, item, ',');)
            {
                Playback::Step step;
                const size_t at = item.find('@');
                const std::string verb = item.substr(0, at);
                step.command = verb == "pause"   ? zx::collection::PlaybackCommand::Pause
                               : verb == "leave" ? zx::collection::PlaybackCommand::Leave
                                                 : zx::collection::PlaybackCommand::None;
                if (at == std::string::npos || step.command == zx::collection::PlaybackCommand::None)
                {
                    std::cerr << "[app] ZX_APP_PLAYBACK_SCRIPT: ignoring \"" << item << "\"" << std::endl;
                    continue;
                }
                step.vsync = std::strtoull(item.c_str() + at + 1, nullptr, 10);
                if (const size_t slash = item.find('/', at); slash != std::string::npos)
                    step.seconds = std::atof(item.c_str() + slash + 1);
                steps.push_back(step);
            }
            return steps;
        }

        // The game's settings: its file's header, its options and rows
        // (zx::app::GameSettingsProfile).
        const GameSettingsProfile *g_profile = nullptr; // set by runApp

        bool gameHas(GameOption option)
        {
            return g_profile && g_profile->has(option);
        }

        // What this boot's setup has (collection/offered.h): the folders the
        // boot was given, the platform, and the readable code Dave Mirra BMX
        // 2's created riders need (the game's own answer).
        Installed setupInstalled()
        {
            Installed installed = findInstalled([](const char *name) { return zx::boot::text(name); });
            installed.set(Need::Dm2ReadableRider, dave_mirra::createPlayerCanRun());
            return installed;
        }

        // A collection keeps the app's own settings (display, graphics,
        // sound: the same in every game) in <support>/settings.ini and only
        // the game's options in games/<id>/settings.ini
        // (loadCollectionSettings).
        std::filesystem::path g_appSettingsFile; // empty: the single-game layout

        bool loadAppSettings(const AppState &state, Settings &settings, std::vector<std::string> *warnings)
        {
            if (g_appSettingsFile.empty())
                return loadSettings(state.paths.settingsFile, settings, warnings);
            return loadCollectionSettings(g_appSettingsFile, state.paths.settingsFile, settings, warnings);
        }

        // A saves import replaced the settings files (save_transfer.h): the
        // settings in use are read again from them, so a change made after
        // it starts from what was imported, and the quit, which keeps the
        // window's place and full screen, writes nothing over them.
        bool g_settingsImported = false;
        void useImportedSettings(AppState &state)
        {
            std::vector<std::string> warnings;
            loadAppSettings(state, state.settings, &warnings);
            g_settingsImported = true;
            std::cout << "[app] settings: read again from the files imported" << std::endl;
        }

        void saveSettingsNow(const AppState &state)
        {
            if (g_playback.active)
                return;
            if (g_appSettingsFile.empty())
            {
                if (!writeTextFile(state.paths.settingsFile, formatSettings(state.settings, *g_profile, SettingsScope::All)))
                    std::cerr << "[app] could not write " << state.paths.settingsFile.string() << std::endl;
                return;
            }
            if (!g_profile)
            {
                // The collection's menu before any game has booted: only the
                // app's own settings can have changed, and only the root
                // file is written (a game's file is its own).
                if (!writeTextFile(g_appSettingsFile, formatSettings(state.settings, everyOptionProfile(), SettingsScope::App)))
                    std::cerr << "[app] could not write " << g_appSettingsFile.string() << std::endl;
                return;
            }
            if (!saveCollectionSettings(g_appSettingsFile, state.paths.settingsFile, state.settings, *g_profile))
                std::cerr << "[app] could not write " << state.paths.settingsFile.string() << " or "
                          << g_appSettingsFile.string() << std::endl;
        }

        bool currentFullscreen()
        {
            bool known = false;
            const bool native = platform::isFullscreen(known);
            if (known)
                return native;
            return ps2_host::windowFullscreen();
        }

        void toggleFullscreen()
        {
            if (!platform::toggleFullscreen())
                ps2_host::setWindowFullscreen(!ps2_host::windowFullscreen());
        }

        // Window, full screen (the system's) or borderless full screen.
        void setWindowMode(AppState &state, int mode)
        {
            const bool wantFull = mode != 0;
            if (mode == 2 && platform::setBorderless(true))
                return;
            if (platform::isBorderless())
                platform::setBorderless(false);
            if (wantFull != currentFullscreen())
                toggleFullscreen();
            (void)state;
        }

        // The screenshot key ([hotkeys] screenshot, F12), every game's: the
        // presented picture at the renderer's resolution as a PNG in
        // ZX_SCREENSHOT_DIR (Pictures/<app>/<game>), named after the game.
        std::filesystem::path screenshotFile()
        {
            std::filesystem::path dir = "Screenshots";
            if (const char *env = zx::boot::text("ZX_SCREENSHOT_DIR"); env && env[0])
                dir = env;
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::time_t now = std::time(nullptr);
            char stamp[64] = {};
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", std::localtime(&now));
            const std::string stem = std::string(g_game->name) + " " + stamp;
            std::filesystem::path file = dir / (stem + ".png");
            for (int n = 2; std::filesystem::exists(file, ec) && n < 100; ++n)
                file = dir / (stem + " " + std::to_string(n) + ".png");
            return file;
        }

        void takeScreenshot(PS2Machine &runtime)
        {
            const std::filesystem::path file = screenshotFile();
            runtime.requestScreenshot(file.string(), [file](bool ok)
                                      {
                                          std::cout << "[app] screenshot " << (ok ? "saved: " : "failed: ") << file.string()
                                                    << std::endl;
                                          zx::settings::notify(ok ? "Screenshot saved" : "Screenshot failed");
                                      });
        }

        // ---- the map window ---------------------------------------------------
        // The bottom screen (panel/panel.h: the level's map, goals and tricks)
        // in a second window while [display] map_window is on. The game
        // thread takes a snapshot of the game at each VBlank, as the Thor
        // does; the window thread paints the panel from the latest one about
        // 20 times a second into a 1240x1080 picture and shows it, and hands
        // the window's pointer to the panel: a press, drag and release are a
        // touch, the wheel scrolls the side pane's list or zooms the map
        // about the pointer (Panel::wheel). The window keeps its
        // place and size in the settings; its close button turns the setting
        // off. ZX_MAP_WINDOW_SHOT=<file.png> writes the picture painted ten
        // seconds after a level is in it, in a headless run too (the painter
        // runs, no window opens).
        bool headlessHost();

        struct MapWindow
        {
            static constexpr int kWidth = panel::kScreenWidth, kHeight = panel::kScreenHeight;
            static constexpr size_t kGuestRamBytes = 32u * 1024u * 1024u; // the game's main RAM, as the Thor passes it
            std::mutex mutex;
            panel::Snapshot snapshot;           // the latest, under mutex
            std::atomic<bool> capturing{false}; // the game thread takes snapshots
            std::atomic<double> capturedAt{0.0};
            std::unique_ptr<panel::Panel> panel;
            std::string dataFolder, dataFallback;
            std::vector<uint32_t> pixels = std::vector<uint32_t>(static_cast<size_t>(kWidth) * kHeight, 0u);
            std::chrono::steady_clock::time_point nextPaint{};
            bool open = false;
            bool onTop = false; // Always on Top as the window has it
            bool pointerDown = false;
            std::string shotFile; // ZX_MAP_WINDOW_SHOT
            bool shotRead = false;
            bool shotDone = false;
            int levelPaints = 0; // paints with a level in the snapshot
        };

        MapWindow &mapWindow()
        {
            static MapWindow window;
            return window;
        }

        double mapSeconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The game thread, at each VBlank (PS2Machine::addVSyncHook): read-only.
        void captureMapSnapshot(uint8_t *rdram)
        {
            MapWindow &m = mapWindow();
            if (!rdram || !m.capturing.load(std::memory_order_acquire))
                return;
            static panel::Snapshot captured;
            panel::captureSnapshot(rdram, MapWindow::kGuestRamBytes, captured);
            {
                std::lock_guard<std::mutex> lock(m.mutex);
                m.snapshot = captured;
            }
            m.capturedAt.store(mapSeconds(), std::memory_order_release);
        }

        // The window's pointer as touches: the left button is one finger; the
        // wheel the panel's own (Panel::wheel: the side pane's list scrolls,
        // the map zooms), left alone while the button is down.
        void mapPointer(MapWindow &m, const ps2_host::MapPointerEvent &e)
        {
            using Kind = ps2_host::MapPointerEvent::Kind;
            const double now = mapSeconds();
            auto touch = [&](panel::TouchPhase phase, int pointer, float x, float y)
            { m.panel->touch(panel::TouchEvent{phase, pointer, x, y, now}); };
            switch (e.kind)
            {
            case Kind::Down:
                touch(panel::TouchPhase::Down, 0, e.x, e.y);
                m.pointerDown = true;
                break;
            case Kind::Move:
                if (m.pointerDown)
                    touch(panel::TouchPhase::Move, 0, e.x, e.y);
                break;
            case Kind::Up:
                if (m.pointerDown)
                    touch(panel::TouchPhase::Up, 0, e.x, e.y);
                m.pointerDown = false;
                break;
            case Kind::Wheel:
                if (!m.pointerDown)
                    m.panel->wheel(e.x, e.y, e.wheel, now);
                break;
            }
        }

        // Once per host frame on the window thread, in the menu and in a game.
        void mapWindowFrame(AppState &state)
        {
            MapWindow &m = mapWindow();
            if (!m.shotRead)
            {
                m.shotRead = true;
                if (const char *file = std::getenv("ZX_MAP_WINDOW_SHOT"); file && file[0])
                    m.shotFile = file;
            }
            const bool wanted = state.settings.mapWindow && !headlessHost();
            if (wanted && !m.open)
            {
                const ps2_host::MapWindowRect rect{state.settings.mapWindowX, state.settings.mapWindowY,
                                                   state.settings.mapWindowWidth, state.settings.mapWindowHeight};
                m.open = ps2_host::openMapWindow("Map", rect);
                m.onTop = false;
                if (m.open)
                    std::cout << "[app] map window: open" << std::endl;
                else
                {
                    std::cerr << "[app] map window: could not be opened; the setting is off" << std::endl;
                    state.settings.mapWindow = false;
                    saveSettingsNow(state);
                }
            }
            else if (!wanted && m.open)
            {
                ps2_host::closeMapWindow();
                m.open = false;
            }
            if (m.open)
            {
                if (ps2_host::takeMapWindowClosed())
                {
                    m.open = false;
                    m.pointerDown = false;
                    state.settings.mapWindow = false;
                    saveSettingsNow(state);
                    std::cout << "[app] map window: closed" << std::endl;
                }
                else if (const ps2_host::MapWindowRect rect = ps2_host::mapWindowRect(); rect.width > 0 && rect.height > 0)
                {
                    if (m.onTop != state.settings.mapWindowOnTop)
                    {
                        ps2_host::setMapWindowOnTop(state.settings.mapWindowOnTop);
                        m.onTop = state.settings.mapWindowOnTop;
                    }
                    // Remembered in the settings (written with them at the quit).
                    state.settings.mapWindowX = rect.x;
                    state.settings.mapWindowY = rect.y;
                    state.settings.mapWindowWidth = rect.width;
                    state.settings.mapWindowHeight = rect.height;
                }
            }
            const bool painting = m.open || (!m.shotFile.empty() && !m.shotDone);
            m.capturing.store(painting, std::memory_order_release);
            if (!painting)
                return;
            if (!m.panel)
                m.panel = std::make_unique<panel::Panel>();
            if (const std::string folder = panel::gameFiles(), fallback = panel::gameFilesFallback();
                folder != m.dataFolder || fallback != m.dataFallback)
            {
                m.dataFolder = folder;
                m.dataFallback = fallback;
                m.panel->setDataFolder(folder, fallback);
            }
            if (m.open)
                for (const ps2_host::MapPointerEvent &e : ps2_host::takeMapEvents())
                    mapPointer(m, e);
            const auto now = std::chrono::steady_clock::now();
            if (now < m.nextPaint)
                return;
            m.nextPaint = now + std::chrono::milliseconds(50);
            panel::Snapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(m.mutex);
                snapshot = m.snapshot;
            }
            if (mapSeconds() - m.capturedAt.load(std::memory_order_acquire) > 2.0)
                snapshot.mode = panel::GameMode::NoLevel; // nothing is running: blank, not the last level
            m.panel->draw(snapshot, mapSeconds(),
                          panel::Surface{m.pixels.data(), MapWindow::kWidth, MapWindow::kHeight, MapWindow::kWidth});
            // What the top screen may leave out (Clean Top Screen): the
            // window's, or a ZX_MAP_WINDOW_SHOT run's while it paints.
            panel::setBottomScreenShows(m.panel->shows());
            const auto *rgba = reinterpret_cast<const uint8_t *>(m.pixels.data());
            const uint32_t stride = static_cast<uint32_t>(MapWindow::kWidth) * 4u;
            if (m.open)
                ps2_host::presentMap(rgba, MapWindow::kWidth, MapWindow::kHeight, stride);
            if (!m.shotFile.empty() && !m.shotDone && snapshot.mode != panel::GameMode::NoLevel && ++m.levelPaints >= 200)
            {
                m.shotDone = true;
                const bool ok = ps2_host::writePng(m.shotFile, rgba, MapWindow::kWidth, MapWindow::kHeight, stride);
                size_t lit = 0;
                for (const uint32_t pixel : m.pixels)
                    lit += (pixel & 0x00FFFFFFu) != 0u;
                std::cout << "[app] map window: picture " << m.shotFile << (ok ? "" : " (not written)") << " (level "
                          << snapshot.level << " \"" << snapshot.levelFile << "\", mode " << static_cast<int>(snapshot.mode)
                          << ", " << lit << " lit pixels, files " << m.dataFolder << ")" << std::endl;
            }
        }

        // Frame Interpolation: the setting, or ZX_INTERPOLATION=on|off over it.
        void applyFrameInterpolation(const AppState &state)
        {
            static const char *const env = std::getenv("ZX_INTERPOLATION");
            const bool on = env && env[0] ? (std::strcmp(env, "on") == 0 || std::strcmp(env, "1") == 0)
                                          : state.settings.frameInterpolation;
            zx::gs_interpolate::setEnabled(on);
        }

        // The window's part of a host frame, in the menu and in a game:
        // the full screen asked for at the start (once per process: the
        // window keeps it across games), Alt+Enter, the window's size kept
        // for the next start, the master volume, vsync.
        void windowFrame(AppState &state)
        {
            ++state.frames;
            if (!state.fullscreenApplied && state.frames >= 2)
            {
                state.fullscreenApplied = true;
                if (state.settings.fullscreen)
                    setWindowMode(state, state.settings.fullscreenMode == FullscreenMode::Borderless ? 2 : 1);
            }

            // Alt+Enter (Cmd+Ctrl+F and Cmd+F are menu shortcuts); F11 on
            // Windows too.
            constexpr int kEnter = 257, kKeypadEnter = 335, kLeftAlt = 342, kRightAlt = 346;
            if ((ps2_host::keyPressed(kEnter) || ps2_host::keyPressed(kKeypadEnter)) &&
                (ps2_host::keyDown(kLeftAlt) || ps2_host::keyDown(kRightAlt)))
                toggleFullscreen();
#if defined(_WIN32)
            constexpr int kF11 = 300;
            if (ps2_host::keyPressed(kF11))
                toggleFullscreen();
#endif

            if (state.fullscreenApplied)
            {
                state.fullscreen = currentFullscreen();
                if (!state.fullscreen && !ps2_host::windowMinimized())
                {
                    int width = 0;
                    int height = 0;
                    ps2_host::windowSize(width, height);
                    state.settings.windowWidth = std::max(320, width);
                    state.settings.windowHeight = std::max(240, height);
                }
            }

            // Master volume, silent in the background if asked.
            const bool background = !ps2_host::windowFocused() && state.focusPauseAllowed;
            const float gain = (state.settings.muteOnFocusLoss && background) ? 0.0f : state.settings.volume / 100.0f;
            if (gain != state.audioGain)
            {
                state.audioGain = gain;
                ps2_host::setAudioGain(gain);
            }
            if (state.settings.vsync != state.vsyncApplied)
            {
                state.vsyncApplied = state.settings.vsync;
                ps2_host::setVsync(state.settings.vsync);
            }
            ps2_host::setVariableRefresh(state.settings.variableRefresh);
            applyFrameInterpolation(state);
            mapWindowFrame(state);
            // (ZX_CLEAN_TOP_SCREEN, when set, wins: panel_snapshot.cpp)
            panel::setCleanTopScreen(state.settings.cleanTopScreen ? panel::CleanTopScreen::On : panel::CleanTopScreen::Off);
        }

        // The game's Clean Top Screen row while a bottom screen is up: added
        // when one comes (the Map Window opened, a second display), removed
        // when it goes. Window thread, once a host frame in a game.
        constexpr const char *kCleanTopScreenKey = "display.clean_top_screen";
        zx::settings::Setting cleanTopScreenSetting(AppState &state); // (with the other rows, below)
        void cleanTopScreenRowFrame(AppState &state)
        {
            const bool wanted = offered(Need::BottomScreen, platformInstalled());
            if (wanted == zx::settings::has(kCleanTopScreenKey))
                return;
            if (wanted)
                zx::session::addSetting(cleanTopScreenSetting(state));
            else
                zx::settings::remove(kCleanTopScreenKey);
        }

        // The keys' presses as last seen ([hotkeys]): each boot starts from
        // the count at its start.
        uint32_t g_switchGamePresses = 0;
        uint32_t g_screenshotPresses = 0;

        // The app menu's Settings... (Cmd+,; Windows' system menu) chosen
        // while the collection's menu is up, for its next frame: its
        // Settings page (the window thread's, as the menu's own items).
        bool g_menuSettingsWanted = false;

        // Restart Game chosen (the game's Settings > Game): the game left and
        // booted again at once, its custom map too. And how many changes
        // waiting for it the player has been told of this boot.
        bool g_restartWanted = false;
        size_t g_restartWaitingSaid = 0;
        bool g_restartOffered = false; // this boot can be restarted (a collection's game, no playback)

        // Restart Game: the game started again (the collection's menu boots
        // it), for the changes that apply at next start; its value says how
        // many wait.
        zx::settings::Setting restartGameSetting()
        {
            namespace st = zx::settings;
            st::Setting restart;
            restart.key = "game.restart";
            restart.label = "Restart Game";
            restart.kind = st::Kind::Action;
            restart.help = "Starts the game again, for the changes that apply at next start.";
            restart.format = [](int)
            {
                const size_t waiting = st::waitingForRestart().size();
                return waiting == 1 ? std::string("1 change waits") : std::to_string(waiting) + " changes wait";
            };
            restart.activate = []()
            {
                std::cout << "[app] restart game: " << st::waitingForRestart().size() << " change(s) waiting" << std::endl;
                g_restartWanted = true;
                ps2_host::requestExitToMenu();
            };
            return restart;
        }

        // A playback's part of a host frame (window thread): Pause, Resume
        // and Leave from the keys, the gamepads and the second screen's
        // strip; the position on the overlay's line and the strip; the
        // player told once when the game no longer plays as recorded.
        void playbackFrame(PS2Machine &runtime, AppState &state)
        {
            namespace col = zx::collection;
            namespace hi = ps2_host_input;
            Playback &p = g_playback;
            const auto now = std::chrono::steady_clock::now();
            const ps2_host::InputFrame &frame = ps2_host::input();
            std::vector<int> keys;
            for (int code = 0; code < hi::kKeyCodeCount; ++code)
                if (frame.pressed.test(static_cast<size_t>(code)))
                    keys.push_back(code);
            uint16_t padPressed = 0;
            const int pads = ps2_host::gamepadCount();
            p.padHeld.resize(static_cast<size_t>(std::max(pads, 0)), 0);
            for (int slot = 0; slot < pads; ++slot)
            {
                const hi::GamepadState pad = ps2_host::gamepadState(slot);
                const uint16_t held = pad.present ? pad.buttons : 0;
                const uint16_t down = static_cast<uint16_t>(held & ~p.padHeld[static_cast<size_t>(slot)]);
                p.padHeld[static_cast<size_t>(slot)] = held;
                if (down == 0)
                    continue;
                padPressed = static_cast<uint16_t>(padPressed | down);
                switch (ps2_host::gamepadStyle(slot))
                {
                case ps2_host::GamepadStyle::PlayStation:
                    p.device = col::HintDevice::PlayStation;
                    break;
                case ps2_host::GamepadStyle::Nintendo:
                    p.device = col::HintDevice::Nintendo;
                    break;
                default:
                    p.device = col::HintDevice::Xbox;
                    break;
                }
            }
            if (padPressed == 0 && !keys.empty())
                p.device = col::HintDevice::Keyboard;
            std::vector<col::PlaybackCommand> commands = {col::playbackCommand(keys, padPressed, p.paused)};
            {
                // (a developer's script: its steps as they come, a pause's end by host time)
                const uint64_t vsync = play_recording::replayResults().vsync;
                if (p.paused && p.resumeAt != std::chrono::steady_clock::time_point{} && now >= p.resumeAt)
                {
                    p.resumeAt = {};
                    commands.push_back(col::PlaybackCommand::Resume);
                }
                while (!p.paused && p.nextStep < p.script.size() && vsync >= p.script[p.nextStep].vsync)
                {
                    const Playback::Step &step = p.script[p.nextStep++];
                    std::cout << "[app] playback script: " << (step.command == col::PlaybackCommand::Pause ? "pause" : "leave")
                              << " at vsync " << vsync << std::endl;
                    commands.push_back(step.command);
                    if (step.command == col::PlaybackCommand::Pause)
                    {
                        p.resumeAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(step.seconds));
                        break;
                    }
                }
            }
            for (const col::PlaybackCommand tap : col::PlaybackStrip::shared().takeTaps())
            {
                p.device = col::HintDevice::Touch;
                commands.push_back(tap);
            }
            const bool wasPaused = p.paused;
            for (const col::PlaybackCommand command : commands)
                switch (command)
                {
                case col::PlaybackCommand::Pause:
                    p.paused = true;
                    break;
                case col::PlaybackCommand::Resume:
                    p.paused = false;
                    break;
                case col::PlaybackCommand::Leave:
                    if (!ps2_host::exitToMenuRequested())
                        std::cout << "[app] playback: left" << std::endl;
                    ps2_host::requestExitToMenu();
                    break;
                case col::PlaybackCommand::None:
                    break;
                }
            const play_recording::ReplayResults results = play_recording::replayResults();
            if (p.paused != wasPaused)
            {
                runtime.setHostPaused(p.paused || state.pausedForFocus);
                std::cout << "[app] playback: " << (p.paused ? "paused" : "resumed") << " at vsync " << results.vsync << std::endl;
                p.lineUntil = now + std::chrono::seconds(3);
            }
            if (results.checksumsDiffered > 0 && !p.toldDiffers)
            {
                // (a recording made by another version, or something this one does not yet replay)
                p.toldDiffers = true;
                std::cout << "[app] playback: the game's state differs from the recording's from vsync "
                          << results.firstDifferingVsync << std::endl;
                zx::settings::notify("From here it plays differently than it was recorded.", 6.0);
                p.lineUntil = now;
            }
            else if (p.paused || now < p.lineUntil)
                zx::settings::notify(col::playbackLine(results.vsync, p.endVsync, p.paused, p.device), 0.5);
            const std::string position = col::playbackPosition(results.vsync, p.endVsync);
            if (const std::string strip = position + (p.paused ? "|paused" : ""); strip != p.stripShown)
            {
                p.stripShown = strip;
                col::PlaybackStrip::shared().show(col::StripView{position, p.paused});
            }
        }

        // Once per host frame on the window thread while a game runs.
        // ZX_PAIR_SHOT=<trigger file> (developers: the top and the bottom
        // screen at one moment, e.g. while a player holds the game at a pad
        // read): when the file appears, its first line is a path prefix; the
        // game's next picture is saved as PREFIX-top.png (as the screenshot
        // key saves it) and the map window's last as PREFIX-map.png, and the
        // file is removed once both are written. Window thread, in a game.
        void pairShotFrame(PS2Machine &runtime)
        {
            static const char *const trigger = std::getenv("ZX_PAIR_SHOT");
            static std::atomic<bool> busy{false};
            if (!trigger || !trigger[0] || busy.load())
                return;
            std::ifstream in(trigger);
            std::string prefix;
            if (!in || !std::getline(in, prefix) || prefix.empty())
                return;
            in.close();
            MapWindow &m = mapWindow();
            const auto *rgba = reinterpret_cast<const uint8_t *>(m.pixels.data());
            const bool mapOk = ps2_host::writePng(prefix + "-map.png", rgba, MapWindow::kWidth, MapWindow::kHeight,
                                                  static_cast<uint32_t>(MapWindow::kWidth) * 4u);
            busy = true;
            runtime.requestScreenshot(prefix + "-top.png", [prefix, mapOk, file = std::string(trigger)](bool ok)
                                      {
                                          std::cout << "[app] pair shot " << prefix << ": top " << (ok ? "ok" : "failed")
                                                    << ", map " << (mapOk ? "ok" : "failed") << std::endl;
                                          std::error_code ec;
                                          std::filesystem::remove(file, ec);
                                          busy = false;
                                      });
        }

        // A change that waits for the game's next start (a restart row's):
        // Restart Game on the page (the last row of Settings > Game) while
        // one does, gone again when none does, so the page is as it was
        // with nothing changed. After each change the settings run
        // (zx::settings::setAfterChanges), so the game's page shows it at
        // once; the number of changes waiting it says comes from its format.
        size_t restartRowFrame()
        {
            const size_t waiting = zx::settings::waitingForRestart().size();
            const bool shown = zx::settings::has("game.restart");
            if (waiting > 0 && !shown)
                zx::settings::add(restartGameSetting());
            else if (waiting == 0 && shown)
                zx::settings::remove("game.restart");
            return waiting;
        }

        // The player told once, over the game, when more changes come to
        // wait for the next start (when no other message is up), and the row
        // checked again (a change made by no row of the page). Twice a
        // second.
        void restartWaitingFrame()
        {
            static int frames = 0;
            if (!g_restartOffered || ++frames % 30 != 0)
                return;
            const size_t waiting = restartRowFrame();
            if (waiting > g_restartWaitingSaid && zx::settings::notice().empty())
            {
                zx::settings::notify("Restart Game, at the end of Settings > Game, applies it now.", 4.0);
                g_restartWaitingSaid = waiting;
            }
            else if (waiting < g_restartWaitingSaid)
                g_restartWaitingSaid = waiting;
        }

        void gameFrame(PS2Machine &runtime, AppState &state)
        {
            // The collection's Switch Game key ([hotkeys] switch_game, F8):
            // back to the menu.
            if (const uint32_t presses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeySwitchGame);
                presses != g_switchGamePresses)
            {
                g_switchGamePresses = presses;
                if (ps2_host::menuAvailable())
                {
                    std::cout << "[app] switch game key: exit to the menu" << std::endl;
                    ps2_host::requestExitToMenu();
                }
            }
            if (const uint32_t presses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeyScreenshot);
                presses != g_screenshotPresses)
            {
                g_screenshotPresses = presses;
                takeScreenshot(runtime);
            }
            windowFrame(state);
            cleanTopScreenRowFrame(state);
            pairShotFrame(runtime);
            restartWaitingFrame();

            runtime.setIntegerScale(state.settings.scaling == Scaling::Integer);
            if (state.showFps)
                state.showFps(state.settings.showFps);

            // The Native Meshes row is the game override's (it switches the
            // renderer's path while the game runs); its value is kept in
            // settings.ini from here, as the next boot's ZX_RENDER_PATH.
            if (zx::settings::has("display.native_meshes"))
            {
                const bool nativeMeshes = zx::settings::value("display.native_meshes", 1) != 0;
                if (nativeMeshes != state.settings.nativeMeshes)
                {
                    state.settings.nativeMeshes = nativeMeshes;
                    saveSettingsNow(state);
                }
            }

            const bool wantPause = state.focusPauseAllowed && state.settings.pauseOnFocusLoss && !ps2_host::windowFocused();
            if (wantPause != state.pausedForFocus)
            {
                state.pausedForFocus = wantPause;
                // (a playback paused by the player stays paused)
                runtime.setHostPaused(wantPause || g_playback.paused);
                std::cout << (wantPause ? "[app] paused (window in the background)" : "[app] resumed") << std::endl;
            }
            if (g_playback.active)
                playbackFrame(runtime, state);
        }

        // The app's own rows (the window, sound), on state.settings: the
        // game's registry has them while a game runs (registerSettings) and
        // the collection's menu shows the same before any game has booted
        // (menuSettingsRows).
        zx::settings::Setting toggleSetting(AppState &state, const char *key, const char *label, bool Settings::*field,
                                              const char *help)
        {
            zx::settings::Setting setting;
            setting.key = key;
            setting.label = label;
            setting.help = help;
            setting.current = [&state, field]() { return state.settings.*field ? 1 : 0; };
            setting.apply = [&state, field](int value)
            {
                state.settings.*field = value != 0;
                saveSettingsNow(state);
            };
            return setting;
        }

        // Offered where the platform has a window to switch (not Android);
        // Borderless where it has one (macOS, Windows).
        zx::settings::Setting windowModeSetting(AppState &state)
        {
            namespace st = zx::settings;
            st::Setting window;
            window.key = "display.window";
            window.label = "Display Mode";
            window.kind = st::Kind::Choice;
            window.choices = {"Window", "Full Screen"};
            if (offered(Need::Borderless, platformInstalled()))
            {
                window.choices.push_back("Borderless");
#if defined(__APPLE__)
                window.help = "Borderless stays in the same Space.";
#endif
            }
            window.current = [&state]()
            {
                if (!currentFullscreen())
                    return 0;
                return platform::isBorderless() ? 2 : 1;
            };
            window.apply = [&state](int value)
            {
                if (value != 0)
                    state.settings.fullscreenMode = value == 2 ? FullscreenMode::Borderless : FullscreenMode::Native;
                state.settings.fullscreen = value != 0;
                setWindowMode(state, value);
                saveSettingsNow(state);
            };
            return window;
        }

        zx::settings::Setting scalingSetting(AppState &state)
        {
            namespace st = zx::settings;
            st::Setting scaling;
            scaling.key = "display.scaling";
            scaling.label = "Scaling";
            scaling.kind = st::Kind::Choice;
            scaling.choices = {"Smooth", "Integer"};
            scaling.help = "Integer keeps pixels sharp.";
            scaling.current = [&state]() { return state.settings.scaling == Scaling::Integer ? 1 : 0; };
            scaling.apply = [&state](int value)
            {
                state.settings.scaling = value == 1 ? Scaling::Integer : Scaling::Smooth;
                saveSettingsNow(state);
            };
            return scaling;
        }

        zx::settings::Setting vsyncSetting(AppState &state)
        {
            return toggleSetting(state, "display.vsync", "VSync", &Settings::vsync, "Off lowers latency but may tear.");
        }

        zx::settings::Setting variableRefreshSetting(AppState &state)
        {
            return toggleSetting(state, "display.variable_refresh", "Variable Refresh", &Settings::variableRefresh,
                                 "For G-Sync and FreeSync displays.");
        }

        zx::settings::Setting frameInterpolationSetting(AppState &state)
        {
            return toggleSetting(state, "display.frame_interpolation", "Frame Interpolation", &Settings::frameInterpolation,
                                 "Smoother motion on 120 Hz and faster displays.");
        }

        zx::settings::Setting showFpsSetting(AppState &state)
        {
            return toggleSetting(state, "display.show_fps", "Show Frame Rate", &Settings::showFps, "Frames per second, top right.");
        }

        zx::settings::Setting pauseInBackgroundSetting(AppState &state)
        {
            return toggleSetting(state, "display.pause_in_background", "Pause in Background", &Settings::pauseOnFocusLoss,
                                 "Pauses when you switch away.");
        }

        // Play recordings (runtime/play_recording.h): off by default; the
        // Recordings line says where they are and how many.
        zx::settings::Setting recordPlaySetting(AppState &state)
        {
            zx::settings::Setting record = toggleSetting(state, "game.record_play", "Record Play", &Settings::recordPlay,
                                                           "Saves your inputs for a replay.");
            record.restart = true;
            return record;
        }

        zx::settings::Setting recordingsSetting(AppState &state)
        {
            namespace st = zx::settings;
            const std::filesystem::path folder = play_recording::folderFor(state.paths.supportDir);
            struct Count
            {
                std::mutex mutex;
                std::chrono::steady_clock::time_point at{};
                size_t files = 0;
            };
            auto count = std::make_shared<Count>();
            st::Setting where;
            where.key = "game.recordings";
            where.label = "Recordings";
            where.kind = st::Kind::Action;
            where.help = folder.string();
            where.format = [folder, count](int)
            {
                std::lock_guard<std::mutex> lock(count->mutex);
                const auto now = std::chrono::steady_clock::now();
                if (now - count->at > std::chrono::seconds(2))
                {
                    count->files = play_recording::countIn(folder);
                    count->at = now;
                }
                return std::to_string(count->files) + (count->files == 1 ? " file" : " files");
            };
            // (shown in the system's file browser; where there is none, said)
            where.activate = [folder]()
            {
                if (!zx::collection::showFolder(folder))
                    zx::settings::notify(zx::collection::folderNotice("Recordings", folder), 6.0);
            };
            return where;
        }

        zx::settings::Setting volumeSetting(AppState &state)
        {
            namespace st = zx::settings;
            st::Setting volume;
            volume.key = "audio.volume";
            volume.label = "Master Volume";
            volume.kind = st::Kind::Range;
            volume.minValue = 0;
            volume.maxValue = 100;
            volume.step = 5;
            volume.unit = "%";
            volume.help = "All sound.";
            volume.current = [&state]() { return state.settings.volume; };
            volume.apply = [&state](int value)
            {
                state.settings.volume = std::clamp(value, 0, 100);
                saveSettingsNow(state);
            };
            return volume;
        }

        zx::settings::Setting muteInBackgroundSetting(AppState &state)
        {
            return toggleSetting(state, "audio.mute_in_background", "Mute in Background", &Settings::muteOnFocusLoss,
                                 "Mutes when you switch away.");
        }

        zx::settings::Setting mapWindowSetting(AppState &state)
        {
            return toggleSetting(state, "display.map_window", "Map Window", &Settings::mapWindow,
                                 "The map, goals and tricks in a second window.");
        }

        // The Map Window above the other windows (mapWindowFrame applies it
        // to the open window).
        zx::settings::Setting mapWindowOnTopSetting(AppState &state)
        {
            return toggleSetting(state, "display.map_window_on_top", "Always on Top", &Settings::mapWindowOnTop,
                                 "Keeps the Map Window above other windows.");
        }

        // Clean Top Screen (runtime/panel_snapshot.h): while a bottom screen
        // is up, the game's HUD pieces it shows are left off the top screen,
        // drawing only. Offered while there is one (Need::BottomScreen: the
        // row comes and goes with it, cleanTopScreenRowFrame).
        zx::settings::Setting cleanTopScreenSetting(AppState &state)
        {
            return toggleSetting(state, kCleanTopScreenKey, "Clean Top Screen", &Settings::cleanTopScreen,
                                 "Hides the HUD pieces the bottom screen already shows.");
        }

        // Custom Freeplay (collection/maps.h): the menu's fourth tile. Off by
        // default: the menu as it was, no boot given a map.
        zx::settings::Setting customFreeplaySetting(AppState &state)
        {
            return toggleSetting(state, "game.custom_freeplay", "Custom Freeplay", &Settings::customFreeplay,
                                 "Adds a tile for custom maps.");
        }

        // The collection's menu ([menu], collection/menu_scene.h): what plays
        // at launch, and the shown game's music under the menu.
        zx::settings::Setting introMovieSetting(AppState &state)
        {
            namespace st = zx::settings;
            st::Setting intro;
            intro.key = "menu.intro_movie";
            intro.label = "Intro Movie";
            intro.kind = st::Kind::Choice;
            intro.choices = {"Logo", "Off"};
            intro.help = "What plays at start.";
            intro.current = [&state]() { return state.settings.introLogo ? 0 : 1; };
            intro.apply = [&state](int value)
            {
                state.settings.introLogo = value == 0;
                saveSettingsNow(state);
            };
            return intro;
        }

        zx::settings::Setting menuMusicSetting(AppState &state)
        {
            return toggleSetting(state, "menu.menu_music", "Menu Music", &Settings::menuMusic, "Each game's own music under its card.");
        }

        // A row of the shared settings model as the menu's Settings page has it
        // (a Setting's kind, choices, range, help and functions, one to one).
        zx::collection::SettingsRow menuRowOf(const zx::settings::Setting &setting)
        {
            namespace st = zx::settings;
            zx::collection::SettingsRow out;
            out.kind = setting.kind == st::Kind::Choice ? zx::collection::SettingsRow::Choice
                       : setting.kind == st::Kind::Range ? zx::collection::SettingsRow::Range
                                                         : zx::collection::SettingsRow::Toggle;
            out.label = setting.label;
            out.choices = setting.choices;
            out.minValue = setting.minValue;
            out.maxValue = setting.maxValue;
            out.step = setting.step;
            out.unit = setting.unit;
            out.help = setting.help;
            // The restart note, as the games' Settings page adds it (zge_settings.cpp).
            if (setting.restart)
                out.help += out.help.empty() ? "Applies the next time the game starts." : " Applies at next start.";
            out.current = setting.current;
            out.apply = setting.apply;
            return out;
        }

        // The collection menu's settings page: the app's rows above, as the
        // page draws and changes them (menuRowOf).
        std::vector<zx::collection::SettingsRow> menuSettingsRows(AppState &state)
        {
            const Installed platform = platformInstalled();
            std::vector<zx::collection::SettingsRow> rows;
            if (offered(Need::WindowModes, platform))
                rows.push_back(menuRowOf(windowModeSetting(state)));
            for (const zx::settings::Setting &setting :
                 {scalingSetting(state), vsyncSetting(state), variableRefreshSetting(state), frameInterpolationSetting(state),
                  showFpsSetting(state), pauseInBackgroundSetting(state), volumeSetting(state), muteInBackgroundSetting(state),
                  introMovieSetting(state), menuMusicSetting(state)})
                rows.push_back(menuRowOf(setting));
            if (offered(Need::SecondWindow, platform))
            {
                rows.push_back(menuRowOf(mapWindowSetting(state)));
                rows.push_back(menuRowOf(mapWindowOnTopSetting(state)));
            }
            if (offered(Need::BottomScreen, platform))
                rows.push_back(menuRowOf(cleanTopScreenSetting(state)));
            rows.push_back(menuRowOf(recordPlaySetting(state)));
            rows.push_back(menuRowOf(customFreeplaySetting(state)));
            return rows;
        }

        // The settings.ini settings in the shared settings model
        // (runtime/settings.h): the game's Settings screen and the menu
        // bar both change them through here. Rows of the game's session
        // (zx::session::addSetting): they refer to its runtime and its
        // options, and go when the game is left.
        void registerSettings(AppState &state, PS2Machine &runtime)
        {
            namespace st = zx::settings;
            auto toggle = [&state](const char *key, const char *label, bool Settings::*field, const char *help)
            { return toggleSetting(state, key, label, field, help); };
            // A row shows only where it does something for this setup.
            const Installed installed = setupInstalled();
            auto offers = [&installed](GameOption option) { return g_profile && offered(*g_profile, option, installed); };

            if (offered(Need::WindowModes, installed))
                zx::session::addSetting(windowModeSetting(state));

            st::Setting renderer;
            renderer.key = "display.renderer";
            renderer.label = "Renderer";
            renderer.kind = st::Kind::Choice;
            renderer.choices = {"CPU", "GPU"};
            renderer.help = "CPU is most accurate, GPU sharper.";
            renderer.current = [&state]() { return state.settings.gpuRenderer ? 1 : 0; };
            renderer.apply = [&state, &runtime](int value)
            {
                state.settings.gpuRenderer = value == 1;
                applyRendererSettings(state.settings);
                runtime.requestRasterBackendReload();
                saveSettingsNow(state);
            };
            zx::session::addSetting(renderer);

            st::Setting resolution;
            resolution.key = "display.resolution";
            resolution.label = "Resolution";
            resolution.kind = st::Kind::Choice;
            resolution.choices = {"Native", "2x", "3x", "4x"};
            resolution.help = "640x448 times this.";
            resolution.current = [&state]() { return std::clamp(state.settings.internalResolution, 1, 4) - 1; };
            resolution.apply = [&state, &runtime](int value)
            {
                state.settings.internalResolution = std::clamp(value + 1, 1, 4);
                applyRendererSettings(state.settings);
                if (state.settings.gpuRenderer)
                    runtime.requestRasterBackendReload();
                saveSettingsNow(state);
            };
            zx::session::addSetting(resolution);

            st::Setting fullHeight = toggle("display.full_height", "Full Height", &Settings::fullHeight,
                                            "A sharper picture.");
            fullHeight.apply = [&state, &runtime](int value)
            {
                state.settings.fullHeight = value != 0;
                applyRendererSettings(state.settings);
                if (state.settings.gpuRenderer)
                    runtime.requestRasterBackendReload();
                saveSettingsNow(state);
            };
            zx::session::addSetting(fullHeight);

            st::Setting widescreen = toggle("display.widescreen", "Widescreen", &Settings::widescreen,
                                            "16:9 in levels, 4:3 menus and movies. "
                                            "Plays as at 4:3: what is only at the sides stays still.");
            widescreen.current = [&runtime]() { return runtime.widescreen() ? 1 : 0; };
            widescreen.apply = [&state, &runtime](int value)
            {
                state.settings.widescreen = value != 0;
                runtime.setWidescreen(state.settings.widescreen);
                saveSettingsNow(state);
            };
            if (offers(GameOption::Widescreen))
                zx::session::addSetting(widescreen);

            st::Setting aspect;
            aspect.key = "display.aspect";
            aspect.label = "Wide Aspect";
            aspect.kind = st::Kind::Choice;
            aspect.choices = {"16:9", "16:10", "21:9", "32:9", "Window"};
            aspect.help = "Window matches the window's shape.";
            aspect.current = [&state]() { return std::clamp(state.settings.wideAspect, 0, 4); };
            aspect.apply = [&state, &runtime](int value)
            {
                state.settings.wideAspect = std::clamp(value, 0, 4);
                if (!envSet("ZX_ASPECT"))
                    runtime.setWideAspect(wideAspectValue(state.settings.wideAspect));
                saveSettingsNow(state);
            };
            if (offers(GameOption::Widescreen))
                zx::session::addSetting(aspect);

            st::Setting blur;
            blur.key = "display.motion_blur";
            blur.label = "Motion Blur";
            blur.kind = st::Kind::Choice;
            blur.choices = {"Off", "Low", "Original"};
            blur.help = "The blur trail in levels.";
            blur.current = [&state]() { return std::clamp(state.settings.motionBlur, 0, 2); };
            blur.apply = [&state, &runtime](int value)
            {
                state.settings.motionBlur = std::clamp(value, 0, 2);
                if (!envSet("ZX_MOTION_BLUR"))
                    runtime.setMotionBlur(state.settings.motionBlur);
                saveSettingsNow(state);
            };
            if (offers(GameOption::MotionBlur))
                zx::session::addSetting(blur);

            st::Setting distance;
            distance.key = "display.draw_distance";
            distance.label = "Draw Distance";
            distance.kind = st::Kind::Choice;
            distance.choices = {"Original", "Far", "Farthest"};
            distance.help = "See further before the fog.";
            // It changes the game (games.h drawDistanceChangesGame,
            // every game's): taken at boot only (gameBootConfig), so the
            // whole run is one game.
            distance.restart = true;
            distance.current = [&state]() { return drawDistanceOf(state.settings, *g_profile); };
            distance.apply = [&state](int value)
            {
                state.settings.drawDistance = std::clamp(value, 0, 2);
                saveSettingsNow(state);
            };
            if (offers(GameOption::DrawDistance))
                zx::session::addSetting(distance);

            // Picture quality beyond the PS2 (GPU renderer): anti-aliasing,
            // texture filtering, fog, shadows, depth precision, perspective.
            // Session rows like the others, so the next game booted gets
            // them here again, not before Display Mode.
            zx::gs_quality::addSettings([&state](zx::gs_quality::Option option, int value)
                                          {
                                              state.settings.quality[static_cast<size_t>(option)] = value;
                                              saveSettingsNow(state);
                                          },
                                          true);

            // Only with the Xbox pack made from the player's Xbox disc
            // (ZX_XBOX_PACK, set above).
            if (offered(Need::XboxTextures, installed))
            {
                st::Setting xboxTextures = toggle("display.xbox_textures", "Xbox Textures", &Settings::xboxTextures,
                                                  "Sharper Xbox textures.");
                xboxTextures.current = []() { return zx::texture_replace::enabled() ? 1 : 0; };
                xboxTextures.apply = [&state](int value)
                {
                    state.settings.xboxTextures = value != 0;
                    zx::texture_replace::setEnabled(state.settings.xboxTextures);
                    saveSettingsNow(state);
                };
                zx::session::addSetting(xboxTextures);
            }
            if (offered(Need::XboxSkaters, installed))
            {
                // Read when a skater is loaded (game_overrides/aggressive_inline_disc.cpp).
                st::Setting xboxSkaters = toggle("display.xbox_skaters", "Xbox Skaters", &Settings::xboxSkaters,
                                                 "More detailed skaters, from the next level.");
                zx::session::addSetting(xboxSkaters);
            }
            if (offered(Need::XboxMovies, installed))
            {
                st::Setting xboxMovies = toggle("display.xbox_movies", "Xbox Movies", &Settings::xboxMovies,
                                                "Sharper movies.");
                xboxMovies.apply = [&state](int value)
                {
                    state.settings.xboxMovies = value != 0; // the movie player reads the row at each movie
                    saveSettingsNow(state);
                };
                zx::session::addSetting(xboxMovies);
            }

            // Texture packs' dump mode: for making a pack (docs/SETTINGS.md "Texture packs").
            st::Setting textureDump = toggle("display.texture_dump", "Dump Textures", &Settings::textureDump,
                                             "Save textures to Texture Dumps, for texture packs.");
            textureDump.restart = true;
            zx::session::addSetting(textureDump);

            zx::session::addSetting(scalingSetting(state));
            zx::session::addSetting(vsyncSetting(state));
            zx::session::addSetting(variableRefreshSetting(state));
            zx::session::addSetting(frameInterpolationSetting(state));
            zx::session::addSetting(showFpsSetting(state));
            zx::session::addSetting(pauseInBackgroundSetting(state));
            if (offered(Need::SecondWindow, installed))
            {
                zx::session::addSetting(mapWindowSetting(state));
                zx::session::addSetting(mapWindowOnTopSetting(state));
            }
            if (offered(Need::BottomScreen, installed))
                zx::session::addSetting(cleanTopScreenSetting(state));
            zx::session::addSetting(recordPlaySetting(state));
            zx::session::addSetting(recordingsSetting(state));
            if (kMenu)
                zx::session::addSetting(customFreeplaySetting(state));
            // Restart Game (restartGameSetting): offered in a collection with
            // its menu, outside a playback; on the page only while a change
            // waits for it (restartWaitingFrame).
            g_restartOffered = kMenu && !g_playback.active;
            if (g_restartOffered)
                st::setAfterChanges([]() { restartRowFrame(); });
            const st::Setting volume = volumeSetting(state);
            zx::session::addSetting(volume);
            zx::session::addSetting(muteInBackgroundSetting(state));

            // Movies have no volume of their own in the game.
            zx::audio_mix::setGain(state.settings.movieVolume / 100.0f);
            st::Setting movie = volume;
            movie.key = "audio.movie_volume";
            movie.label = "Movie Volume";
            movie.help = "Intro and menu movies.";
            movie.current = [&state]() { return state.settings.movieVolume; };
            movie.apply = [&state](int value)
            {
                state.settings.movieVolume = std::clamp(value, 0, 100);
                zx::audio_mix::setGain(state.settings.movieVolume / 100.0f);
                saveSettingsNow(state);
            };
            if (offers(GameOption::MovieVolume))
                zx::session::addSetting(movie);

            st::Setting intro = toggle("game.skip_intro_movies", "Skip Intro Movies", &Settings::skipIntroMovies,
                                       "Start at the title screen.");
            intro.restart = true;
            if (offers(GameOption::SkipIntroMovies))
                zx::session::addSetting(intro);

            // Language and French voices: AI's with its Xbox release's files
            // (the US disc's own other string tables only hold the memory
            // card texts); BMX XXX's with its other releases' translations.
            const bool bmxGame = g_profile && std::string_view(g_profile->id) == "bmx";
            if (offers(GameOption::Language))
            {
                st::Setting language;
                language.key = "game.language";
                language.label = "Language";
                language.kind = st::Kind::Choice;
                language.choices = {"English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1" "ol"}; // UTF-8
                language.restart = true;
                language.help = bmxGame ? "All of the game's text."
                                        : "Menus, challenges and tips.";
                language.current = [&state]() { return std::clamp(state.settings.language, 0, 3); };
                language.apply = [&state](int value)
                {
                    state.settings.language = std::clamp(value, 0, 3);
                    // French voices follow the language again.
                    state.settings.frenchVoices = -1;
                    saveSettingsNow(state);
                };
                zx::session::addSetting(language);
            }
            if (offers(GameOption::FrenchVoices))
            {
                // Read by the sound driver at each line (live).
                st::Setting voices = toggle("audio.french_voices", "French Voices", nullptr,
                                            "Speech in French.");
                voices.current = [&state]()
                {
                    return state.settings.frenchVoices < 0 ? (state.settings.language == 1 ? 1 : 0)
                                                           : state.settings.frenchVoices;
                };
                voices.apply = [&state](int value)
                {
                    state.settings.frenchVoices = value != 0 ? 1 : 0;
                    saveSettingsNow(state);
                };
                zx::session::addSetting(voices);
            }

            st::Setting developer = toggle("game.developer_menu", "Developer Menu", &Settings::developerMenu,
                                           "Press F1 in a level.");
            developer.restart = true;
            if (offers(GameOption::DeveloperMenu))
                zx::session::addSetting(developer);

            st::Setting zoo = toggle("game.zoo", "Zoo Level", &Settings::zoo,
                                     "The cut level, in Free Skate and Timed Run.");
            zoo.restart = true;
            if (offers(GameOption::Zoo))
                zx::session::addSetting(zoo);
            // With the Estate made from the GameCube disc.
            if (offers(GameOption::Estate))
            {
                st::Setting estate = toggle("game.estate", "Estate Level", &Settings::estate,
                                            "An early Tutorial, in Free Skate.");
                estate.restart = true;
                zx::session::addSetting(estate);
            }
            // Dave Mirra BMX 2: with what its Xbox disc gave.
            if (offers(GameOption::Dm2XboxLevels))
            {
                st::Setting xbox = toggle("game.dm2_xbox_levels", "Xbox Levels", &Settings::dm2XboxLevels,
                                          "Venice and Greenville, with their goals.");
                xbox.restart = true;
                zx::session::addSetting(xbox);
            }
            if (offers(GameOption::Dm2XboxThemes))
            {
                st::Setting themes = toggle("game.xbox_themes", "Desert and Highway Themes", &Settings::dm2XboxThemes,
                                            "Two unused park editor themes.");
                themes.restart = true;
                zx::session::addSetting(themes);
            }
            if (offers(GameOption::Dm2RetroMovie))
            {
                st::Setting retro = toggle("game.retro_movie", "Retro Movie", &Settings::dm2RetroMovie,
                                           "Adds Retro to the movie gallery.");
                retro.restart = true;
                zx::session::addSetting(retro);
            }
            // DM2: the attract demo at the title screen (the Xbox's and GameCube's).
            if (offers(GameOption::Dm2AttractDemo))
            {
                st::Setting demo = toggle("game.attract_demo", "Attract Demo", &Settings::dm2AttractDemo,
                                          "Replays play at an idle title screen.");
                demo.restart = true;
                zx::session::addSetting(demo);
            }
            // DM2: its cut Create Player menu, from the main menu (with the
            // readable newRider, which makes a created rider's model).
            if (offers(GameOption::Dm2CreatePlayer))
            {
                st::Setting create = toggle("game.create_player", "Create Rider", &Settings::dm2CreatePlayer,
                                            "Adds CREATE RIDER to the main menu.");
                create.restart = true;
                zx::session::addSetting(create);
            }
            // BMX XXX: its park editor, from the main menu.
            if (offers(GameOption::BmxParkEditor))
            {
                st::Setting park = toggle("game.park_editor", "Park Editor", &Settings::bmxParkEditor,
                                          "Adds PARK EDITOR to the main menu.");
                park.restart = true;
                zx::session::addSetting(park);
            }
            // BMX XXX: its Xbox and GameCube releases' content, each with the
            // disc that gave it (the intro movie is the PS2 disc's own).
            if (offers(GameOption::BmxXboxChallenges))
            {
                st::Setting challenges = toggle("game.xbox_challenges", "Xbox Challenges", &Settings::bmxXboxChallenges,
                                                "Fixed challenges in four levels.");
                challenges.restart = true;
                zx::session::addSetting(challenges);
            }
            if (offers(GameOption::BmxEditorPieces))
            {
                st::Setting pieces = toggle("game.editor_pieces", "Pool and Trash Can Pieces", &Settings::bmxEditorPieces,
                                            "Two more pieces, kept for parks using them.");
                pieces.restart = true;
                zx::session::addSetting(pieces);
            }
            if (offers(GameOption::BmxLevelPreviews))
            {
                st::Setting previews = toggle("game.level_previews", "Level Previews", &Settings::bmxLevelPreviews,
                                              "Preview movies in the level select.");
                previews.restart = true;
                zx::session::addSetting(previews);
            }
            if (offers(GameOption::BmxLogoMovie))
            {
                st::Setting logo = toggle("game.logo_movie", "Z-Axis Logo Movie", &Settings::bmxLogoMovie,
                                          "Adds it to the movie gallery.");
                logo.restart = true;
                zx::session::addSetting(logo);
            }
            if (offers(GameOption::BmxGcLevels))
            {
                st::Setting levels = toggle("game.gc_levels", "GameCube Levels", &Settings::bmxGcLevels,
                                            "The Secret Level and three test levels.");
                levels.restart = true;
                zx::session::addSetting(levels);
            }
            if (offers(GameOption::BmxLogoSplash))
            {
                st::Setting splash = toggle("game.logo_splash", "Z-Axis Logo at Start", &Settings::bmxLogoSplash,
                                            "The GameCube's logo movie at start.");
                splash.restart = true;
                zx::session::addSetting(splash);
            }
            if (offers(GameOption::BmxTrailers))
            {
                st::Setting trailers = toggle("game.trailers", "Turok and Vexx Trailers", &Settings::bmxTrailers,
                                              "The Xbox's two trailers in Extras.");
                trailers.restart = true;
                zx::session::addSetting(trailers);
            }
            if (offers(GameOption::BmxCarTextures))
            {
                st::Setting car = toggle("game.car_textures", "Create-a-Rider Extras", &Settings::bmxCarTextures,
                                         "The other releases' hats, tops and more.");
                car.restart = true;
                zx::session::addSetting(car);
            }
            if (offers(GameOption::BmxGreenSkin))
            {
                st::Setting green = toggle("game.green_skin", "Green Skin", &Settings::bmxGreenSkin,
                                           "Green skins in create-a-rider.");
                green.restart = true;
                zx::session::addSetting(green);
            }
            if (offers(GameOption::BmxTopless))
            {
                st::Setting topless = toggle("game.topless_girl", "Topless Girl", &Settings::bmxTopless,
                                             "The topless top in create-a-rider.");
                topless.restart = true;
                zx::session::addSetting(topless);
            }
            if (offers(GameOption::BmxXboxSongs))
            {
                st::Setting songs = toggle("game.xbox_songs", "Xbox Songs", &Settings::bmxXboxSongs,
                                           "Eight unused songs in the soundtrack.");
                songs.restart = true;
                zx::session::addSetting(songs);
            }
            if (offers(GameOption::BmxMenuLines))
            {
                st::Setting lines = toggle("game.menu_lines", "Unused Menu Lines", &Settings::bmxMenuLines,
                                           "Hidden lines back in the game's menus.");
                lines.restart = true;
                zx::session::addSetting(lines);
            }
            if (offers(GameOption::BmxAllMpModes))
            {
                st::Setting modes = toggle("game.all_mp_modes", "All Multiplayer Modes", &Settings::bmxAllMpModes,
                                           "Every mode that works, in SELECT GAME.");
                modes.restart = true;
                zx::session::addSetting(modes);
            }
            if (offers(GameOption::BmxIntroMovie))
            {
                st::Setting introMovie = toggle("game.intro_movie", "Intro Movie", &Settings::bmxIntroMovie,
                                                "Plays the intro movie at start.");
                introMovie.restart = true;
                zx::session::addSetting(introMovie);
            }
            if (offers(GameOption::ZooUnlock))
            {
                st::Setting unlock;
                unlock.key = "game.zoo_unlock";
                unlock.label = "Zoo Unlock";
                unlock.kind = st::Kind::Choice;
                unlock.choices = {"Museum", "Always"};
                unlock.restart = true;
                unlock.help = "The Museum's 8th challenge opens it.";
                unlock.current = [&state]() { return std::clamp(state.settings.zooUnlock, 0, 1); };
                unlock.apply = [&state](int value)
                {
                    state.settings.zooUnlock = std::clamp(value, 0, 1);
                    saveSettingsNow(state);
                };
                zx::session::addSetting(unlock);
            }

            if (offers(GameOption::Tuning))
            {
                st::Setting tuning;
                tuning.key = "game.tuning";
                tuning.label = "Gameplay Tuning";
                tuning.kind = st::Kind::Choice;
                tuning.choices = {"PS2", "Xbox"};
                tuning.restart = true;
                tuning.help = bmxGame ? "Syracuse's timed challenges as on Xbox."
                                      : "Special trick XP and key loot as on Xbox.";
                tuning.current = [&state]() { return std::clamp(state.settings.tuning, 0, 1); };
                tuning.apply = [&state](int value)
                {
                    state.settings.tuning = std::clamp(value, 0, 1);
                    saveSettingsNow(state);
                };
                zx::session::addSetting(tuning);
            }
            if (offers(GameOption::CareerScripts))
            {
                st::Setting scripts;
                scripts.key = "game.career_scripts";
                scripts.label = "Career Scripts";
                scripts.kind = st::Kind::Choice;
                scripts.choices = {"PS2", "Xbox"};
                scripts.restart = true;
                scripts.help = "Xbox fixes to some challenges.";
                scripts.current = [&state]() { return std::clamp(state.settings.careerScripts, 0, 1); };
                scripts.apply = [&state](int value)
                {
                    state.settings.careerScripts = std::clamp(value, 0, 1);
                    saveSettingsNow(state);
                };
                zx::session::addSetting(scripts);
            }

            st::Setting ape = toggle("game.ape", "Ape Skater", &Settings::ape,
                                     "Unlocked with the Mummy.");
            ape.restart = true;
            if (offers(GameOption::Ape))
                zx::session::addSetting(ape);

            st::Setting freeSkate = toggle("game.xbox_free_skate", "Xbox Free Skate", &Settings::xboxFreeSkate,
                                           "Doors and rides in Boardwalk and Cannery.");
            freeSkate.restart = true;
            if (offers(GameOption::XboxFreeSkate))
                zx::session::addSetting(freeSkate);
        }

        // Native save files (the game override) unless ZX_SAVES=card.
        bool nativeSaves()
        {
            const char *mode = zx::boot::text("ZX_SAVES");
            return !(mode && std::string(mode) == "card");
        }

        // The native saves' folder and the save file the game override uses
        // for this profile.
        std::filesystem::path nativeSavesFolder()
        {
            const char *dir = zx::boot::text("ZX_SAVES_DIR");
            return std::filesystem::path(dir ? dir : "");
        }
        std::filesystem::path nativeSaveFile()
        {
            const char *profile = zx::boot::text("ZX_SAVE_PROFILE");
            return nativeSavesFolder() / (zx::saves::safeFileName(profile ? profile : "", "Career") + ".sav");
        }

        // Offers once to copy the game's save from a PCSX2 card: into the
        // native save file, or into card slot 1 with ZX_SAVES=card. The
        // game's other save folders on the card (the profile's saveDir after
        // the game save's, comma-separated: DM2's park and replay libraries)
        // come with it: natively into <saves>/PCSX2 card (kPcsx2CardFolder),
        // where the game's own import brings their saves in and never replaces
        // one; with ZX_SAVES=card into card slot 1 when it has none.
        void offerPcsx2Import(AppState &state)
        {
            std::string saveEntry = g_game->saveDir;
            if (saveEntry.empty())
                return;
            std::vector<std::string> otherFolders;
            for (size_t comma; (comma = saveEntry.rfind(',')) != std::string::npos; saveEntry.resize(comma))
                otherFolders.insert(otherFolders.begin(), saveEntry.substr(comma + 1));
            const size_t slash = saveEntry.find('/');
            const std::string saveDir = saveEntry.substr(0, slash);
            const std::string dataFile = slash == std::string::npos ? std::string() : saveEntry.substr(slash + 1);
            const bool native = nativeSaves();
            const std::filesystem::path saveFile = nativeSaveFile();
            std::error_code ec;
            // A save in slot 1 counts too: the game copies it to the native
            // file the first time it runs.
            const bool haveSave = std::filesystem::exists(state.paths.memcardSlot1 / saveDir, ec) ||
                                  (native && std::filesystem::exists(saveFile, ec));
            auto import = [&](const std::filesystem::path &card, bool overwrite, std::string &error)
            {
                const bool imported = native ? importCardSaveToFile(card, saveDir, saveFile, overwrite, &error, dataFile)
                                             : importCardSave(card, saveDir, state.paths.memcardSlot1, overwrite, &error);
                if (!imported || otherFolders.empty())
                    return imported;
                std::string why;
                const std::vector<std::string> copied =
                    native ? importCardFolders(card, otherFolders, nativeSavesFolder() / zx::saves::kPcsx2CardFolder, true, &why)
                           : importCardFolders(card, otherFolders, state.paths.memcardSlot1, false, &why);
                for (const std::string &folder : copied)
                    std::cout << "[app] import " << card.string() << ": " << folder << " too" << std::endl;
                if (!why.empty())
                    std::cerr << "[app] import " << card.string() << ": " << why << std::endl;
                return imported;
            };

            // ZX_APP_IMPORT_CARD=<card.ps2>: import without asking (automation).
            if (const char *card = std::getenv("ZX_APP_IMPORT_CARD"); card && card[0])
            {
                std::string error;
                const bool ok = import(card, true, error);
                std::cout << "[app] import " << card << ": " << (ok ? "done" : error) << std::endl;
                return;
            }
            if (state.settings.pcsx2ImportOffered || haveSave)
                return;
            const std::vector<std::filesystem::path> cards = findPcsx2CardsWithSave(saveDir, getEnv);
            if (cards.empty())
                return;

            state.settings.pcsx2ImportOffered = true;
            saveSettingsNow(state);
            const std::filesystem::path &card = cards.front();
            if (!platform::ask("Import your PCSX2 save?",
                               "A " + std::string(g_game->name) + " save was found on the PCSX2 memory card \"" + card.filename().string() +
                                   "\". Your PCSX2 card stays as it is.",
                               "Import", "Not Now"))
                return;
            std::string error;
            if (import(card, false, error))
                std::cout << "[app] imported the save from " << card.string() << std::endl;
            else
                platform::showMessage("The save could not be imported", error, false);
        }

        // The folder the app is in: the one holding the .app bundle on macOS,
        // elsewhere the executable's.
        std::filesystem::path appFolder(const char *argv0)
        {
            const std::filesystem::path resources = platform::resourceFolder(argv0);
            const std::filesystem::path contents = resources.parent_path();
            if (resources.filename() == "Resources" && contents.filename() == "Contents" &&
                contents.parent_path().extension() == ".app")
                return contents.parent_path().parent_path();
            return resources;
        }

        // Portable mode: a portable.txt next to the app keeps settings,
        // saves, mods, logs and screenshots in "<app> Data" beside it
        // instead of the user's folders (unless ZX_APP_SUPPORT_DIR says).
        void applyPortableMode(const char *argv0)
        {
            if (envSet("ZX_APP_SUPPORT_DIR"))
                return;
            const std::filesystem::path folder = appFolder(argv0);
            std::error_code ec;
            if (!folder.empty() && std::filesystem::is_regular_file(folder / "portable.txt", ec))
                g_portableSupportDir = (folder / (std::string(ZX_APP_NAME) + " Data")).string();
        }

        // Screenshots: Pictures/<app> in the user's home, or Screenshots in
        // the support folder when that is set from outside (portable mode,
        // automated runs).
        std::filesystem::path screenshotFolder(const AppPaths &paths)
        {
            if (envSet("ZX_APP_SUPPORT_DIR"))
                return paths.supportDir / "Screenshots";
#if defined(_WIN32)
            const char *home = getEnv("USERPROFILE");
#else
            const char *home = getEnv("HOME");
#endif
            if (!home || !home[0])
                return paths.supportDir / "Screenshots";
            if (kCollection)
                return std::filesystem::path(home) / "Pictures" / ZX_APP_NAME / g_game->name;
            return std::filesystem::path(home) / "Pictures" / ZX_APP_NAME;
        }

        // The game's code: linked into the app (a workshop build), else the module made from this disc on this
        // machine (collection/game_code.h): kept in the game's folder (code/), made now when there is none for this
        // disc and app (the setup makes it first: appGameCodeStep; this is for a game set up before).
        bool ensureGameCode(const AppState &state, const GameProfile &game, const std::filesystem::path &discDir,
                            const DiscCheck &disc, const std::filesystem::path &toolsFolder, std::string &error)
        {
            if (zx::game_module::hasCodeFor(disc.bootElf))
                return true;
            zx::app::GameCodeTools tools;
            if (!tools.open(toolsFolder, &error))
                return false;
            zx::app::GameCodeRequest request;
            request.elf = discDir / disc.bootElf;
            request.id = zx::app::gameCodeGame(tools, game.id, request.elf);
            request.cache = zx::app::gameCodeCache(state.paths.supportDir);
            const std::filesystem::path file = zx::app::gameCodeFile(tools, request);
            std::error_code ec;
            if (!std::filesystem::exists(file, ec))
            {
                std::filesystem::create_directories(request.cache, ec);
                std::cout << "[app] game code: making " << file.string() << " from " << request.elf.string() << std::endl;
                const auto t0 = std::chrono::steady_clock::now();
                std::string last;
                const bool made = zx::app::makeGameCode(tools, request, [&last](const zx::app::GameCodeProgress &p)
                {
                    const std::string line = std::string(p.step) + " " + std::to_string(p.total ? p.done * 100 / p.total : 0) + "%";
                    if (line != last && (p.done == 0 || p.done == p.total || p.done % 50 == 0))
                        std::cout << "[app] game code: " << line << " (" << p.done << "/" << p.total << ")" << std::endl;
                    last = line;
                    return true;
                }, &error);
                if (!made)
                    return false;
                std::cout << "[app] game code: made in " << std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
            }
            if (!zx::game_module::load(file.string(), error))
                return false;
            std::cout << "[app] game code: " << file.string() << std::endl;
            return true;
        }

        // The game's bundled files: Resources, or Resources/games/<id> in a
        // collection.
        std::filesystem::path gameResources(const char *argv0, const GameProfile &game)
        {
            if (kCollection)
                return platform::resourceFolder(argv0) / "games" / game.id;
            return platform::resourceFolder(argv0);
        }

        // The app's support folder (runApp sets it): each game's files from
        // the player's own disc go in its game folder there.
        std::filesystem::path g_supportRoot;

        // A game's folder in the support folder: <support>/games/<id> in a
        // collection, the support folder itself for a single game.
        std::filesystem::path gameFolder(const GameProfile &game)
        {
            return kCollection ? g_supportRoot / "games" / game.id : g_supportRoot;
        }

        // The game as the setup (collection/game_setup.h) knows it.
        SetupGame setupGame(const GameProfile &game)
        {
            return {kCollection ? game.id : "game", game.title, game.elf, game.elfSha256};
        }

        // The game's files: ZX_APP_DISC_DIR, else those set up from the
        // player's disc (<game folder>/disc), else those bundled with the
        // app when it was built (platform/mac/make-app.sh: Resources/disc, or
        // Resources/games/<id>/disc in a collection).
        std::filesystem::path discFolder(const char *argv0, const GameProfile &game)
        {
            if (const char *forced = std::getenv("ZX_APP_DISC_DIR"); forced && forced[0])
                return forced;
            std::error_code ec;
            if (!g_supportRoot.empty())
                if (const std::filesystem::path installed = installedDiscFolder(gameFolder(game));
                    std::filesystem::is_regular_file(installed / "SYSTEM.CNF", ec))
                    return installed;
            return gameResources(argv0, game) / "disc";
        }

        // Whether the game has files to run from (set up, bundled or forced).
        bool gameHasFiles(const char *argv0, const GameProfile &game)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(discFolder(argv0, game) / "SYSTEM.CNF", ec);
        }
    }

    bool startedAsApp(int argc, char *argv[])
    {
        if (argc < 2 || !argv[1] || argv[1][0] == '\0')
            return true;
        const std::string arg = argv[1];
        if (kCollection && (arg == "--game" || arg == "--play"))
            return true;
        // The setup from the command line (setupCommand), and a game's code made as the first run makes it
        // (makeGameCodeCommand).
        if (arg == "--setup" || arg == "--check-games" || arg == "--remove-game" || arg == "--make-game-code")
            return true;
        return arg.rfind("-psn_", 0) == 0; // old Finder launches
    }

    namespace
    {
        // --game <id> or ZX_APP_GAME (developers, checks): the game to
        // boot without the menu, by its index in kGamesInOrder; -1: none (the
        // menu). A single-game app always boots its game.
        int developerGame(int argc, char *argv[])
        {
            if (kGameCount == 1)
                return 0;
            std::string wanted;
            for (int i = 1; i + 1 < argc; ++i)
                if (std::string(argv[i]) == "--game")
                    wanted = argv[i + 1];
            if (wanted.empty())
                if (const char *env = getEnv("ZX_APP_GAME"); env && env[0])
                    wanted = env;
            if (wanted.empty())
                return -1;
            for (size_t i = 0; i < kGameCount; ++i)
                if (lowerAscii(kGamesInOrder[i].id) == lowerAscii(wanted))
                    return static_cast<int>(i);
            std::cerr << "[app] no game \"" << wanted << "\" in this app" << std::endl;
            return -1;
        }

        // --map <folder> or ZX_APP_MAP (developers, the playbot's rides),
        // with --game: Custom Freeplay on the maps folder's map of that
        // folder name, no menu first; "" none.
        std::string developerMap(int argc, char *argv[])
        {
            for (int i = 1; i + 1 < argc; ++i)
                if (std::string(argv[i]) == "--map")
                    return argv[i + 1];
            if (const char *env = getEnv("ZX_APP_MAP"); env && env[0])
                return env;
            return {};
        }

        // --play <file.zrec> or ZX_APP_PLAY (developers, checks): the
        // recording played back as its row on the Recordings page plays it,
        // no menu first; "" none.
        std::string developerPlayback(int argc, char *argv[])
        {
            for (int i = 1; i + 1 < argc; ++i)
                if (std::string(argv[i]) == "--play")
                    return argv[i + 1];
            if (const char *env = getEnv("ZX_APP_PLAY"); env && env[0])
                return env;
            return {};
        }

        // The game whose files take over the collection's shared ones of an
        // earlier version (migrateCollectionFiles): Aggressive Inline, whose
        // actions were every game's default controls.
        constexpr const char *kKeeperGame = "ai";

        // The game's folder in a collection: <support>/games/<id>, logs in
        // <logs>/<id>. Its single-game app's folder, if there is one and the
        // game's folder is new, is copied in once (settings, controls, saves,
        // cards, mods; not logs): that app keeps working with its own. The
        // controls of a game other than Aggressive Inline are not copied:
        // that app wrote AI's actions as its defaults, so the game starts
        // from its own.
        AppPaths gamePaths(const AppPaths &collection, const GameProfile &game)
        {
            AppPaths paths = collection;
            const std::filesystem::path dir = collection.supportDir / "games" / game.id;
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec))
            {
                const AppPaths legacy = resolveAppPaths(game.name, getEnv);
                if (!envSet("ZX_APP_SUPPORT_DIR") && std::filesystem::is_directory(legacy.supportDir, ec))
                {
                    std::filesystem::create_directories(dir, ec);
                    for (const auto &entry : std::filesystem::directory_iterator(legacy.supportDir, ec))
                    {
                        if (entry.path().filename() == "Logs" ||
                            (entry.path().filename() == "controls.ini" && std::string(game.id) != kKeeperGame))
                            continue;
                        std::filesystem::copy(entry.path(), dir / entry.path().filename(),
                                              std::filesystem::copy_options::recursive, ec);
                    }
                    std::cout << "[app] copied " << legacy.supportDir.string() << " -> " << dir.string()
                              << (ec ? " (incomplete: " + ec.message() + ")" : std::string()) << std::endl;
                }
            }
            paths.supportDir = dir;
            paths.settingsFile = dir / "settings.ini";
            paths.controlsFile = dir / "controls.ini";
            paths.memcardSlot1 = dir / "memcards" / "slot1";
            paths.memcardSlot2 = dir / "memcards" / "slot2";
            paths.savesDir = dir / "Saves";
            paths.hostRoot = dir / "host";
            paths.logDir = collection.logDir / game.id;
            return paths;
        }
    }


    namespace
    {
        namespace col = zx::collection;

        const char *g_argv0 = nullptr;

        // The app's tools for making the game code: Resources/game-code-tools, or ZX_GAME_CODE_TOOLS.
        std::filesystem::path gameCodeToolsFolder()
        {
            const char *forced = std::getenv("ZX_GAME_CODE_TOOLS");
            return forced && forced[0] ? std::filesystem::path(forced) : platform::resourceFolder(g_argv0) / "game-code-tools";
        }

        // The setup's code step (collection/game_setup.h): the game's code made from the installed disc when the app
        // has the tools for it (the compile shown on the setup's screens), else the code built into the app.
        std::unique_ptr<GameCodeStep> appGameCodeStep()
        {
            const std::filesystem::path tools = gameCodeToolsFolder();
            if (zx::app::GameCodeTools probe; probe.open(tools, nullptr))
                return std::make_unique<zx::app::BuiltGameCode>(tools);
            return makeGameCodeStep();
        }

        // An update's code step for a game whose code is not ready (pendingUpdates): its code made with the update's
        // progress in the game's row (job.reporter()). False with `why` when it could not be made.
        struct SetupJob;
        bool updateGameCode(SetupJob &job, const GameProfile &game, std::string &why);
        // The runtime of the game running, nullptr on the menu: the menu
        // bar's items act on it.
        PS2Machine *g_runtime = nullptr;

        // The host's window: none for a null-GS or ZX_HEADLESS run (as
        // PS2Machine::initialize decides when it makes the host itself).
        bool headlessHost()
        {
            const char *headless = std::getenv("ZX_HEADLESS");
            return headless && headless[0] ? std::strcmp(headless, "0") != 0 : gsNullRequested();
        }

        // Before the window is made: the Windows icon, SDL's own messages
        // (ZX_SDL_LOG=1: video, audio, GPU drivers).
        void prepareHost()
        {
#if defined(_WIN32) && defined(ZX_APP_HAS_ICON)
            // The executable's icon resource for the window too.
            SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
            SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
            SDL_SetLogPriorities(envSet("ZX_SDL_LOG") ? SDL_LOG_PRIORITY_VERBOSE : SDL_LOG_PRIORITY_WARN);
        }

        // The menu bar (View, File): its items act on the game running, if
        // any. Installed once, when the window is there.
        void installAppMenus(AppState &state, const AppPaths &folders)
        {
            platform::MenuActions actions;
            actions.integerScaling = [&state]()
            { return state.settings.scaling == Scaling::Integer; };
            actions.toggleIntegerScaling = [&state]()
            { zx::settings::set("display.scaling", state.settings.scaling == Scaling::Integer ? 0 : 1); };
            // The collection's item is there for every game, greyed out where
            // the game has no widescreen.
            if (kMenu || gameHas(GameOption::Widescreen))
            {
                actions.widescreen = []()
                { return g_runtime && g_runtime->widescreen(); };
                actions.toggleWidescreen = []()
                {
                    if (g_runtime && gameHas(GameOption::Widescreen))
                        zx::settings::set("display.widescreen", g_runtime->widescreen() ? 0 : 1);
                };
                actions.widescreenAvailable = []()
                { return g_runtime && gameHas(GameOption::Widescreen); };
            }
            actions.pauseInBackground = [&state]()
            { return state.settings.pauseOnFocusLoss; };
            actions.togglePauseInBackground = [&state]()
            { zx::settings::set("display.pause_in_background", state.settings.pauseOnFocusLoss ? 0 : 1); };
            // The row's own apply: it works on the menu too, where the
            // registry has no rows yet.
            actions.mapWindow = [&state]() { return state.settings.mapWindow; };
            actions.toggleMapWindow = [&state]() { mapWindowSetting(state).apply(state.settings.mapWindow ? 0 : 1); };
            actions.gpuRenderer = [&state]()
            { return state.settings.gpuRenderer; };
            actions.internalResolution = [&state]()
            { return state.settings.internalResolution; };
            // Switched while running: the GS renderer is made again between two
            // frames.
            actions.setRenderer = [](bool gpu, int resolution)
            {
                zx::settings::set("display.resolution", std::clamp(resolution, 1, 4) - 1);
                zx::settings::set("display.renderer", gpu ? 1 : 0);
            };
            actions.settingsFolder = folders.supportDir;
            actions.logsFolder = folders.logDir;
            if (kMenu)
            {
                // Back to the collection's menu.
                actions.switchGame = []() { ps2_host::requestExitToMenu(); };
                actions.switchGameAvailable = []() { return g_runtime != nullptr; };
            }
            platform::installMenus(actions);
            // The game's own Settings page in a game; on the collection's
            // menu its Settings page. Greyed out in a game that shows no
            // settings in its own menus.
            platform::setOpenSettingsAction(
                []()
                {
                    if (kMenu && !g_runtime)
                        g_menuSettingsWanted = true;
                    else
                        zx::settings::requestOpen();
                },
                []() { return (kMenu && !g_runtime) || zx::settings::hasOpenHandler(); });
        }

        // How a boot ended, and the app's exit status where it ends the app.
        struct BootResult
        {
            col::GameEnd end = col::GameEnd::Quit;
            int status = 0;
            std::string why; // a boot that failed: what to tell the player
        };

        // A converted custom map's files as the boot's memory folder
        // (runtime/game_files.h), for the boot's life: every file system
        // of the game reads them over its mods, and nothing is written.
        class ConvertedMapFiles
        {
        public:
            explicit ConvertedMapFiles(ConvertedMap *map)
            {
                if (!map)
                    return;
                zx::game_files::MemoryFolder files;
                for (auto &[path, bytes] : map->files)
                    files[path] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
                map->files.clear();
                zx::game_files::registerMemoryFolder(kConvertedMapFolder, std::move(files));
                m_registered = true;
            }
            ~ConvertedMapFiles()
            {
                if (m_registered)
                    zx::game_files::removeMemoryFolder(kConvertedMapFolder);
            }
            ConvertedMapFiles(const ConvertedMapFiles &) = delete;
            ConvertedMapFiles &operator=(const ConvertedMapFiles &) = delete;

        private:
            bool m_registered = false;
        };

        // The bottom screen's level file for a custom map (collection/maps.h),
        // made from the files the boot reads: the map's (converted, or its
        // folder's level files in the slot) over the game's PS2 disc, served
        // under the slot's panel name for the boot's life
        // (runtime/panel_snapshot.h setServedLevelFile). None for a DM2 park
        // (its level is built by the game) or when it cannot be made.
        class MapPanelFile
        {
        public:
            MapPanelFile(const GameProfile &game, const CustomMap *custom, const MapSlot *slot, const ConvertedMap *converted,
                         const std::filesystem::path &disc)
            {
                namespace ex = zx::app::extras;
                if (!custom || !slot || slot->park || !slot->panelLevel[0])
                    return;
                const CustomMap &map = *custom;
                const auto start = std::chrono::steady_clock::now();
                ex::FolderFiles discFiles;
                if (!discFiles.open(disc, {"ASSETS"}))
                    return;
                std::map<std::string, ex::Bytes> over;
                const std::string slotFolder = std::string(slot->folder) + "/" + slot->slot + "/";
                if (converted)
                    over = converted->files;
                else
                    for (const std::string &path : zx::game_files::listFilesBelow(map.folder))
                        if (lowerAscii(path).rfind(lowerAscii(slotFolder), 0) == 0)
                            if (ex::Bytes bytes; zx::game_files::readFileBelow(map.folder, path, bytes))
                                over[path] = std::move(bytes);
                const ex::LayeredFiles files(discFiles, std::move(over));
                const ex::PanelGame which = game.id == std::string("ai")    ? ex::PanelGame::AggressiveInline
                                            : game.id == std::string("dm2") ? ex::PanelGame::DaveMirra2
                                                                            : ex::PanelGame::BmxXxx;
                auto bytes = std::make_shared<ex::Bytes>();
                std::vector<std::string> log;
                std::string why;
                if (!ex::makeMapPanelFile(which, files, slot->slot, map.name, *bytes, log, &why))
                {
                    std::cout << "[app] custom freeplay: " << why << std::endl;
                    return;
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::cout << "[app] custom freeplay: the bottom screen's map made as " << slot->panelLevel << ".pmap in "
                          << static_cast<long long>(ms) << " ms (" << bytes->size() << " bytes)" << std::endl;
                for (const std::string &line : log)
                    std::cout << "[app] custom freeplay:   " << line << std::endl;
                panel::setServedLevelFile(slot->panelLevel, std::move(bytes));
                m_served = true;
            }
            ~MapPanelFile()
            {
                if (m_served)
                    panel::setServedLevelFile("", nullptr);
            }
            MapPanelFile(const MapPanelFile &) = delete;
            MapPanelFile &operator=(const MapPanelFile &) = delete;

        private:
            bool m_served = false;
        };

        // What a playback boots (a Recordings page's row): the recording and
        // what it says of its boot.
        struct PlaybackBoot
        {
            zx::collection::RecordingEntry entry;
            play_recording::Summary summary;
        };

        // The boot's vsyncs, its first and its last (game thread, at each
        // VBlank): the game's own running time, the save profile's time
        // played (collection/save_profiles.h). A pause of the app's stops
        // them; the game's pause menu does not.
        std::atomic<uint64_t> g_firstVsync{UINT64_MAX}, g_lastVsync{0};
        void countVsync(uint64_t tick)
        {
            uint64_t none = UINT64_MAX;
            g_firstVsync.compare_exchange_strong(none, tick);
            g_lastVsync.store(tick);
        }

        // While the player's play is recorded: the level the game is in, as
        // an event of the recording whenever it changes (the Recordings
        // page's level), from the panel's snapshot (read-only) every half
        // second. Game thread, at each VBlank. The first check always notes
        // where the game is ("" in its menus): none noted yet is not "".
        std::optional<std::string> g_levelNoted;
        void noteLevel(const uint8_t *rdram, uint64_t tick)
        {
            constexpr uint64_t kEvery = 30; // vsyncs
            if (!rdram || tick % kEvery != 0u || play_recording::mode() != play_recording::Mode::Record)
                return;
            static panel::Snapshot captured;
            panel::captureSnapshot(rdram, MapWindow::kGuestRamBytes, captured);
            const std::string level =
                captured.mode == panel::GameMode::NoLevel
                    ? std::string()
                    : std::string(captured.levelFile, strnlen(captured.levelFile, sizeof(captured.levelFile)));
            if (g_levelNoted && *g_levelNoted == level)
                return;
            g_levelNoted = level;
            play_recording::noteEvent("level", level);
        }

        namespace ex = zx::app::extras;
        std::string remakeExtras(const GameProfile &game, std::vector<ex::ExtrasSource> added, const SetupProgressFn &progress,
                                 bool &ok);

        // One game's boot, from its folders to its runtime destroyed: its
        // settings and BootConfig, the runtime on the app's host, its
        // settings rows (the session's), the disc check, the ELF, the run,
        // its shutdown hooks. `settingsLoaded`: the app loaded the game's
        // settings already (a single-game app, which reads them first for
        // its log). `customMap`: Custom Freeplay's map; `converted`: its
        // files made into this game's (a map made in another game), given
        // to the boot in memory and freed when it ends. `playback`: a
        // recording played back (collection/recordings.h).
        BootResult bootGame(AppState &state, const AppPaths &appPaths, const GameProfile &game,
                            const std::string &gamepadMappings, bool settingsLoaded, const CustomMap *customMap = nullptr,
                            ConvertedMap *converted = nullptr, const PlaybackBoot *playback = nullptr)
        {
            BootResult result;
            const auto bootStart = std::chrono::steady_clock::now();
            g_game = &game;
            state.paths = kCollection ? gamePaths(appPaths, game) : appPaths;
            g_profile = &gameSettingsProfile(kCollection ? g_game->id : g_game->elf);
            std::error_code ec;
            for (const auto &dir : {state.paths.supportDir, state.paths.memcardSlot1, state.paths.memcardSlot2, state.paths.hostRoot})
                std::filesystem::create_directories(dir, ec);
            if (!settingsLoaded)
            {
                // The game's options and the app's settings as their files
                // have them; the window's place and full screen as they are
                // now (kept for the next start only when the app quits).
                const Settings window = state.settings;
                std::vector<std::string> warnings;
                const bool hadSettings = loadAppSettings(state, state.settings, &warnings);
                state.settings.windowWidth = window.windowWidth;
                state.settings.windowHeight = window.windowHeight;
                state.settings.fullscreen = window.fullscreen;
                state.settings.fullscreenMode = window.fullscreenMode;
                std::cout << "[app] game: " << g_game->id << " (" << g_game->title << ")\n"
                          << "[app] support folder: " << state.paths.supportDir.string() << "\n"
                          << "[app] settings: " << state.paths.settingsFile.string() << (hadSettings ? "" : " (new)")
                          << std::endl;
                for (const std::string &warning : warnings)
                    std::cout << "[app] settings.ini " << warning << std::endl;
                if (!hadSettings)
                    saveSettingsNow(state);
            }

            // The extras' parts an update left to make (collection/extras/setup.h): the menu's update makes them
            // first; a game started with no menu (--game) has them made here.
            if (kCollection && !ex::staleExtras(game.id, gameFolder(game)).empty())
            {
                bool extrasOk = false;
                std::cout << "[update] " << game.id << ": " << remakeExtras(game, {}, {}, extrasOk) << std::endl;
            }

            // Relative paths of the debug switches (dumps, traces) land here.
            std::filesystem::current_path(state.paths.supportDir, ec);

            // What the game is given: its BootConfig (runtime/boot_config.h),
            // built for this boot from the settings and the game's folders. The
            // environment still wins where it sets a name (developer runs,
            // checks).
            BootFolders folders;
            folders.resources = gameResources(g_argv0, game);
            // The extras the setup made from the player's other discs
            // (collection/extras/setup.h), read before the bundled ones.
            folders.extras = state.paths.supportDir / "extras";
            // The Thor's bottom screen (panel/panel.h): this game's panel
            // files (the setup's, else the bundled ones), and Aggressive
            // Inline's capture until the game's override installs its own
            // (dave_mirra_panel.cpp).
            panel::setGameCapture(nullptr);
            panel::setGameFiles((folders.extras / "panel").string(), (folders.resources / "panel").string());
            folders.screenshots = screenshotFolder(state.paths);
            folders.gamepadMappings = gamepadMappings;
            // The collection's Mods folder (collection/mods.h): its mods for
            // this game, with the game's own folder's.
            if (kCollection && !g_appSettingsFile.empty())
                folders.sharedMods = g_appSettingsFile.parent_path() / kModsFolder;
            if (customMap)
            {
                // Custom Freeplay: this boot plays the map (collection/maps.h),
                // its folder in the slot its files are in, or its files
                // converted for the slot of this game they were made for.
                folders.customMap = customMap->folder;
                folders.customMapSlot = converted && converted->slot ? converted->slot->slot : customMap->slot;
                folders.customMapSlotNpcs = customMap->slotNpcs && !converted;
                folders.customMapConverted = converted != nullptr;
                folders.customMapPark = converted ? std::string() : customMap->park;
            }
            zx::BootConfig config =
                gameBootConfig(kCollection ? g_game->id : "", state.settings, state.paths, *g_profile, folders, getEnv);
            // A playback (collection/recordings.h): the boot the recording
            // was made with, from the saves it holds, copied into a folder of
            // its own (the player's are neither read nor written).
            zx::collection::PlaybackFolders playbackFolders;
            if (playback)
            {
                playbackFolders = zx::collection::playbackFolders(state.paths.supportDir / "Playback");
                std::string error;
                if (!zx::collection::restoreSaves(playback->entry.file, playbackFolders, state.paths.controlsFile, &error))
                {
                    std::cerr << "[app] playback: " << error << std::endl;
                    result.end = col::GameEnd::Failed;
                    result.status = 2;
                    result.why = "The recording cannot be played: " + error;
                    return result;
                }
                config = zx::collection::playbackBootConfig(playback->summary, playback->entry.file, config, playbackFolders);
                // (from here nothing of the player's is saved: playbackFrame, saveSettingsNow)
                g_playback = {};
                g_playback.active = true;
                g_playback.endVsync = playback->entry.vsyncs;
                std::cout << "[app] playback: " << playback->entry.file.string() << " (" << playback->entry.started << ", "
                          << zx::collection::lengthText(playback->entry.vsyncs) << ", made by " << playback->entry.commit
                          << (playback->entry.commit == play_recording::buildCommit() ? ": this build" : ": another build")
                          << "), its saves in " << playbackFolders.root.string() << std::endl;
            }
            // (the playback's state goes with the boot, however it ends)
            struct EndPlayback
            {
                bool playing;
                ~EndPlayback()
                {
                    if (!playing)
                        return;
                    g_playback = {};
                    zx::collection::PlaybackStrip::shared().show(std::nullopt);
                }
            } endPlayback{playback != nullptr};
            const std::filesystem::path cardSlot1 = playback ? playbackFolders.memcardSlot1 : state.paths.memcardSlot1;
            const std::filesystem::path cardSlot2 = playback ? playbackFolders.memcardSlot2 : state.paths.memcardSlot2;
            std::cout << "[app] screenshots: " << config.text("ZX_SCREENSHOT_DIR") << std::endl;
            if (customMap)
                std::cout << "[app] custom freeplay: " << customMap->name << " (" << customMap->folder.string() << ", made in "
                          << customMap->game << ") in slot " << folders.customMapSlot << ", start " << config.text("ZX_DEV_START_LEVEL")
                          << ", mods " << config.text("ZX_MODS_DIR")
                          << (converted ? ", its files converted, in memory (" + std::to_string(converted->files.size()) + " files, " +
                                              std::to_string(converted->bytes) + " bytes)"
                                        : std::string())
                          << std::endl;
            // (before the runtime: the game's file systems are made in its boot)
            const MapPanelFile mapPanel(game, customMap, mapSlot(g_game->id, folders.customMapSlot), converted, discFolder(g_argv0, game));
            const ConvertedMapFiles convertedFiles(converted);
            if (!state.settings.modsOff.empty())
            {
                // (the loader's [disc] line lists the mods read)
                std::cout << "[app] mods switched off:";
                for (const std::string &name : state.settings.modsOff)
                    std::cout << " " << name;
                std::cout << std::endl;
            }
            {
                const std::string backend = config.text("ZX_GS_BACKEND");
                const std::string scale = config.text("ZX_GS_SCALE");
                const bool gpu = backend == "gpu";
                std::cout << "[app] renderer: " << (gpu ? "GPU, internal resolution " : "CPU")
                          << (gpu ? (scale.empty() ? std::string("1") : scale) + "x" : "") << std::endl;
            }
            // Scripted and remote-controlled runs have no window focus; never
            // pause them.
            state.focusPauseAllowed = config.text("ZX_PAD_SCRIPT").empty() && config.text("ZX_HOST_INPUT_SCRIPT").empty() &&
                                      !envSet("ZX_CONTROL");
            state.pausedForFocus = false;

            const std::filesystem::path discDir = discFolder(g_argv0, game);
            // Card and host roots before anything can touch them (the defaults
            // are folders next to the current directory or the ELF).
            {
                PS2Machine::IoPaths early = PS2Machine::getIoPaths();
                early.elfDirectory = state.paths.hostRoot;
                early.hostRoot = state.paths.hostRoot;
                early.mcRoot = cardSlot1;
                early.cdRoot = discDir;
                early.cdImage.clear();
                early.cdFromImage = false;
                PS2Machine::setIoPaths(early);
            }
            // A leave asked for before this boot is not this game's.
            ps2_host::clearExitToMenu();

#if defined(ZX_ENABLE_DEBUG_UI)
            // Before the runtime, which calls its shutdown when it goes
            // (a boot that fails after initialize).
            PS2DebugPanel debugPanel;
#endif
            // The panel goes with this boot: nothing may call it after.
            struct ForgetPanel
            {
                AppState &state;
                ~ForgetPanel() { state.showFps = {}; }
            } forgetPanel{state};

            PS2Machine runtime(std::move(config));
            PS2Machine::HostOptions options;
            options.windowWidth = state.settings.windowWidth;
            options.windowHeight = state.settings.windowHeight;
            options.highDpi = true;
            options.smoothFilter = true;
            options.integerScale = state.settings.scaling == Scaling::Integer;
            options.widescreen = state.settings.widescreen;
            // The app's: it shuts the host down itself when it quits.
            options.hostOwned = true;
            runtime.setHostOptions(options);

#if defined(ZX_ENABLE_DEBUG_UI)
            // Hidden unless ZX_DEBUG_UI=1; F1 still toggles it.
            {
                const char *debugUi = std::getenv("ZX_DEBUG_UI");
                debugPanel.setVisible(debugUi && debugUi[0] == '1');
            }
            state.showFps = [&debugPanel](bool show) { debugPanel.setShowFps(show); };
            // (the player's messages over the game, as the menus write their help line)
            debugPanel.setNoticeStyle((platform::resourceFolder(g_argv0) / col::kShippedFontFile).string(), col::kHelpColour);
            runtime.setDebugUiCallbacks(
                [](PS2Machine &, void *userData)
                { static_cast<PS2DebugPanel *>(userData)->initialize(); },
                [](PS2Machine &rt, void *userData)
                { static_cast<PS2DebugPanel *>(userData)->draw(rt); },
                [](PS2Machine &, void *userData)
                { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
                &debugPanel);
#endif

            const std::string cannotStart = std::string(g_game->name) + " cannot start";
            auto failed = [&](const std::string &why, int status)
            {
                std::cerr << "[app] " << g_game->id << ": " << why << std::endl;
                result.end = col::GameEnd::Failed;
                result.status = status;
                result.why = cannotStart + ": " + why;
                // A single-game app says so and ends; the collection's menu
                // shows it.
                if (!kMenu)
                    platform::showMessage(cannotStart, why, true);
                return result;
            };

            if (!kMenu)
                prepareHost();
            if (!runtime.initialize(g_game->name))
                return failed("The game window could not be created.", 1);
            ps2_host::setWindowTitle(g_game->name);

            applyViewSettings(state.settings, runtime);
            if (!kMenu)
                installAppMenus(state, state.paths);
            registerSettings(state, runtime);

            const DiscCheck disc = checkDiscFolder(discDir, g_game->title, g_game->elf, g_game->elfSha256);
            if (disc.status != DiscCheck::Status::Ok)
            {
                std::cerr << "[app] game files: " << disc.message << std::endl;
                // Missing: the player has not set the game up yet; other
                // files: set it up again from its disc.
                const std::string fix = kMenu ? " Add its disc in Settings > Games."
                                              : " Start the app with --setup <your disc image or folder>.";
                return failed(disc.message + fix, 2);
            }
            std::cout << "[app] game files: " << discDir.string() << " (" << disc.bootElf << ", SHA-256 " << disc.elfSha256
                      << ")" << std::endl;
            if (std::string codeError; !ensureGameCode(state, *g_game, discDir, disc, gameCodeToolsFolder(), codeError))
            {
                std::cerr << "[app] game code: " << codeError << std::endl;
                return failed("Making the game code failed. " + codeError, 2);
            }

            if (!playback)
                offerPcsx2Import(state);

            // The bundled folder is the CD (read in place, never written). All
            // roots are set first: loading must not derive any from a folder
            // (the ELF is named as if it sat in the host folder).
            PS2Machine::IoPaths paths = PS2Machine::getIoPaths();
            paths.cdRoot = discDir;
            paths.cdImage.clear();
            paths.cdFromImage = false;
            paths.hostRoot = state.paths.hostRoot;
            paths.mcRoot = cardSlot1;
            paths.elfDirectory = state.paths.hostRoot;
            PS2Machine::setIoPaths(paths);
            if (!runtime.loadELFImage((state.paths.hostRoot / disc.bootElf).string(), disc.elf, false))
                return failed("The game program could not be loaded.", 1);
            if (nativeSaves())
                std::cout << "[app] saves: " << nativeSaveFile().string() << " (memory card folders, imported once: "
                          << state.paths.memcardSlot1.string() << ", " << state.paths.memcardSlot2.string() << ")"
                          << std::endl;
            else
                std::cout << "[app] memory cards: " << cardSlot1.string() << ", " << cardSlot2.string() << std::endl;

            g_switchGamePresses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeySwitchGame);
            g_screenshotPresses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeyScreenshot);
            runtime.setHostFrameCallback([&state](PS2Machine &rt)
                                         { gameFrame(rt, state); });
            g_levelNoted.reset();
            g_firstVsync = UINT64_MAX;
            g_lastVsync = 0;
            runtime.addVSyncHook([](uint8_t *rdram, uint64_t tick)
                                 {
                                     captureMapSnapshot(rdram);
                                     noteLevel(rdram, tick);
                                     countVsync(tick);
                                 });
            // (the profile this boot plays: the player may pick another for the next one meanwhile)
            const std::filesystem::path playtimeFile = playback || !nativeSaves() ? std::filesystem::path()
                                                       : col::profilePlaytime(nativeSavesFolder(), nativeSaveFile().stem().string());
            if (playback)
            {
                g_playback.lineUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                g_playback.script = playbackScript(std::getenv("ZX_APP_PLAYBACK_SCRIPT"));
                ps2_host::setWindowTitle((std::string(g_game->name) + " - Playback").c_str());
            }
            std::cout << "[app] " << g_game->id << (playback ? ": playing back" : ": running") << std::endl;
            // (the settings as this boot runs with them: what Restart Game's row counts from)
            zx::settings::markStarted();
            g_restartWaitingSaid = 0;
            g_restartWanted = false;
            g_runtime = &runtime;
            runtime.run();
            ps2_game_overrides::runShutdownHooks(runtime);
            g_runtime = nullptr;
            // (the row added while a change waited goes with the boot)
            zx::settings::setAfterChanges({});
            zx::settings::remove("game.restart");
            g_restartOffered = false;
            if (const uint64_t first = g_firstVsync, last = g_lastVsync; !playtimeFile.empty() && first != UINT64_MAX && last > first)
            {
                // (NTSC: 60000 / 1001 vsyncs a second, the three games' releases)
                const uint64_t seconds = (last - first) * 1001u / 60000u;
                if (!col::addPlaytime(playtimeFile, seconds))
                    std::cerr << "[app] playtime: " << playtimeFile.string() << " not written" << std::endl;
                else
                    std::cout << "[app] playtime: " << seconds << " s more, " << col::readPlaytime(playtimeFile) << " s in "
                              << playtimeFile.string() << std::endl;
            }

            // Window closed (close button, Cmd+Q, Alt+F4): the app quits. Left
            // for the menu, or the game ended by itself: the collection's
            // menu again.
            if (ps2_host::quitRequested())
                result.end = col::GameEnd::Quit;
            else
                result.end = kMenu ? col::GameEnd::ExitToMenu : col::GameEnd::Quit;
            const bool left = ps2_host::exitToMenuRequested();
            ps2_host::clearExitToMenu();
            state.settings.fullscreen = state.fullscreen;
            saveSettingsNow(state);
            if (playback)
            {
                // The playback's end: what it showed, for the menu's line;
                // the settings as the player has them again (the ones it
                // replayed were the recording's, and were not saved).
                const play_recording::ReplayResults replayed = play_recording::replayResults();
                std::cout << "[app] playback: " << (left ? "left" : "ended") << " at vsync " << replayed.vsync << " of "
                          << playback->entry.vsyncs << ", " << replayed.checksumsMatched << " state checks matched, "
                          << replayed.checksumsDiffered << " differed" << std::endl;
                if (replayed.checksumsDiffered > 0)
                    result.why = "It played differently than it was recorded from " +
                                 zx::collection::lengthText(replayed.firstDifferingVsync) + ".";
                else if (!left)
                    result.why = playback->entry.ended ? "End of the recording." : "End of the recording (it was cut short).";
                const Settings window = state.settings;
                loadAppSettings(state, state.settings, nullptr);
                state.settings.windowWidth = window.windowWidth;
                state.settings.windowHeight = window.windowHeight;
                state.settings.fullscreen = window.fullscreen;
                state.settings.fullscreenMode = window.fullscreenMode;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bootStart).count();
            char took[32];
            std::snprintf(took, sizeof(took), "%.1f", seconds);
            std::cout << "[app] " << g_game->id << ": "
                      << (result.end == col::GameEnd::Quit ? "the app quits" : "left for the menu") << " after " << took
                      << " s" << std::endl;
            return result;
        }

        bool readWholeFile(const std::filesystem::path &file, std::vector<uint8_t> &out)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        // A game's card from its front-end files (collection/front_art.h):
        // its title picture, its colour, whether it is set up.
        col::Tile menuTile(const GameProfile &game, const col::FrontGame &front)
        {
            col::Tile tile;
            tile.name = game.name;
            tile.cardName = col::shortGameName(game.id);
            tile.player = col::playerType(game.id);
            tile.colour = front.colour;
            tile.installed = front.installed;
            tile.art = front.card;
            tile.focus = front.cardFocus;
            return tile;
        }

        // ---- setting games up from the player's discs ---------------------------

        std::vector<SetupGame> setupGames()
        {
            std::vector<SetupGame> games;
            for (const GameProfile &game : kGamesInOrder)
                games.push_back(setupGame(game));
            return games;
        }

        // The sources a picked or dropped path (or document) stands for:
        // itself, or, for a folder that is no disc, each of its folders and
        // images that is one (a folder of every game's dumps).
        std::vector<std::filesystem::path> discSources(const std::filesystem::path &path) { return discSourcesIn(path); }

        // Documents picked together: a BIN/CUE dump's CUE goes when its BIN
        // came with it (a CUE document alone cannot read its BIN).
        std::vector<std::pair<int, std::string>> withoutPairedCues(std::vector<std::pair<int, std::string>> picks)
        {
            auto isCue = [](const std::string &pick) { return isCueSheetSource(pathFromUtf8(pick)); };
            std::vector<std::pair<int, std::string>> kept;
            const bool others = std::any_of(picks.begin(), picks.end(), [&](const auto &pick)
                                            { return isDocument(pathFromUtf8(pick.second)) && !isCue(pick.second); });
            for (auto &pick : picks)
                if (!others || !isDocument(pathFromUtf8(pick.second)) || !isCue(pick.second))
                    kept.push_back(std::move(pick));
            return kept;
        }

        // ---- the extras: the other releases' discs (collection/extras/setup.h) ----

        namespace ex = zx::app::extras;

        // An extras disc's image: a document through the platform's own
        // access (Android), else the file at the path.
        ex::ImageOpener extrasOpener()
        {
            return [](const std::string &source, std::string *error) -> std::unique_ptr<ex::ImageFile>
            {
                const std::filesystem::path path = pathFromUtf8(source);
                if (isDocument(path) && documentAccess())
                    return ex::ImageFile::adopt(documentAccess()->open(source), sourceName(path), error);
                return ex::ImageFile::open(path, error);
            };
        }

        int gameIndexOf(const std::string &id)
        {
            for (size_t i = 0; i < kGameCount; ++i)
                if (id == kGamesInOrder[i].id)
                    return static_cast<int>(i);
            return -1;
        }

        // The setup's progress for the extras' steps.
        ex::ExtrasProgressFn extrasProgress(const SetupProgressFn &progress)
        {
            return [progress](const std::string &step, size_t done, size_t total)
            {
                SetupProgress p;
                p.step = step;
                p.done = done;
                p.total = total;
                return !progress || progress(p);
            };
        }

        // A game's extras brought up to date (collection/extras/setup.h): the
        // parts whose key differs made again, from the discs recorded for it
        // (each read and checked again; one of an added disc's release left
        // out) and `added` (none: no new one). A recorded disc that does not
        // open now (a drive not there, a moved image) keeps what was made
        // from it, and stays recorded. The player's words for how it went.
        std::string remakeExtras(const GameProfile &game, std::vector<ex::ExtrasSource> added, const SetupProgressFn &progress,
                                 bool &ok)
        {
            const std::filesystem::path dir = gameFolder(game);
            std::vector<ex::ExtrasSource> sources;
            std::vector<ex::RecordedDisc> unopened;
            for (const ex::RecordedDisc &recorded : ex::recordedExtras(dir))
            {
                if (std::any_of(added.begin(), added.end(), [&](const ex::ExtrasSource &source)
                                { return source.disc && recorded.release == source.disc->release; }))
                    continue;
                ex::ExtrasSource again = ex::checkExtrasSource(recorded.source, game.id, extrasOpener());
                if (!again.disc)
                {
                    std::cout << "[extras] " << game.id << ": " << recorded.source << ": " << again.message
                              << " (what was made from it is kept)" << std::endl;
                    unopened.push_back(recorded);
                    continue;
                }
                sources.push_back(std::move(again));
            }
            for (ex::ExtrasSource &source : added)
                sources.push_back(std::move(source));
            const auto started = std::chrono::steady_clock::now();
            const ex::ExtrasResult result =
                ex::makeExtras(game.id, dir, sources, ex::engineXboxModelMaker(), extrasProgress(progress), unopened);
            for (const std::string &line : result.log)
                std::cout << "[extras] " << line << std::endl;
            std::string made;
            for (const std::string &part : result.made)
                made += (made.empty() ? "" : ", ") + part;
            std::cout << "[extras] " << game.id << ": " << result.message << (made.empty() ? "" : " (" + made + ")") << " ("
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
            ok = result.ok;
            std::string message = result.message;
            for (const ex::RecordedDisc &disc : result.missing)
            {
                std::string title = disc.release;
                for (const ex::ExtrasDisc *known : ex::extrasDiscsOf(game.id))
                    if (disc.release == known->release)
                        title = known->title;
                const std::string ask = "Reconnect " + title + " (" + sourceName(pathFromUtf8(disc.source)) + ") to update " +
                                        col::shortGameName(game.id) + "'s extras: they are kept as they are.";
                std::cout << "[extras] " << game.id << ": " << ask << std::endl;
                message += "  " + ask;
            }
            return message;
        }

        // An Xbox or GameCube disc the player gave, for the game it is a
        // release of; false when the source is no extras disc the app knows
        // (*message then says so when it is another release of one).
        bool setUpExtrasFrom(const std::string &source, std::string &message, const SetupProgressFn &progress, int &changed,
                             bool &ok)
        {
            ex::ExtrasSource checked = ex::checkExtrasSource(source, "", extrasOpener());
            if (checked.gameDisc) // (a PS2 disc goes on to the PS2 setup unlogged)
                std::cout << "[extras] " << source << ": " << checked.message << std::endl;
            if (!checked.disc)
            {
                // An Xbox or GameCube disc the app makes nothing from: says so.
                if (checked.gameDisc)
                    message = checked.message;
                return checked.gameDisc;
            }
            const int index = gameIndexOf(checked.disc->game);
            if (index < 0)
            {
                message = checked.message;
                return true;
            }
            const GameProfile &game = kGamesInOrder[static_cast<size_t>(index)];
            if (!std::filesystem::is_directory(gameFolder(game) / "disc"))
            {
                message = std::string(game.title) + "'s extras need its PS2 disc first: set it up, then add " +
                          checked.disc->title + ".";
                return true;
            }
            std::vector<ex::ExtrasSource> added;
            added.push_back(std::move(checked));
            message = remakeExtras(game, std::move(added), progress, ok);
            changed = index;
            return true;
        }

        // One source set up for the game it is (`wanted` >= 0: the game the
        // player meant). The player's words for how it went; `changed` the
        // game whose files changed, -1 none.
        std::string setUpFrom(const std::filesystem::path &path, int wanted, const SetupProgressFn &progress, int &changed,
                              bool &ok)
        {
            changed = -1;
            ok = false;
            // An Xbox or GameCube disc: its game's extras.
            std::string extrasMessage;
            if (setUpExtrasFrom(utf8Of(path), extrasMessage, progress, changed, ok))
                return extrasMessage;
            DiscSource source;
            std::string error;
            if (!source.open(path, &error))
                return error;
            const std::vector<SetupGame> games = setupGames();
            const SourceCheck check = checkSource(source, games, wanted);
            std::cout << "[setup] " << path.string() << ": " << check.message << std::endl;
            if (check.status != SourceCheck::Status::Ok)
                return check.message;
            const GameProfile &game = kGamesInOrder[static_cast<size_t>(check.game)];
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            const auto started = std::chrono::steady_clock::now();
            const InstallResult result = installGame(source, games[static_cast<size_t>(check.game)], gameFolder(game), code.get(), progress);
            if (result.filesInstalled)
                changed = check.game;
            ok = result.ok;
            if (result.ok)
            {
                // The bottom screen's maps from the new files, and the extras
                // from the discs recorded for the game, made again with them.
                bool extrasOk = false;
                const std::string extras = remakeExtras(game, {}, progress, extrasOk);
                if (!extrasOk)
                    std::cout << "[setup] " << game.id << ": " << extras << std::endl;
            }
            std::cout << "[setup] " << game.id << ": " << result.message << " (" << result.files << " files, " << sizeText(result.bytes)
                      << ", " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
            return result.message;
        }

        // ---- the discs found in a folder (the Games page's Find Discs, a drop) ----

        // What the discs found do for one game: its PS2 disc installed (none:
        // the files installed before kept), then its extras made from the
        // discs recorded for it and these.
        struct FoundDiscs
        {
            int game = -1; // -1: nothing to do (no PS2 disc for it, or set up with no new extras)
            std::filesystem::path ps2;
            std::vector<std::filesystem::path> extras;
        };

        // The plan for game `index` from what the search found.
        FoundDiscs plannedSetup(const DiscSearch &search, size_t index)
        {
            const GameProfile &game = kGamesInOrder[index];
            const std::filesystem::path dir = gameFolder(game);
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            const InstalledCheck check = checkInstalled(dir, setupGame(game), false, code.get());
            const bool installed = check.status == InstalledCheck::Status::Ok || check.status == InstalledCheck::Status::ByHand;
            std::error_code ec;
            FoundDiscs out;
            if (const FoundDisc *ps2 = search.ps2Of(static_cast<int>(index)); ps2 && !installed)
                out.ps2 = ps2->source;
            const std::vector<ex::RecordedDisc> recorded = ex::recordedExtras(dir);
            for (const FoundDisc *disc : search.extrasOf(game.id))
                if (std::none_of(recorded.begin(), recorded.end(), [&](const ex::RecordedDisc &r)
                                 { return r.release == disc->extras->release && r.source == utf8Of(disc->source); }))
                    out.extras.push_back(disc->source);
            const bool hasDisc = std::filesystem::is_directory(dir / "disc", ec);
            if (out.ps2.empty() && (!hasDisc || out.extras.empty()))
                return {};
            out.game = static_cast<int>(index);
            return out;
        }

        // One game set up from the discs found. The player's words for how
        // it went; `ok` when its PS2 disc is in place (its extras may still
        // have failed: their words then).
        std::string setUpFound(const FoundDiscs &found, const SetupProgressFn &progress, bool &ok)
        {
            ok = false;
            const GameProfile &game = kGamesInOrder[static_cast<size_t>(found.game)];
            std::string said;
            if (!found.ps2.empty())
            {
                DiscSource source;
                std::string error;
                if (!source.open(found.ps2, &error))
                    return error;
                const std::vector<SetupGame> games = setupGames();
                const SourceCheck check = checkSource(source, games, found.game);
                std::cout << "[setup] " << utf8Of(found.ps2) << ": " << check.message << std::endl;
                if (check.status != SourceCheck::Status::Ok)
                    return check.message;
                const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
                const auto started = std::chrono::steady_clock::now();
                const InstallResult result =
                    installGame(source, games[static_cast<size_t>(found.game)], gameFolder(game), code.get(), progress);
                std::cout << "[setup] " << game.id << ": " << result.message << " (" << result.files << " files, "
                          << sizeText(result.bytes) << ", "
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
                if (!result.ok)
                    return result.message;
                said = result.message;
            }
            std::vector<ex::ExtrasSource> added;
            for (const std::filesystem::path &disc : found.extras)
            {
                ex::ExtrasSource checked = ex::checkExtrasSource(utf8Of(disc), game.id, extrasOpener());
                std::cout << "[extras] " << utf8Of(disc) << ": " << checked.message << std::endl;
                if (checked.disc)
                    added.push_back(std::move(checked));
            }
            ok = true;
            bool extrasOk = false;
            const std::string extras = remakeExtras(game, std::move(added), progress, extrasOk);
            return extrasOk && !said.empty() ? said : extras;
        }

        // A setup off the menu's thread: its progress for the screen, its
        // results when it ends.
        struct SetupJob
        {
            std::thread worker;
            std::mutex mutex;
            SetupProgress progress; // (mutex)
            std::string title;      // what it does, for the screen
            std::vector<std::string> messages; // (mutex)
            std::vector<int> changed;          // (mutex) the games whose files changed
            // A search for discs: what it found, where, and each game's plan.
            std::optional<DiscSearch> found;   // (mutex)
            std::string where;
            std::vector<FoundDiscs> plans;     // (mutex) by game
            // A setup of several games (the discs found): each game's part,
            // for its row on the Games page; empty for any other job.
            struct GamePart
            {
                enum State
                {
                    None,
                    Waiting,
                    Running,
                    Done,
                    Failed,
                    Stopped,
                } state = None;
                std::string said;
            };
            std::vector<GamePart> parts; // (mutex) by game
            bool settingUpFound = false; // (the menu's thread) it sets up the discs found: the Games page shows their progress
            bool updating = false;       // (the menu's thread) with settingUpFound: the update's work (startUpdate), on those rows
            bool settingsImported = false; // (mutex) a saves import replaced the settings files
            std::atomic<bool> cancel{false};
            std::atomic<bool> finished{false};

            SetupProgressFn reporter()
            {
                return [this](const SetupProgress &p)
                {
                    std::lock_guard lock(mutex);
                    progress = p;
                    return !cancel.load();
                };
            }
            ~SetupJob()
            {
                cancel = true;
                if (worker.joinable())
                    worker.join();
            }
        };

        // (declared above) The game's code made as Set Up Games' last step makes it, its progress on the game's row
        // ("Building code 64%"): the module the boot then loads (ensureGameCode finds it made).
        bool updateGameCode(SetupJob &job, const GameProfile &game, std::string &why)
        {
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            const std::filesystem::path dir = gameFolder(game);
            const auto started = std::chrono::steady_clock::now();
            const bool made = code->make(setupGame(game), installedDiscFolder(dir), dir, job.reporter(), &why);
            std::cout << "[update] " << game.id << ": its code " << (made ? "made" : job.cancel ? "stopped" : "not made: " + why) << " in "
                      << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count() << " s"
                      << std::endl;
            if (!made && job.cancel)
                why.clear();
            return made;
        }

        // The paths a file dialog gave (its callback may run on another
        // thread), each with the game whose row asked (-1: any).
        struct DialogPicks
        {
            std::mutex mutex;
            std::vector<std::pair<int, std::string>> picks;
        };
        DialogPicks g_dialogPicks;

        // The system's file or folder dialog for a game's disc. False when
        // there is no window to show one on.
        bool askForDisc(int game, bool folder)
        {
            // The system's own picker where it has one (Android's documents).
            if (platform::pickDocuments(folder, [game](std::vector<std::string> picks)
                                        {
                                            std::lock_guard lock(g_dialogPicks.mutex);
                                            for (std::string &pick : picks)
                                                g_dialogPicks.picks.emplace_back(game, std::move(pick));
                                        }))
                return true;
            SDL_Window *window = ps2_host::sdlWindow();
            if (!window)
                return false;
            // (the formats' usual names: the setup tells them by their bytes)
            static const SDL_DialogFileFilter kFilters[] = {
                {"PS2 disc images (ISO, BIN/CUE, CHD, CSO, ZSO)", "iso;bin;cue;img;chd;cso;zso"}, {"All files", "*"}};
            const SDL_DialogFileCallback picked = [](void *userdata, const char *const *files, int)
            {
                const std::unique_ptr<int> forGame(static_cast<int *>(userdata));
                if (!files)
                {
                    std::cerr << "[setup] the file dialog failed: " << SDL_GetError() << std::endl;
                    return;
                }
                std::lock_guard lock(g_dialogPicks.mutex);
                for (; *files; ++files)
                    g_dialogPicks.picks.emplace_back(*forGame, *files);
            };
            if (folder)
                SDL_ShowOpenFolderDialog(picked, new int(game), window, nullptr, false);
            else
                SDL_ShowOpenFileDialog(picked, new int(game), window, kFilters, 2, nullptr, false);
            return true;
        }

        // The system's file dialog for an extras disc (an Xbox or GameCube
        // disc image), its pick set up as a disc is: the disc says which game
        // and release it is. False when there is no window to show one on.
        bool askForExtrasDisc(int game)
        {
            if (platform::pickDocuments(false, [game](std::vector<std::string> picks)
                                        {
                                            std::lock_guard lock(g_dialogPicks.mutex);
                                            for (std::string &pick : picks)
                                                g_dialogPicks.picks.emplace_back(game, std::move(pick));
                                        }))
                return true;
            SDL_Window *window = ps2_host::sdlWindow();
            if (!window)
                return false;
            static const SDL_DialogFileFilter kFilters[] = {
                {"Xbox and GameCube disc images (ISO, XISO, RVZ, GCM)", "iso;xiso;rvz;gcm"}, {"All files", "*"}};
            const SDL_DialogFileCallback picked = [](void *userdata, const char *const *files, int)
            {
                const std::unique_ptr<int> forGame(static_cast<int *>(userdata));
                if (!files)
                {
                    std::cerr << "[extras] the file dialog failed: " << SDL_GetError() << std::endl;
                    return;
                }
                std::lock_guard lock(g_dialogPicks.mutex);
                for (; *files; ++files)
                    g_dialogPicks.picks.emplace_back(*forGame, *files);
            };
            SDL_ShowOpenFileDialog(picked, new int(game), window, kFilters, 2, nullptr, false);
            return true;
        }

        // Where the saves go to or come from (Games page, "Saves and
        // Settings"): what a dialog gave (its callback may run on another
        // thread), "" none.
        struct TransferPicks
        {
            std::mutex mutex;
            std::string exportTo, importFrom;
        };
        TransferPicks g_transferPicks;

        void keepTransferPick(bool exporting, std::string pick)
        {
            std::lock_guard lock(g_transferPicks.mutex);
            (exporting ? g_transferPicks.exportTo : g_transferPicks.importFrom) = std::move(pick);
        }

        // The system's dialog for the saves' file: a new one to export to,
        // or one to import. False when there is no window to show one on.
        bool askForTransfer(bool exporting)
        {
            constexpr const char *kSuggested = ZX_APP_NAME " saves.zip";
            const bool own = exporting ? platform::createDocument(kSuggested, "application/zip",
                                                                  [](std::string pick) { keepTransferPick(true, std::move(pick)); })
                                       : platform::pickDocuments(false, [](std::vector<std::string> picks)
                                                                 { keepTransferPick(false, std::move(picks.front())); });
            if (own)
                return true;
            SDL_Window *window = ps2_host::sdlWindow();
            if (!window)
                return false;
            static const SDL_DialogFileFilter kFilters[] = {{"Saves (zip)", "zip"}};
            const SDL_DialogFileCallback picked = [](void *userdata, const char *const *files, int)
            {
                if (!files)
                    std::cerr << "[saves] the file dialog failed: " << SDL_GetError() << std::endl;
                else if (*files)
                    keepTransferPick(userdata != nullptr, *files);
            };
            void *exportingFlag = exporting ? &g_transferPicks : nullptr;
            if (exporting)
                SDL_ShowSaveFileDialog(picked, exportingFlag, window, kFilters, 1, kSuggested);
            else
                SDL_ShowOpenFileDialog(picked, exportingFlag, window, kFilters, 1, nullptr, false);
            return true;
        }

        // The text the menu draws when AI's menu font is not there (AI not
        // set up yet): SDL's own 8x8 characters, twice their size.
        col::BitmapFont builtInFont()
        {
            constexpr int kScale = 2, kCell = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE, kColumns = 16, kRows = 6;
            col::BitmapFont font;
            SDL_Surface *surface = SDL_CreateSurface(kColumns * kCell * kScale, kRows * kCell * kScale, SDL_PIXELFORMAT_RGBA32);
            SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
            if (renderer)
            {
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                SDL_RenderClear(renderer);
                SDL_SetRenderScale(renderer, static_cast<float>(kScale), static_cast<float>(kScale));
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                for (int c = 33; c < 127; ++c)
                {
                    const char text[2] = {static_cast<char>(c), '\0'};
                    const int index = c - 32;
                    SDL_RenderDebugText(renderer, static_cast<float>(index % kColumns * kCell), static_cast<float>(index / kColumns * kCell), text);
                }
                SDL_RenderPresent(renderer);
                font.page.width = surface->w;
                font.page.height = surface->h;
                font.page.pixels.resize(static_cast<size_t>(surface->w) * static_cast<size_t>(surface->h));
                for (int y = 0; y < surface->h; ++y)
                    std::memcpy(font.page.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(surface->w),
                                static_cast<const uint8_t *>(surface->pixels) + static_cast<size_t>(y) * static_cast<size_t>(surface->pitch),
                                static_cast<size_t>(surface->w) * 4u);
                font.height = (kCell + 2) * kScale;
                for (int c = 33; c < 127; ++c)
                {
                    const int index = c - 32;
                    col::BitmapFont::Glyph &glyph = font.glyphs[static_cast<size_t>(c)];
                    glyph.present = true;
                    glyph.u = index % kColumns * kCell * kScale;
                    glyph.v = index / kColumns * kCell * kScale;
                    glyph.width = (kCell - 1) * kScale;
                    glyph.height = kCell * kScale;
                    glyph.top = 1;
                }
                SDL_DestroyRenderer(renderer);
            }
            if (surface)
                SDL_DestroySurface(surface);
            return font;
        }

        // The collection's menu: what it shows and its input between frames.
        struct MenuScreen
        {
            static constexpr int kWidth = 960, kHeight = 720; // 4:3, as the games are shown
            col::Canvas canvas{kWidth, kHeight};
            col::Layout layout = col::layout(kWidth, kHeight, kGameCount);
            std::vector<col::Tile> tiles;
            col::BitmapFont font;
            col::MenuScript script;
            // The front end from the games' discs (collection/front_art.h):
            // each game's card, riders, menu movie and logo, in the cards'
            // order; what moves behind the menu; the menu's music and sounds
            // (played through `sound` while the menu is up). The Thor's
            // bottom screen: each game's roster heads, read when first shown.
            std::shared_ptr<const std::vector<col::FrontGame>> front = std::make_shared<std::vector<col::FrontGame>>();
            col::MenuScene scene;
            col::MenuAudio audio;
            SDL_AudioStream *sound = nullptr;
            std::map<int, std::shared_ptr<const std::vector<col::Image>>> heads;
            double fadeInFrom = -1.0; // the menu fades in from black (after the intro or a game) from then
            std::string music;        // the game whose music plays ("" none)
            std::string bottomShown;  // what the bottom screen was handed last (showBottom)
            std::vector<uint16_t> padHeld; // per gamepad slot, D-pad bits from the stick included
            col::HoldRepeat repeat;        // the directions held, going again
            float wheel = 0.0f;            // the wheel's turn not yet a whole step
            // Whose buttons the bottom line names: the device last used (at
            // the start the first gamepad's, when one is connected).
            col::HintDevice hint = col::HintDevice::Keyboard;
            bool hintChosen = false;
            // The gamepad the Thor's bottom line names (the one last used,
            // else the first; none: no gamepad), whichever device was used last.
            int padSlot = 0;
            std::optional<col::HintDevice> pad;
            std::string notice;
            int visits = 0;
            std::vector<col::SettingsRow> rows; // the app's settings page in the menu
            col::PageLayout page;
            // The Mods page (Settings > Mods): every mod, in the order, listed
            // again whenever the page opens (`library`, collection/mods.h; the
            // page's entries, one each); the page of the mod chosen, made
            // again after each change on it.
            std::vector<Mod> library;
            std::shared_ptr<std::vector<col::ModEntry>> mods = std::make_shared<std::vector<col::ModEntry>>();
            std::vector<col::SettingsRow> modRows;
            col::PageLayout modsPage;
            std::vector<col::SettingsRow> modPageRows;
            col::PageLayout modPage;
            int modShown = -1;      // the mod whose page the rows are
            bool modsChanged = false; // a switch or a move on the mod page: list and make its rows again
            col::MenuPage shownPage = col::MenuPage::Tiles;
            // Custom Freeplay: the player page's games (those whose maps
            // start, freeplayStarts; read with the front end), their tiles
            // and Back, and its line on why a game is not there ("" none);
            // the custom maps of the maps folder (listed again whenever the
            // map page opens) and the map page's rows for the player's game.
            std::vector<int> players;
            col::Layout playerLayout = col::layout(kWidth, kHeight, kGameCount);
            col::PageLayout playerPage = col::pageLayout(kWidth, kHeight, 0);
            std::string playersNote;
            std::vector<CustomMap> maps;
            std::vector<col::SettingsRow> mapRows;
            col::PageLayout mapsPage;
            int mapsFor = -1; // the game the rows are for
            // The Games page (Settings > Games): each game's files as the
            // setup last found them, and the page's rows.
            std::vector<InstalledCheck> gameStatus;
            std::vector<col::SettingsRow> gameRows;
            col::PageLayout gamesPage;
            // The setup running (one at a time, off this thread), and what a
            // Games page row asked for (done between frames).
            std::unique_ptr<SetupJob> job;
            // A game's boot or a recording's playback chosen while the game's code was not this app's (the update
            // stopped or failed before it): taken again once that game's update is done.
            std::optional<col::MenuAction> afterUpdate;
            struct Request
            {
                enum Kind
                {
                    None,
                    CheckEveryFile,
                    Repair,
                    Remove,
                    SetUpFound, // the games ticked among the discs found
                } kind = None;
                int game = -1;
            } request;
            // The discs the last search found (Find Discs, a drop), where
            // it looked, each game's plan and tick; why each game the last
            // setup of them did not set up ("" none), until the next search.
            struct Found
            {
                bool searched = false;
                std::string where;
                DiscSearch search;
                std::vector<FoundDiscs> plans; // by game
                std::vector<int> ticked;       // by game, 0 or 1
            } found;
            std::vector<std::string> failed = std::vector<std::string>(kGameCount);
            std::vector<std::string> scriptDrops; // a menu script's drop: steps, taken as the window's drops
            std::string gamesSubtitle; // under the Games page's title: what the search found ("" none)
            // The map about to be played, made into the player's files (a map
            // made in another game), held until its boot ends; and the maps
            // that did not convert this session, by "<folder>|<player>": why.
            std::optional<ConvertedMap> converted;
            std::map<std::string, std::string> conversionFailures;
            // The Recordings page (Settings > Recordings): every game's
            // recordings, listed again whenever the page opens, and its rows
            // (a heading per game, a row per recording), by the index the
            // rows' Play gives.
            std::vector<col::RecordingEntry> recordings;
            std::vector<col::SettingsRow> recordingRows;
            col::PageLayout recordingsPage;
            // Mods and maps dropped on the window (collection/mod_install.h):
            // those the player said yes to, copied one at a time off this
            // thread, the menu working on meanwhile.
            struct Install
            {
                DropInstall plan;
                std::thread worker;
                std::atomic<bool> done{false};
                bool ok = false;     // (the worker's, read once it is done)
                std::string error;
            };
            std::unique_ptr<Install> install;
            std::deque<DropInstall> installs;
            // The Save Profiles page (Settings > Save Profiles): each game's
            // careers (collection/save_profiles.h), made again whenever it
            // opens and after a change on it.
            std::vector<col::SettingsRow> profileRows;
            col::PageLayout profilesPage;
            bool profilesChanged = false;
        };

        // The layout of the tiles page: the games' tiles, and the Custom
        // Freeplay tile when the shell has it.
        col::Layout tilesLayout(const col::Shell &shell)
        {
            return col::layout(MenuScreen::kWidth, MenuScreen::kHeight, kGameCount + (shell.freeplay() ? 1 : 0));
        }

        // A game's settings as its next boot reads them: the game's own
        // file under the root's, or, for the game last played, what the app
        // holds of it (state.settings, written when the app quits).
        Settings gameSettings(const AppState &state, const AppPaths &collection, const GameProfile &game)
        {
            if (g_profile && g_game == &game)
                return state.settings;
            Settings settings;
            loadCollectionSettings(g_appSettingsFile, collection.supportDir / "games" / game.id / "settings.ini", settings);
            return settings;
        }

        // The collection's Mods folder: <support>/Mods.
        std::filesystem::path sharedModsFolder(const AppPaths &collection)
        {
            return collection.supportDir / kModsFolder;
        }

        // What a mod holds, for the page: "files, music, textures".
        std::string contentsText(unsigned contents)
        {
            std::string text;
            auto add = [&text](const char *part)
            {
                text += (text.empty() ? "" : ", ") + std::string(part);
            };
            if (contents & kModFiles)
                add("game files");
            if (contents & kModMusic)
                add("music");
            if (contents & kModTuning)
                add("tuning");
            if (contents & kModTextures)
                add("textures");
            if (contents & kModMap)
                add("a map");
            return text;
        }

        // Every mod as the Mods page lists it (menu.library, in the order:
        // each game's own Mods folder's, then the collection's), and the
        // page's entries: for each game a mod is for, whether its boot reads
        // it (gameBootConfig's modsForGame) and what it wins and loses there.
        void listLibrary(MenuScreen &menu, const AppState &state, const AppPaths &collection)
        {
            const std::vector<Mod> shared = listSharedMods(sharedModsFolder(collection));
            std::vector<Mod> library;
            std::vector<std::vector<Mod>> own(kGameCount);
            std::vector<Settings> settings(kGameCount);
            for (size_t g = 0; g < kGameCount; ++g)
            {
                const GameProfile &game = kGamesInOrder[g];
                settings[g] = gameSettings(state, collection, game);
                own[g] = listMods(collection.supportDir / "games" / game.id / "Mods", gameResources(g_argv0, game) / "disc",
                                  settings[g].modsOff, game.id);
                library.insert(library.end(), own[g].begin(), own[g].end());
            }
            library.insert(library.end(), shared.begin(), shared.end());
            orderMods(library, state.settings.modOrder);

            // Each game's list as its boot has it, and its winners.
            std::vector<std::vector<Mod>> lists(kGameCount);
            std::vector<std::vector<ModWinners>> winners(kGameCount);
            for (size_t g = 0; g < kGameCount; ++g)
            {
                lists[g] = modsForGame(kGamesInOrder[g].id, own[g], shared, state.settings.modOrder, settings[g].modsOff);
                winners[g] = modWinners(lists[g], kGamesInOrder[g].id);
                // (the single archive entries each gives that the game does not read)
                checkArchiveEntries(lists[g], collection.supportDir / "games" / kGamesInOrder[g].id / "Mods",
                                    gameResources(g_argv0, kGamesInOrder[g]) / "disc");
            }
            auto nameOf = [](const Mod &mod) { return mod.title.empty() ? mod.name : mod.title; };
            auto overlapText = [&](const std::vector<Mod> &list, const std::vector<ModOverlap> &overlaps, const char *verb)
            {
                std::string text;
                for (const ModOverlap &o : overlaps)
                    text += (text.empty() ? std::string(verb) + " " : std::string(", ")) + std::to_string(o.count) +
                            (std::string(verb) == "wins" ? " over " : " to ") + nameOf(list[o.mod]);
                return text;
            };
            menu.mods->clear();
            for (const Mod &mod : library)
            {
                col::ModEntry entry;
                entry.title = nameOf(mod);
                entry.version = mod.version;
                entry.author = mod.author;
                entry.where = mod.orderKey();
                entry.contents = contentsText(mod.contents);
                entry.logic = mod.changesLogic();
                entry.map = mod.isMap();
                entry.problem = mod.problem;
                entry.notes = mod.notes;
                for (size_t g = 0; g < kGameCount && !entry.map; ++g)
                {
                    const auto it = std::find_if(lists[g].begin(), lists[g].end(),
                                                 [&](const Mod &each) { return each.orderKey() == mod.orderKey(); });
                    if (it == lists[g].end())
                        continue;
                    const size_t at = static_cast<size_t>(it - lists[g].begin());
                    col::ModEntry::Game game;
                    game.title = col::shortGameName(kGamesInOrder[g].id);
                    game.on = it->on;
                    game.read = it->read;
                    game.why = it->why;
                    const std::string wins = overlapText(lists[g], winners[g][at].winsOver, "wins");
                    const std::string loses = overlapText(lists[g], winners[g][at].losesTo, "loses");
                    game.winners = wins + (!wins.empty() && !loses.empty() ? ", " : "") + loses;
                    game.entriesNotRead = it->entriesNotRead;
                    entry.games.push_back(std::move(game));
                }
                menu.mods->push_back(std::move(entry));
            }
            menu.library = std::move(library);
        }

        // A mod switched on or off in a game on its page: kept in the game's
        // settings.ini ([mods], by its key), read at the game's next boot.
        void switchMod(AppState &state, const AppPaths &collection, const GameProfile &game, const std::string &name, bool on)
        {
            std::cout << "[app] mods: " << game.id << " " << name << (on ? " on" : " off") << std::endl;
            if (g_profile && g_game == &game)
            {
                setModOn(state.settings, name, on);
                saveSettingsNow(state);
                return;
            }
            const std::filesystem::path file = collection.supportDir / "games" / game.id / "settings.ini";
            Settings settings = gameSettings(state, collection, game);
            setModOn(settings, name, on);
            if (!writeTextFile(file, formatSettings(settings, gameSettingsProfile(game.id), SettingsScope::Game)))
                std::cerr << "[app] could not write " << file.string() << std::endl;
        }

        // A mod moved one place earlier or later in the order: the whole
        // order kept in the collection's settings.ini ([mod order]).
        // The mod's index after it.
        size_t moveMod(MenuScreen &menu, AppState &state, size_t index, int steps)
        {
            const size_t to = steps < 0 ? (index == 0 ? 0 : index - 1) : std::min(index + 1, menu.library.size() - 1);
            if (to == index)
                return index;
            std::swap(menu.library[index], menu.library[to]);
            state.settings.modOrder = modOrderOf(menu.library);
            std::cout << "[app] mods: " << menu.library[to].orderKey() << " now " << (to + 1) << " of " << menu.library.size() << std::endl;
            saveSettingsNow(state);
            return to;
        }

        // A page's last row: its folder shown in the system's file browser
        // (col::showFolder), or where it is said on the notice line where
        // the system has none.
        col::SettingsRow folderRow(MenuScreen &menu, const char *label, const char *name, std::filesystem::path folder,
                                   const char *help)
        {
            col::SettingsRow row;
            row.kind = col::SettingsRow::Action;
            row.label = label;
            row.help = help;
            row.activate = [&menu, name, folder = std::move(folder)]()
            {
                menu.notice = col::showFolder(folder) ? std::string() : col::folderNotice(name, folder);
                return col::MenuAction{};
            };
            return row;
        }

        // The Mods page's rows, every mod listed again, then Open Mods Folder
        // (the collection's: a mod for every game goes there).
        void refreshMods(MenuScreen &menu, AppState &state, const AppPaths &collection)
        {
            listLibrary(menu, state, collection);
            // (a mod's switch on its row: on or off in every game it is for, as its page's switches)
            menu.modRows = col::modsPageRows(*menu.mods, kModsFolder,
                                             [&menu, &state, &collection](size_t m, bool on)
                                             {
                                                 if (m >= menu.library.size())
                                                     return;
                                                 for (const GameProfile &game : kGamesInOrder)
                                                     if (menu.library[m].forGame(game.id) && !menu.library[m].isMap())
                                                         switchMod(state, collection, game, menu.library[m].name, on);
                                                 menu.modsChanged = true;
                                             });
            menu.modRows.push_back(folderRow(menu, "Open Mods Folder", kModsFolder, sharedModsFolder(collection),
                                             "Put a mod's folder or .zip in it."));
            menu.modsPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.modRows.size());
        }

        // The page of the mod of the Mods page's row `index`: its rows, made
        // from the list as it is now.
        void refreshModPage(MenuScreen &menu, AppState &state, const AppPaths &collection, col::Shell &shell, int index)
        {
            if (index < 0 || static_cast<size_t>(index) >= menu.library.size())
            {
                menu.modPageRows.clear();
                menu.modShown = -1;
                menu.modPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, 0);
                return;
            }
            const size_t m = static_cast<size_t>(index);
            std::vector<const GameProfile *> games; // the entry's games, in its order
            for (const GameProfile &game : kGamesInOrder)
                if (menu.library[m].forGame(game.id) && !menu.library[m].isMap())
                    games.push_back(&game);
            const std::string key = menu.library[m].name;
            menu.modPageRows = col::modPageRows(
                menu.mods, m,
                [&state, &collection, &menu, games, key](size_t g, bool on)
                {
                    if (g < games.size())
                        switchMod(state, collection, *games[g], key, on);
                    menu.modsChanged = true;
                },
                [&menu, &state, &shell, m](int steps)
                {
                    shell.followMod(static_cast<int>(moveMod(menu, state, m, steps)));
                    menu.modsChanged = true;
                });
            menu.modPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.modPageRows.size());
            menu.modShown = index;
        }

        // What a slot's start needs that the setup lacks, as the map page
        // says it (the only such start: AI's Tutorial, through the Estate).
        constexpr const char *kStartNeeds = "starts through the Estate: add AI's GameCube disc in Games";

        // Which of `player`'s slots its setup can start (collection/maps.h
        // SlotStartable): what each needs (offered.h), of the folders a boot
        // of the game would be given.
        SlotStartable slotStartable(const GameProfile &player)
        {
            BootFolders folders;
            folders.resources = gameResources(g_argv0, player);
            folders.extras = ex::extrasFolder(gameFolder(player));
            const Installed installed = findInstalled(folders);
            return [installed](const MapSlot &slot) { return offered(slot.need, installed); };
        }

        // Every map the menu lists (collection/maps.h listAllMaps): the
        // collection's Mods folder's, each game's own Mods folder's, the
        // maps folder's.
        std::vector<CustomMap> listMenuMaps(const AppPaths &collection)
        {
            std::vector<CustomMap> gameMaps;
            for (const GameProfile &game : kGamesInOrder)
                for (CustomMap &map : listGameMaps(collection.supportDir / "games" / game.id / kModsFolder, game.id))
                    gameMaps.push_back(std::move(map));
            return listAllMaps(listSharedMods(sharedModsFolder(collection)), collection.supportDir / kMapsFolder, gameMaps);
        }

        // What a game none of whose slots the setup starts lacks, as the map
        // page says it (AI's Tutorial slot needs the Estate; its Movie Lot
        // needs nothing, so today every game starts), and the player page
        // (after the game's name).
        constexpr const char *kNoSlotStarts = "Its custom maps start through the Estate: add its GameCube disc in Games.";
        constexpr const char *kPlayerNeeds = "'s custom maps start through the Estate: add its GameCube disc in Games.";

        // Whether `player`'s Custom Freeplay start works with the setup as
        // it is: one of its game's slots starts (collection/maps.h).
        bool freeplayStarts(const GameProfile &player)
        {
            const SlotStartable startable = slotStartable(player);
            for (const MapSlot *slot : mapSlotsOf(player.id))
                if (startable(*slot))
                    return true;
            return false;
        }

        // The custom maps of the collection's maps folder (collection/maps.h),
        // and the map page's rows for the player's game `game`: every map the
        // game can start (as it is, or converted from the game it was made in)
        // starts it (a Boot with the map's index); one that did not convert
        // for this game this session is listed with why. Previews are read
        // here (BMPs, PNGs, JPEGs: collection/picture_file.h).
        void refreshMaps(MenuScreen &menu, const AppPaths &collection, int game)
        {
            menu.maps = listMenuMaps(collection);
            std::vector<col::MapChoice> choices;
            const GameProfile &player = kGamesInOrder[static_cast<size_t>(std::clamp<int>(game, 0, static_cast<int>(kGameCount) - 1))];
            const SlotStartable startable = slotStartable(player);
            for (size_t i = 0; i < menu.maps.size(); ++i)
            {
                const CustomMap &map = menu.maps[i];
                col::MapChoice choice;
                choice.name = map.name;
                choice.author = map.author;
                choice.where = map.where;
                choice.game = game;
                choice.map = static_cast<int>(i);
                const GameProfile *madeIn = nullptr;
                for (const GameProfile &each : kGamesInOrder)
                    if (map.game == each.id)
                        madeIn = &each;
                choice.builtFor = madeIn ? col::shortGameName(madeIn->id) : map.game;
                choice.playable = mapPlayableIn(map, player.id, startable);
                const auto failure = menu.conversionFailures.find(map.where + "|" + player.id);
                const MapSlot *own = mapSlot(map.game, map.slot);
                if (failure != menu.conversionFailures.end())
                {
                    choice.playable = false;
                    choice.why = failure->second;
                }
                else if (choice.playable)
                    choice.why.clear();
                else if (!own)
                    choice.why = std::string("its files are in no slot ") + col::shortGameName(map.game) + " lends";
                else if (own->park && map.game != player.id)
                    choice.why = std::string("a park: ") + col::shortGameName(map.game) + " only";
                else if (mapPlayableIn(map, player.id))
                    choice.why = kStartNeeds; // a slot the setup cannot start
                else
                    choice.why = "not for this game";
                std::vector<uint8_t> bytes;
                col::Image preview;
                if (map.readPreview(bytes) && col::readPictureFile(bytes, preview))
                    choice.preview = std::make_shared<const col::Image>(std::move(preview));
                else if (!map.preview.empty())
                    std::cout << "[app] custom freeplay: " << map.previewFile().string()
                              << " not read (a PNG, a JPEG, or a BMP of 4, 8, 24 or 32 bits)" << std::endl;
                choices.push_back(std::move(choice));
            }
            // A game none of whose slots the setup starts offers no map (the
            // player page leaves such a player out; a setup changed since it
            // was read is said here).
            const bool starts = freeplayStarts(player);
            menu.mapRows = col::freeplayMapRows(choices, player.name, kMapsFolder, starts,
                                                !starts && mapSlotOf(player.id) ? kNoSlotStarts : std::string());
            menu.mapRows.push_back(folderRow(menu, "Open Maps Folder", kMapsFolder, collection.supportDir / kMapsFolder,
                                             "Put a map's folder in it."));
            menu.mapsPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.mapRows.size(), true);
            menu.mapsFor = game;
            std::cout << "[app] custom freeplay: " << menu.maps.size() << " map(s) in " << sharedModsFolder(collection).string() << " and "
                      << (collection.supportDir / kMapsFolder).string() << ", listed for " << player.id << std::endl;
        }

        // The map made into `player`'s files for the boot about to start
        // (menu.converted), when it was made in another game. False (and
        // why in the menu's notice, the map listed with it) when it does not
        // convert.
        bool prepareMap(MenuScreen &menu, const CustomMap &map, const GameProfile &player)
        {
            menu.converted.reset();
            const MapSlot *target = mapSlotFor(map, player.id, slotStartable(player));
            if (!target)
            {
                menu.notice = map.name + " has no slot in " + std::string(col::shortGameName(player.id)) + ".";
                return false;
            }
            if (!mapNeedsConversion(map, *target))
                return true;
            try
            {
                menu.converted = convertMap(map, *target);
                std::cout << "[app] custom freeplay: " << map.name << " (made in " << map.game << ") converted for " << player.id
                          << " (slot " << target->slot << ") in "
                          << static_cast<long long>(menu.converted->milliseconds) << " ms: " << menu.converted->files.size() << " files, "
                          << menu.converted->bytes << " bytes" << std::endl;
                for (const std::string &line : menu.converted->report)
                    std::cout << "[app] custom freeplay:   " << line << std::endl;
                return true;
            }
            catch (const levels::LevelError &e)
            {
                std::cerr << "[app] custom freeplay: " << map.name << " could not be made into " << player.id << "'s level files: " << e.what()
                          << std::endl;
                menu.conversionFailures[map.where + "|" + player.id] = e.what();
                menu.notice = map.name + " did not convert.";
                return false;
            }
        }

        // "Last played <date>, <time> played" under a game's name: when its
        // native save file (<games/<id>>/Saves/<profile>.sav, the profile its
        // own settings' [saves] profile) was last written, and how long that
        // profile has been played (collection/save_profiles.h); "" with no save.
        std::string lastPlayedLine(const AppPaths &collection, const GameProfile &game)
        {
            const std::filesystem::path dir = collection.supportDir / "games" / game.id;
            Settings settings;
            loadCollectionSettings(g_appSettingsFile, dir / "settings.ini", settings);
            std::error_code ec;
            const auto written = std::filesystem::last_write_time(col::profileSave(dir / "Saves", settings.saveProfile), ec);
            if (ec)
                return {};
            // The file clock's time as the system clock's (C++17 has no cast).
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::string played = col::playtimeText(col::readPlaytime(col::profilePlaytime(dir / "Saves", settings.saveProfile)));
            return col::lastPlayedText(std::chrono::system_clock::to_time_t(system)) + (played.empty() ? "" : ", " + played + " played");
        }

        void refreshLastPlayed(MenuScreen &menu, const AppPaths &collection)
        {
            for (size_t i = 0; i < kGameCount && i < menu.tiles.size(); ++i)
                menu.tiles[i].lastPlayed = lastPlayedLine(collection, kGamesInOrder[i]);
        }

        // The font the app ships (collection/vector_font.h): from the app's
        // resources folder, or on Android from the APK's assets (android_assets.h:
        // read from the APK itself, not by SDL's relative path, which a file in
        // the internal storage would stand in for).
        bool loadShippedFont(col::BitmapFont &font, std::string &from)
        {
#if defined(__ANDROID__)
            if (std::vector<uint8_t> ttf; zx_android::readAsset(col::kShippedFontFile, ttf) &&
                                          col::bakeFont(ttf, col::kShippedFontPixels, font))
            {
                from = std::string("the APK's assets/") + col::kShippedFontFile;
                return true;
            }
#endif
            std::vector<std::string> places = {(platform::resourceFolder(g_argv0) / col::kShippedFontFile).string()};
            for (const std::string &place : places)
            {
                size_t size = 0;
                void *data = SDL_LoadFile(place.c_str(), &size);
                if (!data)
                    continue;
                const std::vector<uint8_t> ttf(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
                SDL_free(data);
                if (col::bakeFont(ttf, col::kShippedFontPixels, font))
                {
                    from = place;
                    return true;
                }
            }
            return false;
        }

        // The menu's text: Aggressive Inline's menu font from its files, or
        // while AI has none the font the app ships, or SDL's built-in one.
        void loadMenuFont(MenuScreen &menu)
        {
            for (const GameProfile &game : kGamesInOrder)
            {
                if (std::string(game.id) != col::kFontGame)
                    continue;
                std::vector<uint8_t> file, page;
                col::Image pageImage;
                col::BitmapFont font;
                const std::filesystem::path disc = discFolder(g_argv0, game);
                const bool ok = readWholeFile(disc / col::kFontFile, file) && readWholeFile(disc / col::kFontPage, page) &&
                                col::readBmpImage(page, pageImage) && col::readFont(file, std::move(pageImage), font);
                std::cout << "[app] menu: font " << (disc / col::kFontFile).string() << (ok ? "" : " (not read: the built-in one)")
                          << std::endl;
                if (ok)
                {
                    menu.font = std::move(font);
                    return;
                }
            }
            std::string from;
            const bool shipped = loadShippedFont(menu.font, from);
            std::cout << "[app] menu: font " << (shipped ? from : std::string("(the built-in one: no ") + col::kShippedFontFile + ")") << std::endl;
            if (!shipped)
                menu.font = builtInFont();
        }

        // Each game's front end from its files as they are now (collection/
        // front_art.h; a game not set up has nothing from its disc), its
        // card, and its menu music and sounds.
        void loadFrontEnd(MenuScreen &menu)
        {
            const auto started = std::chrono::steady_clock::now();
            auto games = std::make_shared<std::vector<col::FrontGame>>();
            for (const GameProfile &game : kGamesInOrder)
            {
                const bool has = gameHasFiles(g_argv0, game);
                const std::filesystem::path disc = has ? discFolder(g_argv0, game) : std::filesystem::path();
                // (BMX XXX's Xbox extras: the releases folder the app was built with)
                const std::filesystem::path extras =
                    std::string_view(game.id) == "bmx" ? gameResources(g_argv0, game) / "releases" : std::filesystem::path();
                games->push_back(col::loadFrontGame(game.id, disc, extras));
                const col::FrontGame &front = games->back();
                std::string missing;
                for (const std::string &file : front.missing)
                    missing += (missing.empty() ? "" : ", ") + file;
                std::cout << "[app] menu: " << game.id << (front.installed ? "" : " not set up") << ": " << front.riders.size()
                          << " riders, " << front.loops.size() << " movies" << (front.still.empty() ? "" : ", a still")
                          << (front.intro.empty() ? "" : ", its logo") << (missing.empty() ? "" : " (not found: " + missing + ")") << std::endl;
                if (has)
                {
                    std::string error;
                    if (!menu.audio.load(game.id, disc, &error))
                        std::cout << "[app] menu: " << game.id << "'s menu sound: " << error << std::endl;
                }
            }
            std::vector<col::Tile> tiles;
            for (size_t i = 0; i < kGameCount; ++i)
            {
                tiles.push_back(menuTile(kGamesInOrder[i], (*games)[i]));
                if (i < menu.tiles.size())
                    tiles.back().lastPlayed = menu.tiles[i].lastPlayed;
            }
            menu.tiles = std::move(tiles);
            // Custom Freeplay's players: the games whose maps start now.
            menu.players.clear();
            menu.playersNote.clear();
            for (size_t i = 0; i < kGameCount; ++i)
                if (freeplayStarts(kGamesInOrder[i]))
                    menu.players.push_back(static_cast<int>(i));
                else if (mapSlotOf(kGamesInOrder[i].id))
                    menu.playersNote += (menu.playersNote.empty() ? "" : " ") + std::string(kGamesInOrder[i].name) + kPlayerNeeds;
            menu.playerLayout = col::layout(MenuScreen::kWidth, MenuScreen::kHeight, menu.players.size());
            menu.front = games;
            menu.scene.setGames(games);
            menu.heads.clear();
            std::cout << "[app] menu: front end read in "
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << " ms" << std::endl;
        }

        // The setup rows' help: Android takes no drop. (The formats are the
        // picker's: ISO, BIN/CUE, CHD, CSO, ZSO.)
#if defined(__ANDROID__)
        constexpr const char *kFindHelp = "Choose the folder with your discs.";
        constexpr const char *kImageHelp = "Choose the disc image.";
        constexpr const char *kFolderHelp = "Choose the disc's folder.";
        constexpr const char *kBootImageHelp = " disc image.";
#else
        constexpr const char *kFindHelp = "Choose the folder with your discs, or drop it here.";
        constexpr const char *kImageHelp = "Choose the disc image, or drop it here.";
        constexpr const char *kFolderHelp = "Choose the disc's folder, or drop it here.";
        constexpr const char *kBootImageHelp = " disc image, or drop it here.";
#endif

        // What the Games page says of a game's files.
        std::string gameStatusLine(const GameProfile &game, const InstalledCheck &check)
        {
            switch (check.status)
            {
            case InstalledCheck::Status::Ok:
                return "From " + sourceName(check.source) + ", " + sizeText(check.bytes);
            case InstalledCheck::Status::ByHand:
                return "Files added by hand";
            case InstalledCheck::Status::Damaged:
                return check.problems.empty() ? "Damaged, set it up again" : "Damaged, " + check.problems.front();
            case InstalledCheck::Status::CodeMissing:
                return "Game code not ready";
            case InstalledCheck::Status::NotInstalled:
                break;
            }
            return gameHasFiles(g_argv0, game) ? "Included" : "Not set up";
        }

        void startJob(MenuScreen &menu, std::string title, std::function<void(SetupJob &)> work);

        // A game's part of a setup of the discs found, as its status row
        // shows it while the setup runs ("Waiting", "Copying files 45%").
        std::string gamePartText(MenuScreen &menu, int index, float *done, col::SettingsRow::Mark *mark)
        {
            using Part = SetupJob::GamePart;
            *done = -1.0f;
            *mark = col::SettingsRow::Mark::None;
            if (!menu.job)
                return {};
            std::lock_guard lock(menu.job->mutex);
            if (menu.job->parts.empty())
                return {};
            const Part &part = menu.job->parts[static_cast<size_t>(index)];
            const SetupProgress &p = menu.job->progress;
            switch (part.state)
            {
            case Part::Waiting:
                *done = 0.0f;
                *mark = col::SettingsRow::Mark::Box;
                return "Waiting";
            case Part::Running:
                *mark = col::SettingsRow::Mark::Box;
                if (menu.job->cancel)
                    return "Stopping"; // (the step under way ends first: a compile, the recompiler)
                *done = p.total ? static_cast<float>(static_cast<double>(p.done) / static_cast<double>(p.total)) : 0.0f;
            {
                // (the step in a word or two, to fit the line's column)
                const std::string step = p.step.find("code") != std::string::npos   ? "Building code"
                                         : p.step.find("Copy") != std::string::npos ? "Copying"
                                         : p.step.find("Check") != std::string::npos ? "Checking"
                                         : p.step.empty()                          ? "Starting"
                                                                                   : "Making extras";
                return p.total ? step + " " + std::to_string(static_cast<int>(p.done * 100u / p.total)) + "%" : step;
            }
            case Part::Done:
                *done = 1.0f;
                *mark = col::SettingsRow::Mark::Tick;
                return "Done";
            case Part::Failed:
                *mark = col::SettingsRow::Mark::Cross;
                return "Failed";
            case Part::Stopped:
                return "Stopped";
            case Part::None:
                break;
            }
            return {};
        }

        // What the search found for a game, as a block of the list: the
        // game's own line (its name, its tick: set it up from these discs),
        // then under it a line for each kind of disc it has (its PS2 disc,
        // then the extras discs of its releases: none listed where it has
        // none) and what was found of it, the disc's file the chosen line's
        // help. A game with no PS2 disc found has "Choose a File" under it.
        void foundRows(MenuScreen &menu, size_t index)
        {
            const DiscSearch &search = menu.found.search;
            const GameProfile &game = kGamesInOrder[index];
            const FoundDisc *ps2 = search.ps2Of(static_cast<int>(index));
            const FoundDiscs &plan = menu.found.plans[index];
            std::error_code ec;
            const bool hasDisc = std::filesystem::is_directory(gameFolder(game) / "disc", ec);
            const std::vector<ex::RecordedDisc> recorded = ex::recordedExtras(gameFolder(game));

            col::SettingsRow own;
            own.steady = true;
            own.label = col::shortGameName(game.id);
            if (plan.game >= 0)
            {
                own.kind = col::SettingsRow::Check;
                own.help = "Set it up with these discs.";
                own.current = [&menu, index]() { return menu.found.ticked[index]; };
                own.apply = [&menu, index](int value) { menu.found.ticked[index] = value; };
            }
            else
            {
                own.kind = col::SettingsRow::Note;
                own.selectable = true;
                own.help = hasDisc ? "Set up already." : "Its PS2 disc is needed first.";
                own.summary = [hasDisc]() { return std::string(hasDisc ? "Set up" : ""); };
            }
            menu.gameRows.push_back(std::move(own));

            auto line = [&](const std::string &kind, const std::string &state, const std::string &help)
            {
                col::SettingsRow out;
                out.kind = col::SettingsRow::Note;
                out.steady = true;
                out.selectable = true;
                out.indent = 1;
                out.label = kind;
                out.summary = [state]() { return state; };
                out.help = help;
                menu.gameRows.push_back(std::move(out));
            };
            if (ps2)
                line("PS2 disc", hasDisc && plan.ps2.empty() ? "Set up" : "Found", ps2->name + ", " + sizeText(ps2->bytes));
            else if (const FoundDisc *other = search.otherReleaseOf(static_cast<int>(index)); other && !hasDisc)
                line("PS2 disc", other->status == SourceCheck::Status::OtherRelease ? "Other release" : "Other version", other->message);
            else
                line("PS2 disc", hasDisc ? "Set up" : "Not found", hasDisc ? "" : "Not in " + menu.found.where + ".");
            for (const ex::ExtrasDisc *kind : ex::extrasDiscsOf(game.id))
            {
                const bool xbox = std::string(kind->release) == "xbox";
                const std::string label = xbox ? "Xbox extras" : "GameCube extras";
                const FoundDisc *found = nullptr;
                for (const FoundDisc *disc : search.extrasOf(game.id))
                    if (disc->extras == kind)
                        found = disc;
                const bool made = std::any_of(recorded.begin(), recorded.end(),
                                              [&](const ex::RecordedDisc &r) { return r.release == kind->release; });
                if (found)
                    line(label, made && std::find(plan.extras.begin(), plan.extras.end(), found->source) == plan.extras.end() ? "Made" : "Found",
                         found->name);
                else
                    line(label, made ? "Made" : "Not found", made ? "" : "Not in " + menu.found.where + ".");
            }
            if (!ps2 && !hasDisc)
            {
                col::SettingsRow choose;
                choose.kind = col::SettingsRow::Action;
                choose.steady = true;
                choose.indent = 1;
                choose.label = "Choose a File";
                choose.help = kImageHelp;
                choose.activate = [&menu, index]()
                {
                    if (!askForDisc(static_cast<int>(index), false))
                        menu.notice = "Drop the disc image on this window.";
                    return col::MenuAction{};
                };
                menu.gameRows.push_back(std::move(choose));
            }
        }

        // The Games page's rows for the games' state now: the search for
        // every game's discs at the top (Find Discs; once it found some, Set
        // Up Games for those ticked), then each game's own.
        void buildGameRows(MenuScreen &menu)
        {
            menu.gameRows.clear();
            menu.gamesSubtitle.clear();
            if (menu.job && menu.job->settingUpFound)
                menu.gamesSubtitle = menu.job->updating ? "Updating..." : "Setting up...";
            else if (menu.found.searched)
            {
                size_t found = 0;
                for (size_t i = 0; i < kGameCount; ++i)
                    found += menu.found.search.ps2Of(static_cast<int>(i)) ? 1u : 0u;
                menu.gamesSubtitle = found == 0                                     ? std::string("No game found")
                                     : found == kGameCount && kGameCount == 3u      ? std::string("All three games found")
                                     : found == kGameCount                          ? std::string("Every game found")
                                                                                    : std::to_string(found) + " of " + std::to_string(kGameCount) + " games found";
            }
            {
                auto top = [&](col::SettingsRow::Kind kind, const std::string &label, const std::string &help,
                               std::function<std::string()> summary, std::function<void()> act)
                {
                    col::SettingsRow row;
                    row.kind = kind;
                    row.label = label;
                    row.help = help;
                    row.summary = std::move(summary);
                    if (act)
                        row.activate = [act]()
                        {
                            act();
                            return col::MenuAction{};
                        };
                    menu.gameRows.push_back(std::move(row));
                };
                const bool anyPlan = std::any_of(menu.found.plans.begin(), menu.found.plans.end(),
                                                 [](const FoundDiscs &plan) { return plan.game >= 0; });
                if (menu.job && menu.job->settingUpFound)
                    top(col::SettingsRow::Action, "Stop",
                        menu.job->updating ? "Stops updating. What is done stays; the rest is done the next time."
                                           : "Stops setting up. Games already done stay set up.",
                        {}, [&menu]() { menu.job->cancel = true; });
                else if (menu.found.searched && anyPlan)
                    top(col::SettingsRow::Action, "Set Up Games", "Copies each disc, then gets its game ready.", {},
                        [&menu]() { menu.request = {MenuScreen::Request::SetUpFound, -1}; });
                // (the list: its one action; a folder of nothing new: another folder)
                if (!(menu.job && menu.job->settingUpFound) && (!menu.found.searched || !anyPlan))
                    top(col::SettingsRow::Action, "Find Discs", kFindHelp, [&menu]() { return menu.found.where; },
                        [&menu]()
                        {
                            if (!askForDisc(-1, true))
                                menu.notice = "Drop the folder with your discs on this window.";
                        });
            }
            for (size_t i = 0; i < kGameCount; ++i)
            {
                const GameProfile &game = kGamesInOrder[i];
                const InstalledCheck &check = menu.gameStatus[i];
                const int index = static_cast<int>(i);
                // (the discs found listed: the game's own line stands for its heading)
                const bool listing = menu.found.searched && i < menu.found.plans.size();
                const bool settingUp = menu.job && menu.job->settingUpFound;
                col::SettingsRow heading;
                heading.kind = col::SettingsRow::Heading;
                heading.label = game.title;
                if (!listing && !settingUp)
                    menu.gameRows.push_back(heading);
                col::SettingsRow status;
                status.kind = col::SettingsRow::Note;
                status.label = gameStatusLine(game, check);
                // (while the discs found are set up: one line a game, its name,
                // its part and a bar)
                if (settingUp)
                {
                    status.summary = [&menu, index, line = status.label]()
                    {
                        float done;
                        col::SettingsRow::Mark mark;
                        const std::string part = gamePartText(menu, index, &done, &mark);
                        return part.empty() ? line : part;
                    };
                    status.label = col::shortGameName(game.id);
                    status.steady = true;
                }
                status.progress = [&menu, index]()
                {
                    float done;
                    col::SettingsRow::Mark mark;
                    gamePartText(menu, index, &done, &mark);
                    return done;
                };
                status.mark = [&menu, index]()
                {
                    float done;
                    col::SettingsRow::Mark mark;
                    gamePartText(menu, index, &done, &mark);
                    return mark;
                };
                // The discs found listed (a compact page: the list in one view):
                // no status; while they are set up, each game's status alone
                // (its progress).
                const bool installed = check.status != InstalledCheck::Status::NotInstalled && check.status != InstalledCheck::Status::ByHand;
                if (!listing || settingUp)
                    menu.gameRows.push_back(status);
                if (settingUp)
                    continue;
                if (!menu.failed[i].empty())
                {
                    col::SettingsRow failed;
                    failed.kind = col::SettingsRow::Note;
                    failed.label = menu.failed[i];
                    failed.mark = []() { return col::SettingsRow::Mark::Cross; };
                    menu.gameRows.push_back(std::move(failed));
                }
                if (listing)
                    foundRows(menu, i);
                auto action = [&](const std::string &label, const std::string &help, std::function<void()> act)
                {
                    col::SettingsRow row;
                    row.kind = col::SettingsRow::Action;
                    row.label = label;
                    row.help = help;
                    row.activate = [act]()
                    {
                        act();
                        return col::MenuAction{};
                    };
                    menu.gameRows.push_back(std::move(row));
                };
                if (listing)
                    continue;
                action("Set Up From Image", kImageHelp,
                           [&menu, index]()
                           {
                               if (!askForDisc(index, false))
                                   menu.notice = "Drop the disc image on this window.";
                           });
                action("Set Up From Folder", kFolderHelp,
                       [&menu, index]()
                       {
                           if (!askForDisc(index, true))
                               menu.notice = "Drop the disc folder on this window.";
                       });
                if (!installed)
                    continue;
                action("Check Files", "Compare every file with the disc.",
                       [&menu, index]() { menu.request = {MenuScreen::Request::CheckEveryFile, index}; });
                if (sourceReadable(check.source))
                    action("Repair From " + sourceName(check.source), "Copy the files again from the disc.",
                           [&menu, index]() { menu.request = {MenuScreen::Request::Repair, index}; });
                action("Remove Files", "Saves and settings stay.",
                       [&menu, index]() { menu.request = {MenuScreen::Request::Remove, index}; });
                // Its extras: what the other releases' discs add (collection/extras/setup.h).
                const std::vector<const ex::ExtrasDisc *> extrasDiscs = ex::extrasDiscsOf(game.id);
                if (extrasDiscs.empty())
                    continue;
                const std::vector<ex::RecordedDisc> recorded = ex::recordedExtras(gameFolder(game));
                col::SettingsRow extras;
                extras.kind = col::SettingsRow::Note;
                extras.label = "Extras: none";
                for (size_t r = 0; r < recorded.size(); ++r)
                    extras.label = (r ? extras.label + ", " : std::string("Extras from ")) +
                                   (recorded[r].release == "xbox" ? "Xbox" : "GameCube") + " (" +
                                   sourceName(pathFromUtf8(recorded[r].source)) + ")";
                menu.gameRows.push_back(extras);
                for (const ex::ExtrasDisc *disc : extrasDiscs)
                {
                    const bool xbox = std::string(disc->release) == "xbox";
                    action(xbox ? "Add Xbox Disc" : "Add GameCube Disc",
                           std::string("Extras from ") + disc->title + ": its image, checked, then made into the game's files.",
                           [&menu, index]()
                           {
                               if (!askForExtrasDisc(index))
                                   menu.notice = "Drop the disc image on this window.";
                           });
                }
                if (!recorded.empty())
                    action("Remove Extras", "The extras made from those discs; the discs stay.",
                           [&menu, index]()
                           {
                               startJob(menu, std::string("Removing ") + kGamesInOrder[static_cast<size_t>(index)].title + "'s extras",
                                        [index](SetupJob &job)
                                        {
                                            const GameProfile &game = kGamesInOrder[static_cast<size_t>(index)];
                                            const std::filesystem::path dir = gameFolder(game);
                                            std::vector<ex::ExtrasSource> none;
                                            const ex::ExtrasResult result = ex::makeExtras(
                                                game.id, dir, none, ex::engineXboxModelMaker(), extrasProgress(job.reporter()));
                                            std::lock_guard lock(job.mutex);
                                            job.messages.push_back(result.ok ? std::string(game.title) + ": the extras are removed."
                                                                             : result.message);
                                            job.changed.push_back(index);
                                        });
                           });
            }
            if (menu.found.searched || (menu.job && menu.job->settingUpFound))
            {
                menu.gamesPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.gameRows.size());
                return;
            }
            // Every game's saves and settings in one file: to another device,
            // or to this app from another copy of it.
            col::SettingsRow saves;
            saves.kind = col::SettingsRow::Heading;
            saves.label = "Saves and Settings";
            menu.gameRows.push_back(saves);
            auto transfer = [&menu](const char *label, const char *help, bool exporting)
            {
                col::SettingsRow row;
                row.kind = col::SettingsRow::Action;
                row.label = label;
                row.help = help;
                row.activate = [&menu, exporting]()
                {
                    if (!askForTransfer(exporting))
                        menu.notice = "There is no window for a file dialog.";
                    return col::MenuAction{};
                };
                menu.gameRows.push_back(std::move(row));
            };
            transfer("Export to a File", "Every game's saves and settings in one file.", true);
            transfer("Import From a File", "Saves and settings from an export file.", false);
            menu.gamesPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.gameRows.size());
        }

        // Whether any game has its files (set up, or bundled with the app).
        bool anyGameSetUp()
        {
            return std::any_of(kGamesInOrder.begin(), kGamesInOrder.end(), [](const GameProfile &game) { return gameHasFiles(g_argv0, game); });
        }

        // Each game's files checked again (quickly: sizes and the boot ELF),
        // and the Games page made for what they are.
        void refreshGames(MenuScreen &menu)
        {
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            menu.gameStatus.clear();
            for (const GameProfile &game : kGamesInOrder)
                menu.gameStatus.push_back(checkInstalled(gameFolder(game), setupGame(game), false, code.get()));
            buildGameRows(menu);
        }

        // After a setup: the tiles' pictures and the font from the files
        // there are now.
        void refreshAfterSetup(MenuScreen &menu)
        {
            loadFrontEnd(menu);
            loadMenuFont(menu);
            refreshGames(menu);
        }

        // A setup started off this thread: `work` reports through the
        // job's progress and gives its result's words.
        void startJob(MenuScreen &menu, std::string title, std::function<void(SetupJob &)> work)
        {
            menu.job = std::make_unique<SetupJob>();
            SetupJob *job = menu.job.get();
            job->title = std::move(title);
            std::cout << "[setup] " << job->title << std::endl;
            job->worker = std::thread([job, work = std::move(work)]()
            {
                work(*job);
                job->finished = true;
            });
        }

        // The discs picked in a dialog or dropped on the window, set up one
        // after another.
        void startSetup(MenuScreen &menu, std::vector<std::pair<int, std::string>> picks)
        {
            picks = withoutPairedCues(std::move(picks));
            startJob(menu, "Setting up from " + sourceName(pathFromUtf8(picks.front().second)),
                     [picks](SetupJob &job)
                     {
                         for (const auto &[game, path] : picks)
                         {
                             // A folder of several games' discs, picked from one game's row
                             // (Android has no drop): each disc for the game it is.
                             const std::vector<std::filesystem::path> sources = discSources(pathFromUtf8(path));
                             const int wanted = sources.size() > 1 ? -1 : game;
                             for (const std::filesystem::path &source : sources)
                             {
                                 if (job.cancel)
                                     return;
                                 int changed = -1;
                                 bool ok = false;
                                 const std::string message = setUpFrom(source, wanted, job.reporter(), changed, ok);
                                 std::lock_guard lock(job.mutex);
                                 job.messages.push_back(message);
                                 if (changed >= 0)
                                     job.changed.push_back(changed);
                             }
                         }
                     });
        }

        // Every game's discs looked for in the places picked or dropped (the
        // Games page lists them when the search ends: takeSearch).
        void startSearch(MenuScreen &menu, std::vector<std::filesystem::path> places)
        {
            std::string where;
            for (const std::filesystem::path &place : places)
                where += (where.empty() ? "" : ", ") + sourceName(place);
            startJob(menu, "Looking for discs", [places, where](SetupJob &job)
            {
                const auto started = std::chrono::steady_clock::now();
                const SetupProgressFn report = job.reporter();
                DiscSearch search = findDiscs(places, setupGames(), extrasOpener(), [&report](const SearchProgress &p)
                {
                    SetupProgress shown;
                    shown.step = "Looking for discs";
                    shown.file = p.at;
                    return report(shown);
                });
                for (const FoundDisc &disc : search.discs)
                    std::cout << "[setup] found " << utf8Of(disc.source) << ": " << disc.message << std::endl;
                std::cout << "[setup] looked at " << search.looked << " files and folders in " << where << " ("
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s"
                          << (search.cancelled ? ", stopped" : "") << ")" << std::endl;
                std::lock_guard lock(job.mutex);
                job.found = std::move(search);
                job.where = where;
            });
        }

        // A search ended: what it found, added to what the searches before
        // found, each game's plan made again and ticked when it does
        // anything. The player's words for it.
        std::string takeSearch(MenuScreen &menu, DiscSearch search, const std::string &where)
        {
            const bool stopped = search.cancelled;
            if (!menu.found.searched)
                menu.found = {};
            menu.found.where = menu.found.searched ? menu.found.where + ", " + where : where;
            menu.found.searched = true;
            menu.found.search.add(std::move(search));
            menu.found.plans.clear();
            menu.found.ticked.clear();
            for (size_t i = 0; i < kGameCount; ++i)
            {
                menu.found.plans.push_back(plannedSetup(menu.found.search, i));
                menu.found.ticked.push_back(menu.found.plans.back().game >= 0 ? 1 : 0);
            }
            std::fill(menu.failed.begin(), menu.failed.end(), std::string());
            // (what it found: the page's subtitle, buildGameRows)
            return stopped ? "Stopped." : "";
        }

        // The games ticked among the discs found, set up one after another,
        // each game's part on its row.
        void startFoundSetup(MenuScreen &menu)
        {
            std::vector<FoundDiscs> plan;
            for (size_t i = 0; i < menu.found.plans.size(); ++i)
                if (menu.found.ticked[i] && menu.found.plans[i].game >= 0)
                    plan.push_back(menu.found.plans[i]);
            if (plan.empty())
            {
                menu.notice = "Tick a game to set it up.";
                return;
            }
            startJob(menu, "Setting up " + std::to_string(plan.size()) + (plan.size() == 1 ? " game" : " games"), [plan](SetupJob &job)
            {
                using Part = SetupJob::GamePart;
                {
                    std::lock_guard lock(job.mutex);
                    job.parts.assign(kGameCount, Part{});
                    for (const FoundDiscs &found : plan)
                        job.parts[static_cast<size_t>(found.game)].state = Part::Waiting;
                }
                size_t done = 0;
                const auto started = std::chrono::steady_clock::now();
                for (const FoundDiscs &found : plan)
                {
                    Part &part = job.parts[static_cast<size_t>(found.game)];
                    if (job.cancel)
                    {
                        std::lock_guard lock(job.mutex);
                        part.state = Part::Stopped;
                        continue;
                    }
                    {
                        std::lock_guard lock(job.mutex);
                        part.state = Part::Running;
                        job.progress = {};
                    }
                    bool ok = false;
                    std::string said = setUpFound(found, job.reporter(), ok);
                    std::lock_guard lock(job.mutex);
                    part.state = ok ? Part::Done : job.cancel ? Part::Stopped : Part::Failed;
                    part.said = std::move(said);
                    job.changed.push_back(found.game);
                    done += ok ? 1u : 0u;
                }
                std::cout << "[setup] " << done << " of " << plan.size() << " games set up from the discs found ("
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
                std::lock_guard lock(job.mutex);
                job.messages.push_back(job.cancel ? std::string("Stopped.")
                                       : done == plan.size()
                                           ? (done == 1 ? std::string("1 game set up.") : std::to_string(done) + " games set up.")
                                           : std::to_string(done) + " of " + std::to_string(plan.size()) + " games set up.");
            });
            menu.job->settingUpFound = true;
            buildGameRows(menu);
        }

        // What an update leaves to do for a game set up before it (one with
        // its disc's files in its folder): its code, when the code there is
        // not this app's, and the parts of its extras whose key differs
        // (collection/extras/setup.h). Read from the files' keys alone: no
        // disc is opened. `games`: those games (indexes), none: every game.
        struct UpdateWork
        {
            int game = -1;
            bool code = false;
            ex::StaleExtras extras;
        };
        std::vector<UpdateWork> pendingUpdates(const std::vector<int> &games = {})
        {
            std::vector<UpdateWork> out;
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            for (size_t i = 0; i < kGameCount; ++i)
            {
                if (!games.empty() && std::find(games.begin(), games.end(), static_cast<int>(i)) == games.end())
                    continue;
                const GameProfile &game = kGamesInOrder[i];
                const std::filesystem::path dir = gameFolder(game);
                std::error_code ec;
                if (!std::filesystem::is_regular_file(installedDiscFolder(dir) / "SYSTEM.CNF", ec))
                    continue;
                UpdateWork work;
                work.game = static_cast<int>(i);
                work.code = code && !code->ready(setupGame(game), installedDiscFolder(dir), dir);
                work.extras = ex::staleExtras(game.id, dir);
                if (work.code || !work.extras.empty())
                    out.push_back(std::move(work));
            }
            return out;
        }

        // The update's work done on the Set Up Games rows, a game at a time:
        // its code, then its extras. Escape or Stop stops it between steps;
        // what is done stays, the rest is done the next time.
        void startUpdate(MenuScreen &menu, std::vector<UpdateWork> work)
        {
            if (work.empty())
                return;
            std::string names;
            for (size_t i = 0; i < work.size(); ++i)
                names += std::string(i == 0 ? "" : i + 1 == work.size() ? " and " : ", ") +
                         col::shortGameName(kGamesInOrder[static_cast<size_t>(work[i].game)].id);
            for (const UpdateWork &w : work)
            {
                std::string parts;
                for (const std::string &part : w.extras.parts)
                    parts += (parts.empty() ? "" : ", ") + part;
                std::cout << "[update] " << kGamesInOrder[static_cast<size_t>(w.game)].id << ":" << (w.code ? " its code" : "")
                          << (parts.empty() ? "" : " its extras (" + parts + ")")
                          << (parts.empty() && w.extras.unkeyed ? " its extras' keys (as 1.1.0 made them)" : "") << std::endl;
            }
            startJob(menu, "Updating " + names, [work](SetupJob &job)
            {
                using Part = SetupJob::GamePart;
                {
                    std::lock_guard lock(job.mutex);
                    job.parts.assign(kGameCount, Part{});
                    for (const UpdateWork &w : work)
                        job.parts[static_cast<size_t>(w.game)].state = Part::Waiting;
                }
                const auto started = std::chrono::steady_clock::now();
                size_t done = 0;
                std::vector<std::string> asks;
                for (const UpdateWork &w : work)
                {
                    const GameProfile &game = kGamesInOrder[static_cast<size_t>(w.game)];
                    Part &part = job.parts[static_cast<size_t>(w.game)];
                    if (job.cancel)
                    {
                        std::lock_guard lock(job.mutex);
                        part.state = Part::Stopped;
                        continue;
                    }
                    {
                        std::lock_guard lock(job.mutex);
                        part.state = Part::Running;
                        job.progress = {};
                    }
                    bool ok = true;
                    std::string said;
                    if (w.code)
                        ok = updateGameCode(job, game, said);
                    if (ok && !w.extras.empty() && !job.cancel)
                    {
                        said = remakeExtras(game, {}, job.reporter(), ok);
                        if (const size_t ask = said.find("Reconnect "); ask != std::string::npos)
                            asks.push_back(said.substr(ask));
                    }
                    std::lock_guard lock(job.mutex);
                    part.state = job.cancel ? Part::Stopped : ok ? Part::Done : Part::Failed;
                    part.said = std::move(said);
                    job.changed.push_back(w.game);
                    done += ok && !job.cancel ? 1u : 0u;
                }
                std::cout << "[update] " << done << " of " << work.size() << " games updated ("
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
                std::lock_guard lock(job.mutex);
                job.messages.push_back(job.cancel ? std::string("Stopped: the rest is updated the next time.")
                                       : done == work.size() ? std::string("Updated.")
                                                             : std::to_string(done) + " of " + std::to_string(work.size()) +
                                                                   " games updated.");
                for (std::string &ask : asks)
                    job.messages.push_back(std::move(ask));
            });
            menu.job->settingUpFound = true;
            menu.job->updating = true;
            buildGameRows(menu);
        }

        // The saves exported to or imported from the file a dialog gave
        // (its own thread: a document goes through the system's files).
        void startTransfer(MenuScreen &menu)
        {
            std::string exportTo, importFrom;
            {
                std::lock_guard lock(g_transferPicks.mutex);
                exportTo = std::exchange(g_transferPicks.exportTo, {});
                importFrom = std::exchange(g_transferPicks.importFrom, {});
            }
            if (!exportTo.empty())
                startJob(menu, "Exporting the saves", [exportTo](SetupJob &job)
                {
                    SDL_IOStream *out = SDL_IOFromFile(exportTo.c_str(), "wb");
                    TransferResult result;
                    if (!out)
                        result.message = "The export cannot be written there (" + std::string(SDL_GetError()) + ").";
                    else
                    {
                        result = exportSaves(g_supportRoot, [out](uint64_t offset, const void *data, size_t size)
                        {
                            return SDL_SeekIO(out, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) == static_cast<Sint64>(offset) &&
                                   SDL_WriteIO(out, data, size) == size;
                        });
                        if (!SDL_CloseIO(out) && result.ok)
                            result = {false, "The export could not be finished (" + std::string(SDL_GetError()) + ")."};
                    }
                    std::cout << "[saves] export: " << result.message << std::endl;
                    std::lock_guard lock(job.mutex);
                    job.messages.push_back(result.message);
                });
            else if (!importFrom.empty() &&
                     (envSet("ZX_MENU_IMPORT_SAVES") ||
                      platform::ask("Import saves?", "Saves and settings of the same name are replaced.", "Import", "Keep")))
                startJob(menu, "Importing the saves", [importFrom](SetupJob &job)
                {
                    SDL_IOStream *in = SDL_IOFromFile(importFrom.c_str(), "rb");
                    TransferResult result;
                    if (!in)
                        result.message = "The file cannot be read (" + std::string(SDL_GetError()) + ").";
                    else
                    {
                        const Sint64 size = SDL_GetIOSize(in);
                        result = importSaves(g_supportRoot, size > 0 ? static_cast<uint64_t>(size) : 0u,
                                             [in](uint64_t offset, void *data, size_t count)
                                             {
                                                 return SDL_SeekIO(in, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) == static_cast<Sint64>(offset) &&
                                                        SDL_ReadIO(in, data, count) == count;
                                             });
                        SDL_CloseIO(in);
                        if (result.ok)
                            result.message = "Imported " + std::to_string(result.files) + " files. Restart the app to use them.";
                    }
                    std::cout << "[saves] import: " << result.message << std::endl;
                    std::lock_guard lock(job.mutex);
                    job.settingsImported = result.ok;
                    job.messages.push_back(result.message);
                });
        }

        // A Games page row's request, between frames.
        void runRequest(MenuScreen &menu)
        {
            const MenuScreen::Request request = std::exchange(menu.request, {});
            if (request.kind == MenuScreen::Request::SetUpFound)
            {
                startFoundSetup(menu);
                return;
            }
            if (request.kind == MenuScreen::Request::None || request.game < 0 || request.game >= static_cast<int>(kGameCount))
                return;
            const GameProfile &game = kGamesInOrder[static_cast<size_t>(request.game)];
            switch (request.kind)
            {
            case MenuScreen::Request::CheckEveryFile:
                startJob(menu, std::string("Checking ") + game.title, [&game](SetupJob &job)
                {
                    const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
                    const auto started = std::chrono::steady_clock::now();
                    const InstalledCheck check = checkInstalled(gameFolder(game), setupGame(game), true, code.get(), job.reporter());
                    std::cout << "[setup] " << game.id << " full check: " << check.message << " ("
                              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
                    std::lock_guard lock(job.mutex);
                    job.messages.push_back(check.message);
                });
                break;
            case MenuScreen::Request::Repair:
            {
                const std::filesystem::path source = menu.gameStatus[static_cast<size_t>(request.game)].source;
                startSetup(menu, {{request.game, utf8Of(source)}});
                break;
            }
            case MenuScreen::Request::Remove:
                if (platform::ask(std::string("Remove ") + game.title + "'s files?",
                                  "Saves and settings are kept.",
                                  "Remove", "Keep"))
                {
                    std::string error;
                    menu.notice = removeInstalled(gameFolder(game), &error) ? std::string(game.title) + " removed." : error;
                    refreshAfterSetup(menu);
                }
                break;
            case MenuScreen::Request::SetUpFound:
            case MenuScreen::Request::None:
                break;
            }
        }

        // The words for a running setup: what it does, how far, which file,
        // and the button that stops it (`device`'s, as the menu's hints).
        std::string jobLine(SetupJob &job, col::HintDevice device)
        {
            std::lock_guard lock(job.mutex);
            const SetupProgress &p = job.progress;
            if (p.step.empty())
                return job.title + "...";
            std::string line = p.step;
            if (p.total > 0)
                line += " " + std::to_string(static_cast<int>(p.done * 100u / p.total)) + "%";
            if (!p.file.empty())
                line += " (" + p.file + ")";
            const std::string stop = col::buttonHint(device, nullptr, "Stop");
            return stop.empty() ? line : line + "   " + stop;
        }

        // A game's folder in the collection (gamePaths, without making it)
        // and its folder of recordings.
        std::filesystem::path recordingsFolder(const AppPaths &collection, const GameProfile &game)
        {
            return play_recording::folderFor(collection.supportDir / "games" / game.id);
        }

        // The Recordings page: a heading per game with recordings, a row per
        // recording (newest first): its level and when it was made, how
        // long it is; Enter plays it back.
        void refreshRecordings(MenuScreen &menu, const AppPaths &collection)
        {
            menu.recordings.clear();
            menu.recordingRows.clear();
            for (size_t g = 0; g < kGameCount; ++g)
            {
                const GameProfile &game = kGamesInOrder[g];
                std::vector<col::RecordingEntry> entries = col::listRecordings(recordingsFolder(collection, game));
                if (entries.empty())
                    continue;
                col::SettingsRow heading;
                heading.kind = col::SettingsRow::Heading;
                heading.label = game.name;
                menu.recordingRows.push_back(std::move(heading));
                // The levels' names from the game's panel files (the setup's, else the bundled ones).
                const std::filesystem::path extrasPanel = collection.supportDir / "games" / game.id / "extras" / "panel";
                const std::filesystem::path bundledPanel = gameResources(g_argv0, game) / "panel";
                const auto levelTitle = [&](const std::string &level)
                {
                    const std::string title = col::levelTitle(extrasPanel, level);
                    return title != level ? title : col::levelTitle(bundledPanel, level);
                };
                for (col::RecordingEntry &entry : entries)
                {
                    col::SettingsRow row;
                    row.kind = col::SettingsRow::Action;
                    row.label = !entry.levelsNoted   ? std::string("Level not noted")
                                : entry.level.empty() ? std::string("Menus")
                                                      : levelTitle(entry.level);
                    if (entry.levels > 1)
                        row.label += " and " + std::to_string(entry.levels - 1) + (entry.levels == 2 ? " more" : " others");
                    const std::string when = col::startedText(entry.started);
                    row.summary = [text = (when.empty() ? std::string() : when + "   ") + col::lengthText(entry.vsyncs)]() { return text; };
                    row.help = entry.commit != play_recording::buildCommit()
                                   ? "Made by another version of the app: it may play differently."
                               : !entry.ended ? "Plays it back. It was cut short, so it stops early."
                                              : "Plays it back as you played it.";
                    const int index = static_cast<int>(menu.recordings.size());
                    row.activate = [index, g]()
                    {
                        col::MenuAction action;
                        action.kind = col::MenuAction::Play;
                        action.game = static_cast<int>(g);
                        action.recording = index;
                        return action;
                    };
                    menu.recordingRows.push_back(std::move(row));
                    menu.recordings.push_back(std::move(entry));
                }
            }
            if (menu.recordings.empty())
            {
                col::SettingsRow none;
                none.kind = col::SettingsRow::Note;
                none.label = "No recordings yet. Turn on Record Play to record your play.";
                menu.recordingRows.push_back(std::move(none));
            }
            menu.recordingsPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.recordingRows.size());
        }

        // A game's save profile for its next start: kept in its settings.ini
        // ([saves] profile), as a mod's switch is (switchMod).
        void chooseSaveProfile(AppState &state, const AppPaths &collection, const GameProfile &game, const std::string &name)
        {
            std::cout << "[app] saves: " << game.id << " plays " << name << " from its next start" << std::endl;
            if (g_profile && g_game == &game)
            {
                state.settings.saveProfile = name;
                saveSettingsNow(state);
                return;
            }
            const std::filesystem::path file = collection.supportDir / "games" / game.id / "settings.ini";
            Settings settings = gameSettings(state, collection, game);
            settings.saveProfile = name;
            if (!writeTextFile(file, formatSettings(settings, gameSettingsProfile(game.id), SettingsScope::Game)))
                std::cerr << "[app] could not write " << file.string() << std::endl;
        }

        // The Save Profiles page: for each game a heading, then Save Profile
        // (the career its next start plays: each <profile>.sav in its Saves
        // folder and the one its settings.ini names, Career first), New Save
        // Profile (the first free "Career N", chosen) and Copy Save Profile
        // (the chosen one's save and time played copied to "<profile>
        // copy", chosen). A change says so on the notice line and makes the
        // rows again.
        void refreshSaveProfiles(MenuScreen &menu, AppState &state, const AppPaths &collection)
        {
            menu.profileRows.clear();
            for (const GameProfile &game : kGamesInOrder)
            {
                const std::filesystem::path saves = gamePaths(collection, game).savesDir;
                const std::string chosen = gameSettings(state, collection, game).saveProfile;
                const std::vector<std::string> names = col::listProfiles(saves, {chosen});
                col::SettingsRow heading;
                heading.kind = col::SettingsRow::Heading;
                heading.label = game.name;
                menu.profileRows.push_back(std::move(heading));

                col::SettingsRow profile;
                profile.kind = col::SettingsRow::Choice;
                profile.label = "Save Profile";
                profile.choices = names;
                profile.help = "The career it plays from its next start: each profile is one of its own.";
                int at = 0;
                for (size_t i = 0; i < names.size(); ++i)
                    if (zx::saves::safeFileName(names[i], col::kDefaultProfile) == zx::saves::safeFileName(chosen, col::kDefaultProfile))
                        at = static_cast<int>(i);
                profile.current = [at]() { return at; };
                profile.apply = [&menu, &state, &collection, &game, names](int value)
                {
                    if (value < 0 || value >= static_cast<int>(names.size()))
                        return;
                    chooseSaveProfile(state, collection, game, names[static_cast<size_t>(value)]);
                    menu.profilesChanged = true;
                };
                menu.profileRows.push_back(std::move(profile));

                col::SettingsRow create;
                create.kind = col::SettingsRow::Action;
                create.label = "New Save Profile";
                create.help = "A new career beside the others, from its next start.";
                create.activate = [&menu, &state, &collection, &game, names]()
                {
                    const std::string name = col::freeProfileName(names, col::kDefaultProfile);
                    chooseSaveProfile(state, collection, game, name);
                    menu.notice = std::string(col::shortGameName(game.id)) + ": " + name + ", a new career, from its next start.";
                    menu.profilesChanged = true;
                    return col::MenuAction{};
                };
                menu.profileRows.push_back(std::move(create));

                col::SettingsRow copy;
                copy.kind = col::SettingsRow::Action;
                copy.label = "Copy Save Profile";
                copy.help = "The chosen profile's career, copied to play on apart.";
                copy.activate = [&menu, &state, &collection, &game, saves, chosen]()
                {
                    std::string error;
                    const std::string name = col::copyProfile(saves, chosen, &error);
                    if (name.empty())
                        menu.notice = std::string(col::shortGameName(game.id)) + ": " + error;
                    else
                    {
                        chooseSaveProfile(state, collection, game, name);
                        menu.notice = std::string(col::shortGameName(game.id)) + ": " + name + ", a copy of " + chosen + ", from its next start.";
                        menu.profilesChanged = true;
                    }
                    return col::MenuAction{};
                };
                menu.profileRows.push_back(std::move(copy));
            }
            menu.profilesPage = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.profileRows.size());
        }

        void loadMenu(MenuScreen &menu, AppState &state, const AppPaths &collection)
        {
            menu.rows = menuSettingsRows(state);
            {
                // Settings > Recordings, after Record Play: the player's
                // recordings, each played back.
                col::SettingsRow recordings;
                recordings.kind = col::SettingsRow::Page;
                recordings.label = "Recordings";
                recordings.opens = col::MenuPage::Recordings;
                recordings.help = "Watch your recorded play.";
                recordings.summary = [collection]()
                {
                    size_t count = 0;
                    for (const GameProfile &game : kGamesInOrder)
                        count += play_recording::countIn(recordingsFolder(collection, game));
                    return count == 0 ? std::string("None") : std::to_string(count);
                };
                const auto recordPlay = std::find_if(menu.rows.begin(), menu.rows.end(),
                                                     [](const col::SettingsRow &row) { return row.label == "Record Play"; });
                menu.rows.insert(recordPlay == menu.rows.end() ? menu.rows.end() : recordPlay + 1, std::move(recordings));
            }
            {
                // Settings > Mods: the Mods page (each game's mods, on or off).
                col::SettingsRow mods;
                mods.kind = col::SettingsRow::Page;
                mods.label = "Mods";
                mods.opens = col::MenuPage::Mods;
                mods.help = "Switch mods on or off and set their order.";
                mods.summary = [list = menu.mods]() { return col::modsSummary(*list); };
                menu.rows.push_back(std::move(mods));
            }
            {
                // Settings > Games: each game's files from the player's disc.
                col::SettingsRow games;
                games.kind = col::SettingsRow::Page;
                games.label = "Games";
                games.opens = col::MenuPage::Games;
                games.help = "Add, check or remove each game's disc.";
                games.summary = [&menu]()
                {
                    int ready = 0;
                    for (size_t i = 0; i < kGameCount; ++i)
                        ready += gameHasFiles(g_argv0, kGamesInOrder[i]) ? 1 : 0;
                    return std::to_string(ready) + " of " + std::to_string(kGameCount) + " ready";
                };
                menu.rows.push_back(std::move(games));
            }
            {
                // Settings > Save Profiles: each game's careers.
                col::SettingsRow profiles;
                profiles.kind = col::SettingsRow::Page;
                profiles.label = "Save Profiles";
                profiles.opens = col::MenuPage::SaveProfiles;
                profiles.help = "Each game's careers: which one it plays, a new one, a copy.";
                menu.rows.push_back(std::move(profiles));
            }
            menu.page = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.rows.size());
            refreshMods(menu, state, collection);
            loadFrontEnd(menu);
            loadMenuFont(menu);
            refreshGames(menu);
            refreshLastPlayed(menu, collection);
            if (const char *spec = std::getenv("ZX_MENU_SCRIPT"); spec && spec[0])
            {
                std::string error;
                if (menu.script.parse(spec, &error))
                    std::cout << "[app] menu script: " << menu.script.size() << " steps" << std::endl;
                else
                    std::cerr << "[app] ZX_MENU_SCRIPT: " << error << std::endl;
            }
        }

        // The menu's Settings page has the Clean Top Screen row while a
        // bottom screen is up (Need::BottomScreen), after Map Window, as the
        // game's page has it (cleanTopScreenRowFrame): it comes and goes with
        // the bottom screen. Once a menu frame.
        void cleanTopScreenMenuRow(MenuScreen &menu, AppState &state)
        {
            auto labelled = [](const char *label) { return [label](const col::SettingsRow &row) { return row.label == label; }; };
            const auto at = std::find_if(menu.rows.begin(), menu.rows.end(), labelled("Clean Top Screen"));
            const bool up = offered(Need::BottomScreen, platformInstalled());
            if (up == (at != menu.rows.end()))
                return;
            if (up)
                menu.rows.insert(std::find_if(menu.rows.begin(), menu.rows.end(), labelled("Record Play")),
                                 menuRowOf(cleanTopScreenSetting(state)));
            else
                menu.rows.erase(at);
            menu.page = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.rows.size());
        }

        // A path dropped on the window, when it is a mod or a map
        // (collection/mod_install.h): asked about (Add or Replace, Cancel; a
        // menu script's drop is the script's yes) and queued for its copy,
        // or why it is not taken said. False for anything else: the disc
        // search's.
        bool takeDroppedMod(MenuScreen &menu, const AppPaths &collection, const std::filesystem::path &place, bool scripted)
        {
            DropInstall plan = planDropInstall(place, collection.supportDir);
            switch (plan.kind)
            {
            case DropInstall::NotContent:
                return false;
            case DropInstall::NoManifest:
            case DropInstall::Unreadable:
                std::cout << "[app] drop: " << place.string() << ": not added, " << plan.why << std::endl;
                menu.notice = plan.title + " is not added: " + plan.why + ".";
                return true;
            case DropInstall::Mod:
            case DropInstall::Map:
                break;
            }
            const bool map = plan.kind == DropInstall::Map;
            const std::string folder = map ? kMapsFolder : kModsFolder;
            if (plan.inPlace)
            {
                menu.notice = plan.title + " is in the " + folder + " folder already.";
                return true;
            }
            const std::string question = "Add " + plan.title + " to your " + (map ? "maps" : "mods") + "?";
            const std::string detail = plan.replaces ? "The " + plan.to.filename().string() + " already in the " + folder +
                                                           " folder is replaced."
                                       : map ? "It is copied into the Maps folder, for Custom Freeplay."
                                             : "It is copied into the Mods folder. Switch it on or off on the Mods page.";
            if (!scripted && !platform::ask(question, detail, plan.replaces ? "Replace" : "Add", "Cancel"))
            {
                std::cout << "[app] drop: " << place.string() << ": not added (Cancel)" << std::endl;
                return true;
            }
            std::cout << "[app] drop: " << place.string() << ": " << (map ? "a map" : "a mod") << ", to " << plan.to.string()
                      << (plan.replaces ? " (replacing it)" : "") << std::endl;
            menu.installs.push_back(std::move(plan));
            return true;
        }

        // A dropped mod's copy ended (its thread joined, so waited for when
        // it is still running): said, and the pages that list mods and maps
        // read again.
        void endInstall(MenuScreen &menu, AppState &state, const AppPaths &collection, const col::Shell &shell)
        {
            menu.install->worker.join();
            const MenuScreen::Install &done = *menu.install;
            const bool map = done.plan.kind == DropInstall::Map;
            std::cout << "[app] drop: " << done.plan.title << (done.ok ? ": added" : ": not added, " + done.error) << std::endl;
            menu.notice = done.ok ? done.plan.title + " is in your " + (map ? "maps." : "mods.")
                                  : done.plan.title + " is not added: " + done.error + ".";
            menu.install.reset();
            refreshMods(menu, state, collection);
            if (shell.page() == col::MenuPage::FreeplayMap)
                refreshMaps(menu, collection, shell.freeplayGame());
        }

        // The dropped mods' copies: one finished said, the next one started.
        void runInstalls(MenuScreen &menu, AppState &state, const AppPaths &collection, const col::Shell &shell)
        {
            if (menu.install && menu.install->done)
                endInstall(menu, state, collection, shell);
            if (!menu.install && !menu.installs.empty())
            {
                auto install = std::make_unique<MenuScreen::Install>();
                install->plan = std::move(menu.installs.front());
                menu.installs.pop_front();
                menu.notice = "Adding " + install->plan.title + "...";
                MenuScreen::Install *running = install.get();
                install->worker = std::thread(
                    [running, supportDir = collection.supportDir]()
                    {
                        running->ok = installDrop(running->plan, supportDir, &running->error);
                        running->done = true;
                    });
                menu.install = std::move(install);
            }
        }

        // One frame's menu input: keys and gamepad buttons that went down
        // (the left stick as the D-pad), the pointer over the picture, the
        // menu script's step.
        col::MenuInput menuFrameInput(MenuScreen &menu, const col::Shell *shell, bool scripted = true)
        {
            namespace hi = ps2_host_input;
            const ps2_host::InputFrame &frame = ps2_host::input();
            std::vector<int> keys;
            for (int code = 0; code < hi::kKeyCodeCount; ++code)
                if (frame.pressed.test(static_cast<size_t>(code)))
                    keys.push_back(code);
            uint16_t padPressed = 0;
            int padUsed = -1;
            const int pads = ps2_host::gamepadCount();
            menu.padHeld.resize(static_cast<size_t>(std::max(pads, 0)), 0);
            for (int slot = 0; slot < pads; ++slot)
            {
                const hi::GamepadState pad = ps2_host::gamepadState(slot);
                uint16_t held = pad.present ? pad.buttons : 0;
                constexpr float kPush = 0.5f;
                if (pad.present)
                {
                    const float x = pad.axes[hi::kLx], y = pad.axes[hi::kLy];
                    held = static_cast<uint16_t>(held | (x < -kPush ? 1u << hi::kLeft : 0u) | (x > kPush ? 1u << hi::kRight : 0u) |
                                                 (y < -kPush ? 1u << hi::kUp : 0u) | (y > kPush ? 1u << hi::kDown : 0u));
                }
                const uint16_t down = static_cast<uint16_t>(held & ~menu.padHeld[static_cast<size_t>(slot)]);
                padPressed = static_cast<uint16_t>(padPressed | down);
                menu.padHeld[static_cast<size_t>(slot)] = held;
                if (down != 0 && padUsed < 0)
                    padUsed = slot;
            }
            auto padHint = [](int slot)
            {
                switch (ps2_host::gamepadStyle(slot))
                {
                case ps2_host::GamepadStyle::PlayStation:
                    return col::HintDevice::PlayStation;
                case ps2_host::GamepadStyle::Nintendo:
                    return col::HintDevice::Nintendo;
                default:
                    return col::HintDevice::Xbox;
                }
            };
            if (!menu.hintChosen)
            {
                menu.hint = pads > 0 ? padHint(0) : col::HintDevice::Keyboard;
                menu.hintChosen = true;
            }
            if (padUsed >= 0)
                menu.padSlot = padUsed;
            if (menu.padSlot >= pads)
                menu.padSlot = 0;
            menu.pad = pads > 0 ? std::optional<col::HintDevice>(padHint(menu.padSlot)) : std::nullopt;
            if (padUsed >= 0)
                menu.hint = padHint(padUsed);
            else if (frame.touched)
                menu.hint = col::HintDevice::Touch;
            else if (!keys.empty() || std::fabs(frame.mouseDx) + std::fabs(frame.mouseDy) > 12.0f)
                menu.hint = col::HintDevice::Keyboard; // (the mouse too: a clear move, as the game's prompts take it)
            {
                // (game:<id> steps: the cards by id, left to right, and the one chosen)
                col::MenuScript::Cards cards;
                if (shell)
                {
                    for (const GameProfile &game : kGamesInOrder)
                        cards.games.push_back(game.id);
                    if (shell->freeplay())
                        cards.games.push_back("freeplay");
                    if (shell->page() == col::MenuPage::FreeplayPlayer)
                        cards.chosen = shell->freeplayOnBack() ? -1 : shell->freeplayGame();
                    else if (shell->page() == col::MenuPage::Tiles && shell->selection() < static_cast<int>(cards.games.size()))
                        cards.chosen = shell->selection();
                }
                std::string problem;
                if (scripted)
                    menu.script.next(keys, padPressed, cards, &problem, &menu.scriptDrops);
                if (!problem.empty())
                    std::cerr << "[app] menu script: " << problem << std::endl;
            }
            col::MenuInput in = col::menuInput(keys, padPressed);
            {
                // A direction held goes again (col::HoldRepeat): the keys
                // that move, the D-pad and the stick of every gamepad.
                auto held = [&](col::MenuDirection direction, int padButton)
                {
                    for (const uint16_t buttons : menu.padHeld)
                        if (buttons & (1u << padButton))
                            return true;
                    for (int code = 0; code < hi::kKeyCodeCount; ++code)
                        if (frame.held.test(static_cast<size_t>(code)) && col::menuDirectionOfKey(code) == direction)
                            return true;
                    return false;
                };
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                menu.repeat.update(in, held(col::MenuDirection::Left, hi::kLeft), held(col::MenuDirection::Right, hi::kRight),
                                   held(col::MenuDirection::Up, hi::kUp), held(col::MenuDirection::Down, hi::kDown), now);
            }
            // The right mouse button goes back; the wheel's steps (a
            // trackpad's parts of a step added up) scroll.
            in.back = in.back || frame.pressed.test(static_cast<size_t>(hi::kMouseCodeBase + 1));
            menu.wheel += frame.wheel;
            in.scroll = static_cast<int>(menu.wheel);
            menu.wheel -= static_cast<float>(in.scroll);

            // The pointer, from window points to the canvas (shown as large
            // as fits, 4:3, centred).
            int width = 0, height = 0;
            ps2_host::windowSize(width, height);
            if (width > 0 && height > 0)
            {
                const float shownWidth = std::min(static_cast<float>(width), height * 4.0f / 3.0f);
                const float shownHeight = shownWidth * 3.0f / 4.0f;
                const float left = (width - shownWidth) / 2.0f, top = (height - shownHeight) / 2.0f;
                in.pointerX = (frame.mouseX - left) / shownWidth * MenuScreen::kWidth;
                in.pointerY = (frame.mouseY - top) / shownHeight * MenuScreen::kHeight;
                in.pointerMoved = frame.mouseMoved;
                in.click = frame.pressed.test(static_cast<size_t>(hi::kMouseCodeBase));
            }
            return in;
        }

        // ---- the menu's moving parts, sound and bottom screen ----------------------

        // The time the menu's movies and fades count in.
        double menuSeconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The game the menu shows behind it (its movie, riders, music): the
        // chosen card's, on Custom Freeplay's pages the player's, else the
        // game last played (the first before any).
        int shownGame(const col::Shell &shell)
        {
            if (shell.page() == col::MenuPage::FreeplayPlayer || shell.page() == col::MenuPage::FreeplayMap)
                return shell.freeplayGame();
            if (shell.selection() >= 0 && shell.selection() < static_cast<int>(kGameCount))
                return shell.selection();
            return std::max(shell.lastGame(), 0);
        }

        // The menu's sound on SDL's audio thread: MenuAudio's mix.
        void SDLCALL menuSoundCallback(void *userdata, SDL_AudioStream *stream, int additional, int)
        {
            auto *audio = static_cast<col::MenuAudio *>(userdata);
            thread_local std::vector<int16_t> buffer;
            const size_t frames = static_cast<size_t>(std::max(additional, 0)) / 4u;
            if (frames == 0)
                return;
            buffer.resize(frames * 2u);
            audio->render(buffer.data(), frames);
            SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(frames * 4u));
        }

        // The menu's sound out (while the menu or the intro is up; a game
        // has the device to itself).
        void openMenuSound(MenuScreen &menu)
        {
            if (menu.sound || !ps2_host::audioReady())
                return;
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, col::MenuAudio::kRate};
            SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, nullptr);
            if (!stream)
            {
                std::cerr << "[app] menu: no sound stream: " << SDL_GetError() << std::endl;
                return;
            }
            SDL_SetAudioStreamGetCallback(stream, menuSoundCallback, &menu.audio);
            if (!SDL_BindAudioStream(static_cast<SDL_AudioDeviceID>(ps2_host::audioDevice()), stream))
            {
                std::cerr << "[app] menu: the sound stream cannot play: " << SDL_GetError() << std::endl;
                SDL_DestroyAudioStream(stream);
                return;
            }
            menu.sound = stream;
        }

        void closeMenuSound(MenuScreen &menu)
        {
            if (!menu.sound)
                return;
            SDL_DestroyAudioStream(menu.sound); // (its callback is not running afterwards)
            menu.sound = nullptr;
        }

        // The shown game's menu music when Menu Music is on and the game is
        // set up, else none.
        void followMusic(MenuScreen &menu, const AppState &state, int shown)
        {
            const bool set = shown >= 0 && shown < static_cast<int>(menu.front->size()) && (*menu.front)[static_cast<size_t>(shown)].installed;
            const std::string wanted = state.settings.menuMusic && set ? kGamesInOrder[static_cast<size_t>(shown)].id : "";
            if (wanted == menu.music)
                return;
            menu.audio.playMusic(wanted);
            menu.music = wanted;
        }

        // A menu sound of the shown game's (or, when it has none, of the
        // first game set up).
        void menuSound(MenuScreen &menu, int shown, col::MenuSound sound)
        {
            std::string_view game;
            if (shown >= 0 && shown < static_cast<int>(kGameCount) && menu.audio.loaded(kGamesInOrder[static_cast<size_t>(shown)].id))
                game = kGamesInOrder[static_cast<size_t>(shown)].id;
            for (size_t i = 0; game.empty() && i < kGameCount; ++i)
                if (menu.audio.loaded(kGamesInOrder[i].id))
                    game = kGamesInOrder[i].id;
            if (!game.empty())
                menu.audio.play(game, sound);
        }

        // Where the shell is, to tell what an input did (menuSoundOf).
        struct MenuPlace
        {
            col::MenuPage page = col::MenuPage::Tiles;
            int selection = 0, pageRow = 0, pageFirst = 0, freeplayGame = 0, revision = 0;
            bool onBack = false;

            explicit MenuPlace(const col::Shell &shell)
                : page(shell.page()), selection(shell.selection()), pageRow(shell.pageRow()), pageFirst(shell.pageFirst()),
                  freeplayGame(shell.freeplayGame()), revision(shell.revision()), onBack(shell.freeplayOnBack())
            {
            }
        };

        int pageDepth(col::MenuPage page)
        {
            switch (page)
            {
            case col::MenuPage::Tiles:
                return 0;
            case col::MenuPage::Settings:
            case col::MenuPage::FreeplayPlayer:
                return 1;
            case col::MenuPage::Mods:
            case col::MenuPage::Games:
            case col::MenuPage::FreeplayMap:
            case col::MenuPage::Recordings:
            case col::MenuPage::SaveProfiles:
                return 2;
            case col::MenuPage::Mod:
                return 3;
            }
            return 0;
        }

        // The games' own sound for what an input did: a page opened or a
        // game started (select), a page closed (cancel), the selection moved
        // or the wheel scrolled the page (move), a value changed (change), a
        // direction that moved nothing (blocked). None for no input.
        std::optional<col::MenuSound> menuSoundOf(const col::MenuInput &in, const col::MenuAction &action, const MenuPlace &before,
                                                  const col::Shell &shell)
        {
            const MenuPlace after(shell);
            if (action.kind != col::MenuAction::None)
                return col::MenuSound::Select;
            if (after.page != before.page)
                return pageDepth(after.page) > pageDepth(before.page) ? col::MenuSound::Select : col::MenuSound::Cancel;
            if (after.selection != before.selection || after.pageRow != before.pageRow || after.freeplayGame != before.freeplayGame ||
                after.onBack != before.onBack || after.pageFirst != before.pageFirst)
                return col::MenuSound::Move;
            if (after.revision != before.revision)
                return col::MenuSound::Change;
            if (in.confirm || in.click)
                return col::MenuSound::Select;
            if (in.left || in.right || in.up || in.down)
                return col::MenuSound::Blocked;
            return std::nullopt;
        }

        // The Thor's bottom screen while the menu is up (collection/
        // menu_bottom.h): the cards, the chosen game's name, line and
        // roster, Play or Set Up, the pages' buttons. Only where there is a
        // second screen (Android); handed over when it changes.
        void showBottom(MenuScreen &menu, const col::Shell &shell)
        {
#if defined(__ANDROID__)
            const int chosen = std::clamp(shownGame(shell), 0, static_cast<int>(kGameCount) - 1);
            const col::FrontGame &game = (*menu.front)[static_cast<size_t>(chosen)];
            const std::string hint = col::bottomHint(game, menu.pad);
            std::vector<std::string> lines;
            // (a setup or an update running: what it does in place of the cards and Play, each game's part as its
            // row on the Games page says it, "Building code 64%")
            std::string busy, busyLine, busyHint;
            std::vector<int> busyGames;
            std::vector<float> busyDone;
            if (menu.job)
            {
                busy = menu.job->updating ? "Updating..." : menu.job->settingUpFound ? "Setting up..." : menu.job->title + "...";
                if (menu.pad && *menu.pad != col::HintDevice::Touch && *menu.pad != col::HintDevice::Keyboard)
                    busyHint = col::buttonHint(*menu.pad, nullptr, "Stop");
            }
            for (size_t i = 0; i < kGameCount; ++i)
            {
                float done = -1.0f;
                col::SettingsRow::Mark mark = col::SettingsRow::Mark::None;
                std::string part = gamePartText(menu, static_cast<int>(i), &done, &mark);
                if (!part.empty())
                {
                    busyGames.push_back(static_cast<int>(i));
                    busyDone.push_back(done);
                }
                lines.push_back(!part.empty()                        ? std::move(part)
                                : !(*menu.front)[i].installed        ? "Not set up"
                                : menu.tiles[i].lastPlayed.empty()   ? "Not played yet"
                                                                     : menu.tiles[i].lastPlayed);
            }
            if (menu.job && busyGames.empty())
            {
                std::lock_guard lock(menu.job->mutex);
                const SetupProgress &p = menu.job->progress;
                busyLine = p.step.empty() ? std::string() : p.total ? p.step + " " + std::to_string(static_cast<int>(p.done * 100u / p.total)) + "%" : p.step;
            }
            std::string key = std::to_string(chosen) + "|" + hint + "|" + (shell.freeplay() ? "f" : "") + "|" +
                              std::to_string(reinterpret_cast<uintptr_t>(menu.front.get()));
            for (const std::string &line : lines)
                key += "|" + line;
            key += "|" + busy + "|" + busyLine + "|" + busyHint;
            for (const float done : busyDone)
                key += "|" + std::to_string(static_cast<int>(done * 200.0f)); // (the bars move by half a percent)
            if (key == menu.bottomShown)
                return;
            menu.bottomShown = key;
            auto heads = menu.heads.find(chosen);
            if (heads == menu.heads.end())
            {
                const GameProfile &profile = kGamesInOrder[static_cast<size_t>(chosen)];
                auto read = std::make_shared<std::vector<col::Image>>(
                    game.installed ? col::loadRosterHeads(profile.id, discFolder(g_argv0, profile)) : std::vector<col::Image>());
                heads = menu.heads.emplace(chosen, std::move(read)).first;
            }
            auto scene = std::make_shared<col::BottomScene>();
            scene->games = menu.front;
            for (size_t i = 0; i < kGameCount; ++i)
                scene->names.push_back(kGamesInOrder[i].name);
            scene->lines = std::move(lines);
            scene->heads = heads->second;
            scene->font = std::make_shared<col::BitmapFont>(menu.font);
            scene->chosen = chosen;
            scene->freeplay = shell.freeplay();
            scene->pad = menu.pad;
            scene->busy = std::move(busy);
            scene->busyLine = std::move(busyLine);
            scene->busyHint = std::move(busyHint);
            scene->busyGames = std::move(busyGames);
            scene->busyDone = std::move(busyDone);
            col::BottomScreen::shared().show(std::move(scene));
#else
            (void)menu;
            (void)shell;
#endif
        }

        // The bottom screen's taps as the menu's input: a card chooses its
        // game, Play is Enter on it, a button opens its page (or quits).
        // True for Quit.
        bool takeBottomTaps(MenuScreen &menu, col::Shell &shell, col::MenuInput &in)
        {
            bool quit = false;
            for (const col::BottomHit &hit : col::BottomScreen::shared().takeTaps())
            {
                menu.hint = col::HintDevice::Touch;
                switch (hit.kind)
                {
                case col::BottomHit::Card:
                    shell.choose(hit.index);
                    break;
                case col::BottomHit::Play:
                    if (shell.page() == col::MenuPage::Tiles || shell.page() == col::MenuPage::FreeplayPlayer)
                    {
                        shell.choose(shownGame(shell));
                        in.confirm = true;
                    }
                    break;
                case col::BottomHit::Button:
                    switch (hit.button)
                    {
                    case col::BottomButton::CustomFreeplay:
                        shell.showPage(col::MenuPage::FreeplayPlayer);
                        break;
                    case col::BottomButton::Settings:
                        shell.showPage(col::MenuPage::Settings);
                        break;
                    case col::BottomButton::Mods:
                        shell.showPage(col::MenuPage::Mods);
                        break;
                    case col::BottomButton::Quit:
                        quit = true;
                        break;
                    }
                    break;
                case col::BottomHit::None:
                    break;
                }
            }
            return quit;
        }

        // The menu's canvas on the window.
        void presentMenu(MenuScreen &menu, bool changed)
        {
            ps2_host::Frame frame;
            frame.rgba = reinterpret_cast<const uint8_t *>(menu.canvas.image().pixels.data());
            frame.texWidth = frame.width = static_cast<uint32_t>(menu.canvas.width());
            frame.texHeight = frame.height = static_cast<uint32_t>(menu.canvas.height());
            frame.stride = frame.texWidth * 4u;
            frame.changed = changed;
            frame.aspect = 4.0f / 3.0f;
            frame.linearFilter = true;
            ps2_host::present(frame);
            if (changed && !g_firstPictureLogged)
            {
                g_firstPictureLogged = true;
                std::cout << "[app] menu: first picture "
                          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_processStart).count()
                          << " ms after the start" << std::endl;
            }
        }

        // The intro (Settings > Intro Movie): the Z-Axis logo from one set-up
        // game's own developer movie, each launch the next game's, over at
        // the first key, button, click or tap. Not with a menu script (a
        // check's run) or without a window.
        void playIntro(AppState &state, MenuScreen &menu)
        {
            if (!state.settings.introLogo || ps2_host::headless() || (std::getenv("ZX_MENU_SCRIPT") && std::getenv("ZX_MENU_SCRIPT")[0]))
                return;
            const col::MovieClip clip = col::introFor(*menu.front, state.settings.introTurn);
            if (clip.empty())
            {
                std::cout << "[app] intro: no game set up has its logo" << std::endl;
                return;
            }
            ++state.settings.introTurn;
            saveSettingsNow(state);
            col::IntroMovie intro;
            if (!intro.open(clip))
            {
                std::cout << "[app] intro: " << clip.movie.string() << " not played (no picture)" << std::endl;
                return;
            }
            openMenuSound(menu);
            menu.audio.playMovie(intro.sound(), intro.soundFrames());
            std::cout << "[app] intro: " << clip.movie.string() << " to " << clip.end << " s, first picture "
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_processStart).count()
                      << " ms after the start" << std::endl;
            const double started = menuSeconds();
            bool skipped = false;
            while (true)
            {
                const auto frameStart = std::chrono::steady_clock::now();
                ps2_host::pumpEvents();
                if (ps2_host::quitRequested())
                    break;
                windowFrame(state);
                const ps2_host::InputFrame &frame = ps2_host::input();
                const col::MenuInput in = menuFrameInput(menu, nullptr);
                if (frame.pressed.any() || frame.touched || in.confirm || in.back || in.left || in.right || in.up || in.down || in.click)
                {
                    skipped = true;
                    break;
                }
                const double now = menuSeconds();
                if (!intro.update(now))
                    break;
                col::drawIntro(menu.canvas, intro.picture(), menu.font, col::buttonHint(menu.hint, nullptr, "Skip"), intro.black(now));
                presentMenu(menu, true);
                const auto spent = std::chrono::steady_clock::now() - frameStart;
                if (spent < std::chrono::milliseconds(16))
                    std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
            menu.audio.playMovie(nullptr);
            menu.fadeInFrom = menuSeconds();
            std::cout << "[app] intro: " << (skipped ? "skipped" : "ended") << " after " << (menuSeconds() - started) << " s" << std::endl;
        }

        // The chosen card grows to the whole screen as its music fades,
        // before its game boots.
        void launchCard(MenuScreen &menu, const col::Shell &shell, const col::Layout &layout, const std::vector<col::SettingsRow> &rows,
                        const col::PageLayout &page)
        {
            constexpr double kLaunch = 0.35;
            menu.audio.playMusic("");
            menu.music.clear();
            const double started = menuSeconds();
            for (double t = 0.0; t < 1.0; t = (menuSeconds() - started) / kLaunch)
            {
                const double now = menuSeconds();
                menu.scene.update(now);
                col::Backdrop backdrop = menu.scene.backdrop(now);
                backdrop.launch = static_cast<float>(t);
                col::drawMenu(menu.canvas, layout, menu.tiles, menu.font, shell, ZX_APP_NAME, menu.notice, rows, page, menu.hint, backdrop,
                              {}, nullptr, appVersion());
                presentMenu(menu, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        // The menu until a game is chosen or the app quits.
        void runMenu(AppState &state, const AppPaths &collection, col::Shell &shell, MenuScreen &menu)
        {
            ++menu.visits;
            menu.script.beginVisit();
            ps2_host::setWindowTitle(ZX_APP_NAME);
            ps2_host::clearExitToMenu();
            refreshLastPlayed(menu, collection); // the game just left may have saved
            refreshMods(menu, state, collection);
            shell.setFreeplay(state.settings.customFreeplay);
            shell.setFreeplayPlayers(menu.players);
            menu.layout = tilesLayout(shell);
            std::cout << "[app] menu (visit " << menu.visits << ", on "
                      << (shell.selection() == shell.freeplayItem() ? "custom freeplay"
                                                                     : kGamesInOrder[std::min<size_t>(shell.selection(), kGameCount - 1)].id)
                      << ")" << std::endl;
            openMenuSound(menu);
            if (menu.visits > 1)
                menu.fadeInFrom = menuSeconds(); // (back from a game)
            menu.bottomShown.clear();
            if (menu.visits == 1 && shell.page() == col::MenuPage::Tiles && !anyGameSetUp())
            {
                // The first start with no game set up: the Games page, its
                // first row the search for every game's discs.
                std::cout << "[app] menu: no game has files yet: the Games page" << std::endl;
                shell.showPage(col::MenuPage::Games);
            }
            bool redraw = true, shot = true;
            int shown = -1;
            col::HintDevice shownHint = menu.hint;
            std::string shownNotice = menu.notice;
            double drawnAt = 0.0;
            // The rows, their layout and the tiles' layout of the page shown.
            auto rowsOf = [&]() -> const std::vector<col::SettingsRow> &
            {
                switch (shell.page())
                {
                case col::MenuPage::Mods:
                    return menu.modRows;
                case col::MenuPage::Games:
                    return menu.gameRows;
                case col::MenuPage::Mod:
                    return menu.modPageRows;
                case col::MenuPage::FreeplayMap:
                    return menu.mapRows;
                case col::MenuPage::Recordings:
                    return menu.recordingRows;
                case col::MenuPage::SaveProfiles:
                    return menu.profileRows;
                default:
                    return menu.rows;
                }
            };
            auto pageOf = [&]() -> const col::PageLayout &
            {
                switch (shell.page())
                {
                case col::MenuPage::Mods:
                    return menu.modsPage;
                case col::MenuPage::Games:
                    return menu.gamesPage;
                case col::MenuPage::Mod:
                    return menu.modPage;
                case col::MenuPage::FreeplayMap:
                    return menu.mapsPage;
                case col::MenuPage::FreeplayPlayer:
                    return menu.playerPage;
                case col::MenuPage::Recordings:
                    return menu.recordingsPage;
                case col::MenuPage::SaveProfiles:
                    return menu.profilesPage;
                default:
                    return menu.page;
                }
            };
            auto layoutOf = [&]() -> const col::Layout &
            { return shell.page() == col::MenuPage::FreeplayPlayer ? menu.playerLayout : menu.layout; };
            // What moves behind the menu now, with the fade in.
            auto backdropNow = [&](double now)
            {
                col::Backdrop backdrop = menu.scene.backdrop(now);
                if (menu.fadeInFrom >= 0.0)
                {
                    constexpr double kFadeIn = 0.3;
                    backdrop.black = static_cast<float>(std::max(0.0, 1.0 - (now - menu.fadeInFrom) / kFadeIn));
                    if (backdrop.black <= 0.0f)
                        menu.fadeInFrom = -1.0;
                }
                return backdrop;
            };
            auto draw = [&](double now)
            {
                // (a setup of the discs found running: the Games page with no Back, its back button Stop)
                const bool settingUp = menu.job && menu.job->settingUpFound && shell.page() == col::MenuPage::Games;
                col::PageLayout page = pageOf();
                if (settingUp)
                    page.back = {};
                const std::string &notice =
                    menu.notice.empty() && shell.page() == col::MenuPage::FreeplayPlayer ? menu.playersNote : menu.notice;
                col::drawMenu(menu.canvas, layoutOf(), menu.tiles, menu.font, shell, ZX_APP_NAME, notice, rowsOf(), page, menu.hint,
                              backdropNow(now),
                              shell.page() != col::MenuPage::Games ? std::string()
                              : settingUp && menu.job->cancel     ? std::string("Stopping...")
                                                                  : menu.gamesSubtitle,
                              settingUp ? "Stop" : nullptr, appVersion());
                drawnAt = now;
            };
            // ZX_MENU_SHOT_DIR (developers, headless checks): every picture
            // of a change the menu shows (not its movie's pictures), and of
            // each step of a setup, as menu-<visit>-<n>.png.
            std::string jobShotStep, jobShown;
            double jobShotAt = -10.0;
            auto writeShot = [&]()
            {
                const char *dir = std::getenv("ZX_MENU_SHOT_DIR");
                if (!dir || !dir[0])
                    return;
                static int shots = 0;
                const std::filesystem::path file =
                    std::filesystem::path(dir) / ("menu-" + std::to_string(menu.visits) + "-" + std::to_string(++shots) + ".png");
                const col::Image &image = menu.canvas.image();
                ps2_host::writePng(file.string(), reinterpret_cast<const uint8_t *>(image.pixels.data()), static_cast<uint32_t>(image.width),
                                   static_cast<uint32_t>(image.height), static_cast<uint32_t>(image.width) * 4u);
            };
            auto leave = [&]()
            {
                menu.audio.playMusic("");
                menu.music.clear();
                closeMenuSound(menu);
                menu.scene.show(-1, menuSeconds()); // (its movie's decoder goes while a game runs)
                col::BottomScreen::shared().show(nullptr);
            };
            while (shell.state() == col::State::Menu)
            {
                const auto frameStart = std::chrono::steady_clock::now();
                ps2_host::pumpEvents();
                if (ps2_host::quitRequested())
                {
                    std::cout << "[app] menu: window closed" << std::endl;
                    shell.quit();
                    break;
                }
                windowFrame(state);
                cleanTopScreenMenuRow(menu, state);
                zx::settings::runPending();
                if (shell.freeplay() != state.settings.customFreeplay)
                {
                    // The setting changed on the Settings page: the tile comes or goes.
                    shell.setFreeplay(state.settings.customFreeplay);
                    menu.layout = tilesLayout(shell);
                }
                shell.setFreeplayPlayers(menu.players); // (the setup changed on the Games page: read with the front end)
                const double now = menuSeconds();
                menu.scene.show(shownGame(shell), now);
                followMusic(menu, state, shownGame(shell));
                if (menu.scene.update(now))
                    redraw = true;
                {
                    // A still's pan, a pair of riders fading, the fade in:
                    // drawn again at 30 pictures a second.
                    const col::Backdrop moving = menu.scene.backdrop(now);
                    const bool animates = !moving.part.empty() || moving.riderOpacity < 1.0f || menu.fadeInFrom >= 0.0;
                    if (animates && now - drawnAt >= 1.0 / 30.0)
                        redraw = true;
                }
                showBottom(menu, shell);
                // A setup running: its progress on the notice line, Escape
                // stops it, nothing else is taken until it ends.
                if (menu.job)
                {
                    // (a menu script waits while the job runs: its steps count frames)
                    const col::MenuInput in = menuFrameInput(menu, &shell, false);
                    // (the discs found being set up: Stop, the page's one row there, takes Enter and a click too;
                    // the rows show the progress, the notice line nothing)
                    const bool settingUp = menu.job->settingUpFound;
                    const bool onStop = settingUp && !menu.gamesPage.rows.empty() &&
                                        (in.confirm || (in.click && menu.gamesPage.rows.front().contains(in.pointerX, in.pointerY)));
                    if (in.back || onStop)
                        menu.job->cancel = true;
                    // (the job's state: shown on the notice line, or for the discs found on their rows, drawn again
                    // whenever it changes)
                    const std::string jobState = jobLine(*menu.job, menu.hint) + (menu.job->cancel ? " (stopping)" : "");
                    // (the bottom screen's taps act on nothing while it runs: none kept for after it)
                    col::BottomScreen::shared().takeTaps();
                    if (jobState != jobShown)
                    {
                        jobShown = jobState;
                        redraw = true;
                    }
                    std::string line = settingUp ? std::string() : jobState;
                    if (menu.job->finished)
                    {
                        menu.job->worker.join();
                        std::string said;
                        std::optional<DiscSearch> found;
                        std::vector<SetupJob::GamePart> parts;
                        std::string where;
                        bool imported = false;
                        {
                            std::lock_guard lock(menu.job->mutex);
                            for (const std::string &message : menu.job->messages)
                                said += (said.empty() ? "" : "  ") + message;
                            found = std::move(menu.job->found);
                            parts = menu.job->parts;
                            where = menu.job->where;
                            imported = menu.job->settingsImported;
                        }
                        if (imported)
                            useImportedSettings(state);
                        line = said.empty() ? "Stopped." : said;
                        menu.job.reset();
                        if (found)
                        {
                            // A search: what it found on the Games page.
                            line = takeSearch(menu, std::move(*found), where);
                            shell.showPage(col::MenuPage::Games);
                        }
                        if (!parts.empty())
                        {
                            // The discs found set up: why each that failed did, until the next search. Stopped: the
                            // list again, each game planned anew (Set Up Games goes on from where it stopped).
                            const bool stopped = std::any_of(parts.begin(), parts.end(), [](const SetupJob::GamePart &part)
                                                             { return part.state == SetupJob::GamePart::Stopped; });
                            if (stopped)
                                for (size_t i = 0; i < kGameCount && i < menu.found.plans.size(); ++i)
                                {
                                    menu.found.plans[i] = plannedSetup(menu.found.search, i);
                                    menu.found.ticked[i] = menu.found.plans[i].game >= 0 ? 1 : 0;
                                }
                            else
                                menu.found = {};
                            for (size_t i = 0; i < parts.size() && i < menu.failed.size(); ++i)
                                menu.failed[i] = parts[i].state == SetupJob::GamePart::Failed ? parts[i].said : std::string();
                        }
                        refreshAfterSetup(menu);
                        shot = true;
                        if (menu.afterUpdate)
                        {
                            const size_t game = static_cast<size_t>(menu.afterUpdate->game);
                            if (game < parts.size() && parts[game].state == SetupJob::GamePart::Done)
                            {
                                // Its code made: the boot (or playback) it waited for, from the page it was chosen on.
                                if (menu.afterUpdate->recording < 0)
                                    shell.showPage(col::MenuPage::Tiles);
                                else
                                    shell.showPage(col::MenuPage::Recordings);
                            }
                            else
                                menu.afterUpdate.reset();
                        }
                        if (shell.page() == col::MenuPage::Tiles)
                            menu.layout = tilesLayout(shell);
                    }
                    if (line != menu.notice)
                    {
                        menu.notice = line;
                        redraw = true;
                    }
                }
                else
                {
                    // Discs dropped on the window (any game: the disc says
                    // which) or picked in a dialog (for its game's row).
                    // A drop, and Find Discs's folder, are searched for every
                    // game's discs; a game row's pick is set up as that game.
                    // (a mod or a map dropped: offered to its folder instead)
                    std::vector<std::pair<int, std::string>> picks;
                    std::vector<std::filesystem::path> places;
                    for (const std::string &path : ps2_host::input().dropped)
                        if (!takeDroppedMod(menu, collection, pathFromUtf8(path), false))
                            places.push_back(pathFromUtf8(path));
                    for (const std::string &path : std::exchange(menu.scriptDrops, {}))
                        if (!takeDroppedMod(menu, collection, pathFromUtf8(path), true))
                            places.push_back(pathFromUtf8(path));
                    runInstalls(menu, state, collection, shell);
                    {
                        std::lock_guard lock(g_dialogPicks.mutex);
                        for (auto &pick : g_dialogPicks.picks)
                            if (pick.first < 0)
                                places.push_back(pathFromUtf8(pick.second));
                            else
                                picks.push_back(std::move(pick));
                        g_dialogPicks.picks.clear();
                    }
                    if (!places.empty())
                        startSearch(menu, std::move(places));
                    else if (!picks.empty())
                        startSetup(menu, std::move(picks));
                    if (!menu.job)
                        startTransfer(menu);
                    runRequest(menu);
                    if (menu.job)
                    {
                        redraw = true;
                        continue;
                    }
                }
                if (menu.job)
                {
                    draw(now);
                    {
                        // (the shots below: one of each step of the job, at most every 3 s)
                        std::string step;
                        {
                            std::lock_guard lock(menu.job->mutex);
                            step = menu.job->progress.step;
                        }
                        if (step != jobShotStep && now - jobShotAt >= 3.0)
                        {
                            jobShotStep = step;
                            jobShotAt = now;
                            writeShot();
                        }
                    }
                    presentMenu(menu, redraw);
                    redraw = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    continue;
                }
                col::MenuInput in = menuFrameInput(menu, &shell);
                const bool tappedQuit = takeBottomTaps(menu, shell, in);
                if (std::exchange(g_menuSettingsWanted, false) && shell.page() == col::MenuPage::Tiles)
                {
                    std::cout << "[app] menu: Settings... from the app menu" << std::endl;
                    shell.showPage(col::MenuPage::Settings);
                }
                const MenuPlace before(shell);
                col::MenuAction action = shell.input(in, layoutOf(), rowsOf(), pageOf());
                if (menu.afterUpdate)
                    action = *std::exchange(menu.afterUpdate, std::nullopt);
                else if ((action.kind == col::MenuAction::Boot && gameHasFiles(g_argv0, kGamesInOrder[action.game])) ||
                         (action.kind == col::MenuAction::Play && action.recording >= 0))
                {
                    // Its code not this app's yet (the update was stopped, or failed): made first on the Games page,
                    // as the update makes it, never in a boot that shows nothing while it builds. (Only the code:
                    // extras that wait for their disc do not hold a game back.)
                    std::vector<UpdateWork> work = pendingUpdates({action.game});
                    if (!work.empty() && work.front().code)
                    {
                        work.front().extras = {};
                        std::cout << "[app] menu: " << kGamesInOrder[action.game].id << ": its code first" << std::endl;
                        menu.afterUpdate = action;
                        startUpdate(menu, std::move(work));
                        shell.showPage(col::MenuPage::Games);
                        redraw = shot = true;
                        continue;
                    }
                }
                if (tappedQuit && action.kind == col::MenuAction::None)
                    action.kind = col::MenuAction::Quit;
                if (const std::optional<col::MenuSound> sound = menuSoundOf(in, action, before, shell))
                    menuSound(menu, shownGame(shell), *sound);
                if (shell.page() != menu.shownPage)
                {
                    // The Games page left: the discs found are let go (the page
                    // lists each game's own rows again).
                    if (menu.shownPage == col::MenuPage::Games && menu.found.searched)
                    {
                        menu.found = {};
                        buildGameRows(menu);
                    }
                    // The Mods page opened (or came back from a mod's): its
                    // folders read again; a mod's page: its rows. The map
                    // page: the maps, for the player chosen.
                    if (shell.page() == col::MenuPage::Mods)
                        refreshMods(menu, state, collection);
                    if (shell.page() == col::MenuPage::Games)
                        refreshGames(menu);
                    if (shell.page() == col::MenuPage::Mod)
                        refreshModPage(menu, state, collection, shell, shell.modsRow());
                    if (shell.page() == col::MenuPage::FreeplayMap)
                        refreshMaps(menu, collection, shell.freeplayGame());
                    if (shell.page() == col::MenuPage::Recordings)
                        refreshRecordings(menu, collection);
                    if (shell.page() == col::MenuPage::SaveProfiles)
                        refreshSaveProfiles(menu, state, collection);
                    menu.shownPage = shell.page();
                }
                else if (menu.modsChanged && shell.page() == col::MenuPage::Mod)
                {
                    // A switch or a move on the mod's page: the list as the
                    // games now read it, the page of the mod where it now is.
                    refreshMods(menu, state, collection);
                    refreshModPage(menu, state, collection, shell, shell.modsRow());
                    redraw = shot = true; // (a move changes rows the shell does not count)
                }
                else if (menu.modsChanged && shell.page() == col::MenuPage::Mods)
                {
                    // A mod's switch on its row: the list as the games now read it.
                    refreshMods(menu, state, collection);
                    redraw = shot = true;
                }
                menu.modsChanged = false;
                if (std::exchange(menu.profilesChanged, false))
                {
                    // A change on the Save Profiles page: its rows made again, the cards' lines read again.
                    refreshSaveProfiles(menu, state, collection);
                    refreshLastPlayed(menu, collection);
                    redraw = shot = true;
                }
                if (action.kind == col::MenuAction::Boot && !gameHasFiles(g_argv0, kGamesInOrder[action.game]) && !anyGameSetUp())
                {
                    // No game set up yet: the Games page, to find every game's discs.
                    std::cout << "[app] menu: no game has files yet: the Games page" << std::endl;
                    shell.showPage(col::MenuPage::Games);
                    redraw = shot = true;
                    continue;
                }
                if (action.kind == col::MenuAction::Boot && !gameHasFiles(g_argv0, kGamesInOrder[action.game]))
                {
                    // Not set up yet: its disc first.
                    const GameProfile &game = kGamesInOrder[action.game];
                    std::cout << "[app] menu: " << game.id << " has no files yet: asking for its disc" << std::endl;
                    menu.notice = askForDisc(action.game, false)
                                      ? std::string("Choose the ") + game.title + kBootImageHelp
                                      : std::string("Drop the ") + game.title + " disc on this window.";
                    redraw = shot = true;
                    continue;
                }
                if (action.kind == col::MenuAction::Boot)
                {
                    const bool onMap = action.map >= 0 && action.map < static_cast<int>(menu.maps.size());
                    std::cout << "[app] menu: boot " << kGamesInOrder[action.game].id
                              << (onMap ? " on " + menu.maps[static_cast<size_t>(action.map)].name : std::string()) << std::endl;
                    if (onMap)
                    {
                        const CustomMap &map = menu.maps[static_cast<size_t>(action.map)];
                        const GameProfile &player = kGamesInOrder[action.game];
                        if (const MapSlot *slot = mapSlotFor(map, player.id, slotStartable(player)); slot && mapNeedsConversion(map, *slot))
                        {
                            // The page says what happens while the map converts.
                            menu.notice = "Converting " + map.name + " for " + col::shortGameName(player.id) + "...";
                            draw(menuSeconds());
                            presentMenu(menu, true);
                        }
                        if (!prepareMap(menu, map, player))
                        {
                            refreshMaps(menu, collection, shell.freeplayGame());
                            redraw = shot = true;
                            continue;
                        }
                        menu.notice.clear();
                    }
                    launchCard(menu, shell, layoutOf(), rowsOf(), pageOf());
                    leave();
                    shell.boot(action.game, action.map);
                    break;
                }
                if (action.kind == col::MenuAction::Play && action.recording >= 0 &&
                    action.recording < static_cast<int>(menu.recordings.size()))
                {
                    std::cout << "[app] menu: play back " << menu.recordings[static_cast<size_t>(action.recording)].file.string()
                              << std::endl;
                    menu.notice.clear();
                    leave();
                    shell.play(action.game, action.recording);
                    break;
                }
                if (action.kind == col::MenuAction::Quit)
                {
                    std::cout << "[app] menu: quit" << std::endl;
                    leave();
                    shell.quit();
                    break;
                }
                if (shell.revision() != shown || menu.hint != shownHint || menu.notice != shownNotice)
                {
                    shown = shell.revision();
                    shownHint = menu.hint;
                    shownNotice = menu.notice; // (a row's action said something: a folder's place)
                    redraw = shot = true;
                    // (the game the input chose: its movie, riders and music in this picture already)
                    menu.scene.show(shownGame(shell), now);
                    menu.scene.update(now);
                    followMusic(menu, state, shownGame(shell));
                }
                if (redraw)
                {
                    draw(now);
                    if (shot)
                        writeShot();
                    shot = false;
                }
                presentMenu(menu, redraw);
                redraw = false;
                // About 60 frames a second without vsync too.
                const auto spent = std::chrono::steady_clock::now() - frameStart;
                if (spent < std::chrono::milliseconds(16))
                    std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
            // A mod's copy still running: waited for, as a game's boot reads
            // the Mods and maps folders and quitting ends the process (a copy
            // cut halfway). Those queued after it wait for the menu again.
            if (menu.install)
            {
                std::cout << "[app] drop: leaving the menu once " << menu.install->plan.title << " is copied" << std::endl;
                endInstall(menu, state, collection, shell);
            }
            if (shell.state() == col::State::Quit)
                leave();
        }

        // Quitting: the window's state kept, the host down, the process
        // ended (std::_Exit: nothing of the games' left for static
        // destructors to run into).
        [[noreturn]] void quitApp(AppState &state, int status)
        {
            state.settings.fullscreen = state.fullscreen;
            if (g_settingsImported)
                std::cout << "[app] quit: the settings imported are kept as they are (the window's too)" << std::endl;
            else if (g_profile)
                saveSettingsNow(state);
            else if (!g_appSettingsFile.empty())
                // The collection's menu only: its own settings (the window).
                writeTextFile(g_appSettingsFile, formatSettings(state.settings, everyOptionProfile(), SettingsScope::App));
            std::cout << "[app] quit" << std::endl;
            ps2_host::shutdown();
            std::fflush(stdout);
            std::fflush(stderr);
#if defined(ZX_PGO_GENERATE)
            __llvm_profile_write_file();
#endif
            std::_Exit(status);
        }
    }

    namespace
    {
        // --make-game-code ID ELF TOOLS CACHE [JOBS [TARGET]]: a game's code made as the app makes it from the player's
        // disc (collection/game_code.h), into CACHE, for this app or another platform (TARGET: android-arm64 for the
        // Thor); prints the module's path and the time each step took.
        int makeGameCodeCommand(int argc, char *argv[])
        {
            if (argc < 6)
            {
                std::cerr << "usage: --make-game-code ID ELF TOOLS CACHE [JOBS [TARGET]]" << std::endl;
                return 2;
            }
            zx::app::GameCodeTools tools;
            std::string error;
            if (!tools.open(argv[4], &error))
            {
                std::cerr << "[game-code] " << error << std::endl;
                return 2;
            }
            zx::app::GameCodeRequest request;
            request.id = argv[2];
            request.elf = argv[3];
            request.cache = argv[5];
            request.jobs = argc >= 7 ? static_cast<unsigned>(std::atoi(argv[6])) : 0u;
            request.target = argc >= 8 ? argv[7] : "";
            std::filesystem::create_directories(request.cache);
            const auto t0 = std::chrono::steady_clock::now();
            auto stepStart = t0;
            std::string step;
            const bool made = zx::app::makeGameCode(tools, request, [&](const zx::app::GameCodeProgress &p)
            {
                if (step != p.step)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (!step.empty())
                        std::cout << "[game-code] " << step << ": " << std::chrono::duration<double>(now - stepStart).count()
                                  << " s" << std::endl;
                    step = p.step;
                    stepStart = now;
                }
                return true;
            }, &error);
            if (!made)
            {
                std::cerr << "[game-code] " << error << std::endl;
                return 1;
            }
            std::cout << "[game-code] " << zx::app::gameCodeFile(tools, request).string() << " in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
            return 0;
        }

        // The setup from the command line, with no window: --setup PATH...
        // (each disc image or folder, or a folder of them, set up for the
        // game it is), --check-games [--full] (each game's files checked),
        // --remove-game ID. Its exit status (0: all went well), or -1 when
        // the arguments are none of these.
        int setupCommand(int argc, char *argv[])
        {
            if (argc < 2)
                return -1;
            const std::string command = argv[1];
            const std::unique_ptr<GameCodeStep> code = appGameCodeStep();
            if (command == "--setup")
            {
                // Every game's discs in the places named (images, folders of
                // them, folders of folders), set up as the Games page's Set Up
                // Games does: each game with a PS2 disc found or set up already,
                // its extras too.
                std::vector<std::filesystem::path> places;
                for (int i = 2; i < argc; ++i)
                    places.push_back(pathFromUtf8(argv[i]));
                const DiscSearch search = findDiscs(places, setupGames(), extrasOpener());
                for (const FoundDisc &disc : search.discs)
                    std::cout << "[setup] found " << utf8Of(disc.source) << ": " << disc.message << std::endl;
                int failures = 0, done = 0;
                for (size_t index = 0; index < kGameCount; ++index)
                {
                    const FoundDiscs found = plannedSetup(search, index);
                    if (found.game < 0)
                    {
                        // (its disc found, the game set up from it before, nothing new: done)
                        if (search.ps2Of(static_cast<int>(index)))
                        {
                            std::cout << "[setup] " << kGamesInOrder[index].id << ": set up already" << std::endl;
                            ++done;
                        }
                        continue;
                    }
                    int lastShown = -1;
                    std::string lastStep;
                    const SetupProgressFn progress = [&](const SetupProgress &p)
                    {
                        const int percent = p.total ? static_cast<int>(p.done * 100u / p.total) : 0;
                        if (p.step != lastStep || percent / 10 != lastShown / 10)
                        {
                            std::cout << "[setup] " << p.step << " " << percent << "%" << std::endl;
                            lastShown = percent;
                            lastStep = p.step;
                        }
                        return true;
                    };
                    bool ok = false;
                    const std::string said = setUpFound(found, progress, ok);
                    std::cout << "[setup] " << kGamesInOrder[index].id << ": " << said << std::endl;
                    ok ? ++done : ++failures;
                }
                // A disc of none of the games, or another release: said, and a failure.
                for (const FoundDisc &disc : search.discs)
                    if (disc.kind == FoundDisc::Kind::Ps2 && disc.status != SourceCheck::Status::Ok)
                        ++failures;
                std::cout << "[setup] " << done << " set up, " << failures << " not" << std::endl;
                return failures == 0 && done > 0 ? 0 : 1;
            }
            if (command == "--check-games")
            {
                const bool full = argc > 2 && std::string(argv[2]) == "--full";
                int bad = 0;
                for (const GameProfile &game : kGamesInOrder)
                {
                    const InstalledCheck check = checkInstalled(gameFolder(game), setupGame(game), full, code.get());
                    const bool usable = check.status == InstalledCheck::Status::Ok || check.status == InstalledCheck::Status::ByHand ||
                                        (check.status == InstalledCheck::Status::NotInstalled && gameHasFiles(g_argv0, game));
                    std::cout << "[setup] " << (kCollection ? game.id : game.elf) << ": " << gameStatusLine(game, check)
                              << (check.message.empty() ? "" : " (" + check.message + ")") << std::endl;
                    bad += usable ? 0 : 1;
                }
                return bad == 0 ? 0 : 1;
            }
            if (command == "--remove-game")
            {
                const std::string id = argc > 2 ? argv[2] : "";
                for (const GameProfile &game : kGamesInOrder)
                    if (!kCollection || lowerAscii(game.id) == lowerAscii(id))
                    {
                        std::string error;
                        const bool ok = removeInstalled(gameFolder(game), &error);
                        std::cout << "[setup] " << (ok ? std::string(game.title) + "'s files removed" : error) << std::endl;
                        return ok ? 0 : 1;
                    }
                std::cerr << "[setup] --remove-game: no game \"" << id << "\" in this app" << std::endl;
                return 1;
            }
            return -1;
        }
    }

    int runApp(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && std::string(argv[1]) == "--make-game-code")
            return makeGameCodeCommand(argc, argv);
        AppState state;
        g_argv0 = argc > 0 ? argv[0] : nullptr;
        applyPortableMode(g_argv0);
        const AppPaths appPaths = resolveAppPaths(ZX_APP_NAME, getEnv);
        state.paths = appPaths;
        g_supportRoot = appPaths.supportDir;
        const int wanted = developerGame(argc, argv);
        if (kCollection)
        {
            // Once, from an earlier version's shared files. The keeper's
            // folder first, as it would be made at its own first start (its
            // single-game app's copied in), so that the move does not stand
            // in for it.
            std::vector<std::string> ids;
            for (const GameProfile &each : kGamesInOrder)
            {
                ids.push_back(each.id);
                if (std::string(each.id) == kKeeperGame)
                    gamePaths(appPaths, each);
            }
            std::vector<std::string> moved;
            migrateCollectionFiles(appPaths.supportDir, kKeeperGame, ids, &moved);
            for (const std::string &line : moved)
                std::cout << "[app] per-game files: " << line << std::endl;
            // App-wide settings at the collection's root, and the user's
            // gamepad mappings (devices, not games); each game's controls
            // stay in its own folder (games/<id>/controls.ini).
            g_appSettingsFile = appPaths.supportDir / "settings.ini";
        }
        const std::string gamepadMappings =
            kCollection ? (appPaths.supportDir / "gamecontrollerdb.txt").string() : std::string();
        if (const int status = setupCommand(argc, argv); status >= 0)
            return status;

        if (!kMenu)
        {
            // One game: its folders and settings first, for its log.
            g_game = &kGamesInOrder[0];
            state.paths = kCollection ? gamePaths(appPaths, *g_game) : appPaths;
            g_profile = &gameSettingsProfile(kCollection ? g_game->id : g_game->elf);
            std::error_code ec;
            std::filesystem::create_directories(state.paths.supportDir, ec);
            std::vector<std::string> warnings;
            const bool hadSettings = loadAppSettings(state, state.settings, &warnings);
            if (state.settings.logFile)
                openLogFile(state.paths.logDir);
            const std::time_t now = std::time(nullptr);
            char stamp[64] = {};
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::cout << "[app] " << ZX_APP_NAME << " " << appVersion() << " started " << stamp << "\n"
                      << (kCollection ? "[app] game: " + std::string(g_game->id) + " (" + g_game->title + ")\n" : std::string())
                      << "[app] support folder: " << state.paths.supportDir.string() << "\n"
                      << "[app] settings: " << state.paths.settingsFile.string() << (hadSettings ? "" : " (new)") << std::endl;
            for (const std::string &warning : warnings)
                std::cout << "[app] settings.ini " << warning << std::endl;
            if (!hadSettings)
                saveSettingsNow(state);
            const BootResult result = bootGame(state, appPaths, *g_game, gamepadMappings, true);
            if (result.end == col::GameEnd::Failed)
            {
                ps2_host::shutdown();
                return result.status;
            }
            quitApp(state, 0);
        }

        // The collection: its own settings (the window, sound, renderer:
        // every game's) and log, then the window, then the menu.
        {
            std::error_code ec;
            std::filesystem::create_directories(appPaths.supportDir, ec);
            std::vector<std::string> warnings;
            const bool hadSettings = loadSettings(g_appSettingsFile, state.settings, &warnings);
            if (state.settings.logFile)
                openLogFile(appPaths.logDir);
            const std::time_t now = std::time(nullptr);
            char stamp[64] = {};
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::cout << "[app] " << ZX_APP_NAME << " " << appVersion() << " started " << stamp << "\n"
                      << "[app] support folder: " << appPaths.supportDir.string() << "\n"
                      << "[app] settings: " << g_appSettingsFile.string() << (hadSettings ? "" : " (new)") << std::endl;
            for (const std::string &warning : warnings)
                std::cout << "[app] settings.ini " << warning << std::endl;
        }
        prepareHost();
        {
            ps2_host::WindowOptions window;
            window.width = state.settings.windowWidth;
            window.height = state.settings.windowHeight;
            window.highDpi = true;
            window.headless = headlessHost();
            if (!ps2_host::initialize(ZX_APP_NAME, window))
            {
                platform::showMessage(std::string(ZX_APP_NAME) + " cannot start", "The window could not be created.", true);
                return 1;
            }
        }
        ps2_host::setMenuAvailable(true);
        ps2_host::setVsync(state.settings.vsync);
        state.vsyncApplied = state.settings.vsync;
        ps2_host::setVariableRefresh(state.settings.variableRefresh);
        applyFrameInterpolation(state);
        AppPaths folders = appPaths;
        installAppMenus(state, folders);

        MenuScreen menu;
        loadMenu(menu, state, appPaths);
        col::Shell shell(kGameCount, state.settings.customFreeplay);
        menu.layout = tilesLayout(shell);
        const std::string wantedMap = developerMap(argc, argv);
        const std::string wantedPlayback = developerPlayback(argc, argv);
        shell.start(wantedMap.empty() && wantedPlayback.empty() ? wanted : -1);
        if (!wantedPlayback.empty())
        {
            play_recording::Summary summary;
            const int game = play_recording::summarize(wantedPlayback, summary) ? gameIndexOf(summary.value("game")) : -1;
            if (game < 0)
                std::cerr << "[app] --play " << wantedPlayback << ": not a recording of a game in this app" << std::endl;
            else
            {
                std::cout << "[app] --play " << wantedPlayback << ": no menu first" << std::endl;
                menu.recordings = {col::recordingEntry(wantedPlayback, summary)};
                shell.play(game, 0);
            }
        }
        if (wanted >= 0)
            std::cout << "[app] --game " << kGamesInOrder[wanted].id << ": no menu first" << std::endl;
        if (wanted >= 0 && !wantedMap.empty())
        {
            // The map started as the map page starts it (converted first when
            // it was made in another game).
            menu.maps = listMenuMaps(appPaths);
            int index = -1;
            for (size_t i = 0; i < menu.maps.size(); ++i)
                if (lowerAscii(menu.maps[i].folderName) == lowerAscii(wantedMap))
                    index = static_cast<int>(i);
            if (index < 0)
                std::cerr << "[app] --map " << wantedMap << ": no such map in " << sharedModsFolder(appPaths).string() << " or "
                          << (appPaths.supportDir / kMapsFolder).string() << std::endl;
            else if (!mapPlayableIn(menu.maps[static_cast<size_t>(index)], kGamesInOrder[wanted].id, slotStartable(kGamesInOrder[wanted])))
                std::cerr << "[app] --map " << wantedMap << ": not a map " << kGamesInOrder[wanted].id << " can start" << std::endl;
            else if (prepareMap(menu, menu.maps[static_cast<size_t>(index)], kGamesInOrder[wanted]))
            {
                std::cout << "[app] --map " << wantedMap << ": Custom Freeplay on " << menu.maps[static_cast<size_t>(index)].name << std::endl;
                shell.boot(wanted, index);
            }
        }
        if (shell.state() == col::State::Menu)
        {
            // An update's work for the games set up before it (their code,
            // their extras' parts whose key differs), on the Games page.
            if (std::vector<UpdateWork> work = pendingUpdates(); !work.empty())
            {
                startUpdate(menu, std::move(work));
                shell.showPage(col::MenuPage::Games);
            }
            playIntro(state, menu);
        }
        // (a developer's check of the saves' import: this file imported as the Games page's Import would, no dialog)
        if (const char *saves = std::getenv("ZX_MENU_IMPORT_SAVES"); saves && saves[0])
            keepTransferPick(false, saves);
        while (shell.state() != col::State::Quit)
        {
            if (shell.state() == col::State::Menu)
            {
                runMenu(state, appPaths, shell, menu);
                continue;
            }
            // A playback: the recording chosen on the Recordings page (the
            // strip on a second screen in the menu's font). A Custom
            // Freeplay boot: the map chosen on the map page.
            std::optional<PlaybackBoot> playing;
            if (shell.playback() >= 0 && shell.playback() < static_cast<int>(menu.recordings.size()))
            {
                playing.emplace();
                playing->entry = menu.recordings[static_cast<size_t>(shell.playback())];
                if (!play_recording::summarize(playing->entry.file, playing->summary))
                {
                    std::cerr << "[app] playback: " << playing->entry.file.string() << " is not a recording" << std::endl;
                    menu.notice = "The recording cannot be played: it cannot be read.";
                    shell.gameEnded(col::GameEnd::Failed);
                    continue;
                }
                col::PlaybackStrip::shared().show(col::StripView{}, std::make_shared<col::BitmapFont>(menu.font));
            }
            const PlaybackBoot *playback = playing ? &*playing : nullptr;
            const CustomMap *map = !playback && shell.map() >= 0 && shell.map() < static_cast<int>(menu.maps.size())
                                       ? &menu.maps[static_cast<size_t>(shell.map())]
                                       : nullptr;
            const BootResult result = bootGame(state, appPaths, kGamesInOrder[shell.game()], gamepadMappings, false, map,
                                               map && menu.converted ? &*menu.converted : nullptr, playback);
            col::PlaybackStrip::shared().show(std::nullopt);
            menu.converted.reset();
            menu.notice = result.why;
            // Restart Game: the same game (on the same map) again, not the menu.
            const bool restart = std::exchange(g_restartWanted, false) && result.end == col::GameEnd::ExitToMenu && !playback;
            const int restartGame = shell.game(), restartMap = map ? shell.map() : -1;
            shell.gameEnded(result.end);
            if (restart)
            {
                std::cout << "[app] restart: " << kGamesInOrder[static_cast<size_t>(restartGame)].id
                          << (map ? " on " + map->name : std::string()) << std::endl;
                if (!map || prepareMap(menu, *map, kGamesInOrder[static_cast<size_t>(restartGame)]))
                    shell.boot(restartGame, restartMap);
            }
        }
        quitApp(state, 0);
    }
}

namespace
{
    // The runner's main starts the app (runtime/front_end.h).
    [[maybe_unused]] const bool g_installed = zx::front_end::installApp({&zx_app::startedAsApp, &zx_app::runApp});
}
