#include "MiniTest.h"
#include "runtime/ps2_save_store.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zx::saves;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("zx-saves-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    Bytes bytes(const std::string &text)
    {
        return Bytes(text.begin(), text.end());
    }

    void put(const std::filesystem::path &path, const Bytes &data)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    Bytes get(const std::filesystem::path &path)
    {
        Bytes data;
        readFile(path, data);
        return data;
    }

    // A save as games store it: u32 byte sum of the data, then the data.
    Bytes save(const std::string &text)
    {
        const Bytes data = bytes(text);
        const uint32_t sum = byteSum(data.data(), data.size());
        Bytes out(4);
        std::memcpy(out.data(), &sum, 4);
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

    bool validSave(const Bytes &file)
    {
        if (file.size() < 4)
            return false;
        uint32_t sum = 0;
        std::memcpy(&sum, file.data(), 4);
        return byteSum(file.data() + 4, file.size() - 4) == sum;
    }
}

void register_ps2_save_store_tests()
{
    MiniTest::Case("SaveStore", [](TestCase &tc)
    {
        tc.Run("byte sum and file names", [](TestCase &t)
        {
            const Bytes data = {1, 2, 0xFF, 0xFF};
            t.Equals(byteSum(data.data(), data.size()), 0x201u, "sum of bytes");
            t.Equals(safeFileName("Alex", "Career"), std::string("Alex"), "kept");
            t.Equals(safeFileName("../a/b:c", "Career"), std::string("_a_b_c"), "no folders or odd characters");
            t.Equals(safeFileName(" .. ", "Career"), std::string("Career"), "nothing left");
            t.Equals(backupPath("x/Career.sav").string(), std::filesystem::path("x/Career.sav.bak").string(), "backup name");
        });

        tc.Run("atomic write keeps the previous version as .bak", [](TestCase &t)
        {
            TempDir dir;
            const auto file = dir.path / "Saves" / "Career.sav";
            std::string error;
            t.IsTrue(writeFileAtomic(file, bytes("one").data(), 3, &error), "first write creates the folder: " + error);
            t.IsTrue(get(file) == bytes("one"), "written");
            t.IsFalse(std::filesystem::exists(backupPath(file)), "no backup of nothing");
            t.IsTrue(writeFileAtomic(file, bytes("two!").data(), 4, &error), "second write: " + error);
            t.IsTrue(get(file) == bytes("two!"), "replaced");
            t.IsTrue(get(backupPath(file)) == bytes("one"), "previous kept");
            t.IsTrue(writeFileAtomic(file, bytes("3").data(), 1, &error), "third write: " + error);
            t.IsTrue(get(backupPath(file)) == bytes("two!"), "only the last previous version is kept");
            t.IsFalse(std::filesystem::exists(tempPath(file)), "no temporary file left");

            put(tempPath(file), bytes("stale half write"));
            t.IsTrue(writeFileAtomic(file, bytes("4").data(), 1, &error), "a stale temporary file is replaced");
            t.IsTrue(get(file) == bytes("4"), "written over it");

            const auto blocked = dir.path / "file-not-folder";
            put(blocked, bytes("x"));
            t.IsFalse(writeFileAtomic(blocked / "Career.sav", bytes("y").data(), 1, &error), "unwritable folder fails");
            t.IsFalse(error.empty(), "with a reason");
        });

        tc.Run("read falls back to the backup", [](TestCase &t)
        {
            TempDir dir;
            const auto file = dir.path / "Career.sav";
            t.IsTrue(readWithBackup(file, validSave).status == ReadStatus::Missing, "nothing there");

            put(file, save("good"));
            ReadResult r = readWithBackup(file, validSave);
            t.IsTrue(r.status == ReadStatus::Ok && r.data == save("good"), "the file");

            put(backupPath(file), save("older"));
            Bytes damaged = save("newer");
            damaged[5] ^= 0x40;
            put(file, damaged);
            r = readWithBackup(file, validSave);
            t.IsTrue(r.status == ReadStatus::FromBackup && r.data == save("older"), "damaged file: the backup");

            std::filesystem::remove(file);
            r = readWithBackup(file, validSave);
            t.IsTrue(r.status == ReadStatus::FromBackup, "interrupted write (file renamed away): the backup");

            put(backupPath(file), damaged);
            r = readWithBackup(file, validSave);
            t.IsTrue(r.status == ReadStatus::Invalid && !r.ok(), "nothing valid");
            t.IsTrue(readWithBackup(file).ok(), "no validator accepts anything");
        });

        tc.Run("import from memory card folders", [](TestCase &t)
        {
            TempDir dir;
            const auto slot1 = dir.path / "memcards" / "slot1";
            const auto slot2 = dir.path / "memcards" / "slot2";
            put(slot1 / "BASLUS-20327CEAIGAME" / "BASLUS-20327CEAIGAME", save("career one"));
            put(slot1 / "BASLUS-20327CEAIGAME" / "icon.sys", bytes("icon"));
            put(slot1 / "BASLUS-20327IPARK000" / "BASLUS-20327IPARK000", save("park a"));
            put(slot2 / "BASLUS-20327CEAIGAME" / "BASLUS-20327CEAIGAME", save("career two"));
            put(slot2 / "BASLUS-20327IPARK000" / "BASLUS-20327IPARK000", save("park b"));
            put(slot2 / "BASLUS-20327IPARK001" / "BASLUS-20327IPARK001", save("park a")); // same park again
            Bytes broken = save("park c");
            broken[0] ^= 1;
            put(slot2 / "BASLUS-20327IPARK002" / "BASLUS-20327IPARK002", broken);
            put(slot2 / "OTHERGAME" / "OTHERGAME", save("not ours"));

            const auto saves = dir.path / "Saves";
            put(saves / "Existing.sav", bytes("keep me"));
            int parks = 0;
            auto target = [&](const std::string &name) -> std::filesystem::path
            {
                if (name == "BASLUS-20327CEAIGAME")
                    return saves / "Career.sav";
                if (name.rfind("BASLUS-20327IPARK", 0) == 0)
                    return saves / "Parks" / ("Park " + std::to_string(parks++) + ".sav");
                if (name == "OTHERGAME")
                    return saves / "Existing.sav";
                return {};
            };
            const Bytes slot1Before = get(slot1 / "BASLUS-20327CEAIGAME" / "BASLUS-20327CEAIGAME");
            std::string error;
            const auto imported = importFromCardFolders({slot1, slot2, dir.path / "missing"}, target, validSave, &error);
            t.IsTrue(error.empty(), "no error: " + error);
            t.IsTrue(get(saves / "Career.sav") == save("career one"), "slot 1 first, the second career is not taken");
            t.IsTrue(get(saves / "Parks" / "Park 0.sav") == save("park a"), "park a");
            t.IsTrue(get(saves / "Parks" / "Park 1.sav") == save("park b"), "park b");
            t.IsFalse(std::filesystem::exists(saves / "Parks" / "Park 2.sav"), "a duplicate and a damaged park skipped");
            t.IsTrue(get(saves / "Existing.sav") == bytes("keep me"), "an existing file is never replaced");
            t.Equals(imported.size(), static_cast<size_t>(3u), "three imported");
            t.IsTrue(get(slot1 / "BASLUS-20327CEAIGAME" / "BASLUS-20327CEAIGAME") == slot1Before, "the card is left as it was");
            t.IsTrue(std::filesystem::exists(slot2 / "BASLUS-20327IPARK000" / "BASLUS-20327IPARK000"), "slot 2 too");
        });
    });
}
