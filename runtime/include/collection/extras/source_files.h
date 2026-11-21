#pragma once

// The files a converter reads (collection/extras): one release's assets
// folder, from an installed disc folder (the PS2 disc's, <support>/games/
// <id>/disc) or straight from a disc image (an Xbox or GameCube disc's,
// collection/extras/disc_image.h). Paths are '/'-separated and relative to
// the assets folder ("levels/civic/civic.txr"), found in any case.
//
// The order of files() is the tooling's: tools/*.py walk a folder with
// os.walk, directories and files sorted by name, a folder's files before its
// folders' (sourceOrderLess). The converters keep it, so they write what the
// Python tools write.

#include "collection/extras/disc_image.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zx::app::extras
{
    class SourceFiles
    {
    public:
        virtual ~SourceFiles() = default;
        // Every file below the assets folder, in the tooling's walk order.
        const std::vector<std::string> &files() const { return m_files; }
        // The path with the source's case of a path in any case, or none.
        std::optional<std::string> find(std::string_view path) const;
        // Whether a folder of this path (any case) holds anything.
        bool hasFolder(std::string_view path) const;
        // The file's bytes (a path as files() or find() give it).
        virtual bool read(const std::string &path, Bytes &out) const = 0;
        // Reads a file found in any case; false when there is none.
        bool readAny(std::string_view path, Bytes &out) const;
        // The bytes' size, without reading them.
        virtual uint64_t size(const std::string &path) const = 0;

    protected:
        // Sets the files (any order) and their case-free index.
        void setFiles(std::vector<std::string> files);

    private:
        std::vector<std::string> m_files;
        std::map<std::string, std::string> m_byLower;
    };

    // An extracted folder's assets: the folder itself when it is an assets
    // folder, else its assets or media/assets (any case) that holds one of
    // `markers` ("levels").
    class FolderFiles final : public SourceFiles
    {
    public:
        // False when there is no such assets folder.
        bool open(const std::filesystem::path &root, const std::vector<std::string> &markers);
        bool read(const std::string &path, Bytes &out) const override;
        uint64_t size(const std::string &path) const override;
        // The assets folder (its name with the disc's case, "ASSETS").
        const std::filesystem::path &assets() const { return m_assets; }

    private:
        std::filesystem::path m_assets;
    };

    // A disc image's assets folder: the files under `prefix` ("media/assets/"
    // on an Xbox disc, "assets/" on a GameCube one, any case).
    class ImageFiles final : public SourceFiles
    {
    public:
        // False when nothing is under the prefix. The disc stays the caller's.
        bool open(DiscFiles &disc, std::string_view prefix);
        bool read(const std::string &path, Bytes &out) const override;
        uint64_t size(const std::string &path) const override;

    private:
        DiscFiles *m_disc = nullptr;
        std::map<std::string, const DiscFile *> m_entries;
    };

    // Another source's files with some of them, or more, given in memory
    // over it (a custom map's files over the game's disc: panel_maps.h
    // makeMapPanelFile). A path given here (any case) is read from here.
    class LayeredFiles final : public SourceFiles
    {
    public:
        // `base` stays the caller's and outlives this.
        LayeredFiles(const SourceFiles &base, std::map<std::string, Bytes> over);
        bool read(const std::string &path, Bytes &out) const override;
        uint64_t size(const std::string &path) const override;

    private:
        const SourceFiles &m_base;
        std::map<std::string, Bytes> m_over; // by lower-case path
    };

    // os.walk's order with sorted names: by folder, a folder's files (by
    // name) before its folders (by name), folders depth first.
    bool sourceOrderLess(std::string_view a, std::string_view b);

    // ASCII upper and lower case (the PS2 discs' names are upper case).
    std::string upperAscii(std::string_view text);
    std::string lowerAscii(std::string_view text);

    // Writes `data` to `path` (its folders made), whole or not at all.
    bool writeFile(const std::filesystem::path &path, const Bytes &data, std::string *error);
    bool writeFile(const std::filesystem::path &path, std::string_view text, std::string *error);
}
