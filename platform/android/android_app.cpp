// The Android build's start and the second screen's painter (the top-level
// platform/android/ has the APK's files). Everything the app front end finds from
// the environment on Mac and Windows comes from the app's external files folder
// here: the game's files are pushed there (the folders of the desktop app's
// Contents/Resources), and HOME, so settings, saves and logs, is below it.
#if defined(__ANDROID__)

#include "android_assets.h"
#include "collection/menu_bottom.h"
#include "collection/panel.h"
#include "collection/panel_overlay.h"
#include "collection/playback.h"
#include "machine.h"
#include "runtime/front_end.h"

#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace zx_android
{
    namespace
    {
        constexpr size_t kGuestRamBytes = 32u * 1024u * 1024u;
        constexpr auto kPanelFrame = std::chrono::milliseconds(50); // about 20 Hz

        using Clock = std::chrono::steady_clock;

        double secondsNow()
        {
            return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        }

        // The latest game state, from the game thread to the painter's. The
        // game thread captures only while a panel is up.
        std::atomic<bool> g_panelUp{false};
        std::mutex g_snapshotMutex;
        panel::Snapshot g_snapshot;
        // When the game thread last captured: with no game running (the
        // collection's menu, a game ending) nothing captures, and the panel
        // would keep the last level's picture. After this long it is blank.
        std::atomic<double> g_captured{0.0};
        constexpr double kCaptureStale = 1.0; // seconds

        std::mutex g_touchMutex;
        std::vector<panel::TouchEvent> g_touches;

        std::thread g_painter;
        std::atomic<bool> g_painterRunning{false};

        // Off the game's frames and its GPU: pixels in a CPU buffer of the
        // second screen's surface. While the collection's menu is up, its
        // bottom screen (collection/menu_bottom.h BottomScreen) is drawn and
        // takes the touches; else the game's panel. One Panel, on this
        // thread only. Its files
        // are the running game's (panel::gameFiles, the app sets it as a game
        // boots: files/games/<id>/panel in the collection); `folder` (the
        // Activity's files/panel) until a game has booted.
        void paintPanel(ANativeWindow *window, std::string folder)
        {
            ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
            panel::Panel panel;
            panel.setDataFolder(folder);
            static panel::Snapshot snapshot; // (17 KB: not on this stack)
            std::vector<panel::TouchEvent> touches;
            std::unique_ptr<zx::collection::Canvas> menuCanvas; // the menu's bottom screen, the surface's size
            std::unique_ptr<zx::collection::Canvas> stripCanvas; // a playback's strip, the band's size
            double drawn = 0.0, worst = 0.0;
            int frames = 0;
            auto next = Clock::now();
            while (g_painterRunning.load(std::memory_order_acquire))
            {
                if (const std::string files = panel::gameFiles(); !files.empty() && files != folder)
                {
                    folder = files;
                    const std::string fallback = panel::gameFilesFallback();
                    panel.setDataFolder(folder, fallback);
                    std::fprintf(stderr, "[panel] files %s (else %s)\n", folder.c_str(), fallback.c_str());
                }
                {
                    std::lock_guard<std::mutex> lock(g_touchMutex);
                    touches.swap(g_touches);
                }
                zx::collection::BottomScreen &menuScreen = zx::collection::BottomScreen::shared();
                const bool menuUp = menuScreen.scene() != nullptr;
                // A recording playing back: its strip at the foot takes the
                // touches in its band (a finger that went down there is its
                // until it lifts); the panel the rest.
                zx::collection::PlaybackStrip &strip = zx::collection::PlaybackStrip::shared();
                const int surfaceWidth = menuCanvas ? menuCanvas->width() : 0, surfaceHeight = menuCanvas ? menuCanvas->height() : 0;
                for (const panel::TouchEvent &touch : touches)
                {
                    if (!menuUp && touch.phase == panel::TouchPhase::Down && strip.press(touch.x, touch.y, surfaceWidth, surfaceHeight))
                        continue;
                    if (!menuUp && (touch.phase == panel::TouchPhase::Up || touch.phase == panel::TouchPhase::Cancel) &&
                        strip.release(touch.x, touch.y, surfaceWidth, surfaceHeight))
                        continue;
                    if (!menuUp)
                        panel.touch(touch);
                    else if (touch.phase == panel::TouchPhase::Down)
                        menuScreen.press(touch.x, touch.y, menuCanvas ? menuCanvas->width() : 0, menuCanvas ? menuCanvas->height() : 0);
                    else if (touch.phase == panel::TouchPhase::Up)
                        menuScreen.release(touch.x, touch.y, menuCanvas ? menuCanvas->width() : 0, menuCanvas ? menuCanvas->height() : 0);
                }
                touches.clear();
                {
                    std::lock_guard<std::mutex> lock(g_snapshotMutex);
                    snapshot = g_snapshot;
                }
                if (secondsNow() - g_captured.load(std::memory_order_acquire) > kCaptureStale)
                    snapshot.mode = panel::GameMode::NoLevel; // nothing is running: blank, not the last level
                ANativeWindow_Buffer buffer{};
                if (ANativeWindow_lock(window, &buffer, nullptr) == 0)
                {
                    const auto begin = Clock::now();
                    if (!menuCanvas || menuCanvas->width() != buffer.width || menuCanvas->height() != buffer.height)
                        menuCanvas = std::make_unique<zx::collection::Canvas>(buffer.width, buffer.height);
                    if (menuUp && menuScreen.draw(*menuCanvas))
                    {
                        panel::setBottomScreenShows(0); // (the menu's bottom screen: no game's HUD pieces)
                        const std::vector<uint32_t> &pixels = menuCanvas->image().pixels;
                        for (int y = 0; y < buffer.height; ++y)
                            std::memcpy(static_cast<uint32_t *>(buffer.bits) + static_cast<size_t>(y) * static_cast<size_t>(buffer.stride),
                                        pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(buffer.width),
                                        static_cast<size_t>(buffer.width) * sizeof(uint32_t));
                    }
                    else
                    {
                        panel.draw(snapshot, secondsNow(),
                                   panel::Surface{static_cast<uint32_t *>(buffer.bits), buffer.width, buffer.height, buffer.stride});
                        // What the top screen may leave out (Clean Top Screen).
                        panel::setBottomScreenShows(panel.shows());
                        const zx::collection::Rect band = zx::collection::stripLayout(buffer.width, buffer.height).strip;
                        if (!stripCanvas || stripCanvas->width() != band.width || stripCanvas->height() != band.height)
                            stripCanvas = std::make_unique<zx::collection::Canvas>(band.width, band.height);
                        if (strip.draw(*stripCanvas, buffer.width, buffer.height))
                        {
                            const std::vector<uint32_t> &pixels = stripCanvas->image().pixels;
                            for (int y = 0; y < band.height; ++y)
                                std::memcpy(static_cast<uint32_t *>(buffer.bits) +
                                                static_cast<size_t>(band.y + y) * static_cast<size_t>(buffer.stride),
                                            pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(band.width),
                                            static_cast<size_t>(band.width) * sizeof(uint32_t));
                        }
                    }
                    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
                    ANativeWindow_unlockAndPost(window);
                    drawn += ms;
                    worst = std::max(worst, ms);
                    if (++frames == 200)
                    {
                        std::fprintf(stderr, "[panel] draw %.2f ms, worst %.2f ms, %dx%d\n", drawn / frames, worst,
                                     buffer.width, buffer.height);
                        drawn = worst = 0.0;
                        frames = 0;
                    }
                }
                next += kPanelFrame;
                const auto now = Clock::now();
                if (next > now)
                    std::this_thread::sleep_until(next);
                else
                    next = now;
            }
            ANativeWindow_release(window);
        }

        void stopPainter()
        {
            g_panelUp.store(false, std::memory_order_release);
            g_painterRunning.store(false, std::memory_order_release);
            if (g_painter.joinable())
                g_painter.join();
        }
    }

    // Game thread, at each vsync: read-only, it cannot change the game's logic.
    void capturePanelState(const uint8_t *rdram)
    {
        if (!rdram || !g_panelUp.load(std::memory_order_acquire))
            return;
        static panel::Snapshot captured;
        panel::captureSnapshot(rdram, kGuestRamBytes, captured);
        {
            std::lock_guard<std::mutex> lock(g_snapshotMutex);
            g_snapshot = captured;
        }
        g_captured.store(secondsNow(), std::memory_order_release);
    }

    // In the background SDL blocks the host loop inside its event pump, and
    // the app's per-frame pause never runs, so the game thread would go on
    // drawing with nothing presenting or reclaiming: GPU memory grew by over
    // 100 MB/s, to 8 GB. The watch is called inside that pump, before it blocks.
    void pauseInBackground(PS2Machine &runtime)
    {
        SDL_AddEventWatch(
            [](void *userdata, SDL_Event *event)
            {
                auto *running = static_cast<PS2Machine *>(userdata);
                if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
                    running->setHostPaused(true);
                else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
                    running->setHostPaused(false);
                return true;
            },
            &runtime);
    }

    // The game code folder (runtime/game_module.h, ZX_GAME_CODE_DIR): in
    // the app's internal storage, the one place Android loads a library
    // from that the app made or was given (the external files folder is
    // mounted noexec). A module built on a computer and pushed to the
    // external files folder's code/ is copied in first, when it is new or
    // has changed; the copies are read-only, as Android asks of code an app
    // loads itself. The folder is that code/'s copy: a module taken away
    // there is taken away here too (else an old one would still load, and
    // its code take its game's place).
    std::string gameCodeFolder(const std::string &externalFiles)
    {
        namespace fs = std::filesystem;
        const char *internal = SDL_GetAndroidInternalStoragePath();
        if (!internal)
            return {};
        const fs::path folder = fs::path(internal) / "code", given = fs::path(externalFiles) / "code";
        std::error_code ec;
        fs::create_directories(folder, ec);
        for (const fs::directory_entry &entry : fs::directory_iterator(given, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".so")
                continue;
            const fs::path copy = folder / entry.path().filename();
            std::error_code statEc;
            if (fs::file_size(copy, statEc) == entry.file_size(ec) && fs::last_write_time(copy, statEc) == entry.last_write_time(ec))
                continue;
            const fs::path partial = folder / (entry.path().filename().string() + ".partial");
            fs::remove(partial, ec);
            if (!fs::copy_file(entry.path(), partial, ec) || ec)
            {
                std::fprintf(stderr, "[android] game code %s not copied: %s\n", entry.path().c_str(), ec.message().c_str());
                continue;
            }
            fs::last_write_time(partial, entry.last_write_time(ec), ec);
            fs::permissions(partial, fs::perms::owner_read | fs::perms::group_read, ec);
            fs::remove(copy, ec);
            fs::rename(partial, copy, ec);
            std::fprintf(stderr, "[android] game code %s copied in\n", entry.path().filename().c_str());
        }
        for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec))
        {
            if (entry.path().extension() != ".so")
                continue;
            std::error_code givenEc;
            if (fs::exists(given / entry.path().filename(), givenEc) || givenEc)
                continue;
            std::error_code removeEc;
            fs::remove(entry.path(), removeEc);
            std::fprintf(stderr, "[android] game code %s %s (no longer in code/)\n", entry.path().filename().c_str(),
                         removeEc ? ("not removed: " + removeEc.message()).c_str() : "removed");
        }
        return folder.string();
    }

    // A file written and on the storage itself (fsync) before it is closed: a handheld that loses its power after the
    // folder is in place keeps the files' bytes, not only their names. False if any step fails.
    bool writeSynced(const std::filesystem::path &path, const void *bytes, size_t size)
    {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return false;
        const char *p = static_cast<const char *>(bytes);
        bool ok = true;
        for (size_t left = size; ok && left > 0;)
        {
            const ssize_t wrote = ::write(fd, p, left);
            if (wrote < 0 && errno == EINTR)
                continue;
            ok = wrote > 0;
            if (ok)
            {
                p += wrote;
                left -= static_cast<size_t>(wrote);
            }
        }
        ok = ::fsync(fd) == 0 && ok;
        return ::close(fd) == 0 && ok;
    }

    // A folder's entries (the names in it) on the storage.
    bool syncFolder(const std::filesystem::path &folder)
    {
        const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            return false;
        const bool ok = ::fsync(fd) == 0;
        return ::close(fd) == 0 && ok;
    }

    // The app's tools for making each game's code (collection/game_code.h), but for its programs (the APK's native
    // libraries): the APK's assets/game-code-tools, listed in its files.txt ("key <its key>", then one file a line),
    // unpacked into the internal storage's game-code-tools when that folder is not this APK's: its stamp names the
    // APK's versionCode and the assets' key, and the folder is unpacked again when either differs (a new APK, or none
    // unpacked yet). The folder; empty if the APK has none.
    //
    // Every file comes from the APK's assets themselves (android_assets.h), never through SDL's relative paths: SDL
    // opens those from the internal storage first, where the unpacked folder has the assets' own names, so an update
    // read the last version's files back and kept them under its own key (1.2.0 over 1.1.0: the game code then failed
    // to compile against 1.1.0's headers). The old folder goes before the new one is made, and the stamp has a name
    // of its own, so a folder that update filled from itself is unpacked once more.
    std::string gameCodeTools()
    {
        namespace fs = std::filesystem;
        const char *internal = SDL_GetAndroidInternalStoragePath();
        std::vector<uint8_t> list;
        if (!internal || !readAsset("game-code-tools/files.txt", list))
            return {};
        std::istringstream lines(std::string(list.begin(), list.end()));
        std::string key;
        std::getline(lines, key);
        const std::string apk = "apk " + std::to_string(apkVersionCode()) + " " + key;
        const fs::path folder = fs::path(internal) / "game-code-tools", stamp = folder / "unpacked-apk.txt";
        std::string unpacked;
        {
            std::ifstream in(stamp);
            std::getline(in, unpacked);
        }
        if (unpacked == apk)
            return folder.string();
        const auto t0 = std::chrono::steady_clock::now();
        const fs::path partial = fs::path(internal) / "game-code-tools.partial";
        std::error_code ec;
        fs::remove_all(partial, ec);
        fs::remove_all(folder, ec);
        size_t files = 0, bytes = 0;
        std::vector<uint8_t> data;
        std::set<fs::path> folders; // the .partial folder's folders, each synced before the rename
        for (std::string name; std::getline(lines, name);)
        {
            if (name.empty())
                continue;
            const bool read = readAsset("game-code-tools/" + name, data);
            fs::create_directories((partial / name).parent_path(), ec);
            for (fs::path up = (partial / name).parent_path(); up != partial && up.has_relative_path(); up = up.parent_path())
                folders.insert(up);
            if (!read || !writeSynced(partial / name, data.data(), data.size()))
            {
                std::fprintf(stderr, "[android] game code tools: %s not unpacked\n", name.c_str());
                return {};
            }
            ++files;
            bytes += data.size();
        }
        // The stamp last, once every file is in, every file and folder on the storage (fsync), then the folder put
        // in place in one rename and its parent synced: a start cut short, or the power lost, anywhere before leaves
        // no stamped folder, so the next start unpacks again; after it, the folder and its bytes are there.
        const std::string stampLine = apk + "\n";
        bool synced = writeSynced(partial / stamp.filename(), stampLine.data(), stampLine.size());
        folders.insert(partial);
        for (auto it = folders.rbegin(); synced && it != folders.rend(); ++it) // (deepest first, the .partial folder last)
            synced = syncFolder(*it);
        if (!synced)
        {
            std::fprintf(stderr, "[android] game code tools: not written to the storage\n");
            return {};
        }
        fs::rename(partial, folder, ec);
        if (!ec && !syncFolder(folder.parent_path()))
            ec = std::make_error_code(std::errc::io_error);
        if (ec)
        {
            std::fprintf(stderr, "[android] game code tools not kept in %s: %s\n", folder.c_str(), ec.message().c_str());
            return {};
        }
        std::fprintf(stderr, "[android] game code tools: %zu files (%zu bytes) unpacked from the APK's assets (%s, was %s) in %lld ms\n",
                     files, bytes, apk.c_str(), unpacked.empty() ? "none" : unpacked.c_str(),
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count()));
        return folder.string();
    }

    void prepareApp(int &argc, char **&argv)
    {
        const char *folder = SDL_GetAndroidExternalStoragePath();
        if (!folder)
        {
            std::fprintf(stderr, "[android] no external files folder: %s\n", SDL_GetError());
            return;
        }
        const std::string files = folder;
        setenv("HOME", files.c_str(), 1);
        setenv("ZX_APP_SUPPORT_DIR", (files + "/support").c_str(), 1);
        if (const std::string code = gameCodeFolder(files); !code.empty())
            setenv("ZX_GAME_CODE_DIR", code.c_str(), 1);
        if (const std::string tools = gameCodeTools(); !tools.empty())
            setenv("ZX_GAME_CODE_TOOLS", tools.c_str(), 1);
        // Developer switches, one NAME=value a line (the Activity takes no
        // environment): ZX_PERF_LOG, ZX_PAD_SCRIPT and the like.
        std::ifstream env(files + "/zx-env.txt");
        for (std::string line; std::getline(env, line);)
        {
            const size_t equals = line.find('=');
            if (line.empty() || line[0] == '#' || equals == std::string::npos)
                continue;
            setenv(line.substr(0, equals).c_str(), line.c_str() + equals + 1, 1);
            std::fprintf(stderr, "[android] %s\n", line.c_str());
        }
        // The stub platform's resource folder is argv[0]'s: the files folder.
        static std::string argv0 = files + "/zx-runner";
        static char *arguments[] = {argv0.data(), nullptr};
        argc = 1;
        argv = arguments;
    }
}

