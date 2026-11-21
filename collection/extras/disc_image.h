#pragma once

// The other releases' discs the player adds as extras, read as the setup
// needs them (collection/extras/setup.h): an Xbox disc image (an XISO, or a
// full image with the game's XDVDFS volume inside) and a GameCube disc image
// (a plain GCM/ISO, or Dolphin's RVZ). The setup reads the files it converts
// straight from the image; nothing reads an image while a game runs.
//
// Both kinds list their files as '/'-separated paths with their case on the
// disc ("media/assets/levels/civic/civic.bin", "assets/levels/TUT/Estate/
// Estate.GEO") and read them by that entry.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zx::app::extras
{
    using Bytes = std::vector<uint8_t>;

    // An image file's bytes: a file opened by path, or a std::FILE* the
    // platform opened (Android's documents, game_setup.h DocumentAccess),
    // which it then owns. Not thread-safe: its DiscImage locks around it.
    class ImageFile
    {
    public:
        // Null with *error in plain words when it cannot be opened.
        static std::unique_ptr<ImageFile> open(const std::filesystem::path &path, std::string *error);
        // `file` (opened "rb") from now on closed by this; `name` for messages.
        static std::unique_ptr<ImageFile> adopt(std::FILE *file, std::string name, std::string *error);
        ~ImageFile();
        ImageFile(const ImageFile &) = delete;
        ImageFile &operator=(const ImageFile &) = delete;

        uint64_t size() const { return m_size; }
        const std::string &name() const { return m_name; }
        // `count` bytes at `offset`; false past the end or on a read error.
        bool readAt(uint64_t offset, size_t count, uint8_t *out);

    private:
        ImageFile() = default;
        std::FILE *m_file = nullptr;
        uint64_t m_size = 0;
        std::string m_name;
    };

    // A disc image's bytes as the disc holds them, whatever the container.
    class DiscImage
    {
    public:
        virtual ~DiscImage() = default;
        // The disc's size in bytes (an RVZ's: the image it expands to).
        virtual uint64_t size() const = 0;
        // `count` bytes at `offset` into `out`; false when they run past the
        // end or the image cannot be read (a damaged RVZ group).
        virtual bool read(uint64_t offset, size_t count, uint8_t *out) = 0;
        // For a log line ("RVZ, zstd level 19, 128 KiB chunks").
        virtual std::string describe() const = 0;
    };

    // The image at `path`: an RVZ (by its magic) or a plain image read as it
    // is. Null with *error in plain words when it cannot be opened (a WIA, a
    // damaged RVZ header, an RVZ of a Wii disc or with a compression the app
    // does not read).
    std::unique_ptr<DiscImage> openDiscImage(const std::filesystem::path &path, std::string *error);
    std::unique_ptr<DiscImage> openDiscImage(std::unique_ptr<ImageFile> file, std::string *error);

    // A file on a disc.
    struct DiscFile
    {
        std::string path;    // '/'-separated, with the disc's case
        uint64_t offset = 0; // in the disc's bytes
        uint64_t size = 0;
    };

    // A disc's files, from its own file system.
    class DiscFiles
    {
    public:
        virtual ~DiscFiles() = default;
        // Every file, in the order its file system lists them.
        const std::vector<DiscFile> &files() const { return m_files; }
        // The file of this path in any case, or null.
        const DiscFile *find(std::string_view path) const;
        // The files whose path matches one of `patterns` (selectFiles).
        std::vector<const DiscFile *> select(const std::vector<std::string> &patterns) const;
        // A file's bytes; false when the image cannot be read.
        bool readFile(const DiscFile &file, Bytes &out);
        // A file's bytes piece by piece (at most 4 MiB a call); `sink`
        // returning false stops (false).
        bool streamFile(const DiscFile &file, const std::function<bool(const uint8_t *, size_t)> &sink);
        DiscImage &image() { return *m_image; }

    protected:
        std::unique_ptr<DiscImage> m_image;
        std::vector<DiscFile> m_files;
    };

    // Whether a disc path matches a pattern, in any case: the path itself, a
    // folder holding it ("assets/levels/TUT/Estate"), or a glob where `*`
    // (which also crosses '/'), `?` and `[...]` match as Python's fnmatch
    // does (tools/modding/gc_disc.py's select).
    bool pathMatches(std::string_view path, std::string_view pattern);

    // Python's fnmatch.fnmatchcase: `*` any run (also across '/'), `?` one
    // character, `[...]` a set (`!` first negates, `a-z` a range).
    bool globMatches(std::string_view text, std::string_view pattern);

    // The files matching any of `patterns`, each once, in the disc's order.
    std::vector<const DiscFile *> selectFiles(const std::vector<DiscFile> &files,
                                              const std::vector<std::string> &patterns);

    // Writes `files` under `folder` (<folder>/<disc path>), each written
    // whole or not at all. False with *error when a file cannot be read or
    // written, or a path would leave the folder.
    bool extractFiles(DiscFiles &disc, const std::vector<const DiscFile *> &files, const std::filesystem::path &folder,
                      std::string *error);
}
