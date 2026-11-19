#include "platform.h"

#include <iostream>

// The console fallback of the builds with no front end of their own (the
// Linux test builds): no dialogs, so a question is answered no.

namespace ps2x_app::platform
{
    std::filesystem::path resourceFolder(const char *argv0)
    {
        std::error_code ec;
        return std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
    }

    void showMessage(const std::string &title, const std::string &message, bool critical)
    {
        (void)critical;
        std::cerr << "[app] " << title << ": " << message << std::endl;
    }

    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no)
    {
        (void)yes;
        std::cerr << "[app] " << title << ": " << message << " (" << no << ")" << std::endl;
        return false;
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

    // The window library's full screen (a borderless desktop-sized one) is
    // what the other platforms use for both modes.
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
