// Aggressive Inline (SLUS-20327): TUT/Estate, a leftover earlier Tutorial
// on the GameCube disc only, as its own level in free skate.
//
// The GameCube disc's assets/levels/TUT/Estate is an earlier export of the
// Tutorial site (its exporter log names Tutorial_114.max) written by the PS2
// exporter: strip groups of at most 60, 8-bit BMP textures, PS2 render
// state words, PS2 INS and DAT records, scripts of version 2.70; 217
// objects, 114 textures, the same site (palms, statues, treehouse) with art
// borrowed from the Movie Lot and BMX 2's park editor themes, and two
// scripts: TUT.bin (career) and TUTT.bin (free skate), an earlier 25-lesson
// tutorial on the retail scripts' object names, with only natives and NPC
// types the PS2 game has. Its own motions.zal (doors, a trigger); no
// MOVIE.bin.
//
// The build makes the files the game loads from it (ps2xGcConvert,
// runtime/ps2_zaxis_estate.h: the one 24-bit texture made 8-bit, the rest
// as it is) and bundles them as Resources/estate (Windows: estate\ next to
// disc\); the app sets PS2X_ESTATE_DIR to that folder, and PS2X_ESTATE from
// Settings > Game > Estate Level (settings.ini [extras] estate, default
// off). Nothing here reads a disc image or converts anything.
//
// With PS2X_ESTATE=1 and the folder:
//   - FrontEnd.zsc's free skate level select gains "Estate" before the
//     Editor Level (after the Zoo when it is on),
//     always unlocked, with its own icon (ICONS.UFNT, made at build time
//     from a picture of the level; without it the Tutorial's cap) and its
//     own still picture (STILL.ULIB, the same view; without it the Editor
//     Level's; no flythrough of it exists);
//   - the level is the Tutorial's level index 7 (scene and folder TUT): its
//     SelectedLevel "Estate" is answered with 7 by the wrapped name lookup
//     func_1A8258, and from then on, while the game is not a career, the
//     files of ASSETS/LEVELS/TUT that Estate has are its own: TUT.ZIT,
//     TUT.GEO, TUT.INS, TUT.TXR, TUT.DAT, TUT.BIN, TUTT.BIN, MOTIONS.ZAL.
//     The rest (MOVIE.BIN, the loading screens of ASSETS/LOADING/TUT) stay
//     the Tutorial's. Any other level's lookup ("Tutorial" in the career's
//     level select) ends it, and a career always loads the retail Tutorial;
//   - the Xbox texture pack is kept off Estate's textures (the
//     .xtp of the retail TUT.TXR, or another level's, could match a
//     borrowed texture's indices with another palette): the build lists
//     their keys (TEXTURES.TXT) and the pack's texture identity notes carry
//     no key for them while Estate is played (zge_xbox_textures.h).
//     Estate's own scripts always run (no Xbox script takes their place),
//     and their texts are left in English with another language: the
//     language's table pairs the Xbox release's level scripts (the retail
//     Tutorial's) with the PS2's, not with these (language::keepUntranslated);
//   - its own records beside the career save (<profile>.estaterecords),
//     the Tutorial's row in the save left as it is ("its own records");
//   - the pause menu is the Tutorial's lesson list, whose items start
//     Estate's own lessons ("the lesson list");
//   - its lesson givers answer Circle (the NPC constructor's free skate
//     hiding skipped in Estate: "the lesson givers").
// PS2X_ESTATE=1 without the folder: off, with a message.

#include "aggressive_inline_estate.h"
#include "aggressive_inline_npcs.h"

#include "aggressive_inline_language.h"
#include "aggressive_inline_saves.h"
#include "ps2_runtime.h"
#include "zge_xbox_textures.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_save_store.h"
#include "runtime/ps2_zaxis_estate.h"
#include "runtime/ps2_zaxis_text.h"
#include "runtime/ps2_zaxis_ui.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_set>
#include <set>
#include <vector>

namespace aggressive_inline::estate
{
    namespace
    {
        namespace gf = ps2x::game_files;
        namespace zx = ps2x::zaxis;
        namespace fs = std::filesystem;

        const char kLevelFolder[] = "ASSETS/LEVELS/TUT/";
        const char kSelectedLevel[] = "Estate"; // the menu's SelectedLevel
        const char kShownName[] = "Estate";     // the level select's name
        constexpr uint32_t kLevelFromName = 0x001A8258u; // (name) -> index in 0x265440 (14: none)
        constexpr uint32_t kTutorialIndex = 7u;
        constexpr uint32_t kGameType = 0x0024C9B3u;      // u8: 1 career
        constexpr uint32_t kLevelIndex = 0x0024C9B4u;    // u32: the level played (low byte)
        constexpr uint32_t kGameMode = 0x00276FE4u;      // 2 in a level
        const char *const kFiles[] = {"TUT.ZIT", "TUT.GEO", "TUT.INS",  "TUT.TXR",
                                      "TUT.DAT", "TUT.BIN", "TUTT.BIN", "MOTIONS.ZAL"};

        struct KeyHash
        {
            size_t operator()(const std::tuple<uint32_t, uint32_t, uint64_t> &k) const
            {
                return static_cast<size_t>(std::get<2>(k) ^ (static_cast<uint64_t>(std::get<0>(k)) << 40) ^
                                           (static_cast<uint64_t>(std::get<1>(k)) << 20));
            }
        };
        struct Estate
        {
            fs::path dir;
            PS2Runtime::RecompiledFunction levelFromName = nullptr;
            std::atomic<bool> selected{false}; // the free skate level select's last lookup was Estate
            std::atomic<bool> career{false}; // the game type is a career (never Estate)
            int entry = -1; // its level-select entry (the menu patch)
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files; // name -> contents
            std::unordered_set<std::tuple<uint32_t, uint32_t, uint64_t>, KeyHash> textureKeys;
            std::atomic<uint64_t> keptOff{0}; // texture notes without a key (the pack kept off)
            std::set<std::string> scriptTexts; // Estate's scripts' strings (kept in English)
            int trace = 0;
        };
        Estate &estate() { return ps2x::session::state<Estate>(); }

