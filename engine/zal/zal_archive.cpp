// The games' .ZAL archives (zal/zal_archive.h): both formats read,
// written as the discs lay them out, and their entries found by file name.
#include "zal/zal_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace zx::zal
{
    namespace
    {
        constexpr uint32_t kLibraryMagic = 0x2A81511Cu;
        constexpr size_t kLibraryCountsAt = 0x10;  // two halfwords: assets a group, groups
        constexpr size_t kLibraryDirectoryAt = 0x14;
        constexpr size_t kLibraryDataStart = 0x10; // entry offsets count from here
        constexpr uint32_t kLibraryAlignment = 4;
        constexpr uint16_t kLibraryAssetsMask = 0x1FFF, kLibraryExternalBit = 1u << 13;
        constexpr uint16_t kLibraryGroupsMask = 0x3FFF, kLibraryLzoBit = 1u << 14;
        constexpr uint16_t kLibraryCompressedBit = 1u << 15;
        constexpr size_t kLibraryEntryBytes = 8;
        constexpr uint32_t kLibraryLengthMask = 0x00FFFFFFu;

        constexpr uint32_t kDirectoryLengthMask = 0x0FFFFFFFu;
        constexpr uint32_t kDefaultAlignmentCode = 5; // a code of 0 (zge's kAssetDefaultAlignmentCode)

        uint32_t u32At(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
        uint16_t u16At(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
        void putU32(std::vector<uint8_t> &out, uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(uint8_t(value >> (8 * i)));
        }
        void putTag(std::vector<uint8_t> &out, const char *tag) { out.insert(out.end(), tag, tag + 4); }
        uint64_t alignUp(uint64_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

        bool fail(std::string *error, std::string why)
        {
            if (error)
                *error = std::move(why);
            return false;
        }

        // The bytes after the directory, between the entries and after the
        // last: each entry's data and padding, the lead padding. `offsets`
        // and `lengths` are the entries'.
        bool readEntries(const std::vector<uint8_t> &file, uint64_t directoryEnd, const std::vector<uint64_t> &offsets,
                         const std::vector<uint64_t> &lengths, Archive &out, std::string *error)
        {
            uint64_t at = directoryEnd;
            for (size_t i = 0; i < out.entries.size(); ++i)
            {
                if (offsets[i] < at)
                    return fail(error, "entry " + std::to_string(i) + " starts inside the one before it (or the directory)");
                if (offsets[i] + lengths[i] > file.size())
                    return fail(error, "entry " + std::to_string(i) + " goes past the end of the file");
                std::vector<uint8_t> &gap = i == 0 ? out.leadPadding : out.entries[i - 1].padding;
                gap.assign(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(offsets[i]));
                out.entries[i].data.assign(file.begin() + std::ptrdiff_t(offsets[i]), file.begin() + std::ptrdiff_t(offsets[i] + lengths[i]));
                at = offsets[i] + lengths[i];
            }
            std::vector<uint8_t> &tail = out.entries.empty() ? out.leadPadding : out.entries.back().padding;
            tail.assign(file.begin() + std::ptrdiff_t(at), file.end());
            return true;
        }

        // A ZDIR archive's directory (`size` bytes of it at least), and with
        // `file` its entries too.
        bool readDirectoryArchive(const uint8_t *start, size_t size, const std::vector<uint8_t> *file, Archive &out, std::string *error)
        {
            if (size < 8 || std::memcmp(start, "ZDIR", 4) != 0)
                return fail(error, "not a ZDIR archive");
            const uint64_t directoryEnd = 8 + uint64_t(u32At(start + 4));
            if (directoryEnd > size)
                return fail(error, "its directory goes past the end of the file");
            Archive archive;
            archive.format = Format::Directory;
            static const char *const kOrder[] = {"DIDF", "GRID", "DLEN", "OFST", "NAME"};
            const uint8_t *chunks[5] = {};
            uint32_t chunkBytes[5] = {};
            size_t next = 0;
            for (uint64_t at = 8; at < directoryEnd;)
            {
                if (at + 8 > directoryEnd)
                    return fail(error, "its directory ends inside a chunk's header");
                const uint8_t *chunk = start + at;
                const uint32_t bytes = u32At(chunk + 4);
                if (at + 8 + bytes > directoryEnd)
                    return fail(error, "a chunk goes past the end of the directory");
                const std::string tag(reinterpret_cast<const char *>(chunk), 4);
                if (tag == "CLEN")
                    return fail(error, "its entries are compressed (a CLEN chunk), which no disc has and this does not read");
                size_t which = next;
                while (which < 5 && tag != kOrder[which])
                    ++which;
                if (which == 5)
                    return fail(error, "its directory has a " + tag + " chunk where DIDF, GRID, DLEN, OFST, NAME are read, in that order");
                chunks[which] = chunk + 8;
                chunkBytes[which] = bytes;
                next = which + 1;
                at += 8 + uint64_t(bytes);
            }
            for (size_t i = 0; i < 4; ++i)
                if (!chunks[i])
                    return fail(error, std::string("its directory has no ") + kOrder[i] + " chunk");
            if (chunkBytes[0] != 8)
                return fail(error, "its DIDF chunk is not 8 bytes");
            const uint32_t count = u32At(chunks[0]);
            archive.reserved = u32At(chunks[0] + 4);
            for (size_t i = 1; i < 4; ++i)
                if (chunkBytes[i] != uint64_t(count) * 4)
                    return fail(error, std::string("its ") + kOrder[i] + " chunk is not 4 bytes an entry");
            archive.entries.resize(count);
            std::vector<uint64_t> offsets(count), lengths(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                Entry &entry = archive.entries[i];
                entry.id = u32At(chunks[1] + 4 * i);
                const uint32_t length = u32At(chunks[2] + 4 * i);
                entry.flags = length >> 28;
                lengths[i] = length & kDirectoryLengthMask;
                offsets[i] = u32At(chunks[3] + 4 * i);
            }
            if (chunks[4])
            {
                archive.names = true;
                size_t at = 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const void *end = std::memchr(chunks[4] + at, 0, chunkBytes[4] - at);
                    if (!end)
                        return fail(error, "its NAME chunk has fewer names than entries");
                    const size_t length = size_t(static_cast<const uint8_t *>(end) - (chunks[4] + at));
                    archive.entries[i].name.assign(reinterpret_cast<const char *>(chunks[4] + at), length);
                    at += length + 1;
                }
                if (alignUp(at, 4) != chunkBytes[4])
                    return fail(error, "its NAME chunk is not its names padded to four bytes");
                for (; at < chunkBytes[4]; ++at)
                    if (chunks[4][at] != 0)
                        return fail(error, "its NAME chunk is padded with other bytes than zeros");
            }
            if (file && !readEntries(*file, directoryEnd, offsets, lengths, archive, error))
                return false;
            out = std::move(archive);
            return true;
        }

        // A zfile library's directory, and with `file` its entries too.
        bool readLibrary(const uint8_t *start, size_t size, const std::vector<uint8_t> *file, Archive &out, std::string *error)
        {
            if (size < kLibraryDirectoryAt || u32At(start) != kLibraryMagic)
                return fail(error, "not a zfile library");
            Archive archive;
            archive.format = Format::Library;
            std::memcpy(archive.header.data(), start + 4, archive.header.size());
            const uint16_t assets = u16At(start + kLibraryCountsAt), groups = u16At(start + kLibraryCountsAt + 2);
            if ((assets & kLibraryCompressedBit) || (groups & kLibraryCompressedBit) || (groups & kLibraryLzoBit))
                return fail(error, "it is a compressed library, which no disc has and this does not read");
            if (assets & kLibraryExternalBit)
                return fail(error, "its assets are files of their own (an external library), which no disc has and this does not read");
            archive.assetsPerGroup = assets & kLibraryAssetsMask;
            const size_t groupCount = groups & kLibraryGroupsMask;
            if (archive.assetsPerGroup == 0 && groupCount != 0)
                return fail(error, "its groups hold no assets");
            const uint64_t directoryEnd = kLibraryDirectoryAt + uint64_t(groupCount) * (4 + kLibraryEntryBytes * archive.assetsPerGroup);
            if (directoryEnd > size)
                return fail(error, "its directory goes past the end of the file");
            std::vector<uint64_t> offsets, lengths;
            const uint8_t *at = start + kLibraryDirectoryAt;
            for (size_t g = 0; g < groupCount; ++g)
            {
                const uint32_t id = u32At(at);
                at += 4;
                for (uint32_t a = 0; a < archive.assetsPerGroup; ++a, at += kLibraryEntryBytes)
                {
                    Entry entry;
                    entry.id = id;
                    entry.asset = a;
                    const uint32_t length = u32At(at + 4);
                    entry.flags = length >> 24;
                    offsets.push_back(kLibraryDataStart + uint64_t(u32At(at)));
                    lengths.push_back(length & kLibraryLengthMask);
                    archive.entries.push_back(std::move(entry));
                }
            }
            if (file && !readEntries(*file, directoryEnd, offsets, lengths, archive, error))
                return false;
            out = std::move(archive);
            return true;
        }

        // The gap's bytes: the entry's own while they are its size, else zeros.
        void putPadding(std::vector<uint8_t> &out, const std::vector<uint8_t> &kept, uint64_t gap)
        {
            if (kept.size() == gap)
                out.insert(out.end(), kept.begin(), kept.end());
            else
                out.insert(out.end(), size_t(gap), uint8_t(0));
        }

        // The entries after the directory (`out` holds it), each at its
        // alignment, the file ending at the last one's.
        void putEntries(const Archive &archive, std::vector<uint8_t> &out)
        {
            if (archive.entries.empty())
                return;
            putPadding(out, archive.leadPadding, alignUp(out.size(), alignmentOf(archive, archive.entries[0])) - out.size());
            for (size_t i = 0; i < archive.entries.size(); ++i)
            {
                const Entry &entry = archive.entries[i];
                out.insert(out.end(), entry.data.begin(), entry.data.end());
                const Entry &after = i + 1 < archive.entries.size() ? archive.entries[i + 1] : entry;
                putPadding(out, entry.padding, alignUp(out.size(), alignmentOf(archive, after)) - out.size());
            }
        }

        bool writeDirectoryArchive(const Archive &archive, std::vector<uint8_t> &out, std::string *error)
        {
            const uint32_t count = uint32_t(archive.entries.size());
            std::vector<uint8_t> names;
            if (archive.names)
            {
                for (const Entry &entry : archive.entries)
                {
                    if (entry.name.find('\0') != std::string::npos)
                        return fail(error, "an entry's name has a zero byte in it");
                    names.insert(names.end(), entry.name.begin(), entry.name.end());
                    names.push_back(0);
                }
                names.resize(size_t(alignUp(names.size(), 4)), 0);
            }
            for (const Entry &entry : archive.entries)
            {
                if (entry.flags > 15 || (entry.flags != 0 && alignmentOf(archive, entry) == 0))
                    return fail(error, "an entry's alignment code is not 0 to 15");
                if (entry.data.size() > kDirectoryLengthMask)
                    return fail(error, "an entry is 256 MB or more");
            }
            out.clear();
            const uint32_t tables = 3 * (8 + 4 * count);
            const uint32_t directory = (8 + 8) + tables + (archive.names ? 8 + uint32_t(names.size()) : 0);
            putTag(out, "ZDIR");
            putU32(out, directory);
            putTag(out, "DIDF");
            putU32(out, 8);
            putU32(out, count);
            putU32(out, archive.reserved);
            // The offsets follow from the lengths and alignments: laid out first.
            std::vector<uint32_t> offsets;
            {
                uint64_t at = 8 + uint64_t(directory);
                for (const Entry &entry : archive.entries)
                {
                    at = alignUp(at, alignmentOf(archive, entry));
                    if (at > 0xFFFFFFFFu)
                        return fail(error, "the archive would be 4 GB or more");
                    offsets.push_back(uint32_t(at));
                    at += entry.data.size();
                }
            }
            putTag(out, "GRID");
            putU32(out, 4 * count);
            for (const Entry &entry : archive.entries)
                putU32(out, entry.id);
            putTag(out, "DLEN");
            putU32(out, 4 * count);
            for (const Entry &entry : archive.entries)
                putU32(out, entry.flags << 28 | uint32_t(entry.data.size()));
            putTag(out, "OFST");
            putU32(out, 4 * count);
            for (uint32_t offset : offsets)
                putU32(out, offset);
            if (archive.names)
            {
                putTag(out, "NAME");
                putU32(out, uint32_t(names.size()));
                out.insert(out.end(), names.begin(), names.end());
            }
            putEntries(archive, out);
            return true;
        }

        bool writeLibrary(const Archive &archive, std::vector<uint8_t> &out, std::string *error)
        {
            const uint32_t perGroup = archive.assetsPerGroup;
            if (perGroup == 0 || perGroup > kLibraryAssetsMask)
                return fail(error, "a library's groups hold 1 to 8191 assets");
            if (archive.entries.size() % perGroup != 0)
                return fail(error, "its entries do not fill its groups of " + std::to_string(perGroup));
            const size_t groups = archive.entries.size() / perGroup;
            if (groups > kLibraryGroupsMask)
                return fail(error, "a library holds 16383 groups at most");
            for (size_t i = 0; i < archive.entries.size(); ++i)
            {
                const Entry &entry = archive.entries[i];
                if (entry.id != archive.entries[i - i % perGroup].id)
                    return fail(error, "the assets of group " + std::to_string(archive.entries[i - i % perGroup].id) + " have other group ids");
                if (entry.data.size() > kLibraryLengthMask)
                    return fail(error, "an asset is 16 MB or more");
                if (entry.flags > 0xFF)
                    return fail(error, "an asset's flags are more than a byte");
            }
            out.clear();
            putU32(out, kLibraryMagic);
            out.insert(out.end(), archive.header.begin(), archive.header.end());
            out.push_back(uint8_t(perGroup));
            out.push_back(uint8_t(perGroup >> 8));
            out.push_back(uint8_t(groups));
            out.push_back(uint8_t(groups >> 8));
            uint64_t at = kLibraryDirectoryAt + uint64_t(groups) * (4 + kLibraryEntryBytes * perGroup);
            for (size_t i = 0; i < archive.entries.size(); ++i)
            {
                const Entry &entry = archive.entries[i];
                if (i % perGroup == 0)
                    putU32(out, entry.id);
                at = alignUp(at, kLibraryAlignment);
                if (at - kLibraryDataStart > 0xFFFFFFFFu)
                    return fail(error, "the library would be 4 GB or more");
                putU32(out, uint32_t(at - kLibraryDataStart));
                putU32(out, entry.flags << 24 | uint32_t(entry.data.size()));
                at += entry.data.size();
            }
            putEntries(archive, out);
            return true;
        }

        bool isHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
        bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
    }

    uint32_t nameHash(std::string_view name)
    {
        uint32_t hash = 0;
        for (char c : name)
        {
            uint32_t letter = static_cast<uint8_t>(c);
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            hash = (hash << 4) + letter;
            const uint32_t high = hash & 0xF0000000u;
            if (high)
                hash ^= high >> 24;
            hash &= ~high;
        }
        return hash;
    }

    uint32_t alignmentOf(const Archive &archive, const Entry &entry)
    {
        if (archive.format == Format::Library)
            return kLibraryAlignment;
        const uint32_t code = entry.flags == 0 ? kDefaultAlignmentCode : entry.flags;
        return code > 15 ? 0 : 1u << (code - 1);
    }

    uint32_t alignmentCode(uint32_t bytes)
    {
        for (uint32_t code = 1; code <= 15; ++code)
            if (bytes == 1u << (code - 1))
                return code;
        return 0;
    }

    uint64_t directoryBytes(const uint8_t *start, size_t size)
    {
        if (size < kLibraryDirectoryAt)
            return 0;
        if (std::memcmp(start, "ZDIR", 4) == 0)
            return 8 + uint64_t(u32At(start + 4));
        if (u32At(start) == kLibraryMagic)
        {
            const uint16_t assets = u16At(start + kLibraryCountsAt) & kLibraryAssetsMask;
            const uint16_t groups = u16At(start + kLibraryCountsAt + 2) & kLibraryGroupsMask;
            return kLibraryDirectoryAt + uint64_t(groups) * (4 + kLibraryEntryBytes * assets);
        }
        return 0;
    }

    bool read(const std::vector<uint8_t> &file, Archive &out, std::string *error)
    {
        if (file.size() >= 4 && std::memcmp(file.data(), "ZDIR", 4) == 0)
            return readDirectoryArchive(file.data(), file.size(), &file, out, error);
        if (file.size() >= 4 && u32At(file.data()) == kLibraryMagic)
            return readLibrary(file.data(), file.size(), &file, out, error);
        return fail(error, "not a ZAL archive (neither a ZDIR archive nor a zfile library)");
    }

    bool readDirectory(const uint8_t *start, size_t size, Archive &out, std::string *error)
    {
        if (size >= 4 && std::memcmp(start, "ZDIR", 4) == 0)
            return readDirectoryArchive(start, size, nullptr, out, error);
        if (size >= 4 && u32At(start) == kLibraryMagic)
            return readLibrary(start, size, nullptr, out, error);
        return fail(error, "not a ZAL archive (neither a ZDIR archive nor a zfile library)");
    }

    bool write(const Archive &archive, std::vector<uint8_t> &out, std::string *error)
    {
        return archive.format == Format::Directory ? writeDirectoryArchive(archive, out, error) : writeLibrary(archive, out, error);
    }

    std::string idName(uint32_t id)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string name = "0x";
        for (int shift = 28; shift >= 0; shift -= 4)
            name += kHex[(id >> shift) & 15u];
        return name;
    }

    std::string libraryName(const Archive &archive, const Entry &entry)
    {
        std::string name = std::to_string(entry.id);
        if (archive.assetsPerGroup != 1)
            name += "-" + std::to_string(entry.asset);
        return name;
    }

    std::optional<size_t> findEntry(const Archive &archive, std::string_view fileName)
    {
        auto first = [&](uint32_t id, uint32_t asset) -> std::optional<size_t>
        {
            for (size_t i = 0; i < archive.entries.size(); ++i)
                if (archive.entries[i].id == id && archive.entries[i].asset == asset)
                    return i;
            return std::nullopt;
        };
        if (archive.format == Format::Directory)
        {
            // "0x" and eight hex digits, the name ending there or at a dot.
            if (fileName.size() >= 10 && fileName[0] == '0' && (fileName[1] == 'x' || fileName[1] == 'X') &&
                std::all_of(fileName.begin() + 2, fileName.begin() + 10, isHexDigit) && (fileName.size() == 10 || fileName[10] == '.'))
                return first(uint32_t(std::stoul(std::string(fileName.substr(2, 8)), nullptr, 16)), 0);
            return first(nameHash(fileName), 0);
        }
        // Digits, then "-" and digits (its place in its group), the name ending there or at a dot.
        size_t at = 0;
        auto number = [&](uint64_t &value)
        {
            const size_t begin = at;
            value = 0;
            while (at < fileName.size() && isDigit(fileName[at]) && at - begin < 10)
                value = value * 10 + uint64_t(fileName[at++] - '0');
            return at > begin && value <= 0xFFFFFFFFu && !(at < fileName.size() && isDigit(fileName[at]));
        };
        uint64_t group = 0, asset = 0;
        if (!number(group))
            return std::nullopt;
        if (at < fileName.size() && fileName[at] == '-')
        {
            ++at;
            if (!number(asset))
                return std::nullopt;
        }
        else if (archive.assetsPerGroup != 1)
            return std::nullopt;
        if (at < fileName.size() && fileName[at] != '.')
            return std::nullopt;
        return first(uint32_t(group), uint32_t(asset));
    }
}
