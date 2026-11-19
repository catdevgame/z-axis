// BMX XXX's panel files (collection/extras/panel_maps.h). Its levels are
// DM2's format (ASSETS/LEVELS/<FOLDER>/<FOLDER>.GEO, .INS, .DAT; their map as
// DM2's, panel_maps_dm2.cpp), with the level's challenges in its own
// OBJCTV.TXT (the engine's objectives script: game/zge/scene/objectives.h,
// game/bmx/objectives). The level table in its executable (game/bmx/profile/
// players.h LevelInfo, g_levels: 0x10 bytes a row, +0 the two-letter code, +4
// the folder, +8 the title) lists the disc's eight levels first, in the level
// select's order; the panel's capture (bmx_xxx_panel.cpp) reads the same
// table's folder for the level being ridden, so the files are named by folder.
#include "sym/collection/extras/panel_maps_bmx.cpp.h"
#include "panel_maps_parts.h"

#include "script_text.h"

#include <map>
#include <memory>
#include <set>
#include <variant>

namespace ps2x::app::extras::panel_maps
{
    namespace
    {
        constexpr uint32_t kLevelTable = sym::bmx::g_levels, kLevelRowBytes = 0x10, kLevelRows = 8;
        // Its string tables (ASSETS/STRING.ZAL, a ZDIR archive of tables in
        // DM2's STRING.EUS format; game/bmx/ui/strings.cpp): the common one,
        // and one a level by the level's code, which the overlay loads with
        // the level; American English.
        constexpr const char *kCommonStrings = "string.common.eus";
        constexpr const char *kLevelStrings = "string.level.%s.eus";

        // ---- OBJCTV.TXT --------------------------------------------------------------

        // The file is lines of KEY "value" between <BLOCK>_BEGIN and
        // <BLOCK>_END, with C and C++ comments. A block's entries keep the
        // file's order (a key given again keeps its first place); a file holds
        // many OBJECTIVE blocks, a PARAMETERS block one of each kind.
        struct Block;
        using BlockPtr = std::shared_ptr<Block>;
        using Value = std::variant<std::string, BlockPtr, std::vector<BlockPtr>>;
        struct Block
        {
            std::vector<std::pair<std::string, Value>> items;

            const Value *find(const std::string &key) const
            {
                for (const auto &item : items)
                    if (item.first == key)
                        return &item.second;
                return nullptr;
            }
            Value *find(const std::string &key)
            {
                for (auto &item : items)
                    if (item.first == key)
                        return &item.second;
                return nullptr;
            }
            void set(const std::string &key, Value value)
            {
                if (Value *v = find(key))
                    *v = std::move(value);
                else
                    items.emplace_back(key, std::move(value));
            }
            // A text entry, `otherwise` when there is none.
            std::string text(const std::string &key, const std::string &otherwise) const
            {
                const Value *v = find(key);
                if (!v)
                    return otherwise;
                if (!std::holds_alternative<std::string>(*v))
                    throw PanelError("an objectives script whose " + key + " is a block, not a text");
                return std::get<std::string>(*v);
            }
            // A block entry; none when there is none.
            BlockPtr block(const std::string &key) const
            {
                const Value *v = find(key);
                if (!v)
                    return nullptr;
                if (!std::holds_alternative<BlockPtr>(*v))
                    throw PanelError("an objectives script whose " + key + " is not a block");
                return std::get<BlockPtr>(*v);
            }
        };

        struct BmxObjective
        {
            std::string name, type, stringId;
            bool child = true;
            BlockPtr params;
        };

        // str.upper() for comparing with a keyword: \xdf is "SS"; the letters
        // whose capitals are no Latin-1 ones stay as they are (no keyword has them).
        std::string upperForComparison(const std::string &text)
        {
            return script_text::upper(script_text::replaceAll(text, "\xdf", "SS"));
        }

        bool regexSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

