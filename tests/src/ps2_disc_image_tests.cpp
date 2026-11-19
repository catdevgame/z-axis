#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Stubs/CD.h"
#include "ps2x/iop/iso9660.h"
#include "runtime/ps2_vfs.h"
#include "test_iso_builder.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("ps2x-disc-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::vector<test_iso::File> sampleFiles()
    {
        std::vector<uint8_t> big(5000u);
        for (size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<uint8_t>(i * 7u);
        return {
            {"SYSTEM.CNF", test_iso::bytesOf("BOOT2 = cdrom0:\\SLUS_203.27;1\r\nVER = 1.00\r\n")},
            {"ASSETS/LEGAL1.RAW", big},
            {"ASSETS/APPDISCTEST.BIN", test_iso::bytesOf("disc-image-file")},
        };
    }

    void writeGuestString(std::vector<uint8_t> &rdram, uint32_t address, const std::string &text)
    {
        std::memcpy(rdram.data() + address, text.c_str(), text.size() + 1u);
    }

    uint32_t readGuestU32(const std::vector<uint8_t> &rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram.data() + address, sizeof(value));
        return value;
    }
}

void register_ps2_disc_image_tests()
{
    MiniTest::Case("DiscImage", [](TestCase &tc)
    {
        tc.Run("Iso9660Image finds files case-insensitively with device and version", [](TestCase &t)
        {
            TempDir dir;
            const auto iso = dir.path / "disc.iso";
            const auto lsns = test_iso::write(iso, sampleFiles(), "AGGRESSIVE_INLINE");

            ps2x::iop::Iso9660Image image;
            std::string error;
            t.IsTrue(image.open(iso, &error), "open should accept the image: " + error);
            t.Equals(image.volumeId(), std::string("AGGRESSIVE_INLINE"), "volume id is read from the PVD");

            const auto cnf = image.find("cdrom0:\\system.cnf;1");
            t.IsTrue(cnf.has_value(), "cdrom0:\\system.cnf;1 should resolve");
            if (cnf)
            {
                t.Equals(cnf->lsn, lsns.at("SYSTEM.CNF"), "LSN comes from the directory record");
                t.IsFalse(cnf->directory, "SYSTEM.CNF is a file");
            }
            const auto legal = image.find("\\Assets\\legal1.raw");
            t.IsTrue(legal.has_value() && legal->size == 5000u, "nested file with backslashes and mixed case");
            const auto assets = image.find("ASSETS");
            t.IsTrue(assets.has_value() && assets->directory, "directories resolve");
            t.IsFalse(image.find("ASSETS/MISSING.BIN").has_value(), "missing files do not resolve");
            t.IsFalse(image.find("SYSTEM.CNF/X").has_value(), "a file is not a directory");
            t.IsFalse(image.find("../SYSTEM.CNF").has_value(), "no parent traversal");
            t.Equals(image.list("ASSETS").size(), static_cast<size_t>(2u), "list returns the directory's entries");

            std::vector<uint8_t> bytes;
            t.IsTrue(legal && image.readFile(*legal, bytes), "readFile reads the extent");
            t.IsTrue(bytes.size() == 5000u && bytes[4999] == static_cast<uint8_t>(4999u * 7u), "file contents match across sectors");
        });

        tc.Run("Iso9660Image rejects files that are not disc images", [](TestCase &t)
        {
            TempDir dir;
            const auto small = dir.path / "small.iso";
            {
                std::ofstream out(small, std::ios::binary);
                out << "not a disc";
            }
            ps2x::iop::Iso9660Image image;
            std::string error;
            t.IsFalse(image.open(small, &error), "a tiny file is not an image");
            t.IsFalse(error.empty(), "an error message is given");

            const auto zeros = dir.path / "zeros.iso";
            {
                std::ofstream out(zeros, std::ios::binary);
                const std::vector<char> blank(20u * 2048u, 0);
                out.write(blank.data(), static_cast<std::streamsize>(blank.size()));
            }
            t.IsFalse(image.open(zeros, &error), "no CD001 descriptor");
            t.IsFalse(image.open(dir.path / "missing.iso", &error), "missing file");
        });

        tc.Run("VFS opens cdrom0: files inside the disc image", [](TestCase &t)
        {
            TempDir dir;
            const auto iso = dir.path / "disc.iso";
            test_iso::write(iso, sampleFiles());

            PS2Vfs vfs;
            PS2RomDevice rom;
            PS2VfsMounts mounts;
            mounts.cdImage = ps2x::iop::Iso9660Image::shared(iso);
            t.IsTrue(mounts.cdImage != nullptr, "shared image opens");

            const int32_t fd = vfs.open("cdrom0:\\ASSETS\\APPDISCTEST.BIN;1", PS2_FIO_O_RDONLY, mounts, rom);
            t.IsTrue(fd >= 0, "open succeeds");
            char buffer[32] = {};
            t.Equals(vfs.read(fd, buffer, sizeof(buffer)), static_cast<int64_t>(15), "read stops at the file size");
            t.Equals(std::string(buffer), std::string("disc-image-file"), "file contents");
            t.Equals(vfs.seek(fd, 5, SEEK_SET), static_cast<int64_t>(5), "seek");
            t.Equals(vfs.read(fd, buffer, 5), static_cast<int64_t>(5), "read after seek");
            t.IsTrue(std::memcmp(buffer, "image", 5) == 0, "read after seek returns the right bytes");
            t.Equals(vfs.close(fd), 0, "close");

            t.IsTrue(vfs.open("cdrom0:\\ASSETS\\APPDISCTEST.BIN;1", PS2_FIO_O_WRONLY, mounts, rom) < 0, "no writes to the disc");
            t.IsTrue(vfs.open("cdrom0:\\NOPE.BIN", PS2_FIO_O_RDONLY, mounts, rom) < 0, "missing file");

            PS2VfsStat stat;
            t.IsTrue(vfs.stat("cdrom0:\\ASSETS", mounts, rom, stat) && stat.directory, "stat of a directory");
            t.IsTrue(vfs.stat("cdrom0:\\ASSETS\\LEGAL1.RAW", mounts, rom, stat) && stat.size == 5000u && stat.readOnly,
                     "stat of a file");
            std::filesystem::path hostPath;
            t.IsFalse(vfs.resolveHostPath("cdrom0:\\SYSTEM.CNF", mounts, hostPath), "no host path inside an image");
        });

        tc.Run("sceCdSearchFile returns the image's own LSN with cdFromImage", [](TestCase &t)
        {
            TempDir dir;
            const auto iso = dir.path / "disc.iso";
            const auto lsns = test_iso::write(iso, sampleFiles());

            const PS2Runtime::IoPaths oldPaths = PS2Runtime::getIoPaths();
            PS2Runtime::IoPaths paths;
            paths.elfDirectory = dir.path;
            paths.hostRoot = dir.path;
            paths.mcRoot = dir.path / "mc0";
            paths.cdImage = iso;
            paths.cdFromImage = true;
            PS2Runtime::setIoPaths(paths);
            t.IsTrue(PS2Runtime::getIoPaths().cdRoot.empty(), "cdFromImage leaves the CD root empty");

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            constexpr uint32_t fileAddr = 0x3000u;
            constexpr uint32_t pathAddr = 0x1000u;
            writeGuestString(rdram, pathAddr, "\\ASSETS\\APPDISCTEST.BIN;1");
            SET_GPR_U32(&ctx, 4, fileAddr);
            SET_GPR_U32(&ctx, 5, pathAddr);
            ps2_stubs::sceCdSearchFile(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<int32_t>(GPR_U32((&ctx), 2)), 1, "found");
            t.Equals(readGuestU32(rdram, fileAddr), lsns.at("ASSETS/APPDISCTEST.BIN"), "real LSN");
            t.Equals(readGuestU32(rdram, fileAddr + 4u), 15u, "size");

            // The LSN reads the file's bytes from the image.
            std::memset(&ctx, 0, sizeof(ctx));
            SET_GPR_U32(&ctx, 4, lsns.at("ASSETS/APPDISCTEST.BIN"));
            SET_GPR_U32(&ctx, 5, 1u);
            SET_GPR_U32(&ctx, 6, 0x8000u);
            ps2_stubs::sceCdRead(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::memcmp(rdram.data() + 0x8000u, "disc-image-file", 15) == 0, "sceCdRead at that LSN");

            PS2Runtime::setIoPaths(oldPaths);
        });

        tc.Run("Memory card slot 2 sits next to a slot1 folder", [](TestCase &t)
        {
            const PS2Runtime::IoPaths oldPaths = PS2Runtime::getIoPaths();
            PS2Runtime::IoPaths paths = oldPaths;
            // An absolute path on every platform (a drive letter on Windows).
            const std::filesystem::path cards =
                (std::filesystem::temp_directory_path() / "ps2x-cards" / "memcards").lexically_normal();
            paths.mcRoot = cards / "slot1";
            PS2Runtime::setIoPaths(paths);
            const auto snapshot = ps2_stubs::getMemoryCardDebugSnapshot();
            t.Equals(snapshot.ports[0].rootPath, cards / "slot1", "slot 1");
            t.Equals(snapshot.ports[1].rootPath, cards / "slot2", "slot 2");
            PS2Runtime::setIoPaths(oldPaths);
        });
    });
}
