#pragma once

// The game's disc files read straight from the host: the extracted disc
// folder (or the disc image), with mod folders on top and in-memory
// patches (runtime/ps2_disc_patch.h) on top of that. A game override that
// replaces the game's own file functions opens every file through
// GameFileSystem::open, so there is one place where a file can be served
// from somewhere else than the disc.
//
// Lookup order for a disc path ("dvd0:\ASSETS\UILIB.ZAL;1" and
// "assets/uilib.zal" are the same file):
//   0. a memory folder's file for the path, the last registered first
//      (registerMemoryFolder: files a host made for this boot and gives it
//      in memory, read over every mod folder; none unless a host registers
//      one)
//   1. <mod>/<path> in each mod folder, the last given first (loose
//      replacement files at the game's own paths, names case-insensitive;
//      the folders come from ZX_MODS_DIR, modsRoots: the collection lists
//      the mods and their order, collection/mods.h). A mod folder may be a
//      zip, or a folder inside one, read in place (runtime/ps2_zip_archive.h).
//      For a .ZAL archive a mod may instead have a folder of that name
//      holding single entries (ASSETS/UILIB.ZAL/CommonFonts.zif): each mod's
//      entries above the file found (a mod's below them, or 2 or 3) replace
//      those of the archive, the lowest mod's first, so mods with other
//      entries of one archive all count (zx/zal_archive.h: findEntry
//      names an entry; one the archive has not is left out, with a line in
//      the log). The archive made is kept for the boot (every file system
//      over the same folders shares it).
//   2. a registered provider for the path (files a game override makes in
//      memory, e.g. a level converted from another release of the game)
//   3. the disc folder, or the disc image when there is no folder
// and then, whichever file was found, a registered head for its path is
// served over its first bytes (registerHead: the rest of the file is read
// through, so a large file is never read whole; made once a boot, as the
// archives with mods' entries are), or a registered patch for its name
// changes its bytes in memory.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zx::disc
{
    class Iso9660Image;
}

namespace zx::game_files
{
    // "cdrom0:\ASSETS\UILIB.ZAL;1" -> "ASSETS/UILIB.ZAL": device prefix and
    // version suffix dropped, '\' -> '/', no leading or doubled '/', "." and
    // ".." resolved (".." never leaves the root), upper case.
    std::string normalizePath(std::string_view path);

    class GameFile
    {
    public:
        enum class Origin
        {
            Disc,
            Mod,
            Patched,
            Provided,
        };

        virtual ~GameFile() = default;
        [[nodiscard]] virtual uint64_t size() const = 0;
        // Reads up to `size` bytes at `offset`; returns the number read (0 at
        // or past the end, or on a host read error). Safe from any thread (a
        // movie's sound and its decoding thread read the same file).
        virtual size_t read(uint64_t offset, void *destination, size_t size) = 0;
        [[nodiscard]] Origin origin() const noexcept { return m_origin; }
        // Where it came from, for logs: the host file or "<image>:<path>".
        [[nodiscard]] const std::string &source() const noexcept { return m_source; }

    protected:
        Origin m_origin = Origin::Disc;
        std::string m_source;
        friend class GameFileSystem;
    };

    struct GameFileRoots
    {
        std::filesystem::path disc;                          // extracted disc folder
        std::shared_ptr<const zx::disc::Iso9660Image> image; // used when disc is empty
        std::vector<std::filesystem::path> mods;             // overlays, lowest first: a later one's file wins
    };

    // ZX_MODS_DIR: one or more folders with the platform's path-list
    // separator between them (';' on Windows, ':' elsewhere), lowest first.
    constexpr char kModsSeparator =
#if defined(_WIN32)
        ';';
#else
        ':';
#endif
    // The folders of a ZX_MODS_DIR text, in its order (an empty entry
    // dropped).
    std::vector<std::filesystem::path> modsRoots(std::string_view text);
    // The text for those folders.
    std::string modsText(const std::vector<std::filesystem::path> &roots);

    // A mod's own files for the host's readers that look in a mod folder
    // rather than at a disc path (a texture pack's pictures, say): `folder`
    // is a folder, a zip, or a folder inside a zip
    // ("<...>/Mods/hd.zip/Textures/ai"), read in place.
    // Every file below `folder` (in any folder below it), as paths relative
    // to it with '/', in name order (a zip's: by their upper-case names).
    std::vector<std::string> listFilesBelow(const std::filesystem::path &folder);
    // One of them (`relative` as listFilesBelow gives it, or any case): its
    // bytes, or false. Safe from any thread, on the same zip too.
    bool readFileBelow(const std::filesystem::path &folder, const std::string &relative, std::vector<uint8_t> &out);

