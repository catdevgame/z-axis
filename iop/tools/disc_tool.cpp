// zx-disc: a PS2 disc image's files, whatever format it is in (an ISO, a
// BIN/CUE dump by its CUE or its BIN, a CHD, a CSO or a ZSO), read as the
// app reads them (zx/disc/iso9660.h). The build's tools take each game's
// boot ELF from the player's disc with it (tooling/recompile.sh).
//
//   zx-disc list IMAGE                    each file: its size and path
//   zx-disc extract IMAGE OUTDIR [PATH...] the files named (all: none named)
//                                           into OUTDIR, in their folders
//   zx-disc to-iso IMAGE OUT              OUT: the image's 2048-byte ISO
#include "zx/disc/cue_sheet.h"
#include "zx/disc/iso9660.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using zx::disc::IsoEntry;
    using zx::disc::Iso9660Image;

    constexpr size_t kChunk = 4u * 1024u * 1024u;

    // Paths from and to UTF-8 text, on every platform.
    fs::path pathOf(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }
    std::string textOf(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    int fail(const std::string &message)
    {
        std::cerr << "zx-disc: " << message << std::endl;
        return 1;
    }

    // The file a picked image is read from: a CUE sheet's data track (by its
    // text, whatever its name), else the file itself.
    fs::path dataFileOf(const fs::path &image)
    {
        std::error_code ec;
        if (fs::file_size(image, ec) > 1024u * 1024u || ec)
            return image;
        std::ifstream in(image, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        if (!zx::disc::isCueSheet(text))
            return image;
        const std::string data = zx::disc::cueDataTrackFile(text);
        return data.empty() ? fs::path() : image.parent_path() / pathOf(data);
    }

    void eachFile(const Iso9660Image &image, const std::string &dir, int depth, std::vector<std::pair<std::string, IsoEntry>> &out)
    {
        if (depth > 32)
            return;
        for (const IsoEntry &entry : image.list(dir.empty() ? "/" : dir))
        {
            const std::string path = dir.empty() ? entry.name : dir + "/" + entry.name;
            if (entry.directory)
                eachFile(image, path, depth + 1, out);
            else
                out.emplace_back(path, entry);
        }
    }

    bool copyOut(const Iso9660Image &image, uint64_t offset, uint64_t size, const fs::path &to)
    {
        std::error_code ec;
        if (!to.parent_path().empty())
            fs::create_directories(to.parent_path(), ec);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        std::vector<char> buffer(kChunk);
        for (uint64_t done = 0; out && done < size;)
        {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - done));
            if (!image.read(offset + done, buffer.data(), count))
                return false;
            out.write(buffer.data(), static_cast<std::streamsize>(count));
            done += count;
        }
        out.close();
        return static_cast<bool>(out);
    }
}

int main(int argc, char **argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    const bool list = args.size() == 2 && args[0] == "list";
    const bool extract = args.size() >= 3 && args[0] == "extract";
    const bool toIso = args.size() == 3 && args[0] == "to-iso";
    if (!list && !extract && !toIso)
    {
        std::cerr << "usage: zx-disc list IMAGE\n"
                     "       zx-disc extract IMAGE OUTDIR [PATH...]\n"
                     "       zx-disc to-iso IMAGE OUT\n";
        return 2;
    }
    const fs::path picked = pathOf(args[1]);
    const fs::path data = dataFileOf(picked);
    if (data.empty())
        return fail(args[1] + ": the CUE sheet names no data track");
    Iso9660Image image;
    std::string error;
    if (!image.open(data, &error))
        return fail(textOf(data) + ": " + error);

    if (toIso)
    {
        if (!copyOut(image, 0u, image.sizeBytes(), pathOf(args[2])))
            return fail("cannot write " + args[2]);
        std::cout << "zx-disc: " << textOf(data.filename()) << " -> " << args[2] << " ("
                  << image.sizeBytes() / Iso9660Image::kSectorSize << " sectors)" << std::endl;
        return 0;
    }
    std::vector<std::pair<std::string, IsoEntry>> files;
    if (list || args.size() == 3)
        eachFile(image, "", 0, files);
    else
        for (size_t i = 3; i < args.size(); ++i)
        {
            const std::optional<IsoEntry> entry = image.find(args[i]);
            if (!entry || entry->directory)
                return fail(args[i] + " is not on " + textOf(picked.filename()));
            // (the disc's own name for it)
            std::vector<std::string> parts = Iso9660Image::pathComponents(args[i]);
            parts.back() = entry->name;
            std::string path;
            for (const std::string &part : parts)
                path += (path.empty() ? "" : "/") + part;
            files.emplace_back(path, *entry);
        }
    for (const auto &[path, entry] : files)
    {
        if (list)
        {
            std::cout << entry.size << " " << path << "\n";
            continue;
        }
        if (!copyOut(image, static_cast<uint64_t>(entry.lsn) * Iso9660Image::kSectorSize, entry.size,
                     pathOf(args[2]) / pathOf(path)))
            return fail("cannot copy " + path);
    }
    return 0;
}
