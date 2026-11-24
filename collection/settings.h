#pragma once

// The app's own files: where they live (AppPaths), its settings.ini (the
// app's rows and each game's options, GameSettingsProfile), a collection's
// files (the root's and each game's), and what a game's boot is given
// (gameBootConfig). The bundled disc's check is disc_check.h, a PCSX2 save's
// import card_import.h. No window library or platform UI in here.

#include "gs/gs_quality.h"
#include "runtime/boot_config.h"
#include "runtime/games.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    struct GameControls;
}

namespace zx::app
{
    using GetEnv = std::function<const char *(const char *name)>;

    struct AppPaths
    {
        std::filesystem::path supportDir;   // settings, controls, cards
        std::filesystem::path settingsFile; // settings.ini
        std::filesystem::path controlsFile; // controls.ini (host input layer)
        std::filesystem::path memcardSlot1; // HLE card folder, port 0
        std::filesystem::path memcardSlot2; // sibling of slot1 (the runtime derives it)
        std::filesystem::path savesDir;     // native save files (games without a memory card)
        std::filesystem::path hostRoot;     // host0:
        std::filesystem::path logDir;
    };

    // macOS: ~/Library/Application Support/<appName> and ~/Library/Logs/<appName>;
    // Windows: %APPDATA%\<appName> (logs in Logs\ there); Linux: XDG data home.
    // ZX_APP_SUPPORT_DIR replaces the support folder (logs then go to its
    // Logs subfolder, for automated runs that must not touch the real one);
    // ZX_APP_LOG_DIR replaces the log folder.
    AppPaths resolveAppPaths(const std::string &appName, const GetEnv &getEnv);

    enum class Scaling
    {
        Smooth,  // largest 4:3 fit, bilinear
        Integer, // whole multiples of 640x480, nearest
    };

    enum class FullscreenMode
    {
        Native,     // the system's own full screen (its own Space on macOS)
        Borderless, // a borderless window covering the display
    };

