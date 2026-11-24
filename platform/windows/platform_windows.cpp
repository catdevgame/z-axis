#include "platform/platform.h"

#include "host_backend.h"
#include "host_sdl.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Windows front end: the game files sit next to the executable, messages are
// message boxes. The macOS menus' items are in the window's system menu (the
// title bar's icon, a right click on the title bar, or Alt+Space in every
// display mode): the app menu's Settings..., the View menu's toggles (Full
// Screen, Integer Scaling, Widescreen, Renderer, Pause in Background, Map
// Window, each acting on the same setting as the macOS item) and the File
// menu's Switch Game..., Show Settings Folder and Show Logs Folder. Full
// screen is also Alt+Enter or F11 (in app.cpp). Full Screen is the window
// library's; Borderless is the game window itself, without its frame, over
// the whole display.

namespace zx_app::platform
{
    namespace
    {
        std::wstring wide(const std::string &text)
        {
            if (text.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        // The game window, so that boxes are modal to it.
        HWND ownerWindow()
        {
            HWND window = GetActiveWindow();
            return window ? window : nullptr;
        }

        // The game window (the host's SDL window), not the map window.
        HWND gameWindow()
        {
            SDL_Window *window = ps2_host::sdlWindow();
            if (!window)
                return nullptr;
            return static_cast<HWND>(
                SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        }

        std::unique_ptr<MenuActions> g_actions;
        std::function<void()> g_openSettings;
        std::function<bool()> g_openSettingsAvailable;

        // The system menu's own items. Their ids are below 0xF000 (the
        // system's SC_ commands) with the low four bits clear, which
        // Windows keeps for itself in WM_SYSCOMMAND.
        enum MenuItem : UINT
        {
            kSettingsItem = 0x0010,
            kSwitchGameItem = 0x0020,
            kSettingsFolderItem = 0x0030,
            kLogsFolderItem = 0x0040,
            kFullScreenItem = 0x0050,
            kIntegerScalingItem = 0x0060,
            kWidescreenItem = 0x0070,
            kPauseInBackgroundItem = 0x0080,
            kMapWindowItem = 0x0090,
            // The Renderer submenu: the CPU renderer, then the GPU one at
            // 1x to 4x (kRendererGpuItem + (scale - 1) * kRendererStep).
            kRendererCpuItem = 0x0100,
            kRendererGpuItem = 0x0110,
            kRendererStep = 0x0010,
        };
        constexpr int kRendererScales = 4;
        HMENU g_rendererMenu = nullptr;

        // The window's procedure before the menu's (the window library's).
        WNDPROC g_windowProcedure = nullptr;

        void showFolder(const std::filesystem::path &folder)
        {
            ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        bool g_borderless = false;

        // Full screen or borderless, as Alt+Enter toggles it (app.cpp): the
        // borderless window goes back to a window, else the window
        // library's full screen.
        bool fullScreenNow()
        {
            return g_borderless || ps2_host::windowFullscreen();
        }

        void toggleFullScreenItem()
        {
            if (!toggleFullscreen())
                ps2_host::setWindowFullscreen(!ps2_host::windowFullscreen());
        }

        void call(const std::function<void()> &action)
        {
            if (action)
                action();
        }

        // An item chosen: true when it is one of the app's.
        bool runMenuItem(UINT item)
        {
            switch (item)
            {
            case kSettingsItem:
                if (g_openSettings)
                    g_openSettings();
                return true;
            case kSwitchGameItem:
                if (g_actions && g_actions->switchGame)
                    g_actions->switchGame();
                return true;
            case kSettingsFolderItem:
                if (g_actions)
                    showFolder(g_actions->settingsFolder);
                return true;
            case kLogsFolderItem:
                if (g_actions)
                    showFolder(g_actions->logsFolder);
                return true;
            case kFullScreenItem:
                toggleFullScreenItem();
                return true;
            case kIntegerScalingItem:
                if (g_actions)
                    call(g_actions->toggleIntegerScaling);
                return true;
            case kWidescreenItem:
                if (g_actions)
                    call(g_actions->toggleWidescreen);
                return true;
            case kPauseInBackgroundItem:
                if (g_actions)
                    call(g_actions->togglePauseInBackground);
                return true;
            case kMapWindowItem:
                if (g_actions)
                    call(g_actions->toggleMapWindow);
                return true;
            case kRendererCpuItem:
                if (g_actions && g_actions->setRenderer)
                    g_actions->setRenderer(false, g_actions->internalResolution ? g_actions->internalResolution() : 1);
                return true;
            default:
                if (item >= kRendererGpuItem && item < kRendererGpuItem + kRendererScales * kRendererStep)
                {
                    if (g_actions && g_actions->setRenderer)
                        g_actions->setRenderer(true, static_cast<int>((item - kRendererGpuItem) / kRendererStep) + 1);
                    return true;
                }
                return false;
            }
        }

        // Borderless: the window's place before (restored, maximized or not).
        WINDOWPLACEMENT g_savedPlacement{};

        void setChecked(HMENU menu, UINT item, bool on)
        {
            CheckMenuItem(menu, item, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }

        // Greyed out as the macOS items are: Settings... without a settings
        // page of the game's own, Switch Game... on the collection's menu,
        // Widescreen in a game without it. Ticked as they are: each toggle
        // by its setting, the renderer in use.
        void updateMenuItems(HMENU menu)
        {
            const bool settings = g_openSettings && (!g_openSettingsAvailable || g_openSettingsAvailable());
            EnableMenuItem(menu, kSettingsItem, MF_BYCOMMAND | (settings ? MF_ENABLED : MF_GRAYED));
            if (!g_actions)
                return;
            if (g_actions->switchGame)
            {
                const bool switchGame = !g_actions->switchGameAvailable || g_actions->switchGameAvailable();
                EnableMenuItem(menu, kSwitchGameItem, MF_BYCOMMAND | (switchGame ? MF_ENABLED : MF_GRAYED));
            }
            setChecked(menu, kFullScreenItem, fullScreenNow());
            if (g_actions->integerScaling)
                setChecked(menu, kIntegerScalingItem, g_actions->integerScaling());
            if (g_actions->toggleWidescreen)
            {
                if (g_actions->widescreen)
                    setChecked(menu, kWidescreenItem, g_actions->widescreen());
                const bool widescreen = !g_actions->widescreenAvailable || g_actions->widescreenAvailable();
                EnableMenuItem(menu, kWidescreenItem, MF_BYCOMMAND | (widescreen ? MF_ENABLED : MF_GRAYED));
            }
            if (g_actions->pauseInBackground)
                setChecked(menu, kPauseInBackgroundItem, g_actions->pauseInBackground());
            if (g_actions->toggleMapWindow && g_actions->mapWindow)
                setChecked(menu, kMapWindowItem, g_actions->mapWindow());
            if (g_rendererMenu && g_actions->gpuRenderer && g_actions->internalResolution)
            {
                const int scale = std::clamp(g_actions->internalResolution(), 1, kRendererScales);
                const UINT picked = g_actions->gpuRenderer() ? kRendererGpuItem + (scale - 1) * kRendererStep
                                                             : kRendererCpuItem;
                CheckMenuRadioItem(g_rendererMenu, kRendererCpuItem,
                                   kRendererGpuItem + (kRendererScales - 1) * kRendererStep, picked, MF_BYCOMMAND);
            }
        }

        // Alt+Space in a window without a system menu of its own (full
        // screen): the same menu at the window's top left corner, its pick
        // sent on as the system menu's would be.
        void showMenuAtCorner(HWND window)
        {
            const HMENU menu = GetSystemMenu(window, FALSE);
            if (!menu)
                return;
            RECT frame{};
            GetWindowRect(window, &frame);
            updateMenuItems(menu);
            // (the window is the display's: Alt+Enter or F11 give the window back)
            for (const UINT command : {SC_RESTORE, SC_MOVE, SC_SIZE, SC_MAXIMIZE})
                EnableMenuItem(menu, command, MF_BYCOMMAND | MF_GRAYED);
            const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                                 frame.left, frame.top, 0, window, nullptr));
            if (picked != 0)
                SendMessageW(window, WM_SYSCOMMAND, picked, 0);
        }

        LRESULT CALLBACK menuWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_SYSCOMMAND:
                if (runMenuItem(static_cast<UINT>(wParam & 0xFFF0)))
                    return 0;
                // Alt+Space opens the system menu. The window library takes
                // every other keyboard menu (Alt alone would stop the game).
                if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == VK_SPACE)
                {
                    if ((GetWindowLongW(window, GWL_STYLE) & WS_SYSMENU) != 0)
                        return DefWindowProcW(window, message, wParam, lParam);
                    showMenuAtCorner(window);
                    return 0;
                }
                break;
            case WM_SYSKEYDOWN:
                // Alt+Space is the system menu's: its Space is not the
                // game's (the menu itself comes from its WM_SYSCHAR). Its
                // release goes on to the window library: a Space held before
                // Alt went down ends there, never left down.
                if (wParam == VK_SPACE)
                    return 0;
                break;
            case WM_SYSCHAR:
                // (a window without WS_SYSMENU gets no SC_KEYMENU from it)
                if (wParam == VK_SPACE && (GetWindowLongW(window, GWL_STYLE) & WS_SYSMENU) == 0)
                {
                    showMenuAtCorner(window);
                    return 0;
                }
                break;
            case WM_INITMENUPOPUP:
            {
                const LRESULT result = CallWindowProcW(g_windowProcedure, window, message, wParam, lParam);
                if (reinterpret_cast<HMENU>(wParam) == GetSystemMenu(window, FALSE))
                    updateMenuItems(reinterpret_cast<HMENU>(wParam));
                return result;
            }
            default:
                break;
            }
            return CallWindowProcW(g_windowProcedure, window, message, wParam, lParam);
        }

