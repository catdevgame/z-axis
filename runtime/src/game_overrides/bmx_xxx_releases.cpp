// BMX XXX (SLUS-20415): what its Xbox and GameCube releases have and the PS2
// release lacks, each behind its own setting, off by default.
//
// The content is made at build time by tooling/ai/tools/bmx_releases.py from
// the user's Xbox and GameCube discs (the app sets ZX_BMX_RELEASES_DIR to
// Resources/games/bmx/releases); nothing here reads a disc image or converts
// anything. Files are served through the host's file layer
// (zx::game_files providers, zx::disc_patch), so the game reads them
// where it reads its own; the hooks are at the game's readable functions,
// by name.
//
//   Challenge scripts (ZX_BMX_XBOX_CHALLENGES=1, settings.ini [extras]
//   xbox_challenges): Las Vegas's, NASA's, Sheep Hills' and Syracuse's
//   objctv.txt as the Xbox and GameCube have them (the same files): the
//   challenges' success and failure lines (31; the PS2's own speech banks
//   hold every phrase they name, its scripts play AUDIO_PLACEHOLDER or
//   nothing there), Sheep Hills' four pickup tethers enabled, a NASA and a
//   Syracuse step fixed. loadLevelObjectives reads the file the session's
//   level names. Syracuse's two 45 s timers are 60 s in those scripts: that
//   is balance, so they stay the PS2's unless Gameplay Tuning is Xbox
//   (ZX_TUNING=xbox).
//
//   Languages (ZX_LANGUAGE=french|german|spanish, the app's Language): the
//   game's own French, German or Spanish (setLanguage is called with that
//   language where the game sets the saved game's, at the UI's set-up;
//   the PS2 has every table but no menu line that picks one), with
//   STRING.ZAL's tables of that language fuller: each text the PS2 leaves in
//   English that the GameCube or Xbox release translates, where that
//   release's English is the PS2's (strings/<table>, whole tables in the
//   PS2's format replacing the PS2's entries of the same names).
//
//   The park editor's Pool and Receptacle (ZX_BMX_EDITOR_PIECES=1,
//   [extras] editor_pieces): the GameCube's and Xbox's piece list
//   (editor/object.obj) has two pieces more than the PS2's, POOL
//   (Editobj_bowl_kidneypool, in BOWLS) and RECEPTACLE (the trash can
//   dynamicsdef_trashcan_, a dynamic object), on meshes the PS2's
//   editor/Objects holds. They come after the PS2's 253 pieces, so each
//   keeps its number: a saved park names its pieces by their number in the
//   list (editObjectSaveExtra, readEditPiece), and the list has no piece of
//   253 or 254 without them. So a park saved with either is recorded beside
//   its file (Saves/Parks/Park NNN.pieces, written after the park's file by
//   the native saves, from the pieces saveParkData's editObjectSaveExtra
//   calls wrote), and a boot that finds such a park turns the two pieces on
//   with the setting off: the park loads, edits and rides as it was made.
//   With no such park and the setting off, nothing of this is installed.
//
//   Level-select previews (ZX_BMX_LEVEL_PREVIEWS=1, [extras]
//   level_previews): UI_menu's level select names a preview movie per level
//   (assets\movies\menu\bronx.psm, ...) and its startMovie plays it on the
//   Xbox and GameCube only (GOTO Root.PlatformID: the PS2's "PS2" has no
//   label there). The script gets a :PS2 label beside :GC and :XBOX, the
//   level's still picture (shown on the PS2 alone, over the movie, which
//   fills the frame behind the panels) stays hidden as on the other two, and
//   the eight PSM files are the Xbox's movies/menu/*.XMV made into PSMs.
//
//   The Z-Axis logo movie in the gallery (ZX_BMX_LOGO_MOVIE=1, [extras]
//   logo_movie): the movie rows (g_movieRows) have it as row 1, "ZAxis",
//   open from a new game, and its reward text sends the player to the
//   gallery, but the gallery's table (mediaMovies, 19 rows) leaves it out on
//   every release. bmxmenuscriptPopulateMediaViewerMenu gets a 20th line,
//   "Z-AXIS" (open as the saved game's movie row 1 is), and
//   bmxmenuscriptGetSelectedMediaFileName answers that line with
//   assets\movies\ZAxis.pss, the Xbox's zaxis.xmv made into a PSS.
//
//   The intro movie at boot (ZX_BMX_INTRO_MOVIE=1, [extras] intro_movie):
//   the PS2's introMenu passes its movie screen (5, UI_common.Movie) at
//   once; the GameCube's plays movies/intro there (its intro menu, case 10),
//   then tells the UI the screen came in. The PS2's introFrame already ends
//   the screen when the movie is over or a button is pressed (menuMovieFrame)
//   and its leaving stops it. The PS2 disc's own INTRO.PSS. Skip Intro Movies
//   (game.skip_intro_movies) wins.
//
//   The Z-Axis logo at boot (ZX_BMX_LOGO_SPLASH=1, [extras] logo_splash):
//   the GameCube's intro screen 4 plays movies/ZAxis on UI_common.Movie
//   where the PS2's shows its developer splash (UI_intro's frame and its
//   quotes), its timing none (g_introTimings: the PS2's 3 s before a button
//   ends it, 6 s in all; the GameCube's 0, 0: the movie's end or a button
//   ends it, its frame asking menuMovieFrame as for screen 5); its leaving
//   hides the movie's frame, then leaves as the PS2's (the splash frame's
//   Shutdown says the screen has left: sent at once, inside the leaving, as
//   the GameCube's is, the PS2's UI never goes on), and once left the movie
//   is stopped (the viewports are reset when screen 5 has left). The
//   logo movie is the gallery's (assets\movies\ZAxis.pss, made from the
//   Xbox's zaxis.xmv). Skip Intro Movies wins.
//
//   The Competitive Edge trailers (ZX_BMX_TRAILERS=1, [extras] trailers):
//   UI_menu's FrameAKAdemos (Extras, Movies) plays a trailer by its line,
//   and has the lines of Turok (AKA_OPT05) and Vexx (AKA_OPT06), but only
//   the Xbox's list (Root.PlatformID "XBOX") offers them, and only the
//   Xbox disc has their files. The PS2's list gets the two after its five,
//   and assets\movies\CompEdge\Turok.pss and Vexx.pss are served (the
//   Xbox's movies/CompEdge/*.xmv made into PSS files with their sound).
//
//   Create-a-rider's textures and parts (ZX_BMX_CAR_TEXTURES=1, [extras]
//   car_textures): the Xbox's and GameCube's create/texture[A-D].zal have
//   51 textures the PS2's lack (green skins and faces, tops, suspenders, a
//   soul patch, clown shoes, hats) and the GameCube's create/parts.zal two
//   parts (GFU_NudeTop, GMHat_BballSw) on which some of them go; their part
//   lists, lock tables (create/*Lock.csv) and definitions are the PS2's
//   byte for byte, so the PS2's already name the two parts. The libraries
//   made with them after the PS2's own entries are served, and
//   createAModelGetFilteredListOfAssetsInLibrary (the style screens' lists)
//   lists the added textures as any other, but the green skin and the
//   topless ones: as on the Xbox and GameCube (their builds' same function),
//   a texture with _HUL in its name is listed only in Green Skin mode
//   (ZX_BMX_GREEN_SKIN=1, [extras] green_skin; their feature 5, "GREEN
//   SKIN", the code MAKEMEANGRY) and one with _TOPLESS only in Topless Girl
//   mode (ZX_BMX_TOPLESS=1, [extras] topless_girl; their feature 6, the
//   GameCube's NICE TATAS). Either mode serves the libraries too. The PS2's
//   feature 5 is its park editor and its saved game has no room for the two
//   modes, so they are the collection's settings, not the game's. A rider
//   saved with an added texture (a saved game naming one) has the libraries
//   served with the settings off, the added textures not listed: it loads
//   as it was made.
//
//   The GameCube's levels (ZX_BMX_GC_LEVELS=1, [extras] gc_levels): the
//   level table (g_levels) has twelve levels before the Bronx's park, and
//   rows 8-11 are the Secret Level (CMP_SCRT) and the flat, grind and vert
//   test levels (TST_FLAT, TST_GRND, TST_VERT), whose files only the
//   GameCube disc has, already in the PS2's formats (art viewer exports).
//   The game means the level select to have them as its items 9-12: its
//   table of items (g_levelSelectLevels) has twelve, the ninth already the
//   Secret Level's, and STRING.ZAL names the twelve
//   (KSTR_COM_LEVEL_OPT_00..11: "SECRET LEVEL", "FLAT TEST LEVEL", "GRIND
//   TEST LEVEL", "VERT TEST LEVEL" after the eight), but its list
//   (bmxmenuscriptGetLevelNames) makes the first eight. With the setting
//   the list gets the four after them (each level's title from g_levels,
//   as the game's eight), the table's three spare items (0: the Bronx)
//   their levels, and the files are served. No release has a picture or a
//   description for the four (UI_menu's OnChange names Levels.level<item>
//   and $KSTR_LEVELINFOMENU_<item>, of which only 0-7 are made; 8 has no
//   text, 9 is the park editor's): their items show none, and no preview
//   movie. They are listed open: nothing
//   in the game opens them but the cheat that opens every level (MASS
//   HYSTERIA). (The GameCube's TST_VERT2 holds TST_VERT.* files: TST_VERT's
//   scene exported again for it, its one texture a DDS; no executable names
//   it and it has no row: there is nothing more to wire.)
//
//   The Xbox's unused songs (ZX_BMX_XBOX_SONGS=1, [extras] xbox_songs):
//   the Xbox disc's audio folder has 28 songs, its music bank (music.zsd)
//   names 20, the PS2's 20 (MUSIC.ZSD's waves); the 8 no table names
//   (music/songs.txt, made from the disc at setup) are MUSIC.ZSD's waves
//   20-27. The bank's tables (its first 0x1A4 bytes: a hash index and
//   {offset, size} a wave, the first wave at 0x800) are served with 8 waves
//   more (the file layer's head, zx::game_files::registerHead: the 221 MB
//   file is read through, never copied), each after the file's end, sized
//   as a PS2 music wave of the song's length; the game's soundtrack is then
//   28 tracks, every one on, so the music plays the 8 in their turn after
//   the 20 (the playlist that turns tracks off, Options' PLAYLIST line, is
//   hidden in every release). A stream of one of those waves is the song's
//   file (the native sound's resolve: releasesSong), and its song info is
//   STRING.ZAL's KSTR_SONGINFO_20 .. 27 texts, added to every language's
//   common table: the title the file's name (the disc has no tags).
//
// ZX_TRACE_BMX_RELEASES=1 (developers): each file served, and each sound
// cue a challenge script gives (objectiveCue), with its phrase name when it
// is one of the release scripts' lines.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_releases.cpp.h"
#include "zge_saves.h"
#include "../game/host/movies.h"

