#pragma once

// BMX XXX (SLUS-20415) game modules. Each install function returns true
// when its hooks are in place; bmx_xxx.cpp calls them only for that ELF.

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

namespace bmx_xxx
{
    // The game's "dvd0" file device on host files, with the Mods folder
    // over them (unless [host] disc = ps2; zge/host/disc_device.cpp, zge_disc.cpp).
    bool installNativeDisc(PS2Runtime &runtime);

    // Movies decoded on the host (unless PS2X_MOVIES=hle;
    // bmx_xxx_native.cpp, zge_movies.cpp).
    bool installNativeMovies(PS2Runtime &runtime);

    // Native meshes (render_path = records only: BMX XXX has no packet
    // observer): the readable class renderers draw from their draw records
    // (zge/host/class_draws.cpp, game/host/native_draws.h). True when on.
    bool installNativeDraw(PS2Runtime &runtime);

    // The ZAudio sound driver on the host (unless PS2X_SOUND=spu2;
    // bmx_xxx_sound.cpp).
    bool installNativeSound(PS2Runtime &runtime);

    // The gameplay pages (Camera) and keys (Hide HUD), Aggressive Inline's
    // at BMX XXX's code; with the app's settings (PS2X_GAME_SETTINGS_MENUS=1;
    // bmx_xxx_gameplay.cpp).
    void installGameplay(PS2Runtime &runtime);
    // The Restart key's hook (with the gameplay keys; bmx_xxx_restart.cpp).
    bool installRestartKey(PS2Runtime &runtime);

    // From boot straight into a HARDCORE TOUR level (PS2X_DEV_START_LEVEL:
    // developers, the collection's Custom Freeplay; bmx_xxx_devstart.cpp).
    void installDevStart(PS2Runtime &runtime);
    // The levels' own NPC riders left out (PS2X_BMX_LEVEL_NPCS=0: a Custom
    // Freeplay map that does not keep its slot's; bmx_xxx_npcs.cpp).
    void installLevelNpcs(PS2Runtime &runtime);

    // The view settings: motion blur, widescreen and draw distance
    // (bmx_xxx_view.cpp, zge_view.cpp).
    bool installView(PS2Runtime &runtime);

    // Native save files in place of the memory card (unless PS2X_SAVES=card;
    // bmx_xxx_saves.cpp, zge_saves.cpp).
    bool installNativeSaves(PS2Runtime &runtime);

    // Mouse-look (controls.ini [mouse] mode = look; unless PS2X_MOUSELOOK=0;
    // bmx_xxx_mouselook.cpp).
    bool installMouseLook(PS2Runtime &runtime);

    // The developer menu: F1 turns the game's debug display on and off
    // (PS2X_DEV_MENU=1; bmx_xxx_devtools.cpp).
    void installDevTools(PS2Runtime &runtime);

    // Exit to Collection in the main menu and the pause menu, in the
    // collection only, and the pause menu's RESTART line (unless
    // PS2X_RESTART=0; bmx_xxx_exit.cpp, zge_exit.cpp); first the main menu's
    // PARK EDITOR line, the game's own park editor menu
    // (PS2X_BMX_PARK_EDITOR=1, installParkEditorLine).
    bool installExitToCollection(PS2Runtime &runtime);
    // The engine's table of script functions in BMX XXX (bmx_xxx_exit.cpp):
    // the host's go in after the menus' bmxuiHandler.
    const zge_host::ScriptFunctions &scriptFunctions();
    // The Settings page in the front end's Options (bmx_xxx_settings.cpp).
    void installSettingsMenus(PS2Runtime &runtime);
    // Button prompts for the device in use (bmx_xxx_prompts.cpp).
    void installPrompts(PS2Runtime &runtime);
    bool installParkEditorLine(PS2Runtime &runtime);
    // The pause and game over lines the game's builders leave out, with
    // their own handlers (PS2X_BMX_MENU_LINES=1, bmx_xxx_exit.cpp).
    bool installMenuLines(PS2Runtime &runtime);
    // Every multiplayer mode whose rules work in SELECT GAME
    // (PS2X_BMX_ALL_MP_MODES=1, bmx_xxx_modes.cpp).
    bool installAllMultiplayerModes(PS2Runtime &runtime);
    // The Xbox and GameCube releases' content, each behind its setting
    // (PS2X_BMX_RELEASES_DIR, made at build time; bmx_xxx_releases.cpp):
    // their challenge scripts, the fuller French, German and Spanish, the
    // level select's preview movies, the Z-Axis logo movie in the gallery,
    // the intro movie at boot.
    void installReleases(PS2Runtime &runtime);
    // The GS texture memory audit (PS2X_GSMEM_AUDIT=1, developers; off by
    // default, nothing hooked): the texture cache's allocations per level,
    // the peak block, overruns (runtime/ps2_gs_texture_audit.h;
    // bmx_xxx_gsmem.cpp).
    void installGsTextureAudit(PS2Runtime &runtime);
    // A park's file written (native saves): beside it, whether it holds the
    // other releases' editor pieces (bmx_xxx_releases.cpp).
    void releasesParkSaved(const std::filesystem::path &file);
}