    struct Settings
    {
        bool fullscreen = false;
        FullscreenMode fullscreenMode = FullscreenMode::Native;
        int windowWidth = 1024;
        int windowHeight = 768;
        Scaling scaling = Scaling::Smooth;
        bool widescreen = false; // 16:9 where the game supports it
        int wideAspect = 0;      // widescreen aspect: kWideAspects index (16:9, 16:10, 21:9, 32:9, the window's)
        bool vsync = true;
        bool variableRefresh = false; // each game frame shown when ready (a G-Sync, FreeSync or adaptive-sync display)
        bool frameInterpolation = false; // pictures between the game's frames on a faster display (gs_interpolate.h)
        bool showFps = false;    // frame rate counter in a corner of the window
        bool gpuRenderer = true;    // hardware GS renderer (false: the CPU one)
#if defined(__ANDROID__)
        int internalResolution = 2; // its resolution multiplier, 1-4 (3 with full height is 2 Mpx: over a phone GPU, 24 frames/s on a Thor)
#else
        int internalResolution = 3; // its resolution multiplier, 1-4
#endif
        bool fullHeight = true;     // GPU renderer: field-height (224-line) frames at twice the vertical resolution
        bool nativeMeshes = true;   // GPU renderer: level meshes drawn natively (false: strict VIF/VU1/GS path)
        bool recordsMeshes = false; // with nativeMeshes: drawn from the draw records, no VU1 work in the packets (render_path = records)
        bool hostDisc = true;       // the game's files read on the host (false: its CD/DVD client over the emulated IOP)
        // GPU renderer picture quality (gs/gs_quality.h), by zx::gs_quality::Option.
        std::array<int, zx::gs_quality::kOptionCount> quality = zx::gs_quality::defaults();
        int motionBlur = 2;         // the game's previous-frame blend: 0 off, 1 low, 2 the game's
        // Far clip and fog, a game's own ([gameplay] draw_distance): 0 the game's, 1 far, 2 farthest; -1
        // not set: the game's default (drawDistanceOf). legacyDrawDistance: an earlier version's app-wide
        // [graphics] draw_distance, read and never written (drawDistanceOf).
        int drawDistance = -1;
        int legacyDrawDistance = -1;
        bool xboxTextures = true;   // the Xbox release's textures, when the app was built with them (GPU renderer)
        bool xboxMovies = true;     // the Xbox release's movies, when the app was built with them
        bool xboxSkaters = true;    // the Xbox release's more detailed skaters, when the app was built with them
        bool textureDump = false;   // texture packs' dump mode into <support>/Texture Dumps (runtime/texture_pack.h)
        bool pauseOnFocusLoss = true;
        bool mapWindow = false;      // the bottom screen (the map, goals and tricks, panel/panel.h) in a second window
        int mapWindowX = -1;         // where that window was last, in window coordinates (-1, -1: the system places it)
        int mapWindowY = -1;
        int mapWindowWidth = 620;    // its size last (half of the screen's 1240x1080)
        int mapWindowHeight = 540;
        bool mapWindowOnTop = false; // that window kept above the others (Always on Top)
        // Clean Top Screen: the game's HUD pieces a bottom screen shows (the clock, the trick line, the goal) left
        // off the top screen while one is up, drawing only (runtime/panel_snapshot.h). Off by default.
        bool cleanTopScreen = false;
        int volume = 100;               // master volume, percent (0-100)
        bool muteOnFocusLoss = false;   // silent while the window is in the background
        bool shuffleMusic = false;      // the soundtrack in a random order (engine/overrides/zge_soundtrack.h)
        int movieVolume = 100;          // movie sound, percent (0-100)
        bool skipIntroMovies = false;   // start at the title screen
        int language = 0;               // kLanguages index: English, or French/German/Spanish from the Xbox release
        int frenchVoices = -1;          // the Xbox release's French speech: -1 with French, 0 off, 1 on
        bool developerMenu = false;     // the game's hidden debug menu (F1 in a level), tuning-reference.ini
        bool zoo = true;                // the cut Zoo level in free skate (game override; [extras])
        int zooUnlock = 1;              // kZooUnlocks index: by the Museum's eight challenges (the Xbox's rule), or always, the default ([extras])
        bool ape = true;                // the Xbox release's Ape skater (game override; [extras])
        bool estate = false;            // TUT/Estate, the GameCube disc's earlier Tutorial, in free skate, when the app was built with that disc ([extras])
        bool dm2XboxLevels = true;      // Dave Mirra BMX 2: its Xbox release's Venice and Greenville, with their ProQuest career, when the app was built with that disc ([extras]); on by default: each game is the superset of its releases
        bool xboxFreeSkate = true;      // the Xbox release's Boardwalk and Cannery free skate scripts ([extras])
        bool dm2CreatePlayer = false;   // Dave Mirra BMX 2: its cut Create Player menu, with created riders that ride and save ([extras]); off by default: the menus as the game shipped
        // Dave Mirra BMX 2: its Xbox release's content, when the app was built with that disc ([extras]); each off by
        // default: the game as the PS2 release made it.
        bool dm2XboxThemes = false;     // the park editor's Desert and Highway themes, which no release lists
        bool dm2RetroMovie = false;     // the Retro movie in the movie gallery
        bool dm2AttractDemo = false;    // the attract demo after the title screen idles (the game's own replays)
        bool bmxParkEditor = false;     // BMX XXX: its park editor's menu as the main menu's PARK EDITOR line ([extras]); off by default: the main menu as the game made it
        // BMX XXX: its Xbox and GameCube releases' content, when the app was built with those discs ([extras]); each
        // off by default: the game as the PS2 release made it.
        bool bmxXboxChallenges = false; // the four levels' challenge scripts of the other releases: success and failure lines, fixes
        bool bmxEditorPieces = false;   // the park editor's Pool and Receptacle (the GameCube's and Xbox's piece list)
        bool bmxLevelPreviews = false;  // the level select's preview movies (the Xbox's)
        bool bmxLogoMovie = false;      // the Z-Axis logo movie in the movie gallery
        bool bmxIntroMovie = false;     // the intro movie at boot, as the GameCube plays it
        bool bmxGcLevels = false;       // the GameCube's Secret Level and three test levels in the level select
        bool bmxLogoSplash = false;     // the Z-Axis logo at boot, as the GameCube plays it
        bool bmxTrailers = false;       // the Xbox's Turok and Vexx trailers in the Competitive Edge movies
        bool bmxCarTextures = false;    // create-a-rider's Xbox and GameCube textures and parts
        bool bmxGreenSkin = false;      // create-a-rider's green skins and faces (the other releases' Green Skin mode)
        bool bmxTopless = false;        // create-a-rider's topless top (the other releases' Topless Girl mode)
        bool bmxXboxSongs = false;      // the Xbox's eight songs no release's soundtrack names, as its tracks 21 to 28
        bool bmxMenuLines = false;      // BMX XXX: the pause and game over lines its menus leave out ([extras]); off by default: the menus as the game made them
        bool bmxAllMpModes = false;     // BMX XXX: every multiplayer mode whose rules work in SELECT GAME ([extras]); off by default: the game's three
        int tuning = 0;                 // kTunings index: the PS2 release's gameplay balance, or the Xbox's ([gameplay])
        int careerScripts = 1;          // kTunings index: the PS2 release's career scripts, or the Xbox's bug-fixed ones ([gameplay])
        bool logFile = true;
        // Record each game's play to <support>/Recordings (runtime/play_recording.h): inputs, telemetry, the
        // saves at boot; a replay runs it again here. Off by default.
        bool recordPlay = false;
        // Practice (collection/practice.h): a restart point and a timer in the games that have one. Off by default.
        bool practice = false;
        // Custom Freeplay (collection/maps.h): the menu's fourth tile, a player from any game on a custom map from the
        // Mods folders. Off by default: the menu as it was, and no boot is given a map.
        bool customFreeplay = false;
        // The collection's menu ([menu], the root file's): the Z-Axis logo at launch (Logo, or Off), the shown game's
        // menu music under it, and whose logo plays next (the games' logos in turn, collection/front_art.h introFor).
        bool introLogo = true;
        bool menuMusic = true;
        uint32_t introTurn = 0;
        bool pcsx2ImportOffered = false;
        std::string saveProfile = "Career"; // native saves: <savesDir>/<profile>.sav
        // The game's mods switched off ([mods] <key> = off; collection/mods.h), by their keys (a manifest's
        // id, else the folder's name), in name order. A mod not named here is on.
        std::vector<std::string> modsOff;
        // The collection's order of its mods ([mod order], the root file's; collection/mods.h orderMods): their
        // order keys, first read first. Empty: each game's own folder's mods, then the collection's, by name.
        std::vector<std::string> modOrder;
    };
    // Switches the mod named `name` on or off in settings.modsOff.
    void setModOn(Settings &settings, const std::string &name, bool on);

