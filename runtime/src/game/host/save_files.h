#pragma once

// The host's services to the engine's save jobs (zge/host/save_jobs.cpp): the
// saves as files and a job's progress between its runs (the kernel's waits
// and the thread's end: host/kernel.h). One boot's state (zx::session).
//
// The files, each the bytes of the memory card's data file (the save's byte
// sum as one word, then the save), so moving saves to and from PCSX2 cards
// stays a copy:
//   <saves>/<profile>.sav        the game save; <profile> = ZX_SAVE_PROFILE
//                                (the app's save profile), else "Career"
//   <saves>/Parks/Park NNN.sav   the saves the player picks from a list (the
//                                park editor's parks), NNN the number their
//                                name on the card ends in
// each written atomically with the one before kept as .bak. <saves> =
// ZX_SAVES_DIR (the app's Saves folder), else a sibling of the slot 1 card
// folder (<card>-saves). At a boot's first job the saves on the memory card
// folders (slot 1, then slot 2) are copied in once (<saves>/imported.txt
// records it); the card folders are left as they are.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class PS2Runtime;

namespace game::host::saves
{
    // What a game saves, where its save client's prepare puts the bytes for a
    // job, and what the game adds.
    struct Game
    {
        // The game save: the word holding its buffer's address (the client's
        // prepare makes the buffer), its bytes, and its folder's name on the
        // memory card.
        uint32_t gameSaveBuffer = 0;
        uint32_t gameSaveBytes = 0;
        const char *gameSaveCardFolder = "";
        // The saves picked from a list (the parks): the word holding the park
        // buffer's address ({bytes, data, client}), a park's bytes, and the
        // name of a park's folder on the card before its three digits, which
        // is also the name the list shows and the player's choice comes back
        // with.
        uint32_t parkBuffer = 0;
        uint32_t parkBytes = 0;
        const char *parkCardFolderPrefix = "";
        // Before a game save is written (a copy of the buffer): what the game's
        // own buffer leaves out (AI: the Ape's career, the extra levels'
        // filters). Null: the buffer as it is.
        void (*completeSave)(const uint8_t *rdram, std::vector<uint8_t> &save) = nullptr;
        // ZX_TRACE_SAVES=1: the game save's buffer checked before it is
        // written (AI: against its host serializer). Null: none.
        void (*traceSave)(const uint8_t *rdram, PS2Runtime *runtime, const uint8_t *save) = nullptr;
        // After a park's file was written (BMX XXX: whether it needs the other
        // releases' editor pieces, recorded beside it). Null: nothing.
        void (*parkSaved)(const std::filesystem::path &file) = nullptr;
    };
    void configure(const Game &game);
    const Game &game();

    // ---- the files ---------------------------------------------------------

    // Where the saves go, the profile and its game save's file.
    std::filesystem::path folder();
    std::string profile();
    std::filesystem::path saveFile();

    // The game save's file into guest `destination` (gameSaveBytes); the
    // backup when the file is missing or damaged. At the boot, a save found
    // or none lets the autosave write (saveSeen), a damaged one does not;
    // from the menus, a save loaded does.
    enum class Loaded
    {
        Loaded,
        FromBackup,
        NoSave,  // no file at all
        Damaged, // a file, but neither it nor its backup is a save (or no buffer to take it)
    };
    Loaded loadGameSaveAtBoot(uint32_t destination);
    Loaded loadGameSave(uint32_t destination);
    inline bool loaded(Loaded how) { return how == Loaded::Loaded || how == Loaded::FromBackup; }
    // The game save's bytes at guest `buffer` (completed by the game's
    // completeSave) written to its file, which lets the autosave write; false
    // when they could not be. Why: the log's.
    bool writeGameSave(uint32_t buffer, const char *why);
    // A game save's bytes (gameSaveBytes, already complete) written to its file.
    bool writeSave(const uint8_t *save, const char *why);

