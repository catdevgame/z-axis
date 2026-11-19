#pragma once

// What the app offers: a settings row, a choice in one or a mode shows only
// where it does something for this player's setup. What an option needs
// beyond the game's PS2 disc (an extras disc's content, the readable code it
// runs on, what the platform can do) is said once here (needOf), and what
// the setup has is found once (findInstalled), from what is actually there:
// the folders a boot is given (gameBootConfig: the extras made from the
// player's Xbox and GameCube discs, or the ones an app was built with) and
// the platform. Rows ask offered(); no row keeps a check of its own.

#include "collection/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ps2x::app
{
    // One thing an option needs that a setup may lack.
    enum class Need : uint8_t
    {
        Nothing,          // the game's PS2 disc alone
        // Aggressive Inline's Xbox disc (extras/xbox, PS2X_XBOX_DIR; xbox-converted, PS2X_XBOX_CONVERTED).
        XboxText,         // its other languages' string tables (string.fre ...)
        XboxSpeech,       // its French speech (audio/speech01.zsd)
        XboxLevelScripts, // its career levels' scripts (levels/<LEVEL>/<LEVEL>.bin)
        XboxFreeSkate,    // its free skate scripts of the Boardwalk and the Cannery (levels/<LEVEL>/<LEVEL>T.bin)
        XboxApe,          // the Ape: its model made at setup (GOR.ZIT) and the Xbox uilib.zal
        // The Xbox pack (xbox-pack, PS2X_XBOX_PACK).
        XboxTextures,     // textures/
        XboxSkaters,      // models/
        XboxMovies,       // movies/
        // Aggressive Inline's GameCube disc: the Estate (estate, PS2X_ESTATE_DIR).
        Estate,
        // Dave Mirra BMX 2's Xbox disc (xbox-levels, PS2X_DM2_XBOX_LEVELS_DIR).
        Dm2XboxLevels,    // levels.txt and the levels
        Dm2XboxThemes,    // themes/
        Dm2RetroMovie,    // movies/ASSETS/MOVIE/RETRO.PSS (or the Xbox's RETRO.BIK)
        // BMX XXX's Xbox and GameCube discs (releases, PS2X_BMX_RELEASES_DIR).
        BmxText,          // strings/: the fuller French, German and Spanish
        BmxChallenges,    // challenges/: the Xbox and GameCube challenge scripts
        BmxEditorPieces,  // editor/OBJECT.OBJ: the Pool and the Receptacle
        BmxLevelPreviews, // movies/ASSETS/MOVIES/MENU: the Xbox's level-select movies
        BmxLogoMovie,     // movies/ASSETS/MOVIES/ZAXIS.PSS (or the Xbox's ZAXIS.XMV)
        BmxGcLevels,      // levels/ASSETS/LEVELS: the GameCube's four levels
        // The code: Dave Mirra BMX 2's created riders' models are the readable
        // newRider's (dave_mirra::createPlayerCanRun).
        Dm2ReadableRider,
        // The platform (Android has one window, always the whole screen).
        SecondWindow,     // a window of its own beside the game's (the Map Window)
        WindowModes,      // a window and full screen to switch between
        Borderless,       // a borderless window covering the display (macOS)
        Count
    };

    // What a setup has: one flag per Need (Nothing always).
    struct Installed
    {
        std::array<bool, static_cast<size_t>(Need::Count)> has{};
        Installed() { set(Need::Nothing); }
        bool operator[](Need need) const { return has[static_cast<size_t>(need)]; }
        void set(Need need, bool on = true) { has[static_cast<size_t>(need)] = on; }
    };

    // The setup's folders as a boot is given them (`bootText`: the boot
    // config's PS2X_XBOX_DIR, PS2X_XBOX_CONVERTED, PS2X_XBOX_PACK,
    // PS2X_ESTATE_DIR, PS2X_DM2_XBOX_LEVELS_DIR, PS2X_BMX_RELEASES_DIR),
    // each looked into for the files its options read (any case), and this
    // build's platform. Dm2ReadableRider is the caller's to set (the game's).
    Installed findInstalled(const GetEnv &bootText);
    // The same for the folders a boot of the game would be given (releaseFolders):
    // the menu's, before the game boots.
    Installed findInstalled(const BootFolders &folders);
    // Only the platform's (the menu's rows, before any game has booted).
    Installed platformInstalled();

    // What `option` needs in `gameId` (Need::Nothing: the game's own).
    Need needOf(std::string_view gameId, GameOption option);
    // A row of the game's option: the game has it and the setup what it needs.
    bool offered(const GameSettingsProfile &game, GameOption option, const Installed &installed);
    // A row of the app's own that needs something (the Xbox pack's, the Map Window).
    bool offered(Need need, const Installed &installed);
}