    // settings.ini names of Settings::wideAspect, motionBlur and drawDistance.
    inline constexpr const char *kWideAspects[] = {"16:9", "16:10", "21:9", "32:9", "window"};
    inline constexpr const char *kMotionBlurLevels[] = {"off", "low", "original"};
    inline constexpr const char *kDrawDistances[] = {"original", "far", "farthest"};
    // settings.ini names of Settings::language (also ZX_LANGUAGE's values;
    // "us" and "uk" of earlier versions read as English).
    inline constexpr const char *kLanguages[] = {"english", "french", "german", "spanish"};
    // settings.ini names of Settings::tuning (also ZX_TUNING's values).
    inline constexpr const char *kTunings[] = {"ps2", "xbox"};
    // settings.ini names of Settings::zooUnlock (also ZX_ZOO_UNLOCK's values).
    inline constexpr const char *kZooUnlocks[] = {"museum", "always"};

    // A game's own options: the settings.ini keys and Settings rows that only
    // some games act on. Each game lists the ones it has (GameSettingsProfile), from
    // any of its releases (PS2, Xbox, GameCube): an option another release
    // brings goes into that game's list when the collection has it for the
    // game, and gets its key here.
    enum class GameOption : uint8_t
    {
        // In the game's own settings.ini.
        Language,        // [general] language: the Xbox release's other languages
        FrenchVoices,    // [general] french_voices: the Xbox release's French speech
        DeveloperMenu,   // [general] developer_menu: the game's hidden debug menu (F1)
        Tuning,          // [gameplay] tuning: the PS2 or the Xbox release's balance
        CareerScripts,   // [gameplay] career_scripts: the PS2 or the Xbox release's level scripts
        Zoo,             // [extras] zoo: AI's cut Zoo level
        ZooUnlock,       // [extras] zoo_unlock
        Estate,          // [extras] estate: the GameCube disc's TUT/Estate
        Dm2XboxLevels,   // [extras] dm2_xbox_levels: DM2's Xbox-only Venice and Greenville
        Ape,             // [extras] ape: AI's Xbox secret skater
        XboxFreeSkate,   // [extras] xbox_free_skate: AI's Xbox free skate scripts
        Dm2CreatePlayer, // [extras] create_player: DM2's cut Create Player menu
        Dm2XboxThemes,   // [extras] xbox_themes: DM2's park editor Desert and Highway (the Xbox disc's)
        Dm2RetroMovie,   // [extras] retro_movie: DM2's Retro movie in the gallery (the Xbox disc's)
        Dm2AttractDemo,  // [extras] attract_demo: DM2's attract demo at the title screen (the Xbox's and GameCube's)
        BmxParkEditor,   // [extras] park_editor: BMX XXX's park editor in its main menu
        BmxXboxChallenges, // [extras] xbox_challenges: BMX XXX's Xbox and GameCube challenge scripts
        BmxEditorPieces,   // [extras] editor_pieces: BMX XXX's park editor Pool and Receptacle
        BmxLevelPreviews,  // [extras] level_previews: BMX XXX's Xbox level-select preview movies
        BmxLogoMovie,      // [extras] logo_movie: BMX XXX's Z-Axis logo movie in the gallery
        BmxIntroMovie,     // [extras] intro_movie: BMX XXX's intro movie at boot (the GameCube's)
        BmxGcLevels,       // [extras] gc_levels: BMX XXX's GameCube Secret Level and test levels
        BmxMenuLines,      // [extras] menu_lines: BMX XXX's left-out pause and game over lines
        BmxAllMpModes,     // [extras] all_mp_modes: BMX XXX's multiplayer modes SELECT GAME leaves out
        BmxLogoSplash,     // [extras] logo_splash: BMX XXX's Z-Axis logo at boot (the GameCube's)
        BmxTrailers,       // [extras] trailers: BMX XXX's Xbox Turok and Vexx trailers
        BmxCarTextures,    // [extras] car_textures: BMX XXX's create-a-rider textures and parts of the other releases
        BmxGreenSkin,      // [extras] green_skin: BMX XXX's Green Skin mode (the other releases')
        BmxTopless,        // [extras] topless_girl: BMX XXX's Topless Girl mode (the other releases')
        BmxXboxSongs,      // [extras] xbox_songs: BMX XXX's Xbox songs no soundtrack names
        SaveProfile,     // [saves] profile: several careers side by side
        Pcsx2Import,     // [memcard] pcsx2_import_offered: the PCSX2 save import offer
        // In the game's gameplay.ini (ZX_GAMEPLAY_INI), written by its
        // override: the Camera, Assists and Cheats pages.
        GameplayPages,
        // App-wide values (every game's settings.ini at the collection's
        // root) that the game acts on: their rows and menu items show only
        // for such a game.
        SkipIntroMovies, // [general] skip_intro_movies
        Widescreen,      // [display] widescreen, aspect
        MotionBlur,      // [graphics] motion_blur
        DrawDistance,    // [graphics] draw_distance
        MovieVolume,     // [audio] movie_volume
        Count
    };

