// Aggressive Inline (SLUS-20327): the Ape, the Xbox release's secret
// skater (its 21st, after the Tinman; the PS2 release has 19 and "None").
//
// What the Xbox executable has that the PS2 one lacks (default.xbe):
//   - one more row in each skater table (index 20): the name "Ape" in the
//     25-byte name table the model is found by (0x1D8790), the code "AP"
//     (0x1D9488), the stats 0 0 2 2 0 2 0 (the Tinman's; 0x1D89A0, seven
//     per skater), the default name "APE" (0x1D8A38), a 21st skater record
//     (0x510 bytes from 0x1EB208); SCENE.TXT's "Ape" model
//     (assets\skaters\Gorilla\GOR.ZIT) and the select screen's cha_ape
//     picture (uilib.zal SkaterShots) and ":AP" label (FrontEnd's Refresh);
//   - the select list (0x50140) adds AP last; the skater lock bits are 22
//     wide instead of 20 (0x3FFFFF), bit 20 the Ape's;
//   - the unlock: all the hidden challenges of a level unlock its secret
//     skater (PS2 0x1BD7xx, table 0x2541C8 by level); the Museum's (level 6)
//     unlock the Mummy (16) and, on the Xbox only, the Ape (0x544C3: level
//     6 also sets the table's ninth entry, 20). The all-skaters cheat code
//     sets 0x3FFFFF. The Ape is therefore unlocked exactly when the Mummy
//     is, which is how it is told here (bit 16 of the unlock word 0x24CBB8;
//     no save change).
//
// Here the Ape takes skater index 0, the "None" slot every PS2 table has
// (index 20 would run past each table into the next one: the 0x510-byte
// records end at 0x254070, where other data starts). Slot 0 of each table
// gets the Ape's row: the code (0x24BFD0), stats (0x24B240), default name
// (0x24B2D0), select picture (0x265738); its record (0x24DB30, never saved
// by the game, which saves skaters 1..19) is the Ape's career, initialised
// by the game's own boot code (0x1BCB28 runs 0x190CE8 on all 20 records)
// from those rows. The places that treat index 0 as "no skater":
//   - the skater setup 0x1910B8 skips the model when the index is 0: it
//     runs with index 20 until it has read the model name (names[20] is the
//     4 padding bytes after the table, 0x24B23C, holding "Ape"), put back
//     at its call 0x191134 of the model lookup 0x13D7F8. Nothing else in
//     between reads the index. Slot 0's name (0x24B048, 25 bytes) holds
//     the new strings ("SkaterShots.cha_ape", "AP");
//   - the select list 0x1A8340 lists 1..19: its Items string gets AP
//     (locked: "!!AP") where it is set (0x1802A8 with the path 0x2654C8);
//   - the career save (begin 0x1D8948) and load (apply 0x1D8CC0) write and
//     read skaters 1..19: the native saves (aggressive_inline_saves.cpp)
//     write the Ape's record after the game's data, in the same layout
//     ("XAPE" block), and apply's wrapper reads it back. The save's size is
//     fixed (0x70400) and the game ignores what follows its own data, so
//     the file stays a PS2 save. (Appended where the file is written, not
//     after begin: begin can be suspended in its allocation and finish
//     after its wrapper returned. With ZX_SAVES=card the Ape's career is
//     not saved.)
// Index 0 needs no other care: the code lookup 0x1A81E8 returns 0 for "AP"
// (and for an unknown code, as before), the records' readers index by it
// (level enter 0x1A5EB4, level select, stats, name entry, the session
// commit 0x194238), and every all-skater loop (unlock cheats) covers 0..19.
//
// Model: the Xbox GOR.ZIT made into the PS2's at build time
// (zx-xbox-convert, runtime/zaxis_ape.h: convertModel's textures as
// 8-bit BMPs at most 256 a side as the PS2 skaters', strips, materials, and
// its materials' first colour as every other skater's), served here from
// ZX_XBOX_CONVERTED (aggressive_inline_converted.h) with the Xbox
// originals (512) registered for the GPU renderer. Animations are the
// shared skaters\MOTIONS.ZAL (the same file on both discs). The model has
// no shadow object, so the Ape casts no shadow, as on the Xbox. Nothing is
// converted here, and nothing derived from either disc is written anywhere.