        // `open` .. `close` (or `open` to the line's end) each made a space.
        std::string withoutComments(const std::string &text)
        {
            std::string once;
            for (size_t i = 0; i < text.size();)
            {
                if (text.compare(i, 2, "/*") == 0)
                {
                    const size_t end = text.find("*/", i + 2);
                    if (end != std::string::npos)
                    {
                        once += ' ';
                        i = end + 2;
                        continue;
                    }
                }
                once += text[i++];
            }
            std::string out;
            for (size_t i = 0; i < once.size();)
            {
                if (once.compare(i, 2, "//") == 0)
                {
                    const size_t end = once.find('\n', i);
                    out += ' ';
                    i = end == std::string::npos ? once.size() : end;
                    continue;
                }
                out += once[i++];
            }
            return out;
        }

        // The file's objectives in its order.
        std::vector<BmxObjective> readObjectives(const Bytes &file)
        {
            const std::string source = withoutComments(std::string(file.begin(), file.end()));
            // the tokens: a quoted text, or a run of anything but spaces
            std::vector<std::string> tokens;
            for (size_t i = 0; i < source.size();)
            {
                if (regexSpace(source[i]))
                {
                    ++i;
                    continue;
                }
                if (source[i] == '"')
                {
                    const size_t close = source.find('"', i + 1);
                    if (close != std::string::npos)
                    {
                        tokens.push_back(source.substr(i, close + 1 - i));
                        i = close + 1;
                        continue;
                    }
                }
                size_t end = i;
                while (end < source.size() && !regexSpace(source[end]))
                    ++end;
                tokens.push_back(source.substr(i, end - i));
                i = end;
            }
            const BlockPtr root = std::make_shared<Block>();
            std::vector<BlockPtr> stack{root};
            std::optional<std::string> key;
            for (const std::string &word : tokens)
            {
                if (word[0] == '"')
                {
                    if (key)
                        stack.back()->set(*key, word.size() >= 2 ? word.substr(1, word.size() - 2) : std::string());
                    key.reset();
                }
                else if (script_text::endsWith(word, "_BEGIN"))
                {
                    const BlockPtr block = std::make_shared<Block>();
                    const std::string name = word.substr(0, word.size() - 6);
                    if (name == "OBJECTIVE")
                    {
                        Value *list = stack.back()->find(name);
                        if (!list)
                        {
                            stack.back()->items.emplace_back(name, std::vector<BlockPtr>{});
                            list = &stack.back()->items.back().second;
                        }
                        if (!std::holds_alternative<std::vector<BlockPtr>>(*list))
                            throw PanelError("an objectives script with an OBJECTIVE that is not a block");
                        std::get<std::vector<BlockPtr>>(*list).push_back(block);
                    }
                    else
                        stack.back()->set(name, block);
                    stack.push_back(block);
                    key.reset();
                }
                else if (script_text::endsWith(word, "_END"))
                {
                    if (stack.size() > 1)
                        stack.pop_back();
                    key.reset();
                }
                else
                    key = word;
            }
            std::vector<BmxObjective> out;
            const Value *blocks = root->find("OBJECTIVE");
            if (!blocks)
                return out;
            if (!std::holds_alternative<std::vector<BlockPtr>>(*blocks))
                throw PanelError("an objectives script with an OBJECTIVE that is not a block");
            for (const BlockPtr &block : std::get<std::vector<BlockPtr>>(*blocks))
            {
                BmxObjective o;
                o.name = block->text("NAME", "");
                o.type = upperForComparison(block->text("TYPE", ""));
                block->text("PERSISTENT", "NO"); // (a text, as the reference reads it)
                // The engine's default: an objective is a set's child unless its
                // block says IS_CHILD "NO" (the 20 goals of a challenge level say
                // so; a set's members and the helpers say nothing).
                o.child = upperForComparison(block->text("IS_CHILD", "YES")) != "NO";
                o.stringId = block->text("STRINGID", "");
                o.params = block->block("PARAMETERS");
                if (!o.params)
                    o.params = std::make_shared<Block>();
                out.push_back(std::move(o));
            }
            return out;
        }

        using ByName = std::unordered_map<std::string, const BmxObjective *>;

        const BmxObjective *byName(const ByName &objectives, const std::string &name)
        {
            const auto it = objectives.find(name);
            return it == objectives.end() ? nullptr : it->second;
        }