    // What a game is to the settings: the game (runtime/games.h: its id,
    // boot ELF, title, what its logic follows), its file's header, and its
    // options.
    struct GameSettingsProfile : zx::games::Game
    {
        std::vector<GameOption> options;
        bool has(GameOption option) const;
    };
    // The game's draw distance (0 .. 2): its own setting; else an earlier
    // version's app-wide value the user picked (not Far, that version's
    // default); else the game's default: Original where the draw distance
    // changes the game, Far elsewhere.
    int drawDistanceOf(const Settings &settings, const GameSettingsProfile &game);
    // By collection id, else by boot ELF (the single-game app); Aggressive
    // Inline's for neither (the baseline the app was first made for).
    const GameSettingsProfile &gameSettingsProfile(std::string_view idOrElf);
    // Every option: what formatSettings(settings) writes.
    const GameSettingsProfile &everyOptionProfile();

    // The folders a game's boot is given besides its AppPaths.
    struct BootFolders
    {
        std::filesystem::path resources;   // the game's bundled files (Resources, or Resources/games/<id>)
        // The extras made from the player's other releases' discs
        // (<support>/games/<id>/extras, collection/extras/setup.h): each of
        // its folders is read in place of the bundled one of that name.
        std::filesystem::path extras;
        std::filesystem::path screenshots; // where the screenshot key saves
        std::string gamepadMappings;       // the collection's gamecontrollerdb.txt ("": the game's own folder)
        // Custom Freeplay: the map this boot plays (collection/maps.h), none by default. The game is started into
        // the slot's free play (mapSlot); the map's folder is read last of the mods, or, when the map was made in
        // another game, not at all: its files converted are the boot's memory folder (kConvertedMapFolder).
        std::filesystem::path customMap;
        std::string customMapSlot;         // the slot this game lends it (the level folder)
        bool customMapSlotNpcs = false;    // it keeps the slot level's own NPCs (map.ini npcs = slot)
        bool customMapConverted = false;   // its files are the memory folder's, converted for this game
        std::string customMapPark;         // DM2's park slot: the saved park's file in the map's folder (map.ini park)
        // The collection's Mods folder (<support>/Mods, collection/mods.h), "" in the single-game app (whose
        // <support>/Mods is the game's own).
        std::filesystem::path sharedMods;
    };
    // The folders of the other releases' content a boot is given, by the
    // boot name the game reads each by (ZX_XBOX_DIR: xbox, ZX_ESTATE_DIR:
    // estate, ...): the one the setup made from the player's discs
    // (folders.extras), else the one bundled with the app (folders.resources);
    // only those there are.
    std::vector<std::pair<std::string, std::filesystem::path>> releaseFolders(const BootFolders &folders);
    // What the app gives a game when it boots it: the game's settings (the
    // options its profile has), its folders and files, the renderer settings
    // (over ZX_GS_SCALE and ZX_GS_FULL_HEIGHT unless the environment
    // sets ZX_GS_BACKEND, as the app always applied them). The
    // environment still wins over the rest (runtime/boot_config.h).
    zx::BootConfig gameBootConfig(const std::string &gameId, const Settings &settings, const AppPaths &paths,
                                    const GameSettingsProfile &profile, const BootFolders &folders,
                                    const GetEnv &getEnv);
    // The renderer settings as the GS renderer reads them: ZX_GS_BACKEND,
    // ZX_GS_SCALE, ZX_GS_FULL_HEIGHT.
    std::vector<std::pair<std::string, std::string>> rendererValues(const Settings &settings);

