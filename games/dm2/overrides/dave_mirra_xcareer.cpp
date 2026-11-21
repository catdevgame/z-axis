// Dave Mirra Freestyle BMX 2 (SLUS-20159): the career (ProQuest) of DM2's
// Xbox-only levels, Venice and Greenville: their goals, gaps, rewards, local
// pros and texts, in Woodward Camp's slot, as their files are
// (dave_mirra_xboxlevel.cpp).
//
// The data is made at build time by tools/modding/dm2_xbox_career.py
// (called by dm2_xbox.py convert) from the Xbox executable's tables, which
// are the PS2's with two more levels: <ZX_DM2_XBOX_LEVELS_DIR>/CAREER.BIN.
// Nothing here reads a disc or converts anything. On with the Xbox levels
// (Settings > Game > Xbox Levels); ZX_DM2_XBOX_CAREER=0 turns it off.
//
// DM2's per-level tables have rows for eight levels (and the park editor):
//   goals 0x233900 (0x1B80 a level: 5 tiers x 4 goals x 0x160), gaps
//   0x231BD0 (0x280: 32 x 20; their count per level at 0x257CB8), tier
//   rewards 0x241500 (0x28), medal rewards 0x241640 (0x18), the all-gaps
//   reward 0x241810 (4), the local pros 0x2335C8 (100), the competition's
//   score scale 0x231BB0 (4);
// and each rider's record (0x2174B4 + rider * 0x3C8, 21 riders, saved in
// BMX2SAVE.DAT's first block) has the career of nine: respect at +0x90 +
// level * 4 (the total at +0xB0) and the progress at +0x13C + level * 0x44
// (+0x00 per goal {flags, part states}, +0x3C bits: 1 open, 2 competition
// open, 0x10 / 0x20 / 0x40 done; +0x3D tier, +0x3E goals done in it, +0x3F
// medal, +0x40 gaps found).
//
// While an Xbox level is in the slot (from its level start until the level
// select's value is no longer it, and for the level select's stats while it
// is shown), Woodward's rows of those tables, the gap count and every
// rider's Woodward respect and progress are the Xbox level's; Woodward's own
// are kept here and put back. The goal names the game's goal init (initLevelGoals)
// turns into hashes are written as names before it ran and as hashes after.
// The texts the game finds by the level number (goals, the pros' places, the
// level's name and instructions: "KSTRSP_GOAL_LEV02_EASY_0") are swapped in
// the string table in memory with the Xbox level's (added to STRING.EUS and
// the other languages under "ZX_XBOX<n>_<key>").
//
// Unlocks: the Xbox opens Venice in Airport's pro tier and its competition
// with Airport's bronze, Greenville in the Commercial District's (in place
// of Galloon, which Greenville opens). The PS2's rows stay as they are; the
// Xbox's rewards for its levels (CAREER.BIN's extras) go into a row's free
// slot (shown at the session's end as the Xbox shows them,
// KSTRSP_REWARD_LEVEL08 ...), else follow that row's first reward. A reward
// opening level 8 or 9 (the Xbox's numbers; the PS2 has none) sets the Xbox
// level's own progress (giveReward, the reward handler, is passed over).
// The ProQuest level select gets the Xbox levels as values 9 and 10 after
// Airport (MainMenu.SelectLevel skips 8, the park editor, in ProQuest), locked
// and opened from their progress as DM2's own.
//
// City rewards (all four levels of a city done, roundRewards over levels
// level & ~3 .. + 3) are zero on the Xbox for every city; for an Xbox level
// in the slot the city check is made to fail (level 0's tier read as 0 while
// it runs), so the Xbox levels never complete Woodward's city, and the
// "every level" rewards stay the PS2's (its eight levels).
//
// Saves: the Xbox levels' respect and progress per rider are saved with the
// game, beside it: <saves>/<profile>.XboxLevels.career (written when the game is
// saved, read when it is loaded), so BMX2SAVE.DAT / <profile>.sav is unchanged in
// size and content (Woodward's rows are Woodward's in it: saves and loads
// run with the slot's own rows in place). A save without that file loads with
// the Xbox levels locked.
//
// ZX_DM2_XCAREER_TRACE=1 logs swaps, rewards, goal parts and gaps.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_xcareer.cpp.h"