namespace
{
    // What the runtime asks of the Android build's start (runtime/front_end.h):
    // the files folder first, the window's pause in the background, the second
    // screen's snapshot every vsync.
    [[maybe_unused]] const bool g_installed = zx::front_end::installPrepare(&zx_android::prepareApp) &&
                                              zx::front_end::installWindowMade(&zx_android::pauseInBackground) &&
                                              zx::front_end::installVsync(&zx_android::capturePanelState);
}

// SecondScreenPanel: its surface (null when it goes: returns with the painter
// stopped) and the folder of the panel's files.
extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_SecondScreenPanel_nativePanelSurface(
    JNIEnv *env, jclass, jobject surface, jstring folder)
{
    using namespace zx_android;
    stopPainter();
    // (the bottom screen on this display: none over the game on the top one, collection/panel_overlay.h)
    panel::Overlay::shared().setSecondScreen(surface != nullptr);
    if (!surface)
        return;
    const char *chars = env->GetStringUTFChars(folder, nullptr);
    const std::string path = chars;
    env->ReleaseStringUTFChars(folder, chars);
    g_painterRunning.store(true, std::memory_order_release);
    g_panelUp.store(true, std::memory_order_release);
    g_painter = std::thread(paintPanel, ANativeWindow_fromSurface(env, surface), path);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_catdevgame_zaxiscollection_SecondScreenPanel_nativePanelTouch(
    JNIEnv *, jclass, jint phase, jint pointer, jfloat x, jfloat y)
{
    using namespace zx_android;
    const panel::TouchEvent event{static_cast<panel::TouchPhase>(phase), pointer, x, y, secondsNow()};
    std::lock_guard<std::mutex> lock(g_touchMutex);
    g_touches.push_back(event);
}

#endif // __ANDROID__
