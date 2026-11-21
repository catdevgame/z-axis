// The host's services to the engine's save jobs (save_files.h).

#include "save_files.h"
#include "ee/host_form.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/game_files.h"
#include "runtime/save_store.h"
#include "runtime/session.h"
#include "Stubs/MemoryCard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace game::host::saves
{
    namespace
    {
        namespace store = zx::saves;
        using store::Bytes;

        struct Saves
        {
            Game game{};
            Job job;
            bool imported = false;
            bool saveSeen = false;
            Bytes lastSave; // the game save last loaded or written
            void (*undoBootScreen)() = nullptr;
            WholeSave whole{};
            bool wholeImported = false;
            std::vector<std::string> slotLibrariesImported; // their folders, this boot

            std::string hostPark; // configureHostPark's file
        };
        Saves &saves() { return zx::session::state<Saves>(); }

        // The profile a boot plays when the app names none, and the one the
        // memory cards' game save is brought into.
        constexpr const char *kDefaultProfile = "Career";
        const Game &g() { return saves().game; }

        // ---- guest memory --------------------------------------------------

        // A guest range as one host span, or null.
        uint8_t *span(uint32_t address, uint32_t size)
        {
            uint8_t *rdram = currentCall().rdram;
            uint8_t *p = getMemPtr(rdram, address);
            if (!p || size == 0)
                return p;
            const uint8_t *last = getConstMemPtr(rdram, address + size - 1u);
            return last == p + size - 1u ? p : nullptr;
        }

        // ---- names and files -----------------------------------------------

        std::string text(const char *name, const char *fallback)
        {
            const char *value = zx::boot::text(name);
            return value && value[0] ? value : fallback;
        }

        std::filesystem::path savesRoot()
        {
            if (const char *dir = zx::boot::text("ZX_SAVES_DIR"); dir && dir[0])
                return dir;
            const std::filesystem::path card = ps2_stubs::memoryCardFolder(0);
            return card.parent_path() / (card.filename().string() + "-saves");
        }

        std::filesystem::path parkFile(int number)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "Park %03d.sav", number);
            return savesRoot() / "Parks" / name;
        }

        // "Park nnn.sav" -> nnn, else -1.
        int parkNumberOfFile(const std::string &file)
        {
            if (file.size() != 12u || file.compare(0, 5, "Park ") != 0 || file.compare(8, 4, ".sav") != 0)
                return -1;
            int number = 0;
            for (size_t i = 5; i < 8; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(file[i])))
                    return -1;
                number = number * 10 + (file[i] - '0');
            }
            return number;
        }

        // The card's data file: the byte sum of the save, then the save.
        bool validSave(const Bytes &file, uint32_t bytes)
        {
            if (file.size() != 4u + bytes)
                return false;
            uint32_t sum = 0;
            std::memcpy(&sum, file.data(), 4);
            return store::byteSum(file.data() + 4, bytes) == sum;
        }

        Bytes withByteSum(const uint8_t *save, uint32_t bytes)
        {
            Bytes file(4u + bytes);
            const uint32_t sum = store::byteSum(save, bytes);
            std::memcpy(file.data(), &sum, 4);
            if (bytes)
                std::memcpy(file.data() + 4, save, bytes);
            return file;
        }

        store::ReadResult readGameSave()
        {
            return store::readWithBackup(saveFile(), [](const Bytes &d) { return validSave(d, g().gameSaveBytes); });
        }

        // The file into guest memory (0: no buffer); the save then is the last one seen.
        Loaded copyIn(const store::ReadResult &read, uint32_t destination)
        {
            uint8_t *out = destination ? span(destination, g().gameSaveBytes) : nullptr;
            if (read.ok() && out)
            {
                std::memcpy(out, read.data.data() + 4, g().gameSaveBytes);
                saves().lastSave.assign(read.data.begin() + 4, read.data.end());
                return read.status == store::ReadStatus::FromBackup ? Loaded::FromBackup : Loaded::Loaded;
            }
            return read.status == store::ReadStatus::Missing ? Loaded::NoSave : Loaded::Damaged;
        }
    }

    void configure(const Game &game) { saves().game = game; }
    const Game &game() { return g(); }

    std::filesystem::path folder() { return savesRoot(); }

    std::string profile() { return store::safeFileName(text("ZX_SAVE_PROFILE", kDefaultProfile), kDefaultProfile); }

    std::filesystem::path saveFile() { return savesRoot() / (profile() + ".sav"); }

    Loaded loadGameSaveAtBoot(uint32_t destination)
    {
        const store::ReadResult read = readGameSave();
        const Loaded loaded = copyIn(read, destination);
        saves().saveSeen = loaded != Loaded::Damaged;
        std::cout << "[saves] boot load " << saveFile().string() << ": "
                  << (loaded == Loaded::Loaded       ? "loaded"
                      : loaded == Loaded::FromBackup ? "loaded the backup"
                      : loaded == Loaded::NoSave     ? "no save yet"
                                                     : "damaged, not loaded")
                  << std::endl;
        return loaded;
    }

    Loaded loadGameSave(uint32_t destination)
    {
        const store::ReadResult read = readGameSave();
        const Loaded loaded = copyIn(read, destination);
        if (loaded == Loaded::Loaded || loaded == Loaded::FromBackup)
        {
            saves().saveSeen = true;
            std::cout << "[saves] load: read " << saveFile().string() << std::endl;
        }
        else if (loaded == Loaded::Damaged)
            std::cerr << "[saves] load: " << saveFile().string() << " is damaged" << std::endl;
        return loaded;
    }

    bool writeSave(const uint8_t *save, const char *why)
    {
        const Bytes file = withByteSum(save, g().gameSaveBytes);
        std::string error;
        const std::filesystem::path path = saveFile();
        if (!store::writeFileAtomic(path, file.data(), file.size(), &error))
        {
            std::cerr << "[saves] " << why << ": " << error << std::endl;
            return false;
        }
        saves().lastSave.assign(save, save + g().gameSaveBytes);
        std::cout << "[saves] " << why << ": wrote " << path.string() << std::endl;
        return true;
    }

    bool writeGameSave(uint32_t buffer, const char *why)
    {
        const Call &call = currentCall();
        const uint8_t *save = buffer ? span(buffer, g().gameSaveBytes) : nullptr;
        if (tracing() && g().traceSave)
            g().traceSave(call.rdram, call.runtime, save);
        // What the game's buffer leaves out (AI: the Ape's career, the filters).
        Bytes completed;
        if (save && g().completeSave)
        {
            completed.assign(save, save + g().gameSaveBytes);
            g().completeSave(call.rdram, completed);
            save = completed.data();
        }
        const bool written = save && writeSave(save, why);
        if (written)
            saves().saveSeen = true;
        return written;
    }

    std::vector<int> parks()
    {
        std::vector<int> numbers;
        std::error_code ec;
        const std::filesystem::path dir = savesRoot() / "Parks";
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            const int number = parkNumberOfFile(entry.path().filename().string());
            if (number >= 0 && entry.is_regular_file(ec) && entry.file_size(ec) == 4u + g().parkBytes)
                numbers.push_back(number);
        }
        std::sort(numbers.begin(), numbers.end());
        return numbers;
    }

    int firstFreePark()
    {
        std::error_code ec;
        int number = 0;
        while (number < kParkNumbers && std::filesystem::exists(parkFile(number), ec))
            ++number;
        return number;
    }

    std::string parkName(int number)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%s%03d", g().parkCardFolderPrefix, number);
        return name;
    }

    int parkNumber(const std::string &name)
    {
        const size_t prefix = std::strlen(g().parkCardFolderPrefix);
        if (name.size() != prefix + 3u || name.compare(0, prefix, g().parkCardFolderPrefix) != 0)
            return -1;
        int number = 0;
        for (size_t i = prefix; i < name.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return -1;
            number = number * 10 + (name[i] - '0');
        }
        return number;
    }

    bool readParkHeader(int number, uint32_t destination, uint32_t bytes)
    {
        uint8_t *out = destination ? span(destination, bytes) : nullptr;
        Bytes file;
        if (!out || !store::readFile(parkFile(number), file) || file.size() != 4u + g().parkBytes || bytes > g().parkBytes)
            return false;
        std::memcpy(out, file.data() + 4, bytes);
        return true;
    }

    bool loadPark(const std::string &name, uint32_t destination)
    {
        const int number = parkNumber(name);
        uint8_t *out = destination ? span(destination, g().parkBytes) : nullptr;
        const store::ReadResult read =
            number >= 0 ? store::readWithBackup(parkFile(number), [](const Bytes &d) { return validSave(d, g().parkBytes); })
                        : store::ReadResult{};
        if (!read.ok() || !out)
        {
            std::cerr << "[saves] park load: '" << name << "' missing or damaged" << std::endl;
            return false;
        }
        std::memcpy(out, read.data.data() + 4, g().parkBytes);
        std::cout << "[saves] park load: read " << parkFile(number).string() << std::endl;
        return true;
    }

    bool writePark(int number, uint32_t source)
    {
        const uint8_t *park = source ? span(source, g().parkBytes) : nullptr;
        if (!park || number < 0 || number >= kParkNumbers)
            return false;
        const Bytes file = withByteSum(park, g().parkBytes);
        std::string error;
        if (!store::writeFileAtomic(parkFile(number), file.data(), file.size(), &error))
        {
            std::cerr << "[saves] park save: " << error << std::endl;
            return false;
        }
        std::cout << "[saves] park save: wrote " << parkFile(number).string() << std::endl;
        if (g().parkSaved)
            g().parkSaved(parkFile(number));
        return true;
    }

    bool saveSeen() { return saves().saveSeen; }
    void setSaveSeen(bool seen) { saves().saveSeen = seen; }

    bool sameAsLast(const std::vector<uint8_t> &save, size_t used)
    {
        const Bytes &last = saves().lastSave;
        return last.size() == save.size() && used <= save.size() && std::equal(save.begin(), save.begin() + used, last.begin());
    }

    void importFromCards()
    {
        if (saves().imported)
            return;
        saves().imported = true;
        const std::filesystem::path root = savesRoot();
        const std::filesystem::path marker = root / "imported.txt";
        std::error_code ec;
        if (std::filesystem::exists(marker, ec))
            return;
        std::vector<std::filesystem::path> cards = {ps2_stubs::memoryCardFolder(0), ps2_stubs::memoryCardFolder(1)};
        // (the cards' game save is the Career profile's, whichever profile this boot plays: a profile made
        // beside it starts empty)
        const auto target = [root](const std::string &name) -> std::filesystem::path
        {
            if (name == g().gameSaveCardFolder)
                return root / (std::string(kDefaultProfile) + ".sav");
            const int number = parkNumber(name);
            if (number < 0)
                return {};
            std::error_code ec;
            for (int n = number, tries = 0; tries < kParkNumbers; n = (n + 1) % kParkNumbers, ++tries)
                if (!std::filesystem::exists(parkFile(n), ec))
                    return parkFile(n);
            return {};
        };
        const auto valid = [](const Bytes &data) { return validSave(data, g().gameSaveBytes) || validSave(data, g().parkBytes); };
        std::string error;
        const std::vector<store::ImportedSave> imported = store::importFromCardFolders(cards, target, valid, &error);
        std::filesystem::create_directories(root, ec);
        std::ofstream log(marker);
        log << "Saves imported from the memory card folders (they were left as they are):\n";
        for (const store::ImportedSave &item : imported)
        {
            log << item.from.string() << " -> " << item.to.filename().string() << "\n";
            std::cout << "[saves] imported " << item.from.string() << " -> " << item.to.string() << std::endl;
        }
        if (imported.empty())
            log << "(none)\n";
        if (!error.empty())
            std::cerr << "[saves] import: " << error << std::endl;
    }

    Job &job() { return saves().job; }
    bool jobRunning() { return saves().job.running; }

    void bootScreenSkipped(void (*undo)()) { saves().undoBootScreen = undo; }

    void bootLoadDone()
    {
        if (void (*undo)() = std::exchange(saves().undoBootScreen, nullptr))
            undo();
    }

    bool tracing()
    {
        static const bool on = []
        {
            const char *v = std::getenv("ZX_TRACE_SAVES");
            return v && v[0] == '1';
        }();
        return on;
    }

    void configureWholeSave(const WholeSave &save) { saves().whole = save; }

    bool writeWholeSave(const std::vector<uint8_t> &file)
    {
        const WholeSave &whole = saves().whole;
        std::string error;
        const bool written = store::writeFileAtomic(saveFile(), file.data(), file.size(), &error);
        if (!written)
            std::cerr << whole.log << ": could not write " << saveFile().string() << ": " << error << std::endl;
        else if (whole.trace)
            std::cout << whole.log << ": game saved to " << saveFile().string() << std::endl;
        return written;
    }

    bool loadWholeSave(std::vector<uint8_t> &file)
    {
        WholeSave &whole = saves().whole;
        if (!saves().wholeImported)
        {
            saves().wholeImported = true;
            if (whole.beforeFirstLoad)
                whole.beforeFirstLoad();
        }
        const store::ReadResult read = store::readWithBackup(saveFile(), whole.valid);
        if (!read.ok())
        {
            if (whole.trace)
                std::cout << whole.log << ": no game save in " << folder().string() << std::endl;
            return false;
        }
        file = read.data;
        if (whole.trace)
            std::cout << whole.log << ": game loaded" << (read.status == store::ReadStatus::FromBackup ? " (backup)" : "")
                      << std::endl;
        return true;
    }

    namespace
    {
        uint32_t slotFileBytes(const SlotLibrary &library) { return library.recordBytes + library.dataBytes; }

        // A slot's file as the game can take it: a used slot's record and data.
        bool validSlot(const SlotLibrary &library, const Bytes &file)
        {
            return file.size() == slotFileBytes(library) && library.used && library.used(file.data());
        }

        // A slot with no file (nor a backup of one).
        bool slotFree(const SlotLibrary &library, uint32_t slot)
        {
            std::error_code ec;
            const std::filesystem::path file = slotFile(library, slot);
            return !std::filesystem::exists(file, ec) && !std::filesystem::exists(store::backupPath(file), ec);
        }

        // A library file's mark in imported.txt: its bytes' FNV-1a hash, 64 bits.
        std::string libraryMark(const Bytes &bytes)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (const uint8_t byte : bytes)
                hash = (hash ^ byte) * 0x100000001B3ull;
            char text[32];
            std::snprintf(text, sizeof(text), "(library %016llx)", static_cast<unsigned long long>(hash));
            return text;
        }

        // The libraries on the card folders brought in, at the first read of
        // a boot: slot 1's card, slot 2's, and the PCSX2 import's copy of a
        // card (<saves>/PCSX2 card: zx::saves::kPcsx2CardFolder). A library
        // whose bytes imported.txt names was brought in before and is passed
        // over; of another, every used slot whose bytes no slot file holds yet
        // goes to its own slot when that is free, else to the first free one,
        // and imported.txt names it. A slot file is never replaced, a card
        // never changed.
        void importSlotLibrary(const SlotLibrary &library)
        {
            std::vector<std::string> &done = saves().slotLibrariesImported;
            if (std::find(done.begin(), done.end(), library.folder) != done.end())
                return;
            done.push_back(library.folder);
            const std::filesystem::path marker = savesRoot() / library.folder / "imported.txt";
            Bytes markerBytes;
            store::readFile(marker, markerBytes);
            const std::string imported(markerBytes.begin(), markerBytes.end());
            const std::filesystem::path cards[] = {ps2_stubs::memoryCardFolder(0), ps2_stubs::memoryCardFolder(1),
                                                   savesRoot() / store::kPcsx2CardFolder};
            std::string lines;
            for (const std::filesystem::path &cardFolder : cards)
            {
                const std::filesystem::path from = cardFolder / library.cardFolder / library.cardFile;
                Bytes card;
                if (!store::readFile(from, card))
                    continue;
                const std::string mark = libraryMark(card);
                if (imported.find(mark) != std::string::npos)
                    continue;
                lines += from.string() + " " + mark + ":\n";
                if (card.size() != library.cardFileBytes)
                {
                    lines += "  not a library (" + std::to_string(card.size()) + " bytes), left\n";
                    continue;
                }
                for (uint32_t slot = 0; slot < library.slots; ++slot)
                {
                    const uint8_t *record = card.data() + slot * library.recordBytes;
                    if (!library.used(record))
                        continue;
                    Bytes file(record, record + library.recordBytes);
                    const uint8_t *data = card.data() + library.slots * library.recordBytes + slot * library.dataBytes;
                    file.insert(file.end(), data, data + library.dataBytes);
                    // (the same save on two cards, or already brought in: once)
                    bool held = false;
                    for (uint32_t other = 0; other < library.slots && !held; ++other)
                    {
                        Bytes there;
                        held = store::readFile(slotFile(library, other), there) && there == file;
                    }
                    const std::string what = "  slot " + std::to_string(slot + 1);
                    if (held)
                    {
                        lines += what + ": held already\n";
                        continue;
                    }
                    uint32_t to = slot;
                    for (uint32_t other = 0; !slotFree(library, to) && other < library.slots; ++other)
                        to = other;
                    if (!slotFree(library, to))
                        lines += what + ": no free slot, left on the card\n";
                    else if (writeSlot(library, to, file))
                        lines += what + " -> " + slotFile(library, to).filename().string() + "\n";
                    else
                        lines += what + ": could not be written, left on the card\n";
                }
            }
            if (lines.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(marker.parent_path(), ec);
            std::ofstream log(marker, std::ios::app);
            if (imported.empty())
                log << "Saves imported from the memory card folders (they were left as they are):\n";
            log << lines;
            std::cout << library.log << ": imported from the memory card folders:\n" << lines << std::flush;
        }
    }

    std::filesystem::path slotFile(const SlotLibrary &library, uint32_t slot)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%s %02u.sav", library.name, slot + 1u);
        return savesRoot() / library.folder / name;
    }

    bool readSlot(const SlotLibrary &library, uint32_t slot, std::vector<uint8_t> &file)
    {
        importSlotLibrary(library);
        if (slot >= library.slots)
            return false;
        const store::ReadResult read =
            store::readWithBackup(slotFile(library, slot), [&library](const Bytes &d) { return validSlot(library, d); });
        if (!read.ok())
        {
            if (read.status == store::ReadStatus::Invalid)
                std::cerr << library.log << ": " << slotFile(library, slot).string() << " is damaged" << std::endl;
            return false;
        }
        file = read.data;
        if (read.status == store::ReadStatus::FromBackup)
            std::cout << library.log << ": " << slotFile(library, slot).filename().string() << " read from its backup"
                      << std::endl;
        return true;
    }

    bool writeSlot(const SlotLibrary &library, uint32_t slot, const std::vector<uint8_t> &file)
    {
        std::string error;
        const std::filesystem::path path = slotFile(library, slot);
        if (slot >= library.slots || !validSlot(library, file))
            error = "not a used slot's bytes";
        else if (store::writeFileAtomic(path, file.data(), file.size(), &error))
        {
            std::cout << library.log << ": wrote " << path.string() << std::endl;
            return true;
        }
        std::cerr << library.log << ": could not write " << path.string() << ": " << error << std::endl;
        return false;
    }

    void configureHostPark(const std::string &file) { saves().hostPark = file; }
    bool hostPark() { return !saves().hostPark.empty(); }

    bool loadHostPark(std::vector<uint8_t> &park)
    {
        const std::string &file = saves().hostPark;
        const char *log = saves().whole.log;
        // (a file in a folder, or in a zip read in place: runtime/game_files.h)
        const std::filesystem::path path(file);
        Bytes data;
        if (!zx::game_files::readFileBelow(path.parent_path(), path.filename().string(), data) || data.empty())
        {
            std::cerr << log << ": no park in " << file << std::endl;
            return false;
        }
        const size_t n = std::min(data.size(), park.size());
        std::copy(data.begin(), data.begin() + n, park.begin());
        std::fill(park.begin() + n, park.end(), 0u);
        if (saves().whole.trace)
            std::cout << log << ": park loaded from " << file << " (" << n << " bytes)" << std::endl;
        return true;
    }
}
