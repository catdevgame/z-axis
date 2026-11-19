// The career (ProQuest) data of DM2's Xbox-only levels, Venice and
// Greenville, from the Xbox executable, as tools/dm2_xbox_career.py build
// writes CAREER.BIN (the runtime, game_overrides/dave_mirra_xcareer.cpp,
// reads that file).
//
// Career goals and gaps are not in the level files: they are tables in
// default.xbe (the US release: title 0x41430001, built 2001-11-09), the
// PS2's tables with two more levels. Per level (the Xbox's numbers: 0-7 as
// on the PS2, 8 Venice, 9 Greenville):
//
// | table          | at       | per level                                          |
// | goals          | 0x1A6090 | 0x1B80: 5 tiers x 4 goals x 0x160                  |
// | gaps           | 0x1A4330 | 0x280: 32 x {jump tag, land tag, flags, points, name} |
// | tier rewards   | 0x1B7390 | 0x28: 5 tiers x 2 x {u16 type, u16 value}           |
// | medal rewards  | 0x1B7520 | 0x18: 3 medals x 2 rewards                          |
// | all-gaps reward| 0x1B76D0 | 4                                                   |
// | local pros     | 0x1A5C78 | 100: 5 x {u32 rider, name, model, bike, tier, ...}  |
//
// A goal (0x160): u8 parts, u32 respect at +4, three entities at +8, then
// four parts of 0x50: u32 type, parameters, four entities at +0x14. The
// game's goal init turns some of each part's names into hashes once (types
// 1-3: +0x10; 6-7: +8; 8-13 and 23: +4; 14-21: +4 and +8); the entities
// stay strings. CAREER.BIN keeps each name as a string and where it goes.
//
// The PS2 levels' rows that open Venice or Greenville on the Xbox are
// written as "extra rewards"; the competition's score scale (no Xbox
// counterpart) is Airport's for Venice and Galloon's for Greenville. The
// texts: each language's keys the game formats with the level number, for
// Woodward Camp's slot (where the levels run), and the reward texts.
#include "dm2_xbox_parts.h"

namespace ps2x::app::extras::dm2
{
    namespace
    {
        constexpr char kMagic[] = "DM2XCAR1";
        constexpr uint32_t kVersion = 1;
        constexpr uint32_t kTitleId = 0x41430001;
        constexpr uint32_t kTimestamp = 0x3BEB77EF;

        enum Kind : uint32_t
        {
            Goals = 1,
            Gaps,
            TierRewards,
            MedalRewards,
            GapsReward,
            Pros,
            CompetitionScale,
        };
        struct Table
        {
            uint32_t address; // in default.xbe's address space
            uint32_t size;    // a level's row
        };
        Table tableOf(Kind kind)
        {
            switch (kind)
            {
            case Goals: return {0x1A6090, 0x1B80};
            case Gaps: return {0x1A4330, 0x280};
            case TierRewards: return {0x1B7390, 0x28};
            case MedalRewards: return {0x1B7520, 0x18};
            case GapsReward: return {0x1B76D0, 4};
            case Pros: return {0x1A5C78, 100};
            default: return {0, 0};
            }
        }
        const int kTierGoals[] = {1, 4, 4, 4, 1};
        const char *const kTierKeys[] = {"EASY", "HARD", "XTRA", "EXPERT"};
        constexpr size_t kGoalSize = 0x160, kPartSize = 0x50, kTierSize = 0x580;
        struct CareerLevel
        {
            int xboxLevel;
            const char *folder;
            uint32_t competitionScale;
        };
        const CareerLevel kCareerLevels[] = {{8, "VENICE", 200000}, {9, "GREENVIL", 145000}};
        constexpr int kSlot = 2; // Woodward Camp: the runtime's slot for the extra levels
        const char *const kLanguages[] = {"string.eus", "string.euk", "string.ger", "string.fre"};
        constexpr uint32_t kHashedName = 1; // the goal init turns this name into its hash