#include "ps2_runtime.h"
#include "runtime/ps2_audio_file.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_zaxis_text.h"
#include "runtime/ps2_zaxis_ui.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmx_xxx::releases
{
    namespace
    {
        namespace gf = zx::game_files;
        namespace fs = std::filesystem;

        constexpr const char kTag[] = "[bmx-releases]";
        constexpr const char kOwner[] = "bmx-xxx-releases";
        constexpr int kRegV0 = 2, kRegA0 = 4, kRegA1 = 5, kRegSp = 29, kRegRa = 31;

        // The levels whose release scripts differ from the PS2's.
        constexpr const char *kChallengeLevels[] = {"CHL_MNCO", "CHL_NASA", "CHL_SHEP", "CHL_SYRA"};

        // The game's languages (game/bmx/ui/strings.h: g_languageCodes'
        // order) by the app's ZX_LANGUAGE.
        struct Language
        {
            const char *setting;
            uint32_t game; // the saved game's language
            const char *code;
        };
        constexpr Language kLanguages[] = {{"spanish", 2, "spa"}, {"german", 3, "ger"}, {"french", 4, "fre"}};

        // The intro's screens and the UI's messages (game/bmx/states/intro.cpp,
        // game/bmx/ui/menu.h).
        constexpr uint32_t kIntroMovieScreen = 5;
        constexpr int32_t kMsgEnded = 0, kMsgEnter = 10, kMsgCameIn = 11, kMsgLeave = 12, kMsgLeft = 13;
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        constexpr const char kIntroMovie[] = "assets\\movies\\intro.pss";
        constexpr const char kMovieNode[] = "UI_Common.Movie";
        constexpr const char kVisible[] = "Visible";
        constexpr const char kMovieFrameWidth[] = "SET Width {640}"; // introEnter's screen (640 by 448)

        // The gallery (game/bmx/ui/menu_script.cpp MediaMovie, mediaMovies):
        // its 19 rows, and the logo movie's line after them.
        constexpr uint32_t kGalleryRows = 19;
        constexpr uint32_t kLogoMovieRow = 1; // SavedGame::movies' row of "ZAxis" (g_movieRows)
        constexpr uint32_t kMovieOpen = 1;    // SavedGame::movies' kMovieRewardBit0
        // The saved game (game/bmx/profile/players.h SavedGame) as far as
        // its movie rows.
        struct SavedGameMovies
        {
            uint8_t _00000[0x21F60];
            uint32_t movies[20]; // kMovieRewardBit0: open
        };
        static_assert(offsetof(SavedGameMovies, movies) == 0x21F60);
        constexpr const char kLogoLine[] = "STRCAT Items {\"\\n%sZ-AXIS\"}";
        constexpr const char kLogoMovie[] = "assets\\movies\\ZAxis.pss";
        constexpr const char kSelectedIndex[] = "SelectedIndex";

        // The park editor: the PS2 list's pieces (editor/object.obj), the
        // number of the first piece only the other releases have, and what a
        // saved park's record of them is called beside its file.
        constexpr uint32_t kPs2Pieces = 253;
        constexpr const char kPiecesRecord[] = ".pieces";
        // An edit object (game/bmx/editor/edit_records.h EditObjectPart) as
        // far as its piece.
        struct EditPiece
        {
            uint8_t _00[0xA4];
            uint32_t pieceDef;
        };
        static_assert(offsetof(EditPiece, pieceDef) == 0xA4);

        // The challenge scripts' sound cue (game/bmx/objectives/objective_visitors.cpp).
        constexpr uint32_t kCueSound = 1;

        // The GameCube's levels: g_levels' rows 8-11, the level select's
        // items 9-12 (game/bmx/ui/menu_script.cpp: the select lists the
        // first kListedLevels of its kLevelSelectItems).
        struct GcLevel
        {
            const char *name; // its folder (LevelInfo::name)
            uint8_t level;    // its g_levels row
        };
        constexpr GcLevel kGcLevels[] = {{"CMP_SCRT", 8}, {"TST_FLAT", 9}, {"TST_GRND", 10}, {"TST_VERT", 11}};
        constexpr uint32_t kListedLevels = 8, kLevelSelectItems = 12;
        // A level's row (game/bmx/profile/players.h LevelInfo) as far as its title.
        struct LevelTitle
        {
            uint32_t code;
            uint32_t name;
            uint32_t title; // ("Secret Level")
            uint8_t _0c[4];
        };
        static_assert(sizeof(LevelTitle) == 0x10 && offsetof(LevelTitle, title) == 0x08);
        // The list's text: the script's line break, at most this long (the
        // list's text buffer).
        constexpr const char kListBreak[] = "\\n";
        constexpr size_t kLevelNamesSize = 0x480;

        // The logo at boot (game/bmx/states/intro.cpp): the developer
        // splash's screen and the intro's timing table (IntroTiming, 6 bytes
        // a screen).
        constexpr uint32_t kDeveloperSplashScreen = 4;
        constexpr uint32_t kIntroTimingBytes = 6;
        constexpr const char kLogoSplashMovie[] = "assets\\movies\\ZAxis.pss";

        // The trailers: FrameAKAdemos' list as the PS2 makes it, and with the
        // Xbox's two lines after its five.
        constexpr const char kTrailersMenu[] = "UI_menu/CommonBackground/FrameExtras/FrameAKAdemos/Menu";
        constexpr const char kTrailerItems[] =
            "SET Items {\"$KSTR_EXTRAS_AKA_OPT00\\n$KSTR_EXTRAS_AKA_OPT01\\n$KSTR_EXTRAS_AKA_OPT02\\n$KSTR_EXTRAS_AKA_OPT03"
            "\\n$KSTR_EXTRAS_AKA_OPT04\"}";
        constexpr const char kTrailerItemsXbox[] =
            "SET Items {\"$KSTR_EXTRAS_AKA_OPT00\\n$KSTR_EXTRAS_AKA_OPT01\\n$KSTR_EXTRAS_AKA_OPT02\\n$KSTR_EXTRAS_AKA_OPT03"
            "\\n$KSTR_EXTRAS_AKA_OPT04\\n$KSTR_EXTRAS_AKA_OPT05\\n$KSTR_EXTRAS_AKA_OPT06\"}";
        constexpr const char *kTrailerFiles[] = {"ASSETS/MOVIES/COMPEDGE/TUROK.PSS", "ASSETS/MOVIES/COMPEDGE/VEXX.PSS"};

        // Create-a-rider: the libraries made (create/, tools/bmx_releases.py)
        // and the marks the Xbox's and GameCube's lists look for (in the
        // name in capitals).
        constexpr const char *kCreateLibraries[] = {"PARTS.ZAL", "TEXTUREA.ZAL", "TEXTUREB.ZAL", "TEXTUREC.ZAL", "TEXTURED.ZAL"};
        constexpr const char kAddedTextures[] = "added.txt";
        constexpr const char kGreenSkinMark[] = "_HUL";
        constexpr const char kToplessMark[] = "_TOPLESS";
        constexpr size_t kAssetNameLimit = 0x40;

        // The Xbox's unused songs: the music bank, its tables (ZGE4's
        // SoundBank, game/zge/sound/bank.h: the counts and offsets of its
        // seven tables after the head; a hash index {hash, number} and the
        // waves' {offset, size}), the hash after its waves' (0x2A78180 ..
        // 0x2A781F9), the songs' list, a sector, and the song info's keys.
        constexpr const char kMusicBank[] = "ASSETS/AUDIO/MUSIC.ZSD";
        constexpr const char kSongsList[] = "songs.txt";
        constexpr size_t kBankHeadBytes = 0x64, kBankTableBytes = 0x0C, kBankFileBytes = 0x08;
        constexpr size_t kBankTables = 7, kBankWaveTable = 2; // (samples, references, waves, playlists, groups, ?, sets)
        constexpr uint32_t kSongWaveHash = 0x2A78200u;
        constexpr uint32_t kGameSongs = 20; // MUSIC.ZSD's own waves (the head is refused when it has another count)
        constexpr uint64_t kSector = 0x800;
        constexpr const char kSongTitleKey[] = "KSTR_SONGINFO_%02u_SONGNAME";
        constexpr const char kSongArtistKey[] = "KSTR_SONGINFO_%02u_ARTIST";
        constexpr const char kSongArtist[] = "XBOX DISC";
        constexpr const char *kStringLanguages[] = {"eus", "euk", "fre", "ger", "spa"};
        struct Song
        {
            fs::path file;
            std::string title;
            double seconds = 0;
            uint64_t start = 0; // its wave's offset in the bank, once made
        };

        struct State
        {
            fs::path dir;
            bool challenges = false;
            bool xboxTimers = false;
            const Language *language = nullptr;
            bool previews = false;
            bool editorPieces = false;
            uint32_t savedExtraPieces = 0; // in the park being saved
            bool logoMovie = false;
            bool introMovie = false;
            bool gcLevels = false;
            bool listingLevels = false; // bmxmenuscriptGetLevelNames is making the list
            bool trace = false;
            bool introPlaying = false; // the movie screen plays the intro (left: hidden again)
            bool logoSplash = false;
            bool logoPlaying = false;     // screen 4 plays the logo
            uint8_t splashTiming[kIntroTimingBytes] = {}; // the PS2's, while the logo plays
            bool trailers = false;
            bool createFiles = false;     // the create-a-rider libraries served
            bool createListed = false;    // the added textures listed (car_textures, green_skin or topless_girl)
            bool greenSkin = false;
            bool topless = false;
            bool listingAssets = false;   // createAModelGetFilteredListOfAssetsInLibrary is making a list
            std::map<std::string, bool> addedTextures; // name in capitals -> listed
            std::vector<std::string> addedNames;        // as the libraries have them
            bool songs = false;
            uint32_t gameSongs = 0;       // MUSIC.ZSD's own waves, once its head is made
            std::vector<Song> songList;
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files;
            std::map<uint32_t, std::string> phrases; // name hash -> a release script's phrase
            PS2Runtime::RecompiledFunction setLanguage = nullptr;
            PS2Runtime::RecompiledFunction introMenu = nullptr;
            PS2Runtime::RecompiledFunction populateGallery = nullptr;
            PS2Runtime::RecompiledFunction gallerySelection = nullptr;
            PS2Runtime::RecompiledFunction objectiveCue = nullptr;
            PS2Runtime::RecompiledFunction playSpeech = nullptr;
            PS2Runtime::RecompiledFunction saveParkData = nullptr;
            PS2Runtime::RecompiledFunction editObjectSaveExtra = nullptr;
            PS2Runtime::RecompiledFunction levelNames = nullptr;
            PS2Runtime::RecompiledFunction setVariableValue = nullptr;
            PS2Runtime::RecompiledFunction introFrame = nullptr;
            PS2Runtime::RecompiledFunction assetList = nullptr;
            PS2Runtime::RecompiledFunction currentAssetName = nullptr;
        };
        State &state() { return zx::session::state<State>(); }

        bool on(const char *name)
        {
            const char *value = zx::boot::text(name);
            return value && value[0] == '1';
        }

        std::shared_ptr<const std::vector<uint8_t>> readHost(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            auto bytes = std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return bytes;
        }

        // A bundled file, read once.
        // A movie of the PS2's name: the PSM or PSS the workshop's build made
        // (tools/bmx_releases.py), else the Xbox disc's XMV the setup copied,
        // which the movie player plays in its place (setHostMovie).
        fs::path movieFile(const std::string &path)
        {
            const fs::path made = state().dir / "movies" / path;
            std::error_code ec;
            if (fs::exists(made, ec))
                return made;
            return fs::path(made).replace_extension(".XMV");
        }

        // Whether the folder holds a file of that extension (a made movie).
        bool hasFileOf(const fs::path &folder, const char *extension)
        {
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == extension)
                    return true;
            return false;
        }

        // The movies of the PS2's names only the Xbox's XMV files are there
        // for, played by the host (none with ZX_MOVIES=hle). False: none.
        bool hostMovies(const fs::path &folder, const std::string &gamePrefix, const char *gameExtension)
        {
            std::error_code ec;
            bool any = false;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::path file = it->path();
                if (file.extension() != ".XMV")
                    continue;
                const std::string gamePath = gamePrefix + file.stem().string() + gameExtension;
                if (fs::exists(fs::path(file).replace_extension(gameExtension), ec))
                    continue; // a made PSM or PSS plays as the game's own
                if (!game::host::movies::playedOnHost())
                    return false;
                const std::u8string host = file.u8string();
                game::host::movies::setHostMovie(gamePath, std::string(host.begin(), host.end()),
                                                 game::host::movies::kPs2MovieWidth, game::host::movies::kPs2MovieHeight);
                any = true;
            }
            return any;
        }

        std::shared_ptr<const std::vector<uint8_t>> bundled(const std::string &disc, const fs::path &host)
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.mutex);
            auto it = st.files.find(disc);
            if (it != st.files.end())
                return it->second;
            auto bytes = readHost(host);
            st.files[disc] = bytes;
            if (st.trace)
                std::cout << kTag << " " << disc << ": " << (bytes ? host.string() : std::string("none bundled")) << std::endl;
            return bytes;
        }

        uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t at)
        {
            uint32_t value = 0;
            if (at + 4 <= bytes.size())
                std::memcpy(&value, bytes.data() + at, 4);
            return value;
        }
        void writeLe32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
        {
            if (at + 4 <= bytes.size())
                std::memcpy(bytes.data() + at, &value, 4);
        }
        void appendLe32(std::vector<uint8_t> &bytes, uint32_t value)
        {
            bytes.resize(bytes.size() + 4);
            writeLe32(bytes, bytes.size() - 4, value);
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }

        // ---- the files ----------------------------------------------------

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            State &st = state();
            if (st.challenges)
                for (const char *level : kChallengeLevels)
                    if (path == std::string("ASSETS/LEVELS/") + level + "/OBJCTV.TXT")
                        return bundled(path, st.dir / "challenges" / (st.xboxTimers ? "xbox" : "ps2-timers") / level / "OBJCTV.TXT");
            if (st.editorPieces && path == "ASSETS/EDITOR/OBJECT.OBJ")
                return bundled(path, st.dir / "editor" / "OBJECT.OBJ");
            if (st.previews && path.rfind("ASSETS/MOVIES/MENU/", 0) == 0 && path.size() > 4 &&
                path.compare(path.size() - 4, 4, ".PSM") == 0)
                return bundled(path, movieFile(path));
            if ((st.logoMovie || st.logoSplash) && path == "ASSETS/MOVIES/ZAXIS.PSS")
                return bundled(path, movieFile(path));
            if (st.gcLevels)
                for (const GcLevel &level : kGcLevels)
                    if (path.rfind(std::string("ASSETS/LEVELS/") + level.name + "/", 0) == 0)
                        return bundled(path, st.dir / "levels" / path);
            if (st.trailers)
                for (const char *trailer : kTrailerFiles)
                    if (path == trailer)
                        return bundled(path, movieFile(path));
            if (st.createFiles)
                for (const char *library : kCreateLibraries)
                    if (path == std::string("ASSETS/CREATE/") + library)
                        return bundled(path, st.dir / "create" / library);
            return nullptr;
        }

        // STRING.ZAL's tables of the language replaced by the bundled fuller ones.
        bool patchStrings(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!st.language || !zx::zaxis::readArchive(bytes, entries))
                return false;
            std::map<uint32_t, fs::path> tables;
            std::error_code ec;
            const std::string suffix = std::string(".") + st.language->code;
            for (const auto &file : fs::directory_iterator(st.dir / "strings", ec))
            {
                const std::string name = file.path().filename().string();
                if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                    tables[zx::zaxis_text::keyHash(name)] = file.path();
            }
            uint32_t replaced = 0;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                auto it = tables.find(entry.nameHash);
                if (it == tables.end())
                    continue;
                if (auto table = readHost(it->second))
                {
                    entry.data = *table;
                    ++replaced;
                }
            }
            if (replaced == 0)
                return false;
            bytes = zx::zaxis::writeArchive(entries);
            std::cout << kTag << " STRING.ZAL: " << replaced << " " << st.language->setting
                      << " tables from the GameCube and Xbox releases" << std::endl;
            return true;
        }

        // UI_menu's level select: its preview movie's start on the PS2 too,
        // and the level's still picture (the PS2's alone, over the movie's
        // middle) left hidden, as on the Xbox and GameCube.
        constexpr const char kLevelPictureShown[] = "IF Root.PlatformID = {\\\"PS2\\\"} SET LevelPicture.Visible {1}";
        constexpr const char kLevelPictureHidden[] = "IF Root.PlatformID = {\\\"PS2\\\"} SET LevelPicture.Visible {0}";
        bool patchLevelSelect(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) != "UI_menu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                zx::zaxis::Span menu = zx::zaxis::findObject(text, "UI_menu/CommonBackground/FrameLevelSelect/OPTION/Menu");
                zx::zaxis::Span option = zx::zaxis::findObject(text, "UI_menu/CommonBackground/FrameLevelSelect/OPTION");
                const std::string p(static_cast<size_t>(menu.found() ? zx::zaxis::indentAt(text, menu.begin) + 3 : 0), '\t');
                if (menu.found() && option.found() &&
                    zx::zaxis::replaceIn(text, menu, p + ":GC\r\n" + p + ":XBOX\r\n",
                                  p + ":GC\r\n" + p + ":XBOX\r\n" + p + ":PS2\r\n") &&
                    zx::zaxis::replaceIn(text, option, kLevelPictureShown, kLevelPictureHidden))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                    std::cout << kTag << " level select: the preview movies" << std::endl;
                }
                else
                {
                    std::cerr << kTag << " UI_menu's level select is not the expected script: no preview movies" << std::endl;
                }
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }

        // UI_menu's level select: the GameCube's four items (8-11) with no
        // picture, description or preview movie (the script names the
        // eight levels' alone): after its last level's label, theirs, the
        // picture's colours clear (the next item's change sets them again),
        // the text empty and the movie faded out.
        constexpr const char kLastLevelLabel[] = ":7\r\n";
        constexpr const char kLastLevelMovie[] = "SET movieFileName {\"assets\\movies\\menu\\NASA.psm\"}\r\n";
        constexpr const char kToMovieStart[] = "GOTO DoneFileName\r\n";
        bool patchGcLevelItems(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) != "UI_menu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                zx::zaxis::Span menu = zx::zaxis::findObject(text, "UI_menu/CommonBackground/FrameLevelSelect/OPTION/Menu");
                const std::string p(static_cast<size_t>(menu.found() ? zx::zaxis::indentAt(text, menu.begin) + 2 : 0), '\t');
                const std::string last = p + kLastLevelLabel + p + kLastLevelMovie + p + kToMovieStart;
                std::string items;
                for (const GcLevel &level : kGcLevels)
                    items += p + ":" + std::to_string(level.level) + "\r\n";
                for (const char *corner : {"TopLeftColor", "TopRightColor", "BottomLeftColor", "BottomRightColor"})
                    items += p + "SET .LevelPicture." + corner + " {0 0 0 0}\r\n";
                items += p + "SET .LevelInfo.Text {\"\"}\r\n" + p + "ABORT startMovie\r\n" + p + "ABORT FadeInMovie\r\n" + p +
                         "PLAY FadeOutMovie\r\n" + p + "EXIT\r\n";
                if (menu.found() && zx::zaxis::replaceIn(text, menu, last, last + items))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                    std::cout << kTag << " level select: the GameCube's levels' items" << std::endl;
                }
                else
                {
                    std::cerr << kTag << " UI_menu's level select is not the expected script: the GameCube's levels' items "
                              << "show the missing picture and text" << std::endl;
                }
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }

        // UI_menu's FrameAKAdemos: the PS2's list of trailers with the Xbox's
        // two after its five.
        bool patchTrailers(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) != "UI_menu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                zx::zaxis::Span menu = zx::zaxis::findObject(text, kTrailersMenu);
                if (menu.found() && zx::zaxis::replaceIn(text, menu, kTrailerItems, kTrailerItemsXbox))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                    std::cout << kTag << " movies: the Turok and Vexx trailers" << std::endl;
                }
                else
                {
                    std::cerr << kTag << " UI_menu's FrameAKAdemos is not the expected script: no Turok and Vexx trailers"
                              << std::endl;
                }
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }

        // The added create-a-rider textures (create/added.txt), each with
        // whether the lists show it: the green skin ones in Green Skin mode,
        // the topless ones in Topless Girl mode, the rest with any of the
        // three settings.
        void readAddedTextures(State &st)
        {
            std::ifstream in(st.dir / "create" / kAddedTextures);
            std::string name;
            while (std::getline(in, name))
            {
                if (!name.empty() && name.back() == '\r')
                    name.pop_back();
                if (name.empty())
                    continue;
                const std::string upperName = upper(name);
                bool listed = st.createListed;
                if (upperName.find(kGreenSkinMark) != std::string::npos)
                    listed = st.greenSkin;
                else if (upperName.find(kToplessMark) != std::string::npos)
                    listed = st.topless;
                st.addedTextures[upperName] = listed;
                st.addedNames.push_back(name);
            }
        }

        // A saved game (Saves/*.sav) naming one of the added textures: a
        // rider made with it.
        std::string saveNamingAddedTexture(const State &st)
        {
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(zge_host::saves::folder(), ec))
            {
                if (entry.path().extension() != ".sav" || !entry.is_regular_file(ec))
                    continue;
                const auto bytes = readHost(entry.path());
                if (!bytes)
                    continue;
                const std::string text(bytes->begin(), bytes->end());
                // (a saved model's names are the libraries', NUL-ended)
                for (const std::string &name : st.addedNames)
                    if (text.find(name + std::string(1, '\0')) != std::string::npos)
                        return entry.path().filename().string();
            }
            return {};
        }

        // The songs (music/songs.txt: file, the Xbox's file, title, seconds a
        // line). False: none to play.
        bool readSongs(State &st)
        {
            std::ifstream in(st.dir / "music" / kSongsList);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::vector<std::string> fields;
                for (size_t at = 0;;)
                {
                    const size_t tab = line.find('\t', at);
                    fields.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
                    if (tab == std::string::npos)
                        break;
                    at = tab + 1;
                }
                if (fields.size() != 4)
                    continue;
                std::error_code ec;
                Song song{st.dir / "music" / fields[0], fields[2], std::strtod(fields[3].c_str(), nullptr), 0};
                if (song.seconds > 0 && fs::exists(song.file, ec))
                    st.songList.push_back(std::move(song));
            }
            return !st.songList.empty();
        }

        // A song's length as a PS2 music wave of it: 48 kHz stereo PS-ADPCM
        // (16 bytes per 28 samples a channel).
        uint64_t ps2MusicBytes(double seconds)
        {
            const uint64_t samples = static_cast<uint64_t>(seconds * 48000.0);
            return (samples + 27) / 28 * 16 * 2;
        }

        // MUSIC.ZSD's tables with the songs' waves after its own: its waves'
        // count, hash index and table grown, the tables after them moved on,
        // each new wave after the file's end. Null (the bank as it is) when
        // the bank has tables other than its waves' or no room before its
        // first wave.
        std::shared_ptr<const std::vector<uint8_t>> musicBankHead(gf::GameFile &bank)
        {
            State &st = state();
            std::vector<uint8_t> head(kSector);
            if (bank.read(0, head.data(), head.size()) != head.size() || std::memcmp(head.data(), "ZSNDPS2 ", 8) != 0)
                return nullptr;
            const uint32_t tableBytes = readLe32(head, kBankTableBytes);
            uint32_t counts[kBankTables], hashes[kBankTables], tables[kBankTables];
            for (size_t t = 0; t < kBankTables; ++t)
            {
                counts[t] = readLe32(head, 0x10 + 12 * t);
                hashes[t] = readLe32(head, 0x14 + 12 * t);
                tables[t] = readLe32(head, 0x18 + 12 * t);
                if (t != kBankWaveTable && counts[t] != 0)
                    return nullptr;
            }
            const uint32_t waves = counts[kBankWaveTable], added = static_cast<uint32_t>(st.songList.size());
            if (hashes[kBankWaveTable] != kBankHeadBytes || tables[kBankWaveTable] != kBankHeadBytes + 8 * waves ||
                tableBytes != tables[kBankWaveTable] + 8 * waves || waves != kGameSongs)
                return nullptr;
            const uint32_t firstWave = readLe32(head, tables[kBankWaveTable]);
            const uint32_t newTables = kBankHeadBytes + 16 * (waves + added);
            if (newTables > firstWave || firstWave > kSector)
                return nullptr;
            std::vector<uint8_t> out(head.begin(), head.begin() + kBankHeadBytes);
            std::vector<uint8_t> hashIndex(head.begin() + hashes[kBankWaveTable], head.begin() + tables[kBankWaveTable]);
            std::vector<uint8_t> waveTable(head.begin() + tables[kBankWaveTable], head.begin() + tableBytes);
            uint64_t at = (bank.size() + kSector - 1) / kSector * kSector;
            for (uint32_t i = 0; i < added; ++i)
            {
                Song &song = st.songList[i];
                const uint64_t size = ps2MusicBytes(song.seconds);
                song.start = at;
                appendLe32(hashIndex, kSongWaveHash + i);
                appendLe32(hashIndex, waves + i);
                appendLe32(waveTable, static_cast<uint32_t>(at));
                appendLe32(waveTable, static_cast<uint32_t>(size));
                at = (at + size + kSector - 1) / kSector * kSector;
            }
            out.insert(out.end(), hashIndex.begin(), hashIndex.end());
            out.insert(out.end(), waveTable.begin(), waveTable.end());
            for (size_t t = 0; t < kBankTables; ++t)
            {
                // (the empty tables before the waves' stay, those after move on)
                const uint32_t moved = tables[t] >= tableBytes ? newTables : tables[t];
                writeLe32(out, 0x14 + 12 * t, t == kBankWaveTable ? kBankHeadBytes : (hashes[t] >= tableBytes ? newTables : hashes[t]));
                writeLe32(out, 0x18 + 12 * t, t == kBankWaveTable ? kBankHeadBytes + 8 * (waves + added) : moved);
            }
            writeLe32(out, 0x10 + 12 * kBankWaveTable, waves + added);
            writeLe32(out, kBankTableBytes, newTables);
            writeLe32(out, kBankFileBytes, static_cast<uint32_t>(at));
            out.resize(firstWave, 0); // (up to the first wave: the bank's own padding)
            st.gameSongs = waves;
            std::cout << kTag << " songs: MUSIC.ZSD's " << waves << " waves and the Xbox's " << added << " unused songs (waves "
                      << waves << "-" << waves + added - 1 << ")" << std::endl;
            return std::make_shared<const std::vector<uint8_t>>(std::move(out));
        }

        // A string table (size, count, {key, offset} a text, the texts; offset
        // 0: no text) with texts added after its own.
        bool appendStrings(std::vector<uint8_t> &table, const std::vector<std::pair<uint32_t, std::string>> &texts)
        {
            if (table.size() < 8)
                return false;
            const uint32_t count = readLe32(table, 4);
            const size_t entries = 8 + 8 * size_t(count), grow = 8 * texts.size();
            if (entries > table.size())
                return false;
            std::vector<uint8_t> out(table.begin(), table.begin() + long(entries));
            for (uint32_t i = 0; i < count; ++i)
                if (const uint32_t offset = readLe32(out, 12 + 8 * i); offset != 0)
                    writeLe32(out, 12 + 8 * i, static_cast<uint32_t>(offset + grow));
            out.resize(entries + grow);
            out.insert(out.end(), table.begin() + long(entries), table.end());
            for (size_t i = 0; i < texts.size(); ++i)
            {
                writeLe32(out, entries + 8 * i, texts[i].first);
                writeLe32(out, entries + 8 * i + 4, static_cast<uint32_t>(out.size()));
                out.insert(out.end(), texts[i].second.begin(), texts[i].second.end());
                out.push_back(0);
            }
            writeLe32(out, 4, static_cast<uint32_t>(count + texts.size()));
            writeLe32(out, 0, static_cast<uint32_t>(out.size()));
            table = std::move(out);
            return true;
        }

        // STRING.ZAL: the songs' info (KSTR_SONGINFO_20 .. 27's title and
        // artist) in every language's common table.
        bool patchSongTexts(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            const uint32_t first = kGameSongs;
            std::vector<std::pair<uint32_t, std::string>> texts;
            char key[64];
            for (uint32_t i = 0; i < st.songList.size(); ++i)
            {
                std::snprintf(key, sizeof(key), kSongTitleKey, first + i);
                texts.push_back({zx::zaxis_text::keyHash(key), upper(st.songList[i].title)});
                std::snprintf(key, sizeof(key), kSongArtistKey, first + i);
                texts.push_back({zx::zaxis_text::keyHash(key), kSongArtist});
            }
            uint32_t tables = 0;
            for (zx::zaxis::ArchiveEntry &entry : entries)
                for (const char *language : kStringLanguages)
                    if (entry.nameHash == zx::zaxis_text::keyHash(std::string("string.common.") + language) &&
                        appendStrings(entry.data, texts))
                        ++tables;
            if (tables == 0)
                return false;
            bytes = zx::zaxis::writeArchive(entries);
            std::cout << kTag << " STRING.ZAL: the songs' info in " << tables << " tables" << std::endl;
            return true;
        }

        // ---- the hooks ----------------------------------------------------

        // Guest scratch below the caller's stack: text at `at`, the callees'
        // stack below it.
        struct Scratch
        {
            uint32_t stack;
            uint32_t at;
        };
        Scratch scratchBelow(R5900Context *ctx)
        {
            const uint32_t stack = (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu;
            return {stack, stack + 0x100u};
        }
        void putText(uint8_t *rdram, uint32_t at, const std::string &text)
        {
            if (uint8_t *p = getMemPtr(rdram, at))
                std::memcpy(p, text.c_str(), text.size() + 1);
        }
        uint32_t readU32(uint8_t *rdram, uint32_t at)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, at))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }
        void writeU32(uint8_t *rdram, uint32_t at, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, at))
                std::memcpy(p, &value, sizeof(value));
        }
        void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, kRegRa); }

        // setLanguage(language): the app's language instead.
        void setLanguageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            SET_GPR_U32(ctx, kRegA0, st.language->game);
            st.setLanguage(rdram, ctx, runtime);
        }

        // The intro's movie frame (UI_common.Movie) hidden again, the movie
        // stopped when `stop` (the GameCube's leaving of screens 4 and 5).
        void hideIntroMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Scratch &scratch, bool stop)
        {
            putText(rdram, scratch.at, kMovieNode);
            putText(rdram, scratch.at + 0x40u, kVisible);
            writeU32(rdram, scratch.at + 0x80u, 0u);
            uint32_t node = 0;
            if (runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode,
                                           {readU32(rdram, sym::bmx::g_uiRoot), scratch.at, kFrameNode}, scratch.stack, &node) &&
                node != 0)
                runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariable, {node, scratch.at + 0x40u, scratch.at + 0x80u},
                                           scratch.stack);
            if (stop)
                runtime->callGuestFunction(rdram, ctx, sym::bmx::stopMenuMovie, {}, scratch.stack);
        }

        // A movie played on the intro's movie frame as the GameCube's intro
        // menu plays one (case 10), the frame as wide as the intro's screen
        // (its Width is Root.ScreenWidth when UI_common is made, the boot's
        // 512: the GameCube's is 640 then, as the PS2's is by the front end).
        // False when there is no such file.
        bool playIntroMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Scratch &scratch, const char *movie)
        {
            putText(rdram, scratch.at, kMovieNode);
            uint32_t node = 0;
            if (runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode,
                                           {readU32(rdram, sym::bmx::g_uiRoot), scratch.at, kFrameNode}, scratch.stack, &node) &&
                node != 0)
            {
                putText(rdram, scratch.at, kMovieFrameWidth);
                runtime->callGuestFunction(rdram, ctx, sym::bmx::runScript, {node, scratch.at, 0u}, scratch.stack);
            }
            putText(rdram, scratch.at, movie);
            uint32_t found = 0;
            runtime->callGuestFunction(rdram, ctx, sym::bmx::playMenuMovie, {scratch.at, 0u}, scratch.stack, &found);
            return found != 0;
        }

        // A message to the UI (the GameCube's intro menu's: the screen came in
        // once its movie starts, has left once it is stopped, ended).
        void sendUiMessage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Scratch &scratch, int32_t message)
        {
            writeU32(rdram, sym::bmx::g_msgType, static_cast<uint32_t>(message));
            runtime->callGuestFunction(rdram, ctx, sym::bmx::bmxuiHandler, {}, scratch.stack);
        }

        // introMenu(event): the intro movie on the movie screen (5) and the
        // logo on the developer splash's (4), as the GameCube's intro menu
        // plays them.
        void introMenuHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const int32_t event = static_cast<int32_t>(getRegU32(ctx, kRegA0));
            if (st.trace && event != kMsgEnter && event != kMsgLeave && event != kMsgLeft)
                std::cout << kTag << " introMenu: message " << event << std::endl;
            if (event != kMsgEnter && event != kMsgLeave && event != kMsgLeft)
            {
                st.introMenu(rdram, ctx, runtime);
                return;
            }
            const Scratch scratch = scratchBelow(ctx);
            uint32_t screen = 0;
            runtime->callGuestFunction(rdram, ctx, sym::bmx::menuId, {}, scratch.stack, &screen);
            if (st.trace)
                std::cout << kTag << " introMenu: screen " << screen << ", message " << event << std::endl;
            const uint32_t timing = sym::bmx::g_introTimings + kDeveloperSplashScreen * kIntroTimingBytes;
            if (screen == kDeveloperSplashScreen && st.logoSplash)
            {
                if (event == kMsgEnter && zx::settings::value("game.skip_intro_movies", 0) == 0 &&
                    playIntroMovie(rdram, ctx, runtime, scratch, kLogoSplashMovie))
                {
                    // The GameCube's timing for the screen: the movie ends it.
                    if (uint8_t *t = getMemPtr(rdram, timing))
                    {
                        std::memcpy(st.splashTiming, t, kIntroTimingBytes);
                        std::memset(t, 0, 4); // (skip after, end after; its load step stays)
                    }
                    st.logoPlaying = true;
                    std::cout << kTag << " intro: " << kLogoSplashMovie << " (the GameCube's logo at boot)" << std::endl;
                    sendUiMessage(rdram, ctx, runtime, scratch, kMsgCameIn);
                    returnToCaller(ctx);
                    return;
                }
                if (event == kMsgLeave && st.logoPlaying)
                {
                    // The GameCube's leaving: the movie's frame hidden and the
                    // movie stopped; the screen's own leaving (its frame's
                    // Shutdown, which says the screen has left) as the PS2's.
                    hideIntroMovie(rdram, ctx, runtime, scratch, false);
                    if (uint8_t *t = getMemPtr(rdram, timing))
                        std::memcpy(t, st.splashTiming, kIntroTimingBytes);
                    st.introMenu(rdram, ctx, runtime);
                    return;
                }
                if (event == kMsgLeft && st.logoPlaying)
                {
                    // (as the PS2's movie screen's leaving; its viewports are reset when screen 5, which
                    // follows, has left, as the GameCube's are)
                    st.logoPlaying = false;
                    runtime->callGuestFunction(rdram, ctx, sym::bmx::stopMenuMovie, {}, scratch.stack);
                }
                st.introMenu(rdram, ctx, runtime);
                return;
            }
            if (screen != kIntroMovieScreen || !st.introMovie || event == kMsgLeft)
            {
                st.introMenu(rdram, ctx, runtime);
                return;
            }
            if (event == kMsgLeave)
            {
                // The GameCube's leaving: the movie's frame hidden (the PS2's
                // never shows it; its kMsgLeft stops the movie).
                if (st.introPlaying)
                {
                    st.introPlaying = false;
                    hideIntroMovie(rdram, ctx, runtime, scratch, false);
                }
                st.introMenu(rdram, ctx, runtime);
                return;
            }
            if (zx::settings::value("game.skip_intro_movies", 0) != 0 || !playIntroMovie(rdram, ctx, runtime, scratch, kIntroMovie))
            {
                st.introMenu(rdram, ctx, runtime); // skipped, or no such file: passed, as the PS2's
                return;
            }
            st.introPlaying = true;
            std::cout << kTag << " intro: " << kIntroMovie << std::endl;
            sendUiMessage(rdram, ctx, runtime, scratch, kMsgCameIn);
            returnToCaller(ctx);
        }

        // introFrame(): on the logo's screen, the movie's frame as on the
        // movie screen: its end or a button ends the screen.
        void introFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.introFrame(rdram, ctx, runtime);
            if (!st.logoPlaying)
                return;
            const Scratch scratch = scratchBelow(ctx);
            uint32_t screen = 0;
            runtime->callGuestFunction(rdram, ctx, sym::bmx::menuId, {}, scratch.stack, &screen);
            if (screen != kDeveloperSplashScreen)
                return;
            putText(rdram, scratch.at, kMovieNode);
            uint32_t node = 0, playing = 0;
            runtime->callGuestFunction(rdram, ctx, sym::bmx::findNode, {readU32(rdram, sym::bmx::g_uiRoot), scratch.at, kFrameNode},
                                       scratch.stack, &node);
            runtime->callGuestFunction(rdram, ctx, sym::bmx::menuMovieFrame, {node}, scratch.stack, &playing);
            if (playing == 0)
                sendUiMessage(rdram, ctx, runtime, scratch, kMsgEnded);
        }

        // createAModelGetFilteredListOfAssetsInLibrary(arguments): the list
        // made with the added textures it does not show left out
        // (currentAssetName, below).
        void assetListHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.listingAssets = true;
            st.assetList(rdram, ctx, runtime);
            st.listingAssets = false;
        }

        // currentAssetName(library): while a list is made, an added texture
        // it does not show has no name, which the list passes over (the
        // Xbox's and GameCube's lists pass a _HUL or _TOPLESS name over
        // there).
        void currentAssetNameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.currentAssetName(rdram, ctx, runtime);
            if (!st.listingAssets)
                return;
            const uint32_t at = getRegU32(ctx, kRegV0);
            const char *name = at ? reinterpret_cast<const char *>(getConstMemPtr(rdram, at)) : nullptr;
            if (!name)
                return;
            const auto it = st.addedTextures.find(upper(std::string(name, strnlen(name, kAssetNameLimit))));
            if (it == st.addedTextures.end() || it->second)
                return;
            SET_GPR_U32(ctx, kRegV0, 0u);
            if (st.trace)
                std::cout << kTag << " create-a-rider: " << it->first << " not listed" << std::endl;
        }

        // bmxmenuscriptPopulateMediaViewerMenu(arguments): the game's 19
        // lines, then the logo movie's.
        void populateGalleryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const uint32_t arguments = getRegU32(ctx, kRegA0);
            st.populateGallery(rdram, ctx, runtime);
            const uint32_t list = readU32(rdram, arguments + 4u);
            const bool open = (readU32(rdram, sym::bmx::g_savedGame + offsetof(SavedGameMovies, movies) + kLogoMovieRow * sizeof(uint32_t)) & kMovieOpen) != 0;
            char line[64];
            std::snprintf(line, sizeof(line), kLogoLine, open ? "" : "!");
            const Scratch scratch = scratchBelow(ctx);
            putText(rdram, scratch.at, line);
            runtime->callGuestFunction(rdram, ctx, sym::bmx::runScript, {list, scratch.at, 0u}, scratch.stack);
            if (st.trace)
                std::cout << kTag << " gallery: line " << kGalleryRows << ", Z-AXIS (" << (open ? "open" : "not open") << ")"
                          << std::endl;
        }

        // bmxmenuscriptGetSelectedMediaFileName(arguments: out, list, list):
        // the logo movie's line answered with its file.
        void gallerySelectionHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const uint32_t arguments = getRegU32(ctx, kRegA0);
            const uint32_t out = readU32(rdram, arguments);
            const uint32_t list = readU32(rdram, arguments + 8u);
            const Scratch scratch = scratchBelow(ctx);
            putText(rdram, scratch.at, kSelectedIndex);
            writeU32(rdram, scratch.at + 0x40u, 0u);
            runtime->callGuestFunction(rdram, ctx, sym::bmx::copyVariable, {list, scratch.at, scratch.at + 0x40u}, scratch.stack);
            if (readU32(rdram, scratch.at + 0x40u) != kGalleryRows)
            {
                st.gallerySelection(rdram, ctx, runtime);
                return;
            }
            putText(rdram, scratch.at, kLogoMovie);
            runtime->callGuestFunction(rdram, ctx, sym::bmx::setVariableValue, {out, scratch.at}, scratch.stack);
            std::cout << kTag << " gallery: " << kLogoMovie << std::endl;
            returnToCaller(ctx);
        }

        // bmxmenuscriptGetLevelNames(list): the game's eight lines, the
        // GameCube's four after them (setVariableValue's text while it runs),
        // their items in the table's spare places.
        void levelNamesHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            if (uint8_t *items = getMemPtr(rdram, sym::bmx::g_levelSelectLevels))
                for (const GcLevel &level : kGcLevels)
                    if (level.level >= kListedLevels && level.level < kLevelSelectItems &&
                        (items[level.level] == 0 || items[level.level] == level.level))
                        items[level.level] = level.level; // (item 8 is the game's own: the Secret Level)
            st.listingLevels = true;
            st.levelNames(rdram, ctx, runtime);
            st.listingLevels = false;
        }

        // setVariableValue(variable, text): while the level select's list is
        // made, its text with the GameCube's levels' lines.
        void setVariableValueHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            char *text = st.listingLevels ? reinterpret_cast<char *>(getMemPtr(rdram, getRegU32(ctx, kRegA1))) : nullptr;
            if (text)
            {
                std::string lines(text, strnlen(text, kLevelNamesSize));
                for (const GcLevel &level : kGcLevels)
                {
                    const uint32_t title = readU32(rdram, sym::bmx::g_levels + level.level * sizeof(LevelTitle) +
                                                              offsetof(LevelTitle, title));
                    const char *t = title ? reinterpret_cast<const char *>(getConstMemPtr(rdram, title)) : nullptr;
                    if (t)
                        lines += std::string(lines.empty() ? "" : kListBreak) + std::string(t, strnlen(t, 64));
                }
                if (lines.size() < kLevelNamesSize)
                    std::memcpy(text, lines.c_str(), lines.size() + 1);
                st.listingLevels = false;
            }
            st.setVariableValue(rdram, ctx, runtime);
        }

        // objectiveCue(kind, first, second): traced (developers).
        void objectiveCueHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const uint32_t kind = getRegU32(ctx, kRegA0);
            const uint32_t first = getRegU32(ctx, kRegA1);
            if (kind == kCueSound)
            {
                auto it = st.phrases.find(first);
                std::cout << kTag << " challenge sound cue 0x" << std::hex << first << std::dec << " "
                          << (it != st.phrases.end() ? it->second : std::string("(not a release script line)")) << std::endl;
            }
            st.objectiveCue(rdram, ctx, runtime);
        }

        // playSpeech(phrase, position): traced (developers): the speech bank's
        // playlist found for a cue, and the speech channel's answer.
        void playSpeechHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const uint32_t phrase = getRegU32(ctx, kRegA0);
            st.playSpeech(rdram, ctx, runtime);
            std::cout << kTag << " speech: playlist 0x" << std::hex << phrase << " said, answer 0x" << getRegU32(ctx, kRegV0)
                      << std::dec << std::endl;
        }

        // saveParkData(...): a park written into the save buffer; its pieces
        // of the other releases counted afresh.
        void saveParkDataHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.savedExtraPieces = 0;
            st.saveParkData(rdram, ctx, runtime);
        }

        // editObjectSaveExtra(part, stream): a plain piece's number written;
        // counted when it is one of the other releases'.
        void editObjectSaveExtraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            const uint32_t part = getRegU32(ctx, kRegA0);
            const uint32_t pieceDef = readU32(rdram, part + offsetof(EditPiece, pieceDef));
            uint32_t index = 0;
            if (pieceDef != 0 &&
                runtime->callGuestFunction(rdram, ctx, sym::bmx::pieceDefIndex, {pieceDef}, scratchBelow(ctx).stack, &index) &&
                index >= kPs2Pieces)
                ++st.savedExtraPieces;
            st.editObjectSaveExtra(rdram, ctx, runtime);
        }

        fs::path piecesRecord(const fs::path &park)
        {
            fs::path record = park;
            return record.replace_extension(kPiecesRecord);
        }

        // A saved park (Saves/Parks) recorded as holding the other releases'
        // pieces, or "".
        std::string parkNeedingPieces()
        {
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(zge_host::saves::folder() / "Parks", ec))
            {
                const fs::path &path = entry.path();
                if (path.extension() == kPiecesRecord && fs::exists(fs::path(path).replace_extension(".sav"), ec))
                    return path.stem().string();
            }
            return {};
        }

        // The phrase names of the release scripts' AUDIO lines, for the trace.
        void readPhrases(State &st)
        {
            for (const char *level : kChallengeLevels)
            {
                std::ifstream in(st.dir / "challenges" / "xbox" / level / "OBJCTV.TXT", std::ios::binary);
                std::string line;
                while (std::getline(in, line))
                {
                    const size_t audio = line.find("AUDIO");
                    const size_t open = line.find('"', audio);
                    const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
                    if (audio == std::string::npos || close == std::string::npos)
                        continue;
                    const std::string phrase = line.substr(open + 1, close - open - 1);
                    st.phrases[zx::zaxis_text::keyHash(phrase)] = std::string(level) + " " + phrase;
                }
            }
        }

        bool wrap(PS2Runtime &runtime, uint32_t function, PS2Runtime::RecompiledFunction &original,
                  PS2Runtime::RecompiledFunction hook, const char *what)
        {
            original = runtime.lookupFunction(function);
            if (original && runtime.replaceFunction(function, hook))
                return true;
            std::cerr << kTag << " " << what << " not hooked" << std::endl;
            original = nullptr;
            return false;
        }
    }
}

