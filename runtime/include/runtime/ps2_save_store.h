#pragma once

// Native save files for games whose memory card saves are replaced by host
// files (see the game overrides). No game knowledge in here: files are byte
// blobs, written atomically with a backup of the previous version, read back
// with a validator that falls back to the backup, and imported once from the
// memory card HLE's host folders (one folder per card, one subfolder per
// save, as Kernel/Stubs/MemoryCard.cpp lays them out).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::saves
{
    using Bytes = std::vector<uint8_t>;
    using Validator = std::function<bool(const Bytes &)>;

    // u32 sum of all bytes (the checksum many PS2 games put in front of
    // their save data).
    uint32_t byteSum(const uint8_t *data, size_t size);

    // A name usable as a file name everywhere: letters, digits, space, '-',
    // '_' and '.' kept, anything else '_', no leading dots or spaces;
    // fallback when nothing is left.
    std::string safeFileName(const std::string &name, const std::string &fallback);

    // <file>.bak and <file>.tmp
    std::filesystem::path backupPath(const std::filesystem::path &file);
    std::filesystem::path tempPath(const std::filesystem::path &file);

    // Writes data to file.tmp, flushes it to disk, keeps the current file
    // (if any) as file.bak and renames the temporary file over file. At any
    // moment either file or file.bak holds a complete save. Creates the
    // parent folder.
    bool writeFileAtomic(const std::filesystem::path &file, const uint8_t *data, size_t size,
                         std::string *error = nullptr);

    // writeFileAtomic on a background thread, for files written while a
    // game runs (the flush to disk takes tens of ms on Windows, a stall on
    // the thread that asks). Requests run in order; a newer request for a
    // file whose write has not started replaces the older one (the newest
    // data is what lands). An error goes to stderr with `what`. Pending
    // writes are finished before the process exits and before
    // readWithBackup reads.
    void writeFileAtomicLater(const std::filesystem::path &file, Bytes data, const std::string &what = {});
    // Waits until every write asked so far is done.
    void flushWrites();

    enum class ReadStatus
    {
        Ok,         // file was valid
        FromBackup, // file missing or invalid, file.bak valid
        Missing,    // neither exists
        Invalid,    // something exists, nothing valid
    };

    struct ReadResult
    {
        ReadStatus status = ReadStatus::Missing;
        Bytes data;
        bool ok() const { return status == ReadStatus::Ok || status == ReadStatus::FromBackup; }
    };

    // Reads file, or file.bak when file is missing or rejected by valid
    // (an empty validator accepts anything).
    ReadResult readWithBackup(const std::filesystem::path &file, const Validator &valid = {});

    bool readFile(const std::filesystem::path &file, Bytes &out);

    // One-time import from memory card host folders. For every save folder
    // on the cards (in card order, folders by name), target(folderName)
    // names the native file to create from the file inside it that has the
    // folder's own name (the PS2 convention: BASLUS-20327CEAIGAME/
    // BASLUS-20327CEAIGAME), or returns an empty path to skip it. Data the
    // validator rejects is skipped; an existing target is never replaced;
    // data identical to a file already imported in this run is not imported
    // twice. The cards are only read.
    struct ImportedSave
    {
        std::filesystem::path from;
        std::filesystem::path to;
    };
    using ImportTarget = std::function<std::filesystem::path(const std::string &folderName)>;
    std::vector<ImportedSave> importFromCardFolders(const std::vector<std::filesystem::path> &cardFolders,
                                                    const ImportTarget &target, const Validator &valid = {},
                                                    std::string *error = nullptr);
}
