#include "app.h"
#include "platform.h"

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
#include "collection/extras/setup.h"
#include "collection/save_transfer.h"
#include "collection/card_import.h"
#include "collection/maps.h"
#include "collection/menu.h"
#include "collection/mods.h"
#include "collection/offered.h"
#include "game_overrides.h"
#include "../game_overrides/dave_mirra.h"
#include "collection/panel.h"
#include "runtime/ps2_save_store.h"
#include "ps2_runtime.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include "ps2_host_backend.h"
#include "ps2_host_sdl.h"
#include "runtime/ps2_audio_mix.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_front_end.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"
#include "runtime/play_recording.h"
#include "runtime/game_module.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_texture_replace.h"
#include "runtime/gs/gs_interpolate.h"
#include "runtime/gs/gs_null_backend.h"
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

#if defined(PS2X_PGO_GENERATE)
// Instrumented builds (clang -fprofile-instr-generate): write the profile
// before std::_Exit, which skips the runtime's own atexit writer.
extern "C" int __llvm_profile_write_file(void);
#endif

#ifndef PS2X_APP_NAME
#error "app.cpp is built only with PS2X_APP_NAME"
#endif
#ifndef PS2X_APP_GAME_TITLE
#define PS2X_APP_GAME_TITLE PS2X_APP_NAME
#endif
#ifndef PS2X_APP_ELF_SHA256
#define PS2X_APP_ELF_SHA256 ""
#endif
#ifndef PS2X_APP_SAVE_DIR
#define PS2X_APP_SAVE_DIR ""
#endif
#if defined(_WIN32)
#define PS2X_APP_BUILD_TOOL "tools\\make-windows.ps1"
#else
#define PS2X_APP_BUILD_TOOL "tools/make-app.sh"
#endif
#ifndef PS2X_APP_VERSION
#define PS2X_APP_VERSION "dev"
#endif

namespace ps2x_app
{
    // A game the app can run. The single-game build has one, from the
    // PS2X_APP_* definitions. A build with several (CMake PS2X_APP_GAMES,
    // ps2x_app_games.h) is a collection named PS2X_APP_NAME: the game comes
    // from its menu (or --game <id>, PS2X_APP_GAME); each keeps its
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
                                // data file is not named like the folder (DM2's BASLUS-20159BMX2GAME/BMX2SAVE.DAT)
    };
}
#if defined(PS2X_APP_GAMES_HEADER)
#include PS2X_APP_GAMES_HEADER
#else
namespace ps2x_app
{
    constexpr GameProfile kGames[] = {
        {"", PS2X_APP_NAME, PS2X_APP_GAME_TITLE, PS2X_APP_ELF, PS2X_APP_ELF_SHA256, PS2X_APP_SAVE_DIR},
    };
}
#endif

namespace ps2x_app
{
    namespace
    {
        using namespace ps2x::app;

