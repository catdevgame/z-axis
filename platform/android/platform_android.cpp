#include "platform/platform.h"

#include "android_documents.h"

#include <SDL3/SDL.h>

#include <iostream>

// Android's: the system's dialogs through SDL (its message boxes are the
// activity's own), the documents UI for discs (android_documents.cpp); no
// menu bar, and the window is always the whole screen.

namespace zx_app::platform
{
    std::filesystem::path resourceFolder(const char *argv0)
    {
        // (android_app.cpp's prepareApp makes argv[0] the external files folder's)
        std::error_code ec;
        return std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
    }

    void showMessage(const std::string &title, const std::string &message, bool critical)
    {
        std::cerr << "[app] " << title << ": " << message << std::endl;
        SDL_ShowSimpleMessageBox(critical ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, title.c_str(), message.c_str(),
                                 nullptr);
    }

    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no)
    {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no.c_str()},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes.c_str()},
        };
        const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(), message.c_str(), 2, buttons, nullptr};
        int pressed = 0;
        if (!SDL_ShowMessageBox(&box, &pressed))
        {
            std::cerr << "[app] " << title << ": " << message << " (" << no << ": " << SDL_GetError() << ")" << std::endl;
            return false;
        }
        return pressed == 1;
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
        return zx_android::pickDocuments(folder, std::move(picked));
    }

    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created)
    {
        return zx_android::createDocument(name, mimeType, std::move(created));
    }
}