        // default.xbe as an address space (its sections' raw data).
        class Xbe
        {
        public:
            explicit Xbe(const Bytes &data) : m_data(data)
            {
                if (data.size() < 0x124 || std::memcmp(data.data(), "XBEH", 4) != 0)
                    throw Dm2Error("default.xbe is not an XBE");
                const uint32_t base = u32At(data, 0x104);
                const uint32_t count = u32At(data, 0x11C), headers = u32At(data, 0x120);
                const uint32_t certificate = u32At(data, 0x118) - base;
                title = u32At(data, certificate + 8);
                timestamp = u32At(data, 0x114);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const size_t at = headers - base + size_t(i) * 0x38;
                    const uint32_t name = u32At(data, at + 20) - base;
                    m_sections.push_back({cText(data, name), u32At(data, at + 4), u32At(data, at + 16), u32At(data, at + 12)});
                }
            }

            Bytes bytes(uint32_t va, uint32_t n) const
            {
                for (const Section &s : m_sections)
                    if (s.address <= va && uint64_t(va) + n <= uint64_t(s.address) + s.size)
                        return sliceOf(m_data, size_t(s.raw) + (va - s.address), size_t(s.raw) + (va - s.address) + n);
                char text[64];
                std::snprintf(text, sizeof text, "0x%X is not in the XBE's data", va);
                throw Dm2Error(text);
            }

            uint32_t u32(uint32_t va) const { return u32At(bytes(va, 4), 0); }

            // The C string at va when va is in .rdata or .data (printable, at
            // most 255 bytes), else none.
            std::optional<std::string> string(uint32_t va) const
            {
                for (const Section &s : m_sections)
                    if ((s.name == ".rdata" || s.name == ".data") && s.address <= va && va < s.address + s.size)
                    {
                        const size_t at = size_t(s.raw) + (va - s.address);
                        const size_t limit = std::min(m_data.size(), at + 256);
                        size_t end = at;
                        while (end < limit && m_data[end] != 0)
                            ++end;
                        if (end == limit)
                            return std::nullopt;
                        if (end == at)
                            return std::nullopt;
                        for (size_t i = at; i < end; ++i)
                            if (m_data[i] < 32 || m_data[i] >= 127)
                                return std::nullopt;
                        return std::string(m_data.begin() + long(at), m_data.begin() + long(end));
                    }
                return std::nullopt;
            }

            uint32_t title = 0, timestamp = 0;

        private:
            struct Section
            {
                std::string name;
                uint32_t address, size, raw;
            };
            const Bytes &m_data;
            std::vector<Section> m_sections;
        };

        // The strings CAREER.BIN lists once each, by first use.
        struct Strings
        {
            std::vector<std::string> items;
            std::map<std::string, uint32_t> index;
            uint32_t add(const std::string &s)
            {
                const auto it = index.find(s);
                if (it != index.end())
                    return it->second;
                index[s] = uint32_t(items.size());
                items.push_back(s);
                return uint32_t(items.size() - 1);
            }
        };

        // A name taken out of a row: where it was, which string, its flags.
        struct Relocation
        {
            uint32_t offset, string, flags;
        };
        struct Block
        {
            Kind kind;
            Bytes data;
            std::vector<Relocation> names;
        };

        // The offsets in a part of the names the goal init hashes.
        std::vector<size_t> goalStringFields(uint32_t type)
        {
            if (type >= 1 && type <= 3)
                return {0x10};
            if (type == 6 || type == 7)
                return {0x08};
            if ((type >= 8 && type <= 13) || type == 23)
                return {0x04};
            if (type >= 14 && type <= 21)
                return {0x04, 0x08};
            return {};
        }