// The menu edits, apart for the tests (tests/ ps2_exit_menu_tests.cpp).
namespace bmx_xxx::exit_menu
{
    // UILIB.ZAL's UI_menu script with the main menu's select and change
    // scripts sending EXIT TO COLLECTION to pcExitToCollection; false (and
    // the text as it was) if the script is not the expected one.
    bool patchMainMenu(std::string &text);
    // EXIT TO COLLECTION added to a menu's Items text (lines between \n
    // escapes, NUL-terminated, in a buffer of size bytes): its line index,
    // or -1 if it does not fit.
    int addExitLine(char *text, size_t size);
    struct PauseLines
    {
        int added = 0;        // lines added
        int restartLine = -1; // the RESTART line's index, -1: none
        int exitLine = -1;    // the Exit to Collection line's index, -1: none
        int photoLine = -1;   // the Photo Mode line's index, -1: none
        int settingsLine = -1; // the Settings line's index, -1: none
    };
    // The pause menu's Items text (as addExitLine's) with the new lines as one
    // block after the game's last line, Exit Game, in this order: RESTART
    // (restart), PHOTO MODE (photo), SETTINGS (settings) and EXIT TO
    // COLLECTION (exit). None has a choice of its own: the game's choices
    // stay as they are and the new lines are known by their indexes.
    struct MainMenuLines
    {
        int added = 0;           // lines added
        int parkEditorLine = -1; // the PARK EDITOR line's index, -1: none
        int exitLine = -1;       // the Exit to Collection line's index, -1: none
    };
    // The main menu's Items text (as addExitLine's) with the PARK EDITOR line
    // (parkEditorText, the game's text for it; null: none) after MULTIPLAYER
    // (choices, the lines' choices, moved to match) and EXIT TO COLLECTION
    // last (exit).
    MainMenuLines addMainMenuLines(char *text, size_t size, uint8_t *choices, const char *parkEditorText, bool exit);
    PauseLines addPauseLines(char *text, size_t size, bool restart, bool exit, bool photo = false, bool settings = false);
    // A line to put in a menu: its choice and its text.
    struct MenuLine
    {
        uint8_t choice;
        std::string text;
    };
    // A menu's Items text (count lines between \n escapes, choices their
    // choices, room for that many) with each of added that is not there yet
    // put in before the first line whose choice comes later in order (the
    // menu's order of choices; one not in it last), the choices moved to
    // match: the lines put in, 0 (and both as they were) if none, or if the
    // text has not count lines.
    int insertMenuLines(std::string &items, uint8_t *choices, int count, size_t room, const std::vector<MenuLine> &added,
                        const uint8_t *order, size_t orderCount);
    // The Photo Mode line wanted in the pause menu (bmx_xxx_photo.cpp).
    void addPhotoLine(PS2Runtime &runtime);
    // The Settings line wanted in the pause menu (bmx_xxx_settings.cpp).
    void addSettingsLine(PS2Runtime &runtime);
}

// The Settings page (bmx_xxx_settings.cpp): the pause menu's SETTINGS line
// chosen, the page shown in the pause menu's place (overlay: UI_overlay's
// node).
namespace bmx_xxx::settings_menu
{
    void openFromPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t overlay);
}

namespace bmx_xxx::photo
{
    // Photo mode (the photo_mode key; bmx_xxx_photo.cpp, zge_photo.cpp):
    // installed with the gameplay pages. While it is on no widget is drawn.
    bool install(PS2Runtime &runtime);
    bool hidesUi();
    // The pause menu's Photo Mode line was chosen: photo mode at the next
    // screen frame, back to the pause menu after.
    void requestFromPause();
}

namespace bmx_xxx::restart
{
    // The Restart key was pressed in a level: the turn again at the next
    // play frame, if one runs.
    void requestKey();
    // The app's Settings... (zge_settings_open.h) with this game's hooks:
    // once the pause menu has its SETTINGS line (bmx_xxx_exit.cpp).
    void installSettingsOpen(PS2Runtime &runtime);
}

namespace panel
{
    struct Snapshot;
}

// The Thor's bottom screen (bmx_xxx_panel.cpp): BMX XXX's state into the
// panel's snapshot, and that capture installed in place of Aggressive
// Inline's.
namespace bmx_xxx::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
    void install();
}
