#pragma once

// Writes small ISO9660 images for tests: a root directory and one level of
// subdirectories, files given as "NAME" or "DIR/NAME".

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace test_iso
{
    struct File
    {
        std::string path;
        std::vector<uint8_t> data;
    };

    inline std::vector<uint8_t> bytesOf(const std::string &text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    inline void putBoth32(uint8_t *at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            at[i] = static_cast<uint8_t>(value >> (8 * i));
            at[7 - i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    inline size_t putRecord(uint8_t *at, uint32_t lsn, uint32_t size, bool directory, const std::string &name)
    {
        const size_t nameLength = name.empty() ? 1u : name.size();
        size_t length = 33u + nameLength;
        length += length & 1u;
        std::memset(at, 0, length);
        at[0] = static_cast<uint8_t>(length);
        putBoth32(at + 2, lsn);
        putBoth32(at + 10, size);
        at[25] = directory ? 2u : 0u;
        at[28] = 1u; // volume sequence number
        at[32] = static_cast<uint8_t>(nameLength);
        if (name.empty())
            at[33] = 0u;
        else if (name == "\x01")
            at[33] = 1u;
        else
            std::memcpy(at + 33, name.data(), name.size());
        return length;
    }

    // Returns the LSN of each file by path.
    inline std::map<std::string, uint32_t> write(const std::filesystem::path &imagePath, const std::vector<File> &files,
                                                 const std::string &volumeId = "TESTDISC")
    {
        constexpr uint32_t kSector = 2048u;
        std::map<std::string, std::vector<const File *>> dirs; // "" = root
        for (const File &file : files)
        {
            const size_t slash = file.path.find('/');
            dirs[slash == std::string::npos ? "" : file.path.substr(0, slash)].push_back(&file);
        }
        dirs[""];

        // Layout: 16 PVD, 17 terminator, then one sector per directory, then files.
        std::map<std::string, uint32_t> dirLsn;
        uint32_t cursor = 18u;
        for (auto &[name, _] : dirs)
            dirLsn[name] = cursor++;
        std::map<std::string, uint32_t> fileLsn;
        for (const File &file : files)
        {
            fileLsn[file.path] = cursor;
            cursor += static_cast<uint32_t>((file.data.size() + kSector - 1u) / kSector);
            if (file.data.empty())
                ++cursor;
        }

        std::vector<uint8_t> image(static_cast<size_t>(cursor) * kSector, 0u);
        uint8_t *pvd = image.data() + 16u * kSector;
        pvd[0] = 1u;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1u;
        std::memset(pvd + 40, ' ', 32);
        std::memcpy(pvd + 40, volumeId.data(), std::min<size_t>(32u, volumeId.size()));
        putBoth32(pvd + 80, cursor);
        pvd[128] = 0x00u;
        pvd[129] = 0x08u; // 2048, little-endian half
        pvd[130] = 0x08u;
        pvd[131] = 0x00u;
        putRecord(pvd + 156, dirLsn[""], kSector, true, "");
        uint8_t *terminator = image.data() + 17u * kSector;
        terminator[0] = 255u;
        std::memcpy(terminator + 1, "CD001", 5);

        for (auto &[name, entries] : dirs)
        {
            uint8_t *dir = image.data() + static_cast<size_t>(dirLsn[name]) * kSector;
            size_t pos = putRecord(dir, dirLsn[name], kSector, true, "");
            pos += putRecord(dir + pos, dirLsn[""], kSector, true, "\x01");
            if (name.empty())
            {
                for (auto &[sub, _] : dirs)
                    if (!sub.empty())
                        pos += putRecord(dir + pos, dirLsn[sub], kSector, true, sub);
            }
            for (const File *file : entries)
            {
                const std::string leaf = file->path.substr(file->path.find('/') == std::string::npos ? 0 : file->path.find('/') + 1);
                pos += putRecord(dir + pos, fileLsn[file->path], static_cast<uint32_t>(file->data.size()), false, leaf + ";1");
            }
        }
        for (const File &file : files)
            if (!file.data.empty())
                std::memcpy(image.data() + static_cast<size_t>(fileLsn[file.path]) * kSector, file.data.data(), file.data.size());

        std::ofstream out(imagePath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
        return fileLsn;
    }
}
