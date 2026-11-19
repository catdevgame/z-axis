// The first-run setup (collection/game_setup.h): disc images and folders read,
// checked, installed, checked again and repaired, on small made-up discs.
#include "MiniTest.h"
#include "collection/disc_check.h"
#include "collection/disc_finder.h"
#include "collection/game_setup.h"
#include "collection/save_transfer.h"
#include "ps2x/iop/iso9660.h"
#include "test_iso_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::app;

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("ps2x-setup-" + std::to_string(now));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    std::vector<uint8_t> elfBytes(uint8_t seed)
    {
        std::vector<uint8_t> elf(9000u);
        elf[0] = 0x7F;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        for (size_t i = 4; i < elf.size(); ++i)
            elf[i] = static_cast<uint8_t>(i * 13u + seed);
        return elf;
    }

    std::vector<test_iso::File> discFiles(const std::string &elf, uint8_t seed = 1)
    {
        std::vector<uint8_t> level(70000u);
        for (size_t i = 0; i < level.size(); ++i)
            level[i] = static_cast<uint8_t>(i * 7u);
        return {
            {"SYSTEM.CNF", test_iso::bytesOf("BOOT2 = cdrom0:\\" + elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n")},
            {elf, elfBytes(seed)},
            {"ASSETS/LEVEL.BIN", level},
            {"ASSETS/EMPTY.DAT", {}},
        };
    }

    std::string hashOf(const std::vector<uint8_t> &bytes) { return sha256Hex(bytes.data(), bytes.size()); }

    std::vector<SetupGame> games()
    {
        return {
            {"ai", "Aggressive Inline (USA)", "SLUS_203.27", hashOf(elfBytes(1))},
            {"dm2", "Dave Mirra Freestyle BMX 2 (USA)", "SLUS_201.59", hashOf(elfBytes(2))},
        };
    }

    std::vector<uint8_t> readAll(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    // The ISO made into a raw CD image: each 2048-byte sector in a 2352-byte
    // one, Mode 2 Form 1 (sync, header, subheader, data, EDC/ECC left zero) or Mode 1.
    void writeRaw(const fs::path &iso, const fs::path &bin, int mode)
    {
        const std::vector<uint8_t> data = readAll(iso);
        std::ofstream out(bin, std::ios::binary | std::ios::trunc);
        for (size_t sector = 0; sector * 2048u < data.size(); ++sector)
        {
            uint8_t raw[2352] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
            const size_t frame = sector + 150u;
            raw[12] = static_cast<uint8_t>(frame / 75u / 60u);
            raw[13] = static_cast<uint8_t>(frame / 75u % 60u);
            raw[14] = static_cast<uint8_t>(frame % 75u);
            raw[15] = static_cast<uint8_t>(mode);
            std::memcpy(raw + (mode == 2 ? 24 : 16), data.data() + sector * 2048u, 2048u);
            out.write(reinterpret_cast<const char *>(raw), sizeof(raw));
        }
    }

    // Documents as Android's documents UI gives them (game_setup.h
    // DocumentAccess), made from a folder: "content://test/<path in it>",
    // the folder's own URI "content://test/", read through files it opens.
    // Installed for one test and taken away after it.
    struct FolderDocuments final : DocumentAccess
    {
        fs::path root;
        mutable int opened = 0;
        explicit FolderDocuments(fs::path folder) : root(std::move(folder)) { installDocumentAccess(this); }
        ~FolderDocuments() override { installDocumentAccess(nullptr); }

        static constexpr std::string_view kPrefix = "content://test/";
        std::string uriOf(const fs::path &path) const
        {
            const std::string relative = fs::relative(path, root).generic_string();
            return std::string(kPrefix) + (relative == "." ? "" : relative);
        }
        fs::path pathOf(const std::string &uri) const { return root / uri.substr(kPrefix.size()); }

        bool isDocument(std::string_view text) const override { return text.rfind(kPrefix, 0) == 0; }
        bool stat(const std::string &uri, Entry &out) const override
        {
            const fs::path path = pathOf(uri);
            std::error_code ec;
            if (!fs::exists(path, ec))
                return false;
            const bool folder = fs::is_directory(path, ec);
            out = {uri, uri == kPrefix ? std::string("picked folder") : path.filename().string(),
                   folder ? 0u : static_cast<uint64_t>(fs::file_size(path, ec)), folder};
            return true;
        }
        std::vector<Entry> list(const std::string &folderUri) const override
        {
            std::vector<Entry> out;
            for (const fs::directory_entry &entry : fs::directory_iterator(pathOf(folderUri)))
            {
                Entry e;
                stat(uriOf(entry.path()), e);
                out.push_back(e);
            }
            return out;
        }
        std::FILE *open(const std::string &uri) const override
        {
            ++opened;
            return std::fopen(pathOf(uri).string().c_str(), "rb");
        }
    };

    struct CodeThatFails final : GameCodeStep
    {
        bool done = false;
        std::string name() const override { return "Making the game's code"; }
        bool ready(const SetupGame &, const fs::path &, const fs::path &) const override { return done; }
        bool make(const SetupGame &, const fs::path &, const fs::path &, const SetupProgressFn &, std::string *error) override
        {
            *error = "The compiler is missing.";
            return false;
        }
    };
}

void register_ps2_game_setup_tests()
{
    MiniTest::Case("GameSetup", [](TestCase &tc)
    {
        tc.Run("SHA-256 given piece by piece is the whole's", [](TestCase &t)
        {
            const std::string abc = "abc";
            t.Equals(sha256Hex(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()),
                     std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "FIPS 180-4's abc");
            const std::vector<uint8_t> bytes = elfBytes(9);
            Sha256 pieces;
            for (size_t at = 0; at < bytes.size(); at += 61u)
                pieces.update(bytes.data() + at, std::min<size_t>(61u, bytes.size() - at));
            t.Equals(pieces.finishHex(), hashOf(bytes), "61-byte pieces");
        });

        tc.Run("BIN images of 2352-byte sectors read as their ISO", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "game.iso", discFiles("SLUS_203.27"));
            for (int mode : {1, 2})
            {
                const fs::path bin = dir.path / ("game-mode" + std::to_string(mode) + ".bin");
                writeRaw(dir.path / "game.iso", bin, mode);
                ps2x::iop::Iso9660Image image;
                std::string error;
                t.IsTrue(image.open(bin, &error), "mode " + std::to_string(mode) + " opens: " + error);
                t.Equals(image.fileSectorSize(), 2352u, "raw sectors");
                t.Equals(image.userDataOffset(), mode == 1 ? 16u : 24u, "user data after the header");
                const auto entry = image.find("ASSETS/LEVEL.BIN");
                std::vector<uint8_t> bytes;
                t.IsTrue(entry && image.readFile(*entry, bytes), "a file across many sectors");
                t.IsTrue(bytes == discFiles("SLUS_203.27")[2].data, "its bytes");
            }
        });

        tc.Run("a CUE names its BIN", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "game.iso", discFiles("SLUS_201.59", 2));
            writeRaw(dir.path / "game.iso", dir.path / "Dave Mirra (USA).bin", 2);
            std::ofstream(dir.path / "Dave Mirra (USA).cue")
                << "FILE \"Dave Mirra (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n";
            DiscSource source;
            std::string error;
            t.IsTrue(source.open(dir.path / "Dave Mirra (USA).cue", &error), "opens: " + error);
            t.IsTrue(source.kind() == DiscSource::Kind::RawImage, "a raw image");
            t.Equals(source.readFrom().filename().string(), std::string("Dave Mirra (USA).bin"), "read from the BIN");
            const SourceCheck check = checkSource(source, games());
            t.IsTrue(check.status == SourceCheck::Status::Ok && check.game == 1, "Dave Mirra: " + check.message);

            std::ofstream(dir.path / "lost.cue") << "FILE \"gone.bin\" BINARY\n";
            t.IsFalse(source.open(dir.path / "lost.cue", &error), "a CUE without its BIN");
            t.IsTrue(error.find("gone.bin") != std::string::npos, "says which file: " + error);
        });

        tc.Run("each disc is told apart in plain words", [](TestCase &t)
        {
            TempDir dir;
            auto check = [&](const std::string &name, const std::vector<test_iso::File> &files, int wanted)
            {
                test_iso::write(dir.path / name, files);
                DiscSource source;
                std::string error;
                if (!source.open(dir.path / name, &error))
                    return SourceCheck{SourceCheck::Status::Unreadable, error};
                return checkSource(source, games(), wanted);
            };
            SourceCheck c = check("ai.iso", discFiles("SLUS_203.27"), -1);
            t.IsTrue(c.status == SourceCheck::Status::Ok && c.game == 0, "AI found by itself: " + c.message);
            t.Equals(c.files, size_t{4}, "its four files");
            c = check("ai2.iso", discFiles("SLUS_203.27"), 1);
            t.IsTrue(c.status == SourceCheck::Status::OtherGame && c.game == 0, "AI on Dave Mirra's row: " + c.message);
            c = check("beta.iso", discFiles("SLUS_203.27", 7), 0);
            t.IsTrue(c.status == SourceCheck::Status::WrongVersion, "another version: " + c.message);
            c = check("pal.iso", discFiles("SLES_504.80"), 0);
            t.IsTrue(c.status == SourceCheck::Status::OtherRelease && c.game == 0, "the European disc: " + c.message);
            t.IsTrue(c.message.find("the European release of") != std::string::npos, "names it");
            c = check("other.iso", discFiles("SLUS_999.99"), -1);
            t.IsTrue(c.status == SourceCheck::Status::UnknownGame, "another game: " + c.message);
            c = check("data.iso", {{"README.TXT", test_iso::bytesOf("hello")}}, -1);
            t.IsTrue(c.status == SourceCheck::Status::NotPs2Disc, "a data disc: " + c.message);

            std::vector<uint8_t> xbox(0x20000u);
            std::memcpy(xbox.data() + 0x10000, "MICROSOFT*XBOX*MEDIA", 20);
            std::ofstream(dir.path / "game-xbox.iso", std::ios::binary).write(reinterpret_cast<const char *>(xbox.data()), xbox.size());
            DiscSource source;
            std::string error;
            t.IsFalse(source.open(dir.path / "game-xbox.iso", &error), "an Xbox image is not taken");
            t.IsTrue(error.find("Xbox") != std::string::npos, "and is called one: " + error);
        });

        tc.Run("a folder of a game's dumps: its PS2 image", [](TestCase &t)
        {
            TempDir dir;
            const fs::path folder = dir.path / "aggressive inline";
            fs::create_directories(folder);
            std::vector<uint8_t> xbox(0x20000u);
            std::memcpy(xbox.data() + 0x10000, "MICROSOFT*XBOX*MEDIA", 20);
            std::ofstream(folder / "a-xbox.iso", std::ios::binary).write(reinterpret_cast<const char *>(xbox.data()), xbox.size());
            test_iso::write(folder / "b-ps2.iso", discFiles("SLUS_203.27"));
            DiscSource source;
            std::string error;
            t.IsTrue(source.open(folder, &error), "opens: " + error);
            t.Equals(source.readFrom().filename().string(), std::string("b-ps2.iso"), "the PS2 image in it");
            t.IsTrue(checkSource(source, games()).status == SourceCheck::Status::Ok, "and it checks");
        });

        tc.Run("install, check, damage, repair, remove", [](TestCase &t)
        {
            TempDir dir;
            const std::vector<test_iso::File> files = discFiles("SLUS_203.27");
            test_iso::write(dir.path / "ai.iso", files);
            DiscSource source;
            std::string error;
            t.IsTrue(source.open(dir.path / "ai.iso", &error), "opens");
            const fs::path gameDir = dir.path / "support" / "games" / "ai";
            PrebuiltGameCode code;
            uint64_t lastDone = 0, total = 0;
            int calls = 0;
            InstallResult result = installGame(source, games()[0], gameDir, &code, [&](const SetupProgress &p)
            {
                ++calls;
                if (p.step == "Copying files")
                {
                    lastDone = p.done;
                    total = p.total;
                }
                return true;
            });
            t.IsTrue(result.ok, "installed: " + result.message);
            t.IsTrue(calls > 2 && lastDone == total && total > 0, "progress up to the whole");
            const fs::path disc = installedDiscFolder(gameDir);
            t.IsTrue(readAll(disc / "ASSETS" / "LEVEL.BIN") == files[2].data, "a file as on the disc");
            t.IsTrue(fs::exists(disc / "ASSETS" / "EMPTY.DAT"), "an empty file too");
            t.IsTrue(fs::exists(installManifestFile(gameDir)) && !fs::exists(gameDir / "disc.partial"), "the manifest, no staging left");
            t.IsTrue(checkDiscFolder(disc, "AI", "SLUS_203.27", games()[0].elfSha256).status == DiscCheck::Status::Ok,
                     "the app's own check of the folder");

            InstalledCheck installed = checkInstalled(gameDir, games()[0], false, &code);
            t.IsTrue(installed.status == InstalledCheck::Status::Ok, "quick check: " + installed.message);
            t.Equals(installed.files, size_t{4}, "four files");
            t.Equals(installed.source.filename().string(), std::string("ai.iso"), "where they came from");

            // Same size, other bytes: only the full check sees it.
            {
                std::fstream f(disc / "ASSETS" / "LEVEL.BIN", std::ios::in | std::ios::out | std::ios::binary);
                f.seekp(100);
                f.put(static_cast<char>(0x55));
            }
            t.IsTrue(checkInstalled(gameDir, games()[0], false).status == InstalledCheck::Status::Ok, "quick: sizes are right");
            installed = checkInstalled(gameDir, games()[0], true);
            t.IsTrue(installed.status == InstalledCheck::Status::Damaged, "full: damaged");
            t.IsTrue(!installed.problems.empty() && installed.problems[0].find("LEVEL.BIN") != std::string::npos, installed.message);
            fs::remove(disc / "ASSETS" / "EMPTY.DAT");
            t.IsTrue(checkInstalled(gameDir, games()[0], false).status == InstalledCheck::Status::Damaged, "quick: a file missing");

            // Repair: installed again from the same disc, over the damaged files.
            result = installGame(source, games()[0], gameDir, &code);
            t.IsTrue(result.ok, "repaired: " + result.message);
            t.IsTrue(checkInstalled(gameDir, games()[0], true).status == InstalledCheck::Status::Ok, "full check after the repair");
            t.IsTrue(!fs::exists(gameDir / "disc.old"), "the old files gone");

            t.IsTrue(removeInstalled(gameDir, &error), "removed: " + error);
            t.IsTrue(checkInstalled(gameDir, games()[0], false).status == InstalledCheck::Status::NotInstalled, "not installed");
        });

        tc.Run("a stopped install leaves nothing; another game's disc installs nothing", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "ai.iso", discFiles("SLUS_203.27"));
            test_iso::write(dir.path / "dm2.iso", discFiles("SLUS_201.59", 2));
            DiscSource source;
            std::string error;
            source.open(dir.path / "ai.iso", &error);
            const fs::path gameDir = dir.path / "games" / "ai";
            InstallResult result = installGame(source, games()[0], gameDir, nullptr, [](const SetupProgress &p) { return p.done < 5000u; });
            t.IsTrue(!result.ok && result.cancelled, "stopped: " + result.message);
            t.IsTrue(!fs::exists(installedDiscFolder(gameDir)) && !fs::exists(gameDir / "disc.partial"), "no files left");

            DiscSource other;
            other.open(dir.path / "dm2.iso", &error);
            result = installGame(other, games()[0], gameDir, nullptr);
            t.IsTrue(!result.ok && !result.filesInstalled, "Dave Mirra's disc for AI: " + result.message);
        });

        tc.Run("the code step's failure is said, and the files stay", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "ai.iso", discFiles("SLUS_203.27"));
            DiscSource source;
            std::string error;
            source.open(dir.path / "ai.iso", &error);
            const fs::path gameDir = dir.path / "games" / "ai";
            CodeThatFails code;
            const InstallResult result = installGame(source, games()[0], gameDir, &code);
            t.IsTrue(!result.ok && result.filesInstalled, "files in, code not");
            t.Equals(result.message, std::string("The compiler is missing."), "the step's own words");
            const InstalledCheck check = checkInstalled(gameDir, games()[0], false, &code);
            t.IsTrue(check.status == InstalledCheck::Status::CodeMissing, "the check says the code is missing: " + check.message);
            code.done = true;
            t.IsTrue(checkInstalled(gameDir, games()[0], false, &code).status == InstalledCheck::Status::Ok, "and Ok once it is made");

            // The prebuilt code: another version of the ELF is not its own.
            SetupGame other = games()[0];
            other.elfSha256 = std::string(64, '0');
            PrebuiltGameCode prebuilt;
            t.IsFalse(prebuilt.ready(other, installedDiscFolder(gameDir), gameDir), "the app's code is for another ELF");
        });

        tc.Run("a disc folder copied by hand is used as it is", [](TestCase &t)
        {
            TempDir dir;
            const fs::path disc = dir.path / "games" / "ai" / "disc";
            fs::create_directories(disc);
            for (const test_iso::File &file : discFiles("SLUS_203.27"))
            {
                fs::create_directories((disc / file.path).parent_path());
                std::ofstream(disc / file.path, std::ios::binary).write(reinterpret_cast<const char *>(file.data.data()), file.data.size());
            }
            const InstalledCheck check = checkInstalled(dir.path / "games" / "ai", games()[0], false);
            t.IsTrue(check.status == InstalledCheck::Status::ByHand, check.message);

            DiscSource folder;
            std::string error;
            t.IsTrue(folder.open(disc, &error) && folder.kind() == DiscSource::Kind::Folder, "a folder source");
            t.IsTrue(checkSource(folder, games()).status == SourceCheck::Status::Ok, "checks as the disc");
        });

        tc.Run("a picked document: an image, set up and repaired through its URI", [](TestCase &t)
        {
            TempDir dir;
            const std::vector<test_iso::File> files = discFiles("SLUS_203.27");
            fs::create_directories(dir.path / "picks");
            test_iso::write(dir.path / "picks" / "Aggressive Inline (USA).iso", files);
            FolderDocuments documents(dir.path / "picks");
            const fs::path picked = pathFromUtf8(documents.uriOf(dir.path / "picks" / "Aggressive Inline (USA).iso"));
            t.IsTrue(isDocument(picked) && !isDocument(dir.path / "picks"), "a document, and a path is none");
            DiscSource source;
            std::string error;
            t.IsTrue(source.open(picked, &error) && source.kind() == DiscSource::Kind::IsoImage, "opens: " + error);
            t.Equals(source.name(), std::string("Aggressive Inline (USA).iso"), "its name as the picker shows it");
            const SourceCheck check = checkSource(source, games());
            t.IsTrue(check.status == SourceCheck::Status::Ok && check.game == 0, check.message);
            t.IsTrue(check.message.rfind("Aggressive Inline (USA).iso", 0) == 0, "the message names it: " + check.message);

            const fs::path gameDir = dir.path / "support" / "games" / "ai";
            PrebuiltGameCode code;
            const InstallResult result = installGame(source, games()[0], gameDir, &code);
            t.IsTrue(result.ok, "installed: " + result.message);
            t.IsTrue(readAll(installedDiscFolder(gameDir) / "ASSETS" / "LEVEL.BIN") == files[2].data, "a file as on the disc");
            const InstalledCheck installed = checkInstalled(gameDir, games()[0], true, &code);
            t.IsTrue(installed.status == InstalledCheck::Status::Ok, installed.message);
            t.Equals(utf8Of(installed.source), utf8Of(picked), "the manifest keeps the URI");
            t.IsTrue(sourceReadable(installed.source), "repairable");
            t.IsTrue(documents.opened > 0, "read through the files it opened");
            t.Equals(sourceName(installed.source), std::string("Aggressive Inline (USA).iso"), "the repair row's name");
            source = DiscSource(); // (its image open: Windows deletes no open file)
            fs::remove(dir.path / "picks" / "Aggressive Inline (USA).iso");
            t.IsFalse(sourceReadable(installed.source), "not once the document is gone");
            t.IsFalse(source.open(picked, &error), "nor opens");
        });

        tc.Run("a picked folder document: the disc's files, or every game's dumps", [](TestCase &t)
        {
            TempDir dir;
            const fs::path disc = dir.path / "AI disc";
            for (const test_iso::File &file : discFiles("SLUS_203.27"))
            {
                fs::create_directories((disc / file.path).parent_path());
                std::ofstream(disc / file.path, std::ios::binary).write(reinterpret_cast<const char *>(file.data.data()), file.data.size());
            }
            {
                FolderDocuments documents(disc);
                DiscSource folder;
                std::string error;
                t.IsTrue(folder.open(pathFromUtf8(documents.uriOf(disc)), &error) && folder.kind() == DiscSource::Kind::Folder,
                         "a folder source: " + error);
                t.Equals(folder.files().size(), size_t{4}, "every level's files");
                t.IsTrue(checkSource(folder, games()).status == SourceCheck::Status::Ok, "checks as the disc");
                const fs::path gameDir = dir.path / "support" / "games" / "ai";
                t.IsTrue(installGame(folder, games()[0], gameDir, nullptr).ok, "installs");
                t.IsTrue(readAll(installedDiscFolder(gameDir) / "ASSETS" / "LEVEL.BIN") == discFiles("SLUS_203.27")[2].data,
                         "a file as in the folder");
            }

            // Every game's dumps in one folder: an ISO and a BIN/CUE.
            const fs::path dumps = dir.path / "PS2";
            fs::create_directories(dumps);
            test_iso::write(dumps / "ai.iso", discFiles("SLUS_203.27"));
            test_iso::write(dir.path / "dm2.iso", discFiles("SLUS_201.59", 2));
            writeRaw(dir.path / "dm2.iso", dumps / "Dave Mirra (USA).bin", 2);
            std::ofstream(dumps / "Dave Mirra (USA).cue") << "FILE \"Dave Mirra (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n";
            std::ofstream(dumps / "notes.txt") << "not a disc";
            {
                FolderDocuments documents(dumps);
                const std::vector<fs::path> sources = discSourcesIn(pathFromUtf8(documents.uriOf(dumps)));
                t.Equals(sources.size(), size_t{2}, "two discs");
                std::vector<std::string> names;
                for (const fs::path &source : sources)
                    names.push_back(sourceName(source));
                std::sort(names.begin(), names.end());
                t.IsTrue(names == std::vector<std::string>{"Dave Mirra (USA).bin", "ai.iso"}, "the ISO, and the dump by its BIN");

                DiscSource cue;
                std::string error;
                t.IsFalse(cue.open(pathFromUtf8(documents.uriOf(dumps / "Dave Mirra (USA).cue")), &error), "a CUE picked alone");
                t.IsTrue(error.find("BIN") != std::string::npos, "asks for its BIN: " + error);
                DiscSource bin;
                t.IsTrue(bin.open(pathFromUtf8(documents.uriOf(dumps / "Dave Mirra (USA).bin")), &error) &&
                             bin.kind() == DiscSource::Kind::RawImage, "its BIN alone: " + error);
                t.IsTrue(checkSource(bin, games()).game == 1, "is Dave Mirra");
            }
            {
                // A folder of one dump: its CUE reads the BIN beside it.
                fs::remove(dumps / "ai.iso");
                FolderDocuments documents(dumps);
                DiscSource folder;
                std::string error;
                t.IsTrue(folder.open(pathFromUtf8(documents.uriOf(dumps)), &error), "the folder of a BIN/CUE: " + error);
                t.Equals(utf8Of(folder.readFrom()), documents.uriOf(dumps / "Dave Mirra (USA).bin"), "read from the BIN's document");
                t.Equals(folder.name(), std::string("picked folder"), "named as the folder");
            }
            // The same folder as a path: the CUE stands for its BIN.
            test_iso::write(dumps / "ai.iso", discFiles("SLUS_203.27"));
            const std::vector<fs::path> paths = discSourcesIn(dumps);
            std::vector<std::string> names;
            for (const fs::path &source : paths)
                names.push_back(source.filename().string());
            std::sort(names.begin(), names.end());
            t.IsTrue(names == std::vector<std::string>{"Dave Mirra (USA).cue", "ai.iso"}, "as paths: the ISO and the CUE");
        });

        tc.Run("saves and settings moved in one file, without the disc files", [](TestCase &t)
        {
            TempDir dir;
            const fs::path from = dir.path / "old app", to = dir.path / "new app";
            auto put = [](const fs::path &file, const std::string &text)
            {
                fs::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary) << text;
            };
            put(from / "settings.ini", "volume = 7\n");
            put(from / "games" / "ai" / "Saves" / "Career.sav", std::string(5000, 'c'));
            put(from / "games" / "dm2" / "controls.ini", "jump = A\n");
            put(from / "games" / "ai" / "disc" / "SLUS_203.27", "elf");
            put(from / "games" / "ai" / "disc.manifest", "z-axis game files 1\n");
            put(from / "games" / "ai" / "gpu_pipelines.txt", "cache");
            put(from / "Logs" / "z-axis-collection.log", "log");
            t.IsTrue(savedInTransfer("games/ai/Saves/Career.sav") && !savedInTransfer("games/ai/disc/SLUS_203.27") &&
                         !savedInTransfer("games/ai/disc.manifest") && !savedInTransfer("Logs/a.log"), "what goes");

            std::vector<uint8_t> zipped;
            const TransferResult made = exportSaves(from, [&](uint64_t offset, const void *data, size_t size)
            {
                if (zipped.size() < offset + size)
                    zipped.resize(offset + size);
                std::memcpy(zipped.data() + offset, data, size);
                return true;
            });
            t.IsTrue(made.ok && made.files == 3u, "exported: " + made.message);

            put(to / "settings.ini", "volume = 1\n");
            put(to / "games" / "bmx" / "settings.ini", "kept\n");
            auto readZip = [&](uint64_t offset, void *data, size_t size)
            {
                if (offset + size > zipped.size())
                    return false;
                std::memcpy(data, zipped.data() + offset, size);
                return true;
            };
            const TransferResult taken = importSaves(to, zipped.size(), readZip);
            t.IsTrue(taken.ok && taken.files == 3u, "imported: " + taken.message);
            t.IsTrue(readAll(to / "games" / "ai" / "Saves" / "Career.sav") == readAll(from / "games" / "ai" / "Saves" / "Career.sav"),
                     "the save as it was");
            t.IsTrue(readAll(to / "settings.ini") == readAll(from / "settings.ini"), "the settings replaced");
            t.IsTrue(fs::exists(to / "games" / "bmx" / "settings.ini"), "the others kept");
            t.IsTrue(!fs::exists(to / "games" / "ai" / "disc") && !fs::exists(to / "Logs"), "no disc files, no logs");

            std::vector<uint8_t> other = zipped;
            other.resize(other.size() / 2);
            const TransferResult refused = importSaves(to, other.size(), [&](uint64_t offset, void *data, size_t size)
            {
                if (offset + size > other.size())
                    return false;
                std::memcpy(data, other.data() + offset, size);
                return true;
            });
            t.IsFalse(refused.ok, "half a file is refused: " + refused.message);
        });

        tc.Run("sizes in words", [](TestCase &t)
        {
            t.Equals(sizeText(960'000'000ull), std::string("960 MB"), "MB");
            t.Equals(sizeText(1'400'000'000ull), std::string("1.4 GB"), "GB");
        });
    });
}
