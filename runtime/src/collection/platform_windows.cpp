#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <string>

// Windows front end: the game files sit next to the executable, messages are
// message boxes. There is no menu bar: full screen is Alt+Enter or F11 (in
// app.cpp), and the game's own Settings pages have everything the macOS
// View menu has. Full screen is the window library's (a borderless window
// covering the display), for both full screen modes.

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
        (void)actions;
    }

    bool toggleFullscreen()
    {
        return false;
    }

    bool isFullscreen(bool &known)
    {
        known = false;
        return false;
    }

    bool setBorderless(bool on)
    {
        (void)on;
        return false;
    }

    bool isBorderless()
    {
        return false;
    }

    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available)
    {
        (void)action;
        (void)available;
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