        Block goalsBlock(const Xbe &xbe, int level, Strings &strings, std::map<std::string, long> &report)
        {
            const Table t = tableOf(Goals);
            Block block{Goals, xbe.bytes(t.address + uint32_t(level) * t.size, t.size), {}};
            Bytes &data = block.data;
            auto take = [&](size_t off, uint32_t flags)
            {
                const uint32_t va = u32At(data, off);
                if (va == 0)
                    return;
                const std::optional<std::string> s = xbe.string(va);
                if (!s)
                    throw Dm2Error("default.xbe's goal table names something that is not a name");
                putU32(data, off, 0);
                block.names.push_back({uint32_t(off), strings.add(*s), flags});
            };
            for (int tier = 0; tier < 5; ++tier)
                for (int g = 0; g < 4; ++g)
                {
                    const size_t at = size_t(tier) * kTierSize + size_t(g) * kGoalSize;
                    const int parts = data[at];
                    if (g >= kTierGoals[tier])
                    {
                        for (size_t i = at; i < at + kGoalSize; ++i)
                            if (data[i])
                                throw Dm2Error("default.xbe's goal table has a goal where none is expected");
                        continue;
                    }
                    if (!parts)
                        continue;
                    ++report["goals"];
                    for (size_t k = 0; k < 3; ++k) // the goal's entities
                        take(at + 8 + 4 * k, 0);
                    for (int p = 0; p < parts; ++p)
                    {
                        const size_t pa = at + 0x20 + size_t(p) * kPartSize;
                        ++report["parts"];
                        for (size_t off : goalStringFields(u32At(data, pa)))
                            take(pa + off, kHashedName);
                        for (size_t k = 0; k < 4; ++k) // the part's entities
                            take(pa + 0x14 + 4 * k, 0);
                    }
                    // Nothing else in the goal may look like a name: integers only.
                    for (size_t off = 0; off < kGoalSize; off += 4)
                    {
                        const uint32_t va = u32At(data, at + off);
                        if (va && xbe.string(va))
                            throw Dm2Error("default.xbe's goal table has a name where none is expected");
                    }
                }
            return block;
        }

        // A table row with string pointers at `fields`.
        Block stringsBlock(const Xbe &xbe, Kind kind, int level, const std::vector<size_t> &fields, Strings &strings)
        {
            const Table t = tableOf(kind);
            Block block{kind, xbe.bytes(t.address + uint32_t(level) * t.size, t.size), {}};
            for (size_t off : fields)
            {
                const uint32_t va = u32At(block.data, off);
                if (va == 0)
                    continue;
                const std::optional<std::string> s = xbe.string(va);
                if (!s)
                    throw Dm2Error("default.xbe's career tables name something that is not a name");
                putU32(block.data, off, 0);
                block.names.push_back({uint32_t(off), strings.add(*s), 0});
            }
            return block;
        }

        int gapCount(const Bytes &row)
        {
            int n = 0;
            while (n < 32 && u32At(row, size_t(n) * 20))
                ++n;
            return n;
        }

        std::vector<size_t> gapFields(const Bytes &row)
        {
            std::vector<size_t> out;
            for (int i = 0; i < gapCount(row); ++i)
                for (size_t off : {0, 4, 16})
                    out.push_back(size_t(i) * 20 + off);
            return out;
        }

        std::vector<size_t> proFields()
        {
            std::vector<size_t> out;
            for (size_t i = 0; i < 5; ++i)
                for (size_t off : {4, 8, 12})
                    out.push_back(i * 0x14 + off);
            return out;
        }

        // {key hash: text} of a string table (offset 0: empty; a key seen
        // again: the later text).
        std::map<uint32_t, Bytes> stringTable(const Bytes &data)
        {
            std::map<uint32_t, Bytes> out;
            const uint32_t count = u32At(data, 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t key = u32At(data, 8 + 8 * size_t(i)), at = u32At(data, 12 + 8 * size_t(i));
                if (!at)
                {
                    out[key] = {};
                    continue;
                }
                const auto end = std::find(data.begin() + std::min<long>(at, long(data.size())), data.end(), 0);
                if (end == data.end())
                    throw Dm2Error("a string table's text has no end");
                out[key] = Bytes(data.begin() + at, end);
            }
            return out;
        }