    // Which part of the settings a settings.ini holds.
    enum class SettingsScope
    {
        All,  // the single-game app: one file with everything
        App,  // a collection's root: the app's own ([display], [graphics], [audio], [general]) for every game
        Game, // a collection's games/<id>: that game's own options
    };
    // Whether a settings.ini key belongs in a collection's root file (the
    // app's, the same in every game) rather than the game's own.
    bool appWideKey(std::string_view section, std::string_view key);
    // The version that first read a key of a collection's root settings.ini,
    // "" for one every release reads (the first, 1.1.0, did). A key added to
    // the root (formatSettings, SettingsScope::App) gets its version in the
    // table (settings.cpp, kAddedRootSettings): before the app restarts into
    // an older version, the keys that version does not read are kept aside
    // for it (collection/updater.h, setAsideNewerSettings).
    std::string rootSettingSince(std::string_view section, std::string_view key);
    // Whether this version writes the key in a collection's root settings.ini.
    bool rootSettingKnown(std::string_view section, std::string_view key);

    // INI with [display], [graphics], [audio], [general], [gameplay], [extras], [saves] and [memcard] sections;
    // unknown keys and bad values keep their defaults and are reported in
    // warnings. Settings of earlier versions ([disc] image) are ignored.
    // A key of an option the game does not have is read like the others
    // (an older file's) and not written back.
    Settings parseSettings(std::string_view text, std::vector<std::string> *warnings = nullptr);
    // The same on top of `settings` (keys the text lacks keep their value):
    // a collection reads the game's file, then the root's.
    void parseSettingsInto(std::string_view text, Settings &settings, std::vector<std::string> *warnings = nullptr);
    // "section.key" of every key = value line, in order.
    std::vector<std::string> settingsKeys(std::string_view text);
    // Every key of every game (tests, older callers).
    std::string formatSettings(const Settings &settings);
    // The file of one game: its header, the app's sections (scope All and
    // App) and the game's options it has (All and Game).
    std::string formatSettings(const Settings &settings, const GameSettingsProfile &game, SettingsScope scope);
    bool loadSettings(const std::filesystem::path &file, Settings &settings, std::vector<std::string> *warnings = nullptr);
    // Written to a temporary file and renamed over the old one.
    bool saveSettings(const std::filesystem::path &file, const Settings &settings);
    bool writeTextFile(const std::filesystem::path &file, const std::string &text);
    bool readTextFile(const std::filesystem::path &file, std::string &text);

