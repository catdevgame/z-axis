#pragma once

// A game set up from the player's own disc (the collection's first run):
// the disc image or folder the player picks or drops is checked (its
// SYSTEM.CNF names the boot ELF, whose SHA-256 says which game and release
// it is), its files are copied into the game's folder in the support folder
// (<support>/games/<id>/disc, through a staging folder, with a manifest
// written last), and the game's code is made ready (GameCodeStep). Each step
// reports its progress, and every failure says in plain words what to do.
// The app reads the installed folder first and the files bundled with it
// second (a workshop build).

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    class Iso9660Image;
}

namespace ps2x::app
{
    // A game the setup can install, as the app knows it.
    struct SetupGame
    {
        std::string id;        // "ai", "dm2", "bmx" (the collection's id)
        std::string title;     // for the player ("Aggressive Inline (USA)")
        std::string elf;       // the boot ELF of the release it runs ("SLUS_203.27")
        std::string elfSha256; // of that ELF ("" = any ELF of that name)
    };

    // A document the system's picker gives in place of a path (Android's
    // content:// URIs: the app may read no path outside its own folders),
    // read through files the platform opens for it. The platform installs
    // its access once (installDocumentAccess, collection/android_documents.cpp);
    // with none, every source is a path. A document's URI goes wherever a
    // source's path goes: DiscSource::open, discSourcesIn, the manifest's
    // source (the Games page's repair row).
    class DocumentAccess
    {
    public:
        struct Entry
        {
            std::string uri;
            std::string name; // its file name, as the picker shows it ("Aggressive Inline (USA).iso")
            uint64_t size = 0;
            bool folder = false;
        };
        virtual ~DocumentAccess() = default;
        // Whether the text names a document (else it is a path).
        virtual bool isDocument(std::string_view text) const = 0;
        // What the document is; false when it cannot be read (gone, or its
        // permission given up).
        virtual bool stat(const std::string &uri, Entry &out) const = 0;
        // A folder's documents, one level.
        virtual std::vector<Entry> list(const std::string &folderUri) const = 0;
        // The document open for reading ("rb"), the caller's to close; null
        // when it cannot be read.
        virtual std::FILE *open(const std::string &uri) const = 0;
    };
    // The platform's documents (none: null); kept for the program's life.
    void installDocumentAccess(const DocumentAccess *access);
    const DocumentAccess *documentAccess();
    // Whether this source (a path or a document's URI) names a document.
    bool isDocument(const std::filesystem::path &source);

    // Where a disc's files come from: a disc image (an ISO of 2048-byte
    // sectors, the BIN of a BIN/CUE dump or its CUE, a CHD, a CSO or a ZSO:
    // told apart by their bytes, not their names) or a folder holding the
    // disc's files (SYSTEM.CNF at its top); either a path or a document.
    class DiscSource
    {
    public:
        enum class Kind
        {
            None,
            Folder,
            IsoImage, // 2048-byte sectors (an ISO, or a CHD, CSO or ZSO of one)
            RawImage, // a CD's raw sectors (a BIN, named by its CUE or not, or a CD's CHD)
        };

        DiscSource();
        ~DiscSource();
        DiscSource(DiscSource &&) noexcept;
        DiscSource &operator=(DiscSource &&) noexcept;

        // False with *error in plain words when the path is none of these.
        bool open(const std::filesystem::path &path, std::string *error);
        Kind kind() const { return m_kind; }
        // What the player picked (a CUE stays the CUE) and what is read.
        const std::filesystem::path &picked() const { return m_picked; }
        const std::filesystem::path &readFrom() const { return m_readFrom; }
        // The picked file's or folder's name, for the player (a document's
        // as the picker shows it).
        const std::string &name() const { return m_name; }
        // "disc image", "BIN/CUE disc image", "CHD disc image" (CSO, ZSO), "folder".
        std::string kindText() const;