        // A target's role from the level's own naming: a face or an edge is a
        // place to ride, an object named as a checkpoint ("checkpoint_can01",
        // "CheckPoint_junkie01") is where to bring things, any other a pickup.
        uint32_t targetRole(PlaceKind kind, const std::string &name)
        {
            if (kind != PlaceObject)
                return kTargetPlace;
            const std::string lower = lowerAscii(name);
            return lower.find("checkpoint") != std::string::npos || lower.find("chkpt") != std::string::npos ? kTargetDropOff
                                                                                                             : kTargetPickup;
        }

        // What an objective's parameters name in the level (game/bmx/
        // objectives, the types' readers): a transfer's faces and edges, a
        // grind's edges, a collect's pickups (instances), a proximity's face
        // or entity; a set's children lend it their places. Keys are numbered
        // (ITEM1, EDGE2: the digits are dropped).
        std::optional<PlaceKind> placeKey(const std::string &block, const std::string &base)
        {
            if (block == "TRANSFER" && (base == "FROM_FACE" || base == "TO_FACE"))
                return PlaceFace;
            if (block == "TRANSFER" && (base == "FROM_EDGE" || base == "TO_EDGE"))
                return PlaceEdge;
            if (block == "GRIND" && base == "EDGE")
                return PlaceEdge;
            if (block == "COLLECT" && base == "ITEM")
                return PlaceObject;
            return std::nullopt;
        }
        constexpr int kSetDepthMost = 8;

        struct Spot
        {
            uint32_t role, name;
            Vec3 point;
        };

        // (first name's hash, the spots) an objective's parameters name, through a set's children.
        uint32_t objectivePlaces(const BmxObjective &objective, const ByName &objectives, const GoalPlaces &places,
                                 std::vector<Spot> &spots, int depth = 0)
        {
            uint32_t first = 0;
            for (const auto &[block, body] : objective.params->items)
            {
                if (!std::holds_alternative<BlockPtr>(body))
                    continue;
                const Block &entries = *std::get<BlockPtr>(body);
                for (const auto &[key, value] : entries.items)
                {
                    if (!std::holds_alternative<std::string>(value))
                        continue;
                    const std::string &text = std::get<std::string>(value);
                    std::string base = key;
                    while (!base.empty() && base.back() >= '0' && base.back() <= '9')
                        base.pop_back();
                    if (block == "SET" && base == "OBJECTIVE")
                    {
                        const BmxObjective *child = byName(objectives, text);
                        if (child && depth < kSetDepthMost)
                        {
                            const uint32_t childFirst = objectivePlaces(*child, objectives, places, spots, depth + 1);
                            first = first ? first : childFirst;
                        }
                        continue;
                    }
                    std::optional<PlaceKind> kind = placeKey(block, base);
                    if (block == "PROXIMITY" && base == "NAME")
                        kind = lowerAscii(entries.text("TYPE", "")) == "face" ? PlaceFace : PlaceObject;
                    if (!kind)
                        continue;
                    const uint32_t hash = dm2::nameHash(text);
                    first = first ? first : hash;
                    const auto it = places.find(hash);
                    if (it != places.end())
                        for (const Vec3 &p : it->second[*kind])
                            spots.push_back({targetRole(*kind, text), hash, p});
                }
            }
            return first;
        }

        // A goal's look (GoalRecord type): its type's, a set's its first
        // child's, a record of a run's score a points goal.
        int64_t goalType(const BmxObjective &objective, const ByName &objectives, int depth = 0)
        {
            if (depth > 100)
                throw PanelError("an objectives script whose sets hold themselves");
            if (objective.type == "SET")
            {
                const BlockPtr set = objective.params->block("SET");
                if (!set)
                    return 0;
                std::vector<const BmxObjective *> children;
                for (const auto &[key, value] : set->items)
                    if (key.compare(0, 9, "OBJECTIVE") == 0)
                    {
                        if (!std::holds_alternative<std::string>(value))
                            throw PanelError("an objectives script whose set names a block");
                        children.push_back(byName(objectives, std::get<std::string>(value)));
                    }
                return !children.empty() && children[0] ? goalType(*children[0], objectives, depth + 1) : 0;
            }
            if (objective.type == "RECORD")
            {
                const BlockPtr record = objective.params->block("RECORD");
                const Value *kind = record ? record->find("RECORDTYPE") : nullptr;
                return kind && std::holds_alternative<std::string>(*kind) && std::get<std::string>(*kind) == "SCORE_RUN" ? 1 : 0;
            }
            if (objective.type == "TRANSFER")
                return 7;
            if (objective.type == "GRIND")
                return 5;
            if (objective.type == "POINTS")
                return 1;
            return 0;
        }

