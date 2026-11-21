#pragma once

// Dave Mirra Freestyle BMX 2 (SLUS-20159) game modules. Each install
// function returns true when its hooks are in place; dave_mirra_bmx2.cpp
// calls them only for that ELF.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace zge_host
{
    struct ScriptFunctions;
}

namespace dave_mirra
{
    // Installs every DM2 module and decides which IRX modules to skip.
    void install(PS2Runtime &runtime);

    // sceGsPutDispEnv for the runtime's presenter (dave_mirra_display.cpp).
    bool installDisplay(PS2Runtime &runtime);

    // Widescreen 3D and draw distance in levels (dave_mirra_view.cpp).
    bool installView(PS2Runtime &runtime);

    // Restart in the pause menu, and Exit to Collection in the collection
    // (dave_mirra_restart.cpp).
    bool installRestart(PS2Runtime &runtime);
    // The Restart key's hook (with the gameplay keys, installGameplay).
    bool installRestartKey(PS2Runtime &runtime);

    // The gameplay pages (Camera, Assists, Cheats) and keys (Hide HUD, Restart), Aggressive Inline's at DM2's
    // code; with the app's settings (ZX_GAME_SETTINGS_MENUS=1;
    // dave_mirra_gameplay.cpp).
    void installGameplay(PS2Runtime &runtime);

    // The developer menu: F1 turns the game's debug display on and off
    // (ZX_DEV_MENU=1; dave_mirra_devtools.cpp).
    void installDevTools(PS2Runtime &runtime);

    // Exit to Collection in the front end's main menu, in the collection
    // only (dave_mirra_exit.cpp).
    bool installExitToCollection(PS2Runtime &runtime);
    // The engine's table of script functions in DM2 (dave_mirra_exit.cpp):
    // the host's go in after the front end's bmxCheckCheat.
    const zge_host::ScriptFunctions &scriptFunctions();
    // The Settings page in the front end's Options (dave_mirra_settings.cpp).
    void installSettingsMenus(PS2Runtime &runtime);
    // Create Player, the cut menu, with its created riders
    // (ZX_DM2_CREATE_PLAYER=1; dave_mirra_creator.cpp). After the readable
    // install: not added unless the readable newRider runs.
    bool installCreatePlayer(PS2Runtime &runtime);
    // Whether Create Player can be added at the next boot: the readable
    // newRider switched on (game::readable::switchedOnAtBoot). Before the
    // game starts (the app's Create Rider row).
    bool createPlayerCanRun();
    // Button prompts for the device in use (dave_mirra_prompts.cpp).
    void installPrompts(PS2Runtime &runtime);

    // Mouse-look through the follow camera's wanted eye
    // (dave_mirra_mouselook.cpp).
    bool installMouseLook(PS2Runtime &runtime);

    // DM2's Xbox-only levels in the level select, made at build time
    // (ZX_DM2_XBOX_LEVELS_DIR; dave_mirra_xboxlevel.cpp).
    bool installXboxLevels(PS2Runtime &runtime);
    // The park editor's Xbox-only themes, Desert and Highway, after the
    // stock ones (ZX_DM2_XBOX_THEMES=1, made at build time in
    // ZX_DM2_XBOX_LEVELS_DIR/themes; dave_mirra_xboxthemes.cpp).
    bool installXboxThemes(PS2Runtime &runtime);
    // The Retro movie as the movie gallery's last line (ZX_DM2_RETRO_MOVIE=1,
    // made at build time in ZX_DM2_XBOX_LEVELS_DIR/movies; dave_mirra_gallery.cpp).
    bool installRetroMovie(PS2Runtime &runtime);
    // The attract demo at the main menu, as on the Xbox and GameCube
    // (ZX_DM2_ATTRACT_DEMO=1; dave_mirra_demo.cpp).
    bool installAttractDemo(PS2Runtime &runtime);