    // The parks on disk, by number (only files of a park's size; the byte sum
    // is checked when one is loaded), and the first free number (kNoRoom when
    // every number is taken).
    constexpr int kParkNumbers = 1000; // three digits
    constexpr int kNoRoom = kParkNumbers;
    std::vector<int> parks();
    int firstFreePark();
    // A park's name as the card has its folder (the prefix and three digits),
    // and the number in one (-1: not a park's name).
    std::string parkName(int number);
    int parkNumber(const std::string &name);
    // The first `bytes` of park `number`'s save into guest `destination`
    // (what the list shows of it); false when it could not be read.
    bool readParkHeader(int number, uint32_t destination, uint32_t bytes);
    // The park the player chose by its name, or park `number`, from or to
    // guest memory (parkBytes); a load takes the backup when the file is
    // damaged.
    bool loadPark(const std::string &name, uint32_t destination);
    bool writePark(int number, uint32_t source);

    // A game save was loaded or saved this run (or the boot's load found
    // none): only then may a game's own autosave write. A damaged save is
    // kept from it: only a save the player asks for replaces it.
    bool saveSeen();
    void setSaveSeen(bool seen);
    // The game save is what was last loaded or written, in its first `used`
    // bytes.
    bool sameAsLast(const std::vector<uint8_t> &save, size_t used);

    // The card folders' saves copied in, at a boot's first job.
    void importFromCards();

    // ---- a job between its runs -------------------------------------------

    // A host function cannot block in the middle (a blocked guest thread
    // resumes at its saved pc, not inside host code), so a job that waits
    // (a frame, a time a message stays up, the player's pick from the list)
    // sleeps and runs again from its entry when woken: this is where it got
    // to.
    struct Job
    {
        bool running = false;
        int thread = -1;
        int kind = 0;
        int step = 0;
        uint64_t until = 0;  // a vsync before which it only sleeps (0: none)
        int32_t result = 0;  // how it went, once it knows
        uint32_t pendingMessage = 0; // the last list entry posted, until it is taken
        std::vector<int> parks;
        size_t next = 0;
        bool answered = false;
        int32_t choice = 0;
        std::string chosenName;
        // DM2's frame: frames until the request is told the frame is over (0:
        // the game's own frame runs).
        int framesToFinish = 0;
    };
    Job &job();
    bool jobRunning();

    // The boot's quiet load opened no save screen (the game's own host form
    // of its screen); `undo` takes back what the client's prepare marked as
    // open, once the load is done.
    void bootScreenSkipped(void (*undo)());
    void bootLoadDone();

    // ZX_TRACE_SAVES=1.
    bool tracing();

    // ---- a game save that is the card's file as it is (DM2) ----------------

    // DM2's game save is one file, BMX2SAVE.DAT's bytes (no byte sum in
    // front): <saves>/<profile>.sav, written atomically with a .bak.
    struct WholeSave
    {
        uint32_t bytes = 0;
        // Whether a file's bytes are a save the game can load.
        bool (*valid)(const std::vector<uint8_t> &file) = nullptr;
        // Before the first load of a boot: the saves found elsewhere brought
        // in (DM2: the card folders' BMX2SAVE.DAT, the saves of before
        // profiles). Null: nothing.
        void (*beforeFirstLoad)() = nullptr;
        // The log's prefix, and whether each save and load is logged.
        const char *log = "[saves]";
        bool trace = false;
    };
    void configureWholeSave(const WholeSave &save);
    // The file written (its bytes, `bytes` of them); false (and logged) when
    // it could not be.
    bool writeWholeSave(const std::vector<uint8_t> &file);
    // The file (or its backup) read and checked; false when there is no save
    // to load.
    bool loadWholeSave(std::vector<uint8_t> &file);

    // ---- a park from a host file (DM2's Custom Freeplay) -------------------

    // ZX_DM2_PARK_FILE's file (the collection's Custom Freeplay gives its
    // map's park.dat; a file in a folder, or in a zip read in place):
    // DM2's park loads read it instead of the card. Empty: the card's.
    void configureHostPark(const std::string &file);
    bool hostPark();
    // The file's first park.size() bytes into `park`, the rest zero; false
    // (and logged) when it could not be read.
    bool loadHostPark(std::vector<uint8_t> &park);
}
