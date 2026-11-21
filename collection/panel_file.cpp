#include "collection/panel_file.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace panel
{
    namespace
    {
        class Reader
        {
        public:
            Reader(const uint8_t *data, size_t bytes) : at_(data), end_(data + bytes) {}
            size_t left() const { return static_cast<size_t>(end_ - at_); }
            const uint8_t *here() const { return at_; }
            bool skip(size_t bytes)
            {
                if (left() < bytes)
                    return false;
                at_ += bytes;
                return true;
            }
            bool bytes(void *out, size_t count)
            {
                if (left() < count)
                    return false;
                std::memcpy(out, at_, count);
                at_ += count;
                return true;
            }
            template <typename T> bool value(T &out) { return bytes(&out, sizeof(T)); }

        private:
            const uint8_t *at_, *end_;
        };

        // Walks the chunks of a file; false when it is not a panel file.
        template <typename F> bool forChunks(const uint8_t *data, size_t bytes, F &&onChunk)
        {
            Reader r(data, bytes);
            char magic[4];
            uint32_t version = 0;
            if (!r.bytes(magic, 4) || std::memcmp(magic, "PMAP", 4) != 0 || !r.value(version) || version != kFileVersion)
                return false;
            while (r.left() >= 8)
            {
                char tag[4];
                uint32_t size = 0;
                r.bytes(tag, 4);
                r.value(size);
                if (r.left() < size)
                    return false;
                if (!onChunk(tag, r.here(), size))
                    return false;
                r.skip(size);
                r.skip(-static_cast<int32_t>(size) & 3);
            }
            return true;
        }

        bool readMap(const uint8_t *data, size_t bytes, LevelFile &out)
        {
            Reader r(data, bytes);
            uint32_t width = 0, height = 0;
            float left = 0, top = 0, ppu = 1;
            uint32_t turned = 0;
            if (!r.value(width) || !r.value(height) || !r.value(left) || !r.value(top) || !r.value(ppu) || !r.value(turned))
                return false;
            if (width == 0 || height == 0 || width > 8192 || height > 8192 || ppu <= 0)
                return false;
            out.transform = {left, top, ppu, static_cast<int>(width), static_cast<int>(height), turned != 0};
            MapLevel base;
            base.width = static_cast<int>(width);
            base.height = static_cast<int>(height);
            base.pixels.resize(size_t(width) * height);
            size_t at = 0;
            const size_t total = base.pixels.size();
            while (at < total)
            {
                uint16_t run = 0, colour = 0;
                if (!r.value(run) || !r.value(colour) || run == 0 || at + run > total)
                    return false;
                std::fill_n(base.pixels.begin() + static_cast<std::ptrdiff_t>(at), run, colour);
                at += run;
            }
            out.mips.clear();
            out.mips.push_back(std::move(base));
            // Smaller levels, for the views that show the whole map.
            for (int i = 0; i < 2; ++i)
            {
                const MapLevel &from = out.mips.back();
                MapLevel half;
                half.width = from.width / 2;
                half.height = from.height / 2;
                if (half.width < 8 || half.height < 8)
                    break;
                half.pixels.resize(size_t(half.width) * half.height);
                for (int y = 0; y < half.height; ++y)
                {
                    const uint16_t *a = from.pixels.data() + size_t(y) * 2 * from.width;
                    const uint16_t *b = a + from.width;
                    uint16_t *o = half.pixels.data() + size_t(y) * half.width;
                    for (int x = 0; x < half.width; ++x)
                    {
                        const uint32_t p0 = (a[x * 2] | (uint32_t(a[x * 2]) << 16)) & 0x07E0F81Fu;
                        const uint32_t p1 = (a[x * 2 + 1] | (uint32_t(a[x * 2 + 1]) << 16)) & 0x07E0F81Fu;
                        const uint32_t p2 = (b[x * 2] | (uint32_t(b[x * 2]) << 16)) & 0x07E0F81Fu;
                        const uint32_t p3 = (b[x * 2 + 1] | (uint32_t(b[x * 2 + 1]) << 16)) & 0x07E0F81Fu;
                        const uint32_t sum = ((p0 + p1 + p2 + p3) >> 2) & 0x07E0F81Fu;
                        o[x] = static_cast<uint16_t>(sum | (sum >> 16));
                    }
                }
                out.mips.push_back(std::move(half));
            }
            return true;
        }

        template <typename T> bool readTable(const uint8_t *data, size_t bytes, std::vector<T> &out)
        {
            Reader r(data, bytes);
            uint32_t count = 0;
            if (!r.value(count) || r.left() < size_t(count) * sizeof(T))
                return false;
            out.resize(count);
            if (count)
                r.bytes(out.data(), size_t(count) * sizeof(T));
            return true;
        }

        bool readImage(Reader &r, Image &out)
        {
            uint32_t width = 0, height = 0;
            if (!r.value(width) || !r.value(height) || width > 4096 || height > 4096)
                return false;
            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);
            out.rgba.resize(size_t(width) * height * 4);
            return r.bytes(out.rgba.data(), out.rgba.size());
        }

        bool readFont(const uint8_t *data, size_t bytes, UiFile &out)
        {
            Reader r(data, bytes);
            char name[33] = {};
            uint32_t line = 0, glyphs = 0, pages = 0;
            if (!r.bytes(name, 32) || !r.value(line) || !r.value(glyphs) || !r.value(pages) || pages > 16)
                return false;
            Font &font = out.fonts[name];
            font.lineHeight = static_cast<int>(line);
            font.pages.resize(pages);
            for (Image &page : font.pages)
                if (!readImage(r, page))
                    return false;
            for (uint32_t i = 0; i < glyphs; ++i)
            {
                GlyphRecord g;
                if (!r.value(g))
                    return false;
                if (g.page < pages)
                {
                    font.glyphs[g.character] = g;
                    font.has[g.character] = true;
                }
            }
            return true;
        }

        bool readIcon(const uint8_t *data, size_t bytes, UiFile &out)
        {
            Reader r(data, bytes);
            char name[33] = {};
            return r.bytes(name, 32) && readImage(r, out.icons[name]);
        }

        bool readFile(const std::string &path, std::vector<uint8_t> &out)
        {
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
                return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            out.resize(size > 0 ? static_cast<size_t>(size) : 0);
            const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
            std::fclose(f);
            return ok;
        }
    }

    bool parseLevelFile(const uint8_t *data, size_t bytes, LevelFile &out)
    {
        bool hasMap = false;
        // The set the goal chunks go to: SCRP starts one, a goal chunk
        // before any SCRP makes the unkeyed one.
        const auto set = [&out]() -> GoalSet & {
            if (out.goalSets.empty())
                out.goalSets.emplace_back();
            return out.goalSets.back();
        };
        const bool ok = forChunks(data, bytes, [&](const char *tag, const uint8_t *body, size_t size) {
            if (std::memcmp(tag, "MAPI", 4) == 0)
                return hasMap = readMap(body, size, out);
            if (std::memcmp(tag, "SCRP", 4) == 0)
            {
                Reader r(body, size);
                GoalSet keyed;
                keyed.keyed = true;
                uint32_t count = 0;
                if (!r.value(keyed.variableBytes) || !r.value(count) || count > 4096u)
                    return false;
                keyed.objectiveTexts.resize(size_t(count) * 2u);
                for (uint32_t &word : keyed.objectiveTexts)
                    if (!r.value(word))
                        return false;
                out.goalSets.push_back(std::move(keyed));
            }
            if (std::memcmp(tag, "GOAL", 4) == 0)
                return readTable(body, size, set().goals);
            if (std::memcmp(tag, "TARG", 4) == 0)
                return readTable(body, size, set().targets);
            if (std::memcmp(tag, "GIVE", 4) == 0)
                return readTable(body, size, set().bubbles);
            if (std::memcmp(tag, "TEXT", 4) == 0)
                set().text.assign(body, body + size);
            if (std::memcmp(tag, "NAME", 4) == 0)
                out.name.assign(reinterpret_cast<const char *>(body), strnlen(reinterpret_cast<const char *>(body), size));
            return true;
        });
        if (!out.goalSets.empty())
            useGoalSet(out, 0);
        return ok && hasMap;
    }

    int matchingGoalSet(const LevelFile &level, uint32_t variableBytes, const uint32_t *objectiveTexts, uint32_t objectiveCount)
    {
        for (size_t i = 0; i < level.goalSets.size(); ++i)
        {
            const GoalSet &set = level.goalSets[i];
            if (!set.keyed)
                return static_cast<int>(i);
            if (set.variableBytes == variableBytes && set.objectiveTexts.size() == size_t(objectiveCount) * 2u &&
                std::equal(set.objectiveTexts.begin(), set.objectiveTexts.end(), objectiveTexts))
                return static_cast<int>(i);
        }
        return -1;
    }

    void useGoalSet(LevelFile &level, int set)
    {
        level.goalSet = set;
        level.goals.clear();
        level.targets.clear();
        level.bubbles.clear();
        level.text.clear();
        if (level.goalSets.empty())
            return;
        const GoalSet &from = level.goalSets[set >= 0 && static_cast<size_t>(set) < level.goalSets.size() ? static_cast<size_t>(set) : 0u];
        level.goals = from.goals;
        level.targets = from.targets;
        level.text = from.text;
        if (set >= 0)
        {
            level.bubbles = from.bubbles;
            return;
        }
        for (GoalRecord &goal : level.goals)
        {
            goal.counterWord = -1;
            goal.targetWord = -1;
            goal.scoreKind = 0;
        }
        for (TargetRecord &target : level.targets)
            target.doneWord = -1;
    }

    bool parseUiFile(const uint8_t *data, size_t bytes, UiFile &out)
    {
        return forChunks(data, bytes, [&](const char *tag, const uint8_t *body, size_t size) {
            if (std::memcmp(tag, "FONT", 4) == 0)
                return readFont(body, size, out);
            if (std::memcmp(tag, "ICON", 4) == 0)
                return readIcon(body, size, out);
            if (std::memcmp(tag, "SPEC", 4) == 0)
                return readTable(body, size, out.specialTricks);
            if (std::memcmp(tag, "TRIK", 4) == 0)
                return readTable(body, size, out.tricks);
            if (std::memcmp(tag, "MOVE", 4) == 0)
                return readTable(body, size, out.moves);
            if (std::memcmp(tag, "FAMS", 4) == 0)
                return readTable(body, size, out.familyNames);
            return true;
        });
    }

    bool loadLevelFile(const std::string &path, LevelFile &out)
    {
        std::vector<uint8_t> bytes;
        return readFile(path, bytes) && parseLevelFile(bytes.data(), bytes.size(), out);
    }

    bool loadUiFile(const std::string &path, UiFile &out)
    {
        std::vector<uint8_t> bytes;
        return readFile(path, bytes) && parseUiFile(bytes.data(), bytes.size(), out);
    }
}