    // A mod's single entries of an archive (above): whether a path is an
    // archive's (it ends in .ZAL), and the files of such a folder that are
    // passed over without a word, being no entries: zal.txt (zal's list,
    // in a folder copied in whole) and the host's own (".DS_Store", any name
    // starting with a dot).
    bool isArchivePath(std::string_view normalized);
    bool ignoredInArchiveFolder(std::string_view name);

    struct ZipPath; // runtime/ps2_zip_archive.h

    class GameFileSystem
    {
    public:
        explicit GameFileSystem(GameFileRoots roots);

        // The one place every game file is opened. Null if it is in neither
        // the mods folder nor on the disc (or is a directory).
        std::shared_ptr<GameFile> open(std::string_view path);
        // A file or a directory of that name exists (mods folder or disc).
        bool exists(std::string_view path);

        [[nodiscard]] const GameFileRoots &roots() const noexcept { return m_roots; }

    private:
        // Host path of `normalized` under `root`, matching each component
        // case-insensitively; empty if there is none.
        std::filesystem::path resolveHost(const std::filesystem::path &root, const std::string &normalized,
                                          bool wantDirectory = false);
        std::shared_ptr<GameFile> openOriginal(const std::string &normalized);
        // A provider's file, else the disc's (2 and 3 above).
        std::shared_ptr<GameFile> openBelowMods(const std::string &normalized);
        // A mod's file at the path (null for none), and its folder of the
        // archive's entries at the path (empty for none).
        std::shared_ptr<GameFile> openInMod(const std::filesystem::path &root, const std::string &normalized);
        std::filesystem::path entryFolderIn(const std::filesystem::path &root, const std::string &normalized);
        // The archive `base` with the entries of `folders` (the lowest mod's
        // first) put in it; `base` as it is when it is no archive.
        std::shared_ptr<GameFile> withEntries(const std::string &normalized, std::shared_ptr<GameFile> base,
                                              const std::vector<std::filesystem::path> &folders);
        // The zip a mod folder is in (or is), null for a folder on the host.
        const ZipPath &zipRoot(const std::filesystem::path &root);

        GameFileRoots m_roots;
        std::map<std::string, std::shared_ptr<ZipPath>> m_zipRoots;
        // Upper-case name -> name on disk, per host directory.
        std::unordered_map<std::string, std::map<std::string, std::string>> m_listings;
    };

    // Files made in memory for paths below a folder: `provide` gets the
    // normalized path ("ASSETS/LEVELS/ZOO/ZOO.TXR") and returns the file's
    // contents, or null to leave the path to the disc. It is asked for
    // every open and exists() below `folder` (a normalized prefix ending
    // in '/'), so it should keep what it makes. Several providers: the
    // first registered that answers wins. Providers are the boot's: a game
    // booted after another in the same process starts with none
    // (runtime/ps2_boot_config.h).
    using Provider = std::function<std::shared_ptr<const std::vector<uint8_t>>(const std::string &normalizedPath)>;
    void registerProvider(const std::string &folder, Provider provide, std::string name = {});
    // Every provider registered with this name (a game's session ending).
    void removeProviders(const std::string &name);
    void clearProviders();

    // A new head for a file: `make` gets the file found at the path (any of
    // 0 to 3 above) and returns the bytes served over its first ones (a
    // sound bank's tables with more waves, say), or null to leave the file as
    // it is; the bytes after them are the file's own, and a head longer than
    // the file makes it that long. Asked once a boot for each file found at
    // the path (by its source and size); the boot's, as providers are.
    using Head = std::function<std::shared_ptr<const std::vector<uint8_t>>(GameFile &file)>;
    void registerHead(const std::string &path, Head make, std::string name = {});
    void removeHeads(const std::string &name);

    // A memory folder: files by their disc path (normalized when
    // registered), each its bytes: files a host made for a boot, given to it
    // without writing them anywhere. The host registers it before the boot
    // and removes it after; every GameFileSystem reads it over its mod
    // folders, and a directory of its paths exists. Registering a name
    // again replaces that folder.
    using MemoryFolder = std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>;
    void registerMemoryFolder(const std::string &name, MemoryFolder files);
    void removeMemoryFolder(const std::string &name);

    // Reads a whole file (for patches and tests).
    bool readAll(GameFile &file, std::vector<uint8_t> &bytes);
}