        std::string keyOf(const char *pattern, int level)
        {
            char text[64];
            std::snprintf(text, sizeof text, pattern, level);
            return text;
        }

        struct Text
        {
            int32_t language, level; // level -1: a fixed key
            std::string key;
            Bytes text;
        };

        // The slot keys (Woodward's, the level number 02) for each level, and
        // the fixed reward keys, in each language.
        std::vector<Text> texts(const SourceFiles &xbox)
        {
            std::vector<Text> out;
            for (int32_t language = 0; language < 4; ++language)
            {
                const std::map<uint32_t, Bytes> table = stringTable(need(xbox, kLanguages[language], "the Xbox files"));
                for (const CareerLevel &level : kCareerLevels)
                {
                    std::vector<std::string> keys = {"KSTR_LEVELNAME_%02d", "KSTRSP_INST_LEV%02d", "KSTRSP_INST_COMP_LEV%02d",
                                                     "KSTRSP_INST_SESSION_LEV%02d", "KSTRSP_GOAL_LEV%02d_ROOKIE"};
                    for (const char *tier : kTierKeys)
                        for (int i = 0; i < 4; ++i)
                            keys.push_back(std::string("KSTRSP_GOAL_LEV%02d_") + tier + "_" + std::to_string(i));
                    for (int i = 0; i < 4; ++i)
                        keys.push_back("KSTRSP_NPC_LOCATION_LEV%02d_" + std::to_string(i));
                    for (const std::string &key : keys)
                    {
                        const auto it = table.find(nameHash(keyOf(key.c_str(), level.xboxLevel)));
                        out.push_back({language, level.xboxLevel, keyOf(key.c_str(), kSlot),
                                       it == table.end() ? Bytes() : it->second});
                    }
                    for (const char *key : {"KSTRSP_REWARD_LEVEL%02d", "KSTRSP_REWARD_CONTEST_LEV%02d"})
                    {
                        const auto it = table.find(nameHash(keyOf(key, level.xboxLevel)));
                        if (it != table.end() && !it->second.empty())
                            out.push_back({language, -1, keyOf(key, level.xboxLevel), it->second});
                    }
                }
            }
            return out;
        }

        struct ExtraReward
        {
            uint32_t kind, level, row;
            uint16_t type, value;
        };

