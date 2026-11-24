#pragma once

// Native UI for the packaged app (Cocoa on macOS, Win32 on Windows;
// elsewhere a console fallback). All calls are made on the window thread
// once the host window exists (ps2_host::initialize).

#include "collection/updater.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace zx_app::platform
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
    // macOS: the View menu (full screen, scaling, widescreen, renderer, pause
    // in background) and the File menu (folder shortcuts, Switch Game...),
    // next to the window library's own app and Window menus. Windows: the
    // same items in the window's system menu.
    void installMenus(const MenuActions &actions);

    // Native full screen where there is one; false: use the window library.
    bool toggleFullscreen();
    bool isFullscreen(bool &known);
    // Borderless full screen: the window covers the display, without the
    // system's full screen (no separate Space or animation on macOS). False
    // if the platform has none (the window library's full screen is used).
    bool setBorderless(bool on);
    bool isBorderless();
    // Opens the game's settings (the app menu's Settings... item, Cmd+,; on
    // Windows the system menu's);
    // the item is greyed out while `available` says no (a game without a
    // settings page of its own).
    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available);

    // Frame Interpolation (the Display setting) turned on or off. Android: the
    // screen is asked for collection/refresh_rate.h's rate (120 Hz while it is
    // on, where the screen has 120; the game's 60 otherwise). Elsewhere
    // nothing: the display keeps the refresh the player gave it.
    void frameInterpolationChanged(bool on);

    // The system's own picker for a disc image or a disc's folder, where the
    // app reads picks as documents and not paths (Android: its documents
    // UI, a folder picked whole; collection/game_setup.h DocumentAccess).
    // The picks (document URIs, several files at once) come to `picked`, on
    // any thread, none when the player backs out. False: no such picker
    // here (the window library's dialogs give paths).
    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked);
    // The same for documents read once, now (a mod copied into the app's
    // folder): no permission to read them is kept past this run. False: no
    // such picker here.
    bool pickDocumentsOnce(bool folder, std::function<void(std::vector<std::string>)> picked);
    // The same for a new document the player names (`name` suggested) and
    // places: its URI to `created`. False: no such picker here.
    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created);
    // The system's one picker for a folder or a file of the type
    // `extension` ("zip": a mod's folder or its .zip), where it has one
    // (macOS: an open panel that takes both). The picked paths come to
    // `picked` on the window thread, none when the player backs out.
    // False: no picker here takes both (Windows' and Android's pick one or
    // the other: the caller asks which first).
    bool pickFolderOrFile(const std::string &extension, std::function<void(std::vector<std::string>)> picked);

    // The app's own update (collection/updater.h): how this platform
    // downloads, unpacks, puts a release's package in place and starts the
    // app again (macOS: the .app bundle swapped whole; Windows: the app
    // folder's files; Android: the system's package installer). Install::None
    // where there is none (the console builds, a runner outside a bundle).
    zx::collection::UpdatePlatform updatePlatform(const char *argv0);
}