        // The taskbar told the window is full screen, so that it goes
        // under the window while the window is active.
        void markFullscreenWindow(HWND window, bool fullscreen)
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ITaskbarList2 *taskbar = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(TaskbarList), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))))
            {
                if (SUCCEEDED(taskbar->HrInit()))
                    taskbar->MarkFullscreenWindow(window, fullscreen ? TRUE : FALSE);
                taskbar->Release();
            }
            if (SUCCEEDED(com))
                CoUninitialize();
        }

        // Square corners while the window covers the display (Windows 11
        // rounds a window's corners; no effect before it).
        void setRoundedCorners(HWND window, bool rounded)
        {
            const DWM_WINDOW_CORNER_PREFERENCE corners = rounded ? DWMWCP_DEFAULT : DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        }
    }

    std::filesystem::path resourceFolder(const char *argv0)
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (n == 0)
                break;
            if (n < path.size())
            {
                path.resize(n);
                return std::filesystem::path(path).parent_path();
            }
            path.resize(path.size() * 2);
        }
        std::error_code ec;
        return std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
    }

    void showMessage(const std::string &title, const std::string &message, bool critical)
    {
        std::cerr << "[app] " << title << ": " << message << std::endl;
        MessageBoxW(ownerWindow(), wide(message).c_str(), wide(title).c_str(),
                    MB_OK | (critical ? MB_ICONERROR : MB_ICONINFORMATION));
    }

    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no)
    {
        // A message box has fixed Yes/No buttons: say what they do.
        const std::string text = message + "\n\nYes: " + yes + "\nNo: " + no;
        return MessageBoxW(ownerWindow(), wide(text).c_str(), wide(title).c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    void installMenus(const MenuActions &actions)
    {
        const HWND window = gameWindow();
        if (!window || g_windowProcedure)
            return;
        g_actions = std::make_unique<MenuActions>(actions);

        // At the top of the system menu: Settings..., Switch Game... (a
        // collection), the macOS View menu's items in its order, the two
        // folders, then the system's own items. Their keys (Alt+Space, then
        // T, G, U, I, W, E, P, D, F or L) are none of the system's.
        const HMENU menu = GetSystemMenu(window, FALSE);
        if (!menu)
            return;
        UINT at = 0;
        auto add = [&](UINT item, const wchar_t *title)
        { InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING, item, title); };
        auto separator = [&]()
        { InsertMenuW(menu, at++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr); };
        add(kSettingsItem, L"Se&ttings\u2026");
        if (g_actions->switchGame)
            add(kSwitchGameItem, L"Switch &Game\u2026");
        separator();
        add(kFullScreenItem, L"F&ull Screen\tAlt+Enter");
        add(kIntegerScalingItem, L"&Integer Scaling");
        if (g_actions->toggleWidescreen)
            add(kWidescreenItem, L"&Widescreen");
        // The renderer: the CPU one, or the GPU one at 1x-4x.
        g_rendererMenu = CreatePopupMenu();
        AppendMenuW(g_rendererMenu, MF_STRING, kRendererCpuItem, L"&CPU (Accurate)");
        AppendMenuW(g_rendererMenu, MF_SEPARATOR, 0, nullptr);
        const wchar_t *const gpuTitles[kRendererScales] = {L"GPU, &Native (640x448)", L"GPU, &2x (1280x896)",
                                                           L"GPU, &3x (1920x1344)", L"GPU, &4x (2560x1792)"};
        for (int scale = 1; scale <= kRendererScales; ++scale)
            AppendMenuW(g_rendererMenu, MF_STRING, kRendererGpuItem + (scale - 1) * kRendererStep, gpuTitles[scale - 1]);
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(g_rendererMenu),
                    L"Rend&erer");
        add(kPauseInBackgroundItem, L"&Pause in Background");
        if (g_actions->toggleMapWindow)
            add(kMapWindowItem, L"Map Win&dow");
        separator();
        add(kSettingsFolderItem, L"Show Settings &Folder");
        add(kLogsFolderItem, L"Show &Logs Folder");
        separator();

        g_windowProcedure = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&menuWindowProcedure)));
    }

    bool toggleFullscreen()
    {
        // Borderless goes back to the window; full screen is the window
        // library's.
        if (g_borderless)
            return setBorderless(false);
        return false;
    }

    bool isFullscreen(bool &known)
    {
        // Known while borderless (the window library sees a window then);
        // else the window library's.
        known = g_borderless;
        return g_borderless;
    }

    bool setBorderless(bool on)
    {
        SDL_Window *sdl = ps2_host::sdlWindow();
        const HWND window = gameWindow();
        if (!sdl || !window)
            return false;
        if (on == g_borderless)
            return true;
        if (on)
        {
            // From the window library's full screen or a maximized window:
            // the window as it is in a window first, the place kept.
            if ((SDL_GetWindowFlags(sdl) & SDL_WINDOW_FULLSCREEN) != 0)
            {
                SDL_SetWindowFullscreen(sdl, false);
                SDL_SyncWindow(sdl);
            }
            g_savedPlacement = {};
            g_savedPlacement.length = sizeof(g_savedPlacement);
            GetWindowPlacement(window, &g_savedPlacement);
            if ((SDL_GetWindowFlags(sdl) & SDL_WINDOW_MAXIMIZED) != 0)
            {
                SDL_RestoreWindow(sdl);
                SDL_SyncWindow(sdl);
            }
            SDL_SetWindowBordered(sdl, false);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT &display = monitor.rcMonitor;
            setRoundedCorners(window, false);
            SetWindowPos(window, HWND_TOP, display.left, display.top, display.right - display.left,
                         display.bottom - display.top, SWP_FRAMECHANGED);
            markFullscreenWindow(window, true);
        }
        else
        {
            markFullscreenWindow(window, false);
            setRoundedCorners(window, true);
            SDL_SetWindowBordered(sdl, true);
            SetWindowPlacement(window, &g_savedPlacement);
        }
        g_borderless = on;
        return true;
    }

    bool isBorderless()
    {
        return g_borderless;
    }

    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available)
    {
        g_openSettings = std::move(action);
        g_openSettingsAvailable = std::move(available);
    }

    void frameInterpolationChanged(bool on)
    {
        (void)on;
    }

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        (void)folder;
        (void)picked;
        return false;
    }

    bool pickDocumentsOnce(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        (void)folder;
        (void)picked;
        return false;
    }

    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created)
    {
        (void)name;
        (void)mimeType;
        (void)created;
        return false;
    }

    bool pickFolderOrFile(const std::string &extension, std::function<void(std::vector<std::string>)> picked)
    {
        // (IFileOpenDialog picks folders or files, never both at once: the menu asks
        // which, and the window library shows it)
        (void)extension;
        (void)picked;
        return false;
    }

    // ---- the app's own update (collection/updater.h) ---------------------------

    namespace
    {
        // A system tool (System32's curl.exe, tar.exe) run to its end with no
        // window, its output (both streams) into `output`; its exit code, -1
        // when it could not start.
        long runTool(const std::wstring &tool, const std::vector<std::wstring> &args, std::string *output)
        {
            wchar_t system[MAX_PATH];
            const UINT n = GetSystemDirectoryW(system, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return -1;
            const std::wstring exe = std::wstring(system) + L"\\" + tool;
            std::wstring line = L"\"" + exe + L"\"";
            for (const std::wstring &arg : args)
                line += L" \"" + arg + L"\"";
            SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
                return -1;
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = writeEnd;
            startup.hStdError = writeEnd;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION process{};
            const BOOL started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                                nullptr, &startup, &process);
            CloseHandle(writeEnd);
            if (started)
            {
                char buffer[4096];
                DWORD got = 0;
                while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0)
                    if (output && output->size() < 65536)
                        output->append(buffer, got);
            }
            CloseHandle(readEnd);
            if (!started)
                return -1;
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<long>(code);
        }

        std::string lastLine(std::string text)
        {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            const size_t newline = text.rfind('\n');
            return newline == std::string::npos ? text : text.substr(newline + 1);
        }

        std::filesystem::path executablePath()
        {
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (n == 0)
                    return {};
                if (n < path.size())
                {
                    path.resize(n);
                    return std::filesystem::path(path);
                }
                path.resize(path.size() * 2);
            }
        }

        // An executable's product version by its version resource's numbers ("1.3.1": a release's x.y.z, which
        // ps2/CMakeLists.txt writes as x,y,z,0), "" none. (The numbers, not the ProductVersion text: the text's name
        // as a string here would be the first one release_check.py finds in the executable, before the resource's.)
        std::string productVersion(const std::filesystem::path &exe)
        {
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
            if (size == 0)
                return {};
            std::vector<char> info(size);
            if (!GetFileVersionInfoW(exe.c_str(), 0, size, info.data()))
                return {};
            VS_FIXEDFILEINFO *fixed = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<void **>(&fixed), &length) || !fixed ||
                length < sizeof(VS_FIXEDFILEINFO))
                return {};
            return std::to_string(HIWORD(fixed->dwProductVersionMS)) + "." + std::to_string(LOWORD(fixed->dwProductVersionMS)) +
                   "." + std::to_string(HIWORD(fixed->dwProductVersionLS));
        }
    }

    zx::collection::UpdatePlatform updatePlatform(const char *argv0)
    {
        (void)argv0;
        zx::collection::UpdatePlatform update;
        const std::filesystem::path exe = executablePath();
        if (exe.empty())
            return update;
        update.install = zx::collection::UpdatePlatform::Install::Folder;
        update.packageSuffixes = {"-windows-x64.zip", "-windows.zip"};
        update.installed = exe.parent_path();
        update.updateFolder = exe.parent_path() / "update";
        update.download = [](const std::string &url, const std::filesystem::path &to, std::string *error)
        {
            // (curl.exe, Windows' own since Windows 10 1803: redirects followed, as GitHub's release downloads need)
            std::string output;
            const long code = runTool(L"curl.exe", {L"--fail", L"--silent", L"--show-error", L"--location", L"--retry", L"2",
                                                    L"--connect-timeout", L"20", L"--output", to.wstring(), wide(url)},
                                      &output);
            if (code != 0 && error)
                *error = output.empty() ? "curl.exe stopped (" + std::to_string(code) + ")" : lastLine(output);
            return code == 0;
        };
        update.unpack = [](const std::filesystem::path &zip, const std::filesystem::path &into, std::string *error)
        {
            // (tar.exe, Windows' own bsdtar, which reads zips)
            std::error_code ec;
            std::filesystem::create_directories(into, ec);
            std::string output;
            const long code = runTool(L"tar.exe", {L"-x", L"-f", zip.wstring(), L"-C", into.wstring()}, &output);
            if (code != 0 && error)
                *error = "tar.exe could not unpack " + zip.filename().string() + ": " + lastLine(output);
            return code == 0;
        };
        update.packageVersion = [name = exe.filename()](const std::filesystem::path &folder)
        { return productVersion(folder / name); };
        update.relaunch = [exe]() -> long
        {
            // (the executable now in place, in this one's environment but the script that drove this run)
            SetEnvironmentVariableW(L"ZX_MENU_SCRIPT", nullptr);
            std::wstring line = L"\"" + exe.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(),
                                &startup, &process))
                return -1;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<long>(process.dwProcessId);
        };
        return update;
    }
}