    // Level meshes drawn natively: from the draw records by default, with
    // the packet override on the GPU renderer for what they do not reach
    // (dave_mirra_native.cpp; the render path: ZX_RENDER_PATH, or
    // ZX_DM2_NATIVE=1 / 0).
    bool installNative(PS2Runtime &runtime);

    // The Xbox levels' career (ProQuest goals, gaps, rewards) in Woodward
    // Camp's slot (dave_mirra_xcareer.cpp); after installXboxLevels.
    bool installXboxCareer(PS2Runtime &runtime);

    // Every level open in the level select and every trick open (developers:
    // ZX_DEV_UNLOCK_ALL=1, or ZX_DM2_OPEN_LEVELS=1 / ZX_DM2_OPEN_TRICKS=1
    // for one of them; dave_mirra_devlevels.cpp).
    bool installOpenLevels(PS2Runtime &runtime);

    // The game save as a native file, no card dialogs; a park from a host
    // file for the park loads (ZX_DM2_PARK_FILE; dave_mirra_saves.cpp).
    bool installNativeSaves(PS2Runtime &runtime);

    // From boot straight into a FREERIDE level (ZX_DEV_START_LEVEL:
    // developers, the collection's Custom Freeplay; dave_mirra_devstart.cpp).
    void installDevStart(PS2Runtime &runtime);

    // The game's "dvd0" file device on host files (unless [host] disc = ps2).
    bool installNativeDisc(PS2Runtime &runtime);

    // Native movies (ps2Movie.c on the host: picture through the MPEG HLE,
    // sound without SDRDRV); true when installed (dave_mirra_movies.cpp).
    bool installNativeMovies(PS2Runtime &runtime);

    // The GS texture memory audit (ZX_GSMEM_AUDIT=1, developers;
    // runtime/ps2_gs_texture_audit.h; dave_mirra_gsmem.cpp).
    void installGsTextureAudit(PS2Runtime &runtime);

    // The ZAudio sound driver on the host; true when installed (ZAUDIO.IRX
    // is then not needed; dave_mirra_sound.cpp).
    bool installNativeSound(PS2Runtime &runtime);

    // The game's music and effects volumes as settings rows, with the app's
    // settings and the native sound (dave_mirra_volumes.cpp).
    void installGameVolumes(PS2Runtime &runtime);
}

namespace dave_mirra::saves
{
    // The native saves' folder and the save profile's files in it
    // (dave_mirra_saves.cpp): <saves>/<profile><suffix>.
    std::filesystem::path savesRoot();
    std::filesystem::path profileFile(const std::string &suffix);
    // The Xbox levels' career beside the profile's game save
    // (dave_mirra_xcareer.cpp).
    constexpr const char *kXboxLevelsSuffix = ".XboxLevels.career";
    // The saves of before profiles copied to the Career profile's, once
    // (the originals kept).
    void adoptUnprofiledSaves();
}

namespace dave_mirra::cheats
{
    // DM2's unlock cheats (bmxCheckCheat's general codes, game/dm2/menu/
    // cheats.cpp, and the Cheats page's Unlock Everything) open what they open
    // for the characters 0 .. 16. The extras that add riders or levels are
    // opened by them as the game's own are:
    // - with Create Player on, its created riders 17 .. 20 too
    //   (dave_mirra_creator.cpp): ridersCovered() is 21, else 17;
    // - the Xbox levels open for those riders when DM2's eight do
    //   (dave_mirra_xcareer.cpp): openXboxLevels, nothing without them.
    // Nothing here runs unless a cheat does.
    constexpr uint32_t kGameCheatRiders = 17;
    constexpr uint32_t kRiderRecords = 21; // the 17 and the created riders' four
    uint32_t ridersCovered();
    void openXboxLevels(uint8_t *rdram);
    // The created riders' part (dave_mirra_creator.cpp), apart for the tests
    // (tests/ ps2_unlock_tests.cpp). `records`: the 21 rider records as the
    // game keeps them (RiderCharacter, 0x3C8 bytes each) after a cheat ran;
    // `before`: the same bytes before it. What it opened for every one of
    // the game's 17 riders (their levels, all bikes, both looks, sponsors,
    // tricks), the created riders 17 .. 20 get. How many things were given.
    size_t shareWithCreatedRiders(uint8_t *records, const uint8_t *before);
    // UILIB.ZAL's MainMenu script: Back from the level select a created
    // rider's RIDE reached goes to Create Rider; false if not found.
    bool patchLevelSelectBack(std::string &text);
}

