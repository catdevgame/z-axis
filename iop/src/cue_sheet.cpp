// A CUE sheet's data track (zx/disc/cue_sheet.h).
#include "zx/disc/cue_sheet.h"

#include <algorithm>
#include <cctype>

namespace zx::disc
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

        std::string_view trimmed(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);
            return text;
        }

        // Each line of the sheet in turn (a UTF-8 byte order mark dropped),
        // trimmed, empty ones skipped: false from `line` stops.
        template <typename Fn>
        void eachLine(std::string_view text, Fn &&line)
        {
            if (text.substr(0, 3) == "\xEF\xBB\xBF")
                text.remove_prefix(3);
            while (!text.empty())
            {
                const size_t end = text.find_first_of("\r\n");
                const std::string_view part = trimmed(text.substr(0, end));
                text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1u);
                if (!part.empty() && !line(part))
                    return;
            }
        }

        std::string_view firstWord(std::string_view line) { return line.substr(0, line.find_first_of(" \t")); }

        // A FILE line's name: in quotes, or up to the first space.
        std::string fileName(std::string_view line)
        {
            std::string_view name = trimmed(line.substr(firstWord(line).size()));
            if (!name.empty() && name.front() == '"')
            {
                const size_t close = name.find('"', 1);
                return std::string(close == std::string_view::npos ? name.substr(1) : name.substr(1, close - 1));
            }
            return std::string(firstWord(name));
        }
    }

    bool isCueSheet(std::string_view text)
    {
        if (text.find('\0') != std::string_view::npos)
            return false;
        static constexpr std::string_view kCommands[] = {"CATALOG", "CDTEXTFILE", "FILE",    "FLAGS", "INDEX",
                                                        "ISRC",    "PERFORMER",  "POSTGAP", "PREGAP", "REM",
                                                        "SONGWRITER", "TITLE",   "TRACK"};
        bool commands = true, file = false;
        eachLine(text, [&](std::string_view line)
        {
            const std::string command = upper(firstWord(line));
            if (std::find(std::begin(kCommands), std::end(kCommands), command) == std::end(kCommands))
            {
                commands = false;
                return false;
            }
            file = file || command == "FILE";
            return true;
        });
        return commands && file;
    }

    std::string cueDataTrackFile(std::string_view text)
    {
        std::string first, file, data;
        bool tracks = false;
        eachLine(text, [&](std::string_view line)
        {
            const std::string command = upper(firstWord(line));
            if (command == "FILE")
            {
                file = fileName(line);
                if (first.empty())
                    first = file;
            }
            else if (command == "TRACK")
            {
                tracks = true;
                // "TRACK 01 MODE2/2352": its type, after its number.
                const std::string_view rest = trimmed(line.substr(firstWord(line).size()));
                const std::string type = upper(trimmed(rest.substr(firstWord(rest).size())));
                if (type.rfind("MODE1", 0) == 0 || type.rfind("MODE2", 0) == 0)
                {
                    data = file;
                    return false;
                }
            }
            return true;
        });
        // (a sheet of FILE lines alone: its first file)
        return tracks ? data : first;
    }
}
