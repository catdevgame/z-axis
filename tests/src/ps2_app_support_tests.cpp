#include "MiniTest.h"
#include "collection/settings.h"
#include "collection/disc_check.h"
#include "collection/card_import.h"
#include "collection/levels/convert.h"
#include "collection/levels/export.h"
#include "collection/levels/geometry.h"
#include "collection/levels/scene.h"
#include "collection/maps.h"
#include "collection/mods.h"
#include "collection/offered.h"
#include "runtime/ps2_game_files.h"
#include "test_zip_builder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ps2x::app;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("ps2x-app-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    GetEnv envFrom(std::map<std::string, std::string> values)
    {
        auto shared = std::make_shared<std::map<std::string, std::string>>(std::move(values));
        return [shared](const char *name) -> const char *
        {
            const auto it = shared->find(name);
            return it == shared->end() ? nullptr : it->second.c_str();
        };
    }

    std::vector<uint8_t> fakeElf(uint8_t fill)
    {
        std::vector<uint8_t> elf(3000u, fill);
        const uint8_t magic[] = {0x7F, 'E', 'L', 'F'};
        std::memcpy(elf.data(), magic, sizeof(magic));
        return elf;
    }

    // A folder of disc files, as tools/make-app.sh extracts them.
    std::filesystem::path writeDiscFolder(const std::filesystem::path &dir, const std::string &name, const std::string &cnf,
                                          const std::string &elfName, const std::vector<uint8_t> &elf)
    {
        const auto path = dir / name;
        std::filesystem::create_directories(path / "ASSETS");
        if (!cnf.empty())
            std::ofstream(path / "SYSTEM.CNF", std::ios::binary) << cnf;
        if (!elfName.empty())
            std::ofstream(path / elfName, std::ios::binary).write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        std::ofstream(path / "ASSETS" / "A.BIN", std::ios::binary) << "a";
        return path;
    }

    // A small PCSX2-style card image without ECC: 512-byte pages, 2 pages per
    // cluster, indirect FAT in cluster 1, FAT in cluster 2, allocatable
    // clusters from 8. Root (relative 0-1): ".", "..", the save dir; save dir
    // (2-3): ".", "..", two files at 4-5 and 6.
    std::vector<uint8_t> makeCard(const std::string &saveDir, const std::vector<uint8_t> &big, const std::string &small,
                                  bool deleted = false, const std::string &bigName = "BIG")
    {
        constexpr uint32_t kCluster = 1024u;
        constexpr uint32_t kAllocOffset = 8u;
        std::vector<uint8_t> card(64u * kCluster, 0u);
        auto put32 = [&](size_t at, uint32_t v)
        { std::memcpy(card.data() + at, &v, 4); };
        auto put16 = [&](size_t at, uint16_t v)
        { std::memcpy(card.data() + at, &v, 2); };
        std::memcpy(card.data(), "Sony PS2 Memory Card Format 1.2.0.0", 35);
        put16(0x28, 512u);
        put16(0x2A, 2u);
        put16(0x2C, 16u);
        put32(0x30, 64u);
        put32(0x34, kAllocOffset);
        put32(0x38, 56u);
        put32(0x3C, 0u);
        put32(0x50, 1u);
        put32(1u * kCluster, 2u); // indirect -> FAT cluster 2
        const size_t fat = 2u * kCluster;
        for (uint32_t i = 0; i < 56u; ++i)
            put32(fat + i * 4u, 0x7FFFFFFFu);
        put32(fat + 0 * 4u, 0x80000001u);
        put32(fat + 1 * 4u, 0xFFFFFFFFu);
        put32(fat + 2 * 4u, 0x80000003u);
        put32(fat + 3 * 4u, 0xFFFFFFFFu);
        put32(fat + 4 * 4u, 0x80000005u);
        put32(fat + 5 * 4u, 0xFFFFFFFFu);
        put32(fat + 6 * 4u, 0xFFFFFFFFu);

        auto entry = [&](uint32_t relCluster, uint32_t index, uint16_t mode, uint32_t length, uint32_t cluster, const std::string &name)
        {
            // Entries are 512 bytes, two per cluster.
            const size_t at = (kAllocOffset + relCluster + index / 2u) * kCluster + (index % 2u) * 512u;
            put16(at, mode);
            put32(at + 4, length);
            put32(at + 0x10, cluster);
            std::memcpy(card.data() + at + 0x40, name.data(), name.size());
        };
        const uint16_t dirMode = 0x8427u;
        const uint16_t fileMode = 0x8497u;
        entry(0, 0, dirMode, 3, 0, ".");
        entry(0, 1, dirMode, 0, 0, "..");
        entry(0, 2, deleted ? static_cast<uint16_t>(dirMode & 0x7FFFu) : dirMode, 4, 2, saveDir);
        entry(2, 0, dirMode, 4, 0, ".");
        entry(2, 1, dirMode, 0, 0, "..");
        entry(2, 2, fileMode, static_cast<uint32_t>(big.size()), 4, bigName);
        entry(2, 3, fileMode, static_cast<uint32_t>(small.size()), 6, "icon.sys");
        std::memcpy(card.data() + (kAllocOffset + 4u) * kCluster, big.data(), big.size());
        std::memcpy(card.data() + (kAllocOffset + 6u) * kCluster, small.data(), small.size());
        return card;
    }

    void writeFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<uint8_t> readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}