        constexpr size_t kGameCount = sizeof(kGames) / sizeof(kGames[0]);
        constexpr bool kCollection = kGameCount > 1 || kGames[0].id[0] != '\0';
        // A collection of several games opens on its menu
        // (collection/menu.h) and comes back to it from a game; one of a
        // single game boots it and quits with it, as a single-game app does.
        constexpr bool kMenu = kCollection && kGameCount > 1;
        // The build's games (kGames, in the order PS2X_APP_GAMES lists them)
        // in the collection's order (collection/game_order.h: DM2, AI, BMX
        // XXX). The menu's cards and the shell's game numbers, the Games,
        // Mods and Custom Freeplay pages and the Thor's bottom screen all
        // count in this order; what is saved is keyed by the game's id.
        std::array<GameProfile, kGameCount> gamesInOrder()
        {
            std::vector<std::string_view> ids;
            for (const GameProfile &game : kGames)
                ids.push_back(game.id);
            const std::vector<size_t> order = ps2x::collection::inGameOrder(ids);
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
        // portable: PS2X_APP_SUPPORT_DIR unless the environment sets it.
        std::string g_portableSupportDir;

        // The app's own lookups: the environment, portable mode's support
        // folder in place of an unset PS2X_APP_SUPPORT_DIR. What the game
        // is given goes in its BootConfig, not the environment.
        const char *getEnv(const char *name)
        {
            const char *value = std::getenv(name);
            if ((!value || !value[0]) && !g_portableSupportDir.empty() && std::string(name) == "PS2X_APP_SUPPORT_DIR")
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
        // PS2Runtime::requestRasterBackendReload, reads them).
        void applyRendererSettings(const Settings &settings)
        {
            for (const auto &[name, value] : rendererValues(settings))
                ps2x::boot::change(name, value);
        }

        // Settings::wideAspect as width / height (0: the window's).
        float wideAspectValue(int index)
        {
            constexpr float kAspects[] = {16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 9.0f, 0.0f};
            return kAspects[std::clamp(index, 0, 4)];
        }

        // The view settings the game override reads (PS2Runtime), unless the
        // environment sets them (PS2X_ASPECT, PS2X_MOTION_BLUR: the runtime
        // read those already). The draw distance came with the boot
        // (gameBootConfig's PS2X_DRAW_DISTANCE).
        void applyViewSettings(const Settings &settings, PS2Runtime &runtime)
        {
            if (!envSet("PS2X_ASPECT"))
                runtime.setWideAspect(wideAspectValue(settings.wideAspect));
            if (!envSet("PS2X_MOTION_BLUR"))
                runtime.setMotionBlur(std::clamp(settings.motionBlur, 0, 2));
            // GPU renderer picture quality (runtime/gs/gs_quality.h).
            for (const ps2x::gs_quality::OptionInfo &q : ps2x::gs_quality::options())
                if (!envSet(q.env))
                    ps2x::gs_quality::set(q.option, settings.quality[static_cast<size_t>(q.option)]);
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
            std::string stem = lowerAscii(kCollection && !kMenu ? g_game->id : PS2X_APP_NAME);
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

        // The game's settings: its file's header, its options and rows
        // (ps2x::app::GameSettingsProfile).
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
            Installed installed = findInstalled([](const char *name) { return ps2x::boot::text(name); });
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

        void saveSettingsNow(const AppState &state)
        {
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
        // PS2X_SCREENSHOT_DIR (Pictures/<app>/<game>), named after the game.
        std::filesystem::path screenshotFile()
        {
            std::filesystem::path dir = "Screenshots";
            if (const char *env = ps2x::boot::text("PS2X_SCREENSHOT_DIR"); env && env[0])
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

        void takeScreenshot(PS2Runtime &runtime)
        {
            const std::filesystem::path file = screenshotFile();
            runtime.requestScreenshot(file.string(), [file](bool ok)
                                      {
                                          std::cout << "[app] screenshot " << (ok ? "saved: " : "failed: ") << file.string()
                                                    << std::endl;
                                          ps2x::settings::notify(ok ? "Screenshot saved" : "Screenshot failed");
                                      });
        }

        // ---- the map window ---------------------------------------------------
        // The bottom screen (panel/panel.h: the level's map, goals and tricks)
        // in a second window while [display] map_window is on. The game
        // thread takes a snapshot of the game at each VBlank, as the Thor
        // does; the window thread paints the panel from the latest one about
        // 20 times a second into a 1240x1080 picture and shows it, and hands
        // the window's pointer to the panel: a press, drag and release are a
        // touch, the wheel a pinch about the pointer. The window keeps its
        // place and size in the settings; its close button turns the setting
        // off. PS2X_MAP_WINDOW_SHOT=<file.png> writes the picture painted ten
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
            std::string dataFolder;
            std::vector<uint32_t> pixels = std::vector<uint32_t>(static_cast<size_t>(kWidth) * kHeight, 0u);
            std::chrono::steady_clock::time_point nextPaint{};
            bool open = false;
            bool pointerDown = false;
            std::string shotFile; // PS2X_MAP_WINDOW_SHOT
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

        // The game thread, at each VBlank (PS2Runtime::addVSyncHook): read-only.
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
        // wheel a pinch about the pointer, two fingers 120 px apart moved
        // apart (up) or together by a tenth a notch.
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
            {
                if (m.pointerDown || e.wheel == 0.0f)
                    break;
                const float half = 60.0f;
                const float scale = std::clamp(1.0f + 0.1f * e.wheel, 0.5f, 2.0f);
                touch(panel::TouchPhase::Down, 1, e.x - half, e.y);
                touch(panel::TouchPhase::Down, 2, e.x + half, e.y);
                touch(panel::TouchPhase::Move, 1, e.x - half * scale, e.y);
                touch(panel::TouchPhase::Move, 2, e.x + half * scale, e.y);
                touch(panel::TouchPhase::Up, 1, e.x - half * scale, e.y);
                touch(panel::TouchPhase::Up, 2, e.x + half * scale, e.y);
                break;
            }
            }
        }

        // Once per host frame on the window thread, in the menu and in a game.
        void mapWindowFrame(AppState &state)
        {
            MapWindow &m = mapWindow();
            if (!m.shotRead)
            {
                m.shotRead = true;
                if (const char *file = std::getenv("PS2X_MAP_WINDOW_SHOT"); file && file[0])
                    m.shotFile = file;
            }
            const bool wanted = state.settings.mapWindow && !headlessHost();
            if (wanted && !m.open)
            {
                const ps2_host::MapWindowRect rect{state.settings.mapWindowX, state.settings.mapWindowY,
                                                   state.settings.mapWindowWidth, state.settings.mapWindowHeight};
                m.open = ps2_host::openMapWindow("Map", rect);
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
            if (const std::string folder = panel::gameFiles(); folder != m.dataFolder)
            {
                m.dataFolder = folder;
                m.panel->setDataFolder(folder);
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

        // Frame Interpolation: the setting, or ZAXIS_INTERPOLATION=on|off over it.
        void applyFrameInterpolation(const AppState &state)
        {
            static const char *const env = std::getenv("ZAXIS_INTERPOLATION");
            const bool on = env && env[0] ? (std::strcmp(env, "on") == 0 || std::strcmp(env, "1") == 0)
                                          : state.settings.frameInterpolation;
            ps2x::gs_interpolate::setEnabled(on);
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
        }

        // The keys' presses as last seen ([hotkeys]): each boot starts from
        // the count at its start.
        uint32_t g_switchGamePresses = 0;
        uint32_t g_screenshotPresses = 0;

        // Once per host frame on the window thread while a game runs.
        void gameFrame(PS2Runtime &runtime, AppState &state)
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

            runtime.setIntegerScale(state.settings.scaling == Scaling::Integer);
            if (state.showFps)
                state.showFps(state.settings.showFps);

            // The Native Meshes row is the game override's (it switches the
            // renderer's path while the game runs); its value is kept in
            // settings.ini from here, as the next boot's PS2X_RENDER_PATH.
            if (ps2x::settings::has("display.native_meshes"))
            {
                const bool nativeMeshes = ps2x::settings::value("display.native_meshes", 1) != 0;
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
                runtime.setHostPaused(wantPause);
                std::cout << (wantPause ? "[app] paused (window in the background)" : "[app] resumed") << std::endl;
            }
        }

        // The app's own rows (the window, sound), on state.settings: the
        // game's registry has them while a game runs (registerSettings) and
        // the collection's menu shows the same before any game has booted
        // (menuSettingsRows).
        ps2x::settings::Setting toggleSetting(AppState &state, const char *key, const char *label, bool Settings::*field,
                                              const char *help)
        {
            ps2x::settings::Setting setting;
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
        // Borderless where it has one (macOS).
        ps2x::settings::Setting windowModeSetting(AppState &state)
        {
            namespace st = ps2x::settings;
            st::Setting window;
            window.key = "display.window";
            window.label = "Display Mode";
            window.kind = st::Kind::Choice;
            window.choices = {"Window", "Full Screen"};
            if (offered(Need::Borderless, platformInstalled()))
            {
                window.choices.push_back("Borderless");
                window.help = "Borderless stays in the same Space.";
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

        ps2x::settings::Setting scalingSetting(AppState &state)
        {
            namespace st = ps2x::settings;
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

        ps2x::settings::Setting vsyncSetting(AppState &state)
        {
            return toggleSetting(state, "display.vsync", "VSync", &Settings::vsync, "Off lowers latency but may tear.");
        }

        ps2x::settings::Setting variableRefreshSetting(AppState &state)
        {
            return toggleSetting(state, "display.variable_refresh", "Variable Refresh", &Settings::variableRefresh,
                                 "For G-Sync and FreeSync displays.");
        }

        ps2x::settings::Setting frameInterpolationSetting(AppState &state)
        {
            return toggleSetting(state, "display.frame_interpolation", "Frame Interpolation", &Settings::frameInterpolation,
                                 "Smoother motion on 120 Hz and faster displays.");
        }

        ps2x::settings::Setting showFpsSetting(AppState &state)
        {
            return toggleSetting(state, "display.show_fps", "Show Frame Rate", &Settings::showFps, "Frames per second, top right.");
        }

        ps2x::settings::Setting pauseInBackgroundSetting(AppState &state)
        {
            return toggleSetting(state, "display.pause_in_background", "Pause in Background", &Settings::pauseOnFocusLoss,
                                 "Pauses when you switch away.");
        }

        // Play recordings (runtime/play_recording.h): off by default; the
        // Recordings line says where they are and how many.
        ps2x::settings::Setting recordPlaySetting(AppState &state)
        {
            ps2x::settings::Setting record = toggleSetting(state, "game.record_play", "Record Play", &Settings::recordPlay,
                                                           "Saves your inputs for a replay.");
            record.restart = true;
            return record;
        }

        ps2x::settings::Setting recordingsSetting(AppState &state)
        {
            namespace st = ps2x::settings;
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
            where.activate = [folder]()
            { ps2x::settings::notify("Recordings: " + folder.string(), 6.0); };
            return where;
        }

        ps2x::settings::Setting volumeSetting(AppState &state)
        {
            namespace st = ps2x::settings;
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

        ps2x::settings::Setting muteInBackgroundSetting(AppState &state)
        {
            return toggleSetting(state, "audio.mute_in_background", "Mute in Background", &Settings::muteOnFocusLoss,
                                 "Mutes when you switch away.");
        }

        ps2x::settings::Setting mapWindowSetting(AppState &state)
        {
            return toggleSetting(state, "display.map_window", "Map Window", &Settings::mapWindow,
                                 "The map, goals and tricks in a second window.");
        }

        // Custom Freeplay (collection/maps.h): the menu's fourth tile. Off by
        // default: the menu as it was, no boot given a map.
        ps2x::settings::Setting customFreeplaySetting(AppState &state)
        {
            return toggleSetting(state, "game.custom_freeplay", "Custom Freeplay", &Settings::customFreeplay,
                                 "Adds a tile for custom maps.");
        }

        // The collection's menu ([menu], collection/menu_scene.h): what plays
        // at launch, and the shown game's music under the menu.
        ps2x::settings::Setting introMovieSetting(AppState &state)
        {
            namespace st = ps2x::settings;
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

        ps2x::settings::Setting menuMusicSetting(AppState &state)
        {
            return toggleSetting(state, "menu.menu_music", "Menu Music", &Settings::menuMusic, "Each game's own music under its card.");
        }

        // The collection menu's settings page: the app's rows above, as the
        // page draws and changes them (a Setting's kind, choices, range, help
        // and functions, one to one).
        std::vector<ps2x::collection::SettingsRow> menuSettingsRows(AppState &state)
        {
            namespace st = ps2x::settings;
            auto row = [](const st::Setting &setting)
            {
                ps2x::collection::SettingsRow out;
                out.kind = setting.kind == st::Kind::Choice ? ps2x::collection::SettingsRow::Choice
                           : setting.kind == st::Kind::Range ? ps2x::collection::SettingsRow::Range
                                                             : ps2x::collection::SettingsRow::Toggle;
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
            };
            const Installed platform = platformInstalled();
            std::vector<ps2x::collection::SettingsRow> rows;
            if (offered(Need::WindowModes, platform))
                rows.push_back(row(windowModeSetting(state)));
            for (const st::Setting &setting :
                 {scalingSetting(state), vsyncSetting(state), variableRefreshSetting(state), frameInterpolationSetting(state),
                  showFpsSetting(state), pauseInBackgroundSetting(state), volumeSetting(state), muteInBackgroundSetting(state),
                  introMovieSetting(state), menuMusicSetting(state)})
                rows.push_back(row(setting));
            if (offered(Need::SecondWindow, platform))
                rows.push_back(row(mapWindowSetting(state)));
            rows.push_back(row(recordPlaySetting(state)));
            rows.push_back(row(customFreeplaySetting(state)));
            return rows;
        }

        // The settings.ini settings in the shared settings model
        // (runtime/ps2_settings.h): the game's Settings screen and the menu
        // bar both change them through here. Rows of the game's session
        // (ps2x::session::addSetting): they refer to its runtime and its
        // options, and go when the game is left.
        void registerSettings(AppState &state, PS2Runtime &runtime)
        {
            namespace st = ps2x::settings;
            auto toggle = [&state](const char *key, const char *label, bool Settings::*field, const char *help)
            { return toggleSetting(state, key, label, field, help); };
            // A row shows only where it does something for this setup.
            const Installed installed = setupInstalled();
            auto offers = [&installed](GameOption option) { return g_profile && offered(*g_profile, option, installed); };

            if (offered(Need::WindowModes, installed))
                ps2x::session::addSetting(windowModeSetting(state));

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
            ps2x::session::addSetting(renderer);

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
            ps2x::session::addSetting(resolution);

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
            ps2x::session::addSetting(fullHeight);

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
                ps2x::session::addSetting(widescreen);

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
                if (!envSet("PS2X_ASPECT"))
                    runtime.setWideAspect(wideAspectValue(state.settings.wideAspect));
                saveSettingsNow(state);
            };
            if (offers(GameOption::Widescreen))
                ps2x::session::addSetting(aspect);

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
                if (!envSet("PS2X_MOTION_BLUR"))
                    runtime.setMotionBlur(state.settings.motionBlur);
                saveSettingsNow(state);
            };
            if (offers(GameOption::MotionBlur))
                ps2x::session::addSetting(blur);

            st::Setting distance;
            distance.key = "display.draw_distance";
            distance.label = "Draw Distance";
            distance.kind = st::Kind::Choice;
            distance.choices = {"Original", "Far", "Farthest"};
            distance.help = "See further before the fog.";
            // It changes the game (ps2_games.h drawDistanceChangesGame,
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
                ps2x::session::addSetting(distance);

            // Picture quality beyond the PS2 (GPU renderer): anti-aliasing,
            // texture filtering, fog, shadows, depth precision, perspective.
            // Session rows like the others, so the next game booted gets
            // them here again, not before Display Mode.
            ps2x::gs_quality::addSettings([&state](ps2x::gs_quality::Option option, int value)
                                          {
                                              state.settings.quality[static_cast<size_t>(option)] = value;
                                              saveSettingsNow(state);
                                          },
                                          true);

            // Only with the Xbox pack made from the player's Xbox disc
            // (PS2X_XBOX_PACK, set above).
            if (offered(Need::XboxTextures, installed))
            {
                st::Setting xboxTextures = toggle("display.xbox_textures", "Xbox Textures", &Settings::xboxTextures,
                                                  "Sharper Xbox textures.");
                xboxTextures.current = []() { return ps2x::texture_replace::enabled() ? 1 : 0; };
                xboxTextures.apply = [&state](int value)
                {
                    state.settings.xboxTextures = value != 0;
                    ps2x::texture_replace::setEnabled(state.settings.xboxTextures);
                    saveSettingsNow(state);
                };
                ps2x::session::addSetting(xboxTextures);
            }
            if (offered(Need::XboxSkaters, installed))
            {
                // Read when a skater is loaded (game_overrides/aggressive_inline_disc.cpp).
                st::Setting xboxSkaters = toggle("display.xbox_skaters", "Xbox Skaters", &Settings::xboxSkaters,
                                                 "More detailed skaters, from the next level.");
                ps2x::session::addSetting(xboxSkaters);
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
                ps2x::session::addSetting(xboxMovies);
            }

            // Texture packs' dump mode: for making a pack (SETTINGS.md "Texture packs").
            st::Setting textureDump = toggle("display.texture_dump", "Dump Textures", &Settings::textureDump,
                                             "Save textures to Texture Dumps, for texture packs.");
            textureDump.restart = true;
            ps2x::session::addSetting(textureDump);

            ps2x::session::addSetting(scalingSetting(state));
            ps2x::session::addSetting(vsyncSetting(state));
            ps2x::session::addSetting(variableRefreshSetting(state));
            ps2x::session::addSetting(frameInterpolationSetting(state));
            ps2x::session::addSetting(showFpsSetting(state));
            ps2x::session::addSetting(pauseInBackgroundSetting(state));
            if (offered(Need::SecondWindow, installed))
                ps2x::session::addSetting(mapWindowSetting(state));
            ps2x::session::addSetting(recordPlaySetting(state));
            ps2x::session::addSetting(recordingsSetting(state));
            if (kMenu)
                ps2x::session::addSetting(customFreeplaySetting(state));
            const st::Setting volume = volumeSetting(state);
            ps2x::session::addSetting(volume);
            ps2x::session::addSetting(muteInBackgroundSetting(state));

            // Movies have no volume of their own in the game.
            ps2x::audio_mix::setGain(state.settings.movieVolume / 100.0f);
            st::Setting movie = volume;
            movie.key = "audio.movie_volume";
            movie.label = "Movie Volume";
            movie.help = "Intro and menu movies.";
            movie.current = [&state]() { return state.settings.movieVolume; };
            movie.apply = [&state](int value)
            {
                state.settings.movieVolume = std::clamp(value, 0, 100);
                ps2x::audio_mix::setGain(state.settings.movieVolume / 100.0f);
                saveSettingsNow(state);
            };
            if (offers(GameOption::MovieVolume))
                ps2x::session::addSetting(movie);

            st::Setting intro = toggle("game.skip_intro_movies", "Skip Intro Movies", &Settings::skipIntroMovies,
                                       "Start at the title screen.");
            intro.restart = true;
            if (offers(GameOption::SkipIntroMovies))
                ps2x::session::addSetting(intro);

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
                ps2x::session::addSetting(language);
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
                ps2x::session::addSetting(voices);
            }

            st::Setting developer = toggle("game.developer_menu", "Developer Menu", &Settings::developerMenu,
                                           "Press F1 in a level.");
            developer.restart = true;
            if (offers(GameOption::DeveloperMenu))
                ps2x::session::addSetting(developer);

            st::Setting zoo = toggle("game.zoo", "Zoo Level", &Settings::zoo,
                                     "The cut level, in Free Skate and Timed Run.");
            zoo.restart = true;
            if (offers(GameOption::Zoo))
                ps2x::session::addSetting(zoo);
            // With the Estate made from the GameCube disc.
            if (offers(GameOption::Estate))
            {
                st::Setting estate = toggle("game.estate", "Estate Level", &Settings::estate,
                                            "An early Tutorial, in Free Skate.");
                estate.restart = true;
                ps2x::session::addSetting(estate);
            }
            // Dave Mirra BMX 2: with what its Xbox disc gave.
            if (offers(GameOption::Dm2XboxLevels))
            {
                st::Setting xbox = toggle("game.dm2_xbox_levels", "Xbox Levels", &Settings::dm2XboxLevels,
                                          "Venice and Greenville, with their goals.");
                xbox.restart = true;
                ps2x::session::addSetting(xbox);
            }
            if (offers(GameOption::Dm2XboxThemes))
            {
                st::Setting themes = toggle("game.xbox_themes", "Desert and Highway Themes", &Settings::dm2XboxThemes,
                                            "Two unused park editor themes.");
                themes.restart = true;
                ps2x::session::addSetting(themes);
            }
            if (offers(GameOption::Dm2RetroMovie))
            {
                st::Setting retro = toggle("game.retro_movie", "Retro Movie", &Settings::dm2RetroMovie,
                                           "Adds Retro to the movie gallery.");
                retro.restart = true;
                ps2x::session::addSetting(retro);
            }
            // DM2: the attract demo at the title screen (the Xbox's and GameCube's).
            if (offers(GameOption::Dm2AttractDemo))
            {
                st::Setting demo = toggle("game.attract_demo", "Attract Demo", &Settings::dm2AttractDemo,
                                          "Replays play at an idle title screen.");
                demo.restart = true;
                ps2x::session::addSetting(demo);
            }
            // DM2: its cut Create Player menu, from the main menu (with the
            // readable newRider, which makes a created rider's model).
            if (offers(GameOption::Dm2CreatePlayer))
            {
                st::Setting create = toggle("game.create_player", "Create Rider", &Settings::dm2CreatePlayer,
                                            "Adds CREATE RIDER to the main menu.");
                create.restart = true;
                ps2x::session::addSetting(create);
            }
            // BMX XXX: its park editor, from the main menu.
            if (offers(GameOption::BmxParkEditor))
            {
                st::Setting park = toggle("game.park_editor", "Park Editor", &Settings::bmxParkEditor,
                                          "Adds PARK EDITOR to the main menu.");
                park.restart = true;
                ps2x::session::addSetting(park);
            }
            // BMX XXX: its Xbox and GameCube releases' content, each with the
            // disc that gave it (the intro movie is the PS2 disc's own).
            if (offers(GameOption::BmxXboxChallenges))
            {
                st::Setting challenges = toggle("game.xbox_challenges", "Xbox Challenges", &Settings::bmxXboxChallenges,
                                                "Fixed challenges in four levels.");
                challenges.restart = true;
                ps2x::session::addSetting(challenges);
            }
            if (offers(GameOption::BmxEditorPieces))
            {
                st::Setting pieces = toggle("game.editor_pieces", "Pool and Trash Can Pieces", &Settings::bmxEditorPieces,
                                            "Two more pieces, kept for parks using them.");
                pieces.restart = true;
                ps2x::session::addSetting(pieces);
            }
            if (offers(GameOption::BmxLevelPreviews))
            {
                st::Setting previews = toggle("game.level_previews", "Level Previews", &Settings::bmxLevelPreviews,
                                              "Preview movies in the level select.");
                previews.restart = true;
                ps2x::session::addSetting(previews);
            }
            if (offers(GameOption::BmxLogoMovie))
            {
                st::Setting logo = toggle("game.logo_movie", "Z-Axis Logo Movie", &Settings::bmxLogoMovie,
                                          "Adds it to the movie gallery.");
                logo.restart = true;
                ps2x::session::addSetting(logo);
            }
            if (offers(GameOption::BmxGcLevels))
            {
                st::Setting levels = toggle("game.gc_levels", "GameCube Levels", &Settings::bmxGcLevels,
                                            "The Secret Level and three test levels.");
                levels.restart = true;
                ps2x::session::addSetting(levels);
            }
            if (offers(GameOption::BmxMenuLines))
            {
                st::Setting lines = toggle("game.menu_lines", "Unused Menu Lines", &Settings::bmxMenuLines,
                                           "Hidden lines back in the game's menus.");
                lines.restart = true;
                ps2x::session::addSetting(lines);
            }
            if (offers(GameOption::BmxAllMpModes))
            {
                st::Setting modes = toggle("game.all_mp_modes", "All Multiplayer Modes", &Settings::bmxAllMpModes,
                                           "Every mode that works, in SELECT GAME.");
                modes.restart = true;
                ps2x::session::addSetting(modes);
            }
            if (offers(GameOption::BmxIntroMovie))
            {
                st::Setting introMovie = toggle("game.intro_movie", "Intro Movie", &Settings::bmxIntroMovie,
                                                "Plays the intro movie at start.");
                introMovie.restart = true;
                ps2x::session::addSetting(introMovie);
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
                ps2x::session::addSetting(unlock);
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
                ps2x::session::addSetting(tuning);
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
                ps2x::session::addSetting(scripts);
            }

            st::Setting ape = toggle("game.ape", "Ape Skater", &Settings::ape,
                                     "Unlocked with the Mummy.");
            ape.restart = true;
            if (offers(GameOption::Ape))
                ps2x::session::addSetting(ape);

            st::Setting freeSkate = toggle("game.xbox_free_skate", "Xbox Free Skate", &Settings::xboxFreeSkate,
                                           "Doors and rides in Boardwalk and Cannery.");
            freeSkate.restart = true;
            if (offers(GameOption::XboxFreeSkate))
                ps2x::session::addSetting(freeSkate);
        }

        // Native save files (the game override) unless PS2X_SAVES=card.
        bool nativeSaves()
        {
            const char *mode = ps2x::boot::text("PS2X_SAVES");
            return !(mode && std::string(mode) == "card");
        }

        // The native save file the game override uses for this profile.
        std::filesystem::path nativeSaveFile()
        {
            const char *dir = ps2x::boot::text("PS2X_SAVES_DIR");
            const char *profile = ps2x::boot::text("PS2X_SAVE_PROFILE");
            return std::filesystem::path(dir ? dir : "") /
                   (ps2x::saves::safeFileName(profile ? profile : "", "Career") + ".sav");
        }

        // Offers once to copy the game's save from a PCSX2 card: into the
        // native save file, or into card slot 1 with PS2X_SAVES=card.
        void offerPcsx2Import(AppState &state)
        {
            const std::string saveEntry = g_game->saveDir;
            if (saveEntry.empty())
                return;
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
                return native ? importCardSaveToFile(card, saveDir, saveFile, overwrite, &error, dataFile)
                              : importCardSave(card, saveDir, state.paths.memcardSlot1, overwrite, &error);
            };

            // PS2X_APP_IMPORT_CARD=<card.ps2>: import without asking (automation).
            if (const char *card = std::getenv("PS2X_APP_IMPORT_CARD"); card && card[0])
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
        // instead of the user's folders (unless PS2X_APP_SUPPORT_DIR says).
        void applyPortableMode(const char *argv0)
        {
            if (envSet("PS2X_APP_SUPPORT_DIR"))
                return;
            const std::filesystem::path folder = appFolder(argv0);
            std::error_code ec;
            if (!folder.empty() && std::filesystem::is_regular_file(folder / "portable.txt", ec))
                g_portableSupportDir = (folder / (std::string(PS2X_APP_NAME) + " Data")).string();
        }

        // Screenshots: Pictures/<app> in the user's home, or Screenshots in
        // the support folder when that is set from outside (portable mode,
        // automated runs).
        std::filesystem::path screenshotFolder(const AppPaths &paths)
        {
            if (envSet("PS2X_APP_SUPPORT_DIR"))
                return paths.supportDir / "Screenshots";
#if defined(_WIN32)
            const char *home = getEnv("USERPROFILE");
#else
            const char *home = getEnv("HOME");
#endif
            if (!home || !home[0])
                return paths.supportDir / "Screenshots";
            if (kCollection)
                return std::filesystem::path(home) / "Pictures" / PS2X_APP_NAME / g_game->name;
            return std::filesystem::path(home) / "Pictures" / PS2X_APP_NAME;
        }

        // The game's code: linked into the app (a workshop build), else the module made from this disc on this
        // machine (collection/game_code.h): kept in the game's folder (code/), made now when there is none for this
        // disc and app (the setup makes it first: appGameCodeStep; this is for a game set up before).
        bool ensureGameCode(const AppState &state, const GameProfile &game, const std::filesystem::path &discDir,
                            const DiscCheck &disc, const std::filesystem::path &toolsFolder, std::string &error)
        {
            if (ps2x::game_module::hasCodeFor(disc.bootElf))
                return true;
            ps2x::app::GameCodeTools tools;
            if (!tools.open(toolsFolder, &error))
                return false;
            ps2x::app::GameCodeRequest request;
            request.elf = discDir / disc.bootElf;
            request.id = ps2x::app::gameCodeGame(tools, game.id, request.elf);
            request.cache = ps2x::app::gameCodeCache(state.paths.supportDir);
            const std::filesystem::path file = ps2x::app::gameCodeFile(tools, request);
            std::error_code ec;
            if (!std::filesystem::exists(file, ec))
            {
                std::filesystem::create_directories(request.cache, ec);
                std::cout << "[app] game code: making " << file.string() << " from " << request.elf.string() << std::endl;
                const auto t0 = std::chrono::steady_clock::now();
                std::string last;
                const bool made = ps2x::app::makeGameCode(tools, request, [&last](const ps2x::app::GameCodeProgress &p)
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
            if (!ps2x::game_module::load(file.string(), error))
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

        // The game's files: PS2X_APP_DISC_DIR, else those set up from the
        // player's disc (<game folder>/disc), else those bundled with the
        // app when it was built (tools/make-app.sh: Resources/disc, or
        // Resources/games/<id>/disc in a collection).
        std::filesystem::path discFolder(const char *argv0, const GameProfile &game)
        {
            if (const char *forced = std::getenv("PS2X_APP_DISC_DIR"); forced && forced[0])
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
        if (kCollection && arg == "--game")
            return true;
        // The setup from the command line (setupCommand), and a game's code made as the first run makes it
        // (makeGameCodeCommand).
        if (arg == "--setup" || arg == "--check-games" || arg == "--remove-game" || arg == "--make-game-code")
            return true;
        return arg.rfind("-psn_", 0) == 0; // old Finder launches
    }

    namespace
    {
        // --game <id> or PS2X_APP_GAME (developers, checks): the game to
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
                if (const char *env = getEnv("PS2X_APP_GAME"); env && env[0])
                    wanted = env;
            if (wanted.empty())
                return -1;
            for (size_t i = 0; i < kGameCount; ++i)
                if (lowerAscii(kGamesInOrder[i].id) == lowerAscii(wanted))
                    return static_cast<int>(i);
            std::cerr << "[app] no game \"" << wanted << "\" in this app" << std::endl;
            return -1;
        }

        // --map <folder> or PS2X_APP_MAP (developers, the playbot's rides),
        // with --game: Custom Freeplay on the maps folder's map of that
        // folder name, no menu first; "" none.
        std::string developerMap(int argc, char *argv[])
        {
            for (int i = 1; i + 1 < argc; ++i)
                if (std::string(argv[i]) == "--map")
                    return argv[i + 1];
            if (const char *env = getEnv("PS2X_APP_MAP"); env && env[0])
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
                if (!envSet("PS2X_APP_SUPPORT_DIR") && std::filesystem::is_directory(legacy.supportDir, ec))
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
        namespace col = ps2x::collection;

        const char *g_argv0 = nullptr;

        // The app's tools for making the game code: Resources/game-code-tools, or PS2X_GAME_CODE_TOOLS.
        std::filesystem::path gameCodeToolsFolder()
        {
            const char *forced = std::getenv("PS2X_GAME_CODE_TOOLS");
            return forced && forced[0] ? std::filesystem::path(forced) : platform::resourceFolder(g_argv0) / "game-code-tools";
        }

        // The setup's code step (collection/game_setup.h): the game's code made from the installed disc when the app
        // has the tools for it (the compile shown on the setup's screens), else the code built into the app.
        std::unique_ptr<GameCodeStep> appGameCodeStep()
        {
            const std::filesystem::path tools = gameCodeToolsFolder();
            if (ps2x::app::GameCodeTools probe; probe.open(tools, nullptr))
                return std::make_unique<ps2x::app::BuiltGameCode>(tools);
            return makeGameCodeStep();
        }
        // The runtime of the game running, nullptr on the menu: the menu
        // bar's items act on it.
        PS2Runtime *g_runtime = nullptr;

        // The host's window: none for a null-GS or PS2X_HEADLESS run (as
        // PS2Runtime::initialize decides when it makes the host itself).
        bool headlessHost()
        {
            const char *headless = std::getenv("PS2X_HEADLESS");
            return headless && headless[0] ? std::strcmp(headless, "0") != 0 : gsNullRequested();
        }

        // Before the window is made: the Windows icon, SDL's own messages
        // (PS2X_SDL_LOG=1: video, audio, GPU drivers).
        void prepareHost()
        {
#if defined(_WIN32) && defined(PS2X_APP_HAS_ICON)
            // The executable's icon resource for the window too.
            SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
            SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
            SDL_SetLogPriorities(envSet("PS2X_SDL_LOG") ? SDL_LOG_PRIORITY_VERBOSE : SDL_LOG_PRIORITY_WARN);
        }

        // The menu bar (View, File): its items act on the game running, if
        // any. Installed once, when the window is there.
        void installAppMenus(AppState &state, const AppPaths &folders)
        {
            platform::MenuActions actions;
            actions.integerScaling = [&state]()
            { return state.settings.scaling == Scaling::Integer; };
            actions.toggleIntegerScaling = [&state]()
            { ps2x::settings::set("display.scaling", state.settings.scaling == Scaling::Integer ? 0 : 1); };
            // The collection's item is there for every game, greyed out where
            // the game has no widescreen.
            if (kMenu || gameHas(GameOption::Widescreen))
            {
                actions.widescreen = []()
                { return g_runtime && g_runtime->widescreen(); };
                actions.toggleWidescreen = []()
                {
                    if (g_runtime && gameHas(GameOption::Widescreen))
                        ps2x::settings::set("display.widescreen", g_runtime->widescreen() ? 0 : 1);
                };
                actions.widescreenAvailable = []()
                { return g_runtime && gameHas(GameOption::Widescreen); };
            }
            actions.pauseInBackground = [&state]()
            { return state.settings.pauseOnFocusLoss; };
            actions.togglePauseInBackground = [&state]()
            { ps2x::settings::set("display.pause_in_background", state.settings.pauseOnFocusLoss ? 0 : 1); };
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
                ps2x::settings::set("display.resolution", std::clamp(resolution, 1, 4) - 1);
                ps2x::settings::set("display.renderer", gpu ? 1 : 0);
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
            // Greyed out unless the game shows the settings in its own menus.
            platform::setOpenSettingsAction([]() { ps2x::settings::requestOpen(); },
                                            []() { return ps2x::settings::hasOpenHandler(); });
        }

        // How a boot ended, and the app's exit status where it ends the app.
        struct BootResult
        {
            col::GameEnd end = col::GameEnd::Quit;
            int status = 0;
            std::string why; // a boot that failed: what to tell the player
        };

        // A converted custom map's files as the boot's memory folder
        // (runtime/ps2_game_files.h), for the boot's life: every file system
        // of the game reads them over its mods, and nothing is written.
        class ConvertedMapFiles
        {
        public:
            explicit ConvertedMapFiles(ConvertedMap *map)
            {
                if (!map)
                    return;
                ps2x::game_files::MemoryFolder files;
                for (auto &[path, bytes] : map->files)
                    files[path] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
                map->files.clear();
                ps2x::game_files::registerMemoryFolder(kConvertedMapFolder, std::move(files));
                m_registered = true;
            }
            ~ConvertedMapFiles()
            {
                if (m_registered)
                    ps2x::game_files::removeMemoryFolder(kConvertedMapFolder);
            }
            ConvertedMapFiles(const ConvertedMapFiles &) = delete;
            ConvertedMapFiles &operator=(const ConvertedMapFiles &) = delete;

        private:
            bool m_registered = false;
        };

        // One game's boot, from its folders to its runtime destroyed: its
        // settings and BootConfig, the runtime on the app's host, its
        // settings rows (the session's), the disc check, the ELF, the run,
        // its shutdown hooks. `settingsLoaded`: the app loaded the game's
        // settings already (a single-game app, which reads them first for
        // its log). `customMap`: Custom Freeplay's map; `converted`: its
        // files made into this game's (a map made in another game), given
        // to the boot in memory and freed when it ends.
        BootResult bootGame(AppState &state, const AppPaths &appPaths, const GameProfile &game,
                            const std::string &gamepadMappings, bool settingsLoaded, const CustomMap *customMap = nullptr,
                            ConvertedMap *converted = nullptr)
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

            // Relative paths of the debug switches (dumps, traces) land here.
            std::filesystem::current_path(state.paths.supportDir, ec);

            // What the game is given: its BootConfig (runtime/ps2_boot_config.h),
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
            panel::setGameFiles((std::filesystem::is_directory(folders.extras / "panel", ec) ? folders.extras / "panel"
                                                                                            : folders.resources / "panel")
                                    .string());
            folders.screenshots = screenshotFolder(state.paths);
            folders.gamepadMappings = gamepadMappings;
            // The collection's Mods folder (collection/mods.h): its mods for
            // this game, with the game's own folder's.
            if (kCollection && !g_appSettingsFile.empty())
                folders.sharedMods = g_appSettingsFile.parent_path() / kModsFolder;
            if (customMap)
            {
                // Custom Freeplay: this boot plays the map (collection/maps.h),
                // in the slot this game lends, its folder or its files
                // converted for this game.
                const MapSlot *slot = mapSlotOf(g_game->id);
                folders.customMap = customMap->folder;
                folders.customMapSlot = slot ? slot->slot : customMap->slot;
                folders.customMapSlotNpcs = customMap->slotNpcs && !converted;
                folders.customMapConverted = converted != nullptr;
            }
            ps2x::BootConfig config =
                gameBootConfig(kCollection ? g_game->id : "", state.settings, state.paths, *g_profile, folders, getEnv);
            std::cout << "[app] screenshots: " << config.text("PS2X_SCREENSHOT_DIR") << std::endl;
            if (customMap)
                std::cout << "[app] custom freeplay: " << customMap->name << " (" << customMap->folder.string() << ", made in "
                          << customMap->game << ") in slot " << folders.customMapSlot << ", start " << config.text("PS2X_DEV_START_LEVEL")
                          << ", mods " << config.text("PS2X_MODS_DIR")
                          << (converted ? ", its files converted, in memory (" + std::to_string(converted->files.size()) + " files, " +
                                              std::to_string(converted->bytes) + " bytes)"
                                        : std::string())
                          << std::endl;
            // (before the runtime: the game's file systems are made in its boot)
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
                const std::string backend = config.text("PS2X_GS_BACKEND");
                const std::string scale = config.text("PS2X_GS_SCALE");
                const bool gpu = backend == "gpu";
                std::cout << "[app] renderer: " << (gpu ? "GPU, internal resolution " : "CPU")
                          << (gpu ? (scale.empty() ? std::string("1") : scale) + "x" : "") << std::endl;
            }
            // Scripted and remote-controlled runs have no window focus; never
            // pause them.
            state.focusPauseAllowed = config.text("PS2X_PAD_SCRIPT").empty() && config.text("PS2X_HOST_INPUT_SCRIPT").empty() &&
                                      !envSet("PS2X_CONTROL");
            state.pausedForFocus = false;

            const std::filesystem::path discDir = discFolder(g_argv0, game);
            // Card and host roots before anything can touch them (the defaults
            // are folders next to the current directory or the ELF).
            {
                PS2Runtime::IoPaths early = PS2Runtime::getIoPaths();
                early.elfDirectory = state.paths.hostRoot;
                early.hostRoot = state.paths.hostRoot;
                early.mcRoot = state.paths.memcardSlot1;
                early.cdRoot = discDir;
                early.cdImage.clear();
                early.cdFromImage = false;
                PS2Runtime::setIoPaths(early);
            }
            // A leave asked for before this boot is not this game's.
            ps2_host::clearExitToMenu();

#if defined(PS2X_ENABLE_DEBUG_UI)
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

            PS2Runtime runtime(std::move(config));
            PS2Runtime::HostOptions options;
            options.windowWidth = state.settings.windowWidth;
            options.windowHeight = state.settings.windowHeight;
            options.highDpi = true;
            options.smoothFilter = true;
            options.integerScale = state.settings.scaling == Scaling::Integer;
            options.widescreen = state.settings.widescreen;
            // The app's: it shuts the host down itself when it quits.
            options.hostOwned = true;
            runtime.setHostOptions(options);

#if defined(PS2X_ENABLE_DEBUG_UI)
            // Hidden unless PS2X_DEBUG_UI=1; F1 still toggles it.
            {
                const char *debugUi = std::getenv("PS2X_DEBUG_UI");
                debugPanel.setVisible(debugUi && debugUi[0] == '1');
            }
            state.showFps = [&debugPanel](bool show) { debugPanel.setShowFps(show); };
            runtime.setDebugUiCallbacks(
                [](PS2Runtime &, void *userData)
                { static_cast<PS2DebugPanel *>(userData)->initialize(); },
                [](PS2Runtime &rt, void *userData)
                { static_cast<PS2DebugPanel *>(userData)->draw(rt); },
                [](PS2Runtime &, void *userData)
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

            offerPcsx2Import(state);

            // The bundled folder is the CD (read in place, never written). All
            // roots are set first: loading must not derive any from a folder
            // (the ELF is named as if it sat in the host folder).
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.cdRoot = discDir;
            paths.cdImage.clear();
            paths.cdFromImage = false;
            paths.hostRoot = state.paths.hostRoot;
            paths.mcRoot = state.paths.memcardSlot1;
            paths.elfDirectory = state.paths.hostRoot;
            PS2Runtime::setIoPaths(paths);
            if (!runtime.loadELFImage((state.paths.hostRoot / disc.bootElf).string(), disc.elf, false))
                return failed("The game program could not be loaded.", 1);
            if (nativeSaves())
                std::cout << "[app] saves: " << nativeSaveFile().string() << " (memory card folders, imported once: "
                          << state.paths.memcardSlot1.string() << ", " << state.paths.memcardSlot2.string() << ")"
                          << std::endl;
            else
                std::cout << "[app] memory cards: " << state.paths.memcardSlot1.string() << ", "
                          << state.paths.memcardSlot2.string() << std::endl;

            g_switchGamePresses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeySwitchGame);
            g_screenshotPresses = PSPadBackend::hotkeyPresses(ps2_host_input::kHotkeyScreenshot);
            runtime.setHostFrameCallback([&state](PS2Runtime &rt)
                                         { gameFrame(rt, state); });
            runtime.addVSyncHook([](uint8_t *rdram, uint64_t) { captureMapSnapshot(rdram); });
            std::cout << "[app] " << g_game->id << ": running" << std::endl;
            g_runtime = &runtime;
            runtime.run();
            ps2_game_overrides::runShutdownHooks(runtime);
            g_runtime = nullptr;

            // Window closed (close button, Cmd+Q, Alt+F4): the app quits. Left
            // for the menu, or the game ended by itself: the collection's
            // menu again.
            if (ps2_host::quitRequested())
                result.end = col::GameEnd::Quit;
            else
                result.end = kMenu ? col::GameEnd::ExitToMenu : col::GameEnd::Quit;
            ps2_host::clearExitToMenu();
            state.settings.fullscreen = state.fullscreen;
            saveSettingsNow(state);
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

        namespace ex = ps2x::app::extras;

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

        // A game's extras made again: from the discs recorded for it (each
        // read and checked again; one of an added disc's release left out)
        // and `added` (none: no new one), with the bottom screen's maps. The
        // player's words for how it went.
        std::string remakeExtras(const GameProfile &game, std::vector<ex::ExtrasSource> added, const SetupProgressFn &progress,
                                 bool &ok)
        {
            const std::filesystem::path dir = gameFolder(game);
            std::vector<ex::ExtrasSource> sources;
            for (const ex::RecordedDisc &recorded : ex::recordedExtras(dir))
            {
                if (std::any_of(added.begin(), added.end(), [&](const ex::ExtrasSource &source)
                                { return source.disc && recorded.release == source.disc->release; }))
                    continue;
                ex::ExtrasSource again = ex::checkExtrasSource(recorded.source, game.id, extrasOpener());
                if (!again.disc)
                {
                    // (moved or gone: its extras are left out until it is added again)
                    std::cout << "[extras] " << game.id << ": " << recorded.source << ": " << again.message << std::endl;
                    continue;
                }
                sources.push_back(std::move(again));
            }
            for (ex::ExtrasSource &source : added)
                sources.push_back(std::move(source));
            const auto started = std::chrono::steady_clock::now();
            const ex::ExtrasResult result =
                ex::makeExtras(game.id, dir, sources, ex::engineXboxModelMaker(), extrasProgress(progress));
            for (const std::string &line : result.log)
                std::cout << "[extras] " << line << std::endl;
            std::cout << "[extras] " << game.id << ": " << result.message << " ("
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s)" << std::endl;
            ok = result.ok;
            return result.message;
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
            constexpr const char *kSuggested = PS2X_APP_NAME " saves.zip";
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
            // Custom Freeplay: the player page's tiles (one per game) and
            // Back, the custom maps of the maps folder (listed again whenever
            // the map page opens) and the map page's rows for the player's
            // game.
            col::Layout playerLayout = col::layout(kWidth, kHeight, kGameCount);
            col::PageLayout playerPage = col::pageLayout(kWidth, kHeight, 0);
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

        // The Mods page's rows, every mod listed again.
        void refreshMods(MenuScreen &menu, AppState &state, const AppPaths &collection)
        {
            listLibrary(menu, state, collection);
            menu.modRows = col::modsPageRows(*menu.mods, kModsFolder);
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

        // What a game's Custom Freeplay start needs that the setup lacks, as
        // the map page says it (the only such start: AI's, through the Estate).
        constexpr const char *kStartNeeds = "Its custom maps start through the Estate: add its GameCube disc in Games.";

        // The custom maps of the collection's maps folder (collection/maps.h),
        // and the map page's rows for the player's game `game`: every map the
        // game can start (as it is, or converted from the game it was made in)
        // starts it (a Boot with the map's index); one that did not convert
        // for this game this session is listed with why. Previews are read
        // here (BMPs).
        void refreshMaps(MenuScreen &menu, const AppPaths &collection, int game)
        {
            menu.maps = listAllMaps(listSharedMods(sharedModsFolder(collection)), collection.supportDir / kMapsFolder);
            std::vector<col::MapChoice> choices;
            const GameProfile &player = kGamesInOrder[static_cast<size_t>(std::clamp<int>(game, 0, static_cast<int>(kGameCount) - 1))];
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
                choice.playable = mapPlayableIn(map, player.id);
                const auto failure = menu.conversionFailures.find(map.where + "|" + player.id);
                if (failure != menu.conversionFailures.end())
                {
                    choice.playable = false;
                    choice.why = failure->second;
                }
                else if (!choice.playable)
                    choice.why = "not for this game";
                std::vector<uint8_t> bytes;
                col::Image preview;
                if (map.readPreview(bytes) && col::readBmpImage(bytes, preview))
                    choice.preview = std::make_shared<const col::Image>(std::move(preview));
                else if (!map.preview.empty())
                    std::cout << "[app] custom freeplay: " << map.previewFile().string() << " not read (a BMP of 4, 8, 24 or 32 bits)"
                              << std::endl;
                choices.push_back(std::move(choice));
            }
            // The slot's start needs what the player's setup may lack (AI's: the
            // Estate, made from its GameCube disc): without it no map is offered.
            const MapSlot *slot = mapSlotOf(player.id);
            BootFolders folders;
            folders.resources = gameResources(g_argv0, player);
            folders.extras = ex::extrasFolder(gameFolder(player));
            const bool starts = slot && offered(slot->need, findInstalled(folders));
            menu.mapRows = col::freeplayMapRows(choices, player.name, kMapsFolder, starts,
                                                slot && !starts ? kStartNeeds : std::string());
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
            if (!mapNeedsConversion(map, player.id))
                return true;
            try
            {
                menu.converted = convertMap(map, player.id);
                std::cout << "[app] custom freeplay: " << map.name << " (made in " << map.game << ") converted for " << player.id << " in "
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

        // "Last played <date>" under a game's name: when its native save
        // file (<games/<id>>/Saves/<profile>.sav, the profile its own
        // settings' [saves] profile) was last written; "" with no save.
        std::string lastPlayedLine(const AppPaths &collection, const GameProfile &game)
        {
            const std::filesystem::path dir = collection.supportDir / "games" / game.id;
            Settings settings;
            loadCollectionSettings(g_appSettingsFile, dir / "settings.ini", settings);
            std::error_code ec;
            const auto written = std::filesystem::last_write_time(dir / "Saves" / (settings.saveProfile + ".sav"), ec);
            if (ec)
                return {};
            // The file clock's time as the system clock's (C++17 has no cast).
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return col::lastPlayedText(std::chrono::system_clock::to_time_t(system));
        }

        void refreshLastPlayed(MenuScreen &menu, const AppPaths &collection)
        {
            for (size_t i = 0; i < kGameCount && i < menu.tiles.size(); ++i)
                menu.tiles[i].lastPlayed = lastPlayedLine(collection, kGamesInOrder[i]);
        }

        // The font the app ships (collection/vector_font.h): from the app's
        // resources folder, or on Android from the APK's assets (SDL reads a
        // relative path there).
        bool loadShippedFont(col::BitmapFont &font, std::string &from)
        {
            std::vector<std::string> places = {(platform::resourceFolder(g_argv0) / col::kShippedFontFile).string()};
#if defined(__ANDROID__)
            places.push_back(col::kShippedFontFile);
#endif
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
                menu.gamesSubtitle = "Setting up...";
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
                    top(col::SettingsRow::Action, "Stop", "Stops setting up. Games already done stay set up.", {},
                        [&menu]() { menu.job->cancel = true; });
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
                     platform::ask("Import saves?", "Saves and settings of the same name are replaced.", "Import", "Keep"))
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
                        // (the settings in use now would be written over them)
                        if (result.ok)
                            result.message = "Imported " + std::to_string(result.files) + " files. Restart the app to use them.";
                    }
                    std::cout << "[saves] import: " << result.message << std::endl;
                    std::lock_guard lock(job.mutex);
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

        void loadMenu(MenuScreen &menu, AppState &state, const AppPaths &collection)
        {
            menu.rows = menuSettingsRows(state);
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
            menu.page = col::pageLayout(MenuScreen::kWidth, MenuScreen::kHeight, menu.rows.size());
            refreshMods(menu, state, collection);
            loadFrontEnd(menu);
            loadMenuFont(menu);
            refreshGames(menu);
            refreshLastPlayed(menu, collection);
            if (const char *spec = std::getenv("PS2X_MENU_SCRIPT"); spec && spec[0])
            {
                std::string error;
                if (menu.script.parse(spec, &error))
                    std::cout << "[app] menu script: " << menu.script.size() << " steps" << std::endl;
                else
                    std::cerr << "[app] PS2X_MENU_SCRIPT: " << error << std::endl;
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
            int selection = 0, pageRow = 0, freeplayGame = 0, revision = 0;
            bool onBack = false;

            explicit MenuPlace(const col::Shell &shell)
                : page(shell.page()), selection(shell.selection()), pageRow(shell.pageRow()), freeplayGame(shell.freeplayGame()),
                  revision(shell.revision()), onBack(shell.freeplayOnBack())
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
                return 2;
            case col::MenuPage::Mod:
                return 3;
            }
            return 0;
        }

        // The games' own sound for what an input did: a page opened or a
        // game started (select), a page closed (cancel), the selection moved
        // (move), a value changed (change), a direction that moved nothing
        // (blocked). None for no input.
        std::optional<col::MenuSound> menuSoundOf(const col::MenuInput &in, const col::MenuAction &action, const MenuPlace &before,
                                                  const col::Shell &shell)
        {
            const MenuPlace after(shell);
            if (action.kind != col::MenuAction::None)
                return col::MenuSound::Select;
            if (after.page != before.page)
                return pageDepth(after.page) > pageDepth(before.page) ? col::MenuSound::Select : col::MenuSound::Cancel;
            if (after.selection != before.selection || after.pageRow != before.pageRow || after.freeplayGame != before.freeplayGame ||
                after.onBack != before.onBack)
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
            for (size_t i = 0; i < kGameCount; ++i)
                lines.push_back(!(*menu.front)[i].installed ? "Not set up"
                                : menu.tiles[i].lastPlayed.empty() ? "Not played yet"
                                                                   : menu.tiles[i].lastPlayed);
            std::string key = std::to_string(chosen) + "|" + hint + "|" + (shell.freeplay() ? "f" : "") + "|" +
                              std::to_string(reinterpret_cast<uintptr_t>(menu.front.get()));
            for (const std::string &line : lines)
                key += "|" + line;
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
            if (!state.settings.introLogo || ps2_host::headless() || (std::getenv("PS2X_MENU_SCRIPT") && std::getenv("PS2X_MENU_SCRIPT")[0]))
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
                col::drawMenu(menu.canvas, layout, menu.tiles, menu.font, shell, PS2X_APP_NAME, menu.notice, rows, page, menu.hint, backdrop);
                presentMenu(menu, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        // The menu until a game is chosen or the app quits.
        void runMenu(AppState &state, const AppPaths &collection, col::Shell &shell, MenuScreen &menu)
        {
            ++menu.visits;
            menu.script.beginVisit();
            ps2_host::setWindowTitle(PS2X_APP_NAME);
            ps2_host::clearExitToMenu();
            refreshLastPlayed(menu, collection); // the game just left may have saved
            refreshMods(menu, state, collection);
            shell.setFreeplay(state.settings.customFreeplay);
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
                col::drawMenu(menu.canvas, layoutOf(), menu.tiles, menu.font, shell, PS2X_APP_NAME, menu.notice, rowsOf(), page, menu.hint,
                              backdropNow(now),
                              shell.page() != col::MenuPage::Games ? std::string()
                              : settingUp && menu.job->cancel     ? std::string("Stopping...")
                                                                  : menu.gamesSubtitle,
                              settingUp ? "Stop" : nullptr);
                drawnAt = now;
            };
            // PS2X_MENU_SHOT_DIR (developers, headless checks): every picture
            // of a change the menu shows (not its movie's pictures), and of
            // each step of a setup, as menu-<visit>-<n>.png.
            std::string jobShotStep, jobShown;
            double jobShotAt = -10.0;
            auto writeShot = [&]()
            {
                const char *dir = std::getenv("PS2X_MENU_SHOT_DIR");
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
                ps2x::settings::runPending();
                if (shell.freeplay() != state.settings.customFreeplay)
                {
                    // The setting changed on the Settings page: the tile comes or goes.
                    shell.setFreeplay(state.settings.customFreeplay);
                    menu.layout = tilesLayout(shell);
                }
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
                    const std::string state = jobLine(*menu.job, menu.hint) + (menu.job->cancel ? " (stopping)" : "");
                    // (the bottom screen's taps act on nothing while it runs: none kept for after it)
                    col::BottomScreen::shared().takeTaps();
                    if (state != jobShown)
                    {
                        jobShown = state;
                        redraw = true;
                    }
                    std::string line = settingUp ? std::string() : state;
                    if (menu.job->finished)
                    {
                        menu.job->worker.join();
                        std::string said;
                        std::optional<DiscSearch> found;
                        std::vector<SetupJob::GamePart> parts;
                        std::string where;
                        {
                            std::lock_guard lock(menu.job->mutex);
                            for (const std::string &message : menu.job->messages)
                                said += (said.empty() ? "" : "  ") + message;
                            found = std::move(menu.job->found);
                            parts = menu.job->parts;
                            where = menu.job->where;
                        }
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
                    std::vector<std::pair<int, std::string>> picks;
                    std::vector<std::filesystem::path> places;
                    for (const std::string &path : ps2_host::input().dropped)
                        places.push_back(pathFromUtf8(path));
                    for (const std::string &path : std::exchange(menu.scriptDrops, {}))
                        places.push_back(pathFromUtf8(path));
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
                const MenuPlace before(shell);
                col::MenuAction action = shell.input(in, layoutOf(), rowsOf(), pageOf());
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
                menu.modsChanged = false;
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
                        if (mapNeedsConversion(map, player.id))
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
                if (action.kind == col::MenuAction::Quit)
                {
                    std::cout << "[app] menu: quit" << std::endl;
                    leave();
                    shell.quit();
                    break;
                }
                if (shell.revision() != shown || menu.hint != shownHint)
                {
                    shown = shell.revision();
                    shownHint = menu.hint;
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
            if (shell.state() == col::State::Quit)
                leave();
        }

        // Quitting: the window's state kept, the host down, the process
        // ended (std::_Exit: nothing of the games' left for static
        // destructors to run into).
        [[noreturn]] void quitApp(AppState &state, int status)
        {
            state.settings.fullscreen = state.fullscreen;
            if (g_profile)
                saveSettingsNow(state);
            else if (!g_appSettingsFile.empty())
                // The collection's menu only: its own settings (the window).
                writeTextFile(g_appSettingsFile, formatSettings(state.settings, everyOptionProfile(), SettingsScope::App));
            std::cout << "[app] quit" << std::endl;
            ps2_host::shutdown();
            std::fflush(stdout);
            std::fflush(stderr);
#if defined(PS2X_PGO_GENERATE)
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
            ps2x::app::GameCodeTools tools;
            std::string error;
            if (!tools.open(argv[4], &error))
            {
                std::cerr << "[game-code] " << error << std::endl;
                return 2;
            }
            ps2x::app::GameCodeRequest request;
            request.id = argv[2];
            request.elf = argv[3];
            request.cache = argv[5];
            request.jobs = argc >= 7 ? static_cast<unsigned>(std::atoi(argv[6])) : 0u;
            request.target = argc >= 8 ? argv[7] : "";
            std::filesystem::create_directories(request.cache);
            const auto t0 = std::chrono::steady_clock::now();
            auto stepStart = t0;
            std::string step;
            const bool made = ps2x::app::makeGameCode(tools, request, [&](const ps2x::app::GameCodeProgress &p)
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
            std::cout << "[game-code] " << ps2x::app::gameCodeFile(tools, request).string() << " in "
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
        const AppPaths appPaths = resolveAppPaths(PS2X_APP_NAME, getEnv);
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
            std::cout << "[app] " << PS2X_APP_NAME << " " << PS2X_APP_VERSION << " started " << stamp << "\n"
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
            std::cout << "[app] " << PS2X_APP_NAME << " " << PS2X_APP_VERSION << " started " << stamp << "\n"
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
            if (!ps2_host::initialize(PS2X_APP_NAME, window))
            {
                platform::showMessage(std::string(PS2X_APP_NAME) + " cannot start", "The window could not be created.", true);
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
        shell.start(wantedMap.empty() ? wanted : -1);
        if (wanted >= 0)
            std::cout << "[app] --game " << kGamesInOrder[wanted].id << ": no menu first" << std::endl;
        if (wanted >= 0 && !wantedMap.empty())
        {
            // The map started as the map page starts it (converted first when
            // it was made in another game).
            menu.maps = listAllMaps(listSharedMods(sharedModsFolder(appPaths)), appPaths.supportDir / kMapsFolder);
            int index = -1;
            for (size_t i = 0; i < menu.maps.size(); ++i)
                if (lowerAscii(menu.maps[i].folderName) == lowerAscii(wantedMap))
                    index = static_cast<int>(i);
            if (index < 0)
                std::cerr << "[app] --map " << wantedMap << ": no such map in " << sharedModsFolder(appPaths).string() << " or "
                          << (appPaths.supportDir / kMapsFolder).string() << std::endl;
            else if (!mapPlayableIn(menu.maps[static_cast<size_t>(index)], kGamesInOrder[wanted].id))
                std::cerr << "[app] --map " << wantedMap << ": not a map " << kGamesInOrder[wanted].id << " can start" << std::endl;
            else if (prepareMap(menu, menu.maps[static_cast<size_t>(index)], kGamesInOrder[wanted]))
            {
                std::cout << "[app] --map " << wantedMap << ": Custom Freeplay on " << menu.maps[static_cast<size_t>(index)].name << std::endl;
                shell.boot(wanted, index);
            }
        }
        if (shell.state() == col::State::Menu)
            playIntro(state, menu);
        while (shell.state() != col::State::Quit)
        {
            if (shell.state() == col::State::Menu)
            {
                runMenu(state, appPaths, shell, menu);
                continue;
            }
            // A Custom Freeplay boot: the map chosen on the map page.
            const CustomMap *map =
                shell.map() >= 0 && shell.map() < static_cast<int>(menu.maps.size()) ? &menu.maps[static_cast<size_t>(shell.map())] : nullptr;
            const BootResult result =
                bootGame(state, appPaths, kGamesInOrder[shell.game()], gamepadMappings, false, map, map && menu.converted ? &*menu.converted : nullptr);
            menu.converted.reset();
            menu.notice = result.why;
            shell.gameEnded(result.end);
        }
        quitApp(state, 0);
    }
}

namespace
{
    // The runner's main starts the app (runtime/ps2_front_end.h).
    [[maybe_unused]] const bool g_installed = ps2x::front_end::installApp({&ps2x_app::startedAsApp, &ps2x_app::runApp});
}