        // The level's people (its DAT's SDPT records of this class; the goal
        // giver's record carries its goal's name) and where the level puts
        // them: (goal name, x, z) of each who holds one, the longest it holds.
        constexpr uint32_t kPersonClass = 0x0422B9CE;
        struct Giver
        {
            std::string goal;
            double x, z;
        };

        std::vector<Giver> givers(const std::optional<Bytes> &dat, const std::vector<std::string> &goalNames)
        {
            std::vector<Giver> out;
            if (!dat || dat->empty())
                return out;
            walkChunks(parseChunks(*dat), [&](const Chunk &c) {
                if (c.tag != "SDPT" || c.data.size < 0x2C || c.data.u32(4) != kPersonClass)
                    return;
                // the record's texts: runs of four or more printable characters
                std::vector<std::string> words;
                for (size_t i = 0; i < c.data.size;)
                {
                    size_t end = i;
                    while (end < c.data.size && c.data.data[end] >= 0x20 && c.data.data[end] <= 0x7E)
                        ++end;
                    if (end - i >= 4)
                        words.emplace_back(reinterpret_cast<const char *>(c.data.data + i), end - i);
                    i = end > i ? end : i + 1;
                }
                const std::string *best = nullptr;
                for (const std::string &w : words)
                    for (const std::string &g : goalNames)
                        if (script_text::endsWith(w, g) && (!best || g.size() > best->size()))
                            best = &g;
                if (best)
                    out.push_back({*best, c.data.f32(0x10), c.data.f32(0x18)});
            });
            return out;
        }

        // " ".join(text.split()) of a text with its line breaks (and written "\n"s) made spaces.
        std::string clean(std::string text)
        {
            text = script_text::replaceAll(std::move(text), "\\n", " ");
            text = script_text::replaceAll(std::move(text), "\r\n", " ");
            text = script_text::replaceAll(std::move(text), "\n", " ");
            std::string out;
            for (const std::string &word : script_text::splitWords(text))
                out += (out.empty() ? "" : " ") + word;
            return out;
        }

        // A status line the panel fills in: exactly two conversions, both %d.
        bool countedStatus(const std::optional<std::string> &text)
        {
            if (!text)
                return false;
            std::vector<std::string> found;
            for (size_t i = 0; i + 1 < text->size();)
            {
                if ((*text)[i] == '%' && (*text)[i + 1] != '%')
                {
                    found.push_back(text->substr(i, 2));
                    i += 2;
                }
                else
                    ++i;
            }
            return found == std::vector<std::string>{"%d", "%d"};
        }

        struct GoalChunks
        {
            Bytes bytes;
            int shown = 0, placed = 0, targets = 0, givers = 0;
        };

