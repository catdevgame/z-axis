// Dave Mirra Freestyle BMX 2 (SLUS-20159): DM2's own Xbox-only levels
// (Venice, Greenville) in the level select, ridden with DM2's own rider,
// physics, collision, grinds and renderer.
//
// The levels are made at build time by tools/modding/dm2_xbox.py from
// the user's DM2 Xbox disc (the same engine's files with the Xbox's
// textures, strip groups and material bits made the PS2's; the app sets
// ZX_DM2_XBOX_LEVELS_DIR, Settings > Game > Xbox Levels sets
// ZX_DM2_XBOX_LEVELS). They are values 9, 10, as on the Xbox after
// Airport, with the Xbox's own names, texts and level-select pictures; each
// level's Xbox pictures (<LEVEL>.XTP) are registered for the GPU renderer
// when its texture library is served. This file only serves those files and
// adds the menu entries. Nothing here reads a disc image or converts
// anything.
//
// The slot: DM2 has eight levels and per-level tables sized for them (and
// the park editor, level 8): stats at 0x215BA0 + level * 0x21C, far clips at
// 0x21CC70, goals, gaps. SCENE.TXT's Level09 ("greenvil", a cut level) has
// no row in those tables, so the Xbox levels run in Woodward Camp's slot
// (level 2, the first in the menu, never locked): when one of the level
// select's extra entries is chosen, Woodward's scene files are its files.
//   - The level select (MainMenu.SelectLevel, a toggler over menu values
//     0..8, 8 = park editor) gets values 9, 10, ... in Free Ride, one per
//     extra level (Init: MaxValue = MaxLevel + 1 + their count instead of
//     + 1). SetupStats shows a value's name and text from the string table
//     ($KSTR_LEVELNAMEMENU_<value>, $KSTR_LEVELINFOMENU_<value>; STRING.EUS
//     and the other languages' files get ours, over DM2's unused 9 and 10)
//     and its picture (LevelModel.Material "Levels.level<value>"). Its
//     OnSelect starts them as any level ("Level9" and on have no label).
//   - The menu-to-level table 0x25A1C8 (menu value -> level, u8[9]) is read
//     by bmxmenuGameLevelUpdate 0x1AF388 (into 0x25A1AC), by BmxStartGame
//     0x1B0EE8 (into the session's level, 0x2566CA) and by
//     bmxSetupLevelStats 0x1AFE58 (the stats and records shown, for the
//     toggler's CurValue). Its byte 9 (0x25A1D1, unused) is 2; past it lie
//     other variables (0x25A1D4 on), so for values from 10 on the first two
//     are set to 2 after the game's own code ran, and the third reads
//     CurValue as 9 (its read of the variable, copyVariableAtPath, is followed).
//   - While Root.CurrentLevel (0x25A1A8) is one of ours, ASSETS/LEVELS/
//     WOODWARD/WOODWARD.ZIT is that level's ZIT, and the four files it
//     names (<LEVEL>.GEO, INS, TXR, DAT) come from its folder; Woodward's
//     far clip in the table 0x21CC70 reads 0, so the level's own (Globals
//     +0x10) is used (initCamera's level start).
//   - After the run the game sets CurrentLevel back from the level index
//     (Woodward's menu value 0); the next time Woodward itself loads.
//   - The pictures: <dir>/PICTURES.ZIF (dm2_xbox.py, the Xbox UILIB's own)
//     holds a material per level ("xb_<level>"); each is renamed to its
//     value's ("Level9", whose own picture, an unused copy of the park
//     editor's, is renamed out of the way) and added to UILIB.ZAL's
//     LEVELS.ZIF, the level select's pictures.
//
// With ZX_DM2_XBOX_LEVELS_DIR=<dir> (holding levels.txt and
// <LEVEL>/<LEVEL>.*): every level of levels.txt, in its order;
// ZX_DM2_XBOX_LEVEL=<FOLDER> keeps only that one (developers: it is then
// value 9); ZX_DM2_XBOX_LEVELS=0 turns them off with the folder set (the
// app's setting).
// ZX_TRACE_XBOXLEVEL=1 (developers): files served, load time, textures,
// parts drawn, and every 60 frames DM2's GS texture cache, the VIF1
// interrupts by MARK value, the frame packet and the frame's DMA chain
// (tags, and whether the rider's packet lies past tag 4,096, where the
// runtime's chain guard used to stop).

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_xboxlevel.cpp.h"
#include "overrides/zge_xbox_textures.h"