#include "aggressive_inline_ape.h"
#include "sym/game_overrides/aggressive_inline_ape.cpp.h"
#include "overrides/zge_sites.h"

#include "aggressive_inline_converted.h"
#include "aggressive_inline_disc.h"
#include "machine.h"
#include "runtime/disc_patch.h"
#include "runtime/game_files.h"
#include "runtime/xbox_files.h"
#include "runtime/zaxis_ui.h"
#include "runtime/zaxis_ape.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aggressive_inline::ape
{
    namespace
    {
        namespace gf = zx::game_files;

        // ---- the skater tables (slot 0 is "None") -------------------------
        constexpr uint32_t kNames = sym::g_skaterModelNames;       // 20 x 25 bytes: SCENE model names
        constexpr uint32_t kNameStride = 25u;
        constexpr uint32_t kApeNameSlot = kNames + 20u * kNameStride; // 0x24B23C, 4 padding bytes
        constexpr uint32_t kShotString = kNames;       // "SkaterShots.cha_ape" (20 bytes) in slot 0
        constexpr uint32_t kCodeString = kNames + 20u; // "AP"
        constexpr uint32_t kCodes = sym::skaterCodes;       // 20 pointers: "None", "CE", ... "TM"
        constexpr uint32_t kStats = sym::statStarts;       // 20 x 7 bytes
        constexpr uint32_t kDefaultNames = sym::skaterNames; // 20 x 16 bytes
        constexpr uint32_t kShots = sym::shotTable;       // 20 pointers: "", "SkaterShots.cha_chris", ...
        constexpr uint32_t kApeIndexOnXbox = 20u;      // the 4 bytes at names[20]
        // The Xbox table's row 20 (0x1D89A0 + 20 * 7): the Tinman's.
        constexpr std::array<uint8_t, 7> kApeStats = {0, 0, 2, 2, 0, 2, 0};

        // ---- the code ------------------------------------------------------
        constexpr uint32_t kSkaterSetup = sym::setupSkater;     // (player): model, bones, ...
        constexpr uint32_t kModelByName = sym::newSceneModelByName;     // (0, name)
        constexpr uint32_t kPlayerSkater = 0x5E0u;         // player byte: skater index (low 5 bits)
        constexpr uint32_t kSetVariable = sym::setVariable;     // (screen, path, text)
        constexpr uint32_t kItemsPath = sym::skaterSelectorItems;       // "...CharacterSelect.Menu.SkaterSelector.Items"
        constexpr uint32_t kUnlockWord = sym::g_unlockedSkaters;      // skater lock bits 0..19
        constexpr uint32_t kMummyBit = 16u;

        // ---- the career save ---------------------------------------------
        constexpr uint32_t kApply = sym::restoreGameSave;     // finish (a0 = result): modes 0 and 3 read it
        constexpr uint32_t kSaveMode = sym::g_gameSaveMode;
        constexpr uint32_t kCareerBuffer = sym::g_gameSaveData;
        constexpr uint32_t kCareerSize = 0x70400u;
        constexpr uint32_t kResultOk = 2u;
        constexpr uint32_t kBlobAlloc = sym::saveRegionAllocate; // (size) -> block
        constexpr uint32_t kBlobFree = sym::saveRegionFree;  // (block)
        constexpr uint32_t kGlobalsSize = 0xF74u;
        constexpr uint32_t kRecords = sym::g_skaterRecords;
        constexpr uint32_t kRecordStride = 0x510u;
        constexpr uint32_t kRecordSize = 0x2A8u;
        constexpr uint32_t kLevels = 10u;
        constexpr uint32_t kLevelStride = 0x2Cu;
        constexpr uint32_t kBlobs = 5u;
        constexpr std::array<uint32_t, 6> kLevelWords = {4u, 0xCu, 0x14u, 0x1Cu, 0x24u, 0x28u};
        constexpr char kBlockMagic[4] = {'X', 'A', 'P', 'E'};
        constexpr uint32_t kBlockVersion = 1u;

        const char kModelFolder[] = "ASSETS/SKATERS/GORILLA/";

        struct Ape
        {
            bool on = false;
            PS2Machine::RecompiledFunction skaterSetup = nullptr;
            PS2Machine::RecompiledFunction modelByName = nullptr;
            PS2Machine::RecompiledFunction setVariable = nullptr;
            PS2Machine::RecompiledFunction apply = nullptr;
            uint32_t pendingPlayer = 0; // its index is 20 until the model name is read

            std::mutex mutex;
            std::shared_ptr<const std::vector<uint8_t>> model;
            bool modelTried = false;
        };
        Ape &ape() { return zx::session::state<Ape>(); }

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        void wr32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, 4);
        }

        uint8_t rd8(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void wr8(uint8_t *rdram, uint32_t address, uint8_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                *p = value;
        }

        std::string guestString(const uint8_t *rdram, uint32_t address, size_t max = 256)
        {
            std::string out;
            for (size_t i = 0; address && i < max; ++i)
            {
                const uint8_t c = rd8(rdram, address + static_cast<uint32_t>(i));
                if (!c)
                    break;
                out.push_back(static_cast<char>(c));
            }
            return out;
        }

        void writeString(uint8_t *rdram, uint32_t address, const std::string &text, size_t field)
        {
            for (size_t i = 0; i < field; ++i)
                wr8(rdram, address + static_cast<uint32_t>(i), i < text.size() ? static_cast<uint8_t>(text[i]) : 0u);
        }

        bool unlocked(const uint8_t *rdram)
        {
            return (rd32(rdram, kUnlockWord) >> kMummyBit) & 1u;
        }

        // Slot 0 of each table becomes the Ape's. False (nothing written) if
        // the tables are not what this was made for.
        bool patchTables(uint8_t *rdram)
        {
            const bool expected = guestString(rdram, kNames) == "None" && guestString(rdram, rd32(rdram, kCodes)) == "None" &&
                                  guestString(rdram, rd32(rdram, kCodes + 4u)) == "CE" &&
                                  guestString(rdram, rd32(rdram, kShots)).empty() &&
                                  guestString(rdram, rd32(rdram, kShots + 19u * 4u)) == "SkaterShots.cha_tinman" &&
                                  guestString(rdram, kNames + 19u * kNameStride) == "Tinman" &&
                                  rd32(rdram, kApeNameSlot) == 0u && guestString(rdram, kDefaultNames) == "NONE";
            if (!expected)
                return false;
            writeString(rdram, kApeNameSlot, "Ape", 4u);
            writeString(rdram, kShotString, "SkaterShots.cha_ape", 20u);
            writeString(rdram, kCodeString, "AP", 3u);
            wr32(rdram, kCodes, kCodeString);
            wr32(rdram, kShots, kShotString);
            for (size_t i = 0; i < kApeStats.size(); ++i)
                wr8(rdram, kStats + static_cast<uint32_t>(i), kApeStats[i]);
            writeString(rdram, kDefaultNames, "APE", 16u);
            return true;
        }

        // ---- the skater setup: the model by name ---------------------------

        // func_1910B8(player): with index 0 it would skip the skater; the
        // Ape's runs with index 20 (whose name, the padding after the
        // table, is "Ape") until the name is read.
        void skaterSetupHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t player = getRegU32(ctx, 4);
            const uint8_t skater = rd8(rdram, player + kPlayerSkater);
            if ((skater & 0x1Fu) == 0u)
            {
                wr8(rdram, player + kPlayerSkater, static_cast<uint8_t>((skater & ~0x1Fu) | kApeIndexOnXbox));
                ape().pendingPlayer = player;
                std::cout << "[ape] setting up the Ape (player 0x" << std::hex << player << std::dec << ")" << std::endl;
            }
            ape().skaterSetup(rdram, ctx, runtime);
            if (ape().pendingPlayer)
            {
                // Not put back by the name lookup: the setup took another way.
                const uint8_t now = rd8(rdram, ape().pendingPlayer + kPlayerSkater);
                wr8(rdram, ape().pendingPlayer + kPlayerSkater, static_cast<uint8_t>(now & ~0x1Fu));
                ape().pendingPlayer = 0;
                std::cerr << "[ape] the skater setup did not look the model up" << std::endl;
            }
        }

        void modelByNameHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            // (the setup's one name lookup: pendingPlayer is set only while
            // the wrapped setup runs; never known by the call's return address)
            if (ape().pendingPlayer)
            {
                zge_host::sites::seen("ai.ape.setup_name");
                const uint8_t now = rd8(rdram, ape().pendingPlayer + kPlayerSkater);
                wr8(rdram, ape().pendingPlayer + kPlayerSkater, static_cast<uint8_t>(now & ~0x1Fu));
                ape().pendingPlayer = 0;
            }
            ape().modelByName(rdram, ctx, runtime);
        }

        // ---- the select list -----------------------------------------------

        // func_1802A8(screen, path, text): the skater list's Items (CE, SY,
        // ... TM, the locked ones as "!!BO") gets AP last, as on the Xbox.
        // The text is 0x1A8340's stack buffer (256 bytes; about 70 used).
        void setVariableHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (getRegU32(ctx, 5) == kItemsPath)
            {
                const uint32_t text = getRegU32(ctx, 6);
                std::string items = guestString(rdram, text, 512);
                if (items.find("TM") != std::string::npos && items.find("AP") == std::string::npos && items.size() < 200u)
                {
                    items += unlocked(rdram) ? "\nAP" : "\n!!AP";
                    writeString(rdram, text, items, items.size() + 1u);
                }
            }
            ape().setVariable(rdram, ctx, runtime);
        }

        // ---- the career save -------------------------------------------------

        // The Ape's record as the game's serializer writes a skater: the
        // 0x2A8-byte record, then for each of the 10 levels six words (five
        // blob sizes, one more) and the blobs.
        bool serializeApe(const uint8_t *rdram, std::vector<uint8_t> &out)
        {
            out.clear();
            auto copy = [&](uint32_t address, uint32_t size)
            {
                for (uint32_t i = 0; i < size; ++i)
                {
                    const uint8_t *p = getConstMemPtr(rdram, address + i);
                    if (!p)
                        return false;
                    out.push_back(*p);
                }
                return true;
            };
            if (!copy(kRecords, kRecordSize))
                return false;
            for (uint32_t level = 0; level < kLevels; ++level)
            {
                const uint32_t record = kRecords + kRecordSize + level * kLevelStride;
                for (uint32_t field : kLevelWords)
                    if (!copy(record + field, 4u))
                        return false;
                for (uint32_t blob = 0; blob < kBlobs; ++blob)
                {
                    const uint32_t size = rd32(rdram, record + blob * 8u + 4u);
                    if (size > kCareerSize || (size && !copy(rd32(rdram, record + blob * 8u), size)))
                        return false;
                }
            }
            return true;
        }

        uint32_t byteSum(const uint8_t *data, size_t size)
        {
            uint32_t sum = 0;
            for (size_t i = 0; i < size; ++i)
                sum += data[i];
            return sum;
        }

        // Where the game's own data in a career body ends (globals, skaters
        // 1..19), or 0 if it does not add up.
        size_t gameDataEnd(const uint8_t *body, size_t size)
        {
            size_t at = kGlobalsSize;
            for (uint32_t skater = 1; skater < 20u; ++skater)
            {
                at += kRecordSize;
                for (uint32_t level = 0; level < kLevels; ++level)
                {
                    if (at + 24u > size)
                        return 0;
                    uint32_t words[6];
                    std::memcpy(words, body + at, sizeof(words));
                    at += 24u;
                    for (uint32_t blob = 0; blob < kBlobs; ++blob)
                    {
                        if (words[blob] > size - at)
                            return 0;
                        at += words[blob];
                    }
                }
            }
            return at <= size ? at : 0;
        }

        // The block at the end of the game's data: magic, version, payload
        // size, payload, the payload's byte sum.
        std::vector<uint8_t> makeBlock(const std::vector<uint8_t> &payload)
        {
            std::vector<uint8_t> block(kBlockMagic, kBlockMagic + 4);
            auto put = [&](uint32_t v)
            {
                const size_t at = block.size();
                block.resize(at + 4u);
                std::memcpy(block.data() + at, &v, 4);
            };
            put(kBlockVersion);
            put(static_cast<uint32_t>(payload.size()));
            block.insert(block.end(), payload.begin(), payload.end());
            put(byteSum(payload.data(), payload.size()));
            return block;
        }

        // The payload of the block in a body, or false.
        bool readBlock(const uint8_t *body, size_t size, std::vector<uint8_t> &payload)
        {
            const size_t end = gameDataEnd(body, size);
            if (!end || end + 16u > size || std::memcmp(body + end, kBlockMagic, 4) != 0)
                return false;
            uint32_t version = 0, length = 0, sum = 0;
            std::memcpy(&version, body + end + 4u, 4);
            std::memcpy(&length, body + end + 8u, 4);
            if (version != kBlockVersion || length > size - end - 16u)
                return false;
            std::memcpy(&sum, body + end + 12u + length, 4);
            if (sum != byteSum(body + end + 12u, length))
                return false;
            payload.assign(body + end + 12u, body + end + 12u + length);
            return true;
        }

        // A payload into skater record 0, as the game's load does a skater
        // (blobs: the old freed, new ones from the game's allocator).
        bool restoreApe(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, const std::vector<uint8_t> &payload)
        {
            // Check it all first.
            size_t at = kRecordSize;
            for (uint32_t level = 0; level < kLevels; ++level)
            {
                if (at + 24u > payload.size())
                    return false;
                uint32_t words[6];
                std::memcpy(words, payload.data() + at, sizeof(words));
                at += 24u;
                for (uint32_t blob = 0; blob < kBlobs; ++blob)
                {
                    if (words[blob] > payload.size() - at)
                        return false;
                    at += words[blob];
                }
            }
            if (at != payload.size())
                return false;
            const uint32_t sp = getRegU32(ctx, 29);
            for (uint32_t i = 0; i < kRecordSize; ++i)
                wr8(rdram, kRecords + i, payload[i]);
            at = kRecordSize;
            for (uint32_t level = 0; level < kLevels; ++level)
            {
                const uint32_t record = kRecords + kRecordSize + level * kLevelStride;
                uint32_t words[6];
                std::memcpy(words, payload.data() + at, sizeof(words));
                at += 24u;
                for (size_t w = 0; w < kLevelWords.size(); ++w)
                    wr32(rdram, record + kLevelWords[w], words[w]);
                for (uint32_t blob = 0; blob < kBlobs; ++blob)
                {
                    const uint32_t pointer = record + blob * 8u;
                    if (const uint32_t old = rd32(rdram, pointer))
                        runtime->callGuestFunction(rdram, ctx, kBlobFree, {old}, sp);
                    wr32(rdram, pointer, 0u);
                    const uint32_t size = words[blob];
                    if (!size)
                        continue;
                    uint32_t block = 0;
                    if (!runtime->callGuestFunction(rdram, ctx, kBlobAlloc, {size}, sp, &block) || !block)
                    {
                        wr32(rdram, pointer + 4u, 0u);
                        std::cerr << "[ape] no memory for the Ape's career data" << std::endl;
                        return false;
                    }
                    for (uint32_t i = 0; i < size; ++i)
                        wr8(rdram, block + i, payload[at + i]);
                    wr32(rdram, pointer, block);
                    at += size;
                }
            }
            return true;
        }

        bool appendBlock(const uint8_t *rdram, uint8_t *body, size_t size, size_t &used)
        {
            std::vector<uint8_t> payload;
            if (!serializeApe(rdram, payload))
            {
                std::cerr << "[ape] the Ape's career is out of range, not saved" << std::endl;
                return false;
            }
            const std::vector<uint8_t> block = makeBlock(payload);
            if (used > size || block.size() > size - used)
            {
                std::cerr << "[ape] no room for the Ape's career in the save" << std::endl;
                return false;
            }
            std::memcpy(body + used, block.data(), block.size());
            used += block.size();
            return true;
        }

        // finish (0x1D8CC0): a load (modes 0 and 3) that succeeded read
        // skaters 1..19 from the buffer; the Ape's follows them.
        void applyHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t mode = rd32(rdram, kSaveMode);
            const uint32_t result = getRegU32(ctx, 4);
            std::vector<uint8_t> payload;
            bool found = false;
            if ((mode == 0u || mode == 3u) && result == kResultOk)
            {
                const uint32_t buffer = rd32(rdram, kCareerBuffer);
                const uint8_t *body = buffer ? getConstMemPtr(rdram, buffer) : nullptr;
                if (body && getConstMemPtr(rdram, buffer + kCareerSize - 1u) == body + kCareerSize - 1u)
                    found = readBlock(body, kCareerSize, payload);
            }
            ape().apply(rdram, ctx, runtime);
            if ((mode == 0u || mode == 3u) && result == kResultOk)
            {
                if (!found)
                    std::cout << "[ape] the career has no Ape data: the Ape starts afresh" << std::endl;
                else if (restoreApe(rdram, ctx, runtime, payload))
                    std::cout << "[ape] the Ape's career loaded" << std::endl;
                else
                    std::cerr << "[ape] the Ape's career data is damaged, not loaded" << std::endl;
            }
        }

        // ---- files --------------------------------------------------------

        // The model made at build time (zx-xbox-convert,
        // runtime/zaxis_ape.h), with the Xbox pictures of its converted
        // textures for the GPU renderer (keyed by the converted textures, as
        // the Zoo's).
        std::shared_ptr<const std::vector<uint8_t>> makeModel()
        {
            auto bytes = aggressive_inline::converted::read(zx::zaxis::ape::kModelPath);
            if (!bytes)
                return nullptr;
            const auto pictures =
                aggressive_inline::converted::registerPictures(zx::zaxis::ape::kModelPath, "ape:GOR.ZIT", false);
            if (pictures.ok)
                std::cout << "[ape] " << pictures.count << " Xbox originals of the Ape's textures for the GPU renderer"
                          << std::endl;
            return bytes;
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            if (path != std::string(kModelFolder) + "GOR.ZIT")
                return nullptr;
            std::lock_guard<std::mutex> lock(ape().mutex);
            if (!ape().modelTried)
            {
                ape().modelTried = true;
                ape().model = makeModel();
            }
            return ape().model;
        }

        // SCENE.TXT: the Ape's model after the Tinman's, as the Xbox's.
        bool patchScene(std::vector<uint8_t> &bytes)
        {
            std::string text(bytes.begin(), bytes.end());
            const size_t name = text.find("NAME\t\t\"Tinman\"");
            const size_t close = name == std::string::npos ? name : text.find("}\r\n", name);
            if (close == std::string::npos || text.find("\"Ape\"") != std::string::npos)
            {
                std::cerr << "[ape] SCENE.TXT: the Tinman's model was not found, no Ape" << std::endl;
                return false;
            }
            text.insert(close + 3u, "\tMODEL\r\n\t\t{\r\n\t\tNAME\t\t\"Ape\"\r\n\t\tTYPE\t\t$kModelTypePlayer\r\n"
                                    "\t\tSUBTYPE\t\t0\r\n\t\tFILE\t\t\"assets\\skaters\\Gorilla\\GOR.ZIT\"\r\n\t\t}\r\n");
            bytes.assign(text.begin(), text.end());
            std::cout << "[ape] SCENE.TXT: the Ape's model" << std::endl;
            return true;
        }

        // UILIB.ZAL: the Ape's picture in SkaterShots (from the Xbox
        // library) and its name in the select screen's Refresh script.
        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries, xboxEntries;
            std::vector<uint8_t> xbox;
            std::string error;
            if (!zx::zaxis::readArchive(bytes, entries, &error) || !zx::xbox_files::readFile("uilib.zal", xbox) ||
                !zx::zaxis::readArchive(xbox, xboxEntries, &error))
            {
                std::cerr << "[ape] UILIB.ZAL: " << (error.empty() ? "no Xbox uilib.zal" : error) << std::endl;
                return false;
            }
            bool picture = false, script = false;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (!picture)
                    for (const zx::zaxis::ArchiveEntry &donor : xboxEntries)
                        if (donor.nameHash == entry.nameHash &&
                            !zx::zaxis::addAssets(entry.data, donor.data, {"cha_ape"}).empty())
                            picture = true;
                if (zx::zaxis::scriptName(entry) != "FrontEnd")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (text.find("\"FreeskateOnChange\"") == std::string::npos)
                    continue;
                // After the Tinman's case of SkaterSelector's Refresh.
                const size_t tinman = text.find("SET SkaterName.Text {\"Junkie\"}");
                const size_t done = tinman == std::string::npos ? tinman : text.find("GOTO Done\r\n", tinman);
                if (done == std::string::npos)
                    continue;
                const size_t lineStart = text.rfind('\n', done) + 1u;
                const std::string tabs = text.substr(lineStart, done - lineStart);
                text.insert(done + 11u, tabs + ":AP\r\n" + tabs + "// Ape\r\n" + tabs + "SET SkaterName.Text {\"Ape\"}\r\n" +
                                            tabs + "GOTO Done\r\n");
                entry.data.assign(text.begin(), text.end());
                script = true;
            }
            if (!picture || !script)
                std::cerr << "[ape] UILIB.ZAL: " << (picture ? "" : "no cha_ape picture ") << (script ? "" : "no select script")
                          << std::endl;
            if (!picture && !script)
                return false;
            bytes = zx::zaxis::writeArchive(entries);
            std::cout << "[ape] UILIB.ZAL: the Ape in the skater select" << std::endl;
            return true;
        }
    }

    bool appendCareer(const uint8_t *rdram, std::vector<uint8_t> &body, size_t &used)
    {
        if (!ape().on)
            return false;
        if (!used)
            used = gameDataEnd(body.data(), body.size());
        return used && appendBlock(rdram, body.data(), body.size(), used);
    }

    void install(PS2Machine &runtime)
    {
        const char *on = zx::boot::text("ZX_APE");
        if (!on || on[0] != '1')
        {
            std::cout << "[ape] off (ZX_APE=1 adds the Xbox release's Ape skater)" << std::endl;
            return;
        }
        if (!aggressive_inline::converted::exists(zx::zaxis::ape::kModelPath) || !zx::xbox_files::init() ||
            !zx::xbox_files::exists("uilib.zal"))
        {
            std::cout << "[ape] no Ape: no GOR.ZIT made at build time ("
                      << aggressive_inline::converted::description() << ") or no Xbox uilib.zal ("
                      << zx::xbox_files::description() << ")" << std::endl;
            return;
        }
        uint8_t *rdram = runtime.memory().getRDRAM();
        if (!rdram || !patchTables(rdram))
        {
            std::cerr << "[ape] the skater tables are not the expected ones, no Ape" << std::endl;
            return;
        }
        if (!(ape().skaterSetup = runtime.lookupFunction(kSkaterSetup)) || !(ape().modelByName = runtime.lookupFunction(kModelByName)) ||
            !(ape().setVariable = runtime.lookupFunction(kSetVariable)) || !(ape().apply = runtime.lookupFunction(kApply)) || !runtime.replaceFunction(kSkaterSetup, &skaterSetupHook) ||
            !runtime.replaceFunction(kModelByName, &modelByNameHook) ||
            !runtime.replaceFunction(kSetVariable, &setVariableHook) ||
            !runtime.replaceFunction(kApply, &applyHook))
        {
            std::cerr << "[ape] the game's functions were not wrapped, the Ape is half there" << std::endl;
            return;
        }
        gf::registerProvider(kModelFolder, &provide, "ape");
        zx::session::onEnd([]() { gf::removeProviders("ape"); });
        zx::disc_patch::registerPatch("SCENE.TXT", &patchScene, "ai");
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, "ai");
        ape().on = true;
        std::cout << "[ape] on: the Ape is skater 0 (" << zx::xbox_files::description()
                  << "), unlocked with the Mummy (the Museum's hidden challenges)" << std::endl;
    }
}

namespace aggressive_inline
{
    void installApe(PS2Machine &runtime)
    {
        ape::install(runtime);
    }
}