        // GOAL, TARG, GIVE and TEXT of a BMX XXX level: the objectives its
        // CHALLENGES list shows (overlay/screen_objectives.cpp addObjectiveLine:
        // every objective that is not a set's child), in the file's order,
        // which is the order the engine's objective pool keeps them;
        // bmx_xxx_panel.cpp walks that pool in the same order, leaving out the
        // children as the list does, and reads each one's state, count and
        // target (script words 2n and 2n + 1 of goal n). A goal's title is its
        // "_NAME" text, its text its "_DESC" (else "_ACTIVATE") text, its
        // activate text its "_ACTIVATE", its status its "_STATUS" when that is
        // a count of a target ("Gaps found: %d of %d"); its targets the places
        // its parameters name (a set's its children's) with their roles; its
        // givers (by the goal's name hash as the record's owner, shown while
        // the goal is ready) the level's people who carry its name.
        GoalChunks goalChunks(const std::vector<BmxObjective> &objectives, const std::unordered_map<uint32_t, std::string> &texts,
                              const GoalPlaces &places, const std::vector<Giver> &giverList)
        {
            TextTable text;
            Bytes targets, data;
            int64_t targetCount = 0;
            ByName names;
            for (const BmxObjective &o : objectives)
                names[o.name] = &o;
            auto named = [&](const std::string &stringId, const char *suffix) -> std::optional<std::string> {
                if (stringId.empty())
                    return std::nullopt;
                const auto it = texts.find(dm2::nameHash(stringId + suffix));
                if (it == texts.end())
                    return std::nullopt;
                return it->second;
            };
            GoalChunks out;
            std::vector<std::string> listed;
            for (const BmxObjective &objective : objectives)
            {
                if (objective.child)
                    continue;
                listed.push_back(objective.name);
                const int64_t slot = out.shown;
                const std::string title = named(objective.stringId, "_NAME").value_or(objective.name);
                std::optional<std::string> body = named(objective.stringId, "_DESC");
                if (!body)
                    body = named(objective.stringId, "_ACTIVATE");
                const std::optional<std::string> status = named(objective.stringId, "_STATUS");
                const std::optional<std::string> activate = named(objective.stringId, "_ACTIVATE");
                std::vector<Spot> spots;
                objectivePlaces(objective, names, places, spots);
                const int64_t first = targetCount;
                // (each named thing once: a transfer's faces or a grind's edge ends join into one place)
                std::set<std::pair<uint32_t, uint32_t>> things;
                for (const Spot &s : spots)
                    things.insert({s.role, s.name});
                for (const auto &[role, key] : things)
                {
                    std::vector<Vec3> points;
                    for (const Spot &s : spots)
                        if (s.role == role && s.name == key)
                            points.push_back(s.point);
                    for (const auto &[x, z] : joinPlaces(points))
                    {
                        putTarget(targets, key, x, z, -1, 0, role);
                        ++targetCount;
                    }
                }
                out.placed += targetCount > first ? 1 : 0;
                GoalFields f;
                f.title = text.add(clean(title));
                f.text = text.add(clean(body && !body->empty() ? *body : title));
                f.owner = dm2::nameHash(objective.name);
                f.counterWord = 2 * slot;
                f.counterTarget = 0;
                f.firstTarget = first;
                f.targetCount = targetCount - first;
                f.type = goalType(objective, names);
                f.status = countedStatus(status) ? text.add(clean(*status)) : 0;
                f.targetWord = 2 * slot + 1;
                f.activate = activate && !activate->empty() ? text.add(clean(*activate)) : 0;
                putGoal(data, f);
                ++out.shown;
            }
            // A giver's goal: the listed one of that name, else the listed one whose name begins with it.
            Bytes give;
            for (const Giver &g : giverList)
            {
                const std::string *match = nullptr;
                for (const std::string &n : listed)
                    if (n == g.goal)
                    {
                        match = &n;
                        break;
                    }
                if (!match)
                    for (const std::string &n : listed)
                        if (n.compare(0, g.goal.size(), g.goal) == 0)
                        {
                            match = &n;
                            break;
                        }
                if (match)
                {
                    putBubble(give, dm2::nameHash(*match), g.x, g.z, -1, 0);
                    ++out.givers;
                }
            }
            Bytes goal;
            putU32(goal, out.shown);
            putBytes(goal, data);
            out.bytes = chunk("GOAL", goal);
            Bytes targ;
            putU32(targ, targetCount);
            putBytes(targ, targets);
            putBytes(out.bytes, chunk("TARG", targ));
            Bytes giveChunk;
            putU32(giveChunk, out.givers);
            putBytes(giveChunk, give);
            putBytes(out.bytes, chunk("GIVE", giveChunk));
            putBytes(out.bytes, chunk("TEXT", text.bytes));
            out.targets = int(targetCount);
            return out;
        }