#include "machine.h"
#include "runtime/disc_patch.h"
#include "runtime/game_files.h"
#include "runtime/pad.h"
#include "runtime/save_store.h"
#include "runtime/texture_replace.h"
#include "gs/gs_backend.h"
#include "runtime/zaxis_text.h"
#include "runtime/zaxis_ui.h"
#include "Stubs/MemoryCard.h"
#include "runtime/boot_config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dave_mirra::xboxlevel
{
    namespace
    {
        namespace gf = zx::game_files;
        namespace fs = std::filesystem;

        constexpr uint32_t kCurrentLevel = sym::dm2::g_currentLevel;   // Root.CurrentLevel, the menu value
        constexpr uint32_t kMenuToLevel = sym::dm2::g_levelMenuGameLevels;    // u8[9]: menu value -> level
        constexpr uint32_t kMenuValue = 9u;               // the first of ours
        constexpr uint32_t kHostLevel = 2u;               // Woodward Camp
        constexpr uint32_t kMenuLevel = sym::dm2::g_currentGameLevel;      // u32, the level of the menu value (bmxmenuGameLevelUpdate)
        constexpr uint32_t kSessionLevel = sym::dm2::g_sessionLevel;   // u8, the level to play (BmxStartGame)
        constexpr uint32_t kLevelUpdate = sym::dm2::bmxmenuGameLevelUpdate;    // bmxmenuGameLevelUpdate
        constexpr uint32_t kStartGame = sym::dm2::bmxStartGame;      // BmxStartGame
        constexpr uint32_t kFarClips = sym::dm2::g_levelFarClips;       // float per level
        constexpr uint32_t kLevelIndex = sym::dm2::g_sessionLevel;     // u8, the level being played
        constexpr uint32_t kLevelFrame = sym::dm2::g_physicsTaskFrames;     // counts while a level runs
        constexpr uint32_t kTextureLoad = sym::dm2::loadTextureChunk;    // TXRC handler (AI 0x128240)
        const char kHostFolder[] = "ASSETS/LEVELS/WOODWARD/";
        const char kHostZit[] = "WOODWARD.ZIT";

        struct Level
        {
            std::string folder; // folder name, "MOVIE", "VENICE"
            std::string name;   // shown in the level select, "Venice"
            fs::path dir;       // its folder: levels.txt, <LEVEL>/<LEVEL>.*, PICTURES.ZIF
            std::string info;   // the level select's text (its INFO.TXT)
        };
        // A level's picture in PICTURES.ZIF (dm2_xbox.py picture_assets).
        std::string pictureName(const Level &level)
        {
            std::string name = "xb_";
            for (char c : level.folder)
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return name;
        }
        constexpr uint32_t kBucketCount = 7u; // DM2's draw buckets

        // One boot's Xbox levels: the levels found, the files served from
        // them, the hooks' originals and the trace's counts. Made at the
        // boot's first use (the install), so a game booted after another in
        // the same process starts as a fresh one does (zx::boot::PerBoot).
        struct State
        {
            std::vector<Level> levels; // menu value 9 + i
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> small; // <LEVEL>/ ZIT, INS, DAT
            std::atomic<uint8_t *> rdram{nullptr};
            bool trace = false;
            std::atomic<int> loadingLevel{-1}; // the level whose ZIT was opened last
            std::vector<std::string> pictureSources; // the Xbox pictures registered (texture_replace sources)
            // Load time: from the ZIT's open to the level's first frame.
            std::chrono::steady_clock::time_point loadStart;
            std::atomic<bool> loading{false};
            std::atomic<uint32_t> textures{0};
            std::atomic<uint32_t> rampFaces{0}, linkedFaces{0}; // DM2's lip links (with the trace)
            size_t pictures = 0; // levels with a picture in LEVELS.ZIF
            // Woodward's far clip while ours is chosen.
            uint32_t savedFarClip = 0;
            bool farClipCleared = false;
            // The hooks' originals.
            PS2Machine::RecompiledFunction textureLoad = nullptr;
            PS2Machine::RecompiledFunction faceUnlinked = nullptr, faceLinked = nullptr;
            PS2Machine::RecompiledFunction levelUpdate = nullptr, startGame = nullptr;
            PS2Machine::RecompiledFunction setupStats = nullptr, readVariable = nullptr;
            bool inSetupStats = false;
            // With the trace: the level's parts and what is drawn of them.
            PS2Machine::RecompiledFunction newPart = nullptr, drawRigid = nullptr;
            std::mutex partsMutex;
            std::atomic<uint32_t> skinnedDraws{0}, rigidDraws{0};
            std::map<uint32_t, uint32_t> partIndex;          // part -> number in the GEO
            std::map<uint32_t, uint16_t> partsDrawn;         // part -> faces
            std::vector<uint32_t> firstDraws, lastFirstDraws; // this and the last frame's
            std::set<uint32_t> frameDraws, lastFrameDraws, suspects; // VU1 writes: parts drawn in all of them
            bool suspected = false;
            uint64_t vu1Generation = 0;
            // With the trace: the collision and grind probes, the skinned
            // renderer, the allocator.
            PS2Machine::RecompiledFunction raycast = nullptr;
            bool probed = false;
            bool grindsProbed = false;
            PS2Machine::RecompiledFunction drawSkinned = nullptr;
            std::atomic<uint32_t> lastSkinnedNode{0};
            std::atomic<uint32_t> riderPacketAt{0}; // where the frame's skinned draw (the rider) starts in its packet
            PS2Machine *runtime = nullptr;
            PS2Machine::RecompiledFunction allocate = nullptr;
            uint64_t failedAllocations = 0;
            // The draw buckets, in extended memory while an Xbox level runs.
            PS2Machine::RecompiledFunction bucketReset = nullptr;
            bool extendedAdded = false;
            uint32_t bigBuckets[kBucketCount] = {}, gameBuckets[kBucketCount][2] = {};
            bool bucketsSwapped = false;
            uint32_t bucketPeak[kBucketCount] = {};
            // With the trace: DM2's GS texture cache, the frame's packet and
            // DMA chain, the MARK interrupts.
            PS2Machine::RecompiledFunction texFrameEnd = nullptr, texFlush = nullptr, texMarkIrq = nullptr;
            std::atomic<uint32_t> texIrqs{0};
            uint32_t texFrames = 0, texOverflowFrames = 0, texFlushes = 0, texPeakUse = 0, texPeakBottom = 0;
            uint32_t texFrameStartBottom = 0, texFrameStartTop = 0, texPeakFrameUse = 0, texIrqPeak = 0;
            uint64_t texIrqTotal = 0, texAllFrames = 0, texAllOverflow = 0;
            uint32_t packetPeak = 0, markPeak = 0, generationChanges = 0, lastGeneration = 0;
            uint32_t tagPeak = 0, framesOver = 0, riderCut = 0, riderFrames = 0;
            uint64_t tagTotal = 0, allOver = 0, allRiderCut = 0, allRiderFrames = 0;
            uint64_t markTotal = 0;
            std::atomic<uint32_t> markIrqs[4] = {};
        };

        State &state()
        {
            static zx::boot::PerBoot<std::unique_ptr<State>> ofBoot([] { return std::make_unique<State>(); });
            return *ofBoot.get();
        }

        // "<in>:<out>" (the probes' and the ride checks' files): split at the
        // first colon that is not a drive letter's ("C:\x" or "C:/x"), so
        // Windows paths work.
        bool splitPathPair(const std::string &text, std::string &first, std::string &second)
        {
            for (size_t at = text.find(':'); at != std::string::npos; at = text.find(':', at + 1u))
            {
                const bool drive = at >= 1u && std::isalpha(static_cast<unsigned char>(text[at - 1u])) &&
                                   (at == 1u || text[at - 2u] == ':') && at + 1u < text.size() &&
                                   (text[at + 1u] == '\\' || text[at + 1u] == '/');
                if (drive)
                    continue;
                first = text.substr(0, at);
                second = text.substr(at + 1u);
                return !first.empty() && !second.empty();
            }
            return false;
        }

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }
        void writeU32(uint8_t *rdram, uint32_t address, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &v, 4);
        }
        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, 4);
        }
        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        // The extra level of a menu value, or -1.
        int levelOf(uint32_t value)
        {
            return value >= kMenuValue && value - kMenuValue < state().levels.size() ? static_cast<int>(value - kMenuValue) : -1;
        }
        // The extra level the level select has chosen, or -1.
        int current()
        {
            const uint8_t *rdram = state().rdram.load();
            return rdram ? levelOf(readU32(rdram, kCurrentLevel)) : -1;
        }
        // The level select's value is ours: the files are the extra level's.
        bool active()
        {
            return current() >= 0;
        }
        std::string levelName(int level)
        {
            return level >= 0 && static_cast<size_t>(level) < state().levels.size() ? state().levels[level].name : std::string("?");
        }

        std::shared_ptr<const std::vector<uint8_t>> readHostFile(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            return std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(in)),
                                                          std::istreambuf_iterator<char>());
        }


        // An Xbox level's Xbox pictures (<LEVEL>/<LEVEL>.XTP, dm2_xbox.py) for
        // the GPU renderer, registered when its texture library is served
        // (runtime/texture_replace.h: keyed by the converted textures'
        // size and indices; display.xbox_textures switches them).
        void registerXboxPictures(const Level &level)
        {
            const std::string source = "dm2xbox:" + level.folder;
            if (zx::texture_replace::hasSource(source))
                return;
            const auto bytes = readHostFile(level.dir / level.folder / (level.folder + ".XTP"));
            std::vector<zx::texture_replace::Entry> entries;
            std::string error;
            if (!bytes || !zx::texture_replace::parsePack(*bytes, entries, &error))
            {
                std::cerr << "[xboxlevel] " << level.name << ": no Xbox pictures (" << (bytes ? error : "no .XTP") << ")"
                          << std::endl;
                return;
            }
            std::cout << "[xboxlevel] " << level.name << ": " << entries.size() << " Xbox pictures for the GPU renderer"
                      << std::endl;
            zx::texture_replace::setSource(source, std::move(entries));
            state().pictureSources.push_back(source);
        }

        // The run stopped: the Xbox pictures this boot registered leave the
        // GPU renderer's sources (the next game booted brings its own).
        void removePictures(PS2Machine &)
        {
            State &st = state();
            for (const std::string &source : st.pictureSources)
                zx::texture_replace::removeSource(source);
            st.pictureSources.clear();
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            State &st = state();
            const int level = current();
            if (level < 0 || path.rfind(kHostFolder, 0) != 0)
                return nullptr;
            const std::string &folder = st.levels[level].folder;
            const std::string name = path.substr(sizeof(kHostFolder) - 1u);
            std::string file;
            if (name == kHostZit)
                file = folder + ".ZIT";
            else
                for (const char *ext : {".GEO", ".INS", ".TXR", ".DAT"})
                    if (name == folder + ext)
                        file = name;
            if (file.empty())
                return nullptr;
            if (name == kHostZit && !st.loading.exchange(true))
            {
                st.loadStart = std::chrono::steady_clock::now();
                st.textures = 0;
                st.rampFaces = 0;
                st.linkedFaces = 0;
                st.loadingLevel = level;
            }
            const fs::path at = st.levels[level].dir / folder / file;
            if (file == folder + ".TXR")
                registerXboxPictures(st.levels[level]);
            std::lock_guard<std::mutex> lock(st.mutex);
            if (auto it = st.small.find(folder + "/" + file); it != st.small.end())
                return it->second;
            std::shared_ptr<const std::vector<uint8_t>> bytes = readHostFile(at);
            if (!bytes)
                std::cerr << "[xboxlevel] " << at.string() << " cannot be read" << std::endl;
            else if (st.trace)
                std::cout << "[xboxlevel] " << path << ": " << folder << "/" << file << " (" << bytes->size() << " bytes)"
                          << std::endl;
            if (bytes && bytes->size() < (1u << 20))
                st.small[folder + "/" + file] = bytes; // the GEO and TXR are read again on the next load
            return bytes;
        }

        // ---- Each extra level its own records ----
        //
        // DM2's level records (13 of 0x14 bytes per mode and kind: a value,
        // the rider, the holder's name; savedRecord) are 0x21C bytes per
        // level at 0x215A88 + level * 0x21C, eight levels, part of the game
        // save (startCardSave / cardWorkDone). The extra levels play in
        // Woodward's slot, so they shared its row. Now each keeps its own,
        // beside the game save (<saves>/AiLevels.records, the name kept from
        // when Aggressive Inline's levels were served here too: "AILR", u32
        // 1, u32 count, then per level its folder in 16 bytes and the row;
        // written atomically with a .bak). At the level's start (gameSetup,
        // which copies the row into the run's records) the extra level's row is
        // put into Woodward's slot, and Woodward's own taken out; when the
        // level is left its row (new records entered at the run's end,
        // bmxoverlayHighScoreHandler) goes back to the file and Woodward's own comes back.
        // Every game save and load in between (setUiFrame operations 0 and
        // 2, the native saves'; startCardSave, cardWorkDone the card's) runs
        // with Woodward's own row in place. A level never played starts from
        // DM2's own starting row for Woodward (as the ELF has it).
        // ZX_DM2_XBOX_LEVEL_RECORDS=0: the shared row, as before.
        namespace records
        {
            constexpr uint32_t kRows = sym::dm2::g_savedRecords, kRowSize = 0x21Cu;
            constexpr uint32_t kHostRow = kRows + kHostLevel * kRowSize;
            constexpr uint32_t kLevelStart = sym::dm2::gameSetup;
            constexpr uint32_t kCardStart = sym::dm2::setUiFrame, kCardWrite = sym::dm2::startCardSave, kCardRead = sym::dm2::cardWorkDone;
            constexpr char kMagic[4] = {'A', 'I', 'L', 'R'};
            using Row = std::vector<uint8_t>;
            // One boot's records and the originals (zx::boot::PerBoot).
            struct RecordsState
            {
                bool on = false;
                Row defaultRow;                   // Woodward's starting row
                std::map<std::string, Row> rows;  // folder -> row (the file)
                Row hostOwn;                      // Woodward's own while an extra level's row is in its slot
                int inSlot = -1;                  // the extra level whose row is in Woodward's slot
                int guard = 0;                    // saves and loads running (the host's own row in place)
                PS2Machine::RecompiledFunction levelStart = nullptr, cardStart = nullptr, cardWrite = nullptr,
                                               cardRead = nullptr;
            };

            RecordsState &recordsState()
            {
                static zx::boot::PerBoot<RecordsState> ofBoot([] { return RecordsState{}; });
                return ofBoot.get();
            }

            fs::path file()
            {
                fs::path root;
                if (const char *dir = zx::boot::text("ZX_SAVES_DIR"); dir && dir[0])
                    root = dir;
                else
                {
                    const fs::path card = ps2_stubs::memoryCardFolder(0);
                    root = card.parent_path() / (card.filename().string() + "-saves");
                }
                return root / "AiLevels.records";
            }
            bool valid(const zx::saves::Bytes &data)
            {
                if (data.size() < 12u || std::memcmp(data.data(), kMagic, 4) != 0)
                    return false;
                uint32_t version = 0, count = 0;
                std::memcpy(&version, data.data() + 4, 4);
                std::memcpy(&count, data.data() + 8, 4);
                return version == 1u && data.size() == 12u + static_cast<size_t>(count) * (16u + kRowSize);
            }
            void readFile()
            {
                const zx::saves::ReadResult r = zx::saves::readWithBackup(file(), valid);
                if (!r.ok())
                    return;
                uint32_t count = 0;
                std::memcpy(&count, r.data.data() + 8, 4);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint8_t *at = r.data.data() + 12u + i * (16u + kRowSize);
                    const std::string folder(reinterpret_cast<const char *>(at), strnlen(reinterpret_cast<const char *>(at), 16));
                    recordsState().rows[folder] = Row(at + 16, at + 16 + kRowSize);
                }
            }
            void writeFile()
            {
                zx::saves::Bytes data(kMagic, kMagic + 4);
                auto put32 = [&](uint32_t v)
                {
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(&v);
                    data.insert(data.end(), b, b + 4);
                };
                put32(1u);
                put32(static_cast<uint32_t>(recordsState().rows.size()));
                for (const auto &[folder, row] : recordsState().rows)
                {
                    char name[16] = {};
                    std::memcpy(name, folder.data(), std::min<size_t>(folder.size(), 15u));
                    data.insert(data.end(), name, name + 16);
                    data.insert(data.end(), row.begin(), row.end());
                }
                std::string error;
                if (!zx::saves::writeFileAtomic(file(), data.data(), data.size(), &error))
                    std::cerr << "[xboxlevel] records: could not write " << file().string() << ": " << error << std::endl;
            }
            Row readRow(const uint8_t *rdram)
            {
                const uint8_t *p = getConstMemPtr(rdram, kHostRow);
                return p ? Row(p, p + kRowSize) : Row();
            }
            void writeRow(uint8_t *rdram, const Row &row)
            {
                if (uint8_t *p = getMemPtr(rdram, kHostRow); p && row.size() == kRowSize)
                    std::memcpy(p, row.data(), kRowSize);
            }
            // The extra level's row into Woodward's slot.
            void swapIn(uint8_t *rdram, int level)
            {
                RecordsState &rs = recordsState();
                State &st = state();
                if (rs.inSlot == level || level < 0 || rs.guard)
                    return;
                if (rs.inSlot < 0)
                    rs.hostOwn = readRow(rdram);
                else
                    rs.rows[st.levels[rs.inSlot].folder] = readRow(rdram);
                const auto it = rs.rows.find(st.levels[level].folder);
                writeRow(rdram, it != rs.rows.end() ? it->second : rs.defaultRow);
                rs.inSlot = level;
                if (st.trace)
                    std::cout << "[xboxlevel] records: " << st.levels[level].name << "'s row in Woodward's slot"
                              << (it != rs.rows.end() ? "" : " (never played: DM2's starting row)") << std::endl;
            }
            // Woodward's own back; the extra level's row kept (and written).
            void swapOut(uint8_t *rdram)
            {
                RecordsState &rs = recordsState();
                State &st = state();
                if (rs.inSlot < 0)
                    return;
                const Row row = readRow(rdram);
                Row &kept = rs.rows[st.levels[rs.inSlot].folder];
                const bool changed = kept != row;
                kept = row;
                writeRow(rdram, rs.hostOwn);
                if (st.trace)
                    std::cout << "[xboxlevel] records: " << st.levels[rs.inSlot].name << "'s row kept"
                              << (changed ? " (changed)" : "") << "; Woodward's own back" << std::endl;
                rs.inSlot = -1;
                if (changed)
                    writeFile();
            }
            void levelStartHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                if (current() >= 0)
                    swapIn(rdram, current());
                else
                    swapOut(rdram);
                recordsState().levelStart(rdram, ctx, runtime);
            }
            // A save or load with Woodward's own row in place; the extra row
            // back afterwards (a load's row is Woodward's new own).
            template <PS2Machine::RecompiledFunction RecordsState::*Original>
            void guarded(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                RecordsState &rs = recordsState();
                const int level = rs.inSlot;
                if (level >= 0)
                    swapOut(rdram);
                ++rs.guard;
                (rs.*Original)(rdram, ctx, runtime);
                --rs.guard;
                if (level >= 0)
                    swapIn(rdram, level);
            }
            void cardStartHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                const uint32_t op = getRegU32(ctx, 5) & 0xFFu;
                if (op == 0u || op == 2u)
                    guarded<&RecordsState::cardStart>(rdram, ctx, runtime);
                else
                    recordsState().cardStart(rdram, ctx, runtime);
            }
            // Every vsync: once the extra level is left, its row out.
            void vsync(uint8_t *rdram)
            {
                RecordsState &rs = recordsState();
                if (rs.defaultRow.empty())
                    rs.defaultRow = readRow(rdram);
                if (rs.inSlot >= 0 && current() != rs.inSlot && !rs.guard)
                    swapOut(rdram);
            }
            void install(PS2Machine &runtime)
            {
                RecordsState &rs = recordsState();
                if (const char *on = std::getenv("ZX_DM2_XBOX_LEVEL_RECORDS"); on && on[0] == '0')
                {
                    std::cout << "[xboxlevel] records: Woodward's row shared (ZX_DM2_XBOX_LEVEL_RECORDS=0)" << std::endl;
                    return;
                }
                readFile();
                if (!(rs.levelStart = runtime.lookupFunction(kLevelStart)) || !runtime.replaceFunction(kLevelStart, &levelStartHook) ||
                    !(rs.cardStart = runtime.lookupFunction(kCardStart)) || !runtime.replaceFunction(kCardStart, &cardStartHook) ||
                    !(rs.cardWrite = runtime.lookupFunction(kCardWrite)) ||
                    !runtime.replaceFunction(kCardWrite, &guarded<&RecordsState::cardWrite>) ||
                    !(rs.cardRead = runtime.lookupFunction(kCardRead)) || !runtime.replaceFunction(kCardRead, &guarded<&RecordsState::cardRead>))
                {
                    std::cerr << "[xboxlevel] records: the hooks cannot be placed; Woodward's row shared" << std::endl;
                    return;
                }
                rs.on = true;
                std::cout << "[xboxlevel] records: each level its own (" << file().string() << ", " << rs.rows.size()
                          << " kept)" << std::endl;
            }
        }

        // MainMenu.SelectLevel: values 9, 10, ... in Free Ride, each with its
        // name, text and picture.
        bool patchLevelSelect(std::string &text)
        {
            State &st = state();
            zx::zaxis::Span select = zx::zaxis::findObject(text, "MainMenu/SelectLevel");
            if (!select.found())
            {
                std::cerr << "[xboxlevel] MainMenu: SelectLevel was not found" << std::endl;
                return false;
            }
            // Init: Free Ride and the other modes that reach the park editor
            // (8 = MaxLevel + 1) get one more value per extra level.
            if (!zx::zaxis::replaceIn(text, select, "\\nADD MaxValue {1}\\n:5\\n",
                               "\\nADD MaxValue {" + std::to_string(1u + st.levels.size()) + "}\\n:5\\n"))
            {
                std::cerr << "[xboxlevel] MainMenu: SelectLevel.Init is not the expected script" << std::endl;
                return false;
            }
            std::cout << "[xboxlevel] level select: " << st.levels.size() << " extra levels from value " << kMenuValue
                      << " (Free Ride), " << st.pictures << " with a picture" << std::endl;
            return true;
        }

        // Renames assets of a library named in `names` (any case): the
        // asset's own name (ULIB > UAST > HAST: path[128], name[32], u32;
        // what "Levels.level3" finds) and its texture's (UMTL > TXRC > TXRH:
        // u32 name hash, ..., name[32] at +0x14; zx::zaxis::addAssets goes by
        // this one), the hash with it. The count of names changed.
        size_t renameTextures(std::vector<uint8_t> &library, const std::map<std::string, std::string> &names)
        {
            auto upper = [](std::string t)
            {
                for (char &c : t)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return t;
            };
            std::map<std::string, std::string> wanted;
            for (const auto &[from, to] : names)
                wanted[upper(from)] = to;
            size_t renamed = 0;
            std::function<void(size_t, size_t)> walk = [&](size_t at, size_t end)
            {
                while (at + 8u <= end)
                {
                    uint32_t size = 0;
                    std::memcpy(&size, library.data() + at + 4u, 4);
                    const size_t body = at + 8u;
                    if (size > end - body)
                        return;
                    const std::string tag(reinterpret_cast<const char *>(library.data() + at), 4);
                    const bool texture = tag == "TXRH" && size >= 0x34u, asset = tag == "HAST" && size >= 0xA0u;
                    if (tag == "ULIB" || tag == "UAST" || tag == "UMTL" || tag == "TXRC")
                        walk(body, body + size);
                    else if (texture || asset)
                    {
                        char *name = reinterpret_cast<char *>(library.data() + body + (texture ? 0x14u : 0x80u));
                        const auto it = wanted.find(upper(std::string(name, strnlen(name, 32))));
                        if (it != wanted.end() && it->second.size() < 32u)
                        {
                            std::memset(name, 0, 32);
                            std::memcpy(name, it->second.data(), it->second.size());
                            if (texture)
                            {
                                const uint32_t hash = zx::zaxis_text::keyHash(it->second);
                                std::memcpy(library.data() + body, &hash, 4);
                            }
                            ++renamed;
                        }
                    }
                    at = body + size;
                }
            };
            walk(0, library.size());
            return renamed;
        }

        // LEVELS.ZIF, the level select's pictures: ours (PICTURES.ZIF, made
        // by dm2_xbox.py) as "Level<value>"; DM2's Level9 out of their way.
        bool addPictures(std::vector<uint8_t> &library)
        {
            State &st = state();
            std::map<std::string, std::string> theirs;
            for (size_t i = 0; i < st.levels.size(); ++i)
            {
                const std::string value = "Level" + std::to_string(kMenuValue + i);
                theirs[value] = "Unused" + std::to_string(kMenuValue + i);
            }
            renameTextures(library, theirs);
            std::vector<fs::path> dirs;
            for (const Level &level : st.levels)
                if (std::find(dirs.begin(), dirs.end(), level.dir) == dirs.end())
                    dirs.push_back(level.dir);
            bool added = false;
            st.pictures = 0;
            for (const fs::path &dir : dirs)
            {
                std::ifstream in(dir / "PICTURES.ZIF", std::ios::binary);
                if (!in)
                    continue;
                std::vector<uint8_t> donor((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::map<std::string, std::string> ours;
                std::vector<std::string> names;
                for (size_t i = 0; i < st.levels.size(); ++i)
                    if (st.levels[i].dir == dir)
                    {
                        const std::string value = "Level" + std::to_string(kMenuValue + i);
                        ours[pictureName(st.levels[i])] = value;
                        names.push_back(value);
                    }
                renameTextures(donor, ours);
                st.pictures += zx::zaxis::addAssets(library, donor, names).size();
                added = true;
            }
            return added;
        }

        // The string tables: the level select's name and text of our values
        // ($KSTR_LEVELNAMEMENU_<value>, $KSTR_LEVELINFOMENU_<value>; DM2 has
        // 9, "Greenville", a cut level, and 10).
        bool patchStrings(std::vector<uint8_t> &bytes)
        {
            State &st = state();
            zx::zaxis_text::StringTable table;
            if (!zx::zaxis_text::readStringTable(bytes, table))
            {
                std::cerr << "[xboxlevel] a string table that cannot be read" << std::endl;
                return false;
            }
            auto set = [&](const std::string &key, const std::string &text)
            {
                const uint32_t hash = zx::zaxis_text::keyHash(key);
                for (auto &entry : table.entries)
                    if (entry.first == hash)
                    {
                        entry.second = text;
                        return;
                    }
                table.entries.emplace_back(hash, text);
            };
            for (size_t i = 0; i < st.levels.size(); ++i)
            {
                const std::string value = std::to_string(kMenuValue + i);
                set("KSTR_LEVELNAMEMENU_" + value, st.levels[i].name);
                set("KSTR_LEVELINFOMENU_" + value, st.levels[i].info.empty() ? "From the Xbox release." : st.levels[i].info);
            }
            bytes = zx::zaxis_text::writeStringTable(table);
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            std::string error;
            if (!zx::zaxis::readArchive(bytes, entries, &error))
            {
                std::cerr << "[xboxlevel] UILIB.ZAL: " << error << std::endl;
                return false;
            }
            bool changed = false;
            state().pictures = 0;
            for (zx::zaxis::ArchiveEntry &entry : entries)
                if (entry.nameHash == zx::zaxis_text::keyHash("LEVELS.ZIF") && addPictures(entry.data))
                    changed = true;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                if (zx::zaxis::scriptName(entry) != "MainMenu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (patchLevelSelect(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                }
            }
            if (changed)
                bytes = zx::zaxis::writeArchive(entries);
            return changed;
        }

        // Every vsync: the menu table's 9th entry, Woodward's far clip while
        // ours is chosen (the game's value back otherwise), the load time.
        void vsync(uint8_t *rdram, uint64_t)
        {
            State &st = state();
            st.rdram = rdram;
            const uint8_t *table = getConstMemPtr(rdram, kMenuToLevel);
            if (!table || table[8] != 8u)
                return; // the ELF is not loaded yet
            if (table[kMenuValue] != kHostLevel)
                if (uint8_t *p = getMemPtr(rdram, kMenuToLevel + kMenuValue))
                    *p = static_cast<uint8_t>(kHostLevel);
            const uint32_t farClip = kFarClips + kHostLevel * 4u;
            if (active() && !st.farClipCleared)
            {
                st.savedFarClip = readU32(rdram, farClip);
                writeU32(rdram, farClip, 0u);
                st.farClipCleared = true;
            }
            else if (!active() && st.farClipCleared)
            {
                writeU32(rdram, farClip, st.savedFarClip);
                st.farClipCleared = false;
            }
            if (records::recordsState().on)
                records::vsync(rdram);
            if (st.loading && readU32(rdram, kLevelFrame) > 0u)
            {
                const uint8_t level = getConstMemPtr(rdram, kLevelIndex) ? *getConstMemPtr(rdram, kLevelIndex) : 0xFFu;
                st.loading = false;
                std::cout << "[xboxlevel] " << levelName(st.loadingLevel) << " loaded in "
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - st.loadStart).count()
                          << " s (level " << int(level) << ", " << st.textures.load() << " textures; ramp and spine faces "
                          << st.rampFaces.exchange(0) << ", tied to a lip " << st.linkedFaces.exchange(0) << ")" << std::endl;
            }
        }

        void textureLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            st.textureLoad(rdram, ctx, runtime);
            if (st.loading)
                ++st.textures;
        }

        // With the trace: DM2's lip links made while the level loads
        // (linkNodeFaces per object: each face of a ramp or spine kind is
        // cleared, clearTagLink, then tied to the nearest edge of its kind
        // within 16, linkFaceToEdge).
        constexpr uint32_t kFaceUnlinked = sym::dm2::clearTagLink, kFaceLinked = sym::dm2::linkFaceToEdge;
        void faceUnlinkedHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            ++state().rampFaces;
            state().faceUnlinked(rdram, ctx, runtime);
        }
        void faceLinkedHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            ++state().linkedFaces;
            state().faceLinked(rdram, ctx, runtime);
        }

        // The menu table has a row for value 9 only: for ours, the level the
        // game's code read is put right (Woodward, the host) after it ran.
        void levelUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            state().levelUpdate(rdram, ctx, runtime);
            if (levelOf(readU32(rdram, kCurrentLevel)) > 0)
                writeU32(rdram, kMenuLevel, kHostLevel);
        }
        void startGameHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            state().startGame(rdram, ctx, runtime);
            if (levelOf(readU32(rdram, kCurrentLevel)) > 0)
                if (uint8_t *p = getMemPtr(rdram, kSessionLevel))
                    *p = static_cast<uint8_t>(kHostLevel);
        }
        // bmxSetupLevelStats reads MainMenu.SelectLevel.CurValue (0x250FE0)
        // through copyVariableAtPath (name, float *out) and indexes the menu table
        // with it: inside it, ours read as 9.
        constexpr uint32_t kSetupStats = sym::dm2::bmxSetupLevelStats;
        constexpr uint32_t kReadVariable = sym::dm2::copyVariableAtPath;
        constexpr uint32_t kSelectLevelValue = sym::dm2::selectLevelCurValuePath;
        void setupStatsHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            st.inSetupStats = true;
            st.setupStats(rdram, ctx, runtime);
            st.inSetupStats = false;
        }
        void readVariableHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t name = getRegU32(ctx, 4), out = getRegU32(ctx, 5);
            state().readVariable(rdram, ctx, runtime);
            if (state().inSetupStats && name == kSelectLevelValue)
            {
                const float value = readFloat(rdram, out);
                if (value >= 0.0f && levelOf(static_cast<uint32_t>(value)) > 0)
                    writeFloat(rdram, out, static_cast<float>(kMenuValue));
            }
        }

        // With the trace: the level's parts, numbered in the order the GEO
        // makes them (newPart, the part allocator, while the level
        // loads), and which of them the rigid renderer drawRigid (node,
        // cull, packet; the part at node +0x17C) draws, to measure how much
        // of the level is drawn; and the parts first drawn in a frame where
        // VU1 micro memory changes (DM2 writes it once, at boot).
        constexpr uint32_t kNewPart = sym::dm2::newPart;
        constexpr uint32_t kDrawRigid = sym::dm2::drawRigid;
        void newPartHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            st.newPart(rdram, ctx, runtime);
            if (!st.loading)
                return;
            std::lock_guard<std::mutex> lock(st.partsMutex);
            if (st.partIndex.empty())
                st.partsDrawn.clear();
            st.partIndex.emplace(getRegU32(ctx, 2), static_cast<uint32_t>(st.partIndex.size()));
        }
        // ZX_DM2_XBOX_LEVEL_PROBE=<points>:<results> (developers, with the
        // trace): DM2's own collision under each "x z" of <points>: its
        // raycast sceneQuery (kind 1, a segment of two float4 points;
        // flags 4, the hit in world space, as every rider and camera query
        // passes; no filter, no object list; the hit list {count, list} in
        // t2) on a vertical segment from y -500 to 500 (+y is down), which
        // keeps the nearest hit: its point is at 0x207C20. Written as
        // "x z y" (or "x z none") once the level has run 300 frames.
        constexpr uint32_t kRaycast = sym::dm2::sceneQuery;
        constexpr uint32_t kScene = sym::dm2::g_collisionWorld;
        // The first face hit's point (zge/collision/collision.h, DM2's QueryState: faces at +0x810, a FaceHit's point at
        // +0x10).
        constexpr uint32_t kHitPoint = sym::dm2::g_queryState + 0x810u + 0x10u;
        void probeCollision(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const char *spec = std::getenv("ZX_DM2_XBOX_LEVEL_PROBE");
            if (st.probed || !spec || !st.raycast || !readU32(rdram, kScene) || readU32(rdram, kLevelFrame) < 300u)
                return;
            st.probed = true;
            std::string from, to;
            if (!splitPathPair(spec, from, to))
            {
                std::cerr << "[xboxlevel] ZX_DM2_XBOX_LEVEL_PROBE=<points>:<results>, not " << spec << std::endl;
                return;
            }
            std::ifstream in(from);
            std::ofstream out(to);
            // Locals on the guest stack, well below the caller's frame.
            const uint32_t base = (getRegU32(ctx, 29) - 0x800u) & ~15u;
            const uint32_t segment = base, hit = base + 0x20u, calleeSp = base - 0x100u;
            float x = 0.0f, z = 0.0f;
            size_t points = 0, found = 0;
            const auto start = std::chrono::steady_clock::now();
            while (in >> x >> z)
            {
                ++points;
                const float p[8] = {x, -500.0f, z, 1.0f, x, 500.0f, z, 1.0f};
                for (int i = 0; i < 8; ++i)
                    writeFloat(rdram, segment + 4u * static_cast<uint32_t>(i), p[i]);
                writeU32(rdram, hit, 0u);
                writeU32(rdram, hit + 4u, 0u);
                uint32_t v0 = 0;
                runtime->callGuestFunction(rdram, ctx, st.raycast, kRaycast, {1u, segment, 4u, 0u, 0u, 0u, hit, 0u},
                                           calleeSp, &v0);
                if (!v0)
                {
                    out << x << " " << z << " none\n";
                    continue;
                }
                ++found;
                out << x << " " << z << " " << readFloat(rdram, kHitPoint + 4u) << "\n";
            }
            std::cout << "[xboxlevel] collision probe: " << points << " points, " << found << " with ground, "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s"
                      << std::endl;
        }

        // ZX_DM2_XBOX_LEVEL_GRINDS=<edges>:<results> (developers, with the
        // trace): for each "ax ay az bx by bz ..." of <edges> (dm2_xbox.py
        // edges: the level's grindable edges), DM2's grind search railEdgesNear (a0
        // the point, f12 the radius; the rider's search uses 3.5) at the
        // edge's middle, radius 0.25: the grindable edges of the faces it
        // reaches, a list of u64 {u32 object, u24 face, u8 edge} at
        // 0x2CC180 ending in 0. Each found edge's ends (vertexWorld:
        // object, face, vertex, out; vertex k and k + 1) are compared with
        // the listed edge's: "found" when both match within 0.01. Written as the
        // input line and "found" / "near" (a grindable edge found, not this
        // one) / "none", once the level has run 300 frames.
        constexpr uint32_t kEdgeSearch = sym::dm2::railEdgesNear;
        constexpr uint32_t kFaceVertex = sym::dm2::vertexWorld;
        constexpr uint32_t kEdgeList = sym::dm2::g_taggedEdges;
        void probeGrinds(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const char *spec = std::getenv("ZX_DM2_XBOX_LEVEL_GRINDS");
            if (state().grindsProbed || !spec || !readU32(rdram, kScene) || readU32(rdram, kLevelFrame) < 300u)
                return;
            state().grindsProbed = true;
            std::string from, to;
            if (!splitPathPair(spec, from, to))
            {
                std::cerr << "[xboxlevel] ZX_DM2_XBOX_LEVEL_GRINDS=<edges>:<results>, not " << spec << std::endl;
                return;
            }
            std::ifstream in(from);
            std::ofstream out(to);
            const uint32_t base = (getRegU32(ctx, 29) - 0x800u) & ~15u;
            const uint32_t point = base, vertex = base + 0x10u, calleeSp = base - 0x100u;
            size_t edges = 0, found = 0, near = 0;
            std::string line;
            const auto start = std::chrono::steady_clock::now();
            while (std::getline(in, line))
            {
                float a[3], b[3];
                if (std::sscanf(line.c_str(), "%f %f %f %f %f %f", &a[0], &a[1], &a[2], &b[0], &b[1], &b[2]) != 6)
                    continue;
                ++edges;
                for (int i = 0; i < 3; ++i)
                    writeFloat(rdram, point + 4u * i, (a[i] + b[i]) * 0.5f);
                writeFloat(rdram, point + 12u, 1.0f);
                const float savedF12 = ctx->f[12];
                ctx->f[12] = 0.25f;
                uint32_t list = 0;
                runtime->callGuestFunction(rdram, ctx, kEdgeSearch, {point}, calleeSp, &list);
                ctx->f[12] = savedF12;
                bool match = false, any = false;
                for (uint32_t at = list; list && !match && at < kEdgeList + 256u * 8u; at += 8u)
                {
                    const uint32_t object = readU32(rdram, at), high = readU32(rdram, at + 4u);
                    if (!object && !high)
                        break;
                    any = true;
                    float ends[2][3];
                    for (uint32_t k = 0; k < 2; ++k)
                    {
                        runtime->callGuestFunction(rdram, ctx, kFaceVertex,
                                                   {object, high & 0xFFFFFFu, ((high >> 24) + k) % 3u, vertex}, calleeSp);
                        for (int i = 0; i < 3; ++i)
                            ends[k][i] = readFloat(rdram, vertex + 4u * i);
                    }
                    auto same = [](const float *p, const float *q)
                    { return std::fabs(p[0] - q[0]) < 0.01f && std::fabs(p[1] - q[1]) < 0.01f && std::fabs(p[2] - q[2]) < 0.01f; };
                    match = (same(ends[0], a) && same(ends[1], b)) || (same(ends[0], b) && same(ends[1], a));
                }
                found += match;
                near += !match && any;
                out << line << " " << (match ? "found" : any ? "near" : "none") << "\n";
            }
            std::cout << "[xboxlevel] grind probe: " << edges << " grind edges, " << found << " found by DM2's grind search, "
                      << near << " with another grindable edge there, " << edges - found - near << " none; "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s" << std::endl;
        }

        void drawRigidHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            if (!st.probed && active())
                probeCollision(rdram, ctx, runtime);
            if (!st.grindsProbed && active())
                probeGrinds(rdram, ctx, runtime);
            const uint32_t part = readU32(rdram, getRegU32(ctx, 4) + 0x17Cu);
            ++st.rigidDraws;
            {
                std::lock_guard<std::mutex> lock(st.partsMutex);
                const auto it = st.partIndex.find(part);
                if (it != st.partIndex.end() && !st.partsDrawn.count(part))
                {
                    uint16_t faces = 0;
                    if (const uint8_t *p = getConstMemPtr(rdram, part + 0x36u))
                        std::memcpy(&faces, p, 2);
                    st.partsDrawn[part] = faces;
                    st.firstDraws.push_back(it->second);
                }
                if (it != st.partIndex.end())
                    st.frameDraws.insert(it->second);
            }
            st.drawRigid(rdram, ctx, runtime);
        }
        // The skinned renderer (drawSkinned: node, cull, packet), the
        // rider's: calls per second with the trace.
        constexpr uint32_t kDrawSkinned = sym::dm2::drawSkinned;
        void drawSkinnedHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            ++st.skinnedDraws;
            st.riderPacketAt = readU32(rdram, getRegU32(ctx, 6)) & 0x0FFFFFFFu;
            st.lastSkinnedNode = getRegU32(ctx, 4);
            st.drawSkinned(rdram, ctx, runtime);
        }
        void traceVsync(uint8_t *, uint64_t vsync)
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.partsMutex);
            const uint64_t generation = st.runtime->memory().getVU1CodeGeneration();
            if (generation != st.vu1Generation && !st.partIndex.empty())
            {
                std::cout << "[xboxlevel] VU1 micro memory written (generation " << generation << "); parts first drawn:";
                for (uint32_t i : st.lastFirstDraws)
                    std::cout << " " << i;
                std::cout << " |";
                for (uint32_t i : st.firstDraws)
                    std::cout << " " << i;
                std::cout << std::endl;
                std::set<uint32_t> drawn = st.frameDraws;
                drawn.insert(st.lastFrameDraws.begin(), st.lastFrameDraws.end());
                if (!st.suspected)
                    st.suspects = drawn;
                else
                    for (auto it = st.suspects.begin(); it != st.suspects.end();)
                        it = drawn.count(*it) ? std::next(it) : st.suspects.erase(it);
                st.suspected = true;
                std::cout << "[xboxlevel] parts drawn at every VU1 write: " << st.suspects.size() << ":";
                size_t shown = 0;
                for (uint32_t i : st.suspects)
                    if (shown++ < 40)
                        std::cout << " " << i;
                std::cout << std::endl;
            }
            st.lastFrameDraws.swap(st.frameDraws);
            st.frameDraws.clear();
            st.vu1Generation = generation;
            st.lastFirstDraws.swap(st.firstDraws);
            st.firstDraws.clear();
            if (vsync % 60u == 0u)
            {
                std::cout << "[xboxlevel] per second: " << st.rigidDraws.exchange(0) << " rigid, " << st.skinnedDraws.exchange(0)
                          << " skinned node draws; skinned node 0x" << std::hex << st.lastSkinnedNode.load() << std::dec
                          << std::endl;
            }
            if (vsync % 300u == 0u && !st.partIndex.empty())
            {
                uint64_t faces = 0;
                for (const auto &kv : st.partsDrawn)
                    faces += kv.second;
                std::cout << "[xboxlevel] drawn: " << st.partsDrawn.size() << " of " << st.partIndex.size() << " parts, "
                          << faces << " faces" << std::endl;
            }
        }

        // With the trace: DM2's allocator (allocateIn: size, kind 0-4)
        // failing while the level loads or runs.
        constexpr uint32_t kAllocate = sym::dm2::allocateIn;
        void allocateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t size = getRegU32(ctx, 4), kind = getRegU32(ctx, 5);
            state().allocate(rdram, ctx, runtime);
            if (getRegU32(ctx, 2) == 0u && active() && state().failedAllocations++ < 20u)
                std::cout << "[xboxlevel] allocation of " << size << " bytes (kind " << kind << ") failed" << std::endl;
        }

        // The draw buckets: initDrawBuckets makes seven arrays of 16-byte
        // entries {cull, node, 0, handler} at boot, of the capacities at
        // 0x1FB368 (512 opaque, 128, 256 blended, 64, 64 skinned, 32, 32);
        // each bucket's {start, next, end, flush handler} at 0x25CF14 +
        // 16 i. The scene task queues every visible node with queueNodeTree
        // and nothing checks the end: DM2's levels queue about 75 nodes a
        // frame (Woodward Camp); Aggressive Inline's Movie Lot, once served
        // here, queued its 1,440 instances over 500, which wrote past bucket
        // 0 into the heap after it (the rider's draws lost, stray VIF codes
        // from the entries' pointers). While an extra level is chosen,
        // resetDrawBuckets (the frame's reset of every
        // bucket's next to its start) finds buckets eight times as large in
        // extended memory; DM2's come back the same way afterwards. The
        // extended memory is added at the first use, after DM2 took its
        // heap at boot, so DM2's own layout is unchanged.
        constexpr uint32_t kBucketReset = sym::dm2::resetDrawBuckets;
        constexpr uint32_t kBuckets = sym::dm2::drawBuckets;
        constexpr uint32_t kBucketCapacities = sym::dm2::bucketCapacities;
        constexpr uint32_t kBucketGrowth = 8u;
        // The extended memory, added once (the draw buckets here, the Xbox
        // levels' career strings in dave_mirra_xcareer.cpp).
        bool ensureExtended(PS2Machine *runtime)
        {
            State &st = state();
            if (!st.extendedAdded)
                st.extendedAdded = runtime->addGuestHeapRegion(PS2_LEGACY_RAM_SIZE, PS2_RAM_SIZE);
            return st.extendedAdded;
        }
        void bucketResetHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            // The frame just drawn: entries per bucket (the trace's peaks).
            for (uint32_t i = 0; i < kBucketCount; ++i)
            {
                const uint32_t start = readU32(rdram, kBuckets + i * 16u), next = readU32(rdram, kBuckets + i * 16u + 4u);
                const uint32_t used = next > start ? (next - start) / 16u : 0u;
                if (used > st.bucketPeak[i] && used < (1u << 20))
                {
                    st.bucketPeak[i] = used;
                    if (st.trace)
                        std::cout << "[xboxlevel] draw bucket " << i << ": " << used << " entries (DM2's capacity "
                                  << readU32(rdram, kBucketCapacities + i * 4u) << ")" << std::endl;
                }
            }
            if (active() && !st.bucketsSwapped)
            {
                bool ready = ensureExtended(runtime);
                for (uint32_t i = 0; ready && i < kBucketCount; ++i)
                    if (!st.bigBuckets[i])
                        ready = (st.bigBuckets[i] = runtime->guestMallocExtended(
                                     readU32(rdram, kBucketCapacities + i * 4u) * kBucketGrowth * 16u, 64u)) != 0u;
                if (ready)
                {
                    for (uint32_t i = 0; i < kBucketCount; ++i)
                    {
                        const uint32_t at = kBuckets + i * 16u;
                        st.gameBuckets[i][0] = readU32(rdram, at);
                        st.gameBuckets[i][1] = readU32(rdram, at + 8u);
                        writeU32(rdram, at, st.bigBuckets[i]);
                        writeU32(rdram, at + 8u,
                                 st.bigBuckets[i] + readU32(rdram, kBucketCapacities + i * 4u) * kBucketGrowth * 16u);
                    }
                    st.bucketsSwapped = true;
                }
                else
                    std::cerr << "[xboxlevel] no extended memory for the draw buckets: the level overruns them" << std::endl;
            }
            else if (!active() && st.bucketsSwapped)
            {
                for (uint32_t i = 0; i < kBucketCount; ++i)
                {
                    writeU32(rdram, kBuckets + i * 16u, st.gameBuckets[i][0]);
                    writeU32(rdram, kBuckets + i * 16u + 8u, st.gameBuckets[i][1]);
                }
                st.bucketsSwapped = false;
            }
            st.bucketReset(rdram, ctx, runtime);
        }

        // With the trace: DM2's GS texture cache. Textures are uploaded per
        // node just before it is drawn (the VIF1 MARK handler vif1Interrupt),
        // their mip levels allocated upwards from the base 0x25CD34
        // (placeTextureLevel, in words; a page boundary when the pixel format
        // changes), their CLUTs downwards from 0x25CD38 (placeTextureClut, 0x100
        // words each). The frame end advanceTextureCache flushes the whole cache
        // (flushTextureCache: every texture to be sent again) when less than
        // 0x18000 words are left. 0x25CD44 is set when the two meet.
        constexpr uint32_t kTexFrameEnd = sym::dm2::advanceTextureCache;
        constexpr uint32_t kTexFlush = sym::dm2::flushTextureCache;
        constexpr uint32_t kTexMarkIrq = sym::dm2::vif1Interrupt;
        // (zge/render/texture.h TextureCache's poolBottom, poolTop, low, high and overflow.)
        constexpr uint32_t kTexBase = sym::dm2::g_textureCache + 0x08u, kTexTopStart = sym::dm2::g_textureCache + 0x04u;
        constexpr uint32_t kTexBottom = sym::dm2::g_textureCache + 0x14u, kTexTop = sym::dm2::g_textureCache + 0x18u,
                           kTexOverflow = sym::dm2::g_textureCache + 0x24u;
        // The frame's VIF1 packet (two buffers of 0x20000 bytes at 0x25CDA8,
        // the packet's pointer at 0x25CDC0) and the node list of the MARK
        // handler (two lists of 0xC00 bytes at 0x25CDE0, the next entry at
        // 0x25CDE8, the list in use 0x25CDB0).
        // (zge/render/renderer.h, DM2's Renderer at g_renderer: frameBuffers, frame, meshPacket's current,
        // requestQueues and requestQueueEnd.)
        constexpr uint32_t kPacketBuffers = sym::dm2::g_renderer + 0x08u, kPacketParity = sym::dm2::g_renderer + 0x10u,
                           kPacketNext = sym::dm2::g_renderer + 0x20u;
        constexpr uint32_t kMarkLists = sym::dm2::g_renderer + 0x40u, kMarkNext = sym::dm2::g_renderer + 0x48u;
        // The frame's DMA chain walked as the DMAC walks it: its tags, and
        // the tag at which the rider's packet is reached. The runtime used to
        // stop a chain after 4,096 tags (a loop guard); every frame whose
        // rider came later lost it.
        void walkFrameChain(const uint8_t *rdram, uint32_t start, uint32_t end)
        {
            State &st = state();
            const uint32_t rider = st.riderPacketAt.exchange(0);
            uint32_t tadr = start, tags = 0, riderTag = 0, stack[2] = {}, depth = 0;
            while (tags < (1u << 20))
            {
                const uint32_t lo = readU32(rdram, tadr), hi = readU32(rdram, tadr + 4u);
                ++tags;
                if (!riderTag && rider && tadr >= rider && tadr >= start && tadr < end)
                    riderTag = tags;
                const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u, addr = hi & 0x7FFFFFF0u;
                bool last = false;
                switch (id)
                {
                case 1: tadr += 16u + qwc * 16u; break;
                case 2: tadr = addr; break;
                case 3: case 4: tadr += 16u; break;
                case 5:
                    if (depth < 2u)
                        stack[depth++] = tadr + 16u + qwc * 16u;
                    tadr = addr;
                    break;
                case 6:
                    if (depth)
                        tadr = stack[--depth];
                    else
                        last = true;
                    break;
                default: last = true; break;
                }
                if (last || (lo >> 31))
                    break;
            }
            st.tagPeak = std::max(st.tagPeak, tags);
            st.tagTotal += tags;
            if (tags > 4096u)
            {
                ++st.framesOver;
                ++st.allOver;
            }
            if (riderTag)
            {
                ++st.riderFrames;
                ++st.allRiderFrames;
                if (riderTag > 4096u)
                {
                    ++st.riderCut;
                    ++st.allRiderCut;
                }
            }
        }
        void texFlushHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            ++state().texFlushes;
            state().texFlush(rdram, ctx, runtime);
        }
        // By the MARK value the handler sees (1 the node list, 2 the
        // shadow's; others do nothing): with VIF1 running data as VIFcodes
        // (a part's CALL into vertex data, see dm2_xbox.py
        // drop_unused_materials) about 2,000 a frame had garbage MARKs.
        void texMarkIrqHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            ++st.texIrqs;
            const uint32_t mark = runtime->memory().readIORegister(0x10003C30u);
            ++st.markIrqs[std::min(mark, 3u)];
            st.texMarkIrq(rdram, ctx, runtime);
        }
        void texFrameEndHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const uint32_t base = readU32(rdram, kTexBase), topStart = readU32(rdram, kTexTopStart);
            const uint32_t bottom = readU32(rdram, kTexBottom), top = readU32(rdram, kTexTop);
            const bool overflow = readU32(rdram, kTexOverflow) != 0u || bottom > top;
            if (active())
            {
                const uint32_t parity = readU32(rdram, kPacketParity) & 1u;
                const uint32_t packetStart = readU32(rdram, kPacketBuffers + parity * 4u) & 0x0FFFFFFFu;
                const uint32_t packetNext = readU32(rdram, kPacketNext) & 0x0FFFFFFFu;
                if (packetNext >= packetStart)
                    st.packetPeak = std::max(st.packetPeak, packetNext - packetStart);
                const uint32_t markStart = readU32(rdram, kMarkLists + parity * 4u) & 0x0FFFFFFFu;
                const uint32_t markNext = readU32(rdram, kMarkNext) & 0x0FFFFFFFu;
                if (markNext >= markStart)
                {
                    st.markPeak = std::max(st.markPeak, (markNext - markStart) / 4u);
                    st.markTotal += (markNext - markStart) / 4u;
                }
                walkFrameChain(rdram, packetStart, packetStart + 0x20000u);
                const uint32_t generation = readU32(rdram, sym::dm2::g_textureEpoch);
                if (generation != st.lastGeneration)
                    ++st.generationChanges;
                st.lastGeneration = generation;
                ++st.texFrames;
                ++st.texAllFrames;
                if (overflow)
                {
                    ++st.texOverflowFrames;
                    ++st.texAllOverflow;
                }
                const uint32_t use = (bottom - base) + (topStart - top);
                st.texPeakUse = std::max(st.texPeakUse, use);
                st.texPeakBottom = std::max(st.texPeakBottom, bottom);
                // This frame's uploads (the cache was not flushed in it).
                if (bottom >= st.texFrameStartBottom && top <= st.texFrameStartTop)
                    st.texPeakFrameUse = std::max(st.texPeakFrameUse, (bottom - st.texFrameStartBottom) +
                                                                        (st.texFrameStartTop - top));
                const uint32_t irqs = st.texIrqs.exchange(0);
                st.texIrqPeak = std::max(st.texIrqPeak, irqs);
                st.texIrqTotal += irqs;
                if (st.texFrames == 60u)
                {
                    std::cout << "[xboxlevel] texture cache, 60 frames: " << st.texOverflowFrames << " overflowed, "
                              << st.texFlushes << " flushes, peak use 0x" << std::hex << st.texPeakUse << " of 0x"
                              << (topStart - base) << " words, peak base 0x" << st.texPeakBottom << ", one frame's peak 0x"
                              << st.texPeakFrameUse << std::dec << "; MARK interrupts " << st.texIrqTotal / 60u
                              << " a frame (peak " << st.texIrqPeak << "); all frames " << st.texAllFrames
                              << ", overflowed " << st.texAllOverflow << std::endl;
                    std::cout << "[xboxlevel] frame packet peak 0x" << std::hex << st.packetPeak << std::dec
                              << " bytes (DM2's buffer 0x20000); MARK node list " << st.markTotal / 60u
                              << " a frame, peak " << st.markPeak << " (DM2's list 768); texture generation changed in "
                              << st.generationChanges << " frames; interrupts by MARK 0/1/2/other: "
                              << st.markIrqs[0].exchange(0) << "/" << st.markIrqs[1].exchange(0) << "/"
                              << st.markIrqs[2].exchange(0) << "/" << st.markIrqs[3].exchange(0) << std::endl;
                    std::cout << "[xboxlevel] DMA chain: " << st.tagTotal / 60u << " tags a frame, peak " << st.tagPeak
                              << "; over 4,096 in " << st.framesOver << " of 60 frames; rider's packet past tag 4,096 in "
                              << st.riderCut << " of " << st.riderFrames << " (all frames: " << st.allOver << " over, rider past in "
                              << st.allRiderCut << " of " << st.allRiderFrames << ")" << std::endl;
                    st.tagPeak = st.framesOver = st.riderCut = st.riderFrames = 0;
                    st.tagTotal = 0;
                    st.packetPeak = st.markPeak = st.generationChanges = 0;
                    st.markTotal = 0;
                    st.texFrames = st.texOverflowFrames = st.texFlushes = st.texPeakUse = st.texPeakBottom = 0;
                    st.texPeakFrameUse = st.texIrqPeak = 0;
                    st.texIrqTotal = 0;
                }
            }
            st.texFrameEnd(rdram, ctx, runtime);
            st.texFrameStartBottom = readU32(rdram, kTexBottom);
            st.texFrameStartTop = readU32(rdram, kTexTop);
        }

        // ---- Ridden checks (developers): ZX_DM2_RIDES=<trials>:<results> ----
        //
        // In any level (DM2's own too), once it has run 300 frames: for each
        // line of <trials>, the rider is put at a start by DM2's own
        // placement, spawnPlayer (player, 1: what a restart does; it looks
        // up the level's "PlayerOne" spawn point with locatorMatrix, whose
        // answer is ours for that call), then ridden by the pad (the game
        // reads the buttons set here as a pad's, PSPadBackend::
        // setOverrideButtons) and watched frame by frame:
        //   grind ax ay az bx by bz [...]   a rail (dm2_xbox.py edges): the
        //       start 10 units (8, 6 if the ground or a wall is in the way)
        //       back from its middle at 30 degrees to it, on whichever side
        //       has a clear run-up (DM2's raycast sceneQuery); pedal, steer
        //       to the middle, crouch, hop, hold grind. "grind" when the
        //       rider's state (+0xFF0) is 4, with whether the edge he grinds
        //       (+0x1120) is this rail's.
        //   qpipe x z y dx dz top [...]     a quarter pipe (games/aggressive/tools/qpride.py
        //       find): the start and the heading to the pipe; pedal, keep the
        //       heading; the air: frames, height over the take-off and over
        //       the face's middle (top), how far past the take-off, the
        //       landing.
        //   ride x z y dx dz frames         pedal along a heading for that many
        //       frames, whatever lies ahead (a drop into water, a hazard):
        //       the surface sound under each wheel (rider +0x1560 + 0x80 *
        //       wheel + 0x70, which groundImpact writes from the face's
        //       material), the states, a crash's bail kind (+0x178), the
        //       lowest point and a respawn (a jump of 3 units or more in a
        //       frame); with the trace, every frame.
        // Results: the line and what happened. ZX_DM2_RIDES_OPTS
        // "key=value,..." tunes the pad: pedal, jump, grind (button names),
        // crouch, hop (distances to the rail), steer (1 / -1: the sign of
        // left), timeout (frames). ZX_DM2_RIDES_TRACE=1 logs every frame.
        namespace rides
        {
            constexpr uint32_t kPlace = sym::dm2::spawnPlayer;       // spawnPlayer(player, reset)
            constexpr uint32_t kSpawnLookup = sym::dm2::locatorMatrix; // locatorMatrix(near, name hash, matrix) -> found
            constexpr uint32_t kRiderHandle = sym::dm2::riderIndexOf; // riderIndexOf(player) -> handle
            constexpr uint32_t kRiders = sym::dm2::g_riders;      // u32 per handle: the rider
            constexpr uint32_t kPlayer = sym::dm2::g_currentPlayer;      // u8, the current player
            constexpr uint32_t kRaycastFn = sym::dm2::sceneQuery;
            constexpr uint32_t kHit = sym::dm2::g_queryState + 0x810u + 0x10u; // the first face hit's point
            constexpr uint32_t kSceneRoot = sym::dm2::g_collisionWorld;
            constexpr uint32_t kFrame = sym::dm2::g_physicsTaskFrames;
            constexpr uint32_t kFaceVertexFn = sym::dm2::vertexWorld;
            constexpr uint32_t kFrameReset = sym::dm2::resetDrawBuckets;
            // The rider.
            constexpr uint32_t kState = 0xFF0u, kRight = 0x1000u, kUp = 0x1010u, kForward = 0x1020u, kPosition = 0x1040u,
                               kVelocity = 0x10A0u, kGrindEdge = 0x1120u;
            // Per wheel (0x80 apart): the surface sound of the face under it
            // (groundImpact stores faceSound's answer, an integer, though
            // its decompile reads it as a float); the bail kind of a crash.
            constexpr uint32_t kWheelSurface = 0x1560u + 0x70u, kBailKind = 0x178u;

            struct Vec
            {
                float x = 0, y = 0, z = 0;
            };
            Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
            Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
            Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
            float flatDot(Vec a, Vec b) { return a.x * b.x + a.z * b.z; }
            float flatLength(Vec a) { return std::sqrt(a.x * a.x + a.z * a.z); }
            Vec flatUnit(Vec a)
            {
                const float l = flatLength(a);
                return l > 1e-6f ? Vec{a.x / l, 0.0f, a.z / l} : Vec{0.0f, 0.0f, 1.0f};
            }

            struct Trial
            {
                std::string line;
                bool grind = false;
                Vec a, b;               // grind: the rail's ends
                Vec start, heading;     // qpipe, ride: the start and the way; grind: chosen
                bool ride = false;      // ride: pedal along the heading for `top` frames
                float top = 0.0f;
                Vec railNormal;         // grind: across the rail, toward the start
                int attempt = 0;        // grind: 0 a late hop, 1 (if 0 fails) an early one
                float railHeight = 0.0f; // grind: the rail over the run-up's ground
                std::string earlier;    // grind: the first attempt's result
            };
            struct Options
            {
                uint16_t pedal = 1u << 4, jump = 1u << 14, grindButton = 1u << 12, left = 1u << 7, right = 1u << 5;
                float crouch = 3.0f, hop = 1.6f, steer = 1.0f;
                uint32_t timeout = 480u, settle = 30u;
            };

            enum class Phase
            {
                Waiting,
                Settling,
                Riding,
                Done
            };

            // This trial's ride.
            struct Ride
            {
                uint32_t frames = 0;
                bool hopped = false, crouched = false;
                int grindFrame = -1, firstAir = -1, landedFrame = -1;
                Vec takeOff, landing;
                float apex = 1e9f, hopDistance = 0.0f, hopSpeed = 0.0f, closest = 1e9f;
                uint64_t edge = 0;
                std::string states; // "state:frames ..."
                int lastState = -1;
                uint32_t stateRun = 0;
                uint32_t airFrames = 0;
                float speedMax = 0.0f, placedOff = 0.0f;
                int airState = -1;
                bool crashed = false;
                float startY = 0.0f;
                // ride trials
                std::string surfaces; // "sound:frames ..." under wheel 0 while riding
                int lastSurface = -2;
                uint32_t surfaceRun = 0;
                int crashFrame = -1, bailKind = -1, respawnFrame = -1;
                Vec crashAt, respawnFrom, respawnTo, last, lowest, startPos;
                bool haveLast = false;
            };

            // One boot's trials and where they are (developers;
            // zx::boot::PerBoot).
            struct RidesState
            {
                Options opts;
                std::vector<Trial> trials;
                size_t next = 0;
                Phase phase = Phase::Waiting;
                std::ofstream out;
                bool traceRides = false;
                PS2Machine::RecompiledFunction frameReset = nullptr, spawnLookup = nullptr, respawnSearch = nullptr;
                // The placement's answer to the spawn lookup (16 floats), while placing.
                bool placing = false;
                float matrix[16] = {};
                uint32_t playerOneHash = 0;
                uint16_t buttons = 0;
                Ride ride; // this trial's
                size_t grinds = 0, grindsOnRail = 0, grindTrials = 0, skipped = 0, firstTry = 0;
            };

            RidesState &ridesState()
            {
                static zx::boot::PerBoot<std::unique_ptr<RidesState>> ofBoot([] { return std::make_unique<RidesState>(); });
                return *ofBoot.get();
            }

            uint32_t readU32At(const uint8_t *rdram, uint32_t a) { return readU32(rdram, a); }
            Vec readVec(const uint8_t *rdram, uint32_t a)
            {
                return {readFloat(rdram, a), readFloat(rdram, a + 4u), readFloat(rdram, a + 8u)};
            }

            uint16_t buttonBit(const std::string &name)
            {
                static const char *const names[16] = {"select", "l3",  "r3", "start", "up",       "right",  "down",  "left",
                                                      "l2",     "r2",  "l1", "r1",    "triangle", "circle", "cross", "square"};
                for (int i = 0; i < 16; ++i)
                    if (name == names[i])
                        return static_cast<uint16_t>(1u << i);
                std::cerr << "[rides] no button " << name << std::endl;
                return 0;
            }
            void readOptions()
            {
                RidesState &rs = ridesState();
                const char *text = std::getenv("ZX_DM2_RIDES_OPTS");
                if (!text)
                    return;
                std::string all(text);
                size_t at = 0;
                while (at < all.size())
                {
                    size_t comma = all.find(',', at);
                    if (comma == std::string::npos)
                        comma = all.size();
                    const std::string item = all.substr(at, comma - at);
                    at = comma + 1u;
                    const size_t eq = item.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const std::string key = item.substr(0, eq), value = item.substr(eq + 1u);
                    if (key == "pedal")
                        rs.opts.pedal = buttonBit(value);
                    else if (key == "jump")
                        rs.opts.jump = buttonBit(value);
                    else if (key == "grind")
                        rs.opts.grindButton = buttonBit(value);
                    else if (key == "crouch")
                        rs.opts.crouch = std::strtof(value.c_str(), nullptr);
                    else if (key == "hop")
                        rs.opts.hop = std::strtof(value.c_str(), nullptr);
                    else if (key == "steer")
                        rs.opts.steer = std::strtof(value.c_str(), nullptr);
                    else if (key == "timeout")
                        rs.opts.timeout = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
                    else if (key == "settle")
                        rs.opts.settle = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
                }
            }

            bool readTrials(const std::string &path)
            {
                std::ifstream in(path);
                std::string line;
                while (std::getline(in, line))
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    Trial t;
                    t.line = line;
                    char kind[16] = {};
                    float v[7] = {};
                    if (std::sscanf(line.c_str(), "%15s %f %f %f %f %f %f", kind, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7)
                        continue;
                    if (std::string(kind) == "grind")
                    {
                        t.grind = true;
                        t.a = {v[0], v[1], v[2]};
                        t.b = {v[3], v[4], v[5]};
                    }
                    else if (std::string(kind) == "qpipe" || std::string(kind) == "ride")
                    {
                        t.ride = std::string(kind) == "ride";
                        t.start = {v[0], v[2], v[1]};
                        t.heading = flatUnit({v[3], 0.0f, v[4]});
                        t.top = v[5];
                    }
                    else
                        continue;
                    ridesState().trials.push_back(t);
                }
                return !ridesState().trials.empty();
            }

            uint32_t rider(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t sp)
            {
                uint32_t handle = 0;
                const uint8_t *player = getConstMemPtr(rdram, kPlayer);
                runtime->callGuestFunction(rdram, ctx, kRiderHandle, {player ? *player : 0u}, sp, &handle);
                return readU32(rdram, kRiders + 4u * (handle & 0xFFu));
            }

            // DM2's raycast on the segment p -> q (+y is down): the nearest hit.
            bool raycast(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t base, Vec p, Vec q, Vec *hit)
            {
                const uint32_t segment = base, hits = base + 0x20u, sp = base - 0x100u;
                const float s[8] = {p.x, p.y, p.z, 1.0f, q.x, q.y, q.z, 1.0f};
                for (int i = 0; i < 8; ++i)
                    writeFloat(rdram, segment + 4u * static_cast<uint32_t>(i), s[i]);
                writeU32(rdram, hits, 0u);
                writeU32(rdram, hits + 4u, 0u);
                uint32_t v0 = 0;
                runtime->callGuestFunction(rdram, ctx, kRaycastFn, {1u, segment, 4u, 0u, 0u, 0u, hits, 0u}, sp, &v0);
                if (v0 && hit)
                    *hit = readVec(rdram, kHit);
                return v0 != 0u;
            }
            bool groundAt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t base, float x, float z, float above,
                          float *y)
            {
                Vec hit;
                if (!raycast(rdram, ctx, runtime, base, {x, above, z}, {x, 500.0f, z}, &hit))
                    return false;
                *y = hit.y;
                return true;
            }

            // The run-up from start (ground y0) to near: flat ground (within
            // 0.5) under every unit of it, nothing in the way at knee and
            // head height.
            bool runUpClear(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t base, Vec start, float y0, Vec near,
                            float yNear, std::string &why)
            {
                const Vec dir = flatUnit(near - start);
                const float distance = flatLength(near - start);
                for (float s = 0.0f; s <= distance; s += 1.0f)
                {
                    const Vec p = start + dir * s;
                    float y = 0.0f;
                    if (!groundAt(rdram, ctx, runtime, base, p.x, p.z, y0 - 1.5f, &y) || std::fabs(y - y0) >= 0.5f)
                    {
                        why = "run-up not flat";
                        return false;
                    }
                }
                for (float h : {0.6f, 1.4f})
                {
                    const Vec p{start.x, y0 - h, start.z}, q{near.x, yNear - h, near.z};
                    if (raycast(rdram, ctx, runtime, base, p, q, nullptr))
                    {
                        why = "something in the way";
                        return false;
                    }
                }
                return true;
            }

            // A grind trial's start: back from the rail's middle at 30 degrees
            // to it, on a side whose run-up is flat ground with nothing in the
            // way and whose rail stands at most 1.6 over the ground by it.
            bool chooseApproach(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t base, Trial &t,
                                std::string &why)
            {
                int skip = 0; // the first approach that qualifies (both attempts)
                const Vec mid = (t.a + t.b) * 0.5f;
                const Vec along = flatUnit(t.b - t.a);
                const Vec across{-along.z, 0.0f, along.x};
                const float sin30 = 0.5f, cos30 = 0.8660254f;
                why = "no side";
                for (float distance : {10.0f, 8.0f, 6.0f})
                    for (float side : {1.0f, -1.0f})
                        for (float slant : {1.0f, -1.0f})
                        {
                            // From the start toward the middle.
                            const Vec dir = flatUnit(across * (-side * cos30) + along * (slant * sin30));
                            const Vec start = mid - dir * distance;
                            float y0 = 0.0f;
                            if (!groundAt(rdram, ctx, runtime, base, start.x, start.z, mid.y - 3.0f, &y0))
                            {
                                why = "no ground at the start";
                                continue;
                            }
                            // The rail over the ground beside it (+y down).
                            const Vec near = mid - dir * 1.0f;
                            float yNear = 0.0f;
                            if (!groundAt(rdram, ctx, runtime, base, near.x, near.z, mid.y - 0.05f, &yNear))
                            {
                                why = "no ground by the rail";
                                continue;
                            }
                            // The ground by the rail is the run-up's (not a
                            // platform the rail stands on), the rail at most 1.6
                            // over it.
                            const float railHeight = yNear - mid.y;
                            if (std::fabs(yNear - y0) > 0.5f)
                            {
                                why = "the rail's ground is not the run-up's";
                                continue;
                            }
                            // Under 0.3 the rail is flush with the ground (a
                            // curb, a platform's edge): a hop passes over it.
                            if (railHeight < 0.3f || railHeight > 1.6f)
                            {
                                why = railHeight < 0.3f ? "flush with the ground" : "rail height " + std::to_string(railHeight);
                                continue;
                            }
                            if (!runUpClear(rdram, ctx, runtime, base, {start.x, y0, start.z}, y0, near, yNear, why))
                                continue;
                            if (skip-- > 0)
                                continue;
                            t.railHeight = railHeight;
                            t.start = {start.x, y0, start.z};
                            t.heading = dir;
                            t.railNormal = across * side; // points from the rail toward the start side
                            return true;
                        }
                return false;
            }

            // locatorMatrix, the spawn lookup: while placing, "PlayerOne" is
            // ours, turned as the level's own (same handedness).
            // With the trace: DM2's respawn search respawnAtStart(point, flags)
            // and what it found (a face handle; 0: none).
            constexpr uint32_t kRespawnSearch = sym::dm2::respawnAtStart;
            void respawnSearchHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                const Vec p = readVec(rdram, getRegU32(ctx, 4));
                const uint32_t flags = getRegU32(ctx, 5);
                ridesState().respawnSearch(rdram, ctx, runtime);
                std::cout << "[rides] respawn search at " << p.x << " " << p.y << " " << p.z << " flags " << flags
                          << ": face 0x" << std::hex << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 1)) << " 0x"
                          << getRegU32(ctx, 2) << std::dec
                          << std::endl;
            }

            void spawnLookupHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                RidesState &rs = ridesState();
                const uint32_t hash = getRegU32(ctx, 5), out = getRegU32(ctx, 6);
                const Vec near = readVec(rdram, getRegU32(ctx, 4));
                rs.spawnLookup(rdram, ctx, runtime);
                if (rs.traceRides && !rs.placing)
                    std::cout << "[rides] spawn lookup 0x" << std::hex << hash << std::dec << " near " << near.x << " "
                              << near.y << " " << near.z << ": " << (getRegU32(ctx, 2) ? "found " : "none ")
                              << readFloat(rdram, out + 0x30u) << " " << readFloat(rdram, out + 0x34u) << " "
                              << readFloat(rdram, out + 0x38u) << std::endl;
                if (!rs.placing || hash != rs.playerOneHash)
                    return;
                const bool found = getRegU32(ctx, 2) != 0u;
                // The level's own rows: right, up, forward.
                float hand = 1.0f;
                if (found)
                {
                    const Vec r = readVec(rdram, out), u = readVec(rdram, out + 16u), f = readVec(rdram, out + 32u);
                    const Vec c{u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
                    hand = c.x * r.x + c.y * r.y + c.z * r.z >= 0.0f ? 1.0f : -1.0f;
                }
                const Vec f = rs.trials[rs.next].heading;
                const Vec u{0.0f, -1.0f, 0.0f};
                const Vec r{hand * (u.y * f.z - u.z * f.y), hand * (u.z * f.x - u.x * f.z), hand * (u.x * f.y - u.y * f.x)};
                const Vec p = rs.trials[rs.next].start;
                const float m[16] = {r.x, r.y, r.z, 0.0f, u.x, u.y, u.z, 0.0f, f.x, f.y, f.z, 0.0f, p.x, p.y - 0.05f, p.z, 1.0f};
                for (int i = 0; i < 16; ++i)
                    writeFloat(rdram, out + 4u * static_cast<uint32_t>(i), m[i]);
                setReturnU32(ctx, 1u);
            }

            void finishTrial(const char *result)
            {
                RidesState &rs = ridesState();
                rs.out << rs.trials[rs.next].line << " | " << result << "\n";
                rs.out.flush();
                ++rs.next;
                rs.phase = rs.next < rs.trials.size() ? Phase::Waiting : Phase::Done;
                rs.buttons = 0;
                PSPadBackend::setOverrideButtons(0);
                if (rs.phase == Phase::Done)
                {
                    std::cout << "[rides] done: " << rs.trials.size() << " trials; rails " << rs.grindTrials
                              << ", skipped (no run-up) " << rs.skipped << ", ridden " << rs.grindTrials - rs.skipped
                              << "; a grind started on " << rs.grinds << " (with the late hop " << rs.firstTry
                              << "), on the rail tried " << rs.grindsOnRail << std::endl;
                    rs.out.close();
                }
            }

            // A grind trial without a grind gets its second approach.
            void finishGrind(bool grind, const char *result)
            {
                RidesState &rs = ridesState();
                Trial &t = rs.trials[rs.next];
                if (!grind && t.attempt == 0)
                {
                    t.earlier = result;
                    t.attempt = 1;
                    rs.phase = Phase::Waiting;
                    rs.buttons = 0;
                    PSPadBackend::setOverrideButtons(0);
                    return;
                }
                const std::string text = t.attempt ? "early hop: " + std::string(result) + " || late hop: " + t.earlier
                                                   : std::string(result);
                finishTrial(text.c_str());
            }

            void noteState(int state)
            {
                Ride &r = ridesState().ride;
                if (state != r.lastState)
                {
                    if (r.lastState >= 0 && r.states.size() < 400u)
                        r.states += std::to_string(r.lastState) + ":" + std::to_string(r.stateRun) + " ";
                    r.lastState = state;
                    r.stateRun = 0;
                }
                ++r.stateRun;
            }

            bool sameEdge(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t base, uint64_t edge, const Trial &t)
            {
                const uint32_t object = static_cast<uint32_t>(edge), high = static_cast<uint32_t>(edge >> 32);
                if (!object)
                    return false;
                const uint32_t vertex = base + 0x40u, sp = base - 0x100u;
                Vec ends[2];
                for (uint32_t k = 0; k < 2; ++k)
                {
                    runtime->callGuestFunction(rdram, ctx, kFaceVertexFn, {object, high & 0xFFFFFFu, ((high >> 24) + k) % 3u, vertex},
                                               sp);
                    ends[k] = readVec(rdram, vertex);
                }
                // On the tried rail's line (within 0.5: a box rail's other top
                // edge counts), between its ends (it may be one of several
                // edges making the rail).
                const Vec along = t.b - t.a;
                const float length2 = along.x * along.x + along.y * along.y + along.z * along.z;
                for (const Vec &e : ends)
                {
                    const Vec d = e - t.a;
                    const float s = (d.x * along.x + d.y * along.y + d.z * along.z) / std::max(length2, 1e-6f);
                    const Vec off = d - along * s;
                    if (s < -0.05f || s > 1.05f || off.x * off.x + off.y * off.y + off.z * off.z > 0.5f * 0.5f)
                        return false;
                }
                return true;
            }

            void step(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                RidesState &rs = ridesState();
                if (rs.phase == Phase::Done || !readU32(rdram, kSceneRoot) || readU32(rdram, kFrame) < 300u)
                    return;
                const uint32_t base = (getRegU32(ctx, 29) - 0x900u) & ~15u;
                const uint32_t self = rider(rdram, ctx, runtime, base - 0x100u);
                if (!self)
                    return;
                Trial &t = rs.trials[rs.next];
                char text[1024];
                if (rs.phase == Phase::Waiting)
                {
                    if (t.grind)
                    {
                        rs.grindTrials += t.attempt == 0;
                        std::string why;
                        if (!chooseApproach(rdram, ctx, runtime, base, t, why))
                        {
                            if (t.attempt == 0)
                            {
                                ++rs.skipped;
                                finishTrial(("skip: " + why).c_str());
                            }
                            else
                                finishTrial(("early hop: no approach || late hop: " + t.earlier).c_str());
                            return;
                        }
                    }
                    rs.ride = Ride{};
                    rs.placing = true;
                    const uint8_t *player = getConstMemPtr(rdram, kPlayer);
                    runtime->callGuestFunction(rdram, ctx, kPlace, {player ? *player : 0u, 1u}, base - 0x100u);
                    rs.placing = false;
                    rs.phase = Phase::Settling;
                    PSPadBackend::setOverrideButtons(0);
                    return;
                }
                const Vec pos = readVec(rdram, self + kPosition), vel = readVec(rdram, self + kVelocity);
                const Vec fwd = readVec(rdram, self + kForward);
                const int state = *getConstMemPtr(rdram, self + kState);
                Ride &r = rs.ride;
                ++r.frames;
                if (rs.traceRides)
                    std::cout << "[rides] " << rs.next << " f" << r.frames << " st " << state << " pos " << pos.x << " " << pos.y
                              << " " << pos.z << " vel " << vel.x << " " << vel.y << " " << vel.z << " fwd " << fwd.x << " "
                              << fwd.z << " buttons 0x" << std::hex << rs.buttons << std::dec << " sound "
                              << static_cast<int>(readU32(rdram, self + kWheelSurface)) << " "
                              << static_cast<int>(readU32(rdram, self + kWheelSurface + 0x80u))
                              << " bail " << static_cast<int>(*getConstMemPtr(rdram, self + kBailKind)) << std::endl;
                if (rs.phase == Phase::Settling)
                {
                    // A quarter pipe's run-up may slope: settle only briefly.
                    if (r.frames < (t.grind ? rs.opts.settle : 3u))
                        return;
                    // DM2's placement puts the rider on a 4/3-unit grid near
                    // the spawn point (further when the nearest point is not
                    // free: 2.6 units by Woodward's quarter pipes).
                    // DM2's placement (its respawn search spawnAtPlayer) puts
                    // the rider on the middle of a ground face near the spawn
                    // point: the run-up is checked again from there (on the
                    // rail's side, 4 to 16 away, flat and clear; a pipe's
                    // run-up within 8 of the chosen start), the heading
                    // taken from there.
                    const float off = flatLength(pos - t.start);
                    std::string why = "placed off by " + std::to_string(off);
                    bool ok = false;
                    if (t.grind)
                    {
                        const Vec mid = (t.a + t.b) * 0.5f;
                        const float distance = flatDot(pos - mid, t.railNormal);
                        const Vec dir = flatUnit(mid - pos);
                        const Vec near = mid - dir * 1.0f;
                        float yNear = 0.0f;
                        ok = distance > rs.opts.crouch + 1.0f && flatLength(mid - pos) < 16.0f &&
                             groundAt(rdram, ctx, runtime, base, near.x, near.z, mid.y - 0.05f, &yNear) &&
                             std::fabs(yNear - pos.y) < 0.6f && runUpClear(rdram, ctx, runtime, base, pos, pos.y, near, yNear, why);
                        if (ok)
                            t.heading = dir;
                    }
                    else if (t.ride)
                        ok = true; // wherever DM2 put him (the result says where)
                    else
                    {
                        // A quarter pipe ahead: the ground flat (within 0.3)
                        // for 3 units or more, then rising 1.5 or more within
                        // 6 (its face), never dropping on the way.
                        const Vec pipe = t.start + t.heading * 10.0f;
                        ok = off < 8.0f && flatDot(pipe - pos, t.heading) > 4.0f;
                        if (ok)
                        {
                            t.heading = flatUnit(pipe - pos);
                            float foot = -1.0f;
                            bool rises = false;
                            for (float d = 0.5f; d <= 18.0f && ok && !rises; d += 0.5f)
                            {
                                const Vec q = pos + t.heading * d;
                                float y = 0.0f;
                                if (!groundAt(rdram, ctx, runtime, base, q.x, q.z, pos.y - 10.0f, &y))
                                {
                                    ok = false;
                                    why = "no ground ahead";
                                    break;
                                }
                                const float h = pos.y - y; // over the start (+y down)
                                if (h < -0.6f)
                                {
                                    ok = false;
                                    why = "the ground drops ahead";
                                }
                                else if (foot < 0.0f && h > 0.3f)
                                {
                                    foot = d;
                                    if (d < 3.0f)
                                    {
                                        ok = false;
                                        why = "no flat run-up";
                                    }
                                }
                                else if (foot >= 0.0f && h >= 1.5f)
                                    rises = d - foot <= 6.0f;
                                else if (foot >= 0.0f && d - foot > 6.0f)
                                {
                                    ok = false;
                                    why = "no pipe face ahead";
                                }
                            }
                            if (ok && !rises)
                            {
                                ok = false;
                                why = "no pipe face ahead";
                            }
                        }
                    }
                    if (!ok)
                    {
                        std::snprintf(text, sizeof text, "%s (placed at %.2f %.2f %.2f, %.2f off)", why.c_str(), pos.x, pos.y,
                                      pos.z, off);
                        if (t.grind)
                            finishGrind(false, text);
                        else
                            finishTrial(text);
                        return;
                    }
                    rs.phase = Phase::Riding;
                    r.frames = 0;
                    r.placedOff = off;
                    r.startY = pos.y;
                    r.startPos = pos;
                    return;
                }
                noteState(state);
                const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
                r.speedMax = std::max(r.speedMax, speed);
                // Steering: toward the rail's middle (grind) or along the heading.
                Vec want = t.heading;
                uint16_t buttons = rs.opts.pedal;
                if (t.grind)
                {
                    const Vec mid = (t.a + t.b) * 0.5f;
                    const float distance = flatDot(pos - mid, t.railNormal); // > 0 before the rail
                    r.closest = std::min(r.closest, flatLength(pos - mid));
                    if (distance > rs.opts.crouch + 0.5f)
                        want = flatUnit(mid - pos);
                    // Attempt 0 hops late (crouch from 3 to 1.6 before the
                    // rail: the rail met on the way up, as rails a unit over
                    // the ground are); attempt 1 early, timed to come down
                    // onto the rail 0.35 over it (a hop leaves at 0.092 a
                    // frame up and falls 0.0055 a frame per frame; the
                    // rider's position is 0.3 over the ground), or at the
                    // top of the hop for a rail higher than that.
                    bool crouch = distance <= rs.opts.crouch && distance > rs.opts.hop, hop = distance <= rs.opts.hop;
                    if (t.attempt == 1)
                    {
                        const float rise = t.railHeight - 0.3f + 0.35f, vy = 0.092f, g = 0.0055f;
                        const float disc = vy * vy - 2.0f * g * rise;
                        const float crossing = disc > 0.0f ? (vy + std::sqrt(disc)) / g : vy / g;
                        const float toward = std::max(-flatDot(vel, t.railNormal), 0.05f);
                        const float frames = distance / toward;
                        hop = frames <= crossing;
                        crouch = !hop && frames <= crossing + 8.0f;
                    }
                    if (!r.hopped && crouch)
                    {
                        buttons |= rs.opts.jump;
                        r.crouched = true;
                    }
                    else if (!r.hopped && hop && r.crouched)
                    {
                        r.hopped = true;
                        r.hopDistance = distance;
                        r.hopSpeed = speed;
                    }
                    if (r.hopped)
                        buttons = rs.opts.grindButton;
                    if (state == 4 && r.grindFrame < 0)
                    {
                        r.grindFrame = static_cast<int>(r.frames);
                        r.edge = static_cast<uint64_t>(readU32(rdram, self + kGrindEdge)) |
                                 (static_cast<uint64_t>(readU32(rdram, self + kGrindEdge + 4u)) << 32);
                    }
                    const bool past = distance < -4.0f || (r.grindFrame >= 0 && r.frames > static_cast<uint32_t>(r.grindFrame) + 30u);
                    if (past || r.frames >= rs.opts.timeout)
                    {
                        const bool onRail = r.grindFrame >= 0 && sameEdge(rdram, ctx, runtime, base, r.edge, t);
                        if (r.grindFrame >= 0)
                        {
                            ++rs.grinds;
                            rs.grindsOnRail += onRail;
                            rs.firstTry += t.attempt == 0;
                        }
                        std::snprintf(text, sizeof text,
                                      "%s at frame %d%s; start %.2f %.2f %.2f heading %.3f %.3f; hop %s at %.2f speed %.2f; "
                                      "closest %.2f; states %s",
                                      r.grindFrame >= 0 ? "grind" : "no grind", r.grindFrame,
                                      r.grindFrame >= 0 ? (onRail ? " (this rail)" : " (another edge)") : "", t.start.x,
                                      t.start.y, t.start.z, t.heading.x, t.heading.z, r.hopped ? "yes" : "no", r.hopDistance,
                                      r.hopSpeed, r.closest, r.states.c_str());
                        finishGrind(r.grindFrame >= 0, text);
                        return;
                    }
                }
                else if (t.ride)
                {
                    // Pedal along the heading whatever comes; watch the
                    // surface under the wheels, a crash, a respawn.
                    const int surface = static_cast<int>(readU32(rdram, self + kWheelSurface));
                    if (surface != r.lastSurface)
                    {
                        if (r.lastSurface > -2 && r.surfaces.size() < 300u)
                            r.surfaces += std::to_string(r.lastSurface) + ":" + std::to_string(r.surfaceRun) + " ";
                        r.lastSurface = surface;
                        r.surfaceRun = 0;
                    }
                    ++r.surfaceRun;
                    if (state == 2 && r.crashFrame < 0)
                    {
                        r.crashFrame = static_cast<int>(r.frames);
                        r.bailKind = *getConstMemPtr(rdram, self + kBailKind);
                        r.crashAt = pos;
                    }
                    if (r.haveLast && r.respawnFrame < 0 && flatLength(pos - r.last) + std::fabs(pos.y - r.last.y) >= 3.0f)
                    {
                        r.respawnFrame = static_cast<int>(r.frames);
                        r.respawnFrom = r.last;
                        r.respawnTo = pos;
                    }
                    if (!r.haveLast || pos.y > r.lowest.y)
                        r.lowest = pos;
                    r.last = pos;
                    r.haveLast = true;
                    if (r.frames >= static_cast<uint32_t>(t.top))
                    {
                        const std::string sur = r.surfaces + std::to_string(r.lastSurface) + ":" + std::to_string(r.surfaceRun);
                        std::snprintf(text, sizeof text,
                                      "placed %.2f off at %.2f %.2f %.2f; surface under wheel 0 (sound:frames) %s; states %s%d:%u; "
                                      "crash %s frame %d bail kind %d at %.2f %.2f %.2f; respawn %s frame %d from %.2f %.2f "
                                      "%.2f to %.2f %.2f %.2f; lowest y %.2f at %.2f %.2f; end %.2f %.2f %.2f",
                                      r.placedOff, r.startPos.x, r.startPos.y, r.startPos.z, sur.c_str(), r.states.c_str(), r.lastState,
                                      r.stateRun, r.crashFrame >= 0 ? "at" : "none", r.crashFrame, r.bailKind, r.crashAt.x,
                                      r.crashAt.y, r.crashAt.z, r.respawnFrame >= 0 ? "at" : "none", r.respawnFrame,
                                      r.respawnFrom.x, r.respawnFrom.y, r.respawnFrom.z, r.respawnTo.x, r.respawnTo.y,
                                      r.respawnTo.z, r.lowest.y, r.lowest.x, r.lowest.z, pos.x, pos.y, pos.z);
                        finishTrial(text);
                        return;
                    }
                }
                else
                {
                    // The rider's states (+0xFF0, seen): 0 riding, 1 vert air
                    // (out of a quarter pipe, held over its lip), 2 crashed,
                    // 3 in the air (a hop, a drop), 4 grinding.
                    if ((state == 1 || state == 3) && r.firstAir < 0 && r.frames > 5u)
                    {
                        r.firstAir = static_cast<int>(r.frames);
                        r.takeOff = pos;
                        r.airState = state;
                    }
                    if (r.firstAir >= 0 && r.landedFrame < 0)
                    {
                        if (state == 1 || state == 3)
                        {
                            r.apex = std::min(r.apex, pos.y);
                            ++r.airFrames;
                        }
                        else
                        {
                            r.landedFrame = static_cast<int>(r.frames);
                            r.landing = pos;
                            r.crashed = state == 2;
                        }
                    }
                    // Pedal and steer on the run-up only; nothing held on the
                    // ramp or in the air.
                    if (r.firstAir >= 0 || std::fabs(pos.y - r.startY) > 0.3f)
                        buttons = 0;
                    if ((r.landedFrame >= 0 && r.frames > static_cast<uint32_t>(r.landedFrame) + 30u) || r.frames >= rs.opts.timeout)
                    {
                        std::snprintf(text, sizeof text,
                                      "%s; air frames %u, above the take-off %.2f, above top %.2f; %s; top speed %.2f; "
                                      "placed %.2f off; states %s",
                                      r.firstAir < 0 ? "no air" : r.airState == 1 ? "vert air" : "air", r.airFrames,
                                      r.firstAir >= 0 ? r.takeOff.y - r.apex : 0.0f, r.firstAir >= 0 ? t.top - r.apex : 0.0f,
                                      r.landedFrame < 0 ? "not down" : r.crashed ? "crashed" : "landed", r.speedMax, r.placedOff,
                                      r.states.c_str());
                        finishTrial(text);
                        return;
                    }
                    if (!buttons)
                    {
                        rs.buttons = 0;
                        PSPadBackend::setOverrideButtons(0);
                        return;
                    }
                }
                // Turn toward want: left or right by the sign of the turn.
                const Vec h = flatUnit(fwd);
                const float cross = h.x * want.z - h.z * want.x, dot = flatDot(h, want);
                if (dot < 0.996f && !(t.grind && r.hopped))
                    buttons |= (cross * rs.opts.steer > 0.0f) ? rs.opts.left : rs.opts.right;
                rs.buttons = buttons;
                PSPadBackend::setOverrideButtons(buttons);
            }

            void frameResetHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
            {
                step(rdram, ctx, runtime);
                ridesState().frameReset(rdram, ctx, runtime);
            }

            void install(PS2Machine &runtime)
            {
                RidesState &rs = ridesState();
                const char *spec = std::getenv("ZX_DM2_RIDES");
                if (!spec || !*spec)
                    return;
                std::string in, out;
                if (!splitPathPair(spec, in, out) || !readTrials(in))
                {
                    std::cerr << "[rides] ZX_DM2_RIDES=<trials>:<results>: no trials in " << spec << std::endl;
                    return;
                }
                rs.out.open(out);
                readOptions();
                rs.traceRides = std::getenv("ZX_DM2_RIDES_TRACE") != nullptr;
                rs.playerOneHash = zx::zaxis_text::keyHash("PlayerOne");
                if (!(rs.frameReset = runtime.lookupFunction(kFrameReset)) || !runtime.replaceFunction(kFrameReset, &frameResetHook) ||
                    !(rs.spawnLookup = runtime.lookupFunction(kSpawnLookup)) ||
                    !runtime.replaceFunction(kSpawnLookup, &spawnLookupHook))
                {
                    std::cerr << "[rides] the hooks cannot be placed" << std::endl;
                    return;
                }
                if (rs.traceRides && (rs.respawnSearch = runtime.lookupFunction(kRespawnSearch)))
                    runtime.replaceFunction(kRespawnSearch, &respawnSearchHook);
                std::cout << "[rides] " << rs.trials.size() << " trials from " << in << " -> " << out << std::endl;
            }
        }

        // levels.txt: "<FOLDER> <display name>" per line.
        bool readLevels(const fs::path &dir, std::vector<std::pair<std::string, std::string>> &levels)
        {
            std::ifstream in(dir / "levels.txt");
            std::string line;
            while (std::getline(in, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                const size_t space = line.find(' ');
                if (line.empty() || space == std::string::npos)
                    continue;
                levels.emplace_back(line.substr(0, space), line.substr(space + 1u));
            }
            return !levels.empty();
        }
    }

    // The levels of levels.txt in dir, complete ones only; `want`
    // (ZX_DM2_XBOX_LEVEL) keeps one folder.
    void addLevels(const char *dir, const char *want)
    {
        std::vector<std::pair<std::string, std::string>> levels;
        if (!readLevels(dir, levels))
        {
            std::cerr << "[xboxlevel] no levels.txt in " << dir << ": its levels left out" << std::endl;
            return;
        }
        const fs::path root(dir);
        for (const auto &[folder, name] : levels)
        {
            if (want && *want && folder != want)
                continue;
            bool complete = true;
            for (const char *ext : {".ZIT", ".GEO", ".INS", ".TXR", ".DAT"})
                if (!fs::exists(root / folder / (folder + ext)))
                {
                    std::cerr << "[xboxlevel] " << (root / folder / (folder + ext)).string() << " is missing: " << name
                              << " left out" << std::endl;
                    complete = false;
                }
            if (!complete)
                continue;
            Level level{folder, name, root, {}};
            if (const auto info = readHostFile(root / folder / "INFO.TXT"))
                level.info.assign(info->begin(), info->end());
            state().levels.push_back(std::move(level));
        }
    }

    bool install(PS2Machine &runtime)
    {
        State &st = state();
        // Values 9, 10: Venice, Greenville, as on the Xbox after Airport.
        const char *dir = zx::boot::text("ZX_DM2_XBOX_LEVELS_DIR");
        const char *on = zx::boot::text("ZX_DM2_XBOX_LEVELS");
        if (!dir || !*dir || (on && on[0] == '0'))
        {
            std::cout << "[xboxlevel] off (ZX_DM2_XBOX_LEVELS_DIR: the levels made by tools/modding/dm2_xbox.py)" << std::endl;
            return false;
        }
        st.levels.clear();
        addLevels(dir, std::getenv("ZX_DM2_XBOX_LEVEL"));
        {
            // The Thor panel's files for them, by value.
            std::vector<const char *> folders;
            for (const Level &level : st.levels)
                folders.push_back(level.folder.c_str());
            panel_state::setXboxLevels(folders.data(), folders.size());
        }
        // The Xbox pictures follow the app's Xbox Textures setting
        // ([display] xbox_textures, which sets ZX_XBOX_TEXTURES).
        const char *pictures = zx::boot::text("ZX_XBOX_TEXTURES");
        zx::texture_replace::setEnabled(!(pictures && pictures[0] == '0'));
        // The GPU renderer finds a picture by the texture object a draw
        // shows, not by what GS memory holds: DM2 streams each node's
        // textures into its GS texture cache and keeps level 0 inside the
        // frame buffer while it draws (BRINGUP round 2), so a texture's
        // indices are seldom in GS memory as a packed picture when they are
        // drawn (zge_xbox_textures.h: the texture cache's notes).
        zge_host::xbox_textures::installTextureNotes(runtime, game::Game::Dm2);
        if (st.levels.empty())
        {
            std::cerr << "[xboxlevel] no complete level: off" << std::endl;
            return false;
        }
        st.trace = std::getenv("ZX_TRACE_XBOXLEVEL") != nullptr;
        gf::registerProvider(kHostFolder, &provide, "xboxlevel");
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        for (const char *strings : {"STRING.EUS", "STRING.EUK", "STRING.GER", "STRING.FRE"})
            zx::disc_patch::registerPatch(strings, &patchStrings);
        runtime.addVSyncHook(&vsync);
        runtime.addStoppedHook(&removePictures);
        if (!(st.bucketReset = runtime.lookupFunction(kBucketReset)) || !runtime.replaceFunction(kBucketReset, &bucketResetHook))
            std::cerr << "[xboxlevel] the draw buckets cannot grow: the level overruns them" << std::endl;
        if ((st.textureLoad = runtime.lookupFunction(kTextureLoad)))
            runtime.replaceFunction(kTextureLoad, &textureLoadHook);
        if (!(st.levelUpdate = runtime.lookupFunction(kLevelUpdate)) || !runtime.replaceFunction(kLevelUpdate, &levelUpdateHook) ||
            !(st.startGame = runtime.lookupFunction(kStartGame)) || !runtime.replaceFunction(kStartGame, &startGameHook) ||
            !(st.setupStats = runtime.lookupFunction(kSetupStats)) || !runtime.replaceFunction(kSetupStats, &setupStatsHook) ||
            !(st.readVariable = runtime.lookupFunction(kReadVariable)) ||
            !runtime.replaceFunction(kReadVariable, &readVariableHook))
        {
            std::cerr << "[xboxlevel] the level select's table cannot be followed: only the first level" << std::endl;
            st.levels.resize(1);
        }
        records::install(runtime);
        if (st.trace && (st.newPart = runtime.lookupFunction(kNewPart)) &&
            (st.drawRigid = runtime.lookupFunction(kDrawRigid)) && runtime.replaceFunction(kNewPart, &newPartHook) &&
            runtime.replaceFunction(kDrawRigid, &drawRigidHook))
        {
            st.runtime = &runtime;
            runtime.addVSyncHook(&traceVsync);
            st.raycast = runtime.lookupFunction(kRaycast);
            if ((st.drawSkinned = runtime.lookupFunction(kDrawSkinned)))
                runtime.replaceFunction(kDrawSkinned, &drawSkinnedHook);
            if ((st.allocate = runtime.lookupFunction(kAllocate)))
                runtime.replaceFunction(kAllocate, &allocateHook);
            if ((st.texFrameEnd = runtime.lookupFunction(kTexFrameEnd)))
                runtime.replaceFunction(kTexFrameEnd, &texFrameEndHook);
            if ((st.texFlush = runtime.lookupFunction(kTexFlush)))
                runtime.replaceFunction(kTexFlush, &texFlushHook);
            if ((st.texMarkIrq = runtime.lookupFunction(kTexMarkIrq)))
                runtime.replaceFunction(kTexMarkIrq, &texMarkIrqHook);
            if ((st.faceUnlinked = runtime.lookupFunction(kFaceUnlinked)))
                runtime.replaceFunction(kFaceUnlinked, &faceUnlinkedHook);
            if ((st.faceLinked = runtime.lookupFunction(kFaceLinked)))
                runtime.replaceFunction(kFaceLinked, &faceLinkedHook);
        }
        std::cout << "[xboxlevel] on: " << st.levels.size() << " levels (";
        for (size_t i = 0; i < st.levels.size(); ++i)
            std::cout << (i ? ", " : "") << st.levels[i].name;
        std::cout << ") in Woodward Camp's slot" << std::endl;
        return true;
    }
}

namespace dave_mirra::xboxlevel
{
    std::string chosenLevel()
    {
        const int level = current();
        return level >= 0 ? state().levels[static_cast<size_t>(level)].folder : std::string();
    }
    std::vector<std::string> levels()
    {
        std::vector<std::string> out;
        for (const Level &level : state().levels)
            out.push_back(level.folder);
        return out;
    }
    uint32_t allocateExtended(PS2Machine &runtime, uint32_t size)
    {
        return ensureExtended(&runtime) ? runtime.guestMallocExtended(size, 64u) : 0u;
    }
}

namespace dave_mirra
{
    bool installXboxLevels(PS2Machine &runtime)
    {
        const bool on = xboxlevel::install(runtime);
        xboxlevel::rides::install(runtime); // after: it chains the frame hook
        return on;
    }
}
