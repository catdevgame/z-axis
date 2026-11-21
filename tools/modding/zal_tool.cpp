// zal: the games' .ZAL archives for modders (zal/zal_archive.h,
// docs/MODS.md "Archives"): an archive unpacked into a folder, a file per
// entry and zal.txt listing them, and packed again. An archive unpacked and
// packed with nothing changed is the same file, byte for byte.
//
//   zal list ARCHIVE                     each entry: its id, size and file name
//   zal unpack ARCHIVE DIR [--names FILE]...
//                                             DIR (new or empty): its entries and zal.txt;
//                                             names found in FILEs too (the game's program:
//                                             SLUS_203.27, other archives) by their hash
//   zal repack DIR ARCHIVE               ARCHIVE made from DIR's zal.txt and files
//   zal verify ARCHIVE [DIR]             ARCHIVE unpacked and packed again in memory is
//                                             byte-identical; with DIR: DIR packs into ARCHIVE
//                                             (and which entries differ when it does not)
#include "zal/zal_archive.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace zal = zx::zal;

    // Paths from and to UTF-8 text, on every platform.
    fs::path pathOf(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }
    std::string textOf(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    int fail(const std::string &message)
    {
        std::cerr << "zal: " << message << std::endl;
        return 1;
    }

    bool readFile(const fs::path &path, std::vector<uint8_t> &out)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return false;
        std::ifstream in(path, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), {});
        return static_cast<bool>(in) || in.eof();
    }

    bool writeFile(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }

    bool readArchive(const std::string &name, std::vector<uint8_t> &file, zal::Archive &archive, std::string &error)
    {
        if (!readFile(pathOf(name), file))
        {
            error = name + " cannot be read";
            return false;
        }
        if (!zal::read(file, archive, &error))
        {
            error = name + ": " + error;
            return false;
        }
        return true;
    }

    // DIR's zal.txt and files into an archive's bytes.
    bool packFolder(const fs::path &dir, std::vector<uint8_t> &out, std::string &error)
    {
        std::vector<uint8_t> listing;
        if (!readFile(dir / zal::kListingFile, listing))
        {
            error = textOf(dir) + " has no " + zal::kListingFile + " (zal unpack makes one)";
            return false;
        }
        std::set<std::string> listed;
        zal::Archive archive;
        const auto fileOf = [&](const std::string &name, std::vector<uint8_t> &bytes)
        {
            listed.insert(name);
            return readFile(dir / pathOf(name), bytes);
        };
        if (!zal::repack(std::string(listing.begin(), listing.end()), fileOf, archive, &error) || !zal::write(archive, out, &error))
        {
            error = textOf(dir) + ": " + error;
            return false;
        }
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), last; !ec && it != last; it.increment(ec))
        {
            const std::string name = textOf(it->path().filename());
            if (name != zal::kListingFile && !listed.count(name) && name != ".DS_Store")
                std::cerr << "zal: " << name << " is not in " << zal::kListingFile << ": not packed" << std::endl;
        }
        return true;
    }

    // The first entry two archives' bytes differ in, as the listing names it.
    void tellDifferences(const zal::Archive &want, const std::vector<uint8_t> &made)
    {
        zal::Archive got;
        if (!zal::read(made, got))
            return;
        const zal::Unpacked names = zal::unpack(want, "");
        if (got.entries.size() != want.entries.size())
            std::cout << "  " << got.entries.size() << " entries where it has " << want.entries.size() << std::endl;
        size_t told = 0;
        for (size_t i = 0; i < std::min(got.entries.size(), want.entries.size()); ++i)
        {
            const zal::Entry &a = want.entries[i], &b = got.entries[i];
            if (a.id == b.id && a.data == b.data && a.flags == b.flags && a.name == b.name && a.padding == b.padding)
                continue;
            if (++told > 20)
            {
                std::cout << "  ..." << std::endl;
                break;
            }
            std::cout << "  entry " << i << " (" << names.files[i].name << "): "
                      << (a.id != b.id ? "another id" : a.data != b.data ? "other bytes" : a.flags != b.flags ? "another alignment" : a.name != b.name ? "another name" : "other padding after it")
                      << std::endl;
        }
    }
}

