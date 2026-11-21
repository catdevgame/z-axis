// The .ZAL archives (zal/zal_archive.h): both formats read and written
// byte for byte, unpacked and packed again, entries found by name; and mods
// that replace single entries of one (runtime/game_files.h,
// collection/mods.h: docs/MODS.md "Archives").
#include "MiniTest.h"
#include "collection/menu.h"
#include "collection/mods.h"
#include "runtime/game_files.h"
#include "zal/zal_archive.h"
#include "test_zip_builder.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace zal = zx::zal;
    namespace gf = zx::game_files;

    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("zx-zal-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::vector<uint8_t> bytesOf(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

    void put(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    }

    // A ZDIR archive of named entries ({name, bytes}), 32-byte aligned (BMX
    // XXX's code), with left-over bytes after the first, as some discs have.
    zal::Archive directoryOf(const std::vector<std::pair<std::string, std::string>> &entries, bool names = false)
    {
        zal::Archive archive;
        archive.names = names;
        for (const auto &[name, text] : entries)
        {
            zal::Entry entry;
            entry.id = zal::nameHash(name);
            entry.flags = 6;
            entry.name = names ? name : std::string();
            entry.data = bytesOf(text);
            archive.entries.push_back(std::move(entry));
        }
        return archive;
    }

    // The entry with this name's bytes, as text ("" none).
    std::string entryText(const std::vector<uint8_t> &file, const std::string &name)
    {
        zal::Archive archive;
        if (!zal::read(file, archive))
            return "(not an archive)";
        const std::optional<size_t> at = zal::findEntry(archive, name);
        return at ? std::string(archive.entries[*at].data.begin(), archive.entries[*at].data.end()) : std::string();
    }

    std::vector<uint8_t> readGameFile(gf::GameFileSystem &files, const std::string &path)
    {
        std::vector<uint8_t> bytes;
        if (std::shared_ptr<gf::GameFile> file = files.open(path))
            gf::readAll(*file, bytes);
        return bytes;
    }
}