#include "machine.h"
#include "runtime/disc_patch.h"
#include "runtime/pad.h"
#include "runtime/save_store.h"
#include "runtime/zaxis_text.h"
#include "runtime/zaxis_ui.h"
#include "Stubs/MemoryCard.h"
#include "runtime/boot_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace dave_mirra::xcareer
{
    namespace checks
    {
        void note(const std::string &text);
    }
    namespace
    {
        namespace fs = std::filesystem;
        using zx::zaxis_text::keyHash;

        // Tables (CAREER.BIN's kinds).
        enum Kind : uint32_t
        {
            kGoals = 1,
            kGaps,
            kTierRewards,
            kMedalRewards,
            kGapsReward,
            kPros,
            kCompScale,
            kKinds
        };
        struct Table
        {
            uint32_t address, size;
        };
        constexpr Table kTables[kKinds] = {{0, 0},           {sym::dm2::g_goalDefinitions, 0x1B80u}, {sym::dm2::g_levelGapDefs, 0x280u},
                                           {sym::dm2::g_challengeRewards, 0x28u}, {sym::dm2::g_medalRewards, 0x18u}, {sym::dm2::g_allGapsRewards, 4u},
                                           {sym::dm2::g_npcDefinitions, 100u},  {sym::dm2::g_competitionTopScores, 4u}};
        constexpr uint32_t kSlot = 2u;                    // Woodward Camp
        constexpr uint32_t kGapCounts = sym::dm2::g_levelGapCounts;      // u8 per level
        constexpr uint32_t kGoalsHashed = sym::dm2::g_goalNamesHashed;    // set by the goal init once it hashed the names
        constexpr uint32_t kTierGoals = sym::dm2::g_challengeGoalCounts;      // u8[5]: goals per tier
        constexpr uint32_t kRiders = sym::dm2::g_riderCharacters, kRiderSize = 0x3C8u, kRiderCount = 21u;
        constexpr uint32_t kRespect = 0x90u, kProgress = 0x13Cu, kProgressSize = 0x44u;
        constexpr uint32_t kPlayers = sym::dm2::g_playerSetups;        // 0x11 a player, +3 its rider
        constexpr uint32_t kStringTable = sym::dm2::g_stringTable;    // u32: the string table in memory
        constexpr uint32_t kMenuToLevel = sym::dm2::g_levelMenuGameLevels;
        // Functions.
        constexpr uint32_t kLevelStart = sym::dm2::gameSetup;     // the level's start (gaps and goals loaded)
        constexpr uint32_t kSetupStats = sym::dm2::bmxSetupLevelStats;     // bmxSetupLevelStats
        constexpr uint32_t kReward = sym::dm2::giveReward;         // (player, reward*)
        constexpr uint32_t kTierDone = sym::dm2::roundRewards;       // (player): rewards for tiers / medals done
        constexpr uint32_t kPartState = sym::dm2::setObjectiveState;      // (player, goal, part, state)
        constexpr uint32_t kGapFound = sym::dm2::findGap;       // (take-off tag, landing tag) -> gap or 0xFF
        constexpr uint32_t kCardStart = sym::dm2::setUiFrame, kCardWrite = sym::dm2::startCardSave, kCardRead = sym::dm2::cardWorkDone;
        constexpr int kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kV0 = 2, kRa = 31;

        constexpr uint32_t kRelHashed = 1u;
        constexpr char kMagic[4] = {'X', 'L', 'C', 'R'};

        struct Rel
        {
            uint32_t offset, string, flags;
        };
        struct Block
        {
            std::vector<uint8_t> data;
            std::vector<Rel> rel;
        };
        struct Level
        {
            std::string folder;
            uint32_t xboxLevel = 0;
            Block blocks[kKinds];
            // The career per rider: progress then respect.
            std::vector<std::vector<uint8_t>> riders;
        };
        struct Text
        {
            int lang, level;
            std::string key, text;
        };
        struct Extra
        {
            uint32_t kind, level, row;
            uint16_t type, value;
        };

        // Woodward Camp's own rows while one of ours is in the slot.
        struct Host
        {
            std::vector<uint8_t> rows[kKinds];
            uint8_t gapCount = 0;
            bool goalsHashed = false;            // when Woodward's goal row was taken
            std::vector<std::vector<uint8_t>> riders;
        };

        // One boot's career: the levels read from CAREER.BIN, their texts
        // and rewards, the slot and the originals of the hooks. Made at the
        // boot's first use (the install), so a game booted after another in
        // the same process starts as a fresh one does (zx::boot::PerBoot).
        struct State
        {
            bool on = false, trace = false;
            std::vector<Level> levels;
            std::vector<std::string> strings;
            std::vector<uint32_t> stringAt; // guest addresses, once placed
            std::vector<Text> texts;
            std::vector<Extra> extras;
            std::map<uint32_t, std::vector<Extra>> following; // a reward slot -> the extras that follow it
            bool extrasPlaced = false;
            PS2Machine *runtime = nullptr;
            // The slot.
            Host host;
            int inSlot = -1;
            int guard = 0;
            std::vector<std::pair<uint32_t, uint32_t>> textSwaps; // (slot key, the level's key) hashes swapped
            // The hooks' originals.
            PS2Machine::RecompiledFunction reward = nullptr, checkCheat = nullptr, levelStart = nullptr,
                                           setupStats = nullptr, tierDone = nullptr, cardStart = nullptr,
                                           cardWrite = nullptr, cardRead = nullptr, partState = nullptr,
                                           gapFound = nullptr, sessionCommit = nullptr;
            bool cardSaves = false; // ZX_SAVES=card: the card's own write and read
            bool commitDone = false;
        };

        State &state()
        {
            static zx::boot::PerBoot<State> ofBoot([] { return State{}; });
            return ofBoot.get();
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, 4);
        }
        uint8_t rd8(const uint8_t *rdram, uint32_t a)
        {
            const uint8_t *p = getConstMemPtr(rdram, a);
            return p ? *p : 0u;
        }
        void wr8(uint8_t *rdram, uint32_t a, uint8_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                *p = v;
        }
        std::vector<uint8_t> readBytes(const uint8_t *rdram, uint32_t a, uint32_t n)
        {
            const uint8_t *p = getConstMemPtr(rdram, a);
            return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>(n, 0u);
        }
        void writeBytes(uint8_t *rdram, uint32_t a, const std::vector<uint8_t> &b)
        {
            if (uint8_t *p = getMemPtr(rdram, a); p && !b.empty())
                std::memcpy(p, b.data(), b.size());
        }
        std::string guestString(const uint8_t *rdram, uint32_t a)
        {
            std::string s;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, a + i);
                if (!p || !*p)
                    break;
                s += static_cast<char>(*p);
            }
            return s;
        }
        uint32_t riderOf(const uint8_t *rdram, uint32_t player)
        {
            return rd8(rdram, kPlayers + (player & 0xFFu) * 0x11u + 3u);
        }
        // The original to its end, then back to the caller: a hook with
        // work after the original runs it this way, as a host call (no
        // safe point inside it: the scheduler cannot unwind it half-way and
        // resume the rest later, after the work).
        void callWhole(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, PS2Machine::RecompiledFunction fn,
                       uint32_t address)
        {
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, fn, address,
                                       {getRegU32(ctx, kA0), getRegU32(ctx, kA1), getRegU32(ctx, kA2), getRegU32(ctx, kA3)},
                                       getRegU32(ctx, 29), &v0);
            setReturnU32(ctx, v0);
            ctx->pc = getRegU32(ctx, kRa);
        }
        uint32_t riderRecord(uint32_t rider) { return kRiders + rider * kRiderSize; }
        uint32_t slotProgress(uint32_t rider) { return riderRecord(rider) + kProgress + kSlot * kProgressSize; }
        uint32_t slotRespect(uint32_t rider) { return riderRecord(rider) + kRespect + kSlot * 4u; }

        int levelByFolder(const std::string &folder)
        {
            for (size_t i = 0; i < state().levels.size(); ++i)
                if (state().levels[i].folder == folder)
                    return static_cast<int>(i);
            return -1;
        }
        int levelByXbox(uint32_t xboxLevel)
        {
            for (size_t i = 0; i < state().levels.size(); ++i)
                if (state().levels[i].xboxLevel == xboxLevel)
                    return static_cast<int>(i);
            return -1;
        }
        int chosen()
        {
            const std::string folder = xboxlevel::chosenLevel();
            return folder.empty() ? -1 : levelByFolder(folder);
        }
        std::vector<uint8_t> freshRider() { return std::vector<uint8_t>(kProgressSize + 4u, 0u); }

        // ---- CAREER.BIN ----
        bool readCareer(const fs::path &file)
        {
            State &st = state();
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            size_t at = 0;
            bool ok = true;
            auto u32 = [&]() -> uint32_t
            {
                if (at + 4u > d.size())
                {
                    ok = false;
                    return 0u;
                }
                uint32_t v = 0;
                std::memcpy(&v, d.data() + at, 4);
                at += 4u;
                return v;
            };
            auto bytes = [&](size_t n) -> std::vector<uint8_t>
            {
                if (at + n > d.size())
                {
                    ok = false;
                    return {};
                }
                std::vector<uint8_t> b(d.begin() + static_cast<std::ptrdiff_t>(at),
                                       d.begin() + static_cast<std::ptrdiff_t>(at + n));
                at += n;
                return b;
            };
            if (d.size() < 16u || std::memcmp(d.data(), "DM2XCAR1", 8) != 0)
                return false;
            at = 8u;
            if (u32() != 1u)
                return false;
            const uint32_t levels = u32();
            for (uint32_t i = 0; ok && i < levels && i < 16u; ++i)
            {
                Level level;
                const std::vector<uint8_t> name = bytes(16u);
                level.folder.assign(reinterpret_cast<const char *>(name.data()), strnlen(reinterpret_cast<const char *>(name.data()), 16));
                level.xboxLevel = u32();
                const uint32_t blocks = u32();
                for (uint32_t b = 0; ok && b < blocks; ++b)
                {
                    const uint32_t kind = u32(), size = u32(), rels = u32();
                    Block block;
                    block.data = bytes(size);
                    for (uint32_t r = 0; ok && r < rels; ++r)
                    {
                        Rel rel{};
                        rel.offset = u32();
                        rel.string = u32();
                        rel.flags = u32();
                        block.rel.push_back(rel);
                    }
                    if (kind > 0u && kind < kKinds && size == kTables[kind].size)
                        level.blocks[kind] = std::move(block);
                    else
                        ok = false;
                }
                level.riders.assign(kRiderCount, freshRider());
                st.levels.push_back(std::move(level));
            }
            const uint32_t strings = u32();
            for (uint32_t i = 0; ok && i < strings; ++i)
            {
                const std::vector<uint8_t> s = bytes(u32());
                st.strings.emplace_back(s.begin(), s.end());
            }
            const uint32_t texts = u32();
            for (uint32_t i = 0; ok && i < texts; ++i)
            {
                Text t;
                t.lang = static_cast<int>(u32());
                t.level = static_cast<int>(u32());
                const std::vector<uint8_t> key = bytes(u32());
                const std::vector<uint8_t> text = bytes(u32());
                t.key.assign(key.begin(), key.end());
                t.text.assign(text.begin(), text.end());
                st.texts.push_back(std::move(t));
            }
            const uint32_t extras = u32();
            for (uint32_t i = 0; ok && i < extras; ++i)
            {
                Extra e{};
                e.kind = u32();
                e.level = u32();
                e.row = u32();
                const uint32_t tv = u32();
                e.type = static_cast<uint16_t>(tv & 0xFFFFu);
                e.value = static_cast<uint16_t>(tv >> 16);
                st.extras.push_back(e);
            }
            for (const Level &level : st.levels)
                for (uint32_t k = 1; k < kKinds; ++k)
                {
                    if (level.blocks[k].data.empty())
                        ok = false;
                    for (const Rel &r : level.blocks[k].rel)
                        if (r.string >= st.strings.size() || r.offset + 4u > kTables[k].size)
                            ok = false;
                }
            return ok && at == d.size();
        }

        // ---- the store: XboxLevels.career beside the game save ----
        fs::path storeFile()
        {
            dave_mirra::saves::adoptUnprofiledSaves();
            return dave_mirra::saves::profileFile(dave_mirra::saves::kXboxLevelsSuffix);
        }
        constexpr size_t kStoreRider = kProgressSize + 4u;
        bool validStore(const zx::saves::Bytes &d)
        {
            if (d.size() < 16u || std::memcmp(d.data(), kMagic, 4) != 0)
                return false;
            uint32_t version = 0, levels = 0, riders = 0;
            std::memcpy(&version, d.data() + 4, 4);
            std::memcpy(&levels, d.data() + 8, 4);
            std::memcpy(&riders, d.data() + 12, 4);
            return version == 1u && riders == kRiderCount && levels <= 16u &&
                   d.size() == 16u + static_cast<size_t>(levels) * (16u + kRiderCount * kStoreRider);
        }
        void writeStore()
        {
            State &st = state();
            zx::saves::Bytes d(kMagic, kMagic + 4);
            auto put = [&](uint32_t v)
            {
                const uint8_t *b = reinterpret_cast<const uint8_t *>(&v);
                d.insert(d.end(), b, b + 4);
            };
            put(1u);
            put(static_cast<uint32_t>(st.levels.size()));
            put(kRiderCount);
            for (const Level &level : st.levels)
            {
                char name[16] = {};
                std::memcpy(name, level.folder.data(), std::min<size_t>(level.folder.size(), 15u));
                d.insert(d.end(), name, name + 16);
                for (const auto &r : level.riders)
                    d.insert(d.end(), r.begin(), r.end());
            }
            std::string error;
            if (!zx::saves::writeFileAtomic(storeFile(), d.data(), d.size(), &error))
                std::cerr << "[xcareer] could not write " << storeFile().string() << ": " << error << std::endl;
            else if (st.trace)
                std::cout << "[xcareer] saved " << storeFile().string() << std::endl;
        }
        void openForChecks()
        {
            // ZX_DEV_UNLOCK_ALL=1 (developers, test runs; as Aggressive Inline's): every
            // rider has the Xbox levels open (their progress only; DM2's own are not touched).
            if (const char *open = zx::boot::text("ZX_DEV_UNLOCK_ALL"); open && open[0] == '1')
                for (Level &level : state().levels)
                    for (auto &r : level.riders)
                        r[0x3Cu] |= 1u;
        }
        void readStore()
        {
            State &st = state();
            for (Level &level : st.levels)
                level.riders.assign(kRiderCount, freshRider());
            openForChecks();
            const zx::saves::ReadResult r = zx::saves::readWithBackup(storeFile(), validStore);
            if (!r.ok())
            {
                std::cout << "[xcareer] no " << storeFile().filename().string()
                          << " with this save: the Xbox levels' career starts afresh" << std::endl;
                return;
            }
            uint32_t levels = 0;
            std::memcpy(&levels, r.data.data() + 8, 4);
            for (uint32_t i = 0; i < levels; ++i)
            {
                const uint8_t *at = r.data.data() + 16u + i * (16u + kRiderCount * kStoreRider);
                const std::string folder(reinterpret_cast<const char *>(at), strnlen(reinterpret_cast<const char *>(at), 16));
                const int l = levelByFolder(folder);
                if (l < 0)
                    continue;
                for (uint32_t k = 0; k < kRiderCount; ++k)
                    st.levels[l].riders[k].assign(at + 16u + k * kStoreRider, at + 16u + (k + 1u) * kStoreRider);
            }
            if (st.trace)
                std::cout << "[xcareer] loaded " << storeFile().string() << std::endl;
        }

        // ---- the slot ----

        // The game's goal init on one level's row: the parts' own names to
        // their hashes (initLevelGoals).
        void hashGoalRow(const uint8_t *rdram, std::vector<uint8_t> &row)
        {
            for (uint32_t tier = 0; tier < 5u; ++tier)
            {
                const uint32_t goals = rd8(rdram, kTierGoals + tier);
                for (uint32_t g = 0; g < goals && g < 4u; ++g)
                {
                    const uint32_t at = tier * 0x580u + g * 0x160u;
                    const uint32_t parts = row[at];
                    for (uint32_t p = 0; p < parts && p < 4u; ++p)
                    {
                        const uint32_t pa = at + 0x20u + p * 0x50u;
                        uint32_t type = 0;
                        std::memcpy(&type, row.data() + pa, 4);
                        std::vector<uint32_t> fields;
                        if (type >= 1u && type <= 3u)
                            fields = {0x10u};
                        else if (type == 6u || type == 7u)
                            fields = {0x08u};
                        else if ((type >= 8u && type <= 13u) || type == 23u)
                            fields = {0x04u};
                        else if (type >= 14u && type <= 21u)
                            fields = {0x04u, 0x08u};
                        for (uint32_t f : fields)
                        {
                            uint32_t v = 0;
                            std::memcpy(&v, row.data() + pa + f, 4);
                            if (v)
                                v = keyHash(guestString(rdram, v));
                            std::memcpy(row.data() + pa + f, &v, 4);
                        }
                    }
                }
            }
        }

        bool placeStrings(uint8_t *rdram)
        {
            State &st = state();
            if (!st.stringAt.empty())
                return true;
            uint32_t size = 0;
            for (const std::string &s : st.strings)
                size += static_cast<uint32_t>(s.size()) + 1u;
            const uint32_t base = st.runtime ? xboxlevel::allocateExtended(*st.runtime, size + 16u) : 0u;
            if (!base)
            {
                std::cerr << "[xcareer] no guest memory for the career's names" << std::endl;
                return false;
            }
            uint32_t at = base;
            for (const std::string &s : st.strings)
            {
                st.stringAt.push_back(at);
                std::vector<uint8_t> b(s.begin(), s.end());
                b.push_back(0u);
                writeBytes(rdram, at, b);
                at += static_cast<uint32_t>(b.size());
            }
            return true;
        }

        void swapTexts(uint8_t *rdram, bool in)
        {
            State &st = state();
            const uint32_t table = rd32(rdram, kStringTable);
            const uint32_t count = table ? rd32(rdram, table + 4u) : 0u;
            if (!table || count > 100000u)
                return;
            auto find = [&](uint32_t hash) -> uint32_t
            {
                for (uint32_t i = 0; i < count; ++i)
                    if (rd32(rdram, table + 8u + i * 8u) == hash)
                        return table + 8u + i * 8u;
                return 0u;
            };
            auto swapPair = [&](uint32_t a, uint32_t b)
            {
                const uint32_t ea = find(a), eb = find(b);
                if (!ea || !eb)
                    return false;
                const uint32_t oa = rd32(rdram, ea + 4u), ob = rd32(rdram, eb + 4u);
                wr32(rdram, ea + 4u, ob);
                wr32(rdram, eb + 4u, oa);
                return true;
            };
            if (!in)
            {
                for (const auto &[a, b] : st.textSwaps)
                    swapPair(a, b);
                st.textSwaps.clear();
                return;
            }
            const Level &level = st.levels[static_cast<size_t>(st.inSlot)];
            std::set<std::string> keys;
            for (const Text &t : st.texts)
                if (t.level == static_cast<int>(level.xboxLevel))
                    keys.insert(t.key);
            for (const std::string &key : keys)
            {
                const uint32_t a = keyHash(key);
                const uint32_t b = keyHash("ZX_XBOX" + std::to_string(level.xboxLevel) + "_" + key);
                if (swapPair(a, b))
                    st.textSwaps.emplace_back(a, b);
            }
        }

        void swapIn(uint8_t *rdram, int index, const char *why)
        {
            State &st = state();
            if (index < 0 || st.inSlot == index || st.guard || !placeStrings(rdram))
                return;
            if (st.inSlot >= 0)
                return; // another one first goes out (swapOut)
            const bool hashed = rd32(rdram, kGoalsHashed) != 0u;
            for (uint32_t k = 1; k < kKinds; ++k)
                st.host.rows[k] = readBytes(rdram, kTables[k].address + kSlot * kTables[k].size, kTables[k].size);
            st.host.gapCount = rd8(rdram, kGapCounts + kSlot);
            st.host.goalsHashed = hashed;
            st.host.riders.assign(kRiderCount, {});
            Level &level = st.levels[static_cast<size_t>(index)];
            for (uint32_t r = 0; r < kRiderCount; ++r)
            {
                std::vector<uint8_t> own = readBytes(rdram, slotProgress(r), kProgressSize);
                const std::vector<uint8_t> respect = readBytes(rdram, slotRespect(r), 4u);
                own.insert(own.end(), respect.begin(), respect.end());
                st.host.riders[r] = std::move(own);
                const std::vector<uint8_t> &mine = level.riders[r];
                writeBytes(rdram, slotProgress(r), std::vector<uint8_t>(mine.begin(), mine.begin() + kProgressSize));
                writeBytes(rdram, slotRespect(r), std::vector<uint8_t>(mine.begin() + kProgressSize, mine.end()));
            }
            for (uint32_t k = 1; k < kKinds; ++k)
            {
                std::vector<uint8_t> row = level.blocks[k].data;
                for (const Rel &rel : level.blocks[k].rel)
                {
                    const uint32_t v = (rel.flags & kRelHashed) && hashed ? keyHash(st.strings[rel.string]) : st.stringAt[rel.string];
                    std::memcpy(row.data() + rel.offset, &v, 4);
                }
                writeBytes(rdram, kTables[k].address + kSlot * kTables[k].size, row);
            }
            uint32_t gaps = 0;
            while (gaps < 32u && rd32(rdram, kTables[kGaps].address + kSlot * kTables[kGaps].size + gaps * 20u) != 0u)
                ++gaps;
            wr8(rdram, kGapCounts + kSlot, static_cast<uint8_t>(gaps));
            st.inSlot = index;
            swapTexts(rdram, true);
            if (st.trace)
                std::cout << "[xcareer] " << level.folder << "'s career in Woodward's slot (" << why << "; " << gaps
                          << " gaps, " << st.textSwaps.size() << " texts, goal names " << (hashed ? "hashed" : "as names")
                          << ")" << std::endl;
        }

        void swapOut(uint8_t *rdram, const char *why)
        {
            State &st = state();
            if (st.inSlot < 0)
                return;
            Level &level = st.levels[static_cast<size_t>(st.inSlot)];
            swapTexts(rdram, false);
            bool changed = false;
            for (uint32_t r = 0; r < kRiderCount; ++r)
            {
                std::vector<uint8_t> mine = readBytes(rdram, slotProgress(r), kProgressSize);
                const std::vector<uint8_t> respect = readBytes(rdram, slotRespect(r), 4u);
                mine.insert(mine.end(), respect.begin(), respect.end());
                changed = changed || mine != level.riders[r];
                level.riders[r] = std::move(mine);
                const std::vector<uint8_t> &own = st.host.riders[r];
                writeBytes(rdram, slotProgress(r), std::vector<uint8_t>(own.begin(), own.begin() + kProgressSize));
                writeBytes(rdram, slotRespect(r), std::vector<uint8_t>(own.begin() + kProgressSize, own.end()));
            }
            if (!st.host.goalsHashed && rd32(rdram, kGoalsHashed) != 0u)
                hashGoalRow(rdram, st.host.rows[kGoals]); // the init ran meanwhile, on our row in the slot
            for (uint32_t k = 1; k < kKinds; ++k)
                writeBytes(rdram, kTables[k].address + kSlot * kTables[k].size, st.host.rows[k]);
            wr8(rdram, kGapCounts + kSlot, st.host.gapCount);
            if (st.trace)
                std::cout << "[xcareer] Woodward's own back (" << why << "); " << level.folder << "'s career kept"
                          << (changed ? " (changed)" : "") << std::endl;
            st.inSlot = -1;
        }

        // ---- rewards ----
        // A reward for an Xbox level (open it, 8; open its competition, 9).
        bool xboxReward(uint8_t *rdram, uint32_t player, uint16_t type, uint16_t value)
        {
            State &st = state();
            if (type != 8u && type != 9u)
                return false;
            const int index = levelByXbox(value);
            if (index < 0)
                return false;
            const uint32_t rider = riderOf(rdram, player);
            if (rider >= kRiderCount)
                return true;
            const uint8_t bit = type == 8u ? 1u : 2u;
            if (index == st.inSlot)
                wr8(rdram, slotProgress(rider) + 0x3Cu, rd8(rdram, slotProgress(rider) + 0x3Cu) | bit);
            else
                st.levels[static_cast<size_t>(index)].riders[rider][0x3Cu] |= bit;
            if (type == 8u)
                wr32(rdram, riderRecord(rider), rd32(rdram, riderRecord(rider)) | 0x4000u); // as the handler does
            std::cout << "[xcareer] rider " << rider << ": " << st.levels[static_cast<size_t>(index)].folder
                      << (type == 8u ? " opened" : "'s competition opened") << std::endl;
            return true;
        }

        void rewardHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const uint32_t player = getRegU32(ctx, kA0), at = getRegU32(ctx, kA1);
            const uint32_t word = rd32(rdram, at);
            const uint16_t type = static_cast<uint16_t>(word & 0xFFFFu), value = static_cast<uint16_t>(word >> 16);
            if (st.trace && type)
                std::cout << "[xcareer] reward " << type << ":" << value << " (0x" << std::hex << at << std::dec
                          << ") for player " << player << std::endl;
            if (type)
                checks::note("reward " + std::to_string(type) + ":" + std::to_string(value));
            if (xboxReward(rdram, player, type, value))
            {
                ctx->pc = getRegU32(ctx, kRa);
                return;
            }
            const auto it = st.following.find(at);
            if (it == st.following.end())
            {
                st.reward(rdram, ctx, runtime);
                return;
            }
            callWhole(rdram, ctx, runtime, st.reward, kReward);
            if (it != st.following.end())
                for (const Extra &e : it->second)
                    xboxReward(rdram, player, e.type, e.value);
        }

        // The Xbox's rewards for its levels into the PS2 rows: a free slot
        // of the row (shown with the others), else after its first reward.
        void placeExtras(uint8_t *rdram)
        {
            State &st = state();
            if (st.extrasPlaced || rd8(rdram, kMenuToLevel + 8u) != 8u)
                return; // once, when the ELF's data is there
            st.extrasPlaced = true;
            for (const Extra &e : st.extras)
            {
                if (e.kind != kTierRewards && e.kind != kMedalRewards)
                    continue;
                const uint32_t row = kTables[e.kind].address + e.level * kTables[e.kind].size + e.row * 8u;
                const uint32_t word = e.type | (static_cast<uint32_t>(e.value) << 16);
                if (rd32(rdram, row + 4u) == 0u)
                    wr32(rdram, row + 4u, word);
                else if (rd32(rdram, row) == 0u)
                    wr32(rdram, row, word);
                else
                    st.following[row].push_back(e);
                if (st.trace)
                    std::cout << "[xcareer] reward " << e.type << ":" << e.value << " in level " << e.level << "'s "
                              << (e.kind == kTierRewards ? "tier " : "medal ") << e.row << " row"
                              << (st.following.count(row) ? " (after its first reward)" : "") << std::endl;
            }
        }

        // The cheats (bmxCheckCheat, a script native): the Xbox's "all
        // levels" and "everything" cheats (codes 7 and 8, the PS2's too) open
        // its ten levels for the first 17 riders; the PS2's open its eight.
        // When one of them has run (every one of those riders has the eight
        // open after it and not before), the Xbox levels open as well, for
        // the riders the cheats cover (cheats::ridersCovered: the created
        // riders too with Create Player); the Cheats page's Unlock Everything
        // opens them the same way (cheats::openXboxLevels).
        constexpr uint32_t kCheckCheat = sym::dm2::bmxCheckCheat;
        bool allEightOpen(const uint8_t *rdram)
        {
            for (uint32_t r = 0; r < 17u; ++r)
                for (uint32_t l = 0; l < 8u; ++l)
                    if (!(rd8(rdram, riderRecord(r) + kProgress + l * kProgressSize + 0x3Cu) & 1u))
                        return false;
            return true;
        }
        // Each Xbox level open (progress bit 1) for every rider the cheats
        // cover; true when there is one.
        bool openForCheat(uint8_t *rdram)
        {
            State &st = state();
            const uint32_t riders = std::min(cheats::ridersCovered(), kRiderCount);
            bool opened = false;
            for (size_t i = 0; i < st.levels.size(); ++i)
                for (uint32_t r = 0; r < riders; ++r)
                {
                    if (static_cast<int>(i) == st.inSlot)
                        wr8(rdram, slotProgress(r) + 0x3Cu, rd8(rdram, slotProgress(r) + 0x3Cu) | 1u);
                    else
                        st.levels[i].riders[r][0x3Cu] |= 1u;
                    opened = true;
                }
            return opened;
        }
        void checkCheatHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const bool before = allEightOpen(rdram);
            const std::vector<uint8_t> was = readBytes(rdram, kRiders, kRiderSize * 17u);
            callWhole(rdram, ctx, runtime, st.checkCheat, kCheckCheat);
            if (st.trace)
            {
                std::string keys;
                for (uint32_t i = 0; i < 32u; ++i)
                    if (const uint8_t c = rd8(rdram, sym::dm2::g_cheatKeys + i))
                        keys += static_cast<char>(c);
                std::cout << "[xcareer] cheat keys '" << keys << "'" << std::endl;
            }
            if (!allEightOpen(rdram) || (before && was == readBytes(rdram, kRiders, kRiderSize * 17u)))
                return; // nothing opened now
            if (openForCheat(rdram))
                std::cout << "[xcareer] the levels cheat: the Xbox levels open too, as on the Xbox" << std::endl;
        }

        // ---- hooks ----

        void levelStartHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const int want = chosen();
            if (st.inSlot >= 0 && st.inSlot != want)
                swapOut(rdram, "another level");
            if (want >= 0)
                swapIn(rdram, want, "level start");
            st.levelStart(rdram, ctx, runtime);
        }
        void setupStatsHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const int want = st.inSlot < 0 ? chosen() : -1;
            if (st.trace)
                std::cout << "[xcareer] level select stats: value " << rd32(rdram, sym::dm2::g_currentLevel) << (want >= 0 ? " (ours)" : "")
                          << std::endl;
            if (want >= 0)
                swapIn(rdram, want, "level select");
            callWhole(rdram, ctx, runtime, st.setupStats, kSetupStats);
            if (want >= 0)
                swapOut(rdram, "level select");
        }
        void tierDoneHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            if (st.inSlot < 0)
            {
                st.tierDone(rdram, ctx, runtime);
                return;
            }
            // Level 0 (Swamp Trails, the first of Woodward's city) read as
            // not done: the city check fails.
            const uint32_t rider = riderOf(rdram, getRegU32(ctx, kA0));
            const uint32_t at = riderRecord(rider) + kProgress + 0x3Du;
            const std::vector<uint8_t> kept = readBytes(rdram, at, 3u);
            writeBytes(rdram, at, {0u, 0u, 0u});
            callWhole(rdram, ctx, runtime, st.tierDone, kTierDone);
            writeBytes(rdram, at, kept);
        }
        void partStateHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            checks::note("goal " + std::to_string(getRegU32(ctx, kA1)) + " part " + std::to_string(getRegU32(ctx, kA2)) +
                             ": state " + std::to_string(getRegU32(ctx, kA3)));
            if (st.trace && st.inSlot >= 0)
                std::cout << "[xcareer] " << st.levels[static_cast<size_t>(st.inSlot)].folder << " goal "
                          << getRegU32(ctx, kA1) << " part " << getRegU32(ctx, kA2) << ": state " << getRegU32(ctx, kA3)
                          << " (tier " << int(rd8(rdram, slotProgress(riderOf(rdram, getRegU32(ctx, kA0))) + 0x3Du)) << ")"
                          << std::endl;
            st.partState(rdram, ctx, runtime);
        }
        void gapFoundHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            callWhole(rdram, ctx, runtime, st.gapFound, kGapFound);
            const uint32_t gap = getRegU32(ctx, kV0) & 0xFFu;
            if (gap != 0xFFu)
                checks::note("gap " + std::to_string(gap) + ": " +
                             guestString(rdram, rd32(rdram, kTables[kGaps].address + kSlot * kTables[kGaps].size + gap * 20u + 16u)));
            if (st.trace && st.inSlot >= 0 && gap != 0xFFu)
                std::cout << "[xcareer] " << st.levels[static_cast<size_t>(st.inSlot)].folder << " gap " << gap << ": "
                          << guestString(rdram, st.stringAt.empty() ? 0u : rd32(rdram, kTables[kGaps].address + kSlot * kTables[kGaps].size + gap * 20u + 16u))
                          << std::endl;
        }

        // The game save first commits the level's session into the
        // records (restartLevelGoals on the current level, when 0x25BA9C is 1
        // and the game type 0x257CB7 is 0): for an Xbox level that is its
        // own progress, so it runs here with the level still in the slot,
        // and the save's own call is passed over (it would run on
        // Woodward's row once that is back).
        constexpr uint32_t kSessionCommit = sym::dm2::restartLevelGoals;
        void sessionCommitHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (state().commitDone)
            {
                ctx->pc = getRegU32(ctx, kRa);
                return;
            }
            state().sessionCommit(rdram, ctx, runtime);
        }
        // A save or a load with Woodward's own rows in place; the Xbox
        // levels' career written or read with it.
        template <PS2Machine::RecompiledFunction State::*Original>
        void guarded(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, int op)
        {
            State &st = state();
            const int level = st.inSlot;
            if (level >= 0 && op != 2 && rd8(rdram, sym::dm2::g_nextGameState) == 1u && rd8(rdram, sym::dm2::g_singlePlayerMode) == 0u)
            {
                uint32_t v0 = 0;
                runtime->callGuestFunction(rdram, ctx, st.sessionCommit, kSessionCommit, {rd8(rdram, sym::dm2::g_currentPlayer)},
                                           (getRegU32(ctx, 29) - 0x400u) & ~0xFu, &v0);
                st.commitDone = true;
            }
            if (level >= 0)
                swapOut(rdram, op == 0 ? "save" : "load");
            ++st.guard;
            (st.*Original)(rdram, ctx, runtime);
            --st.guard;
            st.commitDone = false;
            if (op == 0)
                writeStore();
            else if (op == 2)
                readStore();
            if (level >= 0)
                swapIn(rdram, level, op == 0 ? "after the save" : "after the load");
        }
        void cardStartHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const uint32_t op = getRegU32(ctx, kA1) & 0xFFu;
            if (!st.cardSaves && getRegU32(ctx, kA0) != 0u && (op == 0u || op == 2u))
                guarded<&State::cardStart>(rdram, ctx, runtime, static_cast<int>(op));
            else if (st.cardSaves && (op == 0u || op == 2u) && st.inSlot >= 0)
                guarded<&State::cardStart>(rdram, ctx, runtime, -1);
            else
                st.cardStart(rdram, ctx, runtime);
        }
        void cardWriteHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const bool game = rd8(rdram, sym::dm2::g_cardFileKind) == 0u; // the game save (not a park or replay)
            if (state().cardSaves && game)
                guarded<&State::cardWrite>(rdram, ctx, runtime, 0);
            else
                guarded<&State::cardWrite>(rdram, ctx, runtime, -1);
        }
        void cardReadHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const bool game = rd8(rdram, sym::dm2::g_cardFileKind) == 0u;
            guarded<&State::cardRead>(rdram, ctx, runtime, state().cardSaves && game ? 2 : -1);
        }

        void vsync(uint8_t *rdram, uint64_t)
        {
            State &st = state();
            placeExtras(rdram);
            if (st.inSlot >= 0 && !st.guard && chosen() != st.inSlot)
                swapOut(rdram, "level left");
        }

        // ---- files ----
        int languageOf(const char *name)
        {
            static const char *kNames[] = {"STRING.EUS", "STRING.EUK", "STRING.GER", "STRING.FRE"};
            for (int i = 0; i < 4; ++i)
                if (std::strcmp(name, kNames[i]) == 0)
                    return i;
            return -1;
        }
        bool patchStrings(std::vector<uint8_t> &bytes, int lang)
        {
            zx::zaxis_text::StringTable table;
            if (!zx::zaxis_text::readStringTable(bytes, table))
                return false;
            auto set = [&](const std::string &key, const std::string &text, bool keep)
            {
                const uint32_t hash = keyHash(key);
                for (auto &entry : table.entries)
                    if (entry.first == hash)
                    {
                        if (!keep)
                            entry.second = text;
                        return;
                    }
                table.entries.emplace_back(hash, text);
            };
            size_t n = 0;
            for (const Text &t : state().texts)
            {
                if (t.lang != lang)
                    continue;
                if (t.level < 0)
                    set(t.key, t.text, false);
                else
                {
                    set(t.key, "", true); // the slot's key, so it can be swapped
                    set("ZX_XBOX" + std::to_string(t.level) + "_" + t.key, t.text, false);
                }
                ++n;
            }
            bytes = zx::zaxis_text::writeStringTable(table);
            if (state().trace)
                std::cout << "[xcareer] string table " << lang << ": " << n << " texts" << std::endl;
            return n != 0u;
        }

        // MainMenu.SelectLevel in ProQuest (ride types 0 and 1): values up
        // to 8 + the Xbox levels, 8 (the park editor) passed over.
        bool patchLevelSelect(std::string &text)
        {
            zx::zaxis::Span select = zx::zaxis::findObject(text, "MainMenu/SelectLevel");
            if (!select.found())
                return false;
            const std::string more = std::to_string(1u + state().levels.size());
            const std::string skip =
                "DECLARE String ZXTmp\\nSET ZXTmp {\\\"ZXR\\\"}\\nSTRCAT ZXTmp Root.CurrentRideType\\nGOTO ZXTmp\\n"
                "GOTO ZXSkipEnd\\n:ZXR0\\n:ZXR1\\nSET ZXTmp {\\\"ZXV\\\"}\\nSTRCAT ZXTmp CurValue\\nGOTO ZXTmp\\n"
                "GOTO ZXSkipEnd\\n:ZXV8\\nSET ZXTmp {\\\"ZXD\\\"}\\nSTRCAT ZXTmp LastDir\\nGOTO ZXTmp\\n"
                "SET CurValue {7}\\nGOTO ZXSkipEnd\\n:ZXD1\\nSET CurValue {9}\\n:ZXSkipEnd\\nUNDECLARE ZXTmp\\n";
            bool ok = zx::zaxis::replaceIn(text, select, "OnChange\t\"SET Root.CurrentLevel CurValue\\n",
                                    "OnChange\t\"" + skip + "SET Root.CurrentLevel CurValue\\n");
            ok = ok && zx::zaxis::replaceIn(text, select,
                                     ":1\\nSET Frame.ProquestInfo.Visible {1}\\nSET Frame.FreeRideInfo.Visible {0}\\n:End",
                                     ":1\\nSET Frame.ProquestInfo.Visible {1}\\nSET Frame.FreeRideInfo.Visible {0}\\nADD MaxValue {" +
                                         more + "}\\nDECLARE String ZXTmp\\nSET ZXTmp {\\\"ZXI\\\"}\\nSTRCAT ZXTmp CurValue\\n"
                                         "GOTO ZXTmp\\nGOTO ZXInitEnd\\n:ZXI8\\nSET CurValue {7}\\n:ZXInitEnd\\n"
                                         "UNDECLARE ZXTmp\\n:End");
            if (!ok)
                std::cerr << "[xcareer] MainMenu.SelectLevel is not the expected script: the Xbox levels not in ProQuest"
                          << std::endl;
            else
                std::cout << "[xcareer] level select: ProQuest values 9 to " << 8u + state().levels.size() << std::endl;
            return ok;
        }
        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            bool changed = false;
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
    }

    // ---- Career checks (developers): ZX_DM2_XCAREER_TEST=<script>:<results> ----
    //
    // In any level (an Xbox level with its career in the slot, or DM2's own
    // to compare), from the level's frame 300, the lines of <script> one
    // after another:
    //   points <file>       places (dm2_xbox_career.py points: "face|edge|object <name> x y z ...")
    //   press <button> [n]  a button held n frames (6; the level's instructions wait for Cross)
    //   give <goal>         the current tier's goal handed over, as a local pro does when
    //                       the rider talks to him (giveGoal(player, goal))
    //   tier <t>            (a shortcut) the earlier tiers' goals marked done, the tier t
    //   ride <from> <to> [back] [hop] [unless ...]   the rider put (DM2's own placement,
    //                       spawnPlayer, as a restart) <back> units before the take-off place
    //                       <from>, up its slope (else on the line from <to>; "line" keeps the
    //                       line), and ridden by the pad: pedal, steer, crouch and hop at the lip;
    //                       the game's own take-off, landing, gap and goal checks run
    //   hit <object> <dx> <dz> [back] | hit <object> here [unless ...]   ridden into an object
    //                       from <back> units on the side (dx, dz), or from where he is
    //   unless <gap> / unless goal <g> <part>   (at a line's end) skipped when found / done
    //   restart 0|1         later placements' restarts without (0) or with (1, the default)
    //                       the session's commit, which starts goals in progress over
    //   reward <level> <tier>, medal <level> <medal>   a PS2 level's reward row granted
    //                       through the game's reward handler (giveReward)
    //   career              the Xbox levels' open and competition bits
    //   save, load          the game's own save / load (setUiFrame op 0 / 2)
    //   wait <frames>, report
    // Results: each line, then the gaps found (findGap's answers), goal part states
    // set (setObjectiveState) and rewards while it ran, and the level's progress after it.
    namespace checks
    {
        constexpr uint32_t kPlace = sym::dm2::spawnPlayer, kSpawnLookup = sym::dm2::locatorMatrix, kRiderHandle = sym::dm2::riderIndexOf;
        constexpr uint32_t kRiderObjects = sym::dm2::g_riders, kPlayer = sym::dm2::g_currentPlayer, kLevelFrame = sym::dm2::g_physicsTaskFrames;
        constexpr uint32_t kGive = sym::dm2::giveGoal, kFrameReset = sym::dm2::resetDrawBuckets;
        constexpr uint32_t kPosition = 0x1040u, kVelocity = 0x10A0u;
        struct V
        {
            float x = 0, y = 0, z = 0;
        };
        V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V operator*(V a, float k) { return {a.x * k, a.y * k, a.z * k}; }
        struct Place
        {
            V at, top, normal; // a face's normal (objects: none)
        };
        constexpr uint32_t kCommit = sym::dm2::restartLevelGoals; // the session's commit a restart runs (restartLevelGoals)

        // One boot's check script and where it is (developers; made at the
        // boot's first use: zx::boot::PerBoot).
        struct CheckState
        {
            std::map<std::string, Place> places;
            std::vector<std::string> lines;
            size_t next = 0;
            std::ofstream out;
            std::string log;   // what happened while the current line ran
            int frame = 0;     // frames into the current line
            bool placing = false, on = false;
            uint32_t reset = 1u; // the placement's restart (1: the session's goals in progress start over)
            bool skipCommit = false;
            PS2Machine::RecompiledFunction commit = nullptr;
            V placeAt, heading{0, 0, 1};
            uint32_t playerOne = 0;
            PS2Machine::RecompiledFunction frameReset = nullptr, spawnLookup = nullptr;
            V from, to;
            // The trick line's frames: when the rider left the ground and
            // came down (-1: not yet), and the side a "hit" line places him on.
            int airFrame = -1, downFrame = -1;
            V hitSide{1, 0, 0};
        };

        CheckState &checkState()
        {
            static zx::boot::PerBoot<std::unique_ptr<CheckState>> ofBoot([] { return std::make_unique<CheckState>(); });
            return *ofBoot.get();
        }
        void commitHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (checkState().skipCommit)
            {
                note("the restart's session commit passed over");
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            checkState().commit(rdram, ctx, runtime);
        }

        void note(const std::string &text)
        {
            if (checkState().on)
                checkState().log += "  " + text + "\n";
        }
        void writeV(uint8_t *rdram, uint32_t a, V v)
        {
            const float f[4] = {v.x, v.y, v.z, 1.0f};
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, f, 12);
        }
        void spawnLookupHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            CheckState &cs = checkState();
            const uint32_t hash = getRegU32(ctx, 5), out = getRegU32(ctx, 6);
            cs.spawnLookup(rdram, ctx, runtime);
            if (!cs.placing || hash != cs.playerOne)
                return;
            const V f = cs.heading, u{0.0f, -1.0f, 0.0f};
            const V r{u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
            const float m[16] = {r.x, r.y, r.z, 0, u.x, u.y, u.z, 0, f.x, f.y, f.z, 0, cs.placeAt.x, cs.placeAt.y - 0.05f, cs.placeAt.z, 1};
            if (uint8_t *p = getMemPtr(rdram, out))
                std::memcpy(p, m, sizeof(m));
            setReturnU32(ctx, 1u);
        }
        uint32_t riderObject(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t sp)
        {
            uint32_t handle = 0;
            runtime->callGuestFunction(rdram, ctx, kRiderHandle, {rd8(rdram, kPlayer)}, sp, &handle);
            return rd32(rdram, kRiderObjects + 4u * (handle & 0xFFu));
        }
        void place(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t sp, V at, V heading)
        {
            CheckState &cs = checkState();
            cs.placeAt = at;
            const float l = std::sqrt(heading.x * heading.x + heading.z * heading.z);
            cs.heading = l > 1e-4f ? V{heading.x / l, 0, heading.z / l} : V{0, 0, 1};
            cs.placing = true;
            cs.skipCommit = cs.reset == 0u;
            runtime->callGuestFunction(rdram, ctx, kPlace, {rd8(rdram, kPlayer), 1u}, sp);
            cs.skipCommit = false;
            cs.placing = false;
        }
        std::string progressText(const uint8_t *rdram)
        {
            const uint32_t rider = riderOf(rdram, rd8(rdram, kPlayer)), level = rd8(rdram, sym::dm2::g_sessionLevel);
            const uint32_t at = riderRecord(rider) + kProgress + level * kProgressSize;
            char b[160];
            std::snprintf(b, sizeof(b), "progress: open/done bits 0x%02X, tier %d, goals done %d, medal %d, gaps 0x%08X; goals",
                          rd8(rdram, at + 0x3Cu), rd8(rdram, at + 0x3Du), rd8(rdram, at + 0x3Eu), rd8(rdram, at + 0x3Fu),
                          rd32(rdram, at + 0x40u));
            std::string s = b;
            for (uint32_t t = 0; t < 5u; ++t)
            {
                s += " |";
                for (uint32_t g = 0; g < 4u; ++g)
                {
                    std::snprintf(b, sizeof(b), " %02X:%02X%02X", rd8(rdram, at + t * 12u + g * 3u), rd8(rdram, at + t * 12u + g * 3u + 1u),
                                  rd8(rdram, at + t * 12u + g * 3u + 2u));
                    s += b;
                }
            }
            return s + "; respect " + std::to_string(rd32(rdram, riderRecord(rider) + kRespect + level * 4u)) + ", total " +
                   std::to_string(rd32(rdram, riderRecord(rider) + 0xB0u));
        }
        void finish(const uint8_t *rdram)
        {
            CheckState &cs = checkState();
            State &st = state();
            std::string own;
            if (st.inSlot >= 0 && !st.host.riders.empty())
            {
                // Woodward's own row (kept aside while an Xbox level is in its slot).
                const std::vector<uint8_t> &r = st.host.riders[riderOf(rdram, rd8(rdram, kPlayer)) % kRiderCount];
                char b[64];
                std::snprintf(b, sizeof b, "  Woodward's own: %02X:%02X%02X ... bits %02X tier %d", r[0], r[1], r[2], r[0x3C], r[0x3D]);
                own = std::string(b) + "\n";
            }
            cs.out << cs.lines[cs.next] << "\n" << cs.log << own << "  " << progressText(rdram) << "\n";
            cs.out.flush();
            cs.log.clear();
            cs.frame = 0;
            ++cs.next;
        }
        bool find(const std::string &name, Place &p)
        {
            const auto it = checkState().places.find(name);
            if (it == checkState().places.end())
                return false;
            p = it->second;
            return true;
        }
        // "unless <gap>" / "unless goal <g> <part>" at the end of a line:
        // the gap found, or that goal part of the current tier done (state 5).
        bool alreadyDone(const uint8_t *rdram, const std::string &line, std::string &why)
        {
            const size_t at = line.find(" unless ");
            if (at == std::string::npos)
                return false;
            std::istringstream l(line.substr(at + 8u));
            std::string first;
            l >> first;
            const uint32_t progress = riderRecord(riderOf(rdram, rd8(rdram, kPlayer))) + kProgress +
                                      rd8(rdram, sym::dm2::g_sessionLevel) * kProgressSize;
            if (first == "goal")
            {
                uint32_t g = 0, part = 0;
                l >> g >> part;
                const uint32_t tier = rd8(rdram, progress + 0x3Du);
                const uint8_t flags = rd8(rdram, progress + tier * 12u + g * 3u);
                const uint8_t states = rd8(rdram, progress + tier * 12u + g * 3u + 1u + part / 2u);
                const uint32_t state = (states >> ((part & 1u) * 4u)) & 7u;
                why = "goal " + std::to_string(g) + " part " + std::to_string(part) + " done";
                return (flags & 0x40u) || state == 5u;
            }
            const uint32_t gap = static_cast<uint32_t>(std::atoi(first.c_str()));
            why = "gap " + first + " found";
            return gap < 32u && (rd32(rdram, progress + 0x40u) >> gap & 1u);
        }
        void step(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            CheckState &cs = checkState();
            State &st = state();
            if (cs.next >= cs.lines.size() || rd32(rdram, kLevelFrame) < 300u || rd32(rdram, sym::dm2::g_gameMode) != 2u)
                return;
            const uint32_t sp = (getRegU32(ctx, 29) - 0x1000u) & ~15u;
            std::istringstream in(cs.lines[cs.next]);
            std::string op, a, b;
            in >> op >> a >> b;
            const int f = cs.frame++;
            if (op == "points")
            {
                std::ifstream file(a);
                std::string line;
                while (std::getline(file, line))
                {
                    std::istringstream l(line);
                    std::string kind, name;
                    Place p;
                    l >> kind >> name >> p.at.x >> p.at.y >> p.at.z >> p.top.x >> p.top.y >> p.top.z;
                    if (!l.fail())
                    {
                        if (kind != "object")
                        {
                            p.normal = p.top; // a face or an edge: its middle and normal
                            p.top = p.at;
                        }
                        cs.places[name] = p;
                    }
                }
                note(std::to_string(cs.places.size()) + " places");
                finish(rdram);
            }
            else if (op == "give")
            {
                runtime->callGuestFunction(rdram, ctx, kGive, {rd8(rdram, kPlayer), static_cast<uint32_t>(std::atoi(a.c_str()))}, sp);
                finish(rdram);
            }
            else if (op == "save" || op == "load")
            {
                // The game's own save or load (setUiFrame, what the menus call), no callbacks.
                wr32(rdram, sym::dm2::g_uiFrameRequest, 0u);
                runtime->callGuestFunction(rdram, ctx, kCardStart, {1u, op == "save" ? 0u : 2u, 0u}, sp);
                finish(rdram);
            }
            else if (op == "reward" || op == "medal")
            {
                // A PS2 level's tier (or medal) row granted through the game's reward handler.
                const uint32_t row = op == "reward" ? sym::dm2::g_challengeRewards + std::atoi(a.c_str()) * 0x28u + std::atoi(b.c_str()) * 8u
                                                    : sym::dm2::g_medalRewards + std::atoi(a.c_str()) * 0x18u + std::atoi(b.c_str()) * 8u;
                for (uint32_t i = 0; i < 2u; ++i)
                    runtime->callGuestFunction(rdram, ctx, kReward, {rd8(rdram, kPlayer), row + 4u * i}, sp);
                finish(rdram);
            }
            else if (op == "tier")
            {
                const uint32_t at = riderRecord(riderOf(rdram, rd8(rdram, kPlayer))) + kProgress +
                                    rd8(rdram, sym::dm2::g_sessionLevel) * kProgressSize;
                // The earlier tiers' goals marked done (flag 0x40), as the
                // level's session commit (restartLevelGoals, run at every restart)
                // works the tier out from them.
                const uint32_t tier = static_cast<uint32_t>(std::atoi(a.c_str()));
                for (uint32_t t = 0; t < tier && t < 5u; ++t)
                    for (uint32_t g = 0; g < rd8(rdram, kTierGoals + t) && g < 4u; ++g)
                        wr8(rdram, at + t * 12u + g * 3u, rd8(rdram, at + t * 12u + g * 3u) | 0x40u);
                wr8(rdram, at + 0x3Du, static_cast<uint8_t>(tier));
                wr8(rdram, at + 0x3Eu, 0u);
                finish(rdram);
            }
            else if (op == "wait")
            {
                if (f >= std::atoi(a.c_str()))
                    finish(rdram);
            }
            else if (op == "report")
                finish(rdram);
            else if (op == "career")
            {
                // The Xbox levels' open / competition bits for the current rider.
                const uint32_t rider = riderOf(rdram, rd8(rdram, kPlayer));
                for (size_t i = 0; i < st.levels.size(); ++i)
                {
                    const uint8_t bits = static_cast<int>(i) == st.inSlot ? rd8(rdram, slotProgress(rider) + 0x3Cu)
                                                                         : st.levels[i].riders[rider][0x3Cu];
                    char t[80];
                    std::snprintf(t, sizeof t, "%s: bits 0x%02X", st.levels[i].folder.c_str(), bits);
                    note(t);
                }
                finish(rdram);
            }
            else if (op == "restart")
            {
                // "restart 0": later placements are restarts without the
                // session's commit (restartLevelGoals, which starts the goals in
                // progress over), so a goal of several parts can be done
                // over several placements as a player does it in one ride;
                // "restart 1": whole restarts (the default).
                cs.reset = a == "0" ? 0u : 1u;
                finish(rdram);
            }
            else if (op == "ride")
            {
                // "ride <from> <to> [back] [hop]": the rider put <back> units
                // (12) before <from> on the line from <to> through it, facing
                // <to>; after 10 frames he pedals (Up) and steers toward <from>,
                // crouches (Cross held) from <hop> + 3 units before it and hops
                // (Cross let go) <hop> units before it (0: rides off it), and
                // once in the air nothing is held until he is down 90 frames
                // (or 460 frames in all; "... unless <gap>": not when that gap
                // is found already). Everything else is the game's.
                Place pa, pb;
                if (!find(a, pa) || !find(b, pb))
                {
                    note("no place for " + (find(a, pa) ? b : a));
                    finish(rdram);
                    return;
                }
                std::istringstream l(cs.lines[cs.next]);
                std::string s1, s2, s3;
                float back = 12.0f, hop = 0.0f;
                l >> s1 >> s2 >> s3;
                if (!(l >> back))
                    back = 12.0f;
                if (!(l >> hop))
                    hop = 0.0f;
                std::string why;
                if (f == 0 && alreadyDone(rdram, cs.lines[cs.next], why))
                {
                    note("skipped: " + why);
                    finish(rdram);
                    return;
                }
                // The run-up: up the take-off face's slope where it has one
                // (against its normal's level part; "line" after the numbers
                // keeps the line from <to>), else the line from <to>.
                V dir = pb.top - pa.top;
                dir.y = 0;
                const float nh = std::sqrt(pa.normal.x * pa.normal.x + pa.normal.z * pa.normal.z);
                if (nh > 0.2f && cs.lines[cs.next].find(" line") == std::string::npos)
                    dir = V{-pa.normal.x, 0, -pa.normal.z};
                const float len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                dir = len > 1e-3f ? dir * (1.0f / len) : V{0, 0, 1};
                const uint32_t rider = riderObject(rdram, ctx, runtime, sp);
                float pos[3] = {}, fwd[3] = {};
                if (const uint8_t *m = getConstMemPtr(rdram, rider + kPosition))
                    std::memcpy(pos, m, 12);
                if (const uint8_t *m = getConstMemPtr(rdram, rider + 0x1020u))
                    std::memcpy(fwd, m, 12);
                const uint8_t state = rd8(rdram, rider + 0xFF0u);
                if (f == 0)
                {
                    cs.airFrame = cs.downFrame = -1;
                    place(rdram, ctx, runtime, sp, pa.top - dir * back, dir);
                    return;
                }
                if (std::getenv("ZX_DM2_XCAREER_TEST_TRACE") && f % 5 == 0)
                {
                    char t[140];
                    float v[3] = {};
                    if (const uint8_t *m = getConstMemPtr(rdram, rider + kVelocity))
                        std::memcpy(v, m, 12);
                    std::snprintf(t, sizeof t, "f%d pos %.2f %.2f %.2f state %u speed %.2f", f, pos[0], pos[1], pos[2], state,
                                  std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
                    note(t);
                }
                uint16_t buttons = 0;
                const V toFrom{pa.top.x - pos[0], 0, pa.top.z - pos[2]};
                const float ahead = toFrom.x * dir.x + toFrom.z * dir.z;
                if (cs.airFrame < 0 && state != 0u && f > 10)
                {
                    cs.airFrame = f;
                    char t[100];
                    std::snprintf(t, sizeof t, "air at frame %d, %.2f before the take-off point", f, ahead);
                    note(t);
                }
                if (cs.airFrame >= 0 && cs.downFrame < 0 && state == 0u)
                {
                    cs.downFrame = f;
                    char t[120];
                    std::snprintf(t, sizeof t, "down at frame %d at %.2f %.2f %.2f (%.2f from the landing point)", f, pos[0],
                                  pos[1], pos[2], std::sqrt((pos[0] - pb.top.x) * (pos[0] - pb.top.x) + (pos[2] - pb.top.z) * (pos[2] - pb.top.z)));
                    note(t);
                }
                if (f > 10 && cs.airFrame < 0)
                {
                    // Up: pedal (ZX_DM2_XCAREER_PEDAL=<n>: n frames on, n off; 0 held).
                    static const int kTap = std::getenv("ZX_DM2_XCAREER_PEDAL") ? std::atoi(std::getenv("ZX_DM2_XCAREER_PEDAL")) : 0;
                    buttons = (kTap <= 0 || (f / kTap) % 2 == 0) ? 0x0010u : 0u;
                    if (hop > 0.0f && ahead < hop + 3.0f && ahead >= hop)
                        buttons |= 0x4000u; // Cross held (crouch) from 3 units before, let go (the hop) <hop> before
                    const float hx = fwd[0], hz = fwd[2], hl = std::sqrt(hx * hx + hz * hz);
                    V want = ahead > 1.0f ? toFrom * (1.0f / std::max(1e-3f, std::sqrt(toFrom.x * toFrom.x + toFrom.z * toFrom.z))) : dir;
                    if (hl > 1e-3f)
                    {
                        const float ux = hx / hl, uz = hz / hl;
                        const float cross = ux * want.z - uz * want.x, dot = ux * want.x + uz * want.z;
                        if (dot < 0.996f)
                            buttons |= cross > 0.0f ? 0x0080u : 0x0020u; // Left / Right
                    }
                }
                PSPadBackend::setOverrideButtons(buttons);
                if ((cs.downFrame >= 0 && f > cs.downFrame + 90) || f > 460)
                {
                    PSPadBackend::setOverrideButtons(0);
                    if (cs.airFrame < 0)
                        note("never in the air");
                    finish(rdram);
                }
            }
            else if (op == "hit")
            {
                // "hit <object> <dx> <dz> [back]": the rider put <back> units
                // (10) from the object's place on the side (dx, dz), facing it,
                // pedals and steers into it, until 3 units past it or 300
                // frames ("... unless goal <g> <part>" as ride).
                Place p;
                std::istringstream l(cs.lines[cs.next]);
                std::string s1, s2;
                float dx = 1, dz = 0, back = 10;
                l >> s1 >> s2;
                l >> dx >> dz;
                if (!(l >> back))
                    back = 10;
                std::string why;
                if (!find(a, p) || (f == 0 && alreadyDone(rdram, cs.lines[cs.next], why)))
                {
                    note(why.empty() ? "no place for " + a : "skipped: " + why);
                    finish(rdram);
                    return;
                }
                // "hit <object> here": from where the rider is, no placement
                // (no restart: the goal's other parts stay done).
                const bool here = s2 == "here" || cs.lines[cs.next].find(" here") != std::string::npos;
                const float dl = std::sqrt(dx * dx + dz * dz);
                const V target = p.at; // the object's origin (a gate's foot)
                const uint32_t rider = riderObject(rdram, ctx, runtime, sp);
                if (f == 0)
                {
                    if (here)
                    {
                        float q[3] = {};
                        if (const uint8_t *m = getConstMemPtr(rdram, rider + kPosition))
                            std::memcpy(q, m, 12);
                        const V d{q[0] - target.x, 0, q[2] - target.z};
                        const float l = std::sqrt(d.x * d.x + d.z * d.z);
                        cs.hitSide = l > 1e-3f ? d * (1.0f / l) : V{1, 0, 0};
                        return;
                    }
                    cs.hitSide = dl > 1e-4f ? V{dx / dl, 0, dz / dl} : V{1, 0, 0};
                    place(rdram, ctx, runtime, sp, target + cs.hitSide * back, cs.hitSide * -1.0f);
                    return;
                }
                const V side = cs.hitSide;
                float pos[3] = {}, fwd[3] = {};
                if (const uint8_t *m = getConstMemPtr(rdram, rider + kPosition))
                    std::memcpy(pos, m, 12);
                if (const uint8_t *m = getConstMemPtr(rdram, rider + 0x1020u))
                    std::memcpy(fwd, m, 12);
                const V to{target.x - pos[0], 0, target.z - pos[2]};
                const float ahead = -(to.x * side.x + to.z * side.z); // > 0 before the object
                uint16_t buttons = 0;
                if (f > 10)
                {
                    buttons = 0x0010u;
                    const float tl = std::sqrt(to.x * to.x + to.z * to.z), hl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
                    if (tl > 1.0f && hl > 1e-3f)
                    {
                        const float ux = fwd[0] / hl, uz = fwd[2] / hl, wx = to.x / tl, wz = to.z / tl;
                        if (ux * wx + uz * wz < 0.996f)
                            buttons |= (ux * wz - uz * wx) > 0.0f ? 0x0080u : 0x0020u;
                    }
                }
                PSPadBackend::setOverrideButtons(buttons);
                if ((f > 10 && ahead < -3.0f) || f > (here ? 600 : 300))
                {
                    PSPadBackend::setOverrideButtons(0);
                    char t[100];
                    std::snprintf(t, sizeof t, "ended at frame %d, %.2f from the object", f,
                                  std::sqrt(to.x * to.x + to.z * to.z));
                    note(t);
                    finish(rdram);
                }
            }
            else if (op == "press")
            {
                // A button held for 6 frames, then 24 frames off (the level's
                // instructions wait for Cross before the rider runs).
                // "press <button> [frames]": held that long (6), then 24 off.
                static const std::map<std::string, uint16_t> kButtons = {
                    {"CROSS", 0x4000u}, {"CIRCLE", 0x2000u}, {"SQUARE", 0x8000u}, {"TRIANGLE", 0x1000u}, {"START", 0x0008u},
                    {"UP", 0x0010u},    {"RIGHT", 0x0020u},  {"DOWN", 0x0040u},   {"LEFT", 0x0080u}};
                const auto it = kButtons.find(a);
                const int hold = b.empty() ? 6 : std::atoi(b.c_str());
                PSPadBackend::setOverrideButtons(f < hold && it != kButtons.end() ? it->second : 0u);
                if (f >= hold + 24)
                    finish(rdram);
            }
            else
            {
                note("unknown line");
                finish(rdram);
            }
        }
        void frameResetHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            step(rdram, ctx, runtime);
            checkState().frameReset(rdram, ctx, runtime);
        }
        void install(PS2Machine &runtime)
        {
            CheckState &cs = checkState();
            const char *spec = std::getenv("ZX_DM2_XCAREER_TEST");
            if (!spec || !*spec)
                return;
            std::string text(spec);
            size_t colon = text.find(':', 2u); // past a drive letter
            if (colon == std::string::npos)
                return;
            std::ifstream in(text.substr(0, colon));
            std::string line;
            while (std::getline(in, line))
                if (!line.empty() && line[0] != '#')
                    cs.lines.push_back(line);
            cs.out.open(text.substr(colon + 1u));
            cs.playerOne = keyHash("PlayerOne");
            if ((cs.commit = runtime.lookupFunction(kCommit)))
                runtime.replaceFunction(kCommit, &commitHook);
            if (!(cs.frameReset = runtime.lookupFunction(kFrameReset)) || !runtime.replaceFunction(kFrameReset, &frameResetHook) ||
                !(cs.spawnLookup = runtime.lookupFunction(kSpawnLookup)) || !runtime.replaceFunction(kSpawnLookup, &spawnLookupHook))
            {
                std::cerr << "[xcareer] checks: the hooks cannot be placed" << std::endl;
                return;
            }
            cs.on = true;
            std::cout << "[xcareer] checks: " << cs.lines.size() << " lines from " << text.substr(0, colon) << std::endl;
        }
    }

    // ZX_DM2_XCAREER_TABLES=<file> (developers): the eight PS2 levels'
    // rows of every career table and the riders' records, written at the
    // first level's frame 300 (ZX_DM2_XCAREER_TABLES_FRAME), to compare
    // runs with the Xbox levels on and off.
    namespace tables
    {
        // One boot's file and whether it is written (made at the boot's
        // first use: zx::boot::PerBoot).
        struct TablesState
        {
            std::string file;
            bool done = false;
        };

        TablesState &tablesState()
        {
            static zx::boot::PerBoot<TablesState> ofBoot([] { return TablesState{}; });
            return ofBoot.get();
        }

        void vsync(uint8_t *rdram, uint64_t)
        {
            static const uint32_t at = std::getenv("ZX_DM2_XCAREER_TABLES_FRAME")
                                           ? static_cast<uint32_t>(std::atoi(std::getenv("ZX_DM2_XCAREER_TABLES_FRAME")))
                                           : 300u;
            if (tablesState().done || rd32(rdram, sym::dm2::g_physicsTaskFrames) < at || rd32(rdram, sym::dm2::g_gameMode) != 2u)
                return;
            tablesState().done = true;
            std::ofstream out(tablesState().file, std::ios::binary);
            for (uint32_t k = 1; k < kKinds; ++k)
            {
                const std::vector<uint8_t> b = readBytes(rdram, kTables[k].address, kTables[k].size * 8u);
                out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
            }
            const std::vector<uint8_t> riders = readBytes(rdram, kRiders, kRiderSize * kRiderCount);
            out.write(reinterpret_cast<const char *>(riders.data()), static_cast<std::streamsize>(riders.size()));
            std::cout << "[xcareer] tables written to " << tablesState().file << " (level " << int(rd8(rdram, sym::dm2::g_sessionLevel)) << ")" << std::endl;
        }
    }

    bool install(PS2Machine &runtime)
    {
        State &st = state();
        if (const char *file = std::getenv("ZX_DM2_XCAREER_TABLES"); file && *file)
        {
            tables::tablesState().file = file;
            runtime.addVSyncHook(&tables::vsync);
        }
        st.trace = std::getenv("ZX_DM2_XCAREER_TRACE") != nullptr;
        if (std::getenv("ZX_DM2_XCAREER_TEST"))
        {
            // The checks' notes (developers), in any level.
            auto hook = [&](uint32_t at, PS2Machine::RecompiledFunction &original, PS2Machine::RecompiledFunction fn)
            { return (original = runtime.lookupFunction(at)) && runtime.replaceFunction(at, fn); };
            hook(kPartState, st.partState, &partStateHook);
            hook(kGapFound, st.gapFound, &gapFoundHook);
            checks::install(runtime);
        }
        const char *dir = zx::boot::text("ZX_DM2_XBOX_LEVELS_DIR");
        const char *on = zx::boot::text("ZX_DM2_XBOX_LEVELS");
        const char *career = std::getenv("ZX_DM2_XBOX_CAREER");
        if (!dir || !*dir || (on && on[0] == '0') || (career && career[0] == '0'))
            return false;
        const std::vector<std::string> folders = xboxlevel::levels();
        if (folders.empty())
            return false;
        if (!readCareer(fs::path(dir) / "CAREER.BIN"))
        {
            std::cerr << "[xcareer] no valid CAREER.BIN in " << dir << " (dm2_xbox.py convert): Free Ride only" << std::endl;
            st.levels.clear();
            return false;
        }
        // Only the levels the level select has (ZX_DM2_XBOX_LEVEL may keep one).
        st.levels.erase(std::remove_if(st.levels.begin(), st.levels.end(),
                                      [&](const Level &l)
                                      { return std::find(folders.begin(), folders.end(), l.folder) == folders.end(); }),
                       st.levels.end());
        // In the level select's order (values 9, 10).
        std::sort(st.levels.begin(), st.levels.end(), [&](const Level &a, const Level &b)
                  { return std::find(folders.begin(), folders.end(), a.folder) < std::find(folders.begin(), folders.end(), b.folder); });
        if (st.levels.empty())
            return false;
        st.runtime = &runtime;
        openForChecks();
        const char *saves = zx::boot::text("ZX_SAVES");
        st.cardSaves = saves && std::string(saves) == "card";
        auto hook = [&](uint32_t at, PS2Machine::RecompiledFunction &original, PS2Machine::RecompiledFunction fn)
        { return (original = runtime.lookupFunction(at)) && runtime.replaceFunction(at, fn); };
        if (!hook(kLevelStart, st.levelStart, &levelStartHook) || !hook(kSetupStats, st.setupStats, &setupStatsHook) ||
            !hook(kReward, st.reward, &rewardHook) || !hook(kTierDone, st.tierDone, &tierDoneHook) ||
            !hook(kSessionCommit, st.sessionCommit, &sessionCommitHook) || !hook(kCheckCheat, st.checkCheat, &checkCheatHook) || !hook(kCardStart, st.cardStart, &cardStartHook) || !hook(kCardWrite, st.cardWrite, &cardWriteHook) ||
            !hook(kCardRead, st.cardRead, &cardReadHook))
        {
            std::cerr << "[xcareer] the hooks cannot be placed: Free Ride only" << std::endl;
            return false;
        }
        if (st.trace && !st.partState)
        {
            hook(kPartState, st.partState, &partStateHook);
            hook(kGapFound, st.gapFound, &gapFoundHook);
        }
        for (const char *name : {"STRING.EUS", "STRING.EUK", "STRING.GER", "STRING.FRE"})
        {
            const int lang = languageOf(name);
            zx::disc_patch::registerPatch(name, [lang](std::vector<uint8_t> &b) { return patchStrings(b, lang); });
        }
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary);
        runtime.addVSyncHook(&vsync);
        st.on = true;
        std::cout << "[xcareer] on: ";
        for (size_t i = 0; i < st.levels.size(); ++i)
            std::cout << (i ? ", " : "") << st.levels[i].folder << " (Xbox level " << st.levels[i].xboxLevel << ")";
        std::cout << "; " << st.strings.size() << " names, " << st.texts.size() << " texts, " << st.extras.size()
                  << " rewards for them in the PS2 rows; career saved in " << storeFile().string() << std::endl;
        return true;
    }
}

namespace dave_mirra
{
    bool installXboxCareer(PS2Machine &runtime)
    {
        return xcareer::install(runtime);
    }

    void cheats::openXboxLevels(uint8_t *rdram)
    {
        if (xcareer::state().on && xcareer::openForCheat(rdram))
            std::cout << "[xcareer] Unlock Everything: the Xbox levels open too, as the levels cheat opens them" << std::endl;
    }
}