        struct File
        {
            std::string path; // '/'-separated, as on the disc ("ASSETS/LEVELS/GAME.BIN")
            uint64_t size = 0;
        };
        // Every file, in the order of the disc's directories.
        std::vector<File> files() const;
        bool readFile(const std::string &path, std::vector<uint8_t> &out, size_t maxBytes) const;
        // The file's bytes piece by piece; `sink` returning false stops (false).
        bool streamFile(const std::string &path, const std::function<bool(const uint8_t *, size_t)> &sink) const;

    private:
        // `siblings`: the documents beside it (a CUE's BIN among them),
        // null when it was picked alone.
        bool openDocument(const DocumentAccess::Entry &document, const std::vector<DocumentAccess::Entry> *siblings,
                          std::string *error);
        // A folder document's file by the disc's path: null when it has none.
        const DocumentAccess::Entry *folderDocument(const std::string &path) const;

        Kind m_kind = Kind::None;
        std::filesystem::path m_picked;
        std::filesystem::path m_readFrom;
        std::string m_name;
        std::shared_ptr<ps2x::iop::Iso9660Image> m_image;
        // A folder document's files, each with the disc's own path as its
        // name ('/'-separated, as files() gives them), sorted by it.
        std::vector<DocumentAccess::Entry> m_folderDocuments;
    };

    // The sources a picked or dropped path or document stands for: itself
    // when it is a disc; for a folder that is none, each image or folder in
    // it that is one (a folder of every game's dumps); else itself (for its
    // message).
    std::vector<std::filesystem::path> discSourcesIn(const std::filesystem::path &path);

    // A source as an installed game's manifest names it (a path or a
    // document): whether it can still be read, and its name for the player.
    bool sourceReadable(const std::filesystem::path &source);
    // Whether a source (a path or a document) is a CUE sheet, by its text.
    bool isCueSheetSource(const std::filesystem::path &source);
    std::string sourceName(const std::filesystem::path &source);

    // The file a CUE sheet's data track is in (ps2x/iop/cue_sheet.h), beside
    // the CUE; empty with *error when there is none.
    std::filesystem::path binOfCue(const std::filesystem::path &cue, std::string *error);

    // A boot ELF of another release of one of the games (from Redump's list
    // of their PS2 discs): which game, and what the player has, in words.
    struct OtherRelease
    {
        const char *elf;  // "SLES_504.80"
        const char *game; // the collection's id
        const char *what; // "the European release (SLES-50480)"
    };
    const OtherRelease *otherRelease(std::string_view bootElf);

    struct SourceCheck
    {
        enum class Status
        {
            Ok,
            Unreadable,   // not a disc image or folder the setup reads
            NotPs2Disc,   // no SYSTEM.CNF with a boot ELF, or that ELF missing
            OtherGame,    // one of the games, but not the one asked for
            OtherRelease, // one of the games, another release (another country, a demo)
            WrongVersion, // the right ELF's name, other contents
            UnknownGame,  // a PS2 disc of none of the games
        };
        Status status = Status::Unreadable;
        std::string message; // for the player
        int game = -1;       // the game it is (an index of the games given), -1 none
        std::string bootElf;
        std::string elfSha256;
        uint64_t bytes = 0; // the disc's files
        size_t files = 0;
    };
    // `wanted` >= 0: the game the player meant (its row); -1: whichever of
    // `games` it is.
    SourceCheck checkSource(const DiscSource &source, const std::vector<SetupGame> &games, int wanted = -1);

    // ---- installing ------------------------------------------------------

    struct SetupProgress
    {
        std::string step;  // "Copying files", the code step's name
        std::string file;  // the file being copied, "" none
        uint64_t done = 0; // bytes (or the step's own units) done of total
        uint64_t total = 0;
    };
    // Called as a step goes on; false stops the install (cancelled).
    using SetupProgressFn = std::function<bool(const SetupProgress &)>;