int main(int argc, char **argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string command = args.empty() ? std::string() : args[0];
    const bool ok = (command == "list" && args.size() == 2) || (command == "unpack" && args.size() >= 3) ||
                    (command == "repack" && args.size() == 3) || (command == "verify" && (args.size() == 2 || args.size() == 3));
    if (!ok)
    {
        std::cerr << "usage: zal list ARCHIVE\n"
                     "       zal unpack ARCHIVE DIR [--names FILE]...\n"
                     "       zal repack DIR ARCHIVE\n"
                     "       zal verify ARCHIVE [DIR]\n";
        return 2;
    }
    std::string error;
    if (command == "repack")
    {
        std::vector<uint8_t> made;
        if (!packFolder(pathOf(args[1]), made, error))
            return fail(error);
        if (!writeFile(pathOf(args[2]), made))
            return fail("cannot write " + args[2]);
        std::cout << "zal: " << args[1] << " -> " << args[2] << " (" << made.size() << " bytes)" << std::endl;
        return 0;
    }

    std::vector<uint8_t> file;
    zal::Archive archive;
    if (!readArchive(args[1], file, archive, error))
        return fail(error);
    const std::string title = textOf(pathOf(args[1]).filename());
    const char *format = archive.format == zal::Format::Directory ? "ZDIR archive" : "zfile library";

    if (command == "list")
    {
        const zal::Unpacked unpacked = zal::unpack(archive, title);
        std::cout << title << ": a " << format << " of " << archive.entries.size() << " entries" << std::endl;
        for (size_t i = 0; i < archive.entries.size(); ++i)
        {
            const zal::Entry &entry = archive.entries[i];
            std::cout << (archive.format == zal::Format::Directory ? zal::idName(entry.id) : std::to_string(entry.id)) << " "
                      << entry.data.size() << " " << unpacked.files[i].name << "\n";
        }
        return 0;
    }

    if (command == "unpack")
    {
        std::vector<std::vector<uint8_t>> sources;
        for (size_t i = 3; i < args.size(); ++i)
        {
            if (args[i] != "--names" || i + 1 >= args.size())
                return fail("after DIR only --names FILE");
            sources.emplace_back();
            if (!readFile(pathOf(args[++i]), sources.back()))
                return fail(args[i] + " cannot be read");
        }
        std::vector<const std::vector<uint8_t> *> sourceList;
        for (const std::vector<uint8_t> &source : sources)
            sourceList.push_back(&source);
        const fs::path dir = pathOf(args[2]);
        std::error_code ec;
        if (fs::exists(dir, ec) && !fs::is_empty(dir, ec))
            return fail(args[2] + " is not empty (unpack writes into a new or empty folder)");
        fs::create_directories(dir, ec);
        if (ec)
            return fail("cannot make " + args[2]);
        const zal::Unpacked unpacked = zal::unpack(archive, title, sourceList);
        for (const zal::UnpackedFile &entry : unpacked.files)
            if (!writeFile(dir / pathOf(entry.name), entry.bytes))
                return fail("cannot write " + textOf(dir / pathOf(entry.name)));
        if (!writeFile(dir / zal::kListingFile, std::vector<uint8_t>(unpacked.listing.begin(), unpacked.listing.end())))
            return fail("cannot write " + textOf(dir / zal::kListingFile));
        size_t named = 0;
        for (const zal::UnpackedFile &entry : unpacked.files)
            named += entry.name.compare(0, 2, "0x") != 0 || archive.format == zal::Format::Library;
        std::cout << "zal: " << title << " (a " << format << ") -> " << args[2] << ": " << unpacked.files.size() << " entries";
        if (archive.format == zal::Format::Directory && !archive.names)
            std::cout << ", " << named << " of them by name";
        std::cout << std::endl;
        // The folder packs into the same file: said, or why not.
        std::vector<uint8_t> made;
        if (!packFolder(dir, made, error))
            return fail(error);
        if (made != file)
        {
            std::cout << "zal: packed again it would not be the same file:" << std::endl;
            tellDifferences(archive, made);
            return 1;
        }
        return 0;
    }

    // verify
    std::vector<uint8_t> made;
    if (args.size() == 3)
    {
        if (!packFolder(pathOf(args[2]), made, error))
            return fail(error);
    }
    else
    {
        const zal::Unpacked unpacked = zal::unpack(archive, title);
        zal::Archive again;
        const auto fileOf = [&](const std::string &name, std::vector<uint8_t> &bytes)
        {
            for (const zal::UnpackedFile &entry : unpacked.files)
                if (entry.name == name)
                {
                    bytes = entry.bytes;
                    return true;
                }
            return false;
        };
        if (!zal::repack(unpacked.listing, fileOf, again, &error) || !zal::write(again, made, &error))
            return fail(title + ": " + error);
    }
    if (made == file)
    {
        std::cout << "zal: " << title << ": identical (" << archive.entries.size() << " entries, " << file.size() << " bytes)" << std::endl;
        return 0;
    }
    std::cout << "zal: " << title << ": differs (" << made.size() << " bytes where it has " << file.size() << ")" << std::endl;
    tellDifferences(archive, made);
    return 1;
}