namespace bmx_xxx
{
    void releasesParkSaved(const std::filesystem::path &file)
    {
        using namespace releases;
        State &st = state();
        if (!st.editorPieces || !st.saveParkData)
            return;
        const fs::path record = piecesRecord(file);
        std::error_code ec;
        if (st.savedExtraPieces == 0)
        {
            fs::remove(record, ec); // (an older park of that number's)
            return;
        }
        std::ofstream out(record, std::ios::binary | std::ios::trunc);
        out << st.savedExtraPieces << " of its pieces are the Pool or the Receptacle (the Xbox and GameCube "
            << "releases' park editor pieces): BMX XXX turns them on to load it.\n";
        std::cout << kTag << " park editor: " << file.filename().string() << " holds " << st.savedExtraPieces
                  << " of the other releases' pieces (" << record.filename().string() << ")" << std::endl;
    }

    zge_host::sound::Replacement releasesSong(const zge_host::sound::StreamSource &source)
    {
        using namespace releases;
        zge_host::sound::Replacement replacement;
        State &st = state();
        if (!st.songs || source.bus != zge_host::sound::Music || !source.file)
            return replacement;
        for (size_t i = 0; i < st.songList.size(); ++i)
        {
            const Song &song = st.songList[i];
            if (song.start == 0 || song.start != source.start)
                continue;
            std::string error;
            replacement.reader = zx::audio_file::open(song.file, &error);
            replacement.ownRate = true;
            if (replacement.reader)
                std::cout << kTag << " songs: track " << st.gameSongs + i << ", " << song.title << " ("
                          << song.file.filename().string() << ")" << std::endl;
            else
                std::cerr << kTag << " songs: cannot play " << song.file.string() << ": " << error << std::endl;
            break;
        }
        return replacement;
    }

