#include "MiniTest.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zip_archive.h"
#include "test_iso_builder.h"
#include "test_zip_builder.h"
#include "ps2x/iop/iso9660.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ps2x::game_files;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("ps2x-gamefiles-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void put(const std::filesystem::path &path, const std::string &text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::string contents(GameFile &file)
    {
        std::vector<uint8_t> bytes;
        readAll(file, bytes);
        return std::string(bytes.begin(), bytes.end());
    }
}

void register_ps2_game_files_tests()
{
    MiniTest::Case("GameFiles", [](TestCase &tc)
    {
        tc.Run("paths", [](TestCase &t)
        {
            t.Equals(normalizePath("dvd0:\\ASSETS\\UILIB.ZAL;1"), std::string("ASSETS/UILIB.ZAL"), "device, version, separators");
            t.Equals(normalizePath("assets\\audio\\SFXCOM.ZSD"), std::string("ASSETS/AUDIO/SFXCOM.ZSD"), "upper case");
            t.Equals(normalizePath("/a//b/./c"), std::string("A/B/C"), "empty and dot components");
            t.Equals(normalizePath("../../a/../b"), std::string("B"), "dot-dot stays inside");
            t.Equals(normalizePath("cdrom0:"), std::string(), "device only");
        });

        tc.Run("disc folder, any case", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "Scene.Txt", "scene");
            GameFileSystem files({dir.path / "disc", nullptr, {}});
            auto file = files.open("assets\\SCENE.TXT");
            t.IsTrue(file != nullptr, "found ignoring case");
            if (!file)
                return;
            t.Equals(file->size(), uint64_t(5), "size");
            t.IsTrue(file->origin() == GameFile::Origin::Disc, "from the disc");
            char buffer[8] = {};
            t.Equals(file->read(2, buffer, 8), size_t(3), "read clamped at the end");
            t.Equals(std::string(buffer, 3), std::string("ene"), "bytes at the offset");
            t.Equals(file->read(5, buffer, 1), size_t(0), "nothing past the end");
            t.IsTrue(files.open("ASSETS/MISSING") == nullptr, "missing file");
            t.IsTrue(files.open("ASSETS") == nullptr, "a directory is not a file");
            t.IsTrue(files.exists("assets"), "the directory exists");
            t.IsTrue(files.exists("ASSETS/SCENE.TXT"), "the file exists");
            t.IsFalse(files.exists("ASSETS/NOPE.TXT"), "no such file");
        });

        tc.Run("mods folder wins", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "STRING.EUS", "original");
            put(dir.path / "disc" / "ASSETS" / "OTHER.BIN", "disc only");
            put(dir.path / "mods" / "assets" / "string.eus", "modded");
            GameFileSystem files({dir.path / "disc", nullptr, {dir.path / "mods"}});
            auto file = files.open("dvd0:\\ASSETS\\STRING.EUS");
            t.IsTrue(file && file->origin() == GameFile::Origin::Mod, "the mod's file");
            if (file)
                t.Equals(contents(*file), std::string("modded"), "mod contents");
            auto other = files.open("ASSETS/OTHER.BIN");
            t.IsTrue(other && other->origin() == GameFile::Origin::Disc, "others from the disc");
        });

        tc.Run("mod folders in order: the last one's file wins", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "A.BIN", "disc a");
            put(dir.path / "disc" / "ASSETS" / "B.BIN", "disc b");
            put(dir.path / "disc" / "ASSETS" / "C.BIN", "disc c");
            put(dir.path / "mods" / "ASSETS" / "A.BIN", "loose a");
            put(dir.path / "mods" / "10-first" / "ASSETS" / "A.BIN", "first a");
            put(dir.path / "mods" / "10-first" / "assets" / "b.bin", "first b");
            put(dir.path / "mods" / "20-second" / "ASSETS" / "B.BIN", "second b");
            const std::string text = modsText({dir.path / "mods", dir.path / "mods" / "10-first", dir.path / "mods" / "20-second"});
            const std::vector<std::filesystem::path> roots = modsRoots(text);
            t.Equals(roots.size(), size_t(3), "three folders from the text");
            t.IsTrue(roots.size() == 3 && roots[2] == dir.path / "mods" / "20-second", "in the text's order");
            GameFileSystem files({dir.path / "disc", nullptr, roots});
            auto a = files.open("ASSETS/A.BIN");
            t.IsTrue(a && contents(*a) == "first a", "the later mod's file over the loose one");
            auto b = files.open("ASSETS/B.BIN");
            t.IsTrue(b && contents(*b) == "second b", "the last mod's file over the earlier one's");
            auto c = files.open("ASSETS/C.BIN");
            t.IsTrue(c && c->origin() == GameFile::Origin::Disc && contents(*c) == "disc c", "the disc's where no mod has it");
            t.IsTrue(files.exists("assets/b.bin"), "exists sees every mod");
            t.IsTrue(modsRoots("").empty(), "no text, no folders");
        });

        tc.Run("disc image", [](TestCase &t)
        {
            TempDir dir;
            const std::filesystem::path iso = dir.path / "disc.iso";
            test_iso::write(iso, {{"SYSTEM.CNF", test_iso::bytesOf("BOOT2")}, {"ASSETS/LEVEL.BIN", test_iso::bytesOf("level data")}});
            auto image = std::make_shared<ps2x::iop::Iso9660Image>();
            t.IsTrue(image->open(iso), "image opens");
            GameFileSystem files({{}, image, {}});
            auto file = files.open("assets\\level.bin");
            t.IsTrue(file != nullptr, "found in the image");
            if (file)
                t.Equals(contents(*file), std::string("level data"), "image contents");
            t.IsTrue(files.exists("ASSETS"), "directory in the image");
        });

        tc.Run("in-memory patch on top", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "UILIB.ZAL", "menu");
            put(dir.path / "mods" / "ASSETS" / "UILIB.ZAL", "mod menu");
            ps2x::disc_patch::clearPatches();
            ps2x::disc_patch::registerPatch("UILIB.ZAL", [](std::vector<uint8_t> &bytes)
            {
                const std::string extra = " + settings";
                bytes.insert(bytes.end(), extra.begin(), extra.end());
                return true;
            });
            GameFileSystem files({dir.path / "disc", nullptr, {dir.path / "mods"}});
            auto file = files.open("assets\\uilib.zal");
            t.IsTrue(file && file->origin() == GameFile::Origin::Patched, "patched");
            if (file)
                t.Equals(contents(*file), std::string("mod menu + settings"), "patch applied to the mod's file");
            ps2x::disc_patch::clearPatches();
        });

        tc.Run("two patches for one file run in order", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "UILIB.ZAL", "menu");
            ps2x::disc_patch::clearPatches();
            ps2x::disc_patch::registerPatch("UILIB.ZAL", [](std::vector<uint8_t> &bytes)
            {
                bytes.push_back('1');
                return true;
            });
            ps2x::disc_patch::registerPatch("uilib.zal", [](std::vector<uint8_t> &bytes)
            {
                bytes.push_back('2');
                return true;
            });
            GameFileSystem files({dir.path / "disc", nullptr, {}});
            auto file = files.open("ASSETS/UILIB.ZAL");
            t.IsTrue(file != nullptr, "opened");
            if (file)
                t.Equals(contents(*file), std::string("menu12"), "first patch, then the second");
            ps2x::disc_patch::clearPatches();
        });

        tc.Run("a zip mod is read in place, over the folders before it", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "A.BIN", "disc a");
            put(dir.path / "disc" / "ASSETS" / "B.BIN", "disc b");
            put(dir.path / "mods" / "10-first" / "ASSETS" / "A.BIN", "first a");
            put(dir.path / "mods" / "10-first" / "ASSETS" / "B.BIN", "first b");
            t.IsTrue(test_zip::write(dir.path / "mods" / "20-packed.zip",
                                     {{"assets/a.bin", "zipped a"}, {"Music/01 Song.wav", "RIFF"}, {"__MACOSX/assets/._a.bin", "x"}}),
                     "zip written");
            GameFileSystem files({dir.path / "disc", nullptr, {dir.path / "mods" / "10-first", dir.path / "mods" / "20-packed.zip"}});
            auto a = files.open("ASSETS/A.BIN");
            t.IsTrue(a && a->origin() == GameFile::Origin::Mod, "the zip's file");
            if (a)
                t.Equals(contents(*a), std::string("zipped a"), "inflated");
            auto b = files.open("ASSETS/B.BIN");
            if (b)
                t.Equals(contents(*b), std::string("first b"), "a file the zip has not: the folder before it");
            t.IsTrue(files.exists("ASSETS") && files.exists("assets/a.bin"), "its folders and files exist");
            t.IsTrue(files.open("__MACOSX/ASSETS/._A.BIN") == nullptr, "the Mac's __MACOSX left out");
        });

        tc.Run("zip archives: one top folder, files below a folder, stored and deflated, nothing outside", [](TestCase &t)
        {
            TempDir dir;
            t.IsTrue(test_zip::write(dir.path / "hd.zip",
                                     {{"hd/mod.ini", "[mod]"}, {"hd/Textures/ai/b/2.png", "two"}, {"hd/Textures/ai/1.png", "one"},
                                      {"hd/Textures/dm2/3.png", "three"}, {"hd/.DS_Store", "x"}},
                                     false),
                     "zip written (stored)");
            std::shared_ptr<ZipArchive> zip = ZipArchive::open(dir.path / "hd.zip");
            t.IsTrue(zip != nullptr, "opened");
            if (!zip)
                return;
            t.Equals(zip->files(), size_t(4), "four files: .DS_Store left out");
            t.IsTrue(zip->hasFile("MOD.INI") && zip->hasFolder("TEXTURES/AI") && !zip->hasFolder("TEXTURES/BMX"),
                     "read from inside its one folder");
            t.IsTrue(ZipArchive::open(dir.path / "hd.zip") == zip, "the same zip opened twice: one archive");
            const std::vector<std::string> below = listFilesBelow(dir.path / "hd.zip" / "Textures" / "ai");
            t.Equals(below.size(), size_t(2), "two files below Textures/ai");
            if (below.size() == 2)
                t.IsTrue(below[0] == "1.png" && below[1] == "b/2.png", "relative, the zip's case, in order");
            std::vector<uint8_t> bytes;
            t.IsTrue(readFileBelow(dir.path / "hd.zip" / "Textures" / "ai", "b/2.png", bytes) &&
                         std::string(bytes.begin(), bytes.end()) == "two",
                     "one read through the zip");
            t.IsFalse(readFileBelow(dir.path / "hd.zip" / "Textures" / "ai", "../dm2/3.png", bytes), "never outside the folder");
            put(dir.path / "loose" / "Textures" / "ai" / "x" / "4.png", "four");
            const std::vector<std::string> loose = listFilesBelow(dir.path / "loose" / "Textures" / "ai");
            t.IsTrue(loose.size() == 1 && loose[0] == "x/4.png", "a folder the same way");
            t.IsTrue(readFileBelow(dir.path / "loose" / "Textures" / "ai", "X/4.PNG", bytes) &&
                         std::string(bytes.begin(), bytes.end()) == "four",
                     "read from a folder, any case");
            t.IsTrue(test_zip::write(dir.path / "slip.zip", {{"../evil.bin", "x"}, {"ok.bin", "ok"}}), "zip with a .. name");
            std::shared_ptr<ZipArchive> slip = ZipArchive::open(dir.path / "slip.zip");
            t.IsTrue(slip && slip->files() == 1 && slip->hasFile("OK.BIN"), "a name with .. is not one of its files");
            put(dir.path / "bad.zip", "not a zip");
            std::string why;
            t.IsTrue(ZipArchive::open(dir.path / "bad.zip", &why) == nullptr && !why.empty(), "not a zip: null, with why");
        });

        tc.Run("providers: after mods, before the disc", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "LEVELS" / "ZOO" / "ZOO.TXR", "stub");
            put(dir.path / "disc" / "ASSETS" / "LEVELS" / "ZOO" / "ZOO.GEO", "disc geo");
            put(dir.path / "mods" / "ASSETS" / "LEVELS" / "ZOO" / "ZOO.INS", "mod ins");
            clearProviders();
            int asked = 0;
            registerProvider("assets/levels/zoo", [&asked](const std::string &path) -> std::shared_ptr<const std::vector<uint8_t>>
            {
                ++asked;
                if (path == "ASSETS/LEVELS/ZOO/ZOO.TXR" || path == "ASSETS/LEVELS/ZOO/ZOO.INS" ||
                    path == "ASSETS/LEVELS/ZOO/NEW.BIN")
                {
                    const std::string text = "made " + path.substr(path.rfind('/') + 1);
                    return std::make_shared<const std::vector<uint8_t>>(text.begin(), text.end());
                }
                return nullptr;
            }, "test");
            GameFileSystem files({dir.path / "disc", nullptr, {dir.path / "mods"}});
            auto txr = files.open("assets/levels/ZOO/zoo.TXR");
            t.IsTrue(txr && txr->origin() == GameFile::Origin::Provided, "provided over the disc");
            if (txr)
            {
                t.Equals(contents(*txr), std::string("made ZOO.TXR"), "provider's contents");
                t.Equals(txr->source(), std::string("test:ASSETS/LEVELS/ZOO/ZOO.TXR"), "source names the provider");
            }
            auto ins = files.open("ASSETS/LEVELS/ZOO/ZOO.INS");
            t.IsTrue(ins && ins->origin() == GameFile::Origin::Mod, "a mod file wins");
            auto geo = files.open("ASSETS/LEVELS/ZOO/ZOO.GEO");
            t.IsTrue(geo && geo->origin() == GameFile::Origin::Disc, "null leaves it to the disc");
            t.IsTrue(files.exists("ASSETS/LEVELS/ZOO/NEW.BIN"), "a file only the provider has exists");
            t.IsTrue(files.open("ASSETS/LEVELS/ZOO/NEW.BIN") != nullptr, "and opens");
            const int before = asked;
            t.IsTrue(files.open("ASSETS/LEVELS/CIVIC/CIVIC.TXR") == nullptr, "other folders");
            t.Equals(asked, before, "not asked outside its folder");
            clearProviders();
        });

        tc.Run("memory folders: none registered reads as before; one is read over the mods", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "LEVELS" / "TUT" / "TUT.GEO", "disc geo");
            put(dir.path / "disc" / "ASSETS" / "LEVELS" / "TUT" / "TUT.TXR", "disc txr");
            put(dir.path / "disc" / "ASSETS" / "UILIB.ZAL", "menu");
            put(dir.path / "mods" / "ASSETS" / "LEVELS" / "TUT" / "TUT.TXR", "mod txr");
            const GameFileRoots roots{dir.path / "disc", nullptr, {dir.path / "mods"}};
            const std::vector<std::string> paths = {"ASSETS/LEVELS/TUT/TUT.GEO", "ASSETS/LEVELS/TUT/TUT.TXR", "ASSETS/UILIB.ZAL",
                                                    "ASSETS/LEVELS/TUT/TUT.INS", "PARK.DAT"};
            // What every path gives with nothing registered: origin and bytes.
            auto read = [&roots, &paths]()
            {
                GameFileSystem files(roots);
                std::vector<std::string> out;
                for (const std::string &path : paths)
                {
                    auto file = files.open(path);
                    out.push_back(file ? std::to_string(static_cast<int>(file->origin())) + ":" + file->source() + ":" + contents(*file)
                                       : std::string("none"));
                    out.push_back(files.exists(path) ? "exists" : "absent");
                }
                out.push_back(files.exists("ASSETS/LEVELS/TUT") ? "folder" : "no folder");
                out.push_back(files.exists("ASSETS/EDITOR") ? "folder" : "no folder");
                return out;
            };
            const std::vector<std::string> before = read();
            t.IsTrue(before[2].find("mod txr") != std::string::npos && before[6] == "none" && before[8] == "none" && before[11] == "no folder",
                     "as before: the mod's TXR, no INS, no park, no EDITOR folder");

            auto bytes = [](const std::string &text) { return std::make_shared<const std::vector<uint8_t>>(text.begin(), text.end()); };
            registerMemoryFolder("test-map", {{"assets/levels/tut/TUT.TXR", bytes("memory txr")},
                                              {"ASSETS/LEVELS/TUT/TUT.INS", bytes("memory ins")},
                                              {"ASSETS/EDITOR/LEWAREHO/LEWAREHO.GEO", bytes("memory park geo")},
                                              {"park.dat", bytes("park")}});
            {
                GameFileSystem files(roots);
                auto txr = files.open("ASSETS/LEVELS/TUT/TUT.TXR");
                t.IsTrue(txr && txr->origin() == GameFile::Origin::Mod && contents(*txr) == "memory txr" &&
                             txr->source() == "memory:test-map:ASSETS/LEVELS/TUT/TUT.TXR",
                         "the memory folder's file over the mod's, its source naming the folder");
                auto ins = files.open("assets\\levels\\tut\\tut.ins;1");
                t.IsTrue(ins && contents(*ins) == "memory ins", "a file only the memory folder has, by any form of its path");
                auto geo = files.open("ASSETS/LEVELS/TUT/TUT.GEO");
                t.IsTrue(geo && geo->origin() == GameFile::Origin::Disc, "the rest from the disc");
                auto park = files.open("PARK.DAT");
                t.IsTrue(park && contents(*park) == "park", "a top-level file");
                t.IsTrue(files.exists("ASSETS/EDITOR") && files.exists("ASSETS/EDITOR/LEWAREHO") && !files.exists("ASSETS/EDITOR/LEWAREH"),
                         "its folders exist, a folder name's prefix does not");
            }
            registerMemoryFolder("test-map", {{"ASSETS/LEVELS/TUT/TUT.INS", bytes("again")}});
            {
                GameFileSystem files(roots);
                auto ins = files.open("ASSETS/LEVELS/TUT/TUT.INS");
                auto txr = files.open("ASSETS/LEVELS/TUT/TUT.TXR");
                t.IsTrue(ins && contents(*ins) == "again" && txr && contents(*txr) == "mod txr", "registered again: replaced, not added to");
            }
            removeMemoryFolder("test-map");
            t.IsTrue(read() == before, "removed: every path reads as with nothing registered");
        });

        tc.Run("patches and providers are the boot's", [](TestCase &t)
        {
            // Games booted one after another in one process: each registers
            // its own at its install, and sees none of the one before.
            TempDir dir;
            put(dir.path / "disc" / "ASSETS" / "UILIB.ZAL", "menu");
            put(dir.path / "disc" / "ASSETS" / "LEVELS" / "ZOO" / "ZOO.TXR", "stub");
            auto mark = [](std::vector<uint8_t> &bytes)
            {
                bytes.push_back('!');
                return true;
            };
            auto made = [](const std::string &) -> std::shared_ptr<const std::vector<uint8_t>>
            {
                const std::string text = "made";
                return std::make_shared<const std::vector<uint8_t>>(text.begin(), text.end());
            };
            {
                ps2x::boot::Scope first(ps2x::BootConfig{});
                ps2x::disc_patch::registerPatch("UILIB.ZAL", mark);
                registerProvider("assets/levels/zoo", made, "test");
                GameFileSystem files({dir.path / "disc", nullptr, {}});
                auto ui = files.open("ASSETS/UILIB.ZAL");
                t.IsTrue(ui && ui->origin() == GameFile::Origin::Patched, "the first boot's patch");
                auto txr = files.open("ASSETS/LEVELS/ZOO/ZOO.TXR");
                t.IsTrue(txr && txr->origin() == GameFile::Origin::Provided, "the first boot's provider");
            }
            ps2x::boot::Scope second(ps2x::BootConfig{});
            t.IsTrue(!ps2x::disc_patch::hasPatch("UILIB.ZAL"), "no patch in the next boot");
            {
                GameFileSystem files({dir.path / "disc", nullptr, {}});
                auto ui = files.open("ASSETS/UILIB.ZAL");
                t.IsTrue(ui && ui->origin() == GameFile::Origin::Disc, "the disc's file");
                if (ui)
                    t.Equals(contents(*ui), std::string("menu"), "unpatched");
                auto txr = files.open("ASSETS/LEVELS/ZOO/ZOO.TXR");
                t.IsTrue(txr && txr->origin() == GameFile::Origin::Disc, "no provider in the next boot");
            }
            // The same install again: one patch, not the first boot's chained
            // with this one's.
            ps2x::disc_patch::registerPatch("UILIB.ZAL", mark);
            GameFileSystem files({dir.path / "disc", nullptr, {}});
            auto ui = files.open("ASSETS/UILIB.ZAL");
            if (ui)
                t.Equals(contents(*ui), std::string("menu!"), "patched once");
            ps2x::disc_patch::clearPatches();
        });
    });
}
