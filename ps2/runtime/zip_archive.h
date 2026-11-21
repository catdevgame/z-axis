#pragma once

// A zip file read as a folder, in place: the files of a mod packed as a zip
// (collection/mods.h) are read from it, never unpacked anywhere. Opening
// reads the zip's central directory (its list of files) and nothing else; a
// file's bytes are read, and inflated, when it is read, one file at a time,
// and not kept. Zips of one folder (a "mod/" holding everything, as zipping
// a folder makes) are read from inside that folder; the Mac's __MACOSX
// folder and .DS_Store files are left out, and so is an entry whose size
// its bytes cannot inflate to (a damaged or made-up header: reading makes
// room for that size first).
//
// Paths in it are compared as the game's file system compares them
// (game_files::normalizePath: upper case, '/'), and given back as the zip
// stores them (their case kept), relative to the zip's root.
//
// Safe from any thread: one reader at a time reads the zip.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zx::game_files
{
    class ZipArchive
    {
    public:
        // The zip at `file`, shared: every open of the same file (unchanged
        // since: the same size and time) gets the same one. Null (with why)
        // when it is not a zip that can be read.
        static std::shared_ptr<ZipArchive> open(const std::filesystem::path &file, std::string *error = nullptr);
        ~ZipArchive();
        ZipArchive(const ZipArchive &) = delete;
        ZipArchive &operator=(const ZipArchive &) = delete;

        [[nodiscard]] const std::filesystem::path &file() const noexcept { return m_file; }
        // Its files' count (folders not counted).
        [[nodiscard]] size_t files() const noexcept { return m_entries.size(); }

        // Whether it has a file, or a folder (a file below it), at a
        // normalized path; "" is its root, which always exists.
        [[nodiscard]] bool hasFile(const std::string &normalized) const;
        [[nodiscard]] bool hasFolder(const std::string &normalized) const;
        // A file's size, 0 for none.
        [[nodiscard]] uint64_t fileSize(const std::string &normalized) const;
        // Every file below the folder at a normalized path ("" the root),
        // in any folder below it, as paths relative to that folder with '/'
        // (the zip's case), in the order of their normalized paths.
        [[nodiscard]] std::vector<std::string> filesBelow(const std::string &normalizedFolder) const;
        // A file's bytes, read and inflated now; false when it has none or
        // the zip cannot be read (damaged, changed on disk).
        bool read(const std::string &normalized, std::vector<uint8_t> &out) const;

    private:
        struct Entry
        {
            uint32_t index = 0;   // in the zip's central directory
            uint64_t size = 0;    // inflated
            std::string name;     // relative to the root, the zip's case
        };
        ZipArchive() = default;

        std::filesystem::path m_file;
        std::map<std::string, Entry> m_entries; // by normalized path
        mutable std::mutex m_mutex;
        struct Reader;
        std::unique_ptr<Reader> m_reader;
    };

    // Whether a path names a zip file (a regular file whose extension is
    // .zip in any case).
    bool isZipFile(const std::filesystem::path &path);

    // A path that may go through a zip ("<...>/Mods/hd.zip/Textures/ai"):
    // the zip (null for a path through none) and the normalized path inside
    // it ("TEXTURES/AI"; "" the zip's root).
    struct ZipPath
    {
        std::shared_ptr<ZipArchive> zip;
        std::string inside;
    };
    ZipPath zipPathOf(const std::filesystem::path &path);
}