    void installReleases(PS2Runtime &runtime)
    {
        using namespace releases;
        State &st = state();
        st.trace = on("ZX_TRACE_BMX_RELEASES");
        st.introMovie = on("ZX_BMX_INTRO_MOVIE"); // the PS2 disc's own movie
        const char *dir = zx::boot::text("ZX_BMX_RELEASES_DIR");
        std::error_code ec;
        const bool releases = dir && dir[0] && fs::is_directory(dir, ec);
        if (releases)
            st.dir = dir;
        const fs::path movies = st.dir / "movies" / "ASSETS" / "MOVIES";
        st.logoSplash = releases && on("ZX_BMX_LOGO_SPLASH") &&
                        (fs::exists(movies / "ZAXIS.PSS", ec) || (fs::exists(movies / "ZAXIS.XMV", ec) && hostMovies(movies, "ASSETS/MOVIES/", ".PSS")));
        if ((st.introMovie || st.logoSplash) && wrap(runtime, sym::bmx::introMenu, st.introMenu, &introMenuHook, "introMenu") &&
            st.logoSplash)
            wrap(runtime, sym::bmx::introFrame, st.introFrame, &introFrameHook, "introFrame");
        if (!releases)
            return; // an app built without the other releases' discs
        st.challenges = on("ZX_BMX_XBOX_CHALLENGES") && fs::is_directory(st.dir / "challenges", ec);
        const char *tuning = zx::boot::text("ZX_TUNING");
        st.xboxTimers = tuning && std::string(tuning) == "xbox";
        if (const char *language = zx::boot::text("ZX_LANGUAGE"))
            for (const Language &l : kLanguages)
                if (std::string(language) == l.setting)
                    st.language = &l;
        const bool piecesFile = fs::exists(st.dir / "editor" / "OBJECT.OBJ", ec);
        st.editorPieces = on("ZX_BMX_EDITOR_PIECES") && piecesFile;
        if (!st.editorPieces && piecesFile)
            if (const std::string park = parkNeedingPieces(); !park.empty())
            {
                // A park saved with them loads as it was made, the setting off or not.
                st.editorPieces = true;
                std::cout << kTag << " park editor: " << park
                          << " holds the Pool or the Receptacle: the two pieces are on for this boot" << std::endl;
            }
        st.previews = on("ZX_BMX_LEVEL_PREVIEWS") && fs::is_directory(movies / "MENU", ec) &&
                      (hasFileOf(movies / "MENU", ".PSM") || hostMovies(movies / "MENU", "ASSETS/MOVIES/MENU/", ".PSM"));
        st.logoMovie = on("ZX_BMX_LOGO_MOVIE") &&
                       (fs::exists(movies / "ZAXIS.PSS", ec) || (fs::exists(movies / "ZAXIS.XMV", ec) && hostMovies(movies, "ASSETS/MOVIES/", ".PSS")));
        st.gcLevels = on("ZX_BMX_GC_LEVELS") && fs::is_directory(st.dir / "levels" / "ASSETS" / "LEVELS", ec);
        st.songs = on("ZX_BMX_XBOX_SONGS") && readSongs(st);
        st.trailers = on("ZX_BMX_TRAILERS") && fs::is_directory(movies / "COMPEDGE", ec) &&
                      (hasFileOf(movies / "COMPEDGE", ".PSS") || hostMovies(movies / "COMPEDGE", "ASSETS/MOVIES/COMPEDGE/", ".PSS"));
        if (fs::exists(st.dir / "create" / kAddedTextures, ec))
        {
            st.greenSkin = on("ZX_BMX_GREEN_SKIN");
            st.topless = on("ZX_BMX_TOPLESS");
            st.createListed = on("ZX_BMX_CAR_TEXTURES");
            st.createFiles = st.createListed || st.greenSkin || st.topless;
            readAddedTextures(st);
            if (!st.createFiles)
                if (const std::string save = saveNamingAddedTexture(st); !save.empty())
                {
                    // A rider made with them loads as it was made, the settings off or not.
                    st.createFiles = true;
                    std::cout << kTag << " create-a-rider: " << save
                              << " has a rider made with the other releases' textures: their libraries are served for this boot"
                              << std::endl;
                }
        }

        if (st.editorPieces)
        {
            std::cout << kTag << " park editor: the Pool and the Receptacle" << std::endl;
            wrap(runtime, sym::bmx::saveParkData, st.saveParkData, &saveParkDataHook, "saveParkData");
            wrap(runtime, sym::bmx::editObjectSaveExtra, st.editObjectSaveExtra, &editObjectSaveExtraHook,
                 "editObjectSaveExtra");
        }
        if (st.challenges || st.editorPieces || st.previews || st.logoMovie || st.logoSplash || st.gcLevels || st.trailers ||
            st.createFiles)
            gf::registerProvider("ASSETS/", &provide, kOwner);
        if (st.trailers)
            zx::disc_patch::registerPatch("UILIB.ZAL", &patchTrailers, kOwner);
        if (st.songs)
        {
            gf::registerHead(kMusicBank, &musicBankHead, kOwner);
            zx::disc_patch::registerPatch("STRING.ZAL", &patchSongTexts, kOwner);
        }
        if (st.createFiles && wrap(runtime, sym::bmx::createAModelGetFilteredListOfAssetsInLibrary, st.assetList, &assetListHook,
                                   "the create-a-rider lists") &&
            wrap(runtime, sym::bmx::currentAssetName, st.currentAssetName, &currentAssetNameHook, "currentAssetName"))
        {
            size_t listed = 0;
            for (const auto &[name, shown] : st.addedTextures)
                listed += shown ? 1 : 0;
            std::cout << kTag << " create-a-rider: the other releases' libraries, " << listed << " of their "
                      << st.addedTextures.size() << " added textures listed (green skin " << (st.greenSkin ? "on" : "off")
                      << ", topless girl " << (st.topless ? "on" : "off") << ")" << std::endl;
        }
        if (st.gcLevels && wrap(runtime, sym::bmx::bmxmenuscriptGetLevelNames, st.levelNames, &levelNamesHook, "the level select's list") &&
            wrap(runtime, sym::bmx::setVariableValue, st.setVariableValue, &setVariableValueHook, "setVariableValue"))
        {
            std::cout << kTag << " level select: the GameCube's Secret Level and three test levels (items 9-12)" << std::endl;
            zx::disc_patch::registerPatch("UILIB.ZAL", &patchGcLevelItems, kOwner);
        }
        if (st.challenges)
        {
            std::cout << kTag << " challenge scripts: the Xbox and GameCube releases' (" << std::size(kChallengeLevels)
                      << " levels), " << (st.xboxTimers ? "their" : "the PS2's") << " timers" << std::endl;
            if (st.trace)
            {
                readPhrases(st);
                wrap(runtime, sym::bmx::objectiveCue, st.objectiveCue, &objectiveCueHook, "objectiveCue");
                wrap(runtime, sym::bmx::playSpeech, st.playSpeech, &playSpeechHook, "playSpeech");
            }
        }
        if (st.language && wrap(runtime, sym::bmx::setLanguage, st.setLanguage, &setLanguageHook, "setLanguage"))
        {
            std::cout << kTag << " language: " << st.language->setting << std::endl;
            zx::disc_patch::registerPatch("STRING.ZAL", &patchStrings, kOwner);
        }
        if (st.previews)
            zx::disc_patch::registerPatch("UILIB.ZAL", &patchLevelSelect, kOwner);
        if (st.logoMovie)
        {
            wrap(runtime, sym::bmx::bmxmenuscriptPopulateMediaViewerMenu, st.populateGallery, &populateGalleryHook,
                 "the gallery's list");
            wrap(runtime, sym::bmx::bmxmenuscriptGetSelectedMediaFileName, st.gallerySelection, &gallerySelectionHook,
                 "the gallery's selection");
        }
    }
}
