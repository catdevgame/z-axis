#include "runtime/ps2_zaxis_text.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

namespace zx::zaxis_text
{
    namespace
    {
        uint32_t readU32(const std::vector<uint8_t> &data, size_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        void putU32(std::vector<uint8_t> &data, size_t offset, uint32_t value)
        {
            std::memcpy(data.data() + offset, &value, sizeof(value));
        }

        bool hasLower(std::string_view text)
        {
            for (char c : text)
                if (c >= 'a' && c <= 'z')
                    return true;
            return false;
        }

        std::string withLineBreaks(const std::string &text)
        {
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1u < text.size() && text[i + 1u] == 'n')
                {
                    out.push_back('\n');
                    ++i;
                }
                else
                {
                    out.push_back(text[i]);
                }
            }
            return out;
        }
    }

    uint32_t keyHash(std::string_view name)
    {
        uint32_t h = 0;
        for (char ch : name)
        {
            uint8_t c = static_cast<uint8_t>(ch);
            if (c >= 'a' && c <= 'z')
                c = static_cast<uint8_t>(c - 'a' + 'A');
            h = (h << 4) + c;
            const uint32_t g = h & 0xF0000000u;
            if (g)
                h ^= g >> 24;
            h &= ~g;
        }
        return h;
    }

    const std::string *StringTable::find(uint32_t key) const
    {
        for (const auto &[k, text] : entries)
            if (k == key)
                return &text;
        return nullptr;
    }

    bool readStringTable(const std::vector<uint8_t> &file, StringTable &out)
    {
        out.entries.clear();
        if (file.size() < 8u)
            return false;
        const uint32_t count = readU32(file, 4);
        if (count > 0x10000u || 8u + static_cast<size_t>(count) * 8u > file.size())
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t key = readU32(file, 8u + i * 8u);
            const uint32_t offset = readU32(file, 12u + i * 8u);
            if (offset >= file.size())
                return false;
            const void *end = std::memchr(file.data() + offset, 0, file.size() - offset);
            if (!end)
                return false;
            const size_t length = static_cast<const uint8_t *>(end) - (file.data() + offset);
            out.entries.emplace_back(key, std::string(reinterpret_cast<const char *>(file.data() + offset), length));
        }
        return true;
    }

    std::vector<uint8_t> writeStringTable(const StringTable &table)
    {
        const size_t count = table.entries.size();
        std::vector<uint8_t> out(8u + count * 8u, 0u);
        putU32(out, 4, static_cast<uint32_t>(count));
        for (size_t i = 0; i < count; ++i)
        {
            putU32(out, 8u + i * 8u, table.entries[i].first);
            putU32(out, 12u + i * 8u, static_cast<uint32_t>(out.size()));
            const std::string &text = table.entries[i].second;
            out.insert(out.end(), text.begin(), text.end());
            out.push_back(0u);
        }
        putU32(out, 0, static_cast<uint32_t>(out.size()));
        return out;
    }

    bool readLevelText(const std::vector<uint8_t> &file, LevelText &out)
    {
        out.strings.clear();
        out.body.clear();
        if (file.size() < 0x14u)
            return false;
        const uint32_t size = readU32(file, 0x10);
        if (0x14u + static_cast<size_t>(size) > file.size())
            return false;
        uint32_t start = 0;
        for (uint32_t i = 0; i < size; ++i)
        {
            if (file[0x14u + i] != 0u)
                continue;
            out.strings.emplace(start, std::string(reinterpret_cast<const char *>(file.data() + 0x14u + start), i - start));
            start = i + 1u;
        }
        out.body.assign(file.begin() + 0x14 + size, file.end());
        return true;
    }

    std::vector<std::pair<std::string, std::string>> pairLevelText(const LevelText &a, const LevelText &b)
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        if (a.body.size() != b.body.size())
            return pairs;
        std::map<uint32_t, std::unordered_map<uint32_t, int>> votes;
        for (size_t k = 0; k + 4u <= a.body.size(); k += 4u)
        {
            const uint32_t x = readU32(a.body, k);
            const uint32_t y = readU32(b.body, k);
            const auto sa = a.strings.find(x);
            if (sa == a.strings.end() || sa->second.empty() || !b.strings.count(y))
                continue;
            ++votes[x][y];
        }
        for (const auto &[x, counts] : votes)
        {
            uint32_t best = 0;
            int most = 0;
            for (const auto &[y, n] : counts)
                if (n > most || (n == most && y < best))
                {
                    best = y;
                    most = n;
                }
            const std::string &other = b.strings.at(best);
            if (!other.empty())
                pairs.emplace_back(a.strings.at(x), other);
        }
        return pairs;
    }

    void Translation::add(const std::string &english, const std::string &other)
    {
        // Single characters are key names and button letters, never words
        // to translate (the Xbox German table has a "SAUVEGARDER" under "a").
        if (english.size() < 2u || other.empty() || english == other)
            return;
        if (!m_exact.count(english))
        {
            m_exact.emplace(english, other);
            m_folded.emplace(asciiUpper(english), other);
            if (english[0] == '\'')
                m_endings.emplace_back(asciiUpper(english), other);
        }
        const std::string brokenEnglish = withLineBreaks(english);
        if (brokenEnglish != english && !m_exact.count(brokenEnglish))
        {
            const std::string brokenOther = withLineBreaks(other);
            m_exact.emplace(brokenEnglish, brokenOther);
            m_folded.emplace(asciiUpper(brokenEnglish), brokenOther);
        }
    }

    void Translation::set(const std::string &english, const std::string &other)
    {
        if (english.empty())
            return;
        m_exact[english] = other;
        m_folded[asciiUpper(english)] = other;
    }

    const std::string *Translation::find(const std::string &english) const
    {
        const auto it = m_exact.find(english);
        return it == m_exact.end() ? nullptr : &it->second;
    }

    std::string Translation::lookup(const std::string &core) const
    {
        if (const std::string *exact = find(core))
            return *exact;
        const auto it = m_folded.find(asciiUpper(core));
        if (it == m_folded.end())
            return {};
        return hasLower(core) ? it->second : asciiUpper(it->second);
    }

    std::string Translation::lookupEnding(const std::string &core) const
    {
        const std::string upper = asciiUpper(core);
        for (const auto &[ending, other] : m_endings)
            if (upper.size() > ending.size() && upper.compare(upper.size() - ending.size(), ending.size(), ending) == 0)
            {
                const std::string rest = core.substr(0, core.size() - ending.size());
                return rest + (hasLower(core.substr(rest.size())) ? other : asciiUpper(other));
            }
        return {};
    }

    std::string Translation::translateRun(const std::string &text) const
    {
        // Spaces around the text kept as they are.
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        const std::string core = text.substr(first, last - first + 1u);
        std::string found = lookup(core);
        // "Jump:", "Attribute Total: 6": the words before a colon.
        if (found.empty())
        {
            const size_t colon = core.find(':');
            if (colon != std::string::npos && colon > 0u)
            {
                std::string head = core.substr(0, colon);
                while (!head.empty() && head.back() == ' ')
                    head.pop_back();
                std::string words = lookup(head);
                // "JAREN's Stats:"
                if (words.empty())
                    words = lookupEnding(head);
                if (!words.empty())
                {
                    // The words after the colon too when they have a
                    // translation ("Pad: Jump"); else as they are (a
                    // value, a name).
                    std::string rest = core.substr(head.size());
                    const size_t after = rest.find(':') + 1u;
                    if (std::string tail = translateRun(rest.substr(after)); !tail.empty())
                        rest = rest.substr(0, after) + tail;
                    found = words + rest;
                }
            }
        }
        // "(all hidden Challenges)", "< Off >": the words in brackets.
        if (found.empty() && core.size() > 2u &&
            ((core.front() == '(' && core.back() == ')') || (core.front() == '<' && core.back() == '>')))
            if (std::string words = translateRun(core.substr(1, core.size() - 2u)); !words.empty())
                found = core.front() + words + core.back();
        // "8 pts", "0% Complete": the words after a number.
        if (found.empty() && std::isdigit(static_cast<unsigned char>(core[0])))
        {
            const size_t space = core.find(' ');
            if (space != std::string::npos && space + 1u < core.size())
                if (std::string words = lookup(core.substr(space + 1u)); !words.empty())
                    found = core.substr(0, space + 1u) + words;
        }
        // "Continue for 0 pts": known words, then a number and more.
        if (found.empty())
        {
            for (size_t space = core.rfind(' '); space != std::string::npos && space > 0u;
                 space = core.rfind(' ', space - 1u))
            {
                if (space + 1u >= core.size() || !std::isdigit(static_cast<unsigned char>(core[space + 1u])))
                    continue;
                if (std::string words = lookup(core.substr(0, space)); !words.empty())
                {
                    std::string rest = core.substr(space);
                    if (std::string tail = translateRun(rest); !tail.empty())
                        rest = tail;
                    found = words + rest;
                    break;
                }
            }
        }
        // "L1 R1  Section": the words after a wide gap (button names before).
        if (found.empty())
        {
            const size_t gap = core.rfind("  ");
            if (gap != std::string::npos && gap > 0u)
            {
                const size_t start = core.find_first_not_of(' ', gap);
                if (start != std::string::npos)
                    if (std::string words = lookup(core.substr(start)); !words.empty())
                        found = core.substr(0, start) + words;
            }
        }
        // "JAREN'S STATS": a name with a translated ending ("'s Stats").
        if (found.empty())
            found = lookupEnding(core);
        // "Start at the title screen. Applies at next start.": sentence by
        // sentence, when each one has a translation.
        if (found.empty() && core.find(". ") != std::string::npos)
        {
            std::string joined;
            size_t start = 0;
            while (start < core.size())
            {
                size_t end = core.find(". ", start);
                end = end == std::string::npos ? core.size() : end + 1u;
                const std::string sentence = core.substr(start, end - start);
                const std::string words = lookup(sentence);
                if (words.empty())
                {
                    joined.clear();
                    break;
                }
                joined += (joined.empty() ? "" : " ") + words;
                start = core.find_first_not_of(' ', end);
                if (start == std::string::npos)
                    break;
            }
            found = std::move(joined);
        }
        if (found.empty())
            return {};
        return text.substr(0, first) + found + text.substr(last + 1u);
    }

    std::string Translation::translate(const std::string &english) const
    {
        if (english.empty())
            return {};
        if (const std::string *exact = find(english))
            return *exact;
        const bool lines = english.find_first_of("\r\n") != std::string::npos;
        if (!lines)
            if (std::string run = translateRun(english); !run.empty())
                return run;
        if (!lines && english.find('#') == std::string::npos)
            return {};
        // Lines and the engine's inline markup (#RGB{r g b}, #SCALE{x y z}):
        // each piece of text between them on its own.
        // A longer translation takes its extra length from the padding
        // spaces that follow it on the line (columns of values), so the
        // line does not wrap.
        std::string out, piece;
        bool any = false;
        size_t longer = 0;
        auto flush = [&]()
        {
            if (piece.empty())
                return;
            size_t spaces = 0;
            while (spaces < piece.size() && piece[spaces] == ' ')
                ++spaces;
            if (longer && spaces > 1u)
            {
                const size_t drop = std::min(longer, spaces - 1u);
                piece.erase(0, drop);
                longer -= drop;
            }
            if (std::string words = translateRun(piece); !words.empty())
            {
                if (words.size() > piece.size())
                    longer += words.size() - piece.size();
                out += words;
                any = true;
            }
            else
            {
                out += piece;
            }
            piece.clear();
        };
        for (size_t i = 0; i < english.size();)
        {
            const char c = english[i];
            if (c == '\n' || c == '\r')
            {
                flush();
                longer = 0;
                out.push_back(c);
                ++i;
                continue;
            }
            if (c == '#')
            {
                size_t j = i + 1u;
                while (j < english.size() && std::isupper(static_cast<unsigned char>(english[j])))
                    ++j;
                const size_t close = j < english.size() && english[j] == '{' ? english.find('}', j) : std::string::npos;
                if (j > i + 1u && close != std::string::npos)
                {
                    flush();
                    out.append(english, i, close + 1u - i);
                    i = close + 1u;
                    continue;
                }
            }
            piece.push_back(c);
            ++i;
        }
        flush();
        return any ? out : std::string();
    }

    std::string latin1FromUtf8(std::string_view utf8)
    {
        std::string out;
        out.reserve(utf8.size());
        for (size_t i = 0; i < utf8.size();)
        {
            const uint8_t c = static_cast<uint8_t>(utf8[i]);
            if (c < 0x80u)
            {
                out.push_back(static_cast<char>(c));
                ++i;
                continue;
            }
            uint32_t code = 0;
            size_t length = 0;
            if ((c & 0xE0u) == 0xC0u)
            {
                code = c & 0x1Fu;
                length = 2;
            }
            else if ((c & 0xF0u) == 0xE0u)
            {
                code = c & 0x0Fu;
                length = 3;
            }
            else if ((c & 0xF8u) == 0xF0u)
            {
                code = c & 0x07u;
                length = 4;
            }
            else
            {
                out.push_back('?');
                ++i;
                continue;
            }
            if (i + length > utf8.size())
            {
                out.push_back('?');
                break;
            }
            for (size_t k = 1; k < length; ++k)
                code = (code << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3Fu);
            out.push_back(code <= 0xFFu ? static_cast<char>(code) : '?');
            i += length;
        }
        return out;
    }

    std::string asciiUpper(std::string_view text)
    {
        std::string out(text);
        for (char &c : out)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return out;
    }
}