namespace dave_mirra::xboxlevel
{
    // The level select's chosen Xbox level, whose files Woodward Camp's
    // slot is served: its folder ("VENICE"), else empty.
    std::string chosenLevel();
    // The Xbox levels in the level select, in their order (values 9, 10).
    std::vector<std::string> levels();
    // Guest memory above 32 MB (the extended region, added once).
    uint32_t allocateExtended(PS2Runtime &runtime, uint32_t size);
}

// The menu edits, apart for the tests (tests/ ps2_exit_menu_tests.cpp).
namespace dave_mirra::exit_menu
{
    // UILIB.ZAL's MainMenu script with Exit to Collection in the main menu;
    // false (and the text as it was) if the script is not the expected one.
    bool patchMainMenu(std::string &text);
}

namespace dave_mirra::restart
{
    struct PauseLines
    {
        int added = 0;      // lines added
        int photoLine = -1; // the Photo Mode line's index, -1: none
        int exitLine = -1;  // the Exit to Collection line's index, -1: none
        int settingsLine = -1; // the Settings line's index, -1: none
    };
    // The pause menu's Items text (lines between \n escapes) with the new
    // lines as one block after the game's last line, End Run, in this order:
    // RESTART (restart: when ids, the lines' action ids, ends with End Run's;
    // its own id follows), PHOTO MODE (photo), SETTINGS (settings) and EXIT
    // TO COLLECTION (exit); the last three have no id (their indexes are kept).
    PauseLines addPauseLines(std::string &items, uint8_t *ids, bool restart, bool exit, bool photo = false,
                             bool settings = false);
    // The pause menu's hooks (the Items text, the chosen line), once.
    bool installPauseHooks(PS2Runtime &runtime);
    // The Photo Mode line wanted in the pause menu (dave_mirra_photo.cpp).
    void addPhotoLine(PS2Runtime &runtime);
    // The Settings line wanted in the pause menu (dave_mirra_settings.cpp).
    void addSettingsLine(PS2Runtime &runtime);
    // The Restart key was pressed in a level: the turn again at the next
    // play frame, if one is on.
    void requestKey();
}

// The Settings page (dave_mirra_settings.cpp): the pause menu's Settings
// line chosen, the page opened in its place (the pause menu's widget tree's
// root).
namespace dave_mirra::settings_menu
{
    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t root);
}

// Photo mode (dave_mirra_photo.cpp): the game paused, a free camera, no HUD
// or menus, picture filters, screenshots through the Screenshot key.
// Installed with the gameplay pages (the app's settings).
namespace dave_mirra::photo
{
    bool install(PS2Runtime &runtime);
    // No widget is drawn (zgui's widget draw, wrapped in
    // dave_mirra_gameplay.cpp): photo mode is on.
    bool hidesUi();
    // The pause menu's Photo Mode line was chosen: photo mode at the next
    // frame, back to the pause menu after.
    void requestFromPause();
}

namespace panel
{
    struct Snapshot;
}

// The Thor's bottom screen (dave_mirra_panel.cpp): DM2's state into the
// panel's snapshot, and that capture installed in place of Aggressive
// Inline's.
namespace dave_mirra::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
    void install();
    // The Xbox levels' folders by value from 9 (dave_mirra_xboxlevel.cpp).
    void setXboxLevels(const char *const *folders, size_t count);
}