        // The eight PS2 levels' rewards that open an Xbox level or its competition.
        std::vector<ExtraReward> extraRewards(const Xbe &xbe)
        {
            std::vector<ExtraReward> out;
            for (const auto &[kind, rows] : {std::pair<Kind, int>{TierRewards, 5}, {MedalRewards, 3}})
            {
                const Table t = tableOf(kind);
                for (uint32_t level = 0; level < 8; ++level)
                    for (uint32_t row = 0; row < uint32_t(rows); ++row)
                        for (uint32_t slot = 0; slot < 2; ++slot)
                        {
                            const Bytes pair = xbe.bytes(t.address + level * t.size + row * 8 + slot * 4, 4);
                            const uint16_t type = u16At(pair, 0), value = u16At(pair, 2);
                            if ((type == 8 || type == 9) && (value == 8 || value == 9))
                                out.push_back({uint32_t(kind), level, row, type, value});
                        }
            }
            return out;
        }
    }

    Bytes buildCareer(const SourceFiles &xbox, const Bytes &defaultXbe, std::vector<std::string> &log)
    {
        const Xbe xbe(defaultXbe);
        if (xbe.title != kTitleId || xbe.timestamp != kTimestamp)
            throw Dm2Error("default.xbe is not the US release of Dave Mirra Freestyle BMX 2");
        // Anchors: the tables are where this release has them.
        const std::pair<uint32_t, const char *> anchors[] = {
            {tableOf(Gaps).address + 16, "pretty much free gap"},
            {tableOf(Gaps).address, "gap_01"},
            {tableOf(Goals).address + 2 * 0x1B80 + uint32_t(kTierSize) + 0x24, "mover_ladder01"},
            {tableOf(Pros).address + 4, "Leigh Ramsdell"}};
        for (const auto &[va, want] : anchors)
            if (xbe.string(xbe.u32(va)) != std::optional<std::string>(want))
                throw Dm2Error(std::string("default.xbe has no \"") + want + "\" where this release has it");
        const Bytes tierRewards = xbe.bytes(tableOf(TierRewards).address, 8), gapsReward = xbe.bytes(tableOf(GapsReward).address, 4);
        if (u16At(tierRewards, 0) != 13 || u16At(tierRewards, 2) != 2 || u16At(tierRewards, 4) != 0 ||
            u16At(tierRewards, 6) != 0 || u16At(gapsReward, 0) != 2 || u16At(gapsReward, 2) != 1000)
            throw Dm2Error("default.xbe's reward tables are not where this release has them");
        Strings strings;
        std::map<std::string, long> report;
        Bytes out(kMagic, kMagic + 8);
        appendU32(out, kVersion);
        appendU32(out, uint32_t(std::size(kCareerLevels)));
        std::string gaps;
        for (const CareerLevel &level : kCareerLevels)
        {
            std::vector<Block> blocks;
            blocks.push_back(goalsBlock(xbe, level.xboxLevel, strings, report));
            const Table g = tableOf(Gaps);
            const Bytes gapRow = xbe.bytes(g.address + uint32_t(level.xboxLevel) * g.size, g.size);
            gaps += std::string(gaps.empty() ? "" : ", ") + level.folder + " " + std::to_string(gapCount(gapRow)) + " gaps";
            blocks.push_back(stringsBlock(xbe, Gaps, level.xboxLevel, gapFields(gapRow), strings));
            for (Kind kind : {TierRewards, MedalRewards, GapsReward})
            {
                const Table t = tableOf(kind);
                blocks.push_back({kind, xbe.bytes(t.address + uint32_t(level.xboxLevel) * t.size, t.size), {}});
            }
            blocks.push_back(stringsBlock(xbe, Pros, level.xboxLevel, proFields(), strings));
            Bytes scale;
            appendU32(scale, level.competitionScale);
            blocks.push_back({CompetitionScale, scale, {}});
            appendPadded(out, level.folder, 16);
            appendU32(out, uint32_t(level.xboxLevel));
            appendU32(out, uint32_t(blocks.size()));
            for (const Block &b : blocks)
            {
                appendU32(out, b.kind);
                appendU32(out, uint32_t(b.data.size()));
                appendU32(out, uint32_t(b.names.size()));
                appendBytes(out, b.data);
                for (const Relocation &r : b.names)
                {
                    appendU32(out, r.offset);
                    appendU32(out, r.string);
                    appendU32(out, r.flags);
                }
            }
        }
        const std::vector<Text> text = texts(xbox);
        const std::vector<ExtraReward> extras = extraRewards(xbe);
        appendU32(out, uint32_t(strings.items.size()));
        for (const std::string &s : strings.items)
        {
            appendU32(out, uint32_t(s.size()));
            appendText(out, s);
        }
        appendU32(out, uint32_t(text.size()));
        for (const Text &t : text)
        {
            appendU32(out, uint32_t(t.language));
            appendU32(out, uint32_t(t.level));
            appendU32(out, uint32_t(t.key.size()));
            appendText(out, t.key);
            appendU32(out, uint32_t(t.text.size()));
            appendBytes(out, t.text);
        }
        appendU32(out, uint32_t(extras.size()));
        for (const ExtraReward &e : extras)
        {
            appendU32(out, e.kind);
            appendU32(out, e.level);
            appendU32(out, e.row);
            appendU16(out, e.type);
            appendU16(out, e.value);
        }
        log.push_back("[dm2xbox] career: " + std::to_string(report["goals"]) + " goals (" + std::to_string(report["parts"]) +
                      " parts), " + gaps + ", " + std::to_string(extras.size()) + " extra rewards, " +
                      std::to_string(text.size()) + " texts; CAREER.BIN " + std::to_string(out.size()) + " bytes");
        return out;
    }
}
