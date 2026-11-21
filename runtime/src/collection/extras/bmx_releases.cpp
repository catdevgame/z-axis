// BMX XXX's Xbox and GameCube content (collection/extras/bmx_releases.h), as
// tools/bmx_releases.py makes it.
#include "collection/extras/bmx_releases.h"
#include "script_text.h"
#include "sequence_matcher.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace zx::app::extras
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
        // The Competitive Edge trailers the PS2's FrameAKAdemos names and only
        // the Xbox has (its AKA_OPT05 and AKA_OPT06 lines).
        const char *const kTrailers[] = {"Turok", "Vexx"};
        // The create-a-rider libraries (assets/create): the four texture
        // libraries and the parts; a named library's entries start on this.
        const char *const kCreateTextures[] = {"TEXTUREA", "TEXTUREB", "TEXTUREC", "TEXTURED"};
        constexpr size_t kLibraryAlign = 32;
        // An ASF's (a WMA's) File Properties object.
        const uint8_t kAsfFileProperties[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                                0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};

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
            for (const char *trailer : kTrailers)
                jobs.push_back({std::string("movies/CompEdge/") + trailer + ".xmv",
                                std::string("ASSETS/MOVIES/COMPEDGE/") + upperAscii(trailer) + ".XMV"});
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

        // ---- the Xbox's unused songs --------------------------------------

        // A WMA's (ASF's) play time: its File Properties object's play
        // duration (100 ns units) less its preroll (ms).
        double asfSeconds(const Bytes &data)
        {
            const auto at = std::search(data.begin(), data.end(), std::begin(kAsfFileProperties), std::end(kAsfFileProperties));
            if (at == data.end() || size_t(at - data.begin()) + 88 > data.size())
                return 0.0;
            const uint8_t *p = data.data() + (at - data.begin()) + 64;
            auto le64 = [](const uint8_t *q) { return uint64_t(le32(q)) | (uint64_t(le32(q + 4)) << 32); };
            return std::max(double(le64(p)) / 1e7 - double(le64(p + 16)) / 1000.0, 0.0);
        }

        // The wave files an Xbox music bank (ZSNDXBOX) names, lower case.
        std::set<std::string> bankWaveNames(const Bytes &data)
        {
            std::set<std::string> names;
            const std::string text = asText(data);
            for (size_t end = text.find(".WMA"); end != std::string::npos; end = text.find(".WMA", end + 4))
            {
                size_t start = end;
                while (start > 0 && text[start - 1] > 32 && text[start - 1] < 127)
                    --start;
                names.insert(lowerAscii(text.substr(start, end + 4 - start)));
            }
            return names;
        }

        // music/<NN>.WMA and music/songs.txt: the songs on the Xbox disc
        // (audio/*.wma) its music bank (audio/music.zsd) does not name, in
        // name order, as the disc has them (the Python tool's are MP3s; the
        // list is the same but for the files' names).
        bool xboxSongs(const SourceFiles *xbox, const std::filesystem::path &out, std::vector<std::string> &manifest,
                       std::string *error)
        {
            Bytes bank;
            if (!xbox || !xbox->readAny("audio/music.zsd", bank))
            {
                manifest.push_back(xbox ? "songs: no Xbox music bank: none" : "songs: no Xbox files: none");
                return true;
            }
            const std::set<std::string> named = bankWaveNames(bank);
            std::vector<std::string> files;
            for (const std::string &file : xbox->files())
            {
                const std::string lower = lowerAscii(file);
                if (lower.rfind("audio/", 0) == 0 && lower.find('/', 6) == std::string::npos && lower.size() > 10 &&
                    lower.compare(lower.size() - 4, 4, ".wma") == 0 && !named.count(lower.substr(6)))
                    files.push_back(file);
            }
            std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) { return lowerAscii(a) < lowerAscii(b); });
            std::string listing;
            for (size_t number = 0; number < files.size(); ++number)
            {
                Bytes data;
                if (!xbox->read(files[number], data))
                {
                    *error = "Could not read the Xbox song " + files[number] + ".";
                    return false;
                }
                const std::string name = files[number].substr(6), song = format("%02zu.WMA", number + 1);
                if (!writeFile(out / "music" / song, data, error))
                    return false;
                const double seconds = asfSeconds(data);
                listing += song + "\t" + name + "\t" + name.substr(0, name.rfind('.')) + "\t" + format("%.3f", seconds) + "\n";
                manifest.push_back(format("songs %s: %s, %.1f s", song.c_str(), name.c_str(), seconds));
            }
            if (files.empty())
                return true;
            if (!writeFile(out / "music" / "songs.txt", listing, error))
                return false;
            manifest.push_back(format("songs: %zu (the Xbox bank names %zu)", files.size(), named.size()));
            return true;
        }

        // ---- create-a-rider -----------------------------------------------

        // A ZDIR library's entry with its name (the create libraries' NAME
        // chunk: the names, NUL-separated, in the entries' order).
        struct NamedEntry
        {
            uint32_t key = 0;
            uint32_t flags = 0;
            Bytes data;
            std::string name;
        };

        bool readNamedZdir(const Bytes &data, std::vector<NamedEntry> &out)
        {
            out.clear();
            if (data.size() < 0x18 || std::memcmp(data.data(), "ZDIR", 4) != 0)
                return false;
            const uint32_t count = le32(data.data() + 0x10);
            const size_t grid = 0x18, lengths = grid + 8 + 4 * size_t(count), offsets = lengths + 8 + 4 * size_t(count),
                         names = offsets + 8 + 4 * size_t(count);
            if (names + 8 > data.size() || std::memcmp(data.data() + grid, "GRID", 4) != 0 ||
                std::memcmp(data.data() + lengths, "DLEN", 4) != 0 || std::memcmp(data.data() + offsets, "OFST", 4) != 0 ||
                std::memcmp(data.data() + names, "NAME", 4) != 0)
                return false;
            const size_t namesEnd = std::min(data.size(), names + 8 + size_t(le32(data.data() + names + 4)));
            size_t at = names + 8;
            for (uint32_t i = 0; i < count; ++i)
            {
                NamedEntry entry;
                entry.key = le32(data.data() + grid + 8 + 4 * i);
                const uint32_t word = le32(data.data() + lengths + 8 + 4 * i);
                entry.flags = word >> 28;
                const size_t from = le32(data.data() + offsets + 8 + 4 * i), size = word & 0x0FFFFFFF;
                if (from + size > data.size())
                    return false;
                entry.data.assign(data.begin() + long(from), data.begin() + long(from + size));
                const size_t end = std::find(data.begin() + long(std::min(at, namesEnd)), data.begin() + long(namesEnd), 0) -
                                   data.begin();
                entry.name.assign(data.begin() + long(std::min(at, namesEnd)), data.begin() + long(end));
                at = end + 1;
                out.push_back(std::move(entry));
            }
            return true;
        }

        // A ZDIR library with its NAME chunk, each entry at kLibraryAlign, the
        // file padded to it (zeros).
        Bytes writeNamedZdir(const std::vector<NamedEntry> &entries)
        {
            const uint32_t count = uint32_t(entries.size());
            Bytes names;
            for (const NamedEntry &entry : entries)
            {
                names.insert(names.end(), entry.name.begin(), entry.name.end());
                names.push_back(0);
            }
            names.resize((names.size() + 3) & ~size_t{3}, 0);
            Bytes body;
            auto tag = [&](const char *name, uint32_t size)
            {
                body.insert(body.end(), name, name + 4);
                put32(body, size);
            };
            tag("DIDF", 8);
            put32(body, count);
            put32(body, 0);
            tag("GRID", 4 * count);
            for (const NamedEntry &entry : entries)
                put32(body, entry.key);
            tag("DLEN", 4 * count);
            for (const NamedEntry &entry : entries)
                put32(body, (entry.flags << 28) | uint32_t(entry.data.size()));
            const size_t offsets = 8 + body.size() + 8;
            tag("OFST", 4 * count);
            body.resize(body.size() + 4 * size_t(count), 0);
            tag("NAME", uint32_t(names.size()));
            body.insert(body.end(), names.begin(), names.end());
            Bytes out;
            out.insert(out.end(), {'Z', 'D', 'I', 'R'});
            put32(out, uint32_t(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            for (uint32_t i = 0; i < count; ++i)
            {
                out.resize((out.size() + kLibraryAlign - 1) & ~(kLibraryAlign - 1), 0);
                const uint32_t at = uint32_t(out.size());
                for (int b = 0; b < 4; ++b)
                    out[offsets + 4 * i + size_t(b)] = uint8_t(at >> (8 * b));
                out.insert(out.end(), entries[i].data.begin(), entries[i].data.end());
            }
            out.resize((out.size() + kLibraryAlign - 1) & ~(kLibraryAlign - 1), 0);
            return out;
        }

        // A part's model (FMDL > MODL): its chunks (TXRC, TGEO, TGIN), or
        // none.
        std::optional<std::vector<std::pair<std::string, Bytes>>> modelChunks(const Bytes &blob)
        {
            if (blob.size() < 16 || std::memcmp(blob.data(), "FMDL", 4) != 0 || std::memcmp(blob.data() + 8, "MODL", 4) != 0)
                return std::nullopt;
            const size_t end = std::min(blob.size(), 16 + size_t(le32(blob.data() + 12)));
            std::vector<std::pair<std::string, Bytes>> out;
            for (size_t at = 16; at + 8 <= end;)
            {
                const size_t size = le32(blob.data() + at + 4);
                if (at + 8 + size > blob.size())
                    return std::nullopt;
                out.push_back({std::string(blob.begin() + long(at), blob.begin() + long(at + 4)),
                               Bytes(blob.begin() + long(at + 8), blob.begin() + long(at + 8 + size))});
                at += 8 + size;
            }
            return out;
        }

        Bytes modelBlob(const std::vector<std::pair<std::string, Bytes>> &chunks)
        {
            Bytes body;
            for (const auto &[tag, data] : chunks)
            {
                body.insert(body.end(), tag.begin(), tag.end());
                put32(body, uint32_t(data.size()));
                body.insert(body.end(), data.begin(), data.end());
            }
            Bytes modl{'M', 'O', 'D', 'L'};
            put32(modl, uint32_t(body.size()));
            modl.insert(modl.end(), body.begin(), body.end());
            Bytes out{'F', 'M', 'D', 'L'};
            put32(out, uint32_t(modl.size()));
            out.insert(out.end(), modl.begin(), modl.end());
            return out;
        }

        // The part a create-a-rider texture goes on, as
        // createAModelGetFilteredListOfAssetsInLibrary groups them: "G" for
        // the texture's "T", up to its second underscore (TMHat_Bunny_Bk_03 ->
        // GMHat_Bunny), lower case.
        std::string partGroup(const std::string &texture)
        {
            const size_t first = texture.find('_');
            if (first == std::string::npos || texture.empty())
                return {};
            const size_t second = texture.find('_', first + 1);
            return lowerAscii("G" + texture.substr(1, (second == std::string::npos ? texture.size() : second) - 1));
        }

        // ---- a GameCube part made the PS2's -------------------------------

        using Chunks = std::vector<std::pair<std::string, Bytes>>;

        // A run of chunks (4-byte tag, 32-bit size).
        Chunks iffChunks(const Bytes &data)
        {
            Chunks out;
            for (size_t at = 0; at + 8 <= data.size();)
            {
                const size_t size = le32(data.data() + at + 4), end = std::min(data.size(), at + 8 + size);
                out.push_back({std::string(data.begin() + long(at), data.begin() + long(at + 4)),
                               Bytes(data.begin() + long(at + 8), data.begin() + long(end))});
                at += 8 + size;
            }
            return out;
        }

        Bytes iffBlob(const Chunks &chunks)
        {
            Bytes out;
            for (const auto &[tag, data] : chunks)
            {
                out.insert(out.end(), tag.begin(), tag.end());
                put32(out, uint32_t(data.size()));
                out.insert(out.end(), data.begin(), data.end());
            }
            return out;
        }

        const Bytes *chunkOf(const Chunks &chunks, const char *tag)
        {
            for (const auto &[t, data] : chunks)
                if (t == tag)
                    return &data;
            return nullptr;
        }

        // A strip group (TGFM, zge/render/part.h): its word and entries.
        // GX's strips: an entry with kStripStart starts one, the triangles'
        // winding alternating along it; the PS2's (zge/native/strips.h): an
        // entry with kStripStart draws nothing, one with kStripFlipped draws
        // its triangle the other way round.
        constexpr uint16_t kStripStart = 0x8000, kStripFlipped = 0x4000, kStripVertex = 0x3FFF;
        constexpr size_t kPs2GroupEntries = 60; // a PS2 strip group's most entries (every PS2 create part's at most)
        using StripGroups = std::vector<std::pair<uint16_t, std::vector<uint16_t>>>;

        StripGroups readStripGroups(const Bytes &data)
        {
            StripGroups groups;
            for (size_t at = 0; at + 4 <= data.size();)
            {
                const uint16_t head = uint16_t(data[at] | data[at + 1] << 8), count = uint16_t(data[at + 2] | data[at + 3] << 8);
                std::vector<uint16_t> entries;
                for (size_t i = 0; i < count && at + 6 + 2 * i <= data.size(); ++i)
                    entries.push_back(uint16_t(data[at + 4 + 2 * i] | data[at + 5 + 2 * i] << 8));
                groups.push_back({head, std::move(entries)});
                at += (size_t(count) * 2 + 7) & ~size_t{3};
            }
            return groups;
        }

        Bytes writeStripGroups(const StripGroups &groups)
        {
            Bytes out;
            for (const auto &[head, entries] : groups)
            {
                for (const uint16_t v : {head, uint16_t(entries.size())})
                {
                    out.push_back(uint8_t(v));
                    out.push_back(uint8_t(v >> 8));
                }
                for (const uint16_t v : entries)
                {
                    out.push_back(uint8_t(v));
                    out.push_back(uint8_t(v >> 8));
                }
                out.resize((out.size() + 3) & ~size_t{3}, 0);
            }
            return out;
        }

        // GX's strip groups as the PS2's: each strip's entries with the flag
        // of their triangle's turn, a strip longer than a group cut (the next
        // part starting again from the two entries before), packed whole into
        // groups of at most kPs2GroupEntries by their word.
        StripGroups gcStripsToPs2(const StripGroups &groups)
        {
            StripGroups out;
            for (const auto &[head, entries] : groups)
            {
                std::vector<std::vector<uint16_t>> strips;
                std::vector<uint16_t> current;
                uint32_t turn = 0;
                for (const uint16_t entry : entries)
                {
                    if (entry & kStripStart)
                    {
                        if (!current.empty() && !(current.back() & kStripStart))
                        {
                            strips.push_back(current);
                            current.clear();
                        }
                        turn = 0;
                        current.push_back(uint16_t(kStripStart | (entry & kStripVertex)));
                        continue;
                    }
                    current.push_back(uint16_t((entry & kStripVertex) | (current.size() >= 2 && turn % 2 ? kStripFlipped : 0)));
                    ++turn;
                }
                if (!current.empty())
                    strips.push_back(current);
                for (std::vector<uint16_t> strip : strips)
                {
                    while (strip.size() > kPs2GroupEntries)
                    {
                        out.push_back({head, std::vector<uint16_t>(strip.begin(), strip.begin() + long(kPs2GroupEntries))});
                        std::vector<uint16_t> rest = {uint16_t(kStripStart | (strip[kPs2GroupEntries - 2] & kStripVertex)),
                                                      uint16_t(kStripStart | (strip[kPs2GroupEntries - 1] & kStripVertex))};
                        rest.insert(rest.end(), strip.begin() + long(kPs2GroupEntries), strip.end());
                        strip = std::move(rest);
                    }
                    if (!out.empty() && out.back().first == head && out.back().second.size() + strip.size() <= kPs2GroupEntries)
                        out.back().second.insert(out.back().second.end(), strip.begin(), strip.end());
                    else
                        out.push_back({head, strip});
                }
            }
            return out;
        }

        // The triangles strip groups draw, each in its winding from its
        // smallest vertex, sorted.
        std::vector<std::array<uint16_t, 3>> stripTriangles(const StripGroups &groups, bool gamecube)
        {
            std::vector<std::array<uint16_t, 3>> out;
            for (const auto &[head, entries] : groups)
            {
                uint32_t turn = 0;
                for (size_t k = 0; k < entries.size(); ++k)
                {
                    if (entries[k] & kStripStart)
                    {
                        turn = 0;
                        continue;
                    }
                    if (k < 2)
                        continue;
                    const uint16_t a = entries[k - 2] & kStripVertex, b = entries[k - 1] & kStripVertex, c = entries[k] & kStripVertex;
                    const bool flipped = gamecube ? turn % 2 == 1 : (entries[k] & kStripFlipped) != 0;
                    ++turn;
                    if (a == b || b == c || a == c)
                        continue;
                    std::array<uint16_t, 3> t = flipped ? std::array<uint16_t, 3>{b, a, c} : std::array<uint16_t, 3>{a, b, c};
                    while (t[0] > t[1] || t[0] > t[2])
                        t = {t[1], t[2], t[0]};
                    out.push_back(t);
                }
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // A material table (TGEO > TMTB): MATI records of 168 bytes, each a
        // layer count and its 28-byte layers, whose render words are each
        // release's own (the rest of a material is the same).
        constexpr size_t kMaterialRecord = 168, kMaterialLayersAt = 8 + 44, kMaterialLayer = 28;
        using MaterialLayers = std::map<Bytes, Bytes>;

        std::optional<std::vector<size_t>> materialLayers(const Bytes &table)
        {
            std::vector<size_t> out;
            for (size_t at = 0; at < table.size(); at += kMaterialRecord)
            {
                if (at + kMaterialRecord > table.size() || std::memcmp(table.data() + at, "MATI", 4) != 0)
                    return std::nullopt;
                const size_t count = le32(table.data() + at + kMaterialLayersAt);
                if (kMaterialLayersAt + 4 + count * kMaterialLayer > kMaterialRecord)
                    return std::nullopt;
                for (size_t i = 0; i < count; ++i)
                    out.push_back(at + kMaterialLayersAt + 4 + i * kMaterialLayer);
            }
            return out;
        }

        std::optional<Bytes> partMaterials(const Bytes &blob)
        {
            const auto chunks = modelChunks(blob);
            if (!chunks || chunks->size() < 2)
                return std::nullopt;
            const Chunks geo = iffChunks((*chunks)[1].second);
            if (const Bytes *table = chunkOf(geo, "TMTB"))
                return *table;
            return std::nullopt;
        }

        // GameCube material layer -> the PS2's: the PS2 layer the parts both
        // releases have give most for it (a tie: the greater bytes).
        MaterialLayers learnMaterialLayers(const std::vector<NamedEntry> &ps2Parts, const std::vector<NamedEntry> &gcParts)
        {
            std::map<std::string, const NamedEntry *> gc;
            for (const NamedEntry &part : gcParts)
                gc[lowerAscii(part.name)] = &part;
            std::map<Bytes, std::map<Bytes, int>> found;
            for (const NamedEntry &part : ps2Parts)
            {
                const auto it = gc.find(lowerAscii(part.name));
                if (it == gc.end())
                    continue;
                const auto mine = partMaterials(part.data), theirs = partMaterials(it->second->data);
                if (!mine || !theirs || mine->size() != theirs->size())
                    continue;
                const auto layers = materialLayers(*mine);
                if (!layers || layers != materialLayers(*theirs))
                    continue;
                for (const size_t at : *layers)
                    ++found[Bytes(theirs->begin() + long(at), theirs->begin() + long(at + kMaterialLayer))]
                           [Bytes(mine->begin() + long(at), mine->begin() + long(at + kMaterialLayer))];
            }
            MaterialLayers out;
            for (const auto &[theirs, counts] : found)
            {
                const auto best = std::max_element(counts.begin(), counts.end(), [](const auto &a, const auto &b)
                                                   { return a.second != b.second ? a.second < b.second : a.first < b.first; });
                out[theirs] = best->first;
            }
            return out;
        }

        // A GameCube create part (FMDL > MODL: TXRC, TGEO, TGIN) in the PS2's
        // (tools/bmx_releases.py gc_part_to_ps2): its 8x8 texture the PS2's
        // placeholder, its strip groups the PS2's (the TGPH's group count with
        // them), its material layers the PS2's. None when it is not such a
        // part, has a layer no shared part shows, or the strips would draw
        // other triangles.
        std::optional<Bytes> gcPartToPs2(const Bytes &blob, const Bytes &placeholder, const MaterialLayers &layers)
        {
            const auto chunks = modelChunks(blob);
            if (!chunks || chunks->size() != 3 || (*chunks)[0].first != "TXRC" || (*chunks)[1].first != "TGEO" ||
                (*chunks)[2].first != "TGIN")
                return std::nullopt;
            Chunks geo = iffChunks((*chunks)[1].second);
            const Bytes *partBytes = chunkOf(geo, "TGPT");
            const Bytes *table = chunkOf(geo, "TMTB");
            if (!partBytes)
                return std::nullopt;
            Bytes materials = table ? *table : Bytes();
            const auto offsets = materialLayers(materials);
            if (!offsets)
                return std::nullopt;
            for (const size_t at : *offsets)
            {
                const auto it = layers.find(Bytes(materials.begin() + long(at), materials.begin() + long(at + kMaterialLayer)));
                if (it == layers.end())
                    return std::nullopt;
                std::copy(it->second.begin(), it->second.end(), materials.begin() + long(at));
            }
            Chunks part = iffChunks(*partBytes);
            const Bytes *head = chunkOf(part, "TGPH");
            const Bytes *faces = chunkOf(part, "TGFM");
            if (!head || !faces || head->size() < 12)
                return std::nullopt;
            const StripGroups groups = readStripGroups(*faces);
            const StripGroups ps2 = gcStripsToPs2(groups);
            if (le32(head->data() + 8) != groups.size() || stripTriangles(ps2, false) != stripTriangles(groups, true))
                return std::nullopt;
            for (auto &[tag, data] : part)
            {
                if (tag == "TGPH")
                    for (int b = 0; b < 4; ++b)
                        data[8 + size_t(b)] = uint8_t(ps2.size() >> (8 * b));
                else if (tag == "TGFM")
                    data = writeStripGroups(ps2);
            }
            for (auto &[tag, data] : geo)
            {
                if (tag == "TGPT")
                    data = iffBlob(part);
                else if (tag == "TMTB")
                    data = materials;
            }
            return modelBlob({{"TXRC", placeholder}, {"TGEO", iffBlob(geo)}, (*chunks)[2]});
        }

        bool readLibrary(const SourceFiles &files, const std::string &path, std::vector<NamedEntry> &entries,
                         std::string *error)
        {
            Bytes data;
            if (!files.readAny(path, data) || !readNamedZdir(data, entries))
            {
                *error = "Could not read " + path + ".";
                return false;
            }
            return true;
        }

        // create/PARTS.ZAL and create/TEXTURE[A-D].ZAL: the PS2's libraries
        // with the entries the other releases have after them (the parts:
        // the GameCube's, its 8x8 texture the PS2's), so every PS2 entry
        // keeps its place; create/added.txt names the textures added.
        bool createARider(const SourceFiles &ps2, const std::vector<const SourceFiles *> &others,
                          const std::filesystem::path &out, std::vector<std::string> &manifest, std::string *error)
        {
            std::vector<NamedEntry> parts;
            if (!readLibrary(ps2, "create/parts.zal", parts, error))
                return false;
            const size_t ps2Parts = parts.size();
            std::optional<Bytes> placeholder;
            if (const auto chunks = parts.empty() ? std::nullopt : modelChunks(parts.front().data))
                for (const auto &[tag, data] : *chunks)
                    if (tag == "TXRC")
                        placeholder = data;
            std::set<std::string> have, otherParts;
            for (const NamedEntry &part : parts)
                have.insert(lowerAscii(part.name));
            std::string addedParts;
            for (const SourceFiles *other : others)
            {
                std::vector<NamedEntry> theirs;
                if (!other || !other->find("create/parts.zal") || !readLibrary(*other, "create/parts.zal", theirs, error))
                    continue;
                const std::vector<NamedEntry> ps2Only(parts.begin(), parts.begin() + long(ps2Parts));
                const MaterialLayers layers = learnMaterialLayers(ps2Only, theirs);
                for (NamedEntry &part : theirs)
                {
                    otherParts.insert(lowerAscii(part.name));
                    auto chunks = modelChunks(part.data);
                    if (have.count(lowerAscii(part.name)) || !chunks || chunks->size() != 3 || (*chunks)[0].first != "TXRC" ||
                        (*chunks)[1].first != "TGEO" || (*chunks)[2].first != "TGIN" || !placeholder)
                        continue;
                    std::map<std::string, int> kinds;
                    std::vector<std::string> order;
                    textureKinds((*chunks)[0].second, 0, (*chunks)[0].second.size(), kinds, order);
                    if (!kinds.count("TDDS"))
                        continue; // (not a GameCube part)
                    const std::optional<Bytes> made = gcPartToPs2(part.data, *placeholder, layers);
                    if (!made)
                        continue;
                    part.data = *made;
                    have.insert(lowerAscii(part.name));
                    addedParts += (addedParts.empty() ? "" : ", ") + part.name;
                    parts.push_back(std::move(part));
                }
            }
            if (!writeFile(out / "create" / "PARTS.ZAL", writeNamedZdir(parts), error))
                return false;
            manifest.push_back(format("create PARTS.ZAL: %zu parts (the PS2's %zu), added after them: %s", parts.size(),
                                      ps2Parts, addedParts.empty() ? "none" : addedParts.c_str()));
            std::string added;
            size_t addedCount = 0, greenSkins = 0, topless = 0;
            for (const char *library : kCreateTextures)
            {
                const std::string path = std::string("create/") + library + ".zal";
                std::vector<NamedEntry> mine;
                if (!readLibrary(ps2, path, mine, error))
                    return false;
                const size_t ps2Count = mine.size();
                std::set<std::string> names;
                for (const NamedEntry &entry : mine)
                    names.insert(lowerAscii(entry.name));
                std::string leftOut;
                size_t leftOutCount = 0;
                for (const SourceFiles *other : others)
                {
                    std::vector<NamedEntry> theirs;
                    if (!other || !other->find(path) || !readLibrary(*other, path, theirs, error))
                        continue;
                    for (NamedEntry &entry : theirs)
                    {
                        if (!names.insert(lowerAscii(entry.name)).second)
                            continue;
                        const std::string group = partGroup(entry.name);
                        if (otherParts.count(group) && !have.count(group))
                        {
                            leftOut += (leftOut.empty() ? "" : ", ") + entry.name;
                            ++leftOutCount;
                            continue;
                        }
                        added += entry.name + "\n";
                        ++addedCount;
                        const std::string upper = upperAscii(entry.name);
                        greenSkins += upper.find("_HUL") != std::string::npos;
                        topless += upper.find("_TOPLESS") != std::string::npos;
                        mine.push_back(std::move(entry));
                    }
                }
                if (!writeFile(out / "create" / (std::string(library) + ".ZAL"), writeNamedZdir(mine), error))
                    return false;
                manifest.push_back(format("create %s.ZAL: %zu textures (the PS2's %zu)", library, mine.size(), ps2Count) +
                                   (leftOutCount ? format(", %zu left out (no part): ", leftOutCount) + leftOut : std::string()));
            }
            if (!writeFile(out / "create" / "added.txt", added, error))
                return false;
            manifest.push_back(format("create added.txt: %zu textures (%zu green skin _Hul, %zu _Topless)", addedCount,
                                      greenSkins, topless));
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
            !gameCubeLevels(gc, out, manifest, error) || !createARider(ps2, {xbox, gc}, out, manifest, error) ||
            !xboxSongs(xbox, out, manifest, error))
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
