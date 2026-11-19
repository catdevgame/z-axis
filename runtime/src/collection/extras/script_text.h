#pragma once

// The games' text files (scripts, piece lists: Latin-1 bytes) taken apart as
// the tooling's Python converters take them, so the converters write the
// same bytes: Python's str.splitlines, split(), strip() and find on text
// decoded as Latin-1. Each byte is one character.

#include <string>
#include <string_view>
#include <vector>

namespace ps2x::app::extras::script_text
{
    // Python's whitespace among Latin-1 characters (str.isspace): \t \n \v
    // \f \r, 0x1C to 0x1F, the space, 0x85 (NEL) and 0xA0 (no-break space).
    inline bool isSpace(unsigned char c)
    {
        return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0;
    }

    // str.splitlines(keepends): lines end at \n, \r, \r\n, \v, \f, 0x1C,
    // 0x1D, 0x1E or 0x85.
    inline std::vector<std::string> splitLines(std::string_view text, bool keepEnds)
    {
        std::vector<std::string> out;
        size_t start = 0, i = 0;
        while (i < text.size())
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const bool ends = c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E ||
                              c == 0x85;
            if (!ends)
            {
                ++i;
                continue;
            }
            size_t next = i + 1;
            if (c == '\r' && next < text.size() && text[next] == '\n')
                ++next;
            out.emplace_back(text.substr(start, (keepEnds ? next : i) - start));
            start = i = next;
        }
        if (start < text.size())
            out.emplace_back(text.substr(start));
        return out;
    }

    // str.split(None, maxSplits): words between runs of whitespace; with
    // maxSplits >= 0 the rest after that many splits is the last word (its
    // leading whitespace dropped, its trailing kept).
    inline std::vector<std::string> splitWords(std::string_view text, int maxSplits = -1)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (true)
        {
            while (i < text.size() && isSpace(static_cast<unsigned char>(text[i])))
                ++i;
            if (i == text.size())
                break;
            if (maxSplits >= 0 && int(out.size()) == maxSplits)
            {
                out.emplace_back(text.substr(i));
                break;
            }
            size_t end = i;
            while (end < text.size() && !isSpace(static_cast<unsigned char>(text[end])))
                ++end;
            out.emplace_back(text.substr(i, end - i));
            i = end;
        }
        return out;
    }

    // str.strip(): whitespace off both ends.
    inline std::string strip(std::string_view text)
    {
        size_t begin = 0, end = text.size();
        while (begin < end && isSpace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    // str.strip(chars): those characters off both ends.
    inline std::string strip(std::string_view text, char c)
    {
        size_t begin = 0, end = text.size();
        while (begin < end && text[begin] == c)
            ++begin;
        while (end > begin && text[end - 1] == c)
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    // str.lower() and str.upper() of Latin-1 text: ASCII and the Latin-1
    // letters (À to Þ and à to þ, but × and ÷), ß and ÿ left as they are
    // (Python's would leave Latin-1).
    inline std::string lower(std::string_view text)
    {
        std::string out(text);
        for (char &ch : out)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
                ch = static_cast<char>(c + 0x20);
        }
        return out;
    }
    inline std::string upper(std::string_view text)
    {
        std::string out(text);
        for (char &ch : out)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
                ch = static_cast<char>(c - 0x20);
        }
        return out;
    }

    // str.replace(old, new): every occurrence, left to right.
    inline std::string replaceAll(std::string text, std::string_view from, std::string_view to)
    {
        if (from.empty())
            return text;
        std::string out;
        size_t at = 0;
        for (size_t found; (found = text.find(from, at)) != std::string::npos; at = found + from.size())
            out.append(text, at, found - at).append(to);
        out.append(text, at, std::string::npos);
        return out;
    }

    inline bool endsWith(std::string_view text, std::string_view ending)
    {
        return text.size() >= ending.size() && text.compare(text.size() - ending.size(), ending.size(), ending) == 0;
    }
}