        // ---- the trick list ------------------------------------------------------------

        // BMX XXX's moves (game/bmx/ui/trick_list.cpp): 100 tricks, then 0x56
        // modifiers, each a name and its buttons in ButtonLib's glyphs; the
        // saved game's movesLanded has a bit a trick, the modifiers' from bit
        // 100 (the panel's move numbers are those bits: bmx_xxx_panel.cpp). The
        // trick list's categories list the tricks by number, leaving out those
        // whose flags (game/trick_types.h g_trickFlags, a word a trick) say
        // so; its SIGNATURE MOVES category lists every trick again, so the
        // panel's signature family lists the tricks the four categories leave
        // out, each move once. The names keep the game's case.
        constexpr uint32_t kTrickNames = sym::bmx::g_trickNames, kModifierNames = sym::bmx::g_modifierNames;
        constexpr uint32_t kTrickButtons = sym::bmx::trickButtonTexts, kModifierButtons = sym::bmx::modifierButtonTexts;
        constexpr uint32_t kTrickFlags = sym::bmx::g_trickKindsR;
        constexpr uint32_t kTricks = 100, kModifiers = 0x56, kFirstModifierBit = 100;
        constexpr uint32_t kTrickNotListed = 0x8;
        const char *const kFamilies[] = {"AIR", "MODIFIERS", "STALLS", "GRINDS", "OTHERS", "SIGNATURE"};
        struct Category
        {
            int64_t family;
            uint32_t first, last; // tricks
        };
        constexpr Category kCategories[] = {{0, 0x00, 0x32}, {2, 0x3C, 0x42}, {3, 0x33, 0x3B}, {4, 0x43, 0x63}};

        Bytes trickChunks(const Elf &elf, int &rows)
        {
            auto flags = [&](uint32_t trick) { return elf.word(kTrickFlags + 4 * trick); };
            std::vector<Bytes> records, moves;
            auto add = [&](int64_t family, uint32_t move, uint32_t names, uint32_t buttons, uint32_t index) {
                records.push_back(buttonsTrickRecord(family, elf.text(elf.word(buttons + 4 * index)),
                                                     elf.text(elf.word(names + 4 * index))));
                Bytes m;
                putU8(m, move);
                putU8(m, 0);
                putU16(m, 0);
                moves.push_back(m);
            };
            for (uint32_t trick = kCategories[0].first; trick <= kCategories[0].last; ++trick)
                if (!(flags(trick) & kTrickNotListed))
                    add(kCategories[0].family, trick, kTrickNames, kTrickButtons, trick);
            for (uint32_t modifier = 0; modifier < kModifiers; ++modifier)
                add(1, modifier + kFirstModifierBit, kModifierNames, kModifierButtons, modifier);
            for (size_t c = 1; c < 4; ++c)
                for (uint32_t trick = kCategories[c].first; trick <= kCategories[c].last; ++trick)
                    if (!(flags(trick) & kTrickNotListed))
                        add(kCategories[c].family, trick, kTrickNames, kTrickButtons, trick);
            for (uint32_t trick = 0; trick < kTricks; ++trick)
                if (flags(trick) & kTrickNotListed)
                    add(5, trick, kTrickNames, kTrickButtons, trick);
            Bytes trik, move, fams;
            putU32(trik, int64_t(records.size()));
            for (const Bytes &r : records)
                putBytes(trik, r);
            putU32(move, int64_t(moves.size()));
            for (const Bytes &m : moves)
                putBytes(move, m);
            putU32(fams, 6);
            for (const char *f : kFamilies)
                putPadded(fams, f, 16, 16);
            Bytes out = chunk("TRIK", trik);
            putBytes(out, chunk("MOVE", move));
            putBytes(out, chunk("FAMS", fams));
            rows = int(records.size());
            return out;
        }

        // ---- the fonts -------------------------------------------------------------------