void register_ps2_app_support_tests()
{
    MiniTest::Case("AppSupport", [](TestCase &tc)
    {
        tc.Run("app paths: platform defaults and the scratch override", [](TestCase &t)
        {
            const AppPaths overridden = resolveAppPaths("Aggressive Inline", envFrom({{"HOME", "/Users/x"}, {"PS2X_APP_SUPPORT_DIR", "/tmp/scratch"}}));
            t.Equals(overridden.supportDir, std::filesystem::path("/tmp/scratch"), "override replaces the support folder");
            t.Equals(overridden.settingsFile, std::filesystem::path("/tmp/scratch/settings.ini"), "settings.ini inside it");
            t.Equals(overridden.memcardSlot1, std::filesystem::path("/tmp/scratch/memcards/slot1"), "slot1");
            t.Equals(overridden.memcardSlot2, std::filesystem::path("/tmp/scratch/memcards/slot2"), "slot2");
            t.Equals(overridden.controlsFile, std::filesystem::path("/tmp/scratch/controls.ini"), "controls.ini");
            t.Equals(overridden.logDir, std::filesystem::path("/tmp/scratch/Logs"), "logs stay inside the override");

            const AppPaths logs = resolveAppPaths("Aggressive Inline", envFrom({{"HOME", "/Users/x"}, {"PS2X_APP_SUPPORT_DIR", "/tmp/s"}, {"PS2X_APP_LOG_DIR", "/tmp/l"}}));
            t.Equals(logs.logDir, std::filesystem::path("/tmp/l"), "log override");
#if defined(__APPLE__)
            const AppPaths mac = resolveAppPaths("Aggressive Inline", envFrom({{"HOME", "/Users/x"}}));
            t.Equals(mac.supportDir, std::filesystem::path("/Users/x/Library/Application Support/Aggressive Inline"), "macOS support folder");
            t.Equals(mac.logDir, std::filesystem::path("/Users/x/Library/Logs/Aggressive Inline"), "macOS log folder");
#endif
        });

        tc.Run("settings round-trip and tolerate bad lines", [](TestCase &t)
        {
            Settings s;
            s.fullscreen = true;
            s.windowWidth = 1600;
            s.windowHeight = 1200;
            s.scaling = Scaling::Integer;
            s.widescreen = true;
            s.gpuRenderer = false;
            s.internalResolution = 2;
            s.pauseOnFocusLoss = false;
            s.logFile = false;
            s.pcsx2ImportOffered = true;
            s.fullscreenMode = FullscreenMode::Borderless;
            s.vsync = false;
            s.showFps = true;
            s.volume = 35;
            s.muteOnFocusLoss = true;
            s.skipIntroMovies = true;
            s.xboxTextures = false;
            s.xboxMovies = false;
            s.mapWindow = true;
            s.mapWindowX = 40;
            s.mapWindowY = 60;
            s.mapWindowWidth = 930;
            s.mapWindowHeight = 810;
            std::vector<std::string> warnings;
            const Settings back = parseSettings(formatSettings(s), &warnings);
            t.IsTrue(warnings.empty(), "formatted settings parse without warnings");
            t.IsTrue(back.fullscreen && back.windowWidth == 1600 && back.windowHeight == 1200, "display");
            t.IsTrue(back.scaling == Scaling::Integer, "scaling");
            t.IsTrue(back.widescreen, "widescreen");
            t.IsTrue(!back.gpuRenderer && back.internalResolution == 2, "renderer settings");
            t.IsTrue(!back.pauseOnFocusLoss && !back.logFile && back.pcsx2ImportOffered, "flags");
            t.IsTrue(back.fullscreenMode == FullscreenMode::Borderless && !back.vsync && back.showFps, "display extras");
            t.IsTrue(back.volume == 35 && back.muteOnFocusLoss && back.skipIntroMovies, "audio and game");
            t.IsTrue(!back.xboxTextures && !back.xboxMovies, "Xbox textures and movies");
            t.IsTrue(back.mapWindow && back.mapWindowX == 40 && back.mapWindowY == 60 && back.mapWindowWidth == 930 &&
                         back.mapWindowHeight == 810,
                     "the map window, its place and size");
            t.IsTrue(!parseSettings("", nullptr).mapWindow && parseSettings("", nullptr).mapWindowX == -1 &&
                         parseSettings("", nullptr).mapWindowWidth == 620,
                     "the map window off by default, placed by the system, half the screen's size");
            t.IsTrue(parseSettings("", nullptr).xboxTextures && parseSettings("", nullptr).xboxMovies,
                     "Xbox textures and movies default on (used when the app has them)");
            const Settings plain = parseSettings("", nullptr);
            t.IsTrue(plain.fullscreenMode == FullscreenMode::Native && plain.vsync && !plain.showFps && plain.volume == 100 &&
                         !plain.muteOnFocusLoss && !plain.skipIntroMovies,
                     "new keys default to the old behaviour");
            warnings.clear();
            const Settings loud = parseSettings("[audio]\nvolume = 150\n", &warnings);
            t.IsTrue(warnings.size() == 1u && loud.volume == 100, "volume past 100 is refused");
            warnings.clear();
            t.IsTrue(plain.hostDisc && !parseSettings("[host]\ndisc = ps2\n", &warnings).hostDisc && warnings.empty() &&
                         parseSettings(formatSettings(parseSettings("[host]\ndisc = ps2\n", nullptr), gameSettingsProfile("ai"),
                                                      SettingsScope::App),
                                       nullptr)
                                 .hostDisc == false,
                     "[host] disc: native by default, ps2 read and written back");

            warnings.clear();
            const Settings odd = parseSettings("[display]\nscaling = blurry\nwindow_width = 10\nfullscreen = YES\nbogus\n[other]\nx = 1\n", &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(4u), "bad value, too small, missing '=', unknown key");
            t.IsTrue(odd.scaling == Scaling::Smooth && odd.windowWidth == 1024 && odd.fullscreen, "bad values keep defaults");

            // The app saves a window of at least 320 x 240 (a smaller one as that): what it writes reads back.
            warnings.clear();
            const Settings small = parseSettings("[display]\nwindow_width = 320\nwindow_height = 240\n", &warnings);
            t.IsTrue(warnings.empty() && small.windowWidth == 320 && small.windowHeight == 240, "the smallest saved window reads back");
            warnings.clear();
            const Settings tiny = parseSettings("[display]\nwindow_height = 200\n", &warnings);
            t.IsTrue(warnings.size() == 1u && tiny.windowHeight == 768, "a height under 240 is refused");

            warnings.clear();
            const Settings gfx = parseSettings("[graphics]\nrenderer = CPU\ninternal_resolution = 5\n", &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(1u), "internal resolution past 4");
            t.IsTrue(!gfx.gpuRenderer && gfx.internalResolution == 3, "renderer read, bad resolution keeps the default");
            const Settings fresh = parseSettings("", nullptr);
            t.IsTrue(fresh.gpuRenderer && fresh.internalResolution == 3, "default renderer: GPU at 3x");
            t.IsTrue(fresh.fullHeight && fresh.wideAspect == 0 && fresh.motionBlur == 2 && fresh.drawDistance == -1,
                     "defaults: full height, 16:9, the game's motion blur, the game's own draw distance");
            t.IsTrue(drawDistanceOf(fresh, gameSettingsProfile("ai")) == 0 && drawDistanceOf(fresh, gameSettingsProfile("dm2")) == 0 &&
                         drawDistanceOf(fresh, gameSettingsProfile("bmx")) == 0,
                     "the draw distance by default: Original in all three (it changes each game)");
            const Settings legacy = parseSettings("[graphics]\ndraw_distance = farthest\n", nullptr);
            t.IsTrue(legacy.drawDistance == -1 && drawDistanceOf(legacy, gameSettingsProfile("ai")) == 2 &&
                         drawDistanceOf(legacy, gameSettingsProfile("bmx")) == 2 && drawDistanceOf(legacy, gameSettingsProfile("dm2")) == 2,
                     "an earlier version's app-wide pick: every game's");
            const Settings legacyFar = parseSettings("[graphics]\ndraw_distance = far\n", nullptr);
            t.IsTrue(drawDistanceOf(legacyFar, gameSettingsProfile("ai")) == 0 && drawDistanceOf(legacyFar, gameSettingsProfile("bmx")) == 0,
                     "an earlier version's Far, its default, is no pick: the game's default");

            warnings.clear();
            const Settings view = parseSettings("[display]\naspect = 21:9\n[graphics]\nfull_height = false\nmotion_blur = OFF\n"
                                                "[gameplay]\ndraw_distance = farthest\n",
                                                &warnings);
            t.IsTrue(warnings.empty() && view.wideAspect == 2 && !view.fullHeight && view.motionBlur == 0 &&
                         view.drawDistance == 2,
                     "aspect, full height, motion blur and draw distance read");
            const Settings viewBack = parseSettings(formatSettings(view), nullptr);
            t.IsTrue(viewBack.wideAspect == 2 && !viewBack.fullHeight && viewBack.motionBlur == 0 && viewBack.drawDistance == 2,
                     "and written back");
            warnings.clear();
            const Settings badView = parseSettings("[display]\naspect = 5:4\n[graphics]\nmotion_blur = max\n", &warnings);
            t.IsTrue(warnings.size() == 2u && badView.wideAspect == 0 && badView.motionBlur == 2, "unknown names keep the defaults");

            warnings.clear();
            const Settings game = parseSettings("[general]\nlanguage = French\nfrench_voices = false\ndeveloper_menu = true\n", &warnings);
            t.IsTrue(warnings.empty() && game.language == 1 && game.frenchVoices == 0 && game.developerMenu,
                     "language, French voices and developer menu read");
            const Settings gameBack = parseSettings(formatSettings(game), nullptr);
            t.IsTrue(gameBack.language == 1 && gameBack.frenchVoices == 0 && gameBack.developerMenu, "and written back");
            t.IsTrue(fresh.language == 0 && fresh.frenchVoices == -1 && !fresh.developerMenu,
                     "defaults: English, French voices with French, no developer menu");
            t.IsTrue(parseSettings("[general]\nlanguage = uk\n", nullptr).language == 0 &&
                         parseSettings("[general]\nlanguage = spanish\n", nullptr).language == 3,
                     "earlier names read as English; Spanish");
            t.IsTrue(parseSettings(formatSettings(fresh), nullptr).frenchVoices == -1, "auto written back");
            t.IsTrue(fresh.zoo, "the Zoo is on by default");
            warnings.clear();
            const Settings noZoo = parseSettings("[extras]\nzoo = false\n", &warnings);
            t.IsTrue(warnings.empty() && !noZoo.zoo, "[extras] zoo read");
            t.IsTrue(!parseSettings(formatSettings(noZoo), nullptr).zoo, "and written back");
            t.IsTrue(fresh.ape, "the Ape is on by default");
            warnings.clear();
            const Settings noApe = parseSettings("[extras]\nape = false\n", &warnings);
            t.IsTrue(warnings.empty() && !noApe.ape && noApe.zoo, "[extras] ape read");
            t.IsTrue(!parseSettings(formatSettings(noApe), nullptr).ape, "and written back");
            warnings.clear();
            const Settings retired = parseSettings("[extras]\ndm2_levels = true\ndm2_rider = career\n"
                                                   "dm2_rider_split = both\nai_levels = true\n", &warnings);
            t.IsTrue(warnings.empty() && retired.zoo, "the cross-game combos' settings of an older file read without a warning");
            t.IsTrue(formatSettings(retired).find("dm2_rider") == std::string::npos, "and not written back");
            t.IsTrue(fresh.dm2XboxLevels, "the DM2 Xbox levels on by default (each game the superset of its releases)");
            warnings.clear();
            const Settings withXbox = parseSettings("[extras]\ndm2_xbox_levels = false\n", &warnings);
            t.IsTrue(warnings.empty() && !withXbox.dm2XboxLevels, "[extras] dm2_xbox_levels read");
            t.IsTrue(!parseSettings(formatSettings(withXbox), nullptr).dm2XboxLevels, "and written back");
            t.IsTrue(fresh.xboxFreeSkate, "the Xbox free skate scripts on by default");
            t.IsTrue(!parseSettings(formatSettings(parseSettings("[extras]\nxbox_free_skate = false\n", nullptr)), nullptr)
                          .xboxFreeSkate,
                     "[extras] xbox_free_skate read and written back");
            t.IsTrue(fresh.tuning == 0, "the PS2 release's balance by default");
            t.IsTrue(fresh.careerScripts == 1, "the Xbox career scripts by default");
            t.IsTrue(fresh.zooUnlock == 1, "the Zoo always open by default");
            t.IsTrue(parseSettings(formatSettings(parseSettings("[extras]\nzoo_unlock = museum\n", nullptr)), nullptr)
                             .zooUnlock == 0,
                     "[extras] zoo_unlock read and written back");
            t.IsTrue(parseSettings(formatSettings(parseSettings("[gameplay]\ncareer_scripts = ps2\n", nullptr)), nullptr)
                             .careerScripts == 0,
                     "[gameplay] career_scripts read and written back");
            warnings.clear();
            const Settings xboxTuning = parseSettings("[gameplay]\ntuning = xbox\n", &warnings);
            t.IsTrue(warnings.empty() && xboxTuning.tuning == 1, "[gameplay] tuning read");
            t.IsTrue(parseSettings(formatSettings(xboxTuning), nullptr).tuning == 1, "and written back");
            warnings.clear();
            t.IsTrue(parseSettings("[gameplay]\ntuning = gamecube\n", &warnings).tuning == 0 && warnings.size() == 1u,
                     "an unknown tuning keeps the PS2's");
            warnings.clear();
            const Settings badGame = parseSettings("[general]\nlanguage = klingon\n", &warnings);
            t.IsTrue(warnings.size() == 1u && badGame.language == 0, "an unknown language keeps US English");

            warnings.clear();
            const Settings old = parseSettings("[disc]\n# Your disc image\nimage = /Volumes/Games/Aggressive Inline (USA).iso\n[display]\nwidescreen = true\n", &warnings);
            t.IsTrue(warnings.empty() && old.widescreen, "an earlier version's disc image line is ignored quietly");
            t.IsTrue(formatSettings(old).find("[disc]") == std::string::npos, "and not written back");

            TempDir dir;
            const auto file = dir.path / "sub" / "settings.ini";
            t.IsTrue(saveSettings(file, s), "save creates the folder");
            Settings loaded;
            t.IsTrue(loadSettings(file, loaded) && loaded.windowWidth == 1600 && loaded.widescreen, "load");
            t.IsFalse(std::filesystem::exists(dir.path / "sub" / "settings.ini.tmp"), "no temporary file left");
            t.IsFalse(loadSettings(dir.path / "missing.ini", loaded), "missing file");
        });

        tc.Run("offered: what a setup has, found from its folders (none, all, the GameCube disc alone)", [](TestCase &t)
        {
            auto put = [](const std::filesystem::path &file)
            {
                std::filesystem::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary) << "x";
            };
            const std::vector<Need> discs = {
                Need::XboxText, Need::XboxSpeech, Need::XboxLevelScripts, Need::XboxFreeSkate, Need::XboxApe,
                Need::XboxTextures, Need::XboxSkaters, Need::XboxMovies, Need::Estate, Need::Dm2XboxLevels,
                Need::Dm2XboxThemes, Need::Dm2RetroMovie, Need::BmxText, Need::BmxChallenges, Need::BmxEditorPieces,
                Need::BmxLevelPreviews, Need::BmxLogoMovie, Need::BmxGcLevels};

            TempDir empty;
            BootFolders nothing;
            nothing.extras = empty.path / "extras";
            nothing.resources = empty.path / "resources";
            const Installed none = findInstalled(nothing);
            t.IsTrue(releaseFolders(nothing).empty(), "no extras, nothing bundled: no folders given");
            bool anyDisc = false;
            for (Need need : discs)
                anyDisc = anyDisc || none[need];
            t.IsTrue(!anyDisc && none[Need::Nothing] && !none[Need::Dm2ReadableRider], "the PS2 discs alone: no extra's need met");
#if defined(__ANDROID__)
            t.IsTrue(!none[Need::SecondWindow] && !none[Need::WindowModes] && !none[Need::Borderless], "Android: one window, the whole screen");
#elif defined(__APPLE__)
            t.IsTrue(none[Need::SecondWindow] && none[Need::WindowModes] && none[Need::Borderless], "macOS: a map window, window modes, borderless");
#else
            t.IsTrue(none[Need::SecondWindow] && none[Need::WindowModes] && !none[Need::Borderless], "a map window and window modes, no borderless");
#endif

            // Every disc's extras, as the setup lays them out (names in the discs' own case).
            TempDir full;
            const std::filesystem::path x = full.path / "extras";
            put(x / "xbox" / "media" / "assets" / "string.fre");
            put(x / "xbox" / "media" / "assets" / "audio" / "speech01.zsd");
            put(x / "xbox" / "media" / "assets" / "uilib.zal");
            put(x / "xbox" / "media" / "assets" / "levels" / "civic" / "civic.bin");
            put(x / "xbox" / "media" / "assets" / "levels" / "brdwlk" / "brdwlkt.bin");
            put(x / "xbox-converted" / "ASSETS" / "SKATERS" / "GORILLA" / "GOR.ZIT");
            put(x / "xbox-pack" / "textures" / "a.xtp");
            put(x / "xbox-pack" / "models" / "a.zit");
            put(x / "xbox-pack" / "movies" / "a.bik");
            put(x / "estate" / "TUT.GEO");
            put(x / "xbox-levels" / "levels.txt");
            put(x / "xbox-levels" / "themes" / "DESERT.TXT");
            put(x / "xbox-levels" / "movies" / "ASSETS" / "MOVIE" / "RETRO.PSS");
            put(x / "releases" / "strings" / "FRENCH");
            put(x / "releases" / "challenges" / "objctv.txt");
            put(x / "releases" / "editor" / "OBJECT.OBJ");
            put(x / "releases" / "movies" / "ASSETS" / "MOVIES" / "MENU" / "BRONX.PSM");
            put(x / "releases" / "movies" / "ASSETS" / "MOVIES" / "ZAXIS.XMV");
            put(x / "releases" / "levels" / "ASSETS" / "LEVELS" / "CMP_SCRT" / "a.geo");
            BootFolders every;
            every.extras = x;
            every.resources = full.path / "resources";
            const Installed all = findInstalled(every);
            bool allDiscs = true;
            for (Need need : discs)
                allDiscs = allDiscs && all[need];
            t.IsTrue(allDiscs, "every extras disc set up: each need met, found in any case");
            t.Equals(releaseFolders(every).size(), size_t(6), "the six folders given");

            // The extras' folder over the bundled one; the bundled one alone counts too.
            put(every.resources / "estate" / "TUT.GEO");
            BootFolders bundled;
            bundled.resources = every.resources;
            const auto given = releaseFolders(bundled);
            t.IsTrue(given.size() == 1 && given[0].first == "PS2X_ESTATE_DIR" && findInstalled(bundled)[Need::Estate],
                     "an app built with the Estate: offered");
            const auto both = releaseFolders(every);
            t.IsTrue(std::any_of(both.begin(), both.end(), [&](const auto &f) { return f.first == "PS2X_ESTATE_DIR" && f.second == x / "estate"; }),
                     "the setup's extras before the bundled ones");

            // BMX XXX with its GameCube disc alone: no Xbox movies.
            TempDir gc;
            put(gc.path / "releases" / "strings" / "FRENCH");
            put(gc.path / "releases" / "challenges" / "objctv.txt");
            put(gc.path / "releases" / "editor" / "OBJECT.OBJ");
            put(gc.path / "releases" / "levels" / "ASSETS" / "LEVELS" / "CMP_SCRT" / "a.geo");
            BootFolders gcOnly;
            gcOnly.extras = gc.path;
            const Installed cube = findInstalled(gcOnly);
            t.IsTrue(cube[Need::BmxText] && cube[Need::BmxChallenges] && cube[Need::BmxEditorPieces] && cube[Need::BmxGcLevels] &&
                         !cube[Need::BmxLevelPreviews] && !cube[Need::BmxLogoMovie],
                     "the GameCube's content offered, the Xbox's movies not");

            // The boot's own names (gameBootConfig's), as the game's rows read them.
            const Installed booted = findInstalled(envFrom({{"PS2X_XBOX_DIR", (x / "xbox").string()},
                                                            {"PS2X_DM2_XBOX_LEVELS_DIR", (gc.path / "none").string()}}));
            t.IsTrue(booted[Need::XboxText] && booted[Need::XboxFreeSkate] && !booted[Need::XboxApe] && !booted[Need::Dm2XboxLevels],
                     "from the boot config: the Xbox files, no Ape without its model, a folder that is not there is none");
        });

        tc.Run("offered: each game's rows with no extras and with every extras disc", [](TestCase &t)
        {
            Installed none = platformInstalled();
            Installed all = platformInstalled();
            for (int i = 0; i < static_cast<int>(Need::Count); ++i)
                all.set(static_cast<Need>(i));
            const GameSettingsProfile &ai = gameSettingsProfile("ai");
            const GameSettingsProfile &dm2 = gameSettingsProfile("dm2");
            const GameSettingsProfile &bmx = gameSettingsProfile("bmx");
            // Every option of every game: with every extra a row for each one the game has, and only those.
            bool fullRows = true;
            for (const GameSettingsProfile *game : {&ai, &dm2, &bmx})
                for (int i = 0; i < static_cast<int>(GameOption::Count); ++i)
                {
                    const GameOption option = static_cast<GameOption>(i);
                    fullRows = fullRows && offered(*game, option, all) == game->has(option) &&
                               (offered(*game, option, none) == (game->has(option) && needOf(game->id, option) == Need::Nothing));
                }
            t.IsTrue(fullRows, "every extra: every row the game has; none: only those that need nothing");

            using O = GameOption;
            t.IsTrue(!offered(ai, O::Language, none) && !offered(ai, O::FrenchVoices, none) && !offered(ai, O::CareerScripts, none) &&
                         !offered(ai, O::XboxFreeSkate, none) && !offered(ai, O::Ape, none) && !offered(ai, O::Estate, none),
                     "AI with its PS2 disc alone: no Language, French Voices, Career Scripts, Xbox Free Skate, Ape, Estate");
            t.IsTrue(offered(ai, O::Zoo, none) && offered(ai, O::ZooUnlock, none) && offered(ai, O::Tuning, none) &&
                         offered(ai, O::DeveloperMenu, none),
                     "AI's Zoo (the PS2 disc's own), its unlock, its tuning (code) and developer menu stay");
            t.IsTrue(!offered(dm2, O::Dm2XboxLevels, none) && !offered(dm2, O::Dm2XboxThemes, none) &&
                         !offered(dm2, O::Dm2RetroMovie, none) && offered(dm2, O::Dm2AttractDemo, none),
                     "DM2 with its PS2 disc alone: no Xbox levels, themes or Retro movie; the attract demo (level 3's replay) stays");
            Installed rider = none;
            t.IsTrue(!offered(dm2, O::Dm2CreatePlayer, none), "DM2's Create Rider: not without the readable newRider");
            rider.set(Need::Dm2ReadableRider);
            t.IsTrue(offered(dm2, O::Dm2CreatePlayer, rider), "and offered with it, no disc needed");
            t.IsTrue(!offered(bmx, O::Language, none) && !offered(bmx, O::Tuning, none) && !offered(bmx, O::BmxXboxChallenges, none) &&
                         !offered(bmx, O::BmxEditorPieces, none) && !offered(bmx, O::BmxLevelPreviews, none) &&
                         !offered(bmx, O::BmxLogoMovie, none) && !offered(bmx, O::BmxGcLevels, none),
                     "BMX XXX with its PS2 disc alone: none of its other releases' rows (the tuning only times their challenges)");
            t.IsTrue(offered(bmx, O::BmxParkEditor, none) && offered(bmx, O::BmxMenuLines, none) && offered(bmx, O::BmxAllMpModes, none) &&
                         offered(bmx, O::BmxIntroMovie, none),
                     "BMX XXX's park editor, menu lines, multiplayer modes and intro movie (the PS2 disc's) stay");
            t.IsTrue(!offered(bmx, O::Zoo, all) && !offered(dm2, O::Language, all), "another game's option: never");
            t.IsTrue(needOf("ai", O::Language) == Need::XboxText && needOf("bmx", O::Language) == Need::BmxText,
                     "one option, each game's need");

            // The app's own rows: the Xbox pack's and the platform's.
            Installed android;
            t.IsTrue(!offered(Need::SecondWindow, android) && !offered(Need::WindowModes, android) && !offered(Need::Borderless, android),
                     "Android: no Map Window, no Display Mode");
            t.IsTrue(!offered(Need::XboxTextures, none) && offered(Need::XboxTextures, all), "Xbox Textures with the Xbox pack only");

            // Custom Freeplay: AI's start goes through the Estate.
            t.IsTrue(mapSlotOf("ai") && mapSlotOf("ai")->need == Need::Estate && mapSlotOf("dm2")->need == Need::Nothing &&
                         mapSlotOf("bmx")->need == Need::Nothing,
                     "AI's maps need the Estate (the GameCube disc); DM2's and BMX XXX's nothing");
        });

        tc.Run("mods: a tuning.ini only where a game reads one", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path shared = dir.path / "Mods";
            auto put = [](const std::filesystem::path &file, const std::string &text)
            {
                std::filesystem::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary) << text;
            };
            put(shared / "10-ai" / "mod.ini", "[mod]\nid = ai-tune\ngames = ai, dm2\n");
            put(shared / "10-ai" / "tuning.ini", "x = 1\n");
            put(shared / "20-dm2" / "mod.ini", "[mod]\nid = dm2-tune\ngames = dm2, bmx\n");
            put(shared / "20-dm2" / "tuning.ini", "x = 1\n");
            const std::vector<Mod> mods = listSharedMods(shared);
            t.Equals(mods.size(), size_t(2), "two mods");
            if (mods.size() != 2)
                return;
            t.IsTrue(mods[0].contents == kModTuning && mods[0].changesLogic() && mods[0].notes.empty(),
                     "for AI among others: tuning, changes game logic");
            t.IsTrue(mods[1].contents == 0u && !mods[1].changesLogic() && mods[1].notes.size() == 1 &&
                         mods[1].notes[0].find("tuning.ini not read") == 0,
                     "for DM2 and BMX XXX alone: no tuning, no logic changed, a note says why");
            TempDir own;
            put(own.path / "Mods" / "tune" / "tuning.ini", "x = 1\n");
            const std::vector<Mod> dm2Own = listMods(own.path / "Mods", {}, {}, "dm2");
            const std::vector<Mod> aiOwn = listMods(own.path / "Mods", {}, {}, "ai");
            t.IsTrue(dm2Own.size() == 1 && !dm2Own[0].changesLogic() && aiOwn.size() == 1 && aiOwn[0].changesLogic(),
                     "a game's own Mods folder: DM2's not logic, AI's logic");
        });

        tc.Run("sha256 matches the FIPS test vectors", [](TestCase &t)
        {
            t.Equals(sha256Hex(nullptr, 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty");
            const std::string abc = "abc";
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()),
                     std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc");
            const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(two.data()), two.size()),
                     std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "two blocks");
        });

        tc.Run("SYSTEM.CNF boot entry", [](TestCase &t)
        {
            t.Equals(bootElfFromSystemCnf("BOOT2=cdrom0:\\SLUS_203.27;1 \r\nVER=1.00 \r\nVMODE=NTSC \r\n"), std::string("SLUS_203.27"), "retail form");
            t.Equals(bootElfFromSystemCnf("boot2 = cdrom0:\\GAME\\MAIN.ELF;1\n"), std::string("GAME\\MAIN.ELF"), "subfolder, spaces");
            t.Equals(bootElfFromSystemCnf("BOOT = cdrom:\\PSX.EXE;1\n"), std::string(), "PS1 discs have no BOOT2");
        });

        tc.Run("disc folder check: right game, wrong game, wrong version, incomplete", [](TestCase &t)
        {
            TempDir dir;
            const auto elf = fakeElf(0x11);
            const std::string sha = sha256Hex(elf.data(), elf.size());
            const std::string cnf = "BOOT2 = cdrom0:\\SLUS_203.27;1\r\nVER = 1.00\r\n";

            const auto good = writeDiscFolder(dir.path, "good", cnf, "SLUS_203.27", elf);
            DiscCheck ok = checkDiscFolder(good, "Aggressive Inline", "SLUS_203.27", sha);
            t.IsTrue(ok.status == DiscCheck::Status::Ok, "the expected game passes: " + ok.message);
            t.IsTrue(ok.elf == elf && ok.elfSha256 == sha && ok.bootElf == "SLUS_203.27", "the ELF comes back for loading");
            t.IsTrue(checkDiscFolder(good, "Aggressive Inline", "slus_203.27", "").status == DiscCheck::Status::Ok, "name only, any case");

            const auto other = writeDiscFolder(dir.path, "other", "BOOT2 = cdrom0:\\SLES_501.23;1\r\n", "SLES_501.23", elf);
            const DiscCheck wrong = checkDiscFolder(other, "Aggressive Inline", "SLUS_203.27", sha);
            t.IsTrue(wrong.status == DiscCheck::Status::WrongGame, "another game is refused");
            t.IsTrue(wrong.message.find("SLES_501.23") != std::string::npos, "the message names the other game");

            const auto patched = writeDiscFolder(dir.path, "patched", cnf, "SLUS_203.27", fakeElf(0x22));
            const DiscCheck version = checkDiscFolder(patched, "Aggressive Inline", "SLUS_203.27", sha);
            t.IsTrue(version.status == DiscCheck::Status::WrongVersion && version.elf.empty(), "a different ELF is refused");

            const auto nocnf = writeDiscFolder(dir.path, "nocnf", "", "SLUS_203.27", elf);
            t.IsTrue(checkDiscFolder(nocnf, "Aggressive Inline", "SLUS_203.27", sha).status == DiscCheck::Status::NotPs2Disc, "no SYSTEM.CNF");
            const auto noelf = writeDiscFolder(dir.path, "noelf", cnf, "", elf);
            t.IsTrue(checkDiscFolder(noelf, "Aggressive Inline", "SLUS_203.27", sha).status == DiscCheck::Status::NotPs2Disc, "ELF missing");
            t.IsTrue(checkDiscFolder(dir.path / "gone", "Aggressive Inline", "SLUS_203.27", sha).status == DiscCheck::Status::Missing, "missing");
            t.IsTrue(checkDiscFolder("", "Aggressive Inline", "SLUS_203.27", sha).status == DiscCheck::Status::Missing, "none");
        });

        tc.Run("PCSX2 card save import into an HLE card folder", [](TestCase &t)
        {
            TempDir dir;
            std::vector<uint8_t> big(1500u);
            for (size_t i = 0; i < big.size(); ++i)
                big[i] = static_cast<uint8_t>(i * 13u + 1u);
            const auto card = dir.path / "Mcd001.ps2";
            writeFile(card, makeCard("BASLUS-20327CEAIGAME", big, "PS2D-icon"));

            CardSave save;
            std::string error;
            t.IsTrue(readCardSave(card, "BASLUS-20327CEAIGAME", save, &error), "read: " + error);
            t.Equals(save.files.size(), static_cast<size_t>(2u), "two files");
            t.IsFalse(readCardSave(card, "BASLUS-99999OTHER", save, &error), "a save that is not there");

            const auto slot = dir.path / "memcards" / "slot1";
            std::filesystem::create_directories(slot);
            t.IsTrue(importCardSave(card, "BASLUS-20327CEAIGAME", slot, false, &error), "import: " + error);
            t.IsTrue(readFile(slot / "BASLUS-20327CEAIGAME" / "BIG") == big, "file spanning two clusters");
            const auto icon = readFile(slot / "BASLUS-20327CEAIGAME" / "icon.sys");
            t.Equals(std::string(icon.begin(), icon.end()), std::string("PS2D-icon"), "small file");
            t.IsFalse(importCardSave(card, "BASLUS-20327CEAIGAME", slot, false, &error), "keeps an existing save");
            t.IsTrue(importCardSave(card, "BASLUS-20327CEAIGAME", slot, true, &error), "overwrite on request");
            t.IsFalse(std::filesystem::exists(slot / "BASLUS-20327CEAIGAME.import"), "no staging folder left");

            const auto removed = dir.path / "Mcd002.ps2";
            writeFile(removed, makeCard("BASLUS-20327CEAIGAME", big, "x", true));
            t.IsFalse(readCardSave(removed, "BASLUS-20327CEAIGAME", save, &error), "deleted saves are not imported");

            writeFile(dir.path / "blank.ps2", std::vector<uint8_t>(64u * 1024u, 0u));
            t.IsFalse(readCardSave(dir.path / "blank.ps2", "BASLUS-20327CEAIGAME", save, &error), "unformatted card");

            const auto home = dir.path / "home";
            std::filesystem::create_directories(home / "Library" / "Application Support" / "PCSX2" / "memcards");
            std::filesystem::copy_file(card, home / "Library" / "Application Support" / "PCSX2" / "memcards" / "Mcd001.ps2");
#if defined(__APPLE__)
            const auto found = findPcsx2CardsWithSave("BASLUS-20327CEAIGAME", envFrom({{"HOME", home.string()}}));
            t.Equals(found.size(), static_cast<size_t>(1u), "PCSX2's memcards folder is searched");
#endif
        });

        tc.Run("PCSX2 card save import into a native save file", [](TestCase &t)
        {
            TempDir dir;
            std::vector<uint8_t> data(1500u);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(i * 7u + 3u);
            const auto card = dir.path / "Mcd001.ps2";
            writeFile(card, makeCard("BASLUS-20327CEAIGAME", data, "PS2D-icon", false, "BASLUS-20327CEAIGAME"));
            const auto file = dir.path / "Saves" / "Career.sav";
            std::string error;
            t.IsTrue(importCardSaveToFile(card, "BASLUS-20327CEAIGAME", file, false, &error), "import: " + error);
            t.IsTrue(readFile(file) == data, "the data file, byte for byte");
            t.IsFalse(importCardSaveToFile(card, "BASLUS-20327CEAIGAME", file, false, &error), "keeps an existing save");
            t.IsTrue(importCardSaveToFile(card, "BASLUS-20327CEAIGAME", file, true, &error), "overwrite on request");
            t.IsTrue(readFile(dir.path / "Saves" / "Career.sav.bak") == data, "the replaced save is kept as .bak");

            const auto other = dir.path / "Mcd002.ps2";
            writeFile(other, makeCard("BASLUS-20327CEAIGAME", data, "x"));
            t.IsFalse(importCardSaveToFile(other, "BASLUS-20327CEAIGAME", dir.path / "x.sav", false, &error),
                      "a save without its data file");

            // DM2's data file is not named like its folder.
            const auto dm2Card = dir.path / "Mcd003.ps2";
            writeFile(dm2Card, makeCard("BASLUS-20159BMX2GAME", data, "x", false, "BMX2SAVE.DAT"));
            const auto dm2File = dir.path / "Saves" / "dm2.sav";
            t.IsFalse(importCardSaveToFile(dm2Card, "BASLUS-20159BMX2GAME", dm2File, false, &error),
                      "DM2: no file named like the folder");
            t.IsTrue(importCardSaveToFile(dm2Card, "BASLUS-20159BMX2GAME", dm2File, false, &error, "BMX2SAVE.DAT"),
                     "DM2: its BMX2SAVE.DAT: " + error);
            t.IsTrue(readFile(dm2File) == data, "DM2: the data file, byte for byte");

            // BMX XXX's is named like its folder (the app's save entry BASLUS-20415BMXXXXGM).
            std::vector<uint8_t> bmxData(1500u);
            for (size_t i = 0; i < bmxData.size(); ++i)
                bmxData[i] = static_cast<uint8_t>(i * 13u + 5u);
            const auto bmxCard = dir.path / "Mcd004.ps2";
            writeFile(bmxCard, makeCard("BASLUS-20415BMXXXXGM", bmxData, "x", false, "BASLUS-20415BMXXXXGM"));
            const auto bmxFile = dir.path / "Saves" / "bmx.sav";
            t.IsTrue(importCardSaveToFile(bmxCard, "BASLUS-20415BMXXXXGM", bmxFile, false, &error), "BMX XXX: " + error);
            t.IsTrue(readFile(bmxFile) == bmxData, "BMX XXX: the data file, byte for byte");
        });

        tc.Run("mods: the folders in Mods, by name, the loose layout's left out", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path mods = dir.path / "Mods", disc = dir.path / "disc";
            std::filesystem::create_directories(disc / "ASSETS");
            std::filesystem::create_directories(disc / "MOVIES");
            t.IsTrue(listMods(mods, disc).empty(), "no Mods folder: no mods");
            std::filesystem::create_directories(mods / "assets" / "LEVELS"); // the loose layout's
            std::filesystem::create_directories(mods / "Music");
            std::filesystem::create_directories(mods / "20-rails");
            std::filesystem::create_directories(mods / "10-Park");
            std::ofstream(mods / "tuning.ini") << "[x]\n";
            const std::vector<Mod> found = listMods(mods, disc);
            t.Equals(found.size(), size_t(2), "two mods");
            t.IsTrue(found.size() == 2 && found[0].name == "10-Park" && found[1].name == "20-rails", "by name");
            t.IsTrue(found.size() == 2 && found[1].folder == mods / "20-rails", "each its folder");
            const std::vector<std::filesystem::path> roots = modsRoots(mods, found);
            t.IsTrue(roots.size() == 3 && roots[0] == mods && roots[2] == mods / "20-rails", "the Mods folder first, then the mods");
        });

        tc.Run("mods: one switched off is listed off and not read; the others keep their order", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path mods = dir.path / "Mods", disc = dir.path / "disc";
            for (const char *name : {"10-Park", "20-rails", "30-music"})
                std::filesystem::create_directories(mods / name);
            const std::vector<Mod> found = listMods(mods, disc, {"20-rails", "20-RAILS", "gone"});
            t.Equals(found.size(), size_t(3), "all three listed");
            t.IsTrue(found.size() == 3 && found[0].on && !found[1].on && found[2].on, "20-rails off, by its exact name");
            const std::vector<std::filesystem::path> roots = modsRoots(mods, found);
            t.IsTrue(roots.size() == 3 && roots[0] == mods && roots[1] == mods / "10-Park" && roots[2] == mods / "30-music",
                     "the Mods folder, then the mods on, in order");
            t.IsTrue(listMods(mods, disc).size() == 3 && listMods(mods, disc)[1].on, "none named: every mod on");

            // The boot: the game's settings say which are off (gameBootConfig's PS2X_MODS_DIR).
            Settings settings;
            setModOn(settings, "20-rails", false);
            AppPaths paths;
            paths.supportDir = dir.path;
            BootFolders folders;
            folders.resources = dir.path;
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const ps2x::BootConfig boot = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), folders, noEnv);
            const std::string *given = boot.find("PS2X_MODS_DIR");
            t.IsTrue(given && *given == ps2x::game_files::modsText({mods, mods / "10-Park", mods / "30-music"}),
                     "the boot's mods: the Mods folder, 10-Park, 30-music");
        });

        tc.Run("mods: mod.ini read; a manifest needs an id and known games; a map-only mod.ini is no manifest", [](TestCase &t)
        {
            ModManifest m;
            std::string error;
            t.IsTrue(parseModManifest("[mod]\nid = hd-rails\nname = HD Rails ; shown\nversion = 1.2\nauthor = someone\ngames = AI, dm2 bmx\n"
                                      "needs = base\nconflicts = old-rails, other\n",
                                      m, &error),
                     "a full manifest: " + error);
            t.IsTrue(m.id == "hd-rails" && m.name == "HD Rails" && m.version == "1.2" && m.author == "someone" &&
                         m.games == std::vector<std::string>{"ai", "dm2", "bmx"} && m.needs == std::vector<std::string>{"base"} &&
                         m.conflicts == std::vector<std::string>{"old-rails", "other"},
                     "every key, games lower case, lists by commas or spaces");
            t.IsTrue(!parseModManifest("[mod]\nname = x\ngames = ai\n", m, &error) && error.find("id") != std::string::npos, "no id: refused");
            t.IsTrue(!parseModManifest("[mod]\nid = x\n", m, &error) && error.find("games") != std::string::npos, "no games: refused");
            t.IsTrue(!parseModManifest("[mod]\nid = x\ngames = ai, thps\n", m, &error) && error.find("thps") != std::string::npos,
                     "an unknown game: refused, named");
            t.IsTrue(!parseModManifest("[mod]\nid = a/b\ngames = ai\n", m, &error), "an id with a slash: refused");
            error = "x";
            t.IsTrue(!parseModManifest("[map]\nname = Park\ngame = ai\nslot = TUT\n", m, &error) && error.empty(),
                     "only a [map]: no manifest, and no error");
        });

        tc.Run("mods: the collection's Mods folder: folders and zips, kinds from the layout, duplicates and strays named", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path shared = dir.path / "Mods";
            auto put = [](const std::filesystem::path &file, const std::string &text)
            {
                std::filesystem::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary) << text;
            };
            put(shared / "10-rails" / "mod.ini", "[mod]\nid = rails\nname = Rails\ngames = ai\n");
            put(shared / "10-rails" / "ASSETS" / "LEVELS" / "CIVIC" / "CIVIC.TXR", "rails");
            put(shared / "10-rails" / "readme.txt", "about");
            t.IsTrue(test_zip::write(shared / "20-songs.zip", {{"songs/mod.ini", "[mod]\nid = songs\nversion = 2\ngames = ai, dm2\n"},
                                                              {"songs/Music/01 First.ogg", "ogg"},
                                                              {"songs/Textures/ai/aa-8.png", "png"},
                                                              {"songs/Textures/bmx/bb-8.png", "png"}}),
                     "a zip mod");
            put(shared / "30-loose" / "ASSETS" / "X.BIN", "no manifest");
            put(shared / "40-rails-again" / "mod.ini", "[mod]\nid = rails\ngames = dm2\n");
            put(shared / "50-park" / "mod.ini", "[mod]\nid = park\ngames = ai\n[map]\nname = Test Park\ngame = ai\nslot = TUT\n");
            put(shared / "50-park" / "ASSETS" / "LEVELS" / "TUT" / "TUT.GEO", "geo");
            put(shared / "60-tune" / "mod.ini", "[mod]\nid = tune\ngames = ai\n");
            put(shared / "60-tune" / "tuning.ini", "x = 1\n");
            put(shared / "70-bad.zip", "not a zip");
            const std::vector<Mod> mods = listSharedMods(shared);
            t.Equals(mods.size(), size_t(7), "every folder and zip listed");
            if (mods.size() != 7)
                return;
            t.IsTrue(mods[0].name == "rails" && mods[0].title == "Rails" && mods[0].contents == kModFiles && mods[0].changesLogic() &&
                         mods[0].problem.empty() && mods[0].place == "Mods" && mods[0].orderKey() == "Mods/10-rails",
                     "a folder mod: its key the id, its files (its readme not one), changes game logic");
            t.IsTrue(mods[1].zip && mods[1].name == "songs" && mods[1].version == "2" && mods[1].contents == (kModMusic | kModTextures) &&
                         !mods[1].changesLogic() && mods[1].notes.size() == 1 && mods[1].notes[0].find("Textures/BMX") == 0,
                     "a zip mod, read inside its one folder: music and textures, logic unchanged; BMX XXX's textures noted");
            t.IsTrue(!mods[2].problem.empty() && mods[2].problem.find("mod.ini") != std::string::npos, "a folder with no mod.ini: why");
            t.IsTrue(mods[3].problem == "Mods/10-rails has the same id", "the second mod with an id: not read, the first named");
            t.IsTrue(mods[4].isMap() && mods[4].map.name == "Test Park" && mods[4].map.folder == shared / "50-park" && mods[4].problem.empty(),
                     "a map mod: a [map] in its mod.ini");
            t.IsTrue(mods[5].contents == kModTuning && mods[5].changesLogic(), "tuning.ini: tuning, changes game logic");
            t.IsTrue(!mods[6].problem.empty(), "a zip that is not one: why");

            // The game lists: AI reads rails, songs, tune; DM2 songs; BMX XXX none.
            const std::vector<Mod> ai = modsForGame("ai", {}, mods, {}, {});
            t.Equals(ai.size(), size_t(3), "AI's: rails, songs, tune (no map, no strays)");
            const std::vector<Mod> dm2 = modsForGame("dm2", {}, mods, {}, {});
            t.IsTrue(dm2.size() == 2 && dm2[0].name == "songs" && dm2[1].name == "rails" && !dm2[1].read &&
                         dm2[1].why == "Mods/10-rails has the same id",
                     "DM2's: songs read, the second rails listed not read");
            t.IsTrue(modsForGame("bmx", {}, mods, {}, {}).empty(), "BMX XXX's: none");
            const std::vector<std::filesystem::path> packs = texturePacks(ai, "ai");
            t.IsTrue(packs.size() == 1 && packs[0] == shared / "20-songs.zip" / "Textures" / "ai", "AI's texture folder, through the zip");
            t.IsTrue(texturePacks(dm2, "dm2").empty(), "DM2's: the zip has none for it");
            std::vector<uint8_t> bytes;
            t.IsTrue(ps2x::game_files::readFileBelow(packs[0], "AA-8.PNG", bytes) && bytes.size() == 3, "its picture read in place");
            const std::vector<CustomMap> maps = listAllMaps(mods, dir.path / kMapsFolder);
            t.IsTrue(maps.size() == 1 && maps[0].where == "Mods/50-park" && maps[0].name == "Test Park", "the map page lists the Mods folder's map");
        });

        tc.Run("mods: the order, needs, conflicts, switches; the boot's folders and texture packs", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path shared = dir.path / "Mods", own = dir.path / "games" / "ai" / "Mods";
            auto mod = [](const std::filesystem::path &folder, const std::string &ini, const std::string &file)
            {
                std::filesystem::create_directories((folder / file).parent_path());
                if (!ini.empty())
                    std::ofstream(folder / "mod.ini") << ini;
                std::ofstream(folder / file) << folder.filename().string();
            };
            mod(own / "10-old", "", "ASSETS/A.BIN");
            mod(shared / "a-base", "[mod]\nid = base\ngames = ai\n", "ASSETS/A.BIN");
            mod(shared / "b-addon", "[mod]\nid = addon\ngames = ai\nneeds = base\n", "ASSETS/A.BIN");
            mod(shared / "c-orphan", "[mod]\nid = orphan\ngames = ai\nneeds = missing\n", "ASSETS/B.BIN");
            mod(shared / "d-rival", "[mod]\nid = rival\ngames = ai\nconflicts = base\n", "ASSETS/B.BIN");
            mod(shared / "e-pics", "[mod]\nid = pics\ngames = ai\n", "Textures/x-8.png");
            const std::vector<Mod> legacy = listMods(own, {}, {}, "ai");
            const std::vector<Mod> library = listSharedMods(shared);
            t.IsTrue(legacy.size() == 1 && legacy[0].ownGame == "ai" && legacy[0].orderKey() == "games/ai/Mods/10-old", "the game's own folder's mod");

            std::vector<Mod> list = modsForGame("ai", legacy, library, {}, {});
            std::vector<std::string> keys;
            for (const Mod &m : list)
                keys.push_back(m.name + (m.read ? "" : "!"));
            t.IsTrue(keys == std::vector<std::string>{"10-old", "base", "addon", "orphan!", "rival!", "pics"},
                     "the game's own first, then by name; a need missing and a conflict with what is read: not read");
            t.IsTrue(list[3].why == "needs missing, which is missing" && list[4].why == "clashes with base, which is on", "each says why");

            // base switched off: addon (needs it) not read, rival (its conflict gone) read.
            list = modsForGame("ai", legacy, library, {}, {"base"});
            t.IsTrue(!list[1].read && list[1].why == "Off" && !list[2].read && list[2].why == "needs base, which is not read" && list[4].read,
                     "off: its needers not read, its conflicts read");

            // The order puts addon before base: it is still read after it.
            const std::vector<std::string> order = {"Mods/b-addon", "Mods/e-pics", "Mods/a-base", "games/ai/Mods/10-old"};
            list = modsForGame("ai", legacy, library, order, {});
            keys.clear();
            for (const Mod &m : list)
                keys.push_back(m.name);
            t.IsTrue(keys == std::vector<std::string>{"pics", "base", "addon", "10-old", "orphan", "rival"},
                     "the order's, a mod after what it needs, the rest after");
            std::vector<Mod> ordered = library;
            orderMods(ordered, order);
            t.IsTrue(modOrderOf(ordered).front() == "Mods/b-addon", "the order's list kept");

            // Winners: A.BIN in 10-old, base and addon: addon's (last) wins over both.
            list = modsForGame("ai", legacy, library, {}, {});
            const std::vector<ModWinners> winners = modWinners(list, "ai");
            t.IsTrue(winners[2].winsOver.size() == 2 && winners[2].winsOver[0].mod == 0 && winners[2].winsOver[0].example == "ASSETS/A.BIN" &&
                         winners[0].losesTo.size() == 1 && winners[0].losesTo[0].mod == 2 && winners[1].losesTo.size() == 1,
                     "the last mod's file wins, over each mod before it with that file");

            // The boot: the game's Mods folder, then the mods read in order; the texture folders.
            Settings settings;
            AppPaths paths;
            paths.supportDir = dir.path / "games" / "ai";
            BootFolders folders;
            folders.resources = dir.path;
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const ps2x::BootConfig alone = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), folders, noEnv);
            t.IsTrue(alone.find("PS2X_MODS_DIR") && *alone.find("PS2X_MODS_DIR") == ps2x::game_files::modsText({own, own / "10-old"}) &&
                         !alone.find("PS2X_TEXTURE_PACKS"),
                     "no collection Mods folder given: the game's own, as before, and no texture packs");
            folders.sharedMods = shared;
            const ps2x::BootConfig boot = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), folders, noEnv);
            t.IsTrue(boot.find("PS2X_MODS_DIR") &&
                         *boot.find("PS2X_MODS_DIR") ==
                             ps2x::game_files::modsText({own, own / "10-old", shared / "a-base", shared / "b-addon", shared / "e-pics"}),
                     "the collection's mods read after the game's own, those not read left out");
            t.IsTrue(boot.find("PS2X_TEXTURE_PACKS") && *boot.find("PS2X_TEXTURE_PACKS") == ps2x::game_files::modsText({shared / "e-pics" / "Textures"}),
                     "a mod of one game: its Textures folder itself");

            // The order round-trips in the collection's root file.
            Settings ordering;
            ordering.modOrder = order;
            const Settings back = parseSettings(formatSettings(ordering, gameSettingsProfile("ai"), SettingsScope::App), nullptr);
            t.IsTrue(back.modOrder == order && appWideKey("mod order", "1"), "[mod order] in the root file, read back in order");
        });

        tc.Run("mods: a zip mod read in place by the game's file system; no Mods folder, the boot unchanged", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path shared = dir.path / "Mods";
            t.IsTrue(test_zip::write(shared / "pack.zip", {{"mod.ini", "[mod]\nid = pack\ngames = dm2\n"}, {"ASSETS/MENU.ZAL", "zipped"}}),
                     "a zip mod");
            Settings settings;
            AppPaths paths;
            paths.supportDir = dir.path / "games" / "dm2";
            BootFolders folders;
            folders.sharedMods = shared;
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const ps2x::BootConfig boot = gameBootConfig("dm2", settings, paths, gameSettingsProfile("dm2"), folders, noEnv);
            const std::string *text = boot.find("PS2X_MODS_DIR");
            t.IsTrue(text != nullptr, "mods given");
            if (!text)
                return;
            std::filesystem::create_directories(dir.path / "disc" / "ASSETS");
            std::ofstream(dir.path / "disc" / "ASSETS" / "MENU.ZAL") << "disc";
            ps2x::game_files::GameFileSystem files({dir.path / "disc", nullptr, ps2x::game_files::modsRoots(*text)});
            auto file = files.open("cdrom0:\\ASSETS\\MENU.ZAL;1");
            std::vector<uint8_t> bytes;
            t.IsTrue(file && ps2x::game_files::readAll(*file, bytes) && std::string(bytes.begin(), bytes.end()) == "zipped",
                     "the game reads the zip's file over the disc's");
            BootFolders none;
            const ps2x::BootConfig plain = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), none, noEnv);
            t.IsTrue(plain.find("PS2X_MODS_DIR") && *plain.find("PS2X_MODS_DIR") == (paths.supportDir / "Mods").string() &&
                         !plain.find("PS2X_TEXTURE_PACKS"),
                     "no mods anywhere: the Mods folder alone, as before");
        });

        tc.Run("custom maps: map.ini read, the maps folder's maps, and the boot into one", [](TestCase &t)
        {
            CustomMap map;
            std::string error;
            t.IsTrue(parseMapDescription("[map]\nname = Test Park ; the park\ngame = AI\nslot = tut\npreview = preview.bmp\nauthor = custom2\n",
                                         map, &error),
                     "a full description parses: " + error);
            t.IsTrue(map.name == "Test Park" && map.game == "ai" && map.slot == "TUT" && map.preview == "preview.bmp" && map.author == "custom2",
                     "name as written, the game lower case, the slot upper case, the preview and author");
            t.IsTrue(!parseMapDescription("name = x\ngame = ai\nslot = TUT\n", map, &error) && error.find("[map]") != std::string::npos,
                     "no [map] section: refused, saying so");
            t.IsTrue(!parseMapDescription("[map]\nname = x\ngame = ai\n", map, &error) && error.find("slot") != std::string::npos,
                     "no slot: refused");
            t.IsTrue(!parseMapDescription("[map]\nname = x\ngame = ai\nslot = TUT\npreview = ../x.bmp\n", map, &error),
                     "a preview outside the folder: refused");
            t.IsTrue(parseMapDescription("[other]\na = b\n[map]\nname = y\ngame = dm2\nslot = WOOD\n", map) && map.game == "dm2" && map.preview.empty(),
                     "other sections pass; no preview is fine");
            t.IsTrue(!map.slotNpcs, "the slot's own NPCs left out by default");
            t.IsTrue(parseMapDescription("[map]\nname = y\ngame = bmx\nslot = CHL_MULY\nnpcs = Slot\n", map) && map.slotNpcs,
                     "npcs = slot keeps them");
            t.IsTrue(parseMapDescription("[map]\nname = y\ngame = bmx\nslot = CHL_MULY\nnpcs = none\n", map) && !map.slotNpcs,
                     "npcs = none: left out");
            t.IsTrue(!parseMapDescription("[map]\nname = y\ngame = bmx\nslot = CHL_MULY\nnpcs = some\n", map, &error) &&
                         error.find("npcs") != std::string::npos,
                     "another npcs value: refused, saying so");

            TempDir dir;
            const std::filesystem::path mods = dir.path / "Mods", disc = dir.path / "disc", mapsFolder = dir.path / kMapsFolder;
            std::filesystem::create_directories(disc / "ASSETS");
            std::filesystem::create_directories(mods / "10-rails");
            std::filesystem::create_directories(mapsFolder / "20-park" / "ASSETS" / "LEVELS" / "TUT");
            std::filesystem::create_directories(mapsFolder / "30-bmxpark");
            std::filesystem::create_directories(mapsFolder / "40-broken");
            std::ofstream(mapsFolder / "20-park" / "map.ini") << "[map]\nname = Test Park\ngame = ai\nslot = TUT\n";
            std::ofstream(mapsFolder / "30-bmxpark" / "map.ini") << "[map]\nname = Mullaly Mix\ngame = bmx\nslot = PARK\n";
            std::ofstream(mapsFolder / "40-broken" / "map.ini") << "[map]\nname = no game\n";
            t.IsTrue(isMapFolder(mapsFolder / "20-park") && !isMapFolder(mods / "10-rails"), "a map folder carries map.ini");
            t.IsTrue(listCustomMaps(dir.path / "nowhere").empty(), "no maps folder: no maps");
            const std::vector<CustomMap> maps = listCustomMaps(mapsFolder);
            t.Equals(maps.size(), size_t(2), "two maps read (the broken description left out)");
            t.IsTrue(maps.size() == 2 && maps[0].name == "Test Park" && maps[0].folderName == "20-park" &&
                         maps[0].folder == mapsFolder / "20-park" && maps[1].game == "bmx",
                     "each once, with its folder and name");
            t.IsTrue(maps.size() == 2 && mapPlayableIn(maps[0], "ai") && mapPlayableIn(maps[0], "dm2") && mapPlayableIn(maps[0], "bmx") &&
                         !mapPlayableIn(maps[1], "bmx") && !mapPlayableIn(maps[1], "ai"),
                     "every player starts a map in the slot its own game lends; none one in a slot its game does not lend (PARK)");
            t.IsTrue(!mapNeedsConversion(maps[0], "ai") && mapNeedsConversion(maps[0], "dm2") && mapNeedsConversion(maps[0], "bmx"),
                     "AI's player reads AI's map as it is; the others get it converted");
            t.IsTrue(mapSlotOf("ai") == mapSlot("ai", "TUT") && mapSlotOf("dm2") == mapSlot("dm2", "WOODWARD") &&
                         mapSlotOf("bmx") == mapSlot("bmx", "CHL_MULY") && !mapSlotOf("xyz"),
                     "the slot each game lends");
            t.IsTrue(mapSlot("ai", "TUT") && std::string(mapSlot("ai", "TUT")->startLevel) == "ESTATE" && !mapSlot("ai", "ZOO"),
                     "AI's slot TUT is reached through the Estate's entry");
            // The other games' slots: DM2's Woodward Camp row (FREERIDE's
            // level select value 0, open from the start), BMX XXX's Bronx
            // row (a challenge level: no clock), its goal counts kept by
            // the start.
            const MapSlot *dm2Slot = mapSlot("dm2", "WOODWARD");
            const MapSlot *bmxSlot = mapSlot("bmx", "CHL_MULY");
            t.IsTrue(dm2Slot && std::string(dm2Slot->startLevel) == "0" && std::string(dm2Slot->folder) == "ASSETS/LEVELS" &&
                         std::string(dm2Slot->needs) == "PS2X_DM2_LEVEL_CAMERA_POINTS" && !mapSlot("dm2", "LEWAREHO"),
                     "DM2's slot is Woodward Camp's level row, started as FREERIDE's level select value 0, its level-point "
                     "camera on the map's own camera points");
            t.IsTrue(bmxSlot && std::string(bmxSlot->startLevel) == "CHL_MULY" &&
                         std::string(bmxSlot->needs) == "PS2X_BMX_KEEP_LEVEL_GOALS" && !mapSlot("bmx", "CMP_RMPG"),
                     "BMX XXX's slot is The Bronx's row, started by its folder name with the row's goal counts kept");
            t.IsTrue(bmxSlot && std::string(bmxSlot->slotNpcs) == "PS2X_BMX_LEVEL_NPCS" && dm2Slot && !dm2Slot->slotNpcs[0] &&
                         !mapSlot("ai", "TUT")->slotNpcs[0],
                     "only BMX XXX's slot has NPCs from outside the map's files (The Bronx's riders)");
            std::filesystem::create_directories(mapsFolder / "50-dm2park" / "ASSETS" / "LEVELS" / "WOODWARD");
            std::ofstream(mapsFolder / "50-dm2park" / "map.ini") << "[map]\nname = DM2 Park\ngame = dm2\nslot = WOODWARD\n";

            // The boot into the map: the Mods folder, the mods on, then the
            // map last; the start into the slot with the entry it needs.
            Settings settings;
            AppPaths paths;
            paths.supportDir = dir.path;
            BootFolders folders;
            folders.resources = dir.path;
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const std::vector<Mod> found = listMods(mods, disc, settings.modsOff);
            const std::vector<std::filesystem::path> roots = freeplayModsRoots(mods, found, maps[0]);
            t.IsTrue(roots.size() == 3 && roots[0] == mods && roots[1] == mods / "10-rails" && roots[2] == mapsFolder / "20-park",
                     "the Mods folder, the plain mod, the map last");
            const std::vector<std::filesystem::path> convertedRoots = freeplayModsRoots(mods, found, maps[0], true);
            t.IsTrue(convertedRoots.size() == 2 && convertedRoots[1] == mods / "10-rails",
                     "a map read converted: its folder not read (its files are the memory folder's)");
            folders.customMap = maps[0].folder;
            folders.customMapSlot = maps[0].slot;
            const ps2x::BootConfig boot = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), folders, noEnv);
            const std::string *given = boot.find("PS2X_MODS_DIR");
            t.IsTrue(given && *given == ps2x::game_files::modsText(roots), "the boot's mods are those");
            t.IsTrue(boot.find("PS2X_DEV_START_LEVEL") && *boot.find("PS2X_DEV_START_LEVEL") == "ESTATE" &&
                         boot.find("PS2X_DEV_START_SKATER") && *boot.find("PS2X_DEV_START_SKATER") == "ask" && boot.find("PS2X_ESTATE") &&
                         *boot.find("PS2X_ESTATE") == "1",
                     "started into the slot's free skate with the Estate's entry on, the skater the player's choice");
            // DM2's boot into its map: the start, the rider the player's.
            {
                const std::vector<CustomMap> dm2Maps = listCustomMaps(mapsFolder);
                const CustomMap *dm2Map = nullptr;
                for (const CustomMap &each : dm2Maps)
                    if (each.game == "dm2")
                        dm2Map = &each;
                BootFolders dm2Folders;
                dm2Folders.resources = dir.path;
                if (dm2Map)
                {
                    dm2Folders.customMap = dm2Map->folder;
                    dm2Folders.customMapSlot = dm2Map->slot;
                }
                const ps2x::BootConfig dm2Boot = gameBootConfig("dm2", settings, paths, gameSettingsProfile("dm2"), dm2Folders, noEnv);
                t.IsTrue(dm2Map && dm2Boot.find("PS2X_DEV_START_LEVEL") && *dm2Boot.find("PS2X_DEV_START_LEVEL") == "0" &&
                             dm2Boot.find("PS2X_DEV_START_SKATER") && *dm2Boot.find("PS2X_DEV_START_SKATER") == "ask" &&
                             !dm2Boot.find("PS2X_DM2_PARK_FILE") && !dm2Boot.find("PS2X_ESTATE") &&
                             dm2Boot.find("PS2X_DM2_LEVEL_CAMERA_POINTS") && *dm2Boot.find("PS2X_DM2_LEVEL_CAMERA_POINTS") == "1",
                         "DM2 started into FREERIDE's Woodward Camp row, the rider the player's choice, the map's camera points on");
            }
            // BMX XXX's boot into its map: The Bronx's NPC riders left out
            // unless the map keeps them; AI's boot has no such switch.
            {
                BootFolders bmxFolders;
                bmxFolders.resources = dir.path;
                bmxFolders.customMap = mapsFolder / "30-bmxpark";
                bmxFolders.customMapSlot = "CHL_MULY";
                const ps2x::BootConfig leftOut = gameBootConfig("bmx", settings, paths, gameSettingsProfile("bmx"), bmxFolders, noEnv);
                bmxFolders.customMapSlotNpcs = true;
                const ps2x::BootConfig kept = gameBootConfig("bmx", settings, paths, gameSettingsProfile("bmx"), bmxFolders, noEnv);
                t.IsTrue(leftOut.find("PS2X_BMX_LEVEL_NPCS") && *leftOut.find("PS2X_BMX_LEVEL_NPCS") == "0" &&
                             leftOut.find("PS2X_BMX_KEEP_LEVEL_GOALS") && kept.find("PS2X_BMX_LEVEL_NPCS") &&
                             *kept.find("PS2X_BMX_LEVEL_NPCS") == "1" && !boot.find("PS2X_BMX_LEVEL_NPCS"),
                         "BMX XXX's map boot: the slot's NPC riders off (0), on (1) with npcs = slot; none given to AI's");
            }
            BootFolders plain;
            plain.resources = dir.path;
            const ps2x::BootConfig asIs = gameBootConfig("ai", settings, paths, gameSettingsProfile("ai"), plain, noEnv);
            t.IsTrue(!asIs.find("PS2X_DEV_START_LEVEL") && asIs.find("PS2X_MODS_DIR") &&
                         *asIs.find("PS2X_MODS_DIR") == ps2x::game_files::modsText({mods, mods / "10-rails"}),
                     "without a map: no start, and only the mods");
            // A map made in AI started by DM2's player: converted (its files
            // the memory folder's), the map's folder unread.
            {
                BootFolders converted;
                converted.resources = dir.path;
                converted.customMap = maps[0].folder;
                converted.customMapSlot = "WOODWARD";
                converted.customMapConverted = true;
                const ps2x::BootConfig boot = gameBootConfig("dm2", settings, paths, gameSettingsProfile("dm2"), converted, noEnv);
                t.IsTrue(boot.find("PS2X_MODS_DIR") && *boot.find("PS2X_MODS_DIR") == ps2x::game_files::modsText({mods, mods / "10-rails"}) &&
                             boot.find("PS2X_DEV_START_LEVEL") && *boot.find("PS2X_DEV_START_LEVEL") == "0",
                         "a converted map: started into the player's slot, its folder not read");
            }

            // The setting: app-wide, off by default.
            t.IsTrue(!parseSettings("", nullptr).customFreeplay, "Custom Freeplay is off by default");
            Settings on;
            on.customFreeplay = true;
            t.IsTrue(parseSettings(formatSettings(on), nullptr).customFreeplay, "[general] custom_freeplay round-trips");
            t.IsTrue(appWideKey("general", "custom_freeplay"), "it is the app's, not a game's");
        });

        tc.Run("custom maps: no boot but a map's own reads its folder, whatever its on/off state", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path mods = dir.path / "Mods", disc = dir.path / "disc";
            std::filesystem::create_directories(disc / "ASSETS");
            std::filesystem::create_directories(mods / "10-rails" / "ASSETS" / "LEVELS" / "TUT");
            // Maps in each game's slot: one left on, one switched off, one
            // switched on by name, one whose map.ini does not parse.
            const std::vector<std::pair<std::string, std::string>> maps = {
                {"20-tut", "[map]\nname = Tut\ngame = ai\nslot = TUT\n"},
                {"30-park", "[map]\nname = Park\ngame = dm2\nslot = WOODWARD\n"},
                {"40-bronx", "[map]\nname = Bronx\ngame = bmx\nslot = CHL_MULY\n"},
                {"50-broken", "[map]\nname = no game\n"},
            };
            for (const auto &[name, ini] : maps)
            {
                std::filesystem::create_directories(mods / name / "ASSETS" / "LEVELS" / "TUT");
                std::ofstream(mods / name / "map.ini") << ini;
            }
            Settings settings;
            setModOn(settings, "30-park", false);
            setModOn(settings, "40-bronx", true);
            t.IsTrue(listMods(mods, disc, settings.modsOff).size() == 1 && listMods(mods, disc)[0].name == "10-rails",
                     "the Mods page lists the plain mod only: a map is not a mod");
            AppPaths paths;
            paths.supportDir = dir.path;
            auto noEnv = [](const char *) -> const char * { return nullptr; };
            const std::string plainMods = ps2x::game_files::modsText({mods, mods / "10-rails"});
            for (const char *game : {"ai", "dm2", "bmx"})
            {
                // The menu's tile, the devstart and the hosts all boot
                // through gameBootConfig: with no map chosen, no map read.
                BootFolders folders;
                folders.resources = dir.path;
                const ps2x::BootConfig boot = gameBootConfig(game, settings, paths, gameSettingsProfile(game), folders, noEnv);
                t.IsTrue(boot.find("PS2X_MODS_DIR") && *boot.find("PS2X_MODS_DIR") == plainMods,
                         std::string(game) + "'s own start reads the Mods folder and the plain mod, no map");
            }
            // A boot into one map reads that map's folder and no other.
            for (const CustomMap &map : listCustomMaps(mods))
            {
                BootFolders folders;
                folders.resources = dir.path;
                folders.customMap = map.folder;
                folders.customMapSlot = map.slot;
                const ps2x::BootConfig boot = gameBootConfig(map.game, settings, paths, gameSettingsProfile(map.game), folders, noEnv);
                t.IsTrue(boot.find("PS2X_MODS_DIR") &&
                             *boot.find("PS2X_MODS_DIR") == ps2x::game_files::modsText({mods, mods / "10-rails", map.folder}),
                         map.folderName + "'s boot reads its folder last and no other map's");
            }
        });

        tc.Run("custom maps: the idle cameras and the respawns go with a map, in each game's form", [](TestCase &t)
        {
            namespace lv = ps2x::app::levels;
            // A 20 m floor, the starts, a respawn and one idle camera 20 m
            // back and 5 m up (the model's axes: +y up, the games' -z).
            auto park = [](bool camera)
            {
                lv::ModelBuilder b;
                std::vector<lv::Rgba> grey(16 * 16, lv::Rgba{150, 150, 144, 255});
                lv::Primitive floor;
                floor.positions = {{-10.0, 0.0, -10.0}, {10.0, 0.0, -10.0}, {10.0, 0.0, 10.0}, {-10.0, 0.0, 10.0}};
                floor.triangles = {{0, 2, 1}, {0, 3, 2}};
                floor.uvs = std::vector<lv::Vec2>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
                floor.material = b.material("concrete", {1.0, 1.0, 1.0, 1.0}, b.image("concrete", 16, 16, grey));
                b.node("floor", b.mesh("floor", {floor}));
                b.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 0.0});
                b.node("PlayerTwo#spawn=PlayerTwo", std::nullopt, {2.0, 1.0, 0.0});
                b.node("Respawn#spawn=respawn", std::nullopt, {5.0, 1.0, 5.0});
                if (camera)
                    b.node("Idle1#camera#fov=70", std::nullopt, {0.0, 5.0, 20.0});
                return b.model();
            };
            auto points = [](const lv::Converted &c, lv::Engine engine, const std::string &dat)
            { return lv::SceneData::read(c.files.at(dat), engine).points; };
            auto named = [](const std::vector<lv::ScenePoint> &ps, uint32_t kind, uint32_t name)
            { return std::count_if(ps.begin(), ps.end(), [&](const lv::ScenePoint &p) { return p.kind == kind && p.name == name; }); };

            const std::vector<lv::ScenePoint> bmx = points(lv::convertModel(park(true), lv::Engine::Zge4), lv::Engine::Zge4, "ASSETS/LEVELS/CHL_MULY/CHL_MULY.DAT");
            const auto bmxCamera = std::find_if(bmx.begin(), bmx.end(), [](const lv::ScenePoint &p) { return p.kind == lv::kCameraPoint; });
            t.IsTrue(bmxCamera != bmx.end() && bmxCamera->name == lv::kIdleCamera && bmxCamera->index == 1 && bmxCamera->tags.size() == 4 &&
                         bmxCamera->tags[0] == 70 && bmxCamera->position == lv::Vec3{0.0, -5.0, -20.0},
                     "BMX XXX: the camera an idle point 1 at the eye, its field of view the first tag byte");
            t.IsTrue(named(bmx, lv::kSpawnPoint, lv::kPlayerTwo) == 1 && named(bmx, lv::kSpawnPoint, 0) == 1,
                     "BMX XXX: the second start kept, the respawn unnamed (its respawn takes any spawn)");

            const std::vector<lv::ScenePoint> dm2 = points(lv::convertModel(park(true), lv::Engine::Zge2), lv::Engine::Zge2, "ASSETS/LEVELS/WOODWARD/WOODWARD.DAT");
            const auto dm2Camera = std::find_if(dm2.begin(), dm2.end(), [](const lv::ScenePoint &p) { return p.kind == lv::kCameraPoint; });
            t.IsTrue(dm2Camera != dm2.end() && dm2Camera->index == 1 && dm2Camera->position == lv::Vec3{0.0, 0.0, -20.0},
                     "DM2: the camera's point 5 m below its eye (the level-point camera's lift)");
            t.IsTrue(named(dm2, lv::kSpawnPoint, lv::kPlayerTwo) == 0 && named(dm2, lv::kSpawnPoint, lv::nameHash("Player")) == 1,
                     "DM2: no second start, the respawn one of its \"Player\" spawns");

            const lv::Converted ai = lv::convertModel(park(true), lv::Engine::Zge3);
            const std::vector<lv::ScenePoint> aiPoints = points(ai, lv::Engine::Zge3, "ASSETS/LEVELS/TUT/TUT.DAT");
            t.IsTrue(named(aiPoints, lv::kCameraPoint, lv::kIdleCamera) == 1 && named(aiPoints, lv::kSpawnPoint, lv::nameHash("Respawn")) == 1,
                     "AI: the camera kept (it reads none; its map carries it on), the respawn its \"Respawn\"");

            // AI's files exported again: the camera and the respawn back in the model.
            lv::LevelFiles files;
            for (const char *ext : {"GEO", "INS", "DAT", "TXR", "ZIT"})
                files[ext] = ai.files.at(std::string("ASSETS/LEVELS/TUT/TUT.") + ext);
            const lv::Model back = lv::exportLevel(lv::Engine::Zge3, files);
            const auto has = [&](const std::string &name)
            { return std::any_of(back.nodes.begin(), back.nodes.end(), [&](const lv::Node &n) { return n.name == name; }); };
            t.IsTrue(has("Idle1#camera#fov=70") && has("Respawn#spawn=respawn") && has("PlayerTwo#spawn=PlayerTwo"),
                     "exported back: the camera, the respawn, the second start");

            // No camera of its own: BMX XXX gets one behind and above the
            // PlayerOne spawn (8 m back along its facing, 3 m up), looking at it; AI none.
            const std::vector<lv::ScenePoint> made = points(lv::convertModel(park(false), lv::Engine::Zge4), lv::Engine::Zge4, "ASSETS/LEVELS/CHL_MULY/CHL_MULY.DAT");
            const auto start = std::find_if(made.begin(), made.end(), [](const lv::ScenePoint &p) { return p.kind == lv::kCameraPoint; });
            bool looks = false;
            if (start != made.end())
            {
                const lv::Quat &q = start->orientation; // its +z, turned: toward the spawn and down
                const double fy = 2.0 * (q[1] * q[2] - q[3] * q[0]), fz = 1.0 - 2.0 * (q[0] * q[0] + q[1] * q[1]);
                looks = fz > 0.9 && fy > 0.1;
            }
            t.IsTrue(start != made.end() && std::abs(start->position[0]) < 1e-9 && std::abs(start->position[1] + 4.0) < 1e-6 &&
                         std::abs(start->position[2] + 8.0) < 1e-6 && looks,
                     "a camera made behind the start, looking at it");
            const std::vector<lv::ScenePoint> aiMade = points(lv::convertModel(park(false), lv::Engine::Zge3), lv::Engine::Zge3, "ASSETS/LEVELS/TUT/TUT.DAT");
            t.IsTrue(named(aiMade, lv::kCameraPoint, lv::kIdleCamera) == 0, "AI makes none (it reads none)");
        });

        tc.Run("custom maps: a cut-out texture halved to fit stays a cut-out", [](TestCase &t)
        {
            namespace lv = ps2x::app::levels;
            // A 20 m floor under a 256 x 256 lattice (one-texel teal bars every
            // other column, clear between: an arch's or a fence's) and a glass
            // pane at a third of full alpha, both over the 128 side budget.
            lv::ModelBuilder b;
            std::vector<lv::Rgba> bars(256 * 256), glass(256 * 256, lv::Rgba{90, 120, 140, 85});
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x)
                    bars[static_cast<size_t>(y) * 256 + static_cast<size_t>(x)] = x % 2 == 0 ? lv::Rgba{40, 200, 180, 255} : lv::Rgba{0, 0, 0, 0};
            auto quad = [&](double z, int material)
            {
                lv::Primitive p;
                p.positions = {{-10.0, 0.0, z - 10.0}, {10.0, 0.0, z - 10.0}, {10.0, 0.0, z + 10.0}, {-10.0, 0.0, z + 10.0}};
                p.triangles = {{0, 2, 1}, {0, 3, 2}};
                p.uvs = std::vector<lv::Vec2>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
                p.material = material;
                return p;
            };
            b.node("arch", b.mesh("arch", {quad(0.0, b.material("bars", {1.0, 1.0, 1.0, 1.0}, b.image("bars", 256, 256, bars)))}));
            b.node("pane", b.mesh("pane", {quad(30.0, b.material("glass", {1.0, 1.0, 1.0, 1.0}, b.image("glass", 256, 256, glass)))}));
            b.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 0.0});
            const lv::Converted ai = lv::convertModel(b.model(), lv::Engine::Zge3);
            lv::LevelFiles files;
            for (const char *ext : {"GEO", "INS", "DAT", "TXR", "ZIT"})
                files[ext] = ai.files.at(std::string("ASSETS/LEVELS/TUT/TUT.") + ext);
            const lv::Model back = lv::exportLevel(lv::Engine::Zge3, files);
            auto image = [&](const std::string &name) -> const lv::Image *
            {
                for (const lv::Image &i : back.images)
                    if (i.name.rfind(name, 0) == 0)
                        return &i;
                return nullptr;
            };
            const lv::Image *barsBack = image("bars"), *glassBack = image("glass");
            t.IsTrue(barsBack && barsBack->width == 128 && barsBack->height == 128 && glassBack && glassBack->width == 128,
                     "both halved to the side budget");
            if (!barsBack || !glassBack)
                return;
            size_t solid = 0, other = 0;
            bool teal = true;
            for (const lv::Rgba &p : barsBack->pixels)
            {
                if (p[3] == 255)
                {
                    ++solid;
                    teal = teal && p[0] == 40 && p[1] == 200 && p[2] == 180;
                }
                else if (p[3] != 0)
                    ++other;
            }
            t.IsTrue(other == 0 && solid == barsBack->pixels.size() && teal,
                     "the bars: every texel solid in the bars' own colour (averaged, half clear and darkened to (20, 100, 90))");
            t.IsTrue(std::all_of(glassBack->pixels.begin(), glassBack->pixels.end(), [](const lv::Rgba &p) { return p[3] == 85; }),
                     "the glass: averaged as before, its alpha kept");
        });

        tc.Run("custom maps: a dynamics prop goes with a map, its name's whole hash its definition's", [](TestCase &t)
        {
            namespace lv = ps2x::app::levels;
            // A 20 m floor and two traffic cones, #dynamics props of one mesh
            // named longer than the name field, with Woodward's cone's
            // definition (DM2's disc: dynamicsDef_ConesA's one box body; its
            // instance's object sound 12).
            const std::string coneName = "DynamicsDef_ExampleConeLongerThanItsField";
            const lv::Bytes body = {0x00, 0x00, 0x00, 0x00, 0xc1, 0x85, 0x7b, 0x0a, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00,
                                    0x48, 0xba, 0xff, 0xff, 0x51, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0xf4, 0x94, 0x0a, 0x42, 0xff, 0xff, 0x51, 0x42, 0x38, 0xbe, 0x24, 0x42};
            const lv::Bytes coneSound = {0, 0, 0, 0, 12, 0, 0, 0};
            lv::ModelBuilder b;
            std::vector<lv::Rgba> grey(16 * 16, lv::Rgba{150, 150, 144, 255});
            lv::Primitive floor;
            floor.positions = {{-10.0, 0.0, -10.0}, {10.0, 0.0, -10.0}, {10.0, 0.0, 10.0}, {-10.0, 0.0, 10.0}};
            floor.triangles = {{0, 2, 1}, {0, 3, 2}};
            floor.uvs = std::vector<lv::Vec2>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
            floor.material = b.material("concrete", {1.0, 1.0, 1.0, 1.0}, b.image("concrete", 16, 16, grey));
            b.node("floor", b.mesh("floor", {floor}));
            lv::Primitive cone;
            cone.positions = {{-0.2, 0.0, -0.2}, {0.2, 0.0, -0.2}, {0.0, 0.8, 0.0}, {0.0, 0.0, 0.2}};
            cone.triangles = {{0, 2, 1}, {1, 2, 3}, {3, 2, 0}, {0, 1, 3}};
            cone.material = b.material("cone", {1.0, 0.4, 0.0, 1.0}, std::nullopt);
            lv::MeshExtras extras;
            extras.dynamics = lv::DynamicsDefinition{{lv::Chunk::leaf(lv::tag("DBDY"), body)}, lv::Engine::Zge2, {}, coneSound};
            const int coneMesh = b.mesh(coneName, {cone}, extras);
            b.node("cone_01#dynamics", coneMesh, {2.0, 0.0, 0.0});
            b.node("cone_02#dynamics", coneMesh, {-2.0, 0.0, 0.0});
            b.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 8.0});

            struct Props
            {
                const lv::GeoObject *prop = nullptr;
                std::vector<const lv::Instance *> instances;
                const lv::Chunk *zdyn = nullptr;
                bool othersHashed = true; // every other object's word its name's hash
            };
            auto propsOf = [&](const lv::Geometry &geo, const lv::Instances &ins)
            {
                Props p;
                for (const lv::GeoObject &o : geo.objects)
                {
                    if (o.typeData && o.typeData->size() >= 4 && lv::readU32(*o.typeData, 0) == lv::kDynamicsObjectClass)
                        p.prop = &o;
                    else
                        p.othersHashed = p.othersHashed && o.header.nameHash == lv::nameHash(o.header.name);
                }
                for (const auto &[place, k] : geo.other)
                    if (k.tag == lv::tag("ZDYN"))
                        p.zdyn = &k;
                for (const lv::Instance &i : ins.instances)
                    if (p.prop && i.geometryId == p.prop->header.id)
                        p.instances.push_back(&i);
                return p;
            };
            auto read = [](const lv::Converted &c, lv::Engine engine, const std::string &base)
            {
                return std::make_pair(lv::Geometry::read(c.files.at(base + "GEO"), engine), lv::Instances::read(c.files.at(base + "INS"), engine));
            };

            // AI's: the field the name's first 31 characters, the word the
            // whole name's hash, which the ZDYN's DDEF has after every object;
            // AI's class data and tags (the definition is another game's).
            const lv::Converted ai = lv::convertModel(b.model(), lv::Engine::Zge3);
            const auto [aiGeo, aiIns] = read(ai, lv::Engine::Zge3, "ASSETS/LEVELS/TUT/TUT.");
            const Props aiProps = propsOf(aiGeo, aiIns);
            t.IsTrue(aiProps.prop && aiProps.prop->header.name == coneName.substr(0, 31) && aiProps.prop->header.nameHash == lv::nameHash(coneName) &&
                         aiProps.othersHashed,
                     "AI: the prop's name cut to its field, its word the whole name's hash; every object's word its name's");
            t.IsTrue(aiProps.zdyn && aiProps.zdyn->kids.size() == 2 && aiProps.zdyn->kids[0].tag == lv::tag("DDEF") &&
                         lv::readU32(aiProps.zdyn->kids[0].data, 0) == lv::nameHash(coneName) && aiProps.zdyn->kids[1].data == body &&
                         aiGeo.other.size() == 1 && aiGeo.other[0].first == aiGeo.objects.size(),
                     "AI: its definition after every object, named by that hash");
            t.IsTrue(aiProps.prop && *aiProps.prop->typeData == lv::Bytes({0x94, 0xF0, 0x10, 0x0A, 1, 1, 0, 0}) && aiProps.instances.size() == 2 &&
                         std::all_of(aiProps.instances.begin(), aiProps.instances.end(),
                                     [](const lv::Instance *i) { return i->flags == 0x63 && i->tags == lv::Bytes(4, 0); }),
                     "AI: DynamicsObject's word and AI's impact sounds, both cones the stock props' flags and tags");

            // DM2's: the definition is DM2's own, so its tags (the object
            // sound) are kept; DM2's flags.
            const lv::Converted dm2 = lv::convertModel(b.model(), lv::Engine::Zge2);
            const auto [dm2Geo, dm2Ins] = read(dm2, lv::Engine::Zge2, "ASSETS/LEVELS/WOODWARD/WOODWARD.");
            const Props dm2Props = propsOf(dm2Geo, dm2Ins);
            t.IsTrue(dm2Props.prop && *dm2Props.prop->typeData == lv::Bytes({0x94, 0xF0, 0x10, 0x0A}) && dm2Props.instances.size() == 2 &&
                         std::all_of(dm2Props.instances.begin(), dm2Props.instances.end(),
                                     [&](const lv::Instance *i) { return i->flags == 0x23 && i->tags == coneSound; }),
                     "DM2: its own definition's tags kept (the cone's sound), the type word alone");

            // BMX XXX's: the sound name "default"; exported again, the prop
            // comes back with its definition and its whole name's hash.
            const lv::Converted bmx = lv::convertModel(b.model(), lv::Engine::Zge4);
            const std::string bmxBase = "ASSETS/LEVELS/CHL_MULY/CHL_MULY.";
            const auto [bmxGeo, bmxIns] = read(bmx, lv::Engine::Zge4, bmxBase);
            const Props bmxProps = propsOf(bmxGeo, bmxIns);
            lv::Bytes bmxClass = {0x94, 0xF0, 0x10, 0x0A, 'd', 'e', 'f', 'a', 'u', 'l', 't'};
            bmxClass.resize(4 + 64);
            t.IsTrue(bmxProps.prop && *bmxProps.prop->typeData == bmxClass && bmxProps.instances.size() == 2 && bmxProps.zdyn,
                     "BMX XXX: the sound name \"default\" in its 64 bytes");
            lv::LevelFiles files;
            for (const char *ext : {"GEO", "INS", "DAT", "TXR", "ZIT"})
                files[ext] = bmx.files.at(bmxBase + ext);
            const lv::Model back = lv::exportLevel(lv::Engine::Zge4, files);
            const auto propNodes = std::count_if(back.nodes.begin(), back.nodes.end(), [](const lv::Node &n) { return n.name.find("#dynamics") != std::string::npos; });
            const auto propMesh = std::find_if(back.meshes.begin(), back.meshes.end(), [](const lv::Mesh &m) { return m.extras.dynamics.has_value(); });
            t.IsTrue(propNodes == 2 && propMesh != back.meshes.end() && propMesh->extras.nameHash == lv::nameHash(coneName) &&
                         propMesh->extras.dynamics->game == lv::Engine::Zge4 && propMesh->extras.dynamics->classData == lv::slice(bmxClass, 4) &&
                         propMesh->extras.dynamics->chunks.size() == 1 && propMesh->extras.dynamics->chunks[0].data == body,
                     "exported back: two #dynamics nodes, the definition with BMX XXX's class data, the whole name's hash");
            const lv::Converted again = lv::convertModel(back, lv::Engine::Zge3);
            const auto [againGeo, againIns] = read(again, lv::Engine::Zge3, "ASSETS/LEVELS/TUT/TUT.");
            const Props againProps = propsOf(againGeo, againIns);
            t.IsTrue(againProps.prop && againProps.prop->header.name == coneName.substr(0, 31) &&
                         againProps.prop->header.nameHash == lv::nameHash(coneName) && againProps.instances.size() == 2 && againProps.zdyn &&
                         lv::readU32(againProps.zdyn->kids[0].data, 0) == lv::nameHash(coneName),
                     "and made into AI's again: the same prop, found by the same hash");

            // A prop needs its definition.
            lv::ModelBuilder bare;
            lv::Primitive bareFloor = floor, bareCone = cone;
            bareFloor.material.reset();
            bareCone.material.reset();
            bare.node("floor", bare.mesh("floor", {bareFloor}));
            bare.node("cone#dynamics", bare.mesh("cone", {bareCone}));
            bare.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 8.0});
            std::string why;
            try
            {
                lv::convertModel(bare.model(), lv::Engine::Zge3);
            }
            catch (const lv::LevelError &e)
            {
                why = e.what();
            }
            t.IsTrue(why.find("#dynamics needs its definition") != std::string::npos, "a #dynamics node without a definition refused, with why");
        });

        tc.Run("custom maps: one map in the maps folder, made into each game's files in memory", [](TestCase &t)
        {
            namespace lv = ps2x::app::levels;
            // A small park as a model: a 20 m concrete floor of two triangles,
            // a ledge's edge to grind, the PlayerOne spawn.
            lv::ModelBuilder b;
            std::vector<lv::Rgba> grey(16 * 16, lv::Rgba{150, 150, 144, 255});
            const int concrete = b.material("concrete", {1.0, 1.0, 1.0, 1.0}, b.image("concrete", 16, 16, grey));
            lv::Primitive floor;
            floor.positions = {{-10.0, 0.0, -10.0}, {10.0, 0.0, -10.0}, {10.0, 0.0, 10.0}, {-10.0, 0.0, 10.0}};
            floor.triangles = {{0, 2, 1}, {0, 3, 2}};
            floor.uvs = std::vector<lv::Vec2>{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
            floor.colours = std::vector<lv::Colour>(4, lv::Colour{1.0, 1.0, 1.0, 1.0});
            floor.material = concrete;
            b.node("floor", b.mesh("floor", {floor}));
            lv::Primitive edge;
            edge.positions = {{-10.0, 0.0, 10.0}, {10.0, 0.0, 10.0}};
            edge.lines = {{0, 1}};
            b.node("edge#grind", b.mesh("edge", {edge}));
            b.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 0.0});
            const lv::Converted ai = lv::convertModel(b.model(), lv::Engine::Zge3);
            t.IsTrue(ai.files.count("ASSETS/LEVELS/TUT/TUT.GEO") && ai.files.count("ASSETS/LEVELS/TUT/TUT.BIN") &&
                         ai.files.count("ASSETS/LEVELS/TUT/MOTIONS.ZAL"),
                     "AI's slot: the level files, the scripts, the motions library");

            // That level as a map made in AI, in the maps folder.
            TempDir dir;
            const std::filesystem::path mapsFolder = dir.path / kMapsFolder, folder = mapsFolder / "park";
            for (const auto &[path, data] : ai.files)
            {
                std::filesystem::create_directories((folder / path).parent_path());
                std::ofstream(folder / path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            std::ofstream(folder / "map.ini") << "[map]\nname = Small Park\ngame = ai\nslot = TUT\n";
            const std::vector<CustomMap> maps = listCustomMaps(mapsFolder);
            t.IsTrue(maps.size() == 1 && maps[0].game == "ai" && mapPlayableIn(maps[0], "bmx") && mapNeedsConversion(maps[0], "bmx"),
                     "listed once, startable by BMX XXX's player, converted for it");
            if (maps.size() != 1)
                return;

            // BMX XXX's player: the map's files made into The Bronx's slot.
            const ConvertedMap bmx = convertMap(maps[0], "bmx");
            const std::string base = "ASSETS/LEVELS/CHL_MULY/CHL_MULY.";
            t.IsTrue(bmx.files.count(base + "GEO") && bmx.files.count(base + "ZIT") && bmx.files.count("ASSETS/LEVELS/CHL_MULY/OBJCTV.TXT") &&
                         bmx.files.count("ASSETS/LEVELS/CHL_MULY/MODELS.ZAL") && bmx.bytes > 0,
                     "the slot's files, its libraries and objectives, in memory");
            const lv::Geometry geo = lv::Geometry::read(bmx.files.at(base + "GEO"), lv::Engine::Zge4);
            const lv::Instances ins = lv::Instances::read(bmx.files.at(base + "INS"), lv::Engine::Zge4);
            const lv::SceneData dat = lv::SceneData::read(bmx.files.at(base + "DAT"), lv::Engine::Zge4);
            t.IsTrue(geo.write(lv::Engine::Zge4) == bmx.files.at(base + "GEO") && ins.write(lv::Engine::Zge4) == bmx.files.at(base + "INS"),
                     "BMX XXX's records read them back byte for byte");
            bool grind = false, tree = false;
            for (const lv::GeoObject &o : geo.objects)
            {
                tree = tree || o.collision.has_value();
                for (const lv::ObjectPart &p : o.parts)
                    grind = grind || p.part.edgeTags.has_value();
            }
            t.IsTrue(tree && grind, "the floor collides (a tree) and its edge grinds");
            t.IsTrue(std::any_of(dat.points.begin(), dat.points.end(), [](const lv::ScenePoint &p) { return p.name == lv::kPlayerOne; }),
                     "the PlayerOne spawn carried");

            // DM2's player: Woodward Camp's level row.
            const ConvertedMap dm2 = convertMap(maps[0], "dm2");
            t.IsTrue(dm2.files.count("ASSETS/LEVELS/WOODWARD/WOODWARD.GEO") && dm2.files.size() == 5,
                     "DM2's slot: the level row's five files");

            // A model that would not fit a game's memory is refused with why,
            // never started: 150,000 triangles of their own vertices (the
            // level files over BMX XXX's largest stock level's).
            lv::ModelBuilder big;
            lv::Primitive many;
            for (int i = 0; i < 150000; ++i)
            {
                const double x = (i % 400) * 0.5, z = (i / 400) * 0.5;
                const int at = static_cast<int>(many.positions.size());
                many.positions.push_back({x, 0.0, z});
                many.positions.push_back({x + 0.5, 0.0, z});
                many.positions.push_back({x, 0.0, z + 0.5});
                many.triangles.push_back({at, at + 2, at + 1});
                many.uvs = many.uvs.value_or(std::vector<lv::Vec2>{});
                for (int k = 0; k < 3; ++k)
                    many.uvs->push_back({i * 0.001 + k * 0.0001, 0.0});
            }
            many.material = big.material("concrete", {1.0, 1.0, 1.0, 1.0}, big.image("concrete", 16, 16, grey));
            big.node("ground", big.mesh("ground", {many}));
            big.node("PlayerOne#spawn=PlayerOne", std::nullopt, {0.0, 1.0, 0.0});
            std::string why;
            try
            {
                lv::convertModel(big.model(), lv::Engine::Zge4);
            }
            catch (const lv::LevelError &e)
            {
                why = e.what();
            }
            t.IsTrue(why.find("too big for its memory") != std::string::npos && why.find("8.7 at most") != std::string::npos,
                     "over BMX XXX's level bytes: refused, saying so (" + why + ")");
            // No PlayerOne: refused too.
            lv::ModelBuilder lost;
            lv::Primitive bare = floor;
            bare.material.reset();
            lost.node("floor", lost.mesh("floor", {bare}));
            why.clear();
            try
            {
                lv::convertModel(lost.model(), lv::Engine::Zge2);
            }
            catch (const lv::LevelError &e)
            {
                why = e.what();
            }
            t.IsTrue(why.find("PlayerOne") != std::string::npos, "no PlayerOne spawn: refused, saying so");
            t.IsTrue(!mapNeedsConversion(maps[0], "ai"), "AI's player reads the folder as it is");
        });

        tc.Run("settings: [mods] keeps the mods switched off, by their folders' names", [](TestCase &t)
        {
            std::vector<std::string> warnings;
            Settings s = parseSettings("[mods]\n20-Rails = off\nHD = Textures = off\n10-park = on\nx = maybe\n", &warnings);
            t.IsTrue(s.modsOff == std::vector<std::string>({"20-Rails", "HD = Textures"}),
                     "off ones by name, the case kept, a name with '=' to the last one");
            t.Equals(warnings.size(), size_t(1), "a value not on or off: a warning");
            setModOn(s, "05-first", false);
            setModOn(s, "20-Rails", true);
            setModOn(s, "05-first", false);
            t.IsTrue(s.modsOff == std::vector<std::string>({"05-first", "HD = Textures"}), "switched, in name order, once each");
            const GameSettingsProfile &dm2 = gameSettingsProfile("dm2");
            const std::string game = formatSettings(s, dm2, SettingsScope::Game);
            t.IsTrue(game.find("[mods]") != std::string::npos && game.find("05-first = off") != std::string::npos, "in the game's file");
            warnings.clear();
            t.IsTrue(parseSettings(game, &warnings).modsOff == s.modsOff && warnings.empty(), "read back");
            t.IsTrue(formatSettings(s, dm2, SettingsScope::App).find("[mods]") == std::string::npos, "not in the collection's root");
            t.IsTrue(parseSettings(formatSettings(s)).modsOff == s.modsOff, "the single-game app's file keeps them too");
            t.IsTrue(parseSettings(formatSettings(Settings{})).modsOff.empty(), "none by default: every mod on");
        });

        tc.Run("settings: the save profile", [](TestCase &t)
        {
            std::vector<std::string> warnings;
            Settings s = parseSettings("[saves]\nprofile = Alex 2\n", &warnings);
            t.Equals(s.saveProfile, std::string("Alex 2"), "parsed");
            t.IsTrue(warnings.empty(), "no warnings");
            t.Equals(parseSettings("").saveProfile, std::string("Career"), "default");
            t.Equals(parseSettings(formatSettings(s)).saveProfile, std::string("Alex 2"), "round trip");
        });

        tc.Run("settings: each game's own file and options", [](TestCase &t)
        {
            const GameSettingsProfile &ai = gameSettingsProfile("ai");
            const GameSettingsProfile &dm2 = gameSettingsProfile("dm2");
            const GameSettingsProfile &bmx = gameSettingsProfile("bmx");
            t.Equals(std::string(gameSettingsProfile("SLUS_201.59").id), std::string("dm2"), "by boot ELF");
            t.Equals(std::string(gameSettingsProfile("slus_204.15").id), std::string("bmx"), "any case");
            t.Equals(std::string(gameSettingsProfile("").id), std::string("ai"), "none: AI's");
            t.IsTrue(ai.has(GameOption::Zoo) && !dm2.has(GameOption::Zoo) && !bmx.has(GameOption::Zoo), "the Zoo is AI's");
            t.IsTrue(dm2.has(GameOption::Dm2XboxLevels) && !ai.has(GameOption::Dm2XboxLevels), "the Xbox levels are DM2's");

            Settings s;
            s.dm2XboxLevels = true;
            s.zoo = false;
            s.widescreen = true;
            const std::string dm2Game = formatSettings(s, dm2, SettingsScope::Game);
            t.IsTrue(dm2Game.rfind("# Dave Mirra Freestyle BMX 2 settings", 0) == 0, "DM2's header");
            t.IsTrue(dm2Game.find("dm2_xbox_levels = true") != std::string::npos, "its Xbox levels");
            for (const char *key : {"zoo", "ape", "estate", "tuning", "career_scripts", "xbox_free_skate", "language",
                                    "[display]", "Aggressive", "no goals or gaps"})
                t.IsTrue(dm2Game.find(key) == std::string::npos, std::string("not in DM2's file: ") + key);
            t.IsTrue(dm2Game.find("profile = ") != std::string::npos && dm2Game.find("pcsx2_import_offered") != std::string::npos,
                     "DM2's save profile and PCSX2 import offer");
            std::vector<std::string> warnings;
            t.IsTrue(parseSettings(dm2Game, &warnings).dm2XboxLevels && warnings.empty(), "read back");

            const std::string bmxGame = formatSettings(s, bmx, SettingsScope::Game);
            t.IsTrue(bmxGame.rfind("# BMX XXX settings", 0) == 0, "BMX's header");
            t.IsTrue(settingsKeys(bmxGame) ==
                         std::vector<std::string>{"general.language", "general.developer_menu", "gameplay.draw_distance", "gameplay.tuning",
                                                  "extras.park_editor", "extras.xbox_challenges", "extras.editor_pieces",
                                                  "extras.level_previews",
                                                  "extras.logo_movie", "extras.intro_movie", "extras.gc_levels",
                                                  "extras.menu_lines", "extras.all_mp_modes", "saves.profile",
                                                  "memcard.pcsx2_import_offered"},
                     "BMX's own options: its languages, the developer menu, the tuning, the park editor, its other "
                     "releases' content, the save profile, the PCSX2 import");
            t.IsTrue(!s.bmxXboxChallenges && !s.bmxEditorPieces && !s.bmxLevelPreviews && !s.bmxLogoMovie && !s.bmxIntroMovie &&
                         !s.bmxGcLevels && !s.bmxMenuLines && !s.bmxAllMpModes && bmxGame.find("gc_levels = false") != std::string::npos &&
                         bmxGame.find("all_mp_modes = false") != std::string::npos &&
                         bmxGame.find("menu_lines = false") != std::string::npos && s.tuning == 0 &&
                         s.language == 0 && bmxGame.find("xbox_challenges = false") != std::string::npos &&
                         bmxGame.find("intro_movie = false") != std::string::npos &&
                         bmxGame.find("tuning = ps2") != std::string::npos,
                     "the other releases' content off by default (the game as the PS2 release made it)");
            Settings releases;
            releases.bmxXboxChallenges = releases.bmxEditorPieces = releases.bmxLevelPreviews = releases.bmxLogoMovie = releases.bmxIntroMovie = true;
            releases.bmxGcLevels = releases.bmxMenuLines = releases.bmxAllMpModes = true;
            const Settings releasesRead = parseSettings(formatSettings(releases, bmx, SettingsScope::Game), &warnings);
            t.IsTrue(releasesRead.bmxXboxChallenges && releasesRead.bmxEditorPieces && releasesRead.bmxLevelPreviews && releasesRead.bmxLogoMovie &&
                         releasesRead.bmxIntroMovie && releasesRead.bmxGcLevels && releasesRead.bmxMenuLines &&
                         releasesRead.bmxAllMpModes && warnings.empty(),
                     "[extras] xbox_challenges, editor_pieces, level_previews, logo_movie, intro_movie, gc_levels, menu_lines, "
                     "all_mp_modes read back");
            t.IsTrue(bmxGame.find("Syracuse") != std::string::npos && bmxGame.find("Civic Center") == std::string::npos,
                     "BMX's tuning line says what it changes there");
            t.IsTrue(bmxGame.find("draw_distance = original") != std::string::npos && bmxGame.find("changes the game") != std::string::npos &&
                         dm2Game.find("draw_distance = original") != std::string::npos && dm2Game.find("changes the game") != std::string::npos,
                     "BMX's and DM2's draw distance: their own, Original by default, and it says it changes the game");
            t.IsTrue(bmx.drawDistanceChangesGame && dm2.drawDistanceChangesGame && ai.drawDistanceChangesGame,
                     "the games whose draw distance changes them: all three");
            t.IsTrue(!s.bmxParkEditor && bmxGame.find("park_editor = false") != std::string::npos,
                     "the park editor off by default (the main menu as the game made it)");
            t.IsTrue(dm2Game.find("park_editor") == std::string::npos && !dm2.has(GameOption::BmxParkEditor) &&
                         !ai.has(GameOption::BmxParkEditor),
                     "the park editor line is BMX XXX's");
            t.IsTrue(!s.dm2CreatePlayer && dm2Game.find("create_player = false") != std::string::npos &&
                         bmxGame.find("create_player") == std::string::npos && !ai.has(GameOption::Dm2CreatePlayer),
                     "Create Player is DM2's, off by default (the menus as the game shipped)");
            Settings create;
            create.dm2CreatePlayer = true;
            t.IsTrue(parseSettings(formatSettings(create, dm2, SettingsScope::Game), &warnings).dm2CreatePlayer && warnings.empty(),
                     "[extras] create_player read back");
            t.IsTrue(!s.dm2XboxThemes && !s.dm2RetroMovie && !s.dm2AttractDemo &&
                         dm2Game.find("xbox_themes = false") != std::string::npos &&
                         dm2Game.find("retro_movie = false") != std::string::npos &&
                         dm2Game.find("attract_demo = false") != std::string::npos &&
                         bmxGame.find("xbox_themes") == std::string::npos && !ai.has(GameOption::Dm2AttractDemo),
                     "DM2's Xbox themes, Retro movie and attract demo: DM2's, off by default");
            Settings extras;
            extras.dm2XboxThemes = extras.dm2RetroMovie = extras.dm2AttractDemo = true;
            const Settings extrasRead = parseSettings(formatSettings(extras, dm2, SettingsScope::Game), &warnings);
            t.IsTrue(extrasRead.dm2XboxThemes && extrasRead.dm2RetroMovie && extrasRead.dm2AttractDemo && warnings.empty(),
                     "[extras] xbox_themes, retro_movie, attract_demo read back");
            Settings park;
            park.bmxParkEditor = true;
            t.IsTrue(parseSettings(formatSettings(park, bmx, SettingsScope::Game), &warnings).bmxParkEditor && warnings.empty(),
                     "[extras] park_editor read back");

            const std::string aiGame = formatSettings(s, ai, SettingsScope::Game);
            t.IsTrue(aiGame.find("zoo = false") != std::string::npos && aiGame.find("language = ") != std::string::npos &&
                         aiGame.find("[display]") == std::string::npos && aiGame.find("dm2_xbox_levels") == std::string::npos,
                     "AI's file: its options, not the app's or DM2's");

            const std::string root = formatSettings(s, dm2, SettingsScope::App);
            for (const std::string &key : settingsKeys(root))
            {
                const size_t dot = key.find('.');
                t.IsTrue(appWideKey(key.substr(0, dot), key.substr(dot + 1)), "the root holds the app's keys only: " + key);
            }
            t.IsTrue(root.find("widescreen = true") != std::string::npos && root.find("motion_blur") != std::string::npos &&
                         root.find("skip_intro_movies") != std::string::npos,
                     "every game's shared values, whichever game wrote it");
            t.IsTrue(root.find("draw_distance") == std::string::npos && aiGame.find("draw_distance = original\n") != std::string::npos &&
                         aiGame.find("changes the game") != std::string::npos,
                     "the draw distance is each game's own, not the root's; AI's Original by default, and it says it changes the game");

            const std::string bmxAlone = formatSettings(s, bmx, SettingsScope::All);
            t.IsTrue(bmxAlone.find("widescreen") != std::string::npos && bmxAlone.find("motion_blur") != std::string::npos &&
                         bmxAlone.find("draw_distance") != std::string::npos && bmxAlone.find("vsync") != std::string::npos &&
                         bmxAlone.find("career_scripts") == std::string::npos,
                     "a single-game file: the app's settings the game acts on");
            t.IsTrue(bmx.has(GameOption::MovieVolume) && bmx.has(GameOption::SkipIntroMovies) &&
                         bmxAlone.find("movie_volume") != std::string::npos &&
                         bmxAlone.find("skip_intro_movies") != std::string::npos,
                     "BMX's movies play on the host: their volume, and the intro movie's skip");

            // An older DM2 file carried AI's options: read quietly, not written back.
            warnings.clear();
            const std::string older = "# Aggressive Inline settings.\n[gameplay]\ntuning = xbox\ncareer_scripts = ps2\n"
                                      "[extras]\nzoo = true\nape = true\ndm2_xbox_levels = true\n";
            const Settings olderRead = parseSettings(older, &warnings);
            t.IsTrue(warnings.empty() && olderRead.dm2XboxLevels, "an older DM2 file reads without warnings");
            t.IsTrue(formatSettings(olderRead, dm2, SettingsScope::Game).find("tuning") == std::string::npos,
                     "and is written back with DM2's options only");
        });

        tc.Run("collection: the root's and the game's settings files", [](TestCase &t)
        {
            TempDir dir;
            const auto rootFile = dir.path / "settings.ini";
            const auto dm2File = dir.path / "games" / "dm2" / "settings.ini";
            Settings s;
            s.volume = 40;
            s.vsync = false;
            s.dm2XboxLevels = true;
            t.IsTrue(saveCollectionSettings(rootFile, dm2File, s, gameSettingsProfile("dm2")), "saved");
            Settings back;
            std::vector<std::string> warnings;
            t.IsTrue(loadCollectionSettings(rootFile, dm2File, back, &warnings), "both files there");
            t.IsTrue(warnings.empty() && back.volume == 40 && !back.vsync && back.dm2XboxLevels, "read back from both");
            std::string game;
            t.IsTrue(readTextFile(dm2File, game) && game.find("volume") == std::string::npos, "the game's file has no app settings");

            // First start of another game: its file is new, the app's are shared.
            const auto bmxFile = dir.path / "games" / "bmx" / "settings.ini";
            Settings bmx;
            t.IsFalse(loadCollectionSettings(rootFile, bmxFile, bmx), "BMX's file is new");
            t.IsTrue(bmx.volume == 40 && !bmx.vsync, "with the collection's settings");

            // Before the root has the app's settings: those of the game's own
            // (copied from its single-game app).
            TempDir fresh;
            writeTextFile(fresh.path / "games" / "ai" / "settings.ini", "[display]\nvsync = false\n[extras]\nzoo = false\n");
            Settings first;
            t.IsFalse(loadCollectionSettings(fresh.path / "settings.ini", fresh.path / "games" / "ai" / "settings.ini", first),
                      "no root file yet");
            t.IsTrue(!first.vsync && !first.zoo, "the game's file gives both");
        });

        tc.Run("collection: an earlier version's shared files move once", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path root = dir.path;
            const std::string shared = "# the user's shared controls\n[buttons]\ncross = X\n";
            writeTextFile(root / "controls.ini", shared);
            writeTextFile(root / "games" / "dm2" / "controls.ini", "# copied from DM2's own app\n");
            writeTextFile(root / "games" / "ai" / "controls.ini", "# copied from AI's own app\n");
            writeTextFile(root / "settings.ini", "[display]\nvsync = false\n[general]\nlog_file = false\n"
                                                 "language = french\ndeveloper_menu = true\n");
            writeTextFile(root / "games" / "ai" / "settings.ini", "[extras]\nzoo = false\n[general]\ndeveloper_menu = false\n");
            std::vector<std::string> log;
            migrateCollectionFiles(root, "ai", {"ai", "dm2", "bmx"}, &log);

            std::string text;
            t.IsTrue(readTextFile(root / "games" / "ai" / "controls.ini", text) && text == shared, "AI keeps the shared controls");
            t.IsFalse(std::filesystem::exists(root / "controls.ini"), "no shared controls.ini left");
            t.IsTrue(readTextFile(root / "games" / "ai" / "controls.ini.bak", text) && text == "# copied from AI's own app\n",
                     "AI's unused copy kept aside");
            t.IsFalse(std::filesystem::exists(root / "games" / "dm2" / "controls.ini"), "DM2 starts from its own defaults");
            t.IsTrue(std::filesystem::exists(root / "games" / "dm2" / "controls.ini.bak"), "its old copy kept aside");
            t.IsFalse(std::filesystem::exists(root / "games" / "bmx"), "nothing made for BMX");

            Settings ai;
            t.IsTrue(loadCollectionSettings(root / "settings.ini", root / "games" / "ai" / "settings.ini", ai), "AI's settings");
            t.IsTrue(ai.language == 1 && !ai.developerMenu && !ai.zoo && !ai.vsync && !ai.logFile,
                     "the root's game options went to AI (its own value wins), the app's stayed");
            t.IsTrue(readTextFile(root / "settings.ini", text) && text.find("language") == std::string::npos &&
                         text.find("vsync = false") != std::string::npos,
                     "the root keeps the app's settings only");
            t.IsTrue(log.size() == 4u, "four moves logged");

            log.clear();
            writeTextFile(root / "games" / "dm2" / "controls.ini", "# DM2's own now\n");
            migrateCollectionFiles(root, "ai", {"ai", "dm2", "bmx"}, &log);
            t.IsTrue(log.empty() && readTextFile(root / "games" / "dm2" / "controls.ini", text) && text == "# DM2's own now\n",
                     "once only: the next start changes nothing");

            // An earlier version's app-wide draw distance: to every game, when picked.
            TempDir picked;
            writeTextFile(picked.path / "settings.ini", "[display]\nvsync = false\n[graphics]\nmotion_blur = low\ndraw_distance = farthest\n");
            writeTextFile(picked.path / "games" / "dm2" / "settings.ini", "[extras]\ncreate_player = true\n");
            log.clear();
            migrateCollectionFiles(picked.path, "ai", {"ai", "dm2", "bmx"}, &log);
            Settings pickedAi, pickedDm2, pickedBmx;
            loadCollectionSettings(picked.path / "settings.ini", picked.path / "games" / "ai" / "settings.ini", pickedAi);
            loadCollectionSettings(picked.path / "settings.ini", picked.path / "games" / "dm2" / "settings.ini", pickedDm2);
            loadCollectionSettings(picked.path / "settings.ini", picked.path / "games" / "bmx" / "settings.ini", pickedBmx);
            t.IsTrue(drawDistanceOf(pickedAi, gameSettingsProfile("ai")) == 2 && pickedAi.drawDistance == 2, "AI keeps the pick, as its own");
            t.IsTrue(drawDistanceOf(pickedDm2, gameSettingsProfile("dm2")) == 2 && pickedDm2.drawDistance == 2 && pickedDm2.dm2CreatePlayer &&
                         drawDistanceOf(pickedBmx, gameSettingsProfile("bmx")) == 2 && pickedBmx.drawDistance == 2,
                     "DM2 and BMX XXX keep it too, as their own, DM2's file otherwise as it was");
            t.IsTrue(readTextFile(picked.path / "settings.ini", text) && text.find("draw_distance") == std::string::npos &&
                         text.find("motion_blur = low") != std::string::npos && text.find("vsync = false") != std::string::npos,
                     "the root drops it and keeps the rest");
            t.IsTrue(log.size() == 4u, "the three moves and the drop logged");
            TempDir unpicked;
            writeTextFile(unpicked.path / "settings.ini", "[graphics]\ndraw_distance = far\n");
            log.clear();
            migrateCollectionFiles(unpicked.path, "ai", {"ai", "dm2", "bmx"}, &log);
            t.IsTrue(log.size() == 1u && !std::filesystem::exists(unpicked.path / "games"),
                     "Far, the earlier default, is not a pick: nothing carried, the root's line dropped");
            // The earlier version's file as its writer wrote it: the line goes
            // with its own comment, every other line stays.
            TempDir commented;
            const std::string earlier = "# A user's note\n\n[graphics]\n"
                                        "# The game's blend of the previous frame in levels: off | low | original.\n"
                                        "motion_blur = low\n"
                                        "# How far the view reaches before the fog: original | far | farthest (split screen\n"
                                        "# 100 -> 250 -> 1000; single player is at the game's maximum already).\n"
                                        "draw_distance = farthest\n"
                                        "# GPU renderer picture quality, beyond the PS2 (native render path):\n"
                                        "anti_aliasing = off\n";
            writeTextFile(commented.path / "settings.ini", earlier);
            migrateCollectionFiles(commented.path, "ai", {"ai"}, nullptr);
            t.IsTrue(readTextFile(commented.path / "settings.ini", text) &&
                         text == "# A user's note\n\n[graphics]\n"
                                 "# The game's blend of the previous frame in levels: off | low | original.\n"
                                 "motion_blur = low\n"
                                 "# GPU renderer picture quality, beyond the PS2 (native render path):\n"
                                 "anti_aliasing = off\n",
                     "the root's draw distance goes with its two comment lines; the other keys' comments and the user's stay");
            TempDir first;
            writeTextFile(first.path / "settings.ini", "[graphics]\n# How far the view reaches\ndraw_distance = far\n\n# kept\n[display]\nvsync = false\n");
            migrateCollectionFiles(first.path, "ai", {"ai"}, nullptr);
            t.IsTrue(readTextFile(first.path / "settings.ini", text) && text == "[graphics]\n\n# kept\n[display]\nvsync = false\n",
                     "the first key of its section: its comment goes, the section line and what follows stay");

            TempDir none;
            log.clear();
            migrateCollectionFiles(none.path, "ai", {"ai", "dm2"}, &log);
            t.IsTrue(log.empty() && !std::filesystem::exists(none.path / "games"), "a new collection: nothing to move");
        });
    });
}
