#pragma once

// Native UI for the packaged app (Cocoa on macOS; elsewhere a console
// fallback). All calls are made on the window thread once the host window
// exists (ps2_host::initialize).

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x_app::platform
{
    // The app's own resources: Contents/Resources of the bundle on macOS,
    // elsewhere the executable's folder (argv0 resolved).
    std::filesystem::path resourceFolder(const char *argv0);
    void showMessage(const std::string &title, const std::string &message, bool critical);
    // Two buttons; true for the first.
    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no);

    struct MenuActions
    {
        std::function<bool()> integerScaling;
        std::function<void()> toggleIntegerScaling;
        // Empty for a game without widescreen (no such item).
        std::function<bool()> widescreen;
        std::function<void()> toggleWidescreen;
        std::function<bool()> pauseInBackground;
        std::function<void()> togglePauseInBackground;
        // The map window (the bottom screen in a second window; empty: no such item).
        std::function<bool()> mapWindow;
        std::function<void()> toggleMapWindow;
        std::function<bool()> gpuRenderer;
        std::function<int()> internalResolution; // 1-4
        std::function<void(bool gpu, int resolution)> setRenderer;
        std::filesystem::path settingsFolder;
        std::filesystem::path logsFolder;
        // Greyed out while this says no (empty: always there).
        std::function<bool()> widescreenAvailable;
        // A collection app: back to its menu (empty: no such item), greyed
        // out while switchGameAvailable says no (on the menu).
        std::function<void()> switchGame;
        std::function<bool()> switchGameAvailable;
    };
    // View menu (full screen, scaling, widescreen, renderer, pause in
    // background) and folder shortcuts, next to the window library's own app
    // and Window menus.
    void installMenus(const MenuActions &actions);

    // Native full screen where there is one; false: use the window library.
    bool toggleFullscreen();
    bool isFullscreen(bool &known);
    // Borderless full screen: the window covers the display, without the
    // system's full screen (no separate Space or animation on macOS). False
    // if the platform has none (the window library's full screen is used).
    bool setBorderless(bool on);
    bool isBorderless();
    // Opens the game's settings (the app menu's Settings... item, Cmd+,);
    // the item is greyed out while `available` says no (a game without a
    // settings page of its own).
    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available);

    // The system's own picker for a disc image or a disc's folder, where the
    // app reads picks as documents and not paths (Android: its documents
    // UI, a folder picked whole; collection/game_setup.h DocumentAccess).
    // The picks (document URIs, several files at once) come to `picked`, on
    // any thread, none when the player backs out. False: no such picker
    // here (the window library's dialogs give paths).
    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked);
    // The same for a new document the player names (`name` suggested) and
    // places: its URI to `created`. False: no such picker here.
    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created);
}