    // The last step of an install: the game's code made ready for the ELF
    // the installed disc boots. The app's own code (PrebuiltGameCode) is
    // ready when that ELF is the one it was made from; a step that makes the
    // code from the player's disc takes its place behind this interface.
    class GameCodeStep
    {
    public:
        virtual ~GameCodeStep() = default;
        // The step's name on the screens ("Preparing the game's code").
        virtual std::string name() const = 0;
        // Whether the code for the disc in `discFolder` is ready.
        virtual bool ready(const SetupGame &game, const std::filesystem::path &discFolder,
                           const std::filesystem::path &gameDir) const = 0;
        // Makes it (progress through `progress`); false with *error in plain
        // words, or cancelled when `progress` said stop.
        virtual bool make(const SetupGame &game, const std::filesystem::path &discFolder,
                          const std::filesystem::path &gameDir, const SetupProgressFn &progress, std::string *error) = 0;
    };

    // The code built into the app (the workshop's recompiled code): ready
    // when the disc's boot ELF has the SHA-256 the app was built from.
    class PrebuiltGameCode final : public GameCodeStep
    {
    public:
        std::string name() const override { return "Checking game code"; }
        bool ready(const SetupGame &game, const std::filesystem::path &discFolder,
                   const std::filesystem::path &gameDir) const override;
        bool make(const SetupGame &game, const std::filesystem::path &discFolder, const std::filesystem::path &gameDir,
                  const SetupProgressFn &progress, std::string *error) override;
    };

    // The code step this app uses: the place where a step that makes the
    // code from the player's disc comes in. Today the app's own code
    // (PrebuiltGameCode).
    std::unique_ptr<GameCodeStep> makeGameCodeStep();

    // The game's files: <gameDir>/disc, complete once <gameDir>/disc.manifest
    // is there (written last).
    std::filesystem::path installedDiscFolder(const std::filesystem::path &gameDir);
    std::filesystem::path installManifestFile(const std::filesystem::path &gameDir);

    struct InstallResult
    {
        bool ok = false;
        bool cancelled = false;
        bool filesInstalled = false; // the files are in place (the code step may still have failed)
        std::string message; // for the player
        uint64_t bytes = 0;
        size_t files = 0;
    };
    // Checks the source (it must be `game`), copies its files into
    // <gameDir>/disc.partial, checks the copy's boot ELF, puts it in place of
    // an earlier <gameDir>/disc, writes the manifest, then runs `code` (none:
    // no code step).
    InstallResult installGame(const DiscSource &source, const SetupGame &game, const std::filesystem::path &gameDir,
                              GameCodeStep *code, const SetupProgressFn &progress = {});

    struct InstalledCheck
    {
        enum class Status
        {
            NotInstalled, // no files in the support folder
            Ok,
            Damaged,      // files missing, of another size or (a full check) other contents
            CodeMissing,  // the files are fine, the code step is not done
            ByHand,       // a disc folder with no manifest (copied there by hand): used as it is
        };
        Status status = Status::NotInstalled;
        std::string message; // for the player
        std::vector<std::string> problems; // the files that are wrong, first few
        uint64_t bytes = 0;
        size_t files = 0;
        std::filesystem::path source; // the image or folder it was installed from
    };
    // Quick (full = false): the manifest, every file's size and the boot
    // ELF's SHA-256; full: every file's SHA-256 too (progress through
    // `progress`). `code` (none: not checked) says whether the code step is done.
    InstalledCheck checkInstalled(const std::filesystem::path &gameDir, const SetupGame &game, bool full,
                                  const GameCodeStep *code = nullptr, const SetupProgressFn &progress = {});

    // The installed files (and the manifest) removed.
    bool removeInstalled(const std::filesystem::path &gameDir, std::string *error);

    // "1.4 GB", "960 MB".
    std::string sizeText(uint64_t bytes);

    // Paths from and to UTF-8 text (a dropped file's, a manifest's line), on
    // every platform.
    std::filesystem::path pathFromUtf8(std::string_view text);
    std::string utf8Of(const std::filesystem::path &path);
}
