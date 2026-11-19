#pragma once

// A game's developer tuning table as name = value lines (Mods/tuning.ini):
// the parser and the matching of names to table entries, without guest
// memory, so it can be tested. A game override reads its own table (names,
// values, ranges) and writes the matched values.
//
//     ; comment
//     [Camera]            <- the table's category (any case)
//     FOV = 90            <- an entry of that category (any case, spaces kept inside)
//     Distance = 5.5
//
// An entry without a section is looked up in every category; a name found in
// two categories at different addresses is ambiguous and needs its section.
// A line starting with '#' is a comment unless it names an entry (the game
// has "# cmd delay frames").

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::tuning
{
    struct Entry
    {
        std::string category;
        std::string name;
        uint32_t address = 0; // the entry (name first, then value, max, min, step)
        float value = 0.0f;
        float maxValue = 0.0f;
        float minValue = 0.0f;
        float step = 0.0f;
    };

    struct Line
    {
        int number = 0;
        std::string section; // as written
        std::string key;
        float value = 0.0f;
        bool hashComment = false; // started with '#': silently ignored when it names nothing
    };

    struct Override
    {
        const Entry *entry = nullptr;
        float value = 0.0f;
        int line = 0;
    };

    inline std::string trimmed(std::string_view text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    inline std::string folded(std::string_view text)
    {
        std::string out = trimmed(text);
        for (char &c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Lines with a key and a number; others are reported in warnings.
    inline std::vector<Line> parse(std::string_view text, std::vector<std::string> *warnings = nullptr)
    {
        std::vector<Line> lines;
        std::string section;
        int number = 0;
        size_t pos = 0;
        while (pos <= text.size())
        {
            const size_t eol = text.find('\n', pos);
            const std::string_view raw = text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
            pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;
            ++number;
            const std::string content = trimmed(raw);
            if (content.empty() || content[0] == ';')
                continue;
            if (content.front() == '[' && content.back() == ']')
            {
                section = trimmed(std::string_view(content).substr(1, content.size() - 2));
                continue;
            }
            const bool hash = content[0] == '#';
            const size_t equals = content.rfind('=');
            if (equals == std::string::npos)
            {
                if (!hash && warnings)
                    warnings->push_back("line " + std::to_string(number) + ": expected name = value");
                continue;
            }
            Line line;
            line.number = number;
            line.section = section;
            line.key = trimmed(std::string_view(content).substr(0, equals));
            line.hashComment = hash;
            const std::string value = trimmed(std::string_view(content).substr(equals + 1));
            char *end = nullptr;
            line.value = std::strtof(value.c_str(), &end);
            if (value.empty() || end == value.c_str() || *end != '\0')
            {
                if (!hash && warnings)
                    warnings->push_back("line " + std::to_string(number) + ": not a number: " + value);
                continue;
            }
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // The entries the lines name, in file order (a later line for the same
    // entry wins when applied in order).
    inline std::vector<Override> match(const std::vector<Line> &lines, const std::vector<Entry> &entries,
                                       std::vector<std::string> *warnings = nullptr)
    {
        std::vector<Override> out;
        for (const Line &line : lines)
        {
            const std::string key = folded(line.key);
            const std::string section = folded(line.section);
            const Entry *found = nullptr;
            bool ambiguous = false;
            for (const Entry &entry : entries)
            {
                if (folded(entry.name) != key || (!section.empty() && folded(entry.category) != section))
                    continue;
                if (found && found->address != entry.address)
                    ambiguous = true;
                if (!found)
                    found = &entry;
            }
            const std::string where = "line " + std::to_string(line.number) + ": ";
            if (ambiguous)
            {
                if (warnings)
                    warnings->push_back(where + line.key + " is in more than one category; put it under its [section]");
                continue;
            }
            if (!found)
            {
                if (!line.hashComment && warnings)
                    warnings->push_back(where + "no entry " + (line.section.empty() ? "" : "[" + line.section + "] ") + line.key);
                continue;
            }
            out.push_back({found, line.value, line.number});
        }
        return out;
    }

    // Every entry, commented out, by category: a starting point for a
    // tuning.ini.
    inline std::string reference(const std::vector<Entry> &entries, const std::string &header)
    {
        std::string out = header;
        std::string category;
        char buffer[160];
        for (const Entry &entry : entries)
        {
            if (entry.category != category)
            {
                category = entry.category;
                out += "\n[" + category + "]\n";
            }
            std::snprintf(buffer, sizeof(buffer), "; %s = %g    (%g to %g, step %g)\n", entry.name.c_str(),
                          static_cast<double>(entry.value), static_cast<double>(entry.minValue),
                          static_cast<double>(entry.maxValue), static_cast<double>(entry.step));
            out += buffer;
        }
        return out;
    }
}
