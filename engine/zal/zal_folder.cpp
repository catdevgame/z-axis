// An unpacked .ZAL (zal/zal_archive.h): the archive as a file per entry
// and zal.txt, and back.
#include "zal/zal_archive.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace zx::zal
{
    namespace
    {
        std::string upper(std::string_view text)
        {
            std::string out(text);
            for (char &c : out)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        }

        std::string hexOf(const std::vector<uint8_t> &bytes)
        {
            static const char kHex[] = "0123456789abcdef";
            std::string out;
            for (uint8_t b : bytes)
            {
                out += kHex[b >> 4];
                out += kHex[b & 15];
            }
            return out;
        }

        bool allZero(const std::vector<uint8_t> &bytes)
        {
            return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
        }

        bool fail(std::string *error, std::string why)
        {
            if (error)
                *error = std::move(why);
            return false;
        }

        // "0x" and eight hex digits, ending there or at a dot: a name findEntry reads as an id.
        bool looksLikeId(std::string_view name)
        {
            return name.size() >= 10 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X') &&
                   std::all_of(name.begin() + 2, name.begin() + 10, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }) &&
                   (name.size() == 10 || name[10] == '.');
        }

        // The names in some bytes that are a Directory's entries' ids'
        // hashes: every run of printable text, cut at spaces, quotes and
        // brackets, each piece and its last path component. The first found
        // for an id is its name.
        void findNames(const std::vector<uint8_t> &bytes, const std::unordered_set<uint32_t> &ids, std::unordered_map<uint32_t, std::string> &names)
        {
            auto offer = [&](std::string_view piece)
            {
                for (std::string_view candidate : {piece, piece.substr(piece.find_last_of("/\\:") == std::string_view::npos ? 0 : piece.find_last_of("/\\:") + 1)})
                {
                    if (candidate.size() < 3 || !fileNameOk(candidate) || looksLikeId(candidate))
                        continue;
                    const uint32_t id = nameHash(candidate);
                    if (ids.count(id) && !names.count(id))
                        names.emplace(id, std::string(candidate));
                }
            };
            static const std::string_view kCuts = " \t\"'()[]{},;=<>|";
            size_t at = 0;
            while (at < bytes.size())
            {
                while (at < bytes.size() && (bytes[at] < 0x20 || bytes[at] > 0x7E))
                    ++at;
                const size_t begin = at;
                while (at < bytes.size() && bytes[at] >= 0x20 && bytes[at] <= 0x7E)
                    ++at;
                const std::string_view run(reinterpret_cast<const char *>(bytes.data()) + begin, at - begin);
                if (run.size() < 3)
                    continue;
                for (size_t start = 0; start < run.size();)
                {
                    size_t end = run.find_first_of(kCuts, start);
                    if (end == std::string_view::npos)
                        end = run.size();
                    if (end > start)
                        offer(run.substr(start, end - start));
                    start = end + 1;
                }
            }
        }

        bool parseHex(std::string_view text, std::vector<uint8_t> &out)
        {
            if (text.size() % 2 != 0)
                return false;
            out.clear();
            for (size_t i = 0; i < text.size(); i += 2)
            {
                auto digit = [](char c) -> int
                {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                };
                const int high = digit(text[i]), low = digit(text[i + 1]);
                if (high < 0 || low < 0)
                    return false;
                out.push_back(uint8_t(high << 4 | low));
            }
            return true;
        }

        bool parseNumber(std::string_view text, uint64_t &value, uint64_t most)
        {
            int base = 10;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                base = 16;
                text.remove_prefix(2);
            }
            if (text.empty() || text.size() > 16)
                return false;
            value = 0;
            for (char c : text)
            {
                const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                              : base == 16 && std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                                                                                           : -1;
                if (d < 0 || d >= base)
                    return false;
                value = value * uint64_t(base) + uint64_t(d);
                if (value > most)
                    return false;
            }
            return true;
        }

        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
                text.remove_suffix(1);
            return text;
        }

        // The alignment zal.txt gives for an entry: bytes, or "default" (code 0).
        std::string alignmentText(uint32_t flags) { return flags == 0 ? std::string("default") : std::to_string(1u << (flags - 1)); }
    }

    bool fileNameOk(std::string_view name)
    {
        if (name.empty() || name.size() > 255 || name == "." || name == ".." || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
            return false;
        for (char c : name)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos)
                return false;
        const std::string all = upper(name);
        if (all == upper(kListingFile))
            return false;
        // Windows' device names, with any extension.
        const std::string base = all.substr(0, all.find('.'));
        static const char *const kDevices[] = {"CON", "PRN", "AUX", "NUL"};
        if (std::find(std::begin(kDevices), std::end(kDevices), base) != std::end(kDevices))
            return false;
        if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '1' && base[3] <= '9')
            return false;
        return true;
    }

    Unpacked unpack(const Archive &archive, std::string_view title, const std::vector<const std::vector<uint8_t> *> &nameSources)
    {
        const bool directory = archive.format == Format::Directory;
        Unpacked out;
        // The entries' file names, unique without case.
        std::unordered_map<uint32_t, std::string> found;
        if (directory && !archive.names && !archive.entries.empty())
        {
            std::unordered_set<uint32_t> ids;
            for (const Entry &entry : archive.entries)
                ids.insert(entry.id);
            for (const Entry &entry : archive.entries)
                findNames(entry.data, ids, found);
            for (const std::vector<uint8_t> *source : nameSources)
                if (source)
                    findNames(*source, ids, found);
        }
        std::set<std::string> taken;
        std::vector<std::string> fileNames;
        for (const Entry &entry : archive.entries)
        {
            std::string name;
            if (!directory)
                name = libraryName(archive, entry);
            else if (archive.names)
                name = entry.name;
            else if (auto it = found.find(entry.id); it != found.end())
                name = it->second;
            if (!fileNameOk(name) || taken.count(upper(name)))
                name = directory ? idName(entry.id) : libraryName(archive, entry);
            for (int n = 2; taken.count(upper(name)); ++n) // (an id more than one entry has: the game finds only the first)
                name = (directory ? idName(entry.id) : libraryName(archive, entry)) + "~" + std::to_string(n);
            taken.insert(upper(name));
            fileNames.push_back(name);
        }

        // zal.txt.
        std::ostringstream text;
        text << "# zal: the entries of " << title << ", in its order. \"zal repack\" makes it again from this list\n"
             << "# and the files beside it, byte for byte while they are unchanged (docs/MODS.md, \"Archives\").\n";
        uint32_t alignmentFlags = 0;
        if (directory)
        {
            std::map<uint32_t, size_t> codes;
            for (const Entry &entry : archive.entries)
                ++codes[entry.flags];
            size_t most = 0;
            alignmentFlags = archive.entries.empty() ? 5u : 0u;
            for (const auto &[code, count] : codes)
                if (count > most)
                {
                    most = count;
                    alignmentFlags = code;
                }
            text << "format zdir\n"
                 << "names " << (archive.names ? "yes" : "no") << "\n"
                 << "align " << alignmentText(alignmentFlags) << "\n";
            if (archive.reserved != 0)
                text << "reserved " << archive.reserved << "\n";
        }
        else
        {
            text << "format zfile\n"
                 << "assets " << archive.assetsPerGroup << "\n";
            const std::vector<uint8_t> header(archive.header.begin(), archive.header.end());
            if (!allZero(header))
                text << "header " << hexOf(header) << "\n";
        }
        if (!allZero(archive.leadPadding))
            text << "lead " << hexOf(archive.leadPadding) << "\n";
        for (size_t i = 0; i < archive.entries.size(); ++i)
        {
            const Entry &entry = archive.entries[i];
            text << "entry " << (directory ? idName(entry.id) : std::to_string(entry.id)) << " " << fileNames[i] << "\n";
            if (directory && entry.flags != alignmentFlags)
                text << "  align " << alignmentText(entry.flags) << "\n";
            if (directory && archive.names && entry.name != fileNames[i])
                text << "  name " << entry.name << "\n";
            if (!directory && entry.flags != 0)
                text << "  flags " << entry.flags << "\n";
            if (!allZero(entry.padding))
                text << "  pad " << hexOf(entry.padding) << "\n";
            out.files.push_back({fileNames[i], entry.data});
        }
        out.listing = text.str();
        return out;
    }

    bool repack(std::string_view listing, const std::function<bool(const std::string &name, std::vector<uint8_t> &bytes)> &readFile,
                Archive &out, std::string *error)
    {
        Archive archive;
        bool formatGiven = false;
        uint32_t alignmentFlags = 5;
        size_t lineNumber = 0;
        for (size_t start = 0; start < listing.size();)
        {
            size_t end = listing.find('\n', start);
            if (end == std::string_view::npos)
                end = listing.size();
            const std::string_view line = trim(listing.substr(start, end - start));
            start = end + 1;
            ++lineNumber;
            if (line.empty() || line.front() == '#')
                continue;
            const size_t space = line.find_first_of(" \t");
            const std::string_view key = line.substr(0, space);
            const std::string_view value = space == std::string_view::npos ? std::string_view() : trim(line.substr(space));
            auto bad = [&](const std::string &why) { return fail(error, std::string(kListingFile) + " line " + std::to_string(lineNumber) + ": " + why); };
            const bool directory = archive.format == Format::Directory;
            uint64_t number = 0;
            if (key == "format")
            {
                if (value != "zdir" && value != "zfile")
                    return bad("the format is zdir or zfile");
                if (!archive.entries.empty())
                    return bad("the format goes before the entries");
                archive.format = value == "zdir" ? Format::Directory : Format::Library;
                formatGiven = true;
            }
            else if (!formatGiven)
                return bad("the first line is the format (zdir or zfile)");
            else if (key == "names" && directory)
            {
                if (value != "yes" && value != "no")
                    return bad("names is yes or no");
                archive.names = value == "yes";
            }
            else if (key == "align")
            {
                if (!directory)
                    return bad("a zfile library's alignment is 4 bytes, not set");
                uint32_t flags = 0;
                if (value != "default")
                {
                    if (!parseNumber(value, number, 0x8000) || (flags = alignmentCode(uint32_t(number))) == 0)
                        return bad("align is a power of two from 1 to 16384, or default");
                }
                if (archive.entries.empty())
                    alignmentFlags = flags;
                else
                    archive.entries.back().flags = flags;
            }
            else if (key == "reserved" && directory)
            {
                if (!parseNumber(value, number, 0xFFFFFFFFu))
                    return bad("reserved is a number");
                archive.reserved = uint32_t(number);
            }
            else if (key == "assets" && !directory)
            {
                if (!parseNumber(value, number, 0x1FFF) || number == 0)
                    return bad("assets is 1 to 8191");
                archive.assetsPerGroup = uint16_t(number);
            }
            else if (key == "header" && !directory)
            {
                std::vector<uint8_t> header;
                if (!parseHex(value, header) || header.size() != archive.header.size())
                    return bad("header is 12 bytes in hex");
                std::copy(header.begin(), header.end(), archive.header.begin());
            }
            else if (key == "lead")
            {
                if (!parseHex(value, archive.leadPadding))
                    return bad("lead is bytes in hex");
            }
            else if (key == "entry")
            {
                const size_t gap = value.find_first_of(" \t");
                if (gap == std::string_view::npos)
                    return bad("an entry is its id and its file");
                const std::string_view id = value.substr(0, gap);
                const std::string file(trim(value.substr(gap)));
                if (!fileNameOk(file))
                    return bad("\"" + file + "\" is not a file name this reads");
                Entry entry;
                entry.flags = directory ? alignmentFlags : 0;
                if (id == "-" && directory)
                    entry.id = nameHash(file);
                else if (!parseNumber(id, number, 0xFFFFFFFFu))
                    return bad(directory ? "an entry's id is 0x and its hex digits, or - for its file name's hash"
                                         : "an entry's id is its group's number");
                else
                    entry.id = uint32_t(number);
                if (!readFile(file, entry.data))
                    return bad(file + " is listed but is not there");
                entry.name = file;
                archive.entries.push_back(std::move(entry));
            }
            else if (key == "pad" || key == "name" || key == "flags")
            {
                if (archive.entries.empty())
                    return bad(std::string(key) + " belongs to an entry, after its line");
                Entry &entry = archive.entries.back();
                if (key == "pad" && !parseHex(value, entry.padding))
                    return bad("pad is bytes in hex");
                if (key == "name")
                {
                    if (!directory)
                        return bad("a zfile library has no names");
                    entry.name = std::string(value);
                }
                if (key == "flags")
                {
                    if (directory || !parseNumber(value, number, 0xFF))
                        return bad("flags (zfile) is 0 to 255");
                    entry.flags = uint32_t(number);
                }
            }
            else
                return bad("\"" + std::string(key) + "\" is not a line of a " + (directory ? "zdir" : "zfile") + " listing");
        }
        if (!formatGiven)
            return fail(error, std::string(kListingFile) + " has no format line");
        for (size_t i = 0; i < archive.entries.size(); ++i)
        {
            Entry &entry = archive.entries[i];
            if (archive.format == Format::Library)
            {
                entry.asset = uint32_t(i % archive.assetsPerGroup);
                entry.name.clear();
            }
            else if (!archive.names)
                entry.name.clear();
        }
        out = std::move(archive);
        return true;
    }
}