    // A collection's files: <root>/settings.ini (the app's settings, every
    // game), <root>/gamecontrollerdb.txt (the user's gamepad mappings) and
    // per game <root>/games/<id>/settings.ini (its options) and
    // controls.ini (its keys and buttons, with the game's own defaults).
    // An earlier version kept one controls.ini at the root for every game
    // and the game options of the first game ([general] language,
    // french_voices, developer_menu) in the root's settings.ini. Once, at
    // the first start of this version (whichever game), they move:
    //  - the root controls.ini becomes keeperId's (Aggressive Inline's: its
    //    actions were every game's defaults), a controls.ini the keeper had
    //    kept aside as controls.ini.bak;
    //  - every other game starts from its own defaults: a controls.ini of
    //    one (copied from its single-game app, unused while the root's was
    //    shared) is put aside as controls.ini.bak;
    //  - the root settings.ini's game options go to the keeper's
    //    settings.ini (its own values win) and the root keeps the app's;
    //  - an app-wide [graphics] draw_distance (the draw distance became each
    //    game's own) goes to every game with the option, as its [gameplay]
    //    draw_distance, when the user had picked it (not Far, the earlier
    //    default, which was written whether picked or not) and its file has
    //    none: it applied to all of them when picked. The root drops it.
    // Nothing happens when the root has neither. What was done goes to log.
    void migrateCollectionFiles(const std::filesystem::path &root, const std::string &keeperId,
                                const std::vector<std::string> &gameIds, std::vector<std::string> *log = nullptr);
    // A player's controls.ini given the defaults that changed since it was
    // written (runtime/host_input.h: kControlsRevision, the file's
    // "controls_version" line, the game's changedDefaults). For each default
    // changed after the file's revision, a control still holding one of its
    // earlier defaults (the player never rebound it) gets the new one; a
    // control holding anything else is the player's and stays; a control the
    // file lacks (one added since) already takes its default when the game
    // reads the file. The file then says the current revision, so a control
    // the player sets back later is theirs. A file of a later revision (a
    // newer app's, before a downgrade) is left as it is. Everything else in
    // the text (comments, order, other values, line ends) is kept. What was
    // done goes to log.
    std::string updateControlsDefaults(const std::string &text, const ps2_host_input::GameControls &game,
                                       std::vector<std::string> *log = nullptr);
    // The same on the file of game `gameId` (runtime/host_input.h,
    // gameControls), rewritten only when something changed; a missing file
    // is left to the game, which writes its defaults. A playback's copy is
    // never given this: the recording plays with the controls it was made
    // with. True when the file was rewritten.
    bool updateControlsFile(const std::filesystem::path &file, const std::string &gameId,
                            std::vector<std::string> *log = nullptr);
    // The settings of a game in a collection: its own file, then the
    // root's on top (the app's settings; before the root has them, those of
    // the game's file, e.g. copied from its single-game app). False if
    // either file is missing (it is written then).
    bool loadCollectionSettings(const std::filesystem::path &rootFile, const std::filesystem::path &gameFile,
                                Settings &settings, std::vector<std::string> *warnings = nullptr);
    bool saveCollectionSettings(const std::filesystem::path &rootFile, const std::filesystem::path &gameFile,
                                const Settings &settings, const GameSettingsProfile &game);
}
