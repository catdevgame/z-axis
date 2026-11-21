#include "platform/platform.h"

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

#include <iostream>
#include <memory>
#include <string>

// Windows front end: the game files sit next to the executable, messages are
// message boxes. The macOS app menu's and File menu's items (Settings...,
// Switch Game..., Show Settings Folder, Show Logs Folder) are in the window's
// system menu: the title bar's icon, a right click on the title bar, or
// Alt+Space in every display mode. Full screen is Alt+Enter or F11 (in
// app.cpp), and the game's own Settings pages have everything the macOS View
// menu has. Full Screen is the window library's; Borderless is the game
// window itself, without its frame, over the whole display.

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
        };

        // The window's procedure before the menu's (the window library's).
        WNDPROC g_windowProcedure = nullptr;

        void showFolder(const std::filesystem::path &folder)
        {
            ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
            default:
                return false;
            }
        }

        // Borderless: the window's place before (restored, maximized or not).
        bool g_borderless = false;
        WINDOWPLACEMENT g_savedPlacement{};

        // Greyed out as the macOS items are: Settings... without a settings
        // page of the game's own, Switch Game... on the collection's menu.
        void updateMenuItems(HMENU menu)
        {
            const bool settings = g_openSettings && (!g_openSettingsAvailable || g_openSettingsAvailable());
            EnableMenuItem(menu, kSettingsItem, MF_BYCOMMAND | (settings ? MF_ENABLED : MF_GRAYED));
            if (g_actions && g_actions->switchGame)
            {
                const bool switchGame = !g_actions->switchGameAvailable || g_actions->switchGameAvailable();
                EnableMenuItem(menu, kSwitchGameItem, MF_BYCOMMAND | (switchGame ? MF_ENABLED : MF_GRAYED));
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
        // collection), the two folders, then the system's own items. Their
        // keys (Alt+Space, then T, G, F or L) are none of the system's.
        const HMENU menu = GetSystemMenu(window, FALSE);
        if (!menu)
            return;
        UINT at = 0;
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING, kSettingsItem, L"Se&ttings\u2026");
        if (g_actions->switchGame)
            InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING, kSwitchGameItem, L"Switch &Game\u2026");
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING, kSettingsFolderItem, L"Show Settings &Folder");
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_STRING, kLogsFolderItem, L"Show &Logs Folder");
        InsertMenuW(menu, at++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

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

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
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
}