        // The panel's fonts from BMX XXX's UI library: its menus' typeface
        // (font_normal, 35 pixels a line, with small letters) under the
        // panel's three names, halved to the panel's size (AI's and DM2's
        // fonts are 15 to 23 a line), and the ButtonLib PlayStation font
        // (ps2_btn, DM2's) for the trick table's directions and buttons.
        const std::vector<std::pair<std::string, std::vector<std::string>>> kUiFonts = {
            {"font_normal", {"fnt_14pt", "fnt_e_12pt", "fnt_ovr_score"}}, {"ps2_btn", {"fnt_ps2buttons"}}};
        const char *const kHalvedFont = "font_normal";

        // An RGBA picture at half size: each pixel the mean of four.
        RgbaPicture halved(const RgbaPicture &p)
        {
            RgbaPicture out;
            out.width = p.width / 2;
            out.height = p.height / 2;
            // (the last byte it reads: the bottom right pixel's of the last two rows)
            if (out.width > 0 && out.height > 0 &&
                (2 * out.height - 1) * p.width * 4 + (out.width - 1) * 8 + 7 >= int64_t(p.rgba.size()))
                throw PanelError("a font page shorter than its size");
            out.rgba.resize(size_t(out.width * out.height * 4));
            auto at = [&](int64_t i) { return int(p.rgba[size_t(i)]); };
            for (int64_t y = 0; y < out.height; ++y)
            {
                const int64_t top = 2 * y * p.width * 4, bottom = (2 * y + 1) * p.width * 4;
                for (int64_t x = 0; x < out.width; ++x)
                {
                    const int64_t i = x * 8, o = (y * out.width + x) * 4;
                    for (int64_t c = 0; c < 4; ++c)
                        out.rgba[size_t(o + c)] =
                            uint8_t((at(top + i + c) + at(top + i + 4 + c) + at(bottom + i + c) + at(bottom + i + 4 + c) + 2) >> 2);
                }
            }
            return out;
        }

        // Glyph records (character, page, _, x, y, width, height) at half size.
        Bytes halvedGlyphs(const Bytes &records)
        {
            const ByteView r(records);
            Bytes out;
            for (size_t g = 0; g < records.size() / 12; ++g)
            {
                const size_t at = 12 * g;
                const int64_t x = r.u16(at + 4), y = r.u16(at + 6), width = r.u16(at + 8), height = r.u16(at + 10);
                const int64_t x2 = x / 2, y2 = y / 2;
                putU8(out, records[at]);
                putU8(out, records[at + 1]);
                putU16(out, r.u16(at + 2));
                putU16(out, x2);
                putU16(out, y2);
                putU16(out, std::max<int64_t>(1, (x + width + 1) / 2 - x2));
                putU16(out, std::max<int64_t>(1, (y + height + 1) / 2 - y2));
            }
            return out;
        }

        void buildUi(const SourceFiles &disc, const Elf &elf, const std::filesystem::path &out, std::vector<std::string> &log)
        {
            int rows = 0;
            Bytes data;
            putText(data, "PMAP");
            putU32(data, 3); // panel_file.h kFileVersion
            putBytes(data, trickChunks(elf, rows));
            std::vector<std::string> found;
            forEachUiAsset(needFile(disc, "ASSETS/UILIB.ZAL"), [&](std::string name, const std::string &source, const Chunk &asset) {
                if (name == "button") // the ButtonLib fonts, by their source file
                    name = sourceStem(source);
                for (const Chunk &k : asset.kids)
                {
                    const auto font = std::find_if(kUiFonts.begin(), kUiFonts.end(), [&](const auto &f) { return f.first == name; });
                    if (k.tag != "UFNT" || font == kUiFonts.end() || std::count(found.begin(), found.end(), name))
                        continue;
                    found.push_back(name);
                    UiFont f = readUiFont(k);
                    const bool half = name == kHalvedFont;
                    if (half)
                    {
                        f.lineHeight = (f.lineHeight + 1) / 2;
                        for (RgbaPicture &p : f.pictures)
                            p = halved(p);
                    }
                    if (name == "ps2_btn")
                        for (size_t g = 0; g < f.glyphs; ++g)
                        {
                            if (12 * g >= f.glyphRecords.size())
                                throw PanelError("a font with fewer glyphs than it says");
                            f.glyphRecords[12 * g] = panelDirectionGlyph(f.glyphRecords[12 * g]);
                        }
                    if (half)
                        f.glyphRecords = halvedGlyphs(f.glyphRecords);
                    for (const std::string &panelName : font->second)
                        putBytes(data, fontChunk(panelName, f));
                }
            });
            writeMade(out / "UI.pmap", data);
            std::string missing;
            for (const auto &f : kUiFonts)
                if (!std::count(found.begin(), found.end(), f.first))
                    missing += " " + f.first;
            log.push_back(formatted("UI: %.1f MB, %d fonts, %d tricks%s", double(data.size()) / 1e6, int(found.size()), rows,
                                 missing.empty() ? "" : (", missing" + missing).c_str()));
        }

