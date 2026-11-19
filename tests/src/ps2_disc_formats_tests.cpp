// Disc images in each format players keep them in (ps2x/iop/disc_image_file.h,
// cue_sheet.h): a made-up disc written as an ISO, then as a CD's CHD (raw and
// cooked sectors), a DVD's CHD, CSO v1 and v2, ZSO and a multi-track BIN/CUE,
// each written here from its format's layout (no game data). Each opens by
// its bytes whatever its name, checks as the disc it is, and installs the
// same files, byte for byte, as the ISO does, from a path and from Android's
// documents.
#include "MiniTest.h"
#include "collection/disc_check.h"
#include "collection/disc_finder.h"
#include "collection/game_setup.h"
#include "ps2x/iop/cue_sheet.h"
#include "ps2x/iop/iso9660.h"
#include "test_iso_builder.h"

#include <lz4.h>
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace ps2x::app;
    using Bytes = std::vector<uint8_t>;

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("ps2x-discfmt-" + std::to_string(now));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    Bytes readAll(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(in), {});
    }

    void writeAll(const fs::path &file, const Bytes &bytes)
    {
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char *>(bytes.data()),
                                                                      static_cast<std::streamsize>(bytes.size()));
    }

    // The made-up disc: its boot ELF, a level file of patterns (blocks that
    // compress) and one of noise (blocks stored as they are).
    Bytes elfBytes()
    {
        Bytes elf(9000u);
        std::memcpy(elf.data(), "\x7F" "ELF", 4);
        for (size_t i = 4; i < elf.size(); ++i)
            elf[i] = static_cast<uint8_t>(i * 13u + 1u);
        return elf;
    }

    std::vector<test_iso::File> discFiles()
    {
        Bytes level(70000u), noise(12000u);
        for (size_t i = 0; i < level.size(); ++i)
            level[i] = static_cast<uint8_t>(i * 7u);
        uint32_t x = 2463534242u;
        for (uint8_t &b : noise)
        {
            x ^= x << 13u;
            x ^= x >> 17u;
            x ^= x << 5u;
            b = static_cast<uint8_t>(x >> 24u);
        }
        return {
            {"SYSTEM.CNF", test_iso::bytesOf("BOOT2 = cdrom0:\\SLUS_203.27;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n")},
            {"SLUS_203.27", elfBytes()},
            {"ASSETS/LEVEL.BIN", level},
            {"ASSETS/NOISE.BIN", noise},
        };
    }

    std::vector<SetupGame> games()
    {
        const Bytes elf = elfBytes();
        return {{"ai", "Aggressive Inline (USA)", "SLUS_203.27", sha256Hex(elf.data(), elf.size())}};
    }

    void putBe(Bytes &out, size_t at, uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            out[at + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
    void putLe(Bytes &out, size_t at, uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            out[at + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }

    // A raw CD sector (2352 bytes) of 2048 bytes of user data: Mode 1 or
    // Mode 2 Form 1 (sync, header, subheader; EDC and ECC left zero).
    Bytes rawSector(const uint8_t *data, size_t lba, int mode)
    {
        Bytes raw(2352u, 0u);
        static constexpr uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        std::memcpy(raw.data(), kSync, sizeof(kSync));
        const size_t frame = lba + 150u;
        raw[12] = static_cast<uint8_t>(frame / 75u / 60u);
        raw[13] = static_cast<uint8_t>(frame / 75u % 60u);
        raw[14] = static_cast<uint8_t>(frame % 75u);
        raw[15] = static_cast<uint8_t>(mode);
        std::memcpy(raw.data() + (mode == 2 ? 24 : 16), data, 2048u);
        return raw;
    }

    Bytes rawImage(const Bytes &iso, int mode)
    {
        Bytes out;
        for (size_t sector = 0; sector * 2048u < iso.size(); ++sector)
        {
            const Bytes raw = rawSector(iso.data() + sector * 2048u, sector, mode);
            out.insert(out.end(), raw.begin(), raw.end());
        }
        return out;
    }

    // ---- CHD v5, uncompressed (MAME's chd.h layout, as libchdr reads it) --

    // `data`: the hunks' bytes, `hunkBytes` each (the last padded with zeros);
    // `metadata`: (tag, text) entries chained in order.
    Bytes writeChd(const Bytes &data, uint32_t hunkBytes, uint32_t unitBytes, uint64_t logicalBytes,
                   const std::vector<std::pair<std::string, std::string>> &metadata)
    {
        const uint32_t hunks = static_cast<uint32_t>((logicalBytes + hunkBytes - 1u) / hunkBytes);
        constexpr size_t kHeader = 124u;
        const size_t mapOffset = kHeader;
        size_t metaOffset = mapOffset + hunks * 4u;
        Bytes meta;
        std::vector<size_t> starts;
        for (const auto &[tag, text] : metadata)
        {
            starts.push_back(metaOffset + meta.size());
            const size_t at = meta.size();
            meta.resize(at + 16u + text.size() + 1u, 0u);
            std::memcpy(meta.data() + at, tag.data(), 4u);
            putBe(meta, at + 4u, (0x01u << 24u) | static_cast<uint32_t>(text.size() + 1u), 4); // flags: checksummed
            std::memcpy(meta.data() + at + 16u, text.c_str(), text.size() + 1u);
        }
        for (size_t i = 0; i + 1u < starts.size(); ++i)
            putBe(meta, starts[i] - metaOffset + 8u, starts[i + 1u], 8);
        // Hunks start on a multiple of the hunk size (the map gives offset / hunk size).
        const size_t firstHunk = (metaOffset + meta.size() + hunkBytes - 1u) / hunkBytes;
        Bytes chd((firstHunk + hunks) * static_cast<size_t>(hunkBytes), 0u);
        std::memcpy(chd.data(), "MComprHD", 8u);
        putBe(chd, 8u, kHeader, 4);
        putBe(chd, 12u, 5u, 4);         // version
        putBe(chd, 32u, logicalBytes, 8); // compressors [16, 32) all none: uncompressed
        putBe(chd, 40u, mapOffset, 8);
        putBe(chd, 48u, metadata.empty() ? 0u : metaOffset, 8);
        putBe(chd, 56u, hunkBytes, 4);
        putBe(chd, 60u, unitBytes, 4);
        for (uint32_t hunk = 0; hunk < hunks; ++hunk)
        {
            putBe(chd, mapOffset + hunk * 4u, firstHunk + hunk, 4);
            const size_t from = static_cast<size_t>(hunk) * hunkBytes;
            const size_t count = std::min<size_t>(hunkBytes, data.size() > from ? data.size() - from : 0u);
            if (count)
                std::memcpy(chd.data() + (firstHunk + hunk) * static_cast<size_t>(hunkBytes), data.data() + from, count);
        }
        if (!meta.empty())
            std::memcpy(chd.data() + metaOffset, meta.data(), meta.size());
        return chd;
    }

    // A CD's CHD (chdman createcd's layout): 2448-byte frames (sector data
    // first, `sectorBytes` of it, then zeros and the subcode), 8 a hunk, the
    // track padded to 4 frames; an audio track after it.
    Bytes cdChd(const Bytes &iso, const char *type, uint32_t sectorBytes)
    {
        constexpr uint32_t kFrame = 2448u, kHunk = 8u * kFrame;
        const size_t sectors = iso.size() / 2048u;
        const size_t padded = (sectors + 3u) / 4u * 4u, audio = 8u;
        Bytes data((padded + audio) * kFrame, 0u);
        for (size_t s = 0; s < sectors; ++s)
        {
            uint8_t *frame = data.data() + s * kFrame;
            if (sectorBytes == 2352u)
            {
                const Bytes raw = rawSector(iso.data() + s * 2048u, s, std::strcmp(type, "MODE1_RAW") == 0 ? 1 : 2);
                std::memcpy(frame, raw.data(), raw.size());
            }
            else if (sectorBytes == 2336u)
                std::memcpy(frame + 8u, iso.data() + s * 2048u, 2048u); // (the subheader, zeros, then Form 1's data)
            else
                std::memcpy(frame, iso.data() + s * 2048u, 2048u);
        }
        for (size_t f = padded; f < padded + audio; ++f)
            std::memset(data.data() + f * kFrame, 0x5A, 2352u);
        const std::string track1 = std::string("TRACK:1 TYPE:") + type + " SUBTYPE:NONE FRAMES:" + std::to_string(sectors) +
                                   " PREGAP:0 PGTYPE:MODE1 PGSUB:RW POSTGAP:0";
        const std::string track2 = "TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:" + std::to_string(audio) +
                                   " PREGAP:150 PGTYPE:AUDIO PGSUB:RW POSTGAP:0";
        return writeChd(data, kHunk, kFrame, data.size(), {{"CHT2", track1}, {"CHT2", track2}});
    }

    // A DVD's CHD (chdman createdvd's): the ISO's bytes in 2048-byte units.
    Bytes dvdChd(const Bytes &iso) { return writeChd(iso, 4u * 2048u, 2048u, iso.size(), {{"DVD ", ""}}); }

    // ---- CSO and ZSO (maxcso's README_CSO.md; ZSO: CSO v1 with LZ4) ------

    enum class Packing { Stored, Deflate, Lz4 };

    Bytes deflateRaw(const uint8_t *data, size_t size)
    {
        Bytes out(size + size / 2u + 1024u);
        const size_t made = tdefl_compress_mem_to_mem(out.data(), out.size(), data, size, TDEFL_DEFAULT_MAX_PROBES);
        out.resize(made);
        return out;
    }

    Bytes lz4Block(const uint8_t *data, size_t size)
    {
        Bytes out(static_cast<size_t>(LZ4_compressBound(static_cast<int>(size))));
        const int made = LZ4_compress_default(reinterpret_cast<const char *>(data), reinterpret_cast<char *>(out.data()),
                                              static_cast<int>(size), static_cast<int>(out.size()));
        out.resize(static_cast<size_t>(made));
        return out;
    }

    // `pick(block)`: how a block is packed, when packing makes it smaller.
    Bytes writeCso(const Bytes &iso, const char *magic, uint8_t version, uint32_t blockBytes, uint8_t shift,
                   const std::function<Packing(size_t)> &pick)
    {
        const size_t blocks = (iso.size() + blockBytes - 1u) / blockBytes;
        Bytes out(24u + (blocks + 1u) * 4u, 0u);
        std::memcpy(out.data(), magic, 4u);
        putLe(out, 4u, 24u, 4);
        putLe(out, 8u, iso.size(), 8);
        putLe(out, 16u, blockBytes, 4);
        out[20] = version;
        out[21] = shift;
        auto align = [&]()
        {
            while (out.size() % (size_t{1} << shift))
                out.push_back(0xEEu); // padding: any byte
        };
        align();
        std::vector<uint32_t> index;
        for (size_t b = 0; b < blocks; ++b)
        {
            const size_t from = b * blockBytes, count = std::min<size_t>(blockBytes, iso.size() - from);
            Packing packing = pick(b);
            Bytes packed = packing == Packing::Lz4 ? lz4Block(iso.data() + from, count)
                                                   : packing == Packing::Deflate ? deflateRaw(iso.data() + from, count) : Bytes();
            if (packing != Packing::Stored && packed.size() + (size_t{1} << shift) >= count)
                packing = Packing::Stored; // (no smaller: stored, as maxcso does)
            uint32_t entry = static_cast<uint32_t>(out.size() >> shift);
            if (packing == Packing::Stored)
            {
                packed.assign(iso.begin() + static_cast<std::ptrdiff_t>(from), iso.begin() + static_cast<std::ptrdiff_t>(from + count));
                if (version < 2u)
                    entry |= 0x80000000u;
                else
                    packed.resize(blockBytes, 0u); // (v2: stored is a whole block's length, the last one's too)
            }
            else if (version >= 2u && packing == Packing::Lz4)
                entry |= 0x80000000u;
            index.push_back(entry);
            out.insert(out.end(), packed.begin(), packed.end());
            align();
        }
        index.push_back(static_cast<uint32_t>(out.size() >> shift));
        for (size_t i = 0; i < index.size(); ++i)
            putLe(out, 24u + i * 4u, index[i], 4);
        return out;
    }

    // Opens, checks and installs `image`: the files it installs.
    struct Installed
    {
        bool ok = false;
        std::string message, kind;
        std::vector<std::pair<std::string, Bytes>> files;
    };
    Installed install(const fs::path &image, const fs::path &gameDir)
    {
        Installed result;
        DiscSource source;
        if (!source.open(image, &result.message))
            return result;
        result.kind = source.kindText();
        const SourceCheck check = checkSource(source, games(), 0);
        if (check.status != SourceCheck::Status::Ok)
        {
            result.message = check.message;
            return result;
        }
        const InstallResult installed = installGame(source, games()[0], gameDir, nullptr);
        result.message = installed.message;
        result.ok = installed.ok;
        const fs::path disc = installedDiscFolder(gameDir);
        std::error_code ec;
        for (fs::recursive_directory_iterator it(disc, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                result.files.emplace_back(fs::relative(it->path(), disc).generic_string(), readAll(it->path()));
        std::sort(result.files.begin(), result.files.end());
        return result;
    }

    // Android's documents made from a folder (as ps2_game_setup_tests.cpp's).
    struct FolderDocuments final : DocumentAccess
    {
        fs::path root;
        explicit FolderDocuments(fs::path folder) : root(std::move(folder)) { installDocumentAccess(this); }
        ~FolderDocuments() override { installDocumentAccess(nullptr); }
        static constexpr std::string_view kPrefix = "content://formats/";
        std::string uriOf(const fs::path &path) const
        {
            const std::string relative = fs::relative(path, root).generic_string();
            return std::string(kPrefix) + (relative == "." ? "" : relative);
        }
        fs::path pathOf(const std::string &uri) const { return root / uri.substr(kPrefix.size()); }
        bool isDocument(std::string_view text) const override { return text.rfind(kPrefix, 0) == 0; }
        bool stat(const std::string &uri, Entry &out) const override
        {
            const fs::path path = pathOf(uri);
            std::error_code ec;
            if (!fs::exists(path, ec))
                return false;
            const bool folder = fs::is_directory(path, ec);
            out = {uri, uri == kPrefix ? std::string("picked folder") : path.filename().string(),
                   folder ? 0u : static_cast<uint64_t>(fs::file_size(path, ec)), folder};
            return true;
        }
        std::vector<Entry> list(const std::string &folderUri) const override
        {
            std::vector<Entry> out;
            for (const fs::directory_entry &entry : fs::directory_iterator(pathOf(folderUri)))
            {
                Entry e;
                stat(uriOf(entry.path()), e);
                out.push_back(e);
            }
            return out;
        }
        std::FILE *open(const std::string &uri) const override { return std::fopen(pathOf(uri).string().c_str(), "rb"); }
    };
}

void register_ps2_disc_formats_tests()
{
    MiniTest::Case("DiscFormats", [](TestCase &tc)
    {
        // Every format of the same disc, each under a name that is not its
        // format's (the bytes tell): the files installed are the ISO's.
        tc.Run("every format installs the ISO's files", [](TestCase &t)
        {
            TempDir dir;
            const fs::path isoPath = dir.path / "disc.iso";
            test_iso::write(isoPath, discFiles());
            const Bytes iso = readAll(isoPath);
            const Installed reference = install(isoPath, dir.path / "games-iso" / "ai");
            t.IsTrue(reference.ok && reference.files.size() == 4u, "the ISO installs: " + reference.message);

            struct Case
            {
                std::string name, kind;
                Bytes bytes;
            };
            const std::vector<Case> cases = {
                {"cd-raw-mode2.chd", "CHD disc image", cdChd(iso, "MODE2_RAW", 2352u)},
                {"cd-raw-mode1.img", "CHD disc image", cdChd(iso, "MODE1_RAW", 2352u)},
                {"cd-mode1.chd", "CHD disc image", cdChd(iso, "MODE1", 2048u)},
                {"cd-mode2.chd", "CHD disc image", cdChd(iso, "MODE2", 2336u)},
                {"dvd.iso", "CHD disc image", dvdChd(iso)},
                {"v1.cso", "CSO disc image", writeCso(iso, "CISO", 1, 2048u, 0, [](size_t) { return Packing::Deflate; })},
                {"v1-shift2.bin", "CSO disc image", writeCso(iso, "CISO", 1, 2048u, 2, [](size_t) { return Packing::Deflate; })},
                {"v2-mixed.cso", "CSO disc image", writeCso(iso, "CISO", 2, 8192u, 1, [](size_t b)
                                                            { return b % 2u ? Packing::Lz4 : Packing::Deflate; })},
                {"lz4.zso", "ZSO disc image", writeCso(iso, "ZISO", 1, 2048u, 0, [](size_t) { return Packing::Lz4; })},
                {"zso-big-blocks.iso", "ZSO disc image", writeCso(iso, "ZISO", 1, 16384u, 3, [](size_t) { return Packing::Lz4; })},
            };
            for (const Case &c : cases)
            {
                writeAll(dir.path / c.name, c.bytes);
                const Installed got = install(dir.path / c.name, dir.path / ("games-" + c.name) / "ai");
                t.IsTrue(got.ok, c.name + " installs: " + got.message);
                t.Equals(got.kind, c.kind, c.name + " is told by its bytes");
                t.IsTrue(got.files == reference.files, c.name + ": the ISO's files, byte for byte");
            }

            // The reader alone: every byte of the image's 2048-byte view.
            for (const Case &c : cases)
            {
                ps2x::iop::Iso9660Image image;
                std::string error;
                Bytes view(iso.size());
                t.IsTrue(image.open(dir.path / c.name, &error) && image.sizeBytes() == iso.size() &&
                             image.read(0u, view.data(), view.size()) && view == iso,
                         c.name + ": the ISO's every sector " + error);
            }
        });

        tc.Run("a damaged compressed image is refused, not read wrong", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "disc.iso", discFiles());
            const Bytes iso = readAll(dir.path / "disc.iso");
            Bytes cso = writeCso(iso, "CISO", 1, 2048u, 0, [](size_t) { return Packing::Deflate; });
            cso.resize(cso.size() / 2u);
            writeAll(dir.path / "half.cso", cso);
            const Installed half = install(dir.path / "half.cso", dir.path / "games" / "ai");
            t.IsFalse(half.ok, "half a CSO: " + half.message);
            Bytes chd = cdChd(iso, "AUDIO", 2352u);
            writeAll(dir.path / "audio.chd", chd);
            DiscSource source;
            std::string error;
            t.IsFalse(source.open(dir.path / "audio.chd", &error), "a CHD with no data track");
            t.IsTrue(error.find("audio.chd") != std::string::npos, "named in the message: " + error);
        });

        tc.Run("a multi-track CUE: its data track's file, the sheet told by its text", [](TestCase &t)
        {
            const std::string sheet = "REM a multi-track dump\r\n"
                                      "FILE \"Game (Track 1).bin\" BINARY\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n"
                                      "FILE \"Game (Track 2).bin\" BINARY\r\n  TRACK 02 MODE2/2352\r\n    INDEX 01 00:00:00\r\n"
                                      "FILE \"Game (Track 3).bin\" BINARY\r\n  TRACK 03 AUDIO\r\n    INDEX 00 00:00:00\r\n";
            t.IsTrue(ps2x::iop::isCueSheet(sheet), "a CUE sheet");
            t.Equals(ps2x::iop::cueDataTrackFile(sheet), std::string("Game (Track 2).bin"), "the data track's file");
            t.IsFalse(ps2x::iop::isCueSheet("BOOT2 = cdrom0:\\SLUS_203.27;1\r\n"), "a SYSTEM.CNF is none");
            std::string bytes = "FILE a.bin BINARY\n";
            bytes.push_back('\0');
            bytes += "TRACK 01 MODE1/2048\n";
            t.IsFalse(ps2x::iop::isCueSheet(bytes), "nor are bytes");
            t.Equals(ps2x::iop::cueDataTrackFile("FILE a.bin BINARY\n TRACK 01 AUDIO\n"), std::string(), "audio alone: none");

            TempDir dir;
            test_iso::write(dir.path / "disc.iso", discFiles());
            const Bytes iso = readAll(dir.path / "disc.iso");
            const Installed reference = install(dir.path / "disc.iso", dir.path / "games-iso" / "ai");
            writeAll(dir.path / "Game (Track 1).bin", Bytes(2352u * 20u, 0x11u));
            writeAll(dir.path / "Game (Track 2).bin", rawImage(iso, 2));
            writeAll(dir.path / "Game (Track 3).bin", Bytes(2352u * 20u, 0x22u));
            std::ofstream(dir.path / "Game.txt", std::ios::binary) << sheet; // (a CUE by its text, not its name)
            const Installed got = install(dir.path / "Game.txt", dir.path / "games-cue" / "ai");
            t.IsTrue(got.ok, "installs from the sheet: " + got.message);
            t.IsTrue(got.files == reference.files, "the ISO's files, byte for byte");
            t.IsTrue(isCueSheetSource(dir.path / "Game.txt") && !isCueSheetSource(dir.path / "disc.iso"), "told apart");

            // A folder of the dump: its sheet stands for the three BINs (the
            // folders below hold the installs' disc folders, found too).
            const std::vector<fs::path> sources = discSourcesIn(dir.path);
            std::vector<std::string> names;
            for (const fs::path &source : sources)
                if (source.parent_path() == dir.path)
                    names.push_back(source.filename().string());
            std::sort(names.begin(), names.end());
            t.IsTrue(names == std::vector<std::string>{"Game.txt", "disc.iso"}, "the sheet and the ISO");
        });

        // The first run's search (collection/disc_finder.h): a player's folder
        // of folders, each disc in another format, among files that are no
        // disc; and the same folder through Android's documents.
        tc.Run("the search finds every format in a folder of folders, among other files", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "disc.iso", discFiles());
            const Bytes iso = readAll(dir.path / "disc.iso");
            const fs::path roms = dir.path / "ROMs";
            struct Placed
            {
                fs::path path;
                bool disc;
            };
            const std::vector<Placed> placed = {
                {roms / "ps2" / "Aggressive Inline (USA).iso", true},
                {roms / "ps2" / "chd" / "ai.chd", true},
                {roms / "ps2" / "cso" / "deep" / "ai.cso", true},
                {roms / "ps2" / "zso" / "ai.zso", true},
                {roms / "ps2" / "bincue" / "ai.bin", false}, // (its CUE's)
                {roms / "ps2" / "bincue" / "ai.cue", true},
                {roms / "bios" / "scph39001.bin", false},
                {roms / "readme.txt", false},
                {roms / "ps2" / "covers" / "ai.png", false},
                {roms / ".hidden" / "ai.iso", false},
                {roms / "a" / "b" / "c" / "d" / "e" / "too-deep.iso", false},
            };
            for (const Placed &p : placed)
                fs::create_directories(p.path.parent_path());
            writeAll(placed[0].path, iso);
            writeAll(placed[1].path, cdChd(iso, "MODE2_RAW", 2352u));
            writeAll(placed[2].path, writeCso(iso, "CISO", 1, 2048u, 0, [](size_t) { return Packing::Deflate; }));
            writeAll(placed[3].path, writeCso(iso, "ZISO", 1, 2048u, 0, [](size_t) { return Packing::Lz4; }));
            writeAll(placed[4].path, rawImage(iso, 2));
            std::ofstream(placed[5].path) << "FILE \"ai.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n";
            writeAll(placed[6].path, Bytes(4u * 1024u * 1024u, 0x5Au));
            std::ofstream(placed[7].path) << "my discs";
            writeAll(placed[8].path, Bytes(3000u, 0x89u));
            writeAll(placed[9].path, iso);
            writeAll(placed[10].path, iso);
            // A disc's files in a folder of their own.
            const fs::path folderDisc = roms / "ps2" / "AI files";
            for (const test_iso::File &file : discFiles())
            {
                fs::create_directories((folderDisc / file.path).parent_path());
                writeAll(folderDisc / file.path, file.data);
            }

            size_t reports = 0;
            const DiscSearch search = findDiscs({roms}, games(), {}, [&](const SearchProgress &p)
            {
                ++reports;
                return !p.at.empty();
            });
            std::vector<std::string> names;
            for (const FoundDisc &disc : search.discs)
            {
                names.push_back(disc.name);
                t.IsTrue(disc.kind == FoundDisc::Kind::Ps2 && disc.game == 0 && disc.status == SourceCheck::Status::Ok,
                         disc.name + " checks as AI: " + disc.message);
            }
            std::sort(names.begin(), names.end());
            t.IsTrue(names == std::vector<std::string>{"AI files", "Aggressive Inline (USA).iso", "ai.chd", "ai.cso", "ai.cue", "ai.zso"},
                     "the six discs, the BIN/CUE by its CUE, nothing else or too deep");
            t.IsTrue(reports == search.looked && search.looked > names.size(), "every file and folder reported");
            t.IsTrue(search.ps2Of(0) && search.ps2Of(0)->bytes > 0, "the game's disc, with its size");
            t.IsNull(search.ps2Of(1), "no other game's");
            t.IsTrue(discSourcesIn(roms).size() == 6u, "discSourcesIn finds the same");

            // Stopped at the first report: nothing more looked at.
            const DiscSearch stopped = findDiscs({roms}, games(), {}, [](const SearchProgress &) { return false; });
            t.IsTrue(stopped.cancelled && stopped.discs.empty() && stopped.looked == 1u, "stops");

            // Files dropped one by one, and a place gone, as places.
            const DiscSearch dropped = findDiscs({placed[0].path, placed[5].path, placed[7].path, dir.path / "gone"}, games(), {});
            t.Equals(dropped.discs.size(), size_t{2}, "the two discs dropped");

            // Added to: a disc found again is not found twice.
            DiscSearch both = dropped;
            both.add(findDiscs({placed[0].path, placed[1].path}, games(), {}));
            t.Equals(both.discs.size(), size_t{3}, "each disc once");

            // The same folder as Android's documents give it.
            FolderDocuments documents(roms);
            const DiscSearch picked = findDiscs({pathFromUtf8(documents.uriOf(roms))}, games(), {});
            std::vector<std::string> documentNames;
            for (const FoundDisc &disc : picked.discs)
            {
                documentNames.push_back(disc.name);
                t.IsTrue(isDocument(disc.source), disc.name + " found as a document");
            }
            std::sort(documentNames.begin(), documentNames.end());
            t.IsTrue(documentNames == std::vector<std::string>{"AI files", "Aggressive Inline (USA).iso", "ai.bin", "ai.chd", "ai.cso",
                                                               "ai.zso"},
                     "the same discs, the BIN/CUE by its BIN (a CUE document alone cannot read it)");
            for (const FoundDisc &disc : picked.discs)
            {
                const Installed got = install(disc.source, dir.path / ("games-" + disc.name) / "ai");
                t.IsTrue(got.ok && got.files == install(placed[0].path, dir.path / "games-ref" / "ai").files,
                         disc.name + " installs the ISO's files: " + got.message);
            }
        });

        tc.Run("Android's documents: a CHD picked, a folder of CSO and ZSO dumps", [](TestCase &t)
        {
            TempDir dir;
            test_iso::write(dir.path / "disc.iso", discFiles());
            const Bytes iso = readAll(dir.path / "disc.iso");
            const Installed reference = install(dir.path / "disc.iso", dir.path / "games-iso" / "ai");
            const fs::path picks = dir.path / "picks";
            fs::create_directories(picks);
            writeAll(picks / "Aggressive Inline (USA).chd", cdChd(iso, "MODE2_RAW", 2352u));
            writeAll(picks / "a.cso", writeCso(iso, "CISO", 2, 2048u, 0, [](size_t b) { return b % 3u ? Packing::Lz4 : Packing::Deflate; }));
            writeAll(picks / "b.zso", writeCso(iso, "ZISO", 1, 2048u, 0, [](size_t) { return Packing::Lz4; }));
            std::ofstream(picks / "notes.txt") << "not a disc";
            FolderDocuments documents(picks);
            for (const char *name : {"Aggressive Inline (USA).chd", "a.cso", "b.zso"})
            {
                const Installed got = install(pathFromUtf8(documents.uriOf(picks / name)), dir.path / "games-doc" / name / "ai");
                t.IsTrue(got.ok, std::string(name) + " through its document: " + got.message);
                t.IsTrue(got.files == reference.files, std::string(name) + ": the ISO's files, byte for byte");
            }
            const std::vector<fs::path> sources = discSourcesIn(pathFromUtf8(documents.uriOf(picks)));
            t.Equals(sources.size(), size_t{3}, "the folder's three discs, by their bytes");
        });
    });
}
