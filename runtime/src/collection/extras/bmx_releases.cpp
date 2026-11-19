// BMX XXX's Xbox and GameCube content (collection/extras/bmx_releases.h), as
// tools/bmx_releases.py makes it.
#include "collection/extras/bmx_releases.h"
#include "script_text.h"
#include "sequence_matcher.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace ps2x::app::extras
{
    namespace
    {
        namespace text = script_text;

        const char *const kChallengeLevels[] = {"CHL_MNCO", "CHL_NASA", "CHL_SHEP", "CHL_SYRA"};
        const char *const kLanguages[] = {"fre", "ger", "spa"};
        constexpr const char kEnglish[] = "eus";
        const char *const kTableGroups[] = {"common",   "level.dm", "level.lp", "level.mo", "level.my",
                                            "level.ra", "level.se", "level.sh", "level.sy", "level.tf",
                                            "level.tg", "level.tv", "level.ug"};
        // The PS2 level select's PSM names (UI_menu FrameLevelSelect, by menu
        // value) and the Xbox movie of each.
        const std::pair<const char *, const char *> kMenuMovies[] = {
            {"BRONX", "bronx"}, {"DAM", "dam"}, {"RAMPAGE", "rampage"}, {"SHEEP", "sheep"},
            {"SYRACUSE", "syracuse"}, {"UGP", "ugp"}, {"LASVEGAS", "lasvegas"}, {"NASA", "nasa"}};
        // The GameCube's levels with rows in the PS2's level table (g_levels)
        // and no files on the PS2 disc, in the PS2's formats.
        const char *const kGameCubeLevels[] = {"CMP_SCRT", "TST_FLAT", "TST_GRND", "TST_VERT"};

        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        void put32(Bytes &out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(uint8_t(v >> (8 * i)));
        }

        std::string asText(const Bytes &data) { return std::string(data.begin(), data.end()); }
        Bytes asBytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

        template <typename... Args>
        std::string format(const char *pattern, Args... args)
        {
            char buffer[1024];
            std::snprintf(buffer, sizeof buffer, pattern, args...);
            return buffer;
        }

        // The engine's name hash (zge/text/strings.h nameHashOf): the ELF hash
        // with ASCII letters upper-cased.
        uint32_t nameHash(std::string_view name)
        {
            uint32_t h = 0;
            for (unsigned char c : name)
            {
                if (c >= 'a' && c <= 'z')
                    c = static_cast<unsigned char>(c - 32);
                h = (h << 4) + c;
                const uint32_t high = h & 0xF0000000u;
                if (high)
                    h ^= high >> 24;
                h &= ~high;
            }
            return h;
        }

        // ---- the ZDIR library (assets\string.zal) ---------------------------

        struct LibraryEntry
        {
            uint32_t key;
            Bytes data;
        };

        // A ZDIR library's entries (DIDF, GRID, DLEN, OFST); empty when it is not one.
        std::optional<std::map<uint32_t, Bytes>> readZdir(const Bytes &data)
        {
            if (data.size() < 0x18 || std::memcmp(data.data(), "ZDIR", 4) != 0 || std::memcmp(data.data() + 8, "DIDF", 4) != 0)
                return std::nullopt;
            const uint32_t count = le32(data.data() + 0x10);
            const size_t grid = 0x18, lengths = grid + 8 + 4 * size_t(count), offsets = lengths + 8 + 4 * size_t(count);
            if (offsets + 8 + 4 * size_t(count) > data.size() || std::memcmp(data.data() + grid, "GRID", 4) != 0 ||
                std::memcmp(data.data() + lengths, "DLEN", 4) != 0 || std::memcmp(data.data() + offsets, "OFST", 4) != 0)
                return std::nullopt;
            std::map<uint32_t, Bytes> out;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t key = le32(data.data() + grid + 8 + 4 * i);
                const uint32_t word = le32(data.data() + lengths + 8 + 4 * i);
                const size_t at = le32(data.data() + offsets + 8 + 4 * i);
                const size_t size = word & 0x0FFFFFFF;
                const size_t from = std::min(at, data.size()), to = std::min(at + size, data.size());
                out[key] = Bytes(data.begin() + long(from), data.begin() + long(to));
            }
            return out;
        }

        // ---- string tables (zge/text/strings.h StringTable) -----------------

        struct TableEntry
        {
            uint32_t key;
            std::optional<std::string> text; // none: offset 0
            bool operator==(const TableEntry &) const = default;
        };
        using Table = std::vector<TableEntry>;

        Table readTable(const Bytes &blob)
        {
            Table out;
            if (blob.size() < 8)
                return out;
            const uint32_t count = le32(blob.data() + 4);
            for (uint32_t i = 0; i < count && 16 + 8 * size_t(i) <= blob.size(); ++i)
            {
                const uint32_t key = le32(blob.data() + 8 + 8 * i), at = le32(blob.data() + 12 + 8 * i);
                if (at == 0)
                    out.push_back({key, std::nullopt});
                else
                {
                    const auto end = std::find(blob.begin() + std::min<long>(at, long(blob.size())), blob.end(), 0);
                    out.push_back({key, std::string(blob.begin() + std::min<long>(at, long(blob.size())), end)});
                }
            }
            return out;
        }

        // A table's bytes: size, count, the entries, then each text, in the
        // entries' order.
        Bytes writeTable(const Table &entries)
        {
            const uint32_t head = uint32_t(8 + 8 * entries.size());
            std::string texts;
            std::vector<uint32_t> offsets;
            for (const TableEntry &e : entries)
            {
                if (!e.text)
                    offsets.push_back(0);
                else
                {
                    offsets.push_back(head + uint32_t(texts.size()));
                    texts += *e.text;
                    texts += '\0';
                }
            }
            Bytes out;
            put32(out, head + uint32_t(texts.size()));
            put32(out, uint32_t(entries.size()));
            for (size_t i = 0; i < entries.size(); ++i)
            {
                put32(out, entries[i].key);
                put32(out, offsets[i]);
            }
            out.insert(out.end(), texts.begin(), texts.end());
            return out;
        }

        // key -> the text a lookup finds (the key's first entry: stringTableText's linear search).
        std::map<uint32_t, std::optional<std::string>> firstTexts(const Table &entries)
        {
            std::map<uint32_t, std::optional<std::string>> out;
            for (const TableEntry &e : entries)
                out.emplace(e.key, e.text);
            return out;
        }

        std::optional<std::string> lookup(const std::map<uint32_t, std::optional<std::string>> &texts, uint32_t key)
        {
            const auto it = texts.find(key);
            return it == texts.end() ? std::nullopt : it->second;
        }

        struct Merged
        {
            Table entries;
            int added = 0;
            int before = 0; // translated on the PS2 already
        };

        // The PS2 table of a language with each text it leaves in English that
        // another release translates, where that release's English is the
        // PS2's. others: (English, language) tables, the first that has one wins.
        Merged mergeTable(const Table &ps2English, const Table &ps2Language, const std::vector<std::pair<Table, Table>> &others)
        {
            const auto english = firstTexts(ps2English);
            std::vector<std::pair<std::map<uint32_t, std::optional<std::string>>, std::map<uint32_t, std::optional<std::string>>>> sources;
            for (const auto &[e, l] : others)
                sources.push_back({firstTexts(e), firstTexts(l)});
            Merged out;
            std::set<uint32_t> seen;
            for (const TableEntry &entry : ps2Language)
            {
                const bool first = seen.insert(entry.key).second;
                if (!first || !entry.text)
                {
                    out.entries.push_back(entry);
                    continue;
                }
                const std::optional<std::string> ps2Text = lookup(english, entry.key);
                if (entry.text != ps2Text)
                {
                    ++out.before;
                    out.entries.push_back(entry);
                    continue;
                }
                std::optional<std::string> replacement;
                for (const auto &[otherEnglish, otherLanguage] : sources)
                {
                    const std::optional<std::string> theirs = lookup(otherLanguage, entry.key);
                    const std::optional<std::string> theirEnglish = lookup(otherEnglish, entry.key);
                    if (theirs && theirEnglish == ps2Text && theirs != theirEnglish)
                    {
                        replacement = theirs;
                        break;
                    }
                }
                if (!replacement)
                    out.entries.push_back(entry);
                else
                {
                    ++out.added;
                    out.entries.push_back({entry.key, replacement});
                }
            }
            return out;
        }

        std::string tableName(std::string_view group, std::string_view language)
        {
            return "string." + std::string(group) + "." + std::string(language);
        }

        bool stringTables(const SourceFiles &ps2, const SourceFiles *xbox, const SourceFiles *gc,
                          const std::filesystem::path &out, std::vector<std::string> &manifest, std::string *error)
        {
            std::map<std::string, std::map<uint32_t, Bytes>> libraries;
            const std::pair<const char *, const SourceFiles *> releases[] = {{"ps2", &ps2}, {"xbox", xbox}, {"gc", gc}};
            for (const auto &[release, files] : releases)
            {
                Bytes data;
                if (!files || !files->readAny("string.zal", data))
                {
                    if (std::string(release) == "ps2")
                    {
                        *error = "The BMX XXX PS2 disc has no STRING.ZAL.";
                        return false;
                    }
                    continue;
                }
                std::optional<std::map<uint32_t, Bytes>> library = readZdir(data);
                if (!library)
                {
                    *error = std::string("The ") + release + " disc's string.zal is not a ZDIR library.";
                    return false;
                }
                libraries[release] = std::move(*library);
            }
            const std::map<uint32_t, Bytes> &ps2Library = libraries["ps2"];
            for (const char *group : kTableGroups)
            {
                const uint32_t englishKey = nameHash(tableName(group, kEnglish));
                if (!ps2Library.count(englishKey))
                    continue;
                const Table ps2English = readTable(ps2Library.at(englishKey));
                for (const char *language : kLanguages)
                {
                    const uint32_t key = nameHash(tableName(group, language));
                    if (!ps2Library.count(key))
                        continue;
                    std::vector<std::pair<Table, Table>> others;
                    for (const char *release : {"gc", "xbox"})
                    {
                        const auto lib = libraries.find(release);
                        if (lib != libraries.end() && lib->second.count(englishKey) && lib->second.count(key))
                            others.push_back({readTable(lib->second.at(englishKey)), readTable(lib->second.at(key))});
                    }
                    const Merged merged = mergeTable(ps2English, readTable(ps2Library.at(key)), others);
                    if (!merged.added)
                        continue;
                    const std::string name = tableName(group, language);
                    if (!writeFile(out / "strings" / name, writeTable(merged.entries), error))
                        return false;
                    manifest.push_back(format("strings %s: %d texts translated of %zu keys, %d more than the PS2's %d",
                                              name.c_str(), merged.before + merged.added, firstTexts(merged.entries).size(),
                                              merged.added, merged.before));
                }
            }
            return true;
        }

        // ---- challenge scripts ----------------------------------------------

        // A `TIMEOUT "<value>"` line's value, or none.
        std::optional<std::string> timeoutValue(const std::string &line)
        {
            const std::vector<std::string> words = text::splitWords(line);
            if (words.size() == 2 && words[0] == "TIMEOUT" && words[1].size() >= 1 && words[1].front() == '"' &&
                words[1].back() == '"')
                return words[1];
            return std::nullopt;
        }

        // The Xbox script with each changed TIMEOUT line the PS2's; the lines put back.
        std::string ps2Timers(const std::string &ps2Text, const std::string &xboxText, int &restored)
        {
            const std::vector<std::string> a = text::splitLines(ps2Text, true), b = text::splitLines(xboxText, true);
            std::string out;
            restored = 0;
            for (const DiffOpcode &op : diffOpcodes(a, b))
            {
                bool timers = op.tag == DiffOpcode::Tag::Replace && op.aEnd - op.aBegin == op.bEnd - op.bBegin;
                for (size_t i = 0; timers && i < op.aEnd - op.aBegin; ++i)
                    timers = timeoutValue(a[op.aBegin + i]) && timeoutValue(b[op.bBegin + i]);
                if (timers)
                {
                    for (size_t i = op.aBegin; i < op.aEnd; ++i)
                        out += a[i];
                    restored += int(op.aEnd - op.aBegin);
                }
                else
                    for (size_t j = op.bBegin; j < op.bEnd; ++j)
                        out += b[j];
            }
            return out;
        }

        // The AUDIO lines' phrase names (comments dropped).
        std::vector<std::string> audioCues(const std::string &script)
        {
            std::vector<std::string> out;
            for (const std::string &line : text::splitLines(script, false))
            {
                const std::vector<std::string> words = text::splitWords(line.substr(0, line.find("//")));
                if (words.size() == 2 && words[0] == "AUDIO")
                    out.push_back(text::strip(words[1], '"'));
            }
            return out;
        }

        size_t outcomeLines(const std::vector<std::string> &cues)
        {
            return size_t(std::count_if(cues.begin(), cues.end(), [](const std::string &c)
            {
                const std::string low = text::lower(c);
                return text::endsWith(low, "_success") || text::endsWith(low, "_failure");
            }));
        }

        bool challenges(const SourceFiles &ps2, const std::vector<const SourceFiles *> &others,
                        const std::filesystem::path &out, std::vector<std::string> &manifest, std::vector<std::string> &log,
                        std::string *error)
        {
            for (const char *level : kChallengeLevels)
            {
                const std::string path = std::string("levels/") + level + "/objctv.txt";
                Bytes ps2Data;
                std::vector<std::string> theirs;
                for (const SourceFiles *other : others)
                {
                    Bytes data;
                    if (other && other->readAny(path, data))
                        theirs.push_back(asText(data));
                }
                if (!ps2.readAny(path, ps2Data) || theirs.empty())
                {
                    log.push_back(std::string("bmx_releases: no ") + level +
                                  " objctv.txt in the PS2 and the other releases' files: left out");
                    continue;
                }
                const std::string &script = theirs[0];
                for (size_t i = 1; i < theirs.size(); ++i)
                    if (theirs[i] != script)
                        log.push_back(std::string("bmx_releases: ") + level +
                                      ": the Xbox and GameCube scripts differ; the first given is used");
                const std::string ps2Text = asText(ps2Data);
                int restored = 0;
                const std::string fixed = ps2Timers(ps2Text, script, restored);
                if (!writeFile(out / "challenges" / "xbox" / level / "OBJCTV.TXT", script, error) ||
                    !writeFile(out / "challenges" / "ps2-timers" / level / "OBJCTV.TXT", fixed, error))
                    return false;
                const std::vector<std::string> ps2Cues = audioCues(ps2Text), cues = audioCues(script);
                manifest.push_back(format(
                    "challenges %s: %zu success and failure lines (the PS2's %zu; its AUDIO_PLACEHOLDER %zu), %d TIMEOUT "
                    "put back to the PS2's",
                    level, outcomeLines(cues), outcomeLines(ps2Cues),
                    size_t(std::count(ps2Cues.begin(), ps2Cues.end(), "AUDIO_PLACEHOLDER")), restored));
            }
            return true;
        }

        // ---- the park editor's pieces ---------------------------------------

        struct ObjectDef
        {
            std::string name; // MaxObjectName, lower case
            std::string body;
        };

        // A piece list's definitions, in its order.
        std::vector<ObjectDef> objectDefs(const std::string &list)
        {
            std::vector<ObjectDef> out;
            size_t at = 0;
            while (true)
            {
                const size_t begin = list.find("\t\tObjectDefBegin", at);
                if (begin == std::string::npos)
                    return out;
                size_t end = list.find("ObjectDefEnd", begin);
                if (end == std::string::npos)
                    return out;
                end = list.find('\n', end + std::strlen("ObjectDefEnd"));
                end = end == std::string::npos ? list.size() : end + 1;
                const std::string body = list.substr(begin, end - begin);
                std::string name;
                for (const std::string &line : text::splitLines(body, false))
                {
                    const std::vector<std::string> words = text::splitWords(line, 1);
                    if (words.size() == 2 && words[0] == "MaxObjectName")
                        name = text::strip(text::strip(words[1]), '"');
                }
                out.push_back({text::lower(name), body});
                at = end;
            }
        }

        // A definition with its Name in capitals, as the PS2's list writes them.
        std::string capitalsName(const std::string &body)
        {
            std::string out;
            for (const std::string &line : text::splitLines(body, true))
            {
                const std::vector<std::string> words = text::splitWords(line, 1);
                if (words.size() == 2 && words[0] == "Name")
                {
                    const std::string value = text::strip(words[1]);
                    out += text::replaceAll(line, value, text::upper(value));
                }
                else
                    out += line;
            }
            return out;
        }

        bool editorPieces(const SourceFiles &ps2, const std::vector<const SourceFiles *> &others,
                          const std::filesystem::path &out, std::vector<std::string> &manifest, std::string *error)
        {
            const std::optional<std::string> ps2File = ps2.find("editor/object.obj");
            std::optional<std::string> theirs;
            for (const SourceFiles *other : others)
            {
                if (!other)
                    continue;
                std::optional<std::string> file = other->find("editor/object00.obj");
                if (!file)
                    file = other->find("editor/object.obj");
                if (file && ps2File && other->size(*file) != ps2.size(*ps2File))
                {
                    Bytes data;
                    if (!other->read(*file, data))
                    {
                        *error = "Could not read " + *file + ".";
                        return false;
                    }
                    theirs = asText(data);
                    break;
                }
            }
            if (!ps2File || !theirs)
            {
                manifest.push_back("editor: no other piece list");
                return true;
            }
            Bytes ps2Data;
            if (!ps2.read(*ps2File, ps2Data))
            {
                *error = "Could not read " + *ps2File + ".";
                return false;
            }
            std::string list = asText(ps2Data);
            std::set<std::string> have;
            for (const ObjectDef &def : objectDefs(list))
                have.insert(def.name);
            std::vector<ObjectDef> added;
            for (const ObjectDef &def : objectDefs(*theirs))
                if (!def.name.empty() && !have.count(def.name))
                    added.push_back({def.name, capitalsName(def.body)});
            size_t end = list.rfind("ObjectListEnd");
            // (the last "\t}" wholly before it: Python's rindex("\t}", 0, end))
            end = end == std::string::npos || end < 2 ? std::string::npos : list.rfind("\t}", end - 2);
            if (end == std::string::npos)
            {
                *error = "The BMX XXX PS2 disc's editor/object.obj has no ObjectListEnd.";
                return false;
            }
            std::string inserted;
            for (const ObjectDef &def : added)
                inserted += def.body;
            list.insert(end, inserted);
            if (!writeFile(out / "editor" / "OBJECT.OBJ", list, error))
                return false;
            std::string names;
            for (size_t i = 0; i < added.size(); ++i)
                names += (i ? ", " : "") + added[i].name;
            manifest.push_back(format("editor OBJECT.OBJ: %zu pieces (the PS2's %zu), added after them: %s",
                                      have.size() + added.size(), have.size(), names.c_str()));
            return true;
        }

        // ---- movies -----------------------------------------------------------

        // The Xbox's movies as they are, for the game's movie player to play
        // in place of the PSM and PSS files the PS2 names.
        bool movies(const SourceFiles *xbox, const std::filesystem::path &out, std::vector<std::string> &manifest,
                    std::string *error)
        {
            if (!xbox)
            {
                manifest.push_back("movies: no Xbox files: none");
                return true;
            }
            std::vector<std::pair<std::string, std::string>> jobs; // (Xbox movie, made file below movies/)
            for (const auto &[ps2, x] : kMenuMovies)
                jobs.push_back({std::string("movies/menu/") + x + ".xmv", std::string("ASSETS/MOVIES/MENU/") + ps2 + ".XMV"});
            jobs.push_back({"movies/zaxis.xmv", "ASSETS/MOVIES/ZAXIS.XMV"});
            for (const auto &[source, target] : jobs)
            {
                const std::optional<std::string> found = xbox->find(source);
                if (!found)
                {
                    manifest.push_back("movies " + target + ": no Xbox movie");
                    continue;
                }
                Bytes data;
                if (!xbox->read(*found, data))
                {
                    *error = "Could not read the Xbox movie " + *found + ".";
                    return false;
                }
                if (!writeFile(out / "movies" / target, data, error))
                    return false;
                manifest.push_back(format("movies %s: the Xbox's %s as it is, %zu bytes", target.c_str(),
                                          found->substr(found->rfind('/') + 1).c_str(), data.size()));
            }
            return true;
        }

        // ---- the GameCube's levels --------------------------------------------

        // A TXR's texture chunks by kind: FORMs of TXRCs, each a TXRH and a
        // picture chunk ("(cut short)" when the chunks overrun).
        void textureKinds(const Bytes &data, size_t at, size_t end, std::map<std::string, int> &found,
                          std::vector<std::string> &order)
        {
            end = std::min(end, data.size());
            while (at + 8 <= end)
            {
                const std::string tag(reinterpret_cast<const char *>(data.data() + at), 4);
                const size_t size = le32(data.data() + at + 4);
                if (tag == "FORM" || tag == "TXRC")
                    textureKinds(data, at + 8, at + 8 + size, found, order);
                else if (tag != "TXRH")
                {
                    if (!found.count(tag))
                        order.push_back(tag);
                    ++found[tag];
                }
                at += 8 + size;
            }
            if (at != end)
            {
                if (!found.count("(cut short)"))
                    order.push_back("(cut short)");
                found["(cut short)"] = 1;
            }
        }

        // Python's repr of the kinds' dict ({'TBMP': 2, 'TDDS': 1}), in the order found.
        std::string kindsRepr(const std::map<std::string, int> &found, const std::vector<std::string> &order)
        {
            std::string out = "{";
            for (size_t i = 0; i < order.size(); ++i)
                out += (i ? ", '" : "'") + order[i] + "': " + std::to_string(found.at(order[i]));
            return out + "}";
        }

        bool gameCubeLevels(const SourceFiles *gc, const std::filesystem::path &out, std::vector<std::string> &manifest,
                            std::string *error)
        {
            if (!gc)
            {
                manifest.push_back("levels: no GameCube files: none");
                return true;
            }
            for (const char *level : kGameCubeLevels)
            {
                const std::string folder = lowerAscii(std::string("levels/") + level + "/");
                std::vector<std::string> names;
                for (const std::string &file : gc->files())
                    if (lowerAscii(file).compare(0, folder.size(), folder) == 0 &&
                        file.find('/', folder.size()) == std::string::npos)
                        names.push_back(file);
                std::sort(names.begin(), names.end(), [&](const std::string &a, const std::string &b)
                          { return a.substr(folder.size()) < b.substr(folder.size()); });
                if (!gc->hasFolder(folder))
                {
                    manifest.push_back(std::string("levels ") + level + ": not in the GameCube files");
                    continue;
                }
                std::optional<std::string> txr;
                for (const std::string &name : names)
                    if (!txr && upperAscii(name.substr(folder.size())) == std::string(level) + ".TXR")
                        txr = name;
                std::map<std::string, int> kinds;
                std::vector<std::string> order;
                if (txr)
                {
                    Bytes data;
                    if (!gc->read(*txr, data))
                    {
                        *error = "Could not read " + *txr + ".";
                        return false;
                    }
                    textureKinds(data, 0, data.size(), kinds, order);
                }
                std::set<std::string> kindSet;
                for (const auto &[kind, n] : kinds)
                    kindSet.insert(kind);
                if (!txr || (kindSet != std::set<std::string>{"TBMP"} && kindSet != std::set<std::string>{"TBMP", "TZMP"}))
                {
                    manifest.push_back(std::string("levels ") + level + ": textures " +
                                       (kinds.empty() ? std::string("none") : kindsRepr(kinds, order)) +
                                       ", not the PS2's: left out");
                    continue;
                }
                uint64_t total = 0;
                for (const std::string &name : names)
                {
                    Bytes data;
                    if (!gc->read(name, data))
                    {
                        *error = "Could not read " + name + ".";
                        return false;
                    }
                    if (!writeFile(out / "levels" / "ASSETS" / "LEVELS" / level / upperAscii(name.substr(folder.size())), data,
                                   error))
                        return false;
                    total += data.size();
                }
                std::string counts;
                for (const auto &[kind, n] : kinds) // sorted by kind, as Python's sorted(kinds.items())
                    counts += (counts.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
                manifest.push_back(format("levels %s: %zu files, %llu bytes, %s", level, names.size(),
                                          (unsigned long long)total, counts.c_str()));
            }
            return true;
        }
    }

    bool makeBmxReleases(const SourceFiles &ps2, const SourceFiles *xbox, const SourceFiles *gc,
                         const std::filesystem::path &out, std::vector<std::string> &log, std::string *error)
    {
        if (!xbox && !gc)
        {
            *error = "BMX XXX's extras need its Xbox or GameCube disc.";
            return false;
        }
        std::vector<std::string> manifest;
        if (!challenges(ps2, {xbox, gc}, out, manifest, log, error) || !stringTables(ps2, xbox, gc, out, manifest, error) ||
            !editorPieces(ps2, {gc, xbox}, out, manifest, error) || !movies(xbox, out, manifest, error) ||
            !gameCubeLevels(gc, out, manifest, error))
            return false;
        std::string text;
        for (const std::string &line : manifest)
        {
            text += line + "\n";
            log.push_back("bmx_releases: " + line);
        }
        if (manifest.empty())
            text = "\n";
        return writeFile(out / "manifest.txt", text, error);
    }
}