        // A level's file: its map, and its challenges as OBJCTV.TXT lists them.
        void buildLevel(const SourceFiles &disc, const std::string &code, const std::string &folder, const std::string &title,
                        const Bytes &geo, const std::unordered_map<uint32_t, ByteView> &tables,
                        const std::unordered_map<uint32_t, std::string> &commonTexts, const std::filesystem::path &out,
                        std::vector<std::string> &log)
        {
            const std::string path = "ASSETS/LEVELS/" + folder + "/" + folder + ".";
            std::unordered_map<uint32_t, std::string> texts = commonTexts;
            const auto level = tables.find(dm2::nameHash(formatted(kLevelStrings, code.c_str())));
            if (level != tables.end() && !level->second.empty())
                for (const auto &[key, text] : dm2Strings(level->second))
                    texts[key] = text;
            const std::vector<BmxObjective> objectives = readObjectives(readFile(disc, "ASSETS/LEVELS/" + folder + "/OBJCTV.TXT"));
            std::vector<std::string> goalNames;
            for (const BmxObjective &o : objectives)
                if (!o.name.empty())
                    goalNames.push_back(o.name.substr(0, o.name.find('.')));
            std::optional<Bytes> dat;
            if (Bytes d; disc.readAny(path + "DAT", d))
                dat = std::move(d);
            const Bytes ins = needFile(disc, path + "INS");
            const GoalChunks goals = goalChunks(objectives, texts, dm2GoalPlaces(geo, ins), givers(dat, goalNames));
            log.push_back(formatted("%s: %d objectives, %d goals, %d placed, %d targets, %d givers", folder.c_str(),
                                 int(objectives.size()), goals.shown, goals.placed, goals.targets, goals.givers));
            std::string line;
            writeMade(out / (folder + ".pmap"), dm2FormatLevelFile(geo, ins, folder, title, goals.bytes, goals.shown, false, line));
            log.push_back(line);
        }
    }

    void makeBmxXxxMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                        std::vector<std::string> &log)
    {
        const Elf elf(needFile(disc, "SLUS_204.15"));
        making("UI.pmap", [&] { buildUi(disc, elf, out, log); });
        const Bytes stringArchive = needFile(disc, "ASSETS/STRING.ZAL");
        const std::unordered_map<uint32_t, ByteView> tables = zdirNamed(stringArchive);
        const auto common = tables.find(dm2::nameHash(kCommonStrings));
        if (common == tables.end())
            throw PanelError("BMX XXX's string tables have no common one");
        const std::unordered_map<uint32_t, std::string> commonTexts = dm2Strings(common->second);
        for (uint32_t row = 0; row < kLevelRows; ++row)
        {
            const uint32_t at = kLevelTable + row * kLevelRowBytes;
            const std::string code = elf.text(elf.word(at)), folder = elf.text(elf.word(at + 4)),
                              title = elf.text(elf.word(at + 8));
            if (!wanted(extras, folder))
                continue;
            const std::string path = "ASSETS/LEVELS/" + folder + "/" + folder + ".";
            Bytes geo;
            if (!disc.readAny(path + "GEO", geo) || geo.empty())
            {
                log.push_back(formatted("%s (%s): not on the disc", folder.c_str(), title.c_str()));
                continue;
            }
            making("the level " + folder, [&] { buildLevel(disc, code, folder, title, geo, tables, commonTexts, out, log); });
        }
    }
}