        std::shared_ptr<const std::vector<uint8_t>> readHostFile(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            return std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(in)),
                                                          std::istreambuf_iterator<char>());
        }

        bool active() { return estate().selected.load() && !estate().career.load(); }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            if (!active() || path.rfind(kLevelFolder, 0) != 0)
                return nullptr;
            const std::string name = path.substr(sizeof(kLevelFolder) - 1u);
            if (std::find(std::begin(kFiles), std::end(kFiles), name) == std::end(kFiles))
                return nullptr; // MOVIE.BIN and the rest: the Tutorial's
            std::lock_guard<std::mutex> lock(estate().mutex);
            if (auto it = estate().files.find(name); it != estate().files.end())
                return it->second;
            std::shared_ptr<const std::vector<uint8_t>> bytes = readHostFile(estate().dir / name);
            if (!bytes)
            {
                std::cerr << "[estate] " << (estate().dir / name).string() << " cannot be read" << std::endl;
                return nullptr;
            }
            if (estate().trace)
                std::cout << "[estate] " << path << ": Estate's (" << bytes->size() << " bytes)" << std::endl;
            // The large ones are read again on the next load.
            if (name != "TUT.GEO" && name != "TUT.TXR")
                estate().files[name] = bytes;
            return bytes;
        }

        // ---- its own records -------------------------------------------------
        //
        // The level records (top speed and the rest: 14 of 0x14 bytes, a
        // value and the holder's name, per level index; FUN_001BCDE0 /
        // FUN_001BCE40 update them) at 0x24CBDC + level * 0x118 are part of
        // the career save's globals. Estate plays as level 7, so it would
        // share the Tutorial's row. It keeps its own beside the career save
        // instead, <saves>/<profile>.estaterecords ("ESTR", u32 1, the row's
        // 0x118 bytes; written atomically with a .bak): Estate's row is put into
        // level 7's when the level select looks Estate up (func_1A8258),
        // copied back to Estate's while it is played and for 5 s after (the
        // records its end writes; the file at most every 10 s), and the
        // Tutorial's own row is put back when another level is looked up. A
        // row that changed under us at another time (a save loaded) becomes
        // the Tutorial's. Every career body the native saves write gets the
        // Tutorial's own row in level 7's place while Estate's is in RAM
        // (addCareerBodyFilter), so a save always holds the Tutorial's
        // records and the file Estate's. Estate never played starts from
        // the row a new game has (level 7's as the boot first fills it,
        // before a save is loaded; the file is written once it changes).
        // PS2X_ESTATE_RECORDS=0 keeps the shared row.
        constexpr uint32_t kRecordRows = 0x0024CBDCu;
        constexpr uint32_t kRecordRowBytes = 0x118u;
        constexpr uint32_t kTutorialRow = kRecordRows + kTutorialIndex * kRecordRowBytes;
        constexpr uint32_t kCareerGlobals = 0x0024CBB8u; // the career body's first bytes
        constexpr size_t kBodyRowOffset = kTutorialRow - kCareerGlobals;
        using Row = std::vector<uint8_t>;
        struct Records
        {
            bool ownRecords = true;
            std::mutex mutex;
            Row newGameRow; // level 7's row of a new game
            Row estateRow; // Estate's own (empty: never played, no file)
            Row tutorialRow; // the Tutorial's own while Estate's is in RAM
            Row inSlot; // what we last put in or read from level 7's row
            bool estateInSlot = false;
            bool recordsRead = false;
            bool recordsDirty = false;
            uint64_t vsyncNow = 0;
            uint64_t playedUntil = 0;
            uint64_t lastRecordsWrite = 0;
            std::atomic<uint64_t> recordsSaved{0};
            std::atomic<uint64_t> savesFiltered{0};
        };
        Records &records() { return ps2x::session::state<Records>(); }

        uint32_t readU32(const uint8_t *rdram, uint32_t address);

        fs::path recordsFile() { return nativeSavesFolder() / (nativeSaveProfile() + ".estaterecords"); }
        Row slotRow(const uint8_t *rdram)
        {
            const uint8_t *p = getConstMemPtr(rdram, kTutorialRow);
            return p ? Row(p, p + kRecordRowBytes) : Row();
        }
        void putSlotRow(uint8_t *rdram, const Row &row)
        {
            if (uint8_t *p = getMemPtr(rdram, kTutorialRow); p && row.size() == kRecordRowBytes)
                std::memcpy(p, row.data(), kRecordRowBytes);
        }
        float topSpeedOf(const Row &row)
        {
            float v = 0.0f;
            if (row.size() == kRecordRowBytes)
                std::memcpy(&v, row.data() + 11u * 0x14u, 4);
            return v;
        }

        void readRecords()
        {
            records().recordsRead = true;
            const ps2x::saves::ReadResult read = ps2x::saves::readWithBackup(
                recordsFile(), [](const ps2x::saves::Bytes &d)
                { return d.size() == 8u + kRecordRowBytes && std::memcmp(d.data(), "ESTR", 4) == 0; });
            if (!read.ok())
                return;
            records().estateRow.assign(read.data.begin() + 8, read.data.end());
            std::cout << "[estate] records from " << recordsFile().string() << " (top speed " << topSpeedOf(records().estateRow)
                      << ")" << std::endl;
        }

        void writeRecords()
        {
            if (records().estateRow.size() != kRecordRowBytes)
                return;
            ps2x::saves::Bytes d{'E', 'S', 'T', 'R', 1, 0, 0, 0};
            d.insert(d.end(), records().estateRow.begin(), records().estateRow.end());
            std::string error;
            if (!ps2x::saves::writeFileAtomic(recordsFile(), d.data(), d.size(), &error))
                std::cerr << "[estate] records not written: " << error << std::endl;
            else
                ++records().recordsSaved;
        }

        // true: Estate's row into level 7's; false: the Tutorial's back.
        void showRecords(uint8_t *rdram, bool estate)
        {
            std::lock_guard<std::mutex> lock(records().mutex);
            if (!records().recordsRead)
                readRecords();
            const Row now = slotRow(rdram);
            if (now.size() != kRecordRowBytes)
                return;
            if (records().estateInSlot && now != records().inSlot && records().vsyncNow > records().playedUntil)
            {
                records().estateInSlot = false; // a save loaded meanwhile: the Tutorial's
                std::cout << "[estate] records: level 7's row changed (a save loaded): the Tutorial's" << std::endl;
            }
            if (estate == records().estateInSlot)
                return;
            if (!estate)
            {
                putSlotRow(rdram, records().tutorialRow);
                records().estateInSlot = false;
                std::cout << "[estate] records: the Tutorial's own row back in level 7's" << std::endl;
                return;
            }
            records().tutorialRow = now;
            const bool own = records().estateRow.size() == kRecordRowBytes;
            records().inSlot = own ? records().estateRow : records().newGameRow.size() == kRecordRowBytes ? records().newGameRow : now;
            putSlotRow(rdram, records().inSlot);
            records().estateInSlot = true;
            std::cout << "[estate] records: Estate's in level 7's row (" << (own ? "its own" : "a new game's")
                      << ", top speed " << topSpeedOf(records().inSlot) << "; the Tutorial's kept, top speed "
                      << topSpeedOf(records().tutorialRow) << ")" << std::endl;
        }

        // Every 30 vsyncs: while Estate is played (and 300 vsyncs after),
        // level 7's row back to Estate's own.
        void syncRecords(uint8_t *rdram, uint64_t vsync, bool playing)
        {
            std::lock_guard<std::mutex> lock(records().mutex);
            records().vsyncNow = vsync;
            if (records().newGameRow.empty())
            {
                // A new game's row: level 7's as the boot first fills it (before
                // a save, if any, is loaded over it).
                const Row now = slotRow(rdram);
                if (now.size() == kRecordRowBytes && std::any_of(now.begin(), now.end(), [](uint8_t b) { return b != 0u; }))
                {
                    records().newGameRow = now;
                    if (estate().trace)
                        std::cout << "[estate] records: a new game's row (top speed " << topSpeedOf(now) << ", vsync "
                                  << vsync << ")" << std::endl;
                }
            }
            if (!records().estateInSlot)
                return;
            if (playing)
                records().playedUntil = vsync + 300u;
            if (vsync % 30u)
                return;
            const Row now = slotRow(rdram);
            if (now.size() == kRecordRowBytes && now != records().inSlot)
            {
                if (vsync > records().playedUntil)
                {
                    records().estateInSlot = false; // a save loaded: the Tutorial's
                    std::cout << "[estate] records: level 7's row changed (a save loaded): the Tutorial's" << std::endl;
                    return;
                }
                records().inSlot = now;
                records().estateRow = now;
                records().recordsDirty = true;
            }
            if (records().recordsDirty && (vsync >= records().lastRecordsWrite + 600u || vsync + 30u > records().playedUntil))
            {
                writeRecords();
                records().recordsDirty = false;
                records().lastRecordsWrite = vsync;
                std::cout << "[estate] records saved (top speed " << topSpeedOf(records().estateRow) << ")" << std::endl;
            }
        }

        // A career body the native saves write: the Tutorial's own row in
        // level 7's place while Estate's is in RAM (its newest values taken
        // into Estate's first, if the sync has not yet).
        void filterCareerBody(std::vector<uint8_t> &body)
        {
            std::lock_guard<std::mutex> lock(records().mutex);
            if (!records().estateInSlot || body.size() < kBodyRowOffset + kRecordRowBytes ||
                records().tutorialRow.size() != kRecordRowBytes)
                return;
            const Row now(body.begin() + kBodyRowOffset, body.begin() + kBodyRowOffset + kRecordRowBytes);
            if (now != records().inSlot)
            {
                if (records().vsyncNow > records().playedUntil)
                    return; // not ours (a save loaded): the Tutorial's as it is
                records().inSlot = now;
                records().estateRow = now;
                records().recordsDirty = true;
            }
            std::memcpy(body.data() + kBodyRowOffset, records().tutorialRow.data(), kRecordRowBytes);
            ++records().savesFiltered;
            std::cout << "[estate] records: the save gets the Tutorial's own row (top speed " << topSpeedOf(records().tutorialRow)
                      << "), Estate's stays beside it" << std::endl;
        }

        // func_1A8258(name): "Estate" is the Tutorial's level 7 and makes
        // Estate the level the Tutorial's files are served for; any other
        // name (the level select's other entries, the career's "Tutorial")
        // ends that. The original runs first either way (for "Estate" it
        // finds no level and answers 14).
        void levelFromNameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t name = getRegU32(ctx, 4);
            char text[16] = {};
            if (const uint8_t *p = getConstMemPtr(rdram, name); p && name)
                for (size_t i = 0; i + 1 < sizeof(text) && p[i]; ++i)
                    text[i] = static_cast<char>(p[i]);
            estate().levelFromName(rdram, ctx, runtime);
            const bool picked = std::strcmp(text, kSelectedLevel) == 0;
            if (picked)
                setReturnU32(ctx, kTutorialIndex);
            if (records().ownRecords)
                showRecords(rdram, picked);
            if (picked != estate().selected.exchange(picked))
                std::cout << "[estate] " << (picked ? "Estate picked: the Tutorial's files are Estate's in free skate"
                                                    : "another level picked: the Tutorial's own files")
                          << std::endl;
        }

        // The game type, every vsync (a career never loads Estate), and with
        // the trace a line when Estate is played.
        void vsyncHook(uint8_t *rdram, uint64_t vsync)
        {
            uint8_t type = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, kGameType))
                type = *p;
            estate().career = type == 1u;
            uint32_t mode = 0, level = 0;
            std::memcpy(&mode, rdram + kGameMode, 4);
            std::memcpy(&level, rdram + kLevelIndex, 4);
            if (records().ownRecords)
                syncRecords(rdram, vsync, mode == 2u && (level & 0xFFu) == kTutorialIndex && active());
            if (!estate().trace || vsync % 600u)
                return;
            if (mode == 2u && (level & 0xFFu) == kTutorialIndex && active())
                std::cout << "[estate] playing Estate; " << estate().keptOff.load()
                          << " texture notes kept off the Xbox pack so far" << std::endl;
        }

        // ---- the pause menu ---------------------------------------------------
        //
        // The pause enter (game mode 3's, 0x1BBC30) shows the Tutorial's
        // lesson list for level 7 (0x1BC1FC reads the level index), Estate
        // too: its items lead to Estate's lessons ("the lesson list" below,
        // through the lesson natives of aggressive_inline_restart, which ask
        // estateLessonJump and the rest while Estate is played).
        uint32_t readU32(const uint8_t *rdram, uint32_t address);

        // ---- lesson 9 -----------------------------------------------------------
        //
        // The lessons are a chain: each TutorialAssigner, once its lesson is
        // done, sets the next one "active" (8 -> 9 -> 10, 10 -> the bowl's
        // lesson 11 -> 12, 20 -> the bowl's lesson 22 -> 23; nothing starts
        // lesson 21). A lesson is given by its assigner's ACTION event, the
        // player's Action by its giver (an NPC data point of its name).
        // Lesson 9 (TutorialAssignerNine: grind Scriptobj_rail_low_L9) has
        // neither a giver nor its rail nor a respawn point in the level, so
        // the chain stopped there. When it becomes active, it is done at
        // once, as its script ends it: lesson 10 active, lesson 9 complete
        // (the state changes func_1E0798 makes; wrapped).
        constexpr uint32_t kSetState = 0x001E0798u; // (object hash, state hash)
        struct LessonNine
        {
            PS2Runtime::RecompiledFunction setState = nullptr;
            std::atomic<uint64_t> skipped{0};
            std::set<int> lessonsDone; // lessons (1..25) whose object reached complete (the lesson list)
            bool jumping = false; // the lesson list's jump is changing states
        };
        LessonNine &lessonNine() { return ps2x::session::state<LessonNine>(); }
        constexpr uint32_t hashOf(const char *text)
        {
            uint32_t h = 0;
            for (; *text; ++text)
            {
                const char c = *text >= 'a' && *text <= 'z' ? static_cast<char>(*text - 'a' + 'A') : *text;
                h = (h << 4) + static_cast<uint8_t>(c);
                const uint32_t g = h & 0xF0000000u;
                if (g)
                    h ^= g >> 24;
                h &= ~g;
            }
            return h;
        }
        constexpr uint32_t kActive = hashOf("active");
        constexpr uint32_t kComplete = hashOf("complete");
        constexpr uint32_t kInit = hashOf("init");
        constexpr uint32_t kLessonNine = hashOf("TutorialAssignerNine");
        constexpr uint32_t kLessonTen = hashOf("TutorialAssignerTen");
        void snapshotVariables(const uint8_t *rdram);
        int lessonOfObject(uint32_t object);
        // ---- the lesson givers -------------------------------------------------
        //
        // A giver is an NPC (a data point of class NPC named after its
        // TutorialAssigner); the player gets its lesson by pressing Action
        // (Circle) by it: the Action move's target search (0x196BF8) asks the
        // nearest talker within reach (0x1BD1F8: every NPC the constructor
        // func_1E3500 listed answers "AreYouCloser", 0x1E1CC0, when it is
        // shown, its node visible and the skater within 4 units) and sends it
        // ACTION, whose handler in the lesson's state gives the lesson. The
        // constructor hides every NPC outside career mode but the Movie Lot's
        // and Boardwalk's trees (func_13BD48(object, 0) at 0x1E36AC: the
        // object's flag 0x10 cleared; aggressive_inline_zoo.cpp skips it in
        // the Zoo as the Xbox does): the PS2's free skate scripts have no
        // givers, Estate's free skate script (an earlier design) has 21. The
        // script shows and hides its givers' nodes (setObjectVisible), which
        // leaves that flag alone, so no giver ever answered: Circle by a giver
        // did nothing (lessons 1, 6 and 7 the last round's driver could not
        // give; the others only through an ACTION event sent by hand). In
        // Estate that one call does nothing, as in the Zoo: the givers'
        // objects stay shown and the script's own show and hide decides
        // which one is there (each lesson's init hides its giver).
        constexpr uint32_t kShowObject = 0x0013BD48u;     // (object, shown): flag 0x10
        struct Givers
        {
            PS2Runtime::RecompiledFunction showObject = nullptr;
            std::atomic<uint64_t> giversKeptShown{0};
        };
        Givers &givers() { return ps2x::session::state<Givers>(); }
        void showObjectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex &&
                npcs::isNpcStartHide(rdram, ctx, runtime))
            {
                ++givers().giversKeptShown;
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            givers().showObject(rdram, ctx, runtime);
        }

        // ---- the free skate script's missing show and hide calls --------------
        //
        // Estate's TUTT.BIN never shows or hides lessons 19, 23, 24 and 25's
        // props, the golf cart (17) and the bowl's pool drop (11, 22); its
        // career TUT.BIN, the same lessons' objects, does it in their init,
        // active and complete states' enter. Left alone they stayed on all
        // the time, lessons 19 and 24's quarter pipes in the same two places
        // (drawn over each other: the flashing ramp). The build lists TUT.BIN's
        // calls for them (VISIBILITY.TXT, runtime/ps2_zaxis_estate.h); when
        // the free skate script's object enters such a state (at the level's
        // start or by func_1E0798),
        // the same setObjectVisible is made (after the state's own "enter",
        // messageHandlerHook), as the native does it
        // (nativeSetObjectVisible: the object's node shown or hidden with its
        // collision, keep 1).
        struct ShowHide
        {
            std::multimap<std::pair<uint32_t, uint32_t>, ps2x::zaxis::estate::VisibilityRule> visibility;
            std::atomic<uint64_t> visibilityApplied{0};
            PS2Runtime::RecompiledFunction startObjects = nullptr;
            PS2Runtime::RecompiledFunction messageHandler = nullptr;
        };
        ShowHide &showHide() { return ps2x::session::state<ShowHide>(); }
        constexpr uint32_t kFindObjectsCall = 0x001646B8u; // (hash*, n) -> the objects, in 0x29D278
        constexpr uint32_t kObjectListWords = 0x0029D278u;
        constexpr uint32_t kObjectNode = 0x001645F0u;      // (object) -> its scene node
        constexpr uint32_t kNodeShow = 0x00132740u;        // (node, visible, 1)
        constexpr uint32_t kNodeCollide = 0x001327D0u;     // (node, visible, 1)
        constexpr uint32_t kVisibleNote = 0x001B86D0u;     // (hash, visible, keep)

        // The object's scene node, 0 if the level has none of that name.
        uint32_t nodeNamed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hash, uint32_t sp)
        {
            uint8_t *slot = getMemPtr(rdram, sp);
            if (!slot)
                return 0u;
            std::memcpy(slot, &hash, 4);
            uint32_t saved[2] = {};
            std::memcpy(saved, getConstMemPtr(rdram, kObjectListWords), sizeof(saved));
            uint32_t list = 0, object = 0, node = 0;
            runtime->callGuestFunction(rdram, ctx, kFindObjectsCall, {sp, 1u}, sp - 0x100u, &list);
            if (list)
                std::memcpy(&object, getConstMemPtr(rdram, list), 4);
            std::memcpy(getMemPtr(rdram, kObjectListWords), saved, sizeof(saved));
            if (object)
                runtime->callGuestFunction(rdram, ctx, kObjectNode, {object}, sp - 0x100u, &node);
            return node;
        }

        void applyVisibility(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t object, uint32_t state)
        {
            auto [first, last] = showHide().visibility.equal_range({object, state});
            if (first == last)
                return;
            const uint32_t v0 = getRegU32(ctx, 2);
            const uint32_t sp = (getRegU32(ctx, 29) - 0x600u) & ~15u;
            for (auto it = first; it != last; ++it)
            {
                const ps2x::zaxis::estate::VisibilityRule &r = it->second;
                const uint32_t node = nodeNamed(rdram, ctx, runtime, r.target, sp);
                if (!node)
                    continue;
                const uint32_t visible = r.visible ? 1u : 0u;
                runtime->callGuestFunction(rdram, ctx, kNodeShow, {node, visible, 1u}, sp - 0x100u);
                runtime->callGuestFunction(rdram, ctx, kNodeCollide, {node, visible, 1u}, sp - 0x100u);
                runtime->callGuestFunction(rdram, ctx, kVisibleNote, {r.target, visible, r.keep ? 1u : 0u}, sp - 0x100u);
                ++showHide().visibilityApplied;
                if (estate().trace)
                    std::cout << "[estate] object 0x" << std::hex << object << " state 0x" << state << ": 0x" << r.target
                              << std::dec << (visible ? " shown" : " hidden") << " (TUT.BIN's call)" << std::endl;
            }
            setReturnU32(ctx, v0);
        }

        // The objects' first states: the script's start (scriptStartObjects,
        // FUN_001DFBF8, with "enter" sent; also after a saved image is put
        // back) enters each object's current state without func_1E0798; then
        // the rules of every object's current state.
        constexpr uint32_t kStartObjects = 0x001DFBF8u; // (sendEnter)
        uint32_t scriptStateOf(const uint8_t *rdram, uint32_t object);
        void startObjectsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sendEnter = getRegU32(ctx, 4);
            const bool estate = active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex;
            if (sendEnter && estate)
                snapshotVariables(rdram); // before any enter runs
            showHide().startObjects(rdram, ctx, runtime);
            if (!sendEnter || !estate)
                return;
            std::set<std::pair<uint32_t, uint32_t>> keys;
            for (const auto &[key, rule] : showHide().visibility)
                keys.insert(key);
            size_t before = showHide().visibilityApplied;
            for (const auto &[object, state] : keys)
                if (scriptStateOf(rdram, object) == state)
                    applyVisibility(rdram, ctx, runtime, object, state);
            std::cout << "[estate] script started: " << showHide().visibilityApplied - before
                      << " of TUT.BIN's show/hide calls for the objects' first states; " << givers().giversKeptShown.load()
                      << " NPCs kept shown (the givers answer Circle)" << std::endl;
        }

        // Every script object's "enter" (a state switch's, func_1E0798, and the
        // level start's for each object's first state, func_1DFBF8) reaches it
        // through the engine message handler func_1DF480 (wrapped): after
        // the script's own handler, TUT.BIN's calls for that object and state.
        constexpr uint32_t kMessageHandler = 0x001DF480u; // (message): target +0x08, type +0x10
        constexpr uint32_t kObjectNameHash = 0x00164618u; // (scene object) -> its name hash
        constexpr uint32_t kEnter = hashOf("enter");
        uint32_t scriptState(const uint8_t *rdram, uint32_t object);
        void messageHandlerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t message = getRegU32(ctx, 4);
            const uint32_t type = readU32(rdram, message + 0x10u), target = readU32(rdram, message + 0x08u);
            showHide().messageHandler(rdram, ctx, runtime);
            if (type != kEnter || !active() || (readU32(rdram, kLevelIndex) & 0xFFu) != kTutorialIndex || showHide().visibility.empty())
                return;
            const uint32_t v0 = getRegU32(ctx, 2);
            uint32_t object = 0;
            runtime->callGuestFunction(rdram, ctx, kObjectNameHash, {target}, (getRegU32(ctx, 29) - 0x400u) & ~15u, &object);
            if (object)
                applyVisibility(rdram, ctx, runtime, object, scriptState(rdram, object));
            setReturnU32(ctx, v0);
        }

        void setStateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, 4), state = getRegU32(ctx, 5);
            lessonNine().setState(rdram, ctx, runtime);
            if (!active() || (readU32(rdram, kLevelIndex) & 0xFFu) != kTutorialIndex)
                return;
            if (state == kComplete && !lessonNine().jumping)
                if (const int lesson = lessonOfObject(object))
                    lessonNine().lessonsDone.insert(lesson);
            if (estate().trace >= 2)
                std::cout << "[estate] setState 0x" << std::hex << object << " 0x" << state << std::dec << " (frame "
                          << readU32(rdram, 0x00231CACu) << ")" << std::endl;
            if (object != kLessonNine || state != kActive)
                return;
            const uint32_t v0 = getRegU32(ctx, 2);
            const uint32_t sp = (getRegU32(ctx, 29) - 0x400u) & ~15u;
            runtime->callGuestFunction(rdram, ctx, lessonNine().setState, kSetState, {kLessonTen, kActive}, sp);
            runtime->callGuestFunction(rdram, ctx, lessonNine().setState, kSetState, {kLessonNine, kComplete}, sp);
            setReturnU32(ctx, v0);
            ++lessonNine().skipped;
            std::cout << "[estate] lesson 9 has no giver, rail or respawn point in the level: lesson 10 is next"
                      << std::endl;
        }

        uint32_t scriptStateOf(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t table = readU32(rdram, 0x002798C0u), states = readU32(rdram, 0x002798BCu);
            if (!table || !states)
                return 0u;
            for (uint32_t entry = table; entry < table + 0x10000u; entry += 8u)
            {
                const uint32_t hash = readU32(rdram, entry);
                if (hash == 0xFFFFFFFFu)
                    break;
                if (hash == object)
                    return readU32(rdram, states + readU32(rdram, table + readU32(rdram, entry + 4u) * 4u) * 4u);
            }
            return 0u;
        }

        // ---- the lesson list ----------------------------------------------------
        //
        // The pause menu of level 7 is the Tutorial's lesson list (Resume
        // Tutorial, Restart Lesson, 23 lessons, ...; aggressive_inline_settings
        // and _restart). The retail Tutorial's 23 lessons are Estate's 25
        // less 9 (no giver, rail or respawn point) and 21 (nothing starts it,
        // no giver or respawn point), the same topics in the same order (each
        // checked against Estate's own texts: Talking To People = lesson 1,
        // ..., Modifying Grinds = 8, Air Tricks = 10, Exit Pipes & Bowls = the
        // bowl's 11, ..., Trick Lines = 20, Juice is life = the bowl's 22,
        // Attributes = 23, Power-Ups = 24, Graduation Day = 25), so the items
        // stay; in Estate they lead to Estate's lessons. Its career script
        // (TUT.BIN) has the retail kind of controller, whose jump ends the
        // current lesson (its object back to "init") and starts the new one
        // ("active", whose enter respawns the skater at respawn_lesson<n>);
        // the free skate script played has neither, so the jump here does
        // the same with the free skate script's objects: the current lesson
        // (the one not in init or complete) back to init (lesson 1, which
        // has no init, to complete: its giver hidden, its counters cleared),
        // its giver's bubble taken down; the new lesson's variables back to
        // the script's initial values (TUTT.BIN's lessons count their steps
        // in variables their enter does not clear, so a lesson played once
        // would not start again), the skater respawned at its point, then the
        // lesson's object into the state the lesson before hands over
        // ("active"; lesson 3 its talk state 0x05FDE35E). The checkboxes: the
        // lessons whose object reached "complete" since the level started.
        uint32_t levelPlayer(const uint8_t *rdram);
        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value);
        constexpr uint32_t kVmObjects = 0x002798C0u;   // the script's object table
        constexpr uint32_t kVmVariables = 0x002798BCu; // its variable words
        constexpr int kMenuLessons = 23;
        constexpr int kMenuLesson[kMenuLessons] = {1,  2,  3,  4,  5,  6,  7,  8,  10, 11, 12, 13,
                                                   14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25};
        constexpr uint32_t kLessonThreeTalk = 0x05FDE35Eu;
        constexpr uint32_t kRespawnNamed = 0x00193EB0u; // (player, locator hash, 1)
        constexpr uint32_t kDestroyNpcBubble = 0x001E41F8u; // (name hash)
        struct LessonList
        {
            std::vector<uint32_t> initialVariables; // the script's variables at its start
            PS2Runtime::RecompiledFunction retailJump = nullptr;
        };
        LessonList &lessonList() { return ps2x::session::state<LessonList>(); }
        const char *const kNumberNames[25] = {
            "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve", "Thirteen",
            "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen", "Twenty", "TwentyOne", "TwentyTwo",
            "TwentyThree", "TwentyFour", "TwentyFive"};
        std::string lessonObjectName(int lesson) // 1..25
        {
            if (lesson == 11)
                return "scriptObj_bowl_qpipe_L11";
            if (lesson == 22)
                return "Scriptobj_bowl_qpipe_L22";
            return std::string("TutorialAssigner") + kNumberNames[lesson - 1];
        }
        uint32_t lessonObject(int lesson) { return hashOf(lessonObjectName(lesson).c_str()); }
        int lessonOfObject(uint32_t object)
        {
            for (int lesson = 1; lesson <= 25; ++lesson)
                if (lessonObject(lesson) == object)
                    return lesson;
            return 0;
        }

        // The script's objects' variable indices, sorted (each object's state
        // word, then its variables up to the next object's).
        std::vector<uint32_t> objectVariableStarts(const uint8_t *rdram)
        {
            std::vector<uint32_t> starts;
            const uint32_t table = readU32(rdram, kVmObjects);
            if (!table)
                return starts;
            for (uint32_t entry = table; entry < table + 0x10000u; entry += 8u)
            {
                if (readU32(rdram, entry) == 0xFFFFFFFFu)
                    break;
                starts.push_back(readU32(rdram, table + readU32(rdram, entry + 4u) * 4u));
            }
            std::sort(starts.begin(), starts.end());
            return starts;
        }

        uint32_t objectVariableStart(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t table = readU32(rdram, kVmObjects);
            for (uint32_t entry = table; table && entry < table + 0x10000u; entry += 8u)
            {
                const uint32_t hash = readU32(rdram, entry);
                if (hash == 0xFFFFFFFFu)
                    break;
                if (hash == object)
                    return readU32(rdram, table + readU32(rdram, entry + 4u) * 4u);
            }
            return 0xFFFFFFFFu;
        }

        // At the script's start: its variables as the file has them.
        void snapshotVariables(const uint8_t *rdram)
        {
            lessonNine().lessonsDone.clear();
            lessonList().initialVariables.clear();
            const uint32_t words = readU32(rdram, kVmVariables);
            const std::vector<uint32_t> starts = objectVariableStarts(rdram);
            if (!words || starts.empty())
                return;
            // The last object's variables: up to the file's count (TUTT.BIN's header).
            uint32_t count = starts.back() + 1u;
            if (auto script = readHostFile(estate().dir / "TUTT.BIN"); script && script->size() >= 12u)
                std::memcpy(&count, script->data() + 8, 4);
            if (count > 0x4000u)
                return;
            for (uint32_t i = 0; i < count; ++i)
                lessonList().initialVariables.push_back(readU32(rdram, words + i * 4u));
        }

        void restoreVariables(uint8_t *rdram, uint32_t object)
        {
            const uint32_t words = readU32(rdram, kVmVariables);
            const std::vector<uint32_t> starts = objectVariableStarts(rdram);
            const uint32_t first = objectVariableStart(rdram, object);
            if (!words || first == 0xFFFFFFFFu)
                return;
            uint32_t end = static_cast<uint32_t>(lessonList().initialVariables.size());
            for (uint32_t s : starts)
                if (s > first)
                {
                    end = std::min(end, s);
                    break;
                }
            for (uint32_t i = first + 1u; i < end; ++i) // the state word stays
                writeWord(rdram, words + i * 4u, lessonList().initialVariables[i]);
        }

        // The lesson running (1..25), 0 if none.
        int runningLesson(const uint8_t *rdram)
        {
            for (int lesson = 1; lesson <= 25; ++lesson)
            {
                const uint32_t state = scriptStateOf(rdram, lessonObject(lesson));
                if (state && state != kInit && state != kComplete)
                    return lesson;
            }
            return 0;
        }

        bool jumpToLesson(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int lesson)
        {
            const uint32_t target = lessonObject(lesson);
            if (!scriptStateOf(rdram, target) || lessonList().initialVariables.empty())
                return false;
            const uint32_t sp = (getRegU32(ctx, 29) - 0x800u) & ~15u;
            const uint32_t v0 = getRegU32(ctx, 2);
            lessonNine().jumping = true;
            // The lessons running end (normally one).
            for (int other = 1; other <= 25; ++other)
            {
                const uint32_t object = lessonObject(other);
                const uint32_t state = scriptStateOf(rdram, object);
                if (!state || state == kInit || state == kComplete)
                    continue;
                runtime->callGuestFunction(rdram, ctx, kDestroyNpcBubble, {object}, sp);
                runtime->callGuestFunction(rdram, ctx, lessonNine().setState, kSetState, {object, other == 1 ? kComplete : kInit}, sp);
            }
            restoreVariables(rdram, target);
            if (const uint32_t player = levelPlayer(rdram))
            {
                writeWord(rdram, player + 0x80u, readU32(rdram, player + 0x80u) | 4u); // respawn at a locator
                const std::string point = std::string("respawn_lesson") + kNumberNames[lesson - 1];
                runtime->callGuestFunction(rdram, ctx, kRespawnNamed, {player, hashOf(point.c_str()), 1u}, sp);
            }
            runtime->callGuestFunction(rdram, ctx, lessonNine().setState, kSetState, {target, lesson == 3 ? kLessonThreeTalk : kActive}, sp);
            // Its giver shown, as the career script's lesson enters do (the
            // free skate script's lesson 1 hides its giver in "complete" and
            // never shows it again).
            if (const uint32_t node = lesson == 11 || lesson == 22 ? 0u : nodeNamed(rdram, ctx, runtime, target, sp))
            {
                runtime->callGuestFunction(rdram, ctx, kNodeShow, {node, 1u, 1u}, sp - 0x100u);
                runtime->callGuestFunction(rdram, ctx, kNodeCollide, {node, 1u, 1u}, sp - 0x100u);
                runtime->callGuestFunction(rdram, ctx, kVisibleNote, {target, 1u, 1u}, sp - 0x100u);
            }
            lessonNine().jumping = false;
            setReturnU32(ctx, v0);
            std::cout << "[estate] lesson " << lesson << " (" << lessonObjectName(lesson) << ") from the pause menu"
                      << std::endl;
            return true;
        }

        // Without the PC pause items (PS2X_GAME_SETTINGS_MENUS unset: the
        // runner; the app has them) the lesson items call the game's own
        // inlinePauseJumpToTutorial (0x1BA398: "GoToLesson" + letter of its
        // argument less one, the Resume item); in Estate the jump above.
        constexpr uint32_t kRetailJump = 0x001BA398u;
        constexpr uint32_t kArgToInt = 0x0017FD00u; // (script value, int*)
        void retailJumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!active() || (readU32(rdram, kLevelIndex) & 0xFFu) != kTutorialIndex)
            {
                lessonList().retailJump(rdram, ctx, runtime);
                return;
            }
            const uint32_t sp = (getRegU32(ctx, 29) - 0x400u) & ~15u;
            writeWord(rdram, sp, 0xFFFFFFFFu);
            runtime->callGuestFunction(rdram, ctx, kArgToInt, {readU32(rdram, getRegU32(ctx, 4)), sp}, sp - 0x100u);
            const int item = static_cast<int>(readU32(rdram, sp)) - 1;
            if (item >= 0 && item < kMenuLessons)
                jumpToLesson(rdram, ctx, runtime, kMenuLesson[item]);
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- developer probes (measurements, off by default) ----------------
        //
        // PS2X_ESTATE_PROBE=<points>:<out>: AI's own collision under each
        // "x z" of <points> (ai_level.py ground --level TUT on a folder with
        // Estate's files: the reference), the topmost surface the skater's
        // swept query (FUN_00125BE0, as moveSkater calls it: kind 1, flags
        // 0xC, the collision world it was last given) finds on a vertical
        // segment from y -500 to 500 (+y is down), bisected to 1e-3; "x z y"
        // or "x z none" lines (ai_level.py compare reads them), once the
        // level has run 300 frames.
        // PS2X_ESTATE_GRINDS=<edges>:<out>: for each "ax ay az bx by bz kind"
        // of <edges> (ai_level.py edges), the game's rail search
        // (searchRails, FUN_00188AA8, as findGrindRail calls it) about the
        // edge's middle, radius 0.25, the edge's kind as the tag: "found"
        // when the rail it fills has the edge's ends, "near" another rail,
        // "none". Player one's position (the level object's player) is moved
        // there for the call and put back.
        constexpr uint32_t kSweepQuery = 0x00125BE0u;
        constexpr uint32_t kSearchRails = 0x00188AA8u; // (player, rail*, tag, 0; f12 radius, f13 min angle) -> found
        constexpr uint32_t kDrawRigid = 0x0010CE30u;
        constexpr uint32_t kLevelFrame = 0x00231CACu;
        constexpr uint32_t kPlayerOne = 0x002A7660u;   // Player (position at +0x90)
        constexpr uint32_t kDrawTerrain = 0x0010B520u;
        struct Probes
        {
            PS2Runtime::RecompiledFunction sweepQuery = nullptr;
            PS2Runtime::RecompiledFunction drawRigid = nullptr;
            PS2Runtime::RecompiledFunction drawTerrain = nullptr;
            uint32_t collisionWorld = 0;
            bool collisionProbed = false;
            bool grindsProbed = false;
            std::vector<std::pair<std::string, uint32_t>> props; // name, hash
            uint64_t frameDraws = 0;
            uint32_t drawFrame = 0;
            uint32_t drawFrames = 0;
            std::vector<uint64_t> drawCounts;
            PS2Runtime::RecompiledFunction destroyBubble = nullptr;
            PS2Runtime::RecompiledFunction overlayText = nullptr;
            PS2Runtime::RecompiledFunction createBubble = nullptr;
            std::atomic<int> bubblesDestroyed{0};
            std::atomic<int> overlayTexts{0};
            std::atomic<int> bubblesCreated{0};
            std::atomic<int> actionsToGiver{0}; // ACTION messages to actionGiver (the driver)
            uint32_t actionGiver = 0;
            PS2Runtime::RecompiledFunction findLocator = nullptr;
            bool locatorArmed = false;
            int locatorAsked = 0;
            uint32_t locatorMatrix[12] = {};
            uint32_t locatorPosition[4] = {};
            PS2Runtime::RecompiledFunction actionTarget = nullptr;
            PS2Runtime::RecompiledFunction postMessage = nullptr;
            int inAction = 0;
            int actionsSent = 0;
            PS2Runtime::RecompiledFunction nearestTalker = nullptr;
            PS2Runtime::RecompiledFunction sendTo = nullptr;
            int inNearest = 0;
            int nearestAsked = 0;
            uint32_t talkersProbedAt = 0; // PS2X_ESTATE_ACTION's probe: the level frame it ran at
            bool propsShown = false;      // PS2X_ESTATE_VISIBLE's dump: made
        };
        Probes &probes() { return ps2x::session::state<Probes>(); }

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }
        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = readU32(rdram, address);
            float f = 0.0f;
            std::memcpy(&f, &v, 4);
            return f;
        }
        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, 4);
        }
        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            uint32_t v = 0;
            std::memcpy(&v, &value, 4);
            writeWord(rdram, address, v);
        }
        bool splitPair(const char *spec, std::string &from, std::string &to)
        {
            // "a:b", the last ':' that is not a drive letter's.
            const std::string text(spec);
            const size_t colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0u || colon + 1u >= text.size() || (colon == 1u))
                return false;
            from = text.substr(0, colon);
            to = text.substr(colon + 1u);
            return true;
        }

        uint32_t levelPlayer(const uint8_t *rdram);

        void sweepQueryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 4) == 1u && getRegU32(ctx, 6) == 0xCu && getRegU32(ctx, 9))
                probes().collisionWorld = readU32(rdram, getRegU32(ctx, 9));
            probes().sweepQuery(rdram, ctx, runtime);
        }

        void probeCollision(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *spec)
        {
            std::string from, to;
            if (!splitPair(spec, from, to))
            {
                std::cerr << "[estate] PS2X_ESTATE_PROBE=<points>:<out>, not " << spec << std::endl;
                return;
            }
            std::ifstream in(from);
            std::ofstream out(to);
            const uint32_t base = (getRegU32(ctx, 29) - 0x800u) & ~15u;
            const uint32_t segment = base, hit = base + 0x20u, query = base + 0x240u, calleeSp = base - 0x100u;
            auto hits = [&](float x, float z, float top, float bottom)
            {
                writeFloat(rdram, segment + 0x00u, x);
                writeFloat(rdram, segment + 0x04u, top);
                writeFloat(rdram, segment + 0x08u, z);
                writeFloat(rdram, segment + 0x0Cu, 1.0f);
                writeFloat(rdram, segment + 0x10u, x);
                writeFloat(rdram, segment + 0x14u, bottom);
                writeFloat(rdram, segment + 0x18u, z);
                writeFloat(rdram, segment + 0x1Cu, 1.0f);
                writeWord(rdram, query, probes().collisionWorld);
                writeWord(rdram, query + 4u, 0u);
                uint32_t v0 = 0;
                runtime->callGuestFunction(rdram, ctx, probes().sweepQuery, kSweepQuery,
                                           {1u, segment, 0xCu, 0u, 0u, query, hit, 0u}, calleeSp, &v0);
                return v0 != 0u;
            };
            float x = 0.0f, z = 0.0f;
            size_t points = 0, found = 0;
            const auto start = std::chrono::steady_clock::now();
            while (in >> x >> z)
            {
                ++points;
                constexpr float kTop = -500.0f, kBottom = 500.0f;
                if (!hits(x, z, kTop, kBottom))
                {
                    out << x << " " << z << " none\n";
                    continue;
                }
                float lo = kTop, hi = kBottom; // [top, hi] hits, [top, lo] does not
                while (hi - lo > 1e-3f)
                {
                    const float mid = 0.5f * (lo + hi);
                    (hits(x, z, kTop, mid) ? hi : lo) = mid;
                }
                ++found;
                out << x << " " << z << " " << hi << "\n";
            }
            std::cout << "[estate] collision probe: " << points << " points, " << found << " with ground, "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s"
                      << std::endl;
        }

        void probeGrinds(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *spec)
        {
            std::string from, to;
            if (!splitPair(spec, from, to))
            {
                std::cerr << "[estate] PS2X_ESTATE_GRINDS=<edges>:<out>, not " << spec << std::endl;
                return;
            }
            std::ifstream in(from);
            std::ofstream out(to);
            const uint32_t base = (getRegU32(ctx, 29) - 0x800u) & ~15u;
            const uint32_t rail = base, calleeSp = base - 0x100u;
            const uint32_t player = levelPlayer(rdram) ? levelPlayer(rdram) : kPlayerOne;
            const uint32_t position = player + 0x90u;
            uint8_t saved[16];
            std::memcpy(saved, getConstMemPtr(rdram, position), sizeof(saved));
            size_t edges = 0, found = 0, near = 0;
            std::string line;
            const auto start = std::chrono::steady_clock::now();
            while (std::getline(in, line))
            {
                float a[3], b[3];
                int kind = 0;
                if (std::sscanf(line.c_str(), "%f %f %f %f %f %f %d", &a[0], &a[1], &a[2], &b[0], &b[1], &b[2], &kind) != 7)
                    continue;
                ++edges;
                for (int i = 0; i < 3; ++i)
                    writeFloat(rdram, position + 4u * i, (a[i] + b[i]) * 0.5f);
                writeFloat(rdram, position + 12u, 1.0f);
                for (uint32_t i = 0; i < 0x50u; i += 4u)
                    writeWord(rdram, rail + i, 0u);
                const float f12 = ctx->f[12], f13 = ctx->f[13];
                ctx->f[12] = 0.25f;
                ctx->f[13] = 0.0f;
                uint32_t hit = 0;
                runtime->callGuestFunction(rdram, ctx, kSearchRails, {player, rail, static_cast<uint32_t>(kind), 0u},
                                           calleeSp, &hit);
                ctx->f[12] = f12;
                ctx->f[13] = f13;
                float ends[2][3];
                for (int k = 0; k < 2; ++k)
                    for (int i = 0; i < 3; ++i)
                        ends[k][i] = readFloat(rdram, rail + 0x10u + 0x10u * static_cast<uint32_t>(k) + 4u * static_cast<uint32_t>(i));
                auto same = [](const float *p, const float *q)
                { return std::fabs(p[0] - q[0]) < 0.01f && std::fabs(p[1] - q[1]) < 0.01f && std::fabs(p[2] - q[2]) < 0.01f; };
                const bool match = hit && ((same(ends[0], a) && same(ends[1], b)) || (same(ends[0], b) && same(ends[1], a)));
                found += match;
                near += !match && hit;
                out << line << " " << (match ? "found" : hit ? "near" : "none") << "\n";
            }
            std::memcpy(getMemPtr(rdram, position), saved, sizeof(saved));
            std::cout << "[estate] grind probe: " << edges << " grindable edges, " << found
                      << " found by the game's rail search, " << near << " another rail there, " << edges - found - near
                      << " none; " << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                      << " s" << std::endl;
        }

        // PS2X_ESTATE_VISIBLE=1 (developers): the lesson props (instances
        // named *_L<n>) whose scene node is shown, by the node's visible bit
        // (what setObjectVisible sets), at frame 300 and with each lesson
        // driver line; and with it, the rigid and terrain renderers' calls
        // per frame for 120 frames from frame 300 (the drawn nodes).
        void readProps()
        {
            auto ins = readHostFile(estate().dir / "TUT.INS");
            if (!ins)
                return;
            const std::vector<uint8_t> &d = *ins;
            for (size_t at = 0; at + 8u + 0x60u <= d.size(); ++at)
            {
                if (std::memcmp(&d[at], "TGIH", 4) != 0)
                    continue;
                const char *name = reinterpret_cast<const char *>(&d[at + 8u + 0x50u]);
                const std::string n(name, strnlen(name, 0x20));
                const size_t l = n.rfind("_L");
                if (l != std::string::npos && l + 2u < n.size() && (std::isdigit(static_cast<unsigned char>(n[l + 2u])) || n.compare(l + 2u, 4, "cess") == 0))
                    probes().props.emplace_back(n, hashOf(n.c_str()));
            }
        }
        std::string visibleProps(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, size_t *count)
        {
            std::string out;
            size_t n = 0;
            const uint32_t sp = (getRegU32(ctx, 29) - 0x600u) & ~15u;
            for (const auto &[name, hash] : probes().props)
                if (const uint32_t node = nodeNamed(rdram, ctx, runtime, hash, sp); node && (readU32(rdram, node) & 1u))
                {
                    out += (n++ ? " " : "") + name;
                }
            if (count)
                *count = n;
            return out;
        }

        // PS2X_ESTATE_LESSONS=<out> (developers): the pause list's 23 lessons
        // played in turn as a player would start them: each started from
        // the pause list's jump (estateJumpToLessonItem: the lesson before
        // ended, the skater at its respawn point), then, for the 18 with a
        // giver (an ACTION handler in the state the jump enters), the
        // skater put 1.5 units from the giver facing it (the NPC data point
        // of the lesson's name) once its bubble is up, and Circle pressed
        // through the pad (6 frames every 40) until the lesson is given (its
        // giver's bubble taken down or the object's state changed) or 900
        // frames pass; the message boxes closed with Cross. The other five
        // (2, 11, 17, 19, 22) have no giver: watched 300 frames for their
        // own start (texts, state changes). A line per lesson, a summary at
        // the end.
        constexpr uint32_t kScriptTable = 0x002798C0u;
        constexpr uint32_t kScriptStates = 0x002798BCu;
        constexpr uint16_t kCircle = 0x2000u, kCross = 0x4000u;
        constexpr uint32_t kFindObjects = 0x001646B8u; // (hash*, n) -> the objects, in kObjectList
        constexpr uint32_t kObjectList = 0x0029D278u;
        constexpr uint32_t kSendMessage = 0x001528D8u; // (0, object, event hash, data, synchronous)
        const char *const kLessonNames[25] = {
            "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve", "Thirteen",
            "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen", "Twenty", "TwentyOne", "TwentyTwo",
            "TwentyThree", "TwentyFour", "TwentyFive"};
        // Lessons 11 and 22 are run by the bowl's quarter pipe objects (no assigner).
        const char *const kLessonObjects[25] = {
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            "scriptObj_bowl_qpipe_L11", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, "Scriptobj_bowl_qpipe_L22", nullptr, nullptr, nullptr};

        uint32_t hashOfText(const std::string &text) { return hashOf(text.c_str()); }

        uint32_t scriptState(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t table = readU32(rdram, kScriptTable), states = readU32(rdram, kScriptStates);
            if (!table || !states)
                return 0u;
            for (uint32_t entry = table; entry < table + 0x10000u; entry += 8u)
            {
                const uint32_t hash = readU32(rdram, entry);
                if (hash == 0xFFFFFFFFu)
                    break;
                if (hash == object)
                    return readU32(rdram, states + readU32(rdram, table + readU32(rdram, entry + 4u) * 4u) * 4u);
            }
            return 0u;
        }

        struct DataPoint
        {
            uint32_t kind = 0, name = 0;
            float position[3] = {};
        };
        std::vector<DataPoint> readDataPoints()
        {
            std::vector<DataPoint> points;
            auto bytes = readHostFile(estate().dir / "TUT.DAT");
            if (!bytes || bytes->size() < 8u)
                return points;
            const std::vector<uint8_t> &d = *bytes;
            for (size_t at = 8; at + 8u <= d.size();)
            {
                uint32_t size = 0;
                std::memcpy(&size, &d[at + 4u], 4);
                if (std::memcmp(&d[at], "SDPT", 4) == 0 && size >= 0x1Cu && at + 8u + size <= d.size())
                {
                    DataPoint p;
                    std::memcpy(&p.kind, &d[at + 12u], 4);
                    std::memcpy(&p.name, &d[at + 16u], 4);
                    std::memcpy(p.position, &d[at + 8u + 0x10u], 12);
                    points.push_back(p);
                }
                at += 8u + size;
            }
            return points;
        }

        // The natives a lesson's ACTION handler calls (its giver's
        // destroyNPCBubble, then displayOverlayText for its lines), counted.
        constexpr uint32_t kDestroyBubble = 0x001B69F0u;  // destroyNPCBubble
        constexpr uint32_t kOverlayText = 0x001B62A0u;    // displayOverlayText
        constexpr uint32_t kCreateBubble = 0x001B6960u;   // createNPCBubble
        void createBubbleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++probes().bubblesCreated;
            probes().createBubble(rdram, ctx, runtime);
        }
        void destroyBubbleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++probes().bubblesDestroyed;
            probes().destroyBubble(rdram, ctx, runtime);
        }
        void overlayTextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++probes().overlayTexts;
            probes().overlayText(rdram, ctx, runtime);
        }

        struct LessonDriver
        {
            int item = -1; // 0..22, -1 not started, 23 done
            int phase = 0;
            uint32_t lastFrame = 0, phaseFrame = 0;
            uint32_t object = 0, startState = 0;
            int boxes = 0, presses = 0;
            bool giver = false, placed = false;
            float giverAt[3] = {}, placedAt[3] = {};
            std::vector<DataPoint> points;
            std::ofstream out;
            int given = 0, givers = 0, ranAlone = 0;
        };
        LessonDriver &lessonDriver() { return ps2x::session::state<LessonDriver>(); }
        // Player one put at a point the way the game respawns it at a
        // locator (0x193EB0 with a name only the wrapped locator lookup
        // 0x187800 answers, as aggressive_inline_restart's retry does): its
        // orientation kept (the respawn negates the matrix's [1][1]).
        // (kRespawnNamed: 0x193EB0, above)
        constexpr uint32_t kFindLocator = 0x00187800u;  // (position*, hash, matrix*) -> found
        constexpr uint32_t kDriverLocator = hashOf("pcEstateLessonGiver");
        void findLocatorHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!probes().locatorArmed || getRegU32(ctx, 5) != kDriverLocator)
            {
                probes().findLocator(rdram, ctx, runtime);
                return;
            }
            ++probes().locatorAsked;
            probes().locatorArmed = false;
            const uint32_t out = getRegU32(ctx, 6);
            for (uint32_t i = 0; i < 12u; ++i)
                writeWord(rdram, out + i * 4u, probes().locatorMatrix[i] ^ (i == 5u ? 0x80000000u : 0u));
            for (uint32_t i = 0; i < 4u; ++i)
                writeWord(rdram, out + 0x30u + i * 4u, probes().locatorPosition[i]);
            setReturnU32(ctx, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }

        // Player one of the level object (the top state's +0x10, players
        // from +0x10).
        uint32_t levelPlayer(const uint8_t *rdram)
        {
            const uint32_t state = readU32(rdram, 0x0024C9BCu);
            const uint32_t level = state ? readU32(rdram, state + 0x10u) : 0u;
            return level ? level + 0x10u : 0u;
        }

        // Facing `face` (the giver: the Action reaches what is in front):
        // the orientation turned about the up axis so that its forward row
        // (row 2) points there.
        void writePosition(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float *at, const float *face)
        {
            const uint32_t player = levelPlayer(rdram);
            if (!player)
                return;
            float m[12];
            for (uint32_t i = 0; i < 12u; ++i)
                m[i] = readFloat(rdram, player + 0xB0u + i * 4u);
            const float tx = face[0] - at[0], tz = face[2] - at[2];
            const float fx = m[8], fz = m[10];
            if ((tx != 0.0f || tz != 0.0f) && (fx != 0.0f || fz != 0.0f))
            {
                const float a = std::atan2(tz, tx) - std::atan2(fz, fx);
                const float c = std::cos(a), sn = std::sin(a);
                for (int row : {0, 2})
                {
                    const float x = m[row * 4], z = m[row * 4 + 2];
                    m[row * 4] = x * c - z * sn;
                    m[row * 4 + 2] = x * sn + z * c;
                }
            }
            std::memcpy(probes().locatorMatrix, m, sizeof(m));
            const float position[4] = {at[0], at[1], at[2], 1.0f};
            std::memcpy(probes().locatorPosition, position, sizeof(position));
            writeWord(rdram, player + 0x80u, readU32(rdram, player + 0x80u) | 4u);
            // Armed until the respawn asks (it may look the locator up on a
            // later update).
            probes().locatorArmed = true;
            probes().locatorAsked = 0;
            const bool ok = runtime->callGuestFunction(rdram, ctx, kRespawnNamed, {player, kDriverLocator, 1u},
                                                       (getRegU32(ctx, 29) - 0x800u) & ~15u);
            if (estate().trace)
                std::cout << "[estate] respawn at " << at[0] << " " << at[1] << " " << at[2] << ": player 0x" << std::hex
                          << player << std::dec << ", call " << (ok ? "ran" : "failed") << ", locator asked "
                          << probes().locatorAsked << " times" << std::endl;
        }

        // The lessons whose state after the jump has an ACTION handler (a giver).
        bool lessonHasGiver(int lesson)
        {
            switch (lesson)
            {
            case 2: case 11: case 17: case 19: case 22: return false;
            default: return true;
            }
        }

        void driveLessons(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *outPath)
        {
            LessonDriver &g = lessonDriver();
            const uint32_t frame = readU32(rdram, kLevelFrame);
            // The 23 items, then lesson 1 again through the jump (a lesson played
            // once starts again).
            constexpr int kSteps = kMenuLessons + 1;
            if (frame == g.lastFrame || g.item >= kSteps)
                return;
            g.lastFrame = frame;
            const uint32_t mode = readU32(rdram, kGameMode);
            if (g.item < 0)
            {
                if (frame < 200u)
                    return;
                g.out.open(outPath);
                g.points = readDataPoints();
                g.item = 0;
                g.phase = 0;
            }
            // Message boxes (the game paused): closed with Cross.
            if (mode == 3u)
            {
                if ((frame / 4u) % 2u == 0u)
                    ++g.boxes;
                PSPadBackend::setOverrideButtons((frame / 4u) % 2u ? kCross : 0u);
                g.phaseFrame += 1u; // the wait does not count
                return;
            }
            const int item = g.item % kMenuLessons;
            const int lesson = kMenuLesson[item];
            const uint32_t elapsed = frame - g.phaseFrame;
            switch (g.phase)
            {
            case 0: // the pause list's jump
            {
                PSPadBackend::setOverrideButtons(0u);
                g.object = lessonObject(lesson);
                g.giver = g.placed = false;
                g.boxes = g.presses = 0;
                for (const DataPoint &p : g.points)
                    if (p.kind == 21315u && p.name == g.object)
                    {
                        g.giver = true;
                        std::memcpy(g.giverAt, p.position, sizeof(g.giverAt));
                    }
                probes().bubblesCreated = 0;
                probes().bubblesDestroyed = 0;
                probes().overlayTexts = 0;
                probes().actionsToGiver = 0;
                probes().actionGiver = g.object;
                // Lesson 1 as the level starts it (running since the start);
                // the rest, and lesson 1 again at the end, through the jump.
                const bool natural = g.item == 0 && runningLesson(rdram) == 1;
                if (!natural && !estateJumpToLessonItem(rdram, ctx, runtime, item))
                {
                    g.out << "lesson " << lesson << ": the jump failed\n";
                    ++g.item;
                    return;
                }
                g.startState = scriptState(rdram, g.object);
                g.phase = lessonHasGiver(lesson) ? 1 : 3;
                g.phaseFrame = frame;
                return;
            }
            case 1: // its giver's bubble up, then to the giver
                if (elapsed < 30u || (probes().bubblesCreated == 0 && elapsed < 300u))
                    return;
                if (g.giver)
                {
                    // 1.5 units out on the side the skater comes from (+x), facing it.
                    const float at[3] = {g.giverAt[0] + 1.5f, g.giverAt[1], g.giverAt[2]};
                    writePosition(rdram, ctx, runtime, at, g.giverAt);
                    g.placed = true;
                }
                g.phase = 2;
                g.phaseFrame = frame;
                return;
            case 2: // Circle until given
            {
                if (elapsed == 2u)
                    for (int i = 0; i < 3; ++i)
                        g.placedAt[i] = readFloat(rdram, levelPlayer(rdram) + 0x90u + 4u * static_cast<uint32_t>(i));
                const uint32_t state = scriptState(rdram, g.object);
                const bool given = probes().bubblesDestroyed > 0 || state != g.startState;
                if (!given && elapsed < 900u)
                {
                    const bool down = elapsed >= 60u && elapsed % 40u >= 20u && elapsed % 40u < 26u;
                    if (down && elapsed % 40u == 20u)
                        ++g.presses;
                    PSPadBackend::setOverrideButtons(down ? kCircle : 0u);
                    return;
                }
                PSPadBackend::setOverrideButtons(0u);
                ++g.givers;
                g.given += given;
                char line[400];
                std::snprintf(line, sizeof(line),
                              "%slesson %d %s: giver at %.1f %.1f %.1f, skater put at %.1f %.1f %.1f; %s by Circle after %d "
                              "presses (%u frames), ACTION to the giver %d, state %08x -> %08x, bubbles taken down %d, "
                              "overlay texts %d, message boxes %d\n",
                              g.item >= kMenuLessons ? "again: " : "", lesson, lessonObjectName(lesson).c_str(),
                              g.giverAt[0], g.giverAt[1], g.giverAt[2],
                              g.placedAt[0], g.placedAt[1], g.placedAt[2], given ? "given" : "NOT given", g.presses, elapsed,
                              probes().actionsToGiver.load(), g.startState, state, probes().bubblesDestroyed.load(), probes().overlayTexts.load(),
                              g.boxes);
                g.out << line;
                g.out.flush();
                std::cout << "[estate] " << line;
                g.phase = 4;
                g.phaseFrame = frame;
                return;
            }
            case 3: // no giver: its own start, watched
            {
                if (elapsed < 300u)
                    return;
                const uint32_t state = scriptState(rdram, g.object);
                const bool ran = probes().overlayTexts > 0 || state != g.startState;
                g.ranAlone += ran;
                char line[300];
                std::snprintf(line, sizeof(line),
                              "lesson %d %s: no giver; %s on its own (overlay texts %d, state %08x -> %08x, message boxes %d)\n",
                              lesson, lessonObjectName(lesson).c_str(), ran ? "ran" : "did NOT run", probes().overlayTexts.load(),
                              g.startState, state, g.boxes);
                g.out << line;
                g.out.flush();
                std::cout << "[estate] " << line;
                g.phase = 4;
                g.phaseFrame = frame;
                return;
            }
            case 4: // a pause between lessons
                if (elapsed < 60u)
                    return;
                ++g.item;
                g.phase = 0;
                if (g.item >= kSteps)
                {
                    char line[200];
                    std::snprintf(line, sizeof(line),
                                  "lessons: %d of %d with a giver given by Circle; %d of %d without one ran on their own\n",
                                  g.given, g.givers, g.ranAlone, kSteps - g.givers);
                    g.out << line;
                    g.out.flush();
                    std::cout << "[estate] " << line;
                }
                return;
            }
        }

        // PS2X_ESTATE_ACTION=1 (developers): the Action move's target search
        // (0x196BF8, the Ground and Air states' Action) traced: the
        // skater's place, the sphere query's finds, the ACTION messages
        // sent and the result.
        constexpr uint32_t kActionTarget = 0x00196BF8u;
        constexpr uint32_t kPostMessage = 0x00152910u;
        void actionTargetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t player = getRegU32(ctx, 4);
            const float x = readFloat(rdram, player + 0x90u), y = readFloat(rdram, player + 0x94u),
                        z = readFloat(rdram, player + 0x98u);
            ++probes().inAction;
            probes().actionsSent = 0;
            probes().actionTarget(rdram, ctx, runtime);
            --probes().inAction;
            std::cout << "[estate] action target: player 0x" << std::hex << player << std::dec << " at " << x << " " << y
                      << " " << z << " state " << static_cast<int>(rdram[player + 0x74u]) << ", -> "
                      << static_cast<int32_t>(getRegU32(ctx, 2)) << ", ACTION messages " << probes().actionsSent << " (frame "
                      << readU32(rdram, kLevelFrame) << ")" << std::endl;
        }
        // The nearest talker search (0x1BD1F8: position*, 0, f12 radius ->
        // the object): the "who is near" message 0x1528D8 sends each
        // listener of the list at gp-0x3AC8 (data {position, best, radius}).
        constexpr uint32_t kNearestTalker = 0x001BD1F8u;
        constexpr uint32_t kSendTo = 0x001528D8u;
        void nearestTalkerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t at = getRegU32(ctx, 4);
            const float radius = ctx->f[12];
            ++probes().inNearest;
            probes().nearestAsked = 0;
            probes().nearestTalker(rdram, ctx, runtime);
            --probes().inNearest;
            std::cout << "[estate]   nearest talker at " << readFloat(rdram, at) << " " << readFloat(rdram, at + 4u) << " "
                      << readFloat(rdram, at + 8u) << " radius " << radius << ": 0x" << std::hex << getRegU32(ctx, 2)
                      << std::dec << " (" << probes().nearestAsked << " listeners asked)" << std::endl;
        }
        void sendToHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (probes().inNearest)
            {
                ++probes().nearestAsked;
                const uint32_t data = getRegU32(ctx, 7);
                const uint32_t object = getRegU32(ctx, 5);
                probes().sendTo(rdram, ctx, runtime);
                std::cout << "[estate]     listener 0x" << std::hex << object << std::dec << ": best now 0x" << std::hex
                          << readU32(rdram, data + 0x10u) << std::dec << ", radius " << readFloat(rdram, data + 0x14u)
                          << ", delivered " << getRegU32(ctx, 2) << std::endl;
                return;
            }
            probes().sendTo(rdram, ctx, runtime);
        }
        void postMessageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t message = getRegU32(ctx, 4);
            if (readU32(rdram, message + 0x10u) == hashOf("action"))
            {
                const uint32_t receiver = readU32(rdram, message + 8u);
                uint32_t name = 0;
                runtime->callGuestFunction(rdram, ctx, kObjectNameHash, {receiver}, (getRegU32(ctx, 29) - 0x400u) & ~15u, &name);
                if (name && name == probes().actionGiver)
                    ++probes().actionsToGiver;
                ++probes().actionsSent;
                if (probes().inAction && std::getenv("PS2X_ESTATE_ACTION"))
                    std::cout << "[estate]   ACTION to 0x" << std::hex << receiver << " (0x" << name << ")" << std::dec
                              << std::endl;
            }
            probes().postMessage(rdram, ctx, runtime);
        }

        void drawRigidHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Any level (Estate, or another as a control: PS2X_ESTATE_PROBE_ANY=1).
            const bool any = std::getenv("PS2X_ESTATE_PROBE_ANY") != nullptr;
            if ((any || (active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex)) && readU32(rdram, kLevelFrame) >= 300u)
            {
                if (const char *spec = std::getenv("PS2X_ESTATE_PROBE"); spec && !probes().collisionProbed && probes().collisionWorld)
                {
                    probes().collisionProbed = true;
                    probeCollision(rdram, ctx, runtime, spec);
                }
                if (const char *spec = std::getenv("PS2X_ESTATE_GRINDS"); spec && !probes().grindsProbed)
                {
                    probes().grindsProbed = true;
                    probeGrinds(rdram, ctx, runtime, spec);
                }
            }
            if (std::getenv("PS2X_ESTATE_ACTION") && active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex &&
                probes().nearestTalker)
            {
                const uint32_t frame = readU32(rdram, kLevelFrame);
                if (frame >= 150u && frame != probes().talkersProbedAt && (frame == 150u || frame == 151u))
                {
                    probes().talkersProbedAt = frame;
                    const uint32_t sp = (getRegU32(ctx, 29) - 0x800u) & ~15u;
                    for (const DataPoint &p : readDataPoints())
                    {
                        if (p.kind != 21315u)
                            continue;
                        for (int i = 0; i < 3; ++i)
                            writeFloat(rdram, sp + 4u * static_cast<uint32_t>(i), p.position[i]);
                        writeFloat(rdram, sp + 12u, 1.0f);
                        const float f12 = ctx->f[12];
                        ctx->f[12] = 4.0f;
                        uint32_t found = 0;
                        std::cout << "[estate] probe: the nearest talker at NPC 0x" << std::hex << p.name << std::dec << std::endl;
                        runtime->callGuestFunction(rdram, ctx, kNearestTalker, {sp, 0u}, sp - 0x100u, &found);
                        ctx->f[12] = f12;
                    }
                }
            }
            if (const char *lessons = std::getenv("PS2X_ESTATE_LESSONS");
                lessons && active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex)
                driveLessons(rdram, ctx, runtime, lessons);
            if (std::getenv("PS2X_ESTATE_VISIBLE") && active() && (readU32(rdram, kLevelIndex) & 0xFFu) == kTutorialIndex)
            {
                const uint32_t frame = readU32(rdram, kLevelFrame);
                if (frame >= 300u && !probes().propsShown)
                {
                    probes().propsShown = true;
                    size_t n = 0;
                    const std::string names = visibleProps(rdram, ctx, runtime, &n);
                    std::cout << "[estate] lesson props shown at frame " << frame << ": " << n << " of " << probes().props.size()
                              << ": " << names << std::endl;
                }
                if (frame != probes().drawFrame)
                {
                    if (probes().drawFrame >= 300u && probes().drawCounts.size() < 120u)
                        probes().drawCounts.push_back(probes().frameDraws);
                    if (probes().drawCounts.size() == 120u)
                    {
                        uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
                        for (uint64_t c : probes().drawCounts)
                            lo = std::min(lo, c), hi = std::max(hi, c), sum += c;
                        std::cout << "[estate] node draws per frame, 120 frames from 300: min " << lo << ", max " << hi
                                  << ", mean " << static_cast<double>(sum) / 120.0 << std::endl;
                        probes().drawCounts.push_back(0); // done
                    }
                    probes().drawFrame = frame;
                    probes().frameDraws = 0;
                }
                ++probes().frameDraws;
            }
            probes().drawRigid(rdram, ctx, runtime);
        }

        void drawTerrainHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (std::getenv("PS2X_ESTATE_VISIBLE"))
                ++probes().frameDraws;
            probes().drawTerrain(rdram, ctx, runtime);
        }

        // The icons of one `SET Items {"a\nb\n..."}` line.
        std::vector<std::string> splitIcons(const std::string &list)
        {
            std::vector<std::string> icons;
            for (size_t start = 0;;)
            {
                const size_t end = list.find("\\n", start);
                icons.push_back(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 2u;
            }
            return icons;
        }

        std::string joinIcons(const std::vector<std::string> &icons)
        {
            std::string out;
            for (size_t i = 0; i < icons.size(); ++i)
                out += (i ? "\\n" : "") + icons[i];
            return out;
        }

        // The number after `key` at `at` ("VisibleCount\t8"), or -1.
        int numberAfter(const std::string &text, size_t at, size_t keySize)
        {
            size_t i = at + keySize;
            while (i < text.size() && (text[i] == '\t' || text[i] == ' ' || text[i] == '{'))
                ++i;
            if (i >= text.size() || text[i] < '0' || text[i] > '9')
                return -1;
            return std::atoi(text.c_str() + i);
        }

        // Its level-select icon (ICONS.UFNT, made by ps2xGcConvert from a
        // picture of the level: 'n' unselected, 'N' selected), appended to
        // the level font (FrontEndFonts' fnt_lvl_icons.zfn); without it the
        // Tutorial's cap ('a', 'A').
        const char kIconFontName[] = "fnt_lvl_icons.zfn";
        struct Art
        {
            std::shared_ptr<const std::vector<uint8_t>> iconFont;
            char iconOff = 'a';
            char iconOn = 'A';
            std::shared_ptr<const std::vector<uint8_t>> still;
            std::string stillName = "still_bkg_01";
        };
        Art &art() { return ps2x::session::state<Art>(); }
        // Its still behind the entry (STILL.ULIB, the same picture at 512 x
        // 256 as an asset of the FrontEnd library's kind), added to the
        // FrontEnd library; without it the Editor Level's (still_bkg_01).

        // FreeskateOnChange: Estate as a new case before the Editor Level's
        // (whatever the patches before made of the row: the PS2's eight, the
        // Zoo's nine), its icon (the Tutorial's cap) before the editor's in
        // every row; the row's visible count and position set (at most ten
        // shown, centred).
        bool patchLevelSelect(std::string &text)
        {
            zx::Span menu = zx::findObject(text, "FrontEnd/Background/LevelSelect/Menu");
            zx::Span script = zx::findObject(text, "FreeskateOnChange", menu);
            if (!menu.found() || !script.found())
            {
                std::cerr << "[estate] FrontEnd.zsc: the free skate level select was not found" << std::endl;
                return false;
            }
            std::string block = text.substr(script.begin, script.end - script.begin);
            const int depth = zx::indentAt(text, script.begin) + 2;
            const std::string tabs(static_cast<size_t>(depth), '\t');
            const std::string editorLevel = tabs + "SET .SelectedLevel {\"PEMovieLot\"}\r\n";
            const std::string abortMovie = tabs + "ABORT StartFlythruMovie \r\n";
            const std::string complete = tabs + ":Complete\r\n";
            const size_t editorAt = block.find(editorLevel);
            const size_t labelAt = editorAt == std::string::npos ? std::string::npos : block.rfind(tabs + ":", editorAt);
            if (labelAt == std::string::npos || block.find(abortMovie) == std::string::npos ||
                block.find(complete) == std::string::npos)
            {
                std::cerr << "[estate] FrontEnd.zsc: FreeskateOnChange is not the expected script" << std::endl;
                return false;
            }
            const size_t labelEnd = block.find("\r\n", labelAt);
            const int editorIndex = std::atoi(block.substr(labelAt + tabs.size() + 1u, labelEnd - labelAt).c_str());
            // Every icon row: the cap before the editor's (the last).
            const std::string itemsKey = "SET Items {\"";
            size_t rows = 0, icons = 0;
            for (size_t at = block.find(itemsKey); at != std::string::npos; at = block.find(itemsKey, at + 1u))
            {
                const size_t from = at + itemsKey.size();
                const size_t to = block.find("\"}", from);
                if (to == std::string::npos)
                    return false;
                std::vector<std::string> row = splitIcons(block.substr(from, to - from));
                if (row.size() < 2u)
                    return false;
                row.insert(row.end() - 1, std::string(1, art().iconOff));
                block.replace(from, to - from, joinIcons(row));
                icons = row.size();
                ++rows;
            }
            std::vector<std::string> ours;
            if (const size_t editorNow = block.find(editorLevel); editorNow != std::string::npos)
                if (const size_t at = block.find(itemsKey, editorNow); at != std::string::npos)
                {
                    const size_t from = at + itemsKey.size();
                    ours = splitIcons(block.substr(from, block.find("\"}", from) - from));
                }
            if (rows < 8u || ours.size() < 3u)
            {
                std::cerr << "[estate] FrontEnd.zsc: " << rows << " icon rows in FreeskateOnChange, no Estate" << std::endl;
                return false;
            }
            // Ours: the editor's row with every icon unselected but Estate's.
            for (std::string &icon : ours)
                for (char &c : icon)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            ours[ours.size() - 2u] = std::string(1, art().iconOn);
            const std::string selected = std::string("\"") + kSelectedLevel + "\"";
            const std::string cases = tabs + ":" + std::to_string(editorIndex) + "\r\n" + tabs + "SET .SelectedLevel {" +
                                      selected + "}\r\n" + tabs + "SET .LevelData.LevelName.TextLabel.Text {\"" +
                                      kShownName + "\"}\r\n" + tabs + "SET Items {\"" + joinIcons(ours) + "\"}\r\n" +
                                      tabs + "GOTO Done\r\n";
            const size_t labelAt2 = block.rfind(tabs + ":", block.find(editorLevel));
            const size_t labelEnd2 = block.find("\r\n", labelAt2) + 2u;
            block.replace(labelAt2, labelEnd2 - labelAt2, cases + tabs + ":" + std::to_string(editorIndex + 1) + "\r\n");
            const size_t abortAt = block.find(abortMovie);
            block.insert(abortAt + abortMovie.size(), tabs + "IF .SelectedLevel == {" + selected + "} GOTO EstateLevel\r\n");
            block.insert(block.find(complete), tabs + "GOTO Complete\r\n" + tabs + ":EstateLevel\r\n" + tabs +
                                                   "SET .LevelData.Data.isLocked {0}\r\n" + tabs +
                                                   "PLAY .LevelData.Data.Refresh\r\n" + tabs +
                                                   "CALL inlineMenuStopMovie\r\n" + tabs +
                                                   "SET Root.FrontEnd.Movie.Material {\"FrontEnd." + art().stillName + "\"}\r\n");
            text.replace(script.begin, script.end - script.begin, block);
            // At most ten icons visible (64 pixels a step on a 640-wide
            // screen; the PS2's eight start at 64), the row centred; a
            // longer row scrolls.
            menu = zx::findObject(text, "FrontEnd/Background/LevelSelect/Menu");
            const std::string menuTabs(static_cast<size_t>(zx::indentAt(text, menu.begin) + 1), '\t');
            const std::string visibleKey = menuTabs + "VisibleCount\t";
            const size_t visibleAt = text.find(visibleKey, menu.begin);
            const int visible = visibleAt != std::string::npos && visibleAt < menu.end
                                    ? numberAfter(text, visibleAt, visibleKey.size())
                                    : -1;
            const int shown = std::min<int>(static_cast<int>(icons), 10);
            if (visible < 0 || !zx::replaceIn(text, menu, visibleKey + std::to_string(visible) + "\r\n",
                                              visibleKey + std::to_string(shown) + "\r\n"))
                std::cerr << "[estate] FrontEnd.zsc: the level select's VisibleCount was not found" << std::endl;
            zx::Span levelSelect = zx::findObject(text, "FrontEnd/Background/LevelSelect");
            zx::Span setup = zx::findObject(text, "SetupForFreeskate", levelSelect);
            const std::string positionKey = "SET Menu.Position.x {";
            const size_t positionAt = setup.found() ? text.find(positionKey, setup.begin) : std::string::npos;
            const int position = positionAt != std::string::npos && positionAt < setup.end
                                     ? numberAfter(text, positionAt, positionKey.size())
                                     : -1;
            const int x = std::max(0, (640 - 64 * shown) / 2);
            if (position < 0 || !zx::replaceIn(text, setup, positionKey + std::to_string(position) + "}",
                                               positionKey + std::to_string(x) + "}"))
                std::cerr << "[estate] FrontEnd.zsc: the free skate level row's position was not found" << std::endl;
            estate().entry = editorIndex;
            std::cout << "[estate] free skate level select: Estate is entry " << editorIndex << " of " << icons
                      << std::endl;
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::ArchiveEntry> entries;
            std::string error;
            if (!zx::readArchive(bytes, entries, &error))
            {
                std::cerr << "[estate] UILIB.ZAL: " << error << std::endl;
                return false;
            }
            bool changed = false;
            // Its icon's page into the level font first (the rows name its
            // letters only when it is there; else the Tutorial's cap).
            art().iconOff = 'a';
            art().iconOn = 'A';
            for (zx::ArchiveEntry &entry : entries)
            {
                if (!art().iconFont || art().iconOff != 'a' || entry.data.size() < 4u || std::memcmp(entry.data.data(), "ULIB", 4) != 0)
                    continue;
                std::string why;
                if (zx::appendFontPages(entry.data, kIconFontName, *art().iconFont, &why))
                {
                    art().iconOff = ps2x::zaxis::estate::kIconUnselected;
                    art().iconOn = ps2x::zaxis::estate::kIconSelected;
                    changed = true;
                    std::cout << "[estate] its icon added to " << kIconFontName << std::endl;
                }
                else if (why != "the font is not in the library")
                    std::cerr << "[estate] its icon: " << why << " (" << kIconFontName << ")" << std::endl;
            }
            if (art().iconFont && art().iconOff == 'a')
                std::cerr << "[estate] its icon: " << kIconFontName << " not changed, the Tutorial's cap" << std::endl;
            // Its still into the FrontEnd library (the one with still_bkg_01).
            art().stillName = "still_bkg_01";
            for (zx::ArchiveEntry &entry : entries)
            {
                if (!art().still || art().stillName != "still_bkg_01" || entry.data.size() < 4u ||
                    std::memcmp(entry.data.data(), "ULIB", 4) != 0 ||
                    std::search(entry.data.begin(), entry.data.end(), art().stillName.begin(), art().stillName.end()) == entry.data.end())
                    continue;
                if (!zx::addAssets(entry.data, *art().still, {ps2x::zaxis::estate::kStillName}).empty())
                {
                    art().stillName = ps2x::zaxis::estate::kStillName;
                    changed = true;
                    std::cout << "[estate] its still added to the FrontEnd library" << std::endl;
                }
            }
            for (zx::ArchiveEntry &entry : entries)
            {
                if (zx::scriptName(entry) != "FrontEnd")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                // The FrontEnd the game loads (the other is an older one).
                if (text.find("\"FreeskateOnChange\"") == std::string::npos)
                    continue;
                if (patchLevelSelect(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                }
            }
            if (changed)
                bytes = zx::writeArchive(entries);
            return changed;
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = ps2x::boot::text("PS2X_ESTATE");
        const char *dir = ps2x::boot::text("PS2X_ESTATE_DIR");
        if (!on || on[0] != '1')
        {
            std::cout << "[estate] off (PS2X_ESTATE=1 and PS2X_ESTATE_DIR, made by ps2xGcConvert, add it to free skate)"
                      << std::endl;
            return;
        }
        if (!dir || !*dir)
        {
            std::cerr << "[estate] no PS2X_ESTATE_DIR (the files ps2xGcConvert made from the GameCube disc): off"
                      << std::endl;
            return;
        }
        estate().dir = dir;
        std::error_code ec;
        for (const char *file : kFiles)
            if (!fs::is_regular_file(estate().dir / file, ec))
            {
                std::cerr << "[estate] " << (estate().dir / file).string() << " is missing: off" << std::endl;
                return;
            }
        if (auto keys = readHostFile(estate().dir / "TEXTURES.TXT"))
            for (const ps2x::zaxis::estate::TextureKey &k :
                 ps2x::zaxis::estate::readTextureKeys(std::string(keys->begin(), keys->end())))
                estate().textureKeys.insert({k.width, k.height, k.indexHash});
        if (auto rules = readHostFile(estate().dir / "VISIBILITY.TXT"))
            for (const ps2x::zaxis::estate::VisibilityRule &r :
                 ps2x::zaxis::estate::readVisibilityRules(std::string(rules->begin(), rules->end())))
                showHide().visibility.insert({{r.object, r.state}, r});
        if (const char *trace = std::getenv("PS2X_TRACE_ESTATE"))
            estate().trace = std::atoi(trace);
        if (!(estate().levelFromName = runtime.lookupFunction(kLevelFromName)) ||
            !runtime.replaceFunction(kLevelFromName, &levelFromNameHook))
        {
            std::cerr << "[estate] the level-name lookup was not wrapped: off" << std::endl;
            return;
        }
        gf::registerProvider(kLevelFolder, &provide, "estate");
        ps2x::session::onEnd([]() { gf::removeProviders("estate"); });
        if (const char *own = std::getenv("PS2X_ESTATE_RECORDS"); own && own[0] == '0')
        {
            records().ownRecords = false;
            std::cout << "[estate] records: the Tutorial's row shared (PS2X_ESTATE_RECORDS=0)" << std::endl;
        }
        else
            addCareerBodyFilter(&filterCareerBody);
        if (!(lessonList().retailJump = runtime.lookupFunction(kRetailJump)) || !runtime.replaceFunction(kRetailJump, &retailJumpHook))
            std::cerr << "[estate] the Tutorial list's own jump was not wrapped" << std::endl;
        if (!npcs::installNpcStart(runtime) || !(givers().showObject = runtime.lookupFunction(kShowObject)) ||
            !runtime.replaceFunction(kShowObject, &showObjectHook))
            std::cerr << "[estate] the NPC constructor's hiding was not wrapped: Circle by a giver does nothing" << std::endl;
        if (!(showHide().startObjects = runtime.lookupFunction(kStartObjects)) ||
            !runtime.replaceFunction(kStartObjects, &startObjectsHook))
            std::cerr << "[estate] the script start was not wrapped: lessons 19, 23-25's props stay on" << std::endl;
        if (!showHide().visibility.empty() &&
            (!(showHide().messageHandler = runtime.lookupFunction(kMessageHandler)) ||
             !runtime.replaceFunction(kMessageHandler, &messageHandlerHook)))
            std::cerr << "[estate] the script message handler was not wrapped: lessons 19, 23-25's props stay on" << std::endl;
        if (!(lessonNine().setState = runtime.lookupFunction(kSetState)) || !runtime.replaceFunction(kSetState, &setStateHook))
            std::cerr << "[estate] the script state change was not wrapped: the lessons stop at lesson 9" << std::endl;
        if (std::getenv("PS2X_ESTATE_LESSONS") &&
            !((probes().findLocator = runtime.lookupFunction(kFindLocator)) && runtime.replaceFunction(kFindLocator, &findLocatorHook) &&
              (probes().destroyBubble = runtime.lookupFunction(kDestroyBubble)) &&
              runtime.replaceFunction(kDestroyBubble, &destroyBubbleHook) &&
              (probes().overlayText = runtime.lookupFunction(kOverlayText)) && runtime.replaceFunction(kOverlayText, &overlayTextHook) &&
              (probes().createBubble = runtime.lookupFunction(kCreateBubble)) && runtime.replaceFunction(kCreateBubble, &createBubbleHook)))
            std::cerr << "[estate] the locator lookup was not wrapped (the lesson driver)" << std::endl;
        if (std::getenv("PS2X_ESTATE_LESSONS") && !std::getenv("PS2X_ESTATE_ACTION") &&
            !((probes().postMessage = runtime.lookupFunction(kPostMessage)) && runtime.replaceFunction(kPostMessage, &postMessageHook)))
            std::cerr << "[estate] the ACTION count was not installed" << std::endl;
        if (std::getenv("PS2X_ESTATE_ACTION") &&
            !((probes().actionTarget = runtime.lookupFunction(kActionTarget)) && runtime.replaceFunction(kActionTarget, &actionTargetHook) &&
              (probes().postMessage = runtime.lookupFunction(kPostMessage)) && runtime.replaceFunction(kPostMessage, &postMessageHook) &&
              (probes().nearestTalker = runtime.lookupFunction(kNearestTalker)) &&
              runtime.replaceFunction(kNearestTalker, &nearestTalkerHook) && (probes().sendTo = runtime.lookupFunction(kSendTo)) &&
              runtime.replaceFunction(kSendTo, &sendToHook)))
            std::cerr << "[estate] the action trace was not installed" << std::endl;
        if (std::getenv("PS2X_ESTATE_VISIBLE"))
        {
            readProps();
            if (!((probes().drawTerrain = runtime.lookupFunction(kDrawTerrain)) && runtime.replaceFunction(kDrawTerrain, &drawTerrainHook)))
                std::cerr << "[estate] the terrain renderer was not wrapped" << std::endl;
        }
        if ((std::getenv("PS2X_ESTATE_PROBE") || std::getenv("PS2X_ESTATE_GRINDS") || std::getenv("PS2X_ESTATE_LESSONS") ||
             std::getenv("PS2X_ESTATE_VISIBLE")) &&
            !((probes().sweepQuery = runtime.lookupFunction(kSweepQuery)) && runtime.replaceFunction(kSweepQuery, &sweepQueryHook) &&
              (probes().drawRigid = runtime.lookupFunction(kDrawRigid)) && runtime.replaceFunction(kDrawRigid, &drawRigidHook)))
            std::cerr << "[estate] the probes' hooks were not installed" << std::endl;
        art().iconFont = readHostFile(estate().dir / "ICONS.UFNT");
        art().still = readHostFile(estate().dir / "STILL.ULIB");
        ps2x::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, "ai");
        runtime.addVSyncHook([](uint8_t *rdram, uint64_t vsync) { vsyncHook(rdram, vsync); });
        // Its scripts' texts stay as they are in another language: the
        // translations come from the Xbox release's level scripts (the
        // retail Tutorial's tut.bnf/.bng/.bns), not of these.
        for (const char *script : {"TUT.BIN", "TUTT.BIN"})
            if (auto bytes = readHostFile(estate().dir / script))
            {
                ps2x::zaxis_text::LevelText text;
                if (ps2x::zaxis_text::readLevelText(*bytes, text))
                    for (const auto &[offset, string] : text.strings)
                        if (!string.empty())
                            estate().scriptTexts.insert(string);
            }
        language::keepUntranslated([](const std::string &english) { return active() && estate().scriptTexts.count(english) != 0; });
        zge_host::xbox_textures::keepTexturesOff([](uint32_t width, uint32_t height, uint64_t indexHash)
                                                 {
                                                     if (!active() || !estate().textureKeys.count({width, height, indexHash}))
                                                         return false;
                                                     ++estate().keptOff;
                                                     return true;
                                                 });
        std::cout << "[estate] on: TUT/Estate from " << estate().dir.string() << " (" << estate().textureKeys.size()
                  << " textures kept off the Xbox pack, " << estate().scriptTexts.size()
                  << " script texts left untranslated, " << showHide().visibility.size()
                  << " show/hide calls from TUT.BIN) as the Tutorial's level 7 in free skate" << std::endl;
    }
}

namespace aggressive_inline
{
    void installEstate(PS2Runtime &runtime)
    {
        estate::install(runtime);
    }

    int estateLevelEntry()
    {
        return estate::estate().entry;
    }

    bool estateActive()
    {
        return estate::active();
    }

    int estateCurrentLessonItem(const uint8_t *rdram)
    {
        const int lesson = estate::runningLesson(rdram);
        for (int item = 0; item < estate::kMenuLessons; ++item)
            if (estate::kMenuLesson[item] == lesson)
                return item;
        return -1;
    }

    bool estateJumpToLessonItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int item)
    {
        if (item < 0 || item >= estate::kMenuLessons || !estate::active())
            return false;
        return estate::jumpToLesson(rdram, ctx, runtime, estate::kMenuLesson[item]);
    }

    std::string estateLessonCheckboxes(const uint8_t *)
    {
        std::string items;
        for (int item = 0; item < estate::kMenuLessons; ++item)
            items += estate::lessonNine().lessonsDone.count(estate::kMenuLesson[item]) ? "\\n`" : "\\n~";
        return items;
    }
}