void register_zal_tests()
{
    MiniTest::Case("ZalArchives", [](TestCase &tc)
    {
        tc.Run("a ZDIR archive written, read and written again: the same bytes", [](TestCase &t)
        {
            zal::Archive archive = directoryOf({{"CommonFonts.zif", "fonts"}, {"SaveLoad.zsc", "OBJECT \"guiScreen\" \"SaveLoad\""}, {"Empty", ""}});
            std::vector<uint8_t> first;
            t.IsTrue(zal::write(archive, first), "written");
            // Left-over bytes in the gap after the first entry, as BMX XXX's MOTGAME.ZAL has.
            zal::Archive read;
            t.IsTrue(zal::read(first, read), "read back");
            t.Equals(read.entries.size(), size_t(3), "its entries");
            t.Equals(read.entries[0].padding.size(), size_t(32 - 5), "the gap to the next 32 bytes kept with the entry");
            t.Equals(read.leadPadding.size(), size_t(0x60 - 0x54), "the directory padded to the first entry's alignment");
            read.entries[0].padding.assign(read.entries[0].padding.size(), 0xCD);
            std::vector<uint8_t> withGarbage, again;
            t.IsTrue(zal::write(read, withGarbage), "written with the gap's bytes");
            zal::Archive reread;
            t.IsTrue(zal::read(withGarbage, reread) && zal::write(reread, again), "and again");
            t.IsTrue(again == withGarbage, "byte for byte, the left-over bytes too");
            t.Equals(withGarbage.size() % 32, size_t(0), "the file ends at the last entry's alignment");
            t.Equals(entryText(again, "savelOAD.ZSC"), std::string("OBJECT \"guiScreen\" \"SaveLoad\""), "an entry found by its name, any case");
        });

        tc.Run("names, a zfile library, and what is not read", [](TestCase &t)
        {
            zal::Archive named = directoryOf({{"fatLattice", "aaaa"}, {"smallBreast", "bb"}}, true);
            std::vector<uint8_t> file, again;
            zal::Archive read;
            t.IsTrue(zal::write(named, file) && zal::read(file, read) && zal::write(read, again) && again == file, "a NAME chunk kept");
            t.IsTrue(read.names && read.entries[1].name == "smallBreast", "its names read");

            zal::Archive library;
            library.format = zal::Format::Library;
            for (uint32_t group : {0u, 1u, 7u})
            {
                zal::Entry entry;
                entry.id = group;
                entry.data = bytesOf(std::string(group + 1, char('a' + group)));
                library.entries.push_back(std::move(entry));
            }
            library.entries[0].padding = {0xCD, 0xCD, 0xCD};
            std::vector<uint8_t> libraryFile, libraryAgain;
            zal::Archive libraryRead;
            t.IsTrue(zal::write(library, libraryFile) && zal::read(libraryFile, libraryRead) && zal::write(libraryRead, libraryAgain) &&
                         libraryAgain == libraryFile,
                     "a zfile library, its gap's bytes too");
            t.Equals(libraryFile.size() % 4, size_t(0), "four-byte aligned");
            t.IsTrue(zal::findEntry(libraryRead, "7") == size_t(2) && zal::findEntry(libraryRead, "7.mot") == size_t(2) &&
                         !zal::findEntry(libraryRead, "8") && !zal::findEntry(libraryRead, "7x"),
                     "its entries by group id, an extension allowed");
            const zal::Archive fonts = directoryOf({{"CommonFonts.zif", "x"}});
            t.IsTrue(zal::findEntry(fonts, zal::idName(zal::nameHash("CommonFonts.zif"))) == size_t(0) &&
                         zal::findEntry(fonts, zal::idName(zal::nameHash("CommonFonts.zif")) + ".zif") == size_t(0) && !zal::findEntry(fonts, "Other"),
                     "a ZDIR entry by 0x and its id too");

            std::string error;
            zal::Archive none;
            t.IsFalse(zal::read(bytesOf("not an archive at all"), none, &error), "not an archive");
            std::vector<uint8_t> compressed = file;
            std::memcpy(compressed.data() + 0x18, "CLEN", 4);
            t.IsFalse(zal::read(compressed, none, &error), "compressed entries are not read");
            t.IsTrue(error.find("compressed") != std::string::npos, "and it says so");
        });

        tc.Run("unpacked into files and zal.txt, packed again: the same bytes", [](TestCase &t)
        {
            zal::Archive archive = directoryOf({{"CommonFonts.zif", "fonts"}, {"Unknown", "u"}, {"SaveLoad.zsc", "s"}});
            archive.entries[1].padding.assign(31, 0xAB);
            std::vector<uint8_t> file;
            zal::Archive read;
            t.IsTrue(zal::write(archive, file) && zal::read(file, read), "an archive");
            // Its names: CommonFonts.zif named in the "game's program", the others nowhere.
            const std::vector<uint8_t> program = bytesOf(std::string("\0\0path/CommonFonts.zif\0more", 27));
            const zal::Unpacked unpacked = zal::unpack(read, "UILIB.ZAL", {&program});
            t.IsTrue(unpacked.files.size() == 3 && unpacked.files[0].name == "CommonFonts.zif" &&
                         unpacked.files[1].name == zal::idName(zal::nameHash("Unknown")),
                     "an entry named by a name its hash is, else by its id");
            t.IsTrue(unpacked.listing.find("format zdir\nnames no\nalign 32\n") != std::string::npos &&
                         unpacked.listing.find("  pad abab") != std::string::npos,
                     "zal.txt: the format, the alignment, the gap's bytes");
            auto fileOf = [&](const std::string &name, std::vector<uint8_t> &bytes)
            {
                for (const zal::UnpackedFile &each : unpacked.files)
                    if (each.name == name)
                    {
                        bytes = each.bytes;
                        return true;
                    }
                return false;
            };
            zal::Archive packed;
            std::vector<uint8_t> again;
            std::string error;
            t.IsTrue(zal::repack(unpacked.listing, fileOf, packed, &error) && zal::write(packed, again), "packed again: " + error);
            t.IsTrue(again == file, "byte for byte");
            t.IsFalse(zal::repack("format zdir\nentry 0x12345678 Missing.bin\n", fileOf, packed, &error), "a file listed but missing");
            t.IsTrue(error.find("line 2") != std::string::npos, "says which line");
            t.IsTrue(zal::repack("format zdir\nalign 16\nentry - CommonFonts.zif\n", fileOf, packed, &error) &&
                         packed.entries[0].id == zal::nameHash("CommonFonts.zif") && packed.entries[0].flags == 5,
                     "an entry added by hand: its id its name's hash");
        });

        tc.Run("mods: an entry replaced, two mods on one archive, a missing entry on the Mods page", [](TestCase &t)
        {
            using namespace zx::app;
            TempDir dir;
            const std::filesystem::path disc = dir.path / "disc", mods = dir.path / "games" / "ai" / "Mods";
            std::vector<uint8_t> uilib;
            t.IsTrue(zal::write(directoryOf({{"CommonFonts.zif", "disc fonts"}, {"SaveLoad.zsc", "disc saveload"}, {"KeyIcons.zif", "disc keys"}}), uilib),
                     "the disc's archive");
            put(disc / "ASSETS" / "UILIB.ZAL", uilib);

            // One mod's entry: that entry replaced, the others as the disc has them.
            put(mods / "10-fonts" / "ASSETS" / "UILIB.ZAL" / "CommonFonts.zif", bytesOf("hd fonts"));
            {
                gf::GameFileSystem files({disc, nullptr, {mods, mods / "10-fonts"}});
                const std::vector<uint8_t> merged = readGameFile(files, "cdrom0:\\ASSETS\\UILIB.ZAL;1");
                t.Equals(entryText(merged, "CommonFonts.zif"), std::string("hd fonts"), "the mod's entry read");
                t.Equals(entryText(merged, "SaveLoad.zsc"), std::string("disc saveload"), "the rest the disc's");
                t.IsTrue(files.open("ASSETS/UILIB.ZAL")->origin() == gf::GameFile::Origin::Mod, "from a mod");
                gf::GameFileSystem plain({disc, nullptr, {mods}});
                t.IsTrue(readGameFile(plain, "ASSETS/UILIB.ZAL") == uilib, "no mod: the disc's file as it is");
            }

            // Two mods, other entries of one archive (one a zip, by id): both read; the same entry: the later's.
            // (with a mod.ini at its top: a zip whose files are all in one folder is read from inside it)
            test_zip::write(mods / "20-saves.zip",
                            {{"mod.ini", "[mod]\nid = saves\nname = Saves\ngames = ai\n"},
                             {"ASSETS/UILIB.ZAL/" + zal::idName(zal::nameHash("SaveLoad.zsc")), "zip saveload"},
                             {"ASSETS/uilib.zal/commonfonts.zif", "zip fonts"}});
            {
                gf::GameFileSystem files({disc, nullptr, {mods, mods / "10-fonts", mods / "20-saves.zip"}});
                const std::vector<uint8_t> merged = readGameFile(files, "ASSETS/UILIB.ZAL");
                t.Equals(entryText(merged, "SaveLoad.zsc"), std::string("zip saveload"), "the second mod's entry");
                t.Equals(entryText(merged, "CommonFonts.zif"), std::string("zip fonts"), "the same entry: the later mod's");
                t.Equals(entryText(merged, "KeyIcons.zif"), std::string("disc keys"), "the rest the disc's");
            }
            // A whole archive in a mod below them: the entries put in it.
            std::vector<uint8_t> other;
            zal::write(directoryOf({{"CommonFonts.zif", "whole fonts"}, {"SaveLoad.zsc", "whole saveload"}, {"KeyIcons.zif", "whole keys"}}), other);
            put(mods / "05-whole" / "ASSETS" / "UILIB.ZAL", other);
            {
                gf::GameFileSystem files({disc, nullptr, {mods, mods / "05-whole", mods / "10-fonts"}});
                const std::vector<uint8_t> merged = readGameFile(files, "ASSETS/UILIB.ZAL");
                t.IsTrue(entryText(merged, "CommonFonts.zif") == "hd fonts" && entryText(merged, "KeyIcons.zif") == "whole keys",
                         "entries above a mod's whole archive: put in it");
            }
            std::filesystem::remove_all(mods / "05-whole");

            // An entry the archive has not: the others read, the page says which.
            put(mods / "30-typo" / "ASSETS" / "UILIB.ZAL" / "KeyIcons.zif", bytesOf("typo keys"));
            put(mods / "30-typo" / "ASSETS" / "UILIB.ZAL" / "KeyIcon.zif", bytesOf("typo"));
            put(mods / "30-typo" / "ASSETS" / "UILIB.ZAL" / ".DS_Store", bytesOf("host"));
            {
                gf::GameFileSystem files({disc, nullptr, {mods, mods / "30-typo"}});
                t.Equals(entryText(readGameFile(files, "ASSETS/UILIB.ZAL"), "KeyIcons.zif"), std::string("typo keys"),
                         "a missing entry left out, the mod's others read");
            }
            std::vector<Mod> list = modsForGame("ai", listMods(mods, disc, {}, "ai"), {}, {}, {});
            t.Equals(list.size(), size_t(3), "three mods");
            checkArchiveEntries(list, mods, disc);
            t.IsTrue(list[0].entriesNotRead.empty() && list[1].entriesNotRead.empty(), "the mods whose entries are there: nothing");
            t.IsTrue(list[2].entriesNotRead == std::vector<std::string>{"ASSETS/UILIB.ZAL has no entry KeyIcon.zif"}, "the missing one, by name");
            // The winners: the same entry by its name and by its id is one thing.
            const std::vector<ModWinners> winners = modWinners(list, "ai");
            t.IsTrue(winners[1].winsOver.size() == 1 && winners[1].winsOver[0].mod == 0 && winners[1].winsOver[0].count == 1,
                     "CommonFonts.zif in 10-fonts and 20-saves.zip: the later wins");
            t.IsTrue(winners[2].things == 2 && winners[2].winsOver.empty() && winners[0].losesTo.size() == 1,
                     "other entries of one archive: no one loses");

            // The page.
            namespace col = zx::collection;
            auto entries = std::make_shared<std::vector<col::ModEntry>>(1);
            (*entries)[0].title = "Typo";
            col::ModEntry::Game game;
            game.title = "Aggressive Inline";
            game.entriesNotRead = list[2].entriesNotRead;
            (*entries)[0].games.push_back(game);
            bool said = false;
            for (const col::SettingsRow &row : col::modPageRows(entries, 0, {}, {}))
                said = said || row.label == "Aggressive Inline: ASSETS/UILIB.ZAL has no entry KeyIcon.zif, not read.";
            t.IsTrue(said, "the Mods page says it");

            // A later mod's whole archive wins over earlier entries.
            put(mods / "40-whole" / "ASSETS" / "UILIB.ZAL", other);
            list = modsForGame("ai", listMods(mods, disc, {}, "ai"), {}, {}, {});
            const std::vector<ModWinners> whole = modWinners(list, "ai");
            t.IsTrue(list.size() == 4 && whole[3].winsOver.size() == 3, "a later whole archive wins over every mod's entries of it");
        });
    });
}
