#include "collection/updater.h"

#include "collection/disc_check.h"
#include "collection/settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <utility>

namespace zx::collection
{
    namespace fs = std::filesystem;

    // ---- the release's files ----------------------------------------------------

    std::vector<PublishedFile> parseSha256Sums(const std::string &text)
    {
        std::vector<PublishedFile> files;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.size() < 66)
                continue;
            std::string sha = line.substr(0, 64);
            if (!std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                continue;
            // ("<sha>  <name>" for a text-mode sum, "<sha> *<name>" for a binary one)
            if (line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
                continue;
            std::string name = line.substr(66);
            if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name == "." ||
                name == "..")
                continue;
            std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            files.push_back({std::move(name), std::move(sha)});
        }
        return files;
    }

    namespace
    {
        // The numbers of an x.y.z ("" not one).
        bool versionParts(const std::string &version, int parts[3])
        {
            size_t at = 0;
            for (int i = 0; i < 3; ++i)
            {
                if (at >= version.size() || !std::isdigit(static_cast<unsigned char>(version[at])))
                    return false;
                int value = 0;
                while (at < version.size() && std::isdigit(static_cast<unsigned char>(version[at])))
                    value = value * 10 + (version[at++] - '0');
                parts[i] = value;
                if (i < 2)
                {
                    if (at >= version.size() || version[at] != '.')
                        return false;
                    ++at;
                }
            }
            return at == version.size();
        }
    }

    std::string versionInName(const std::string &name)
    {
        // (the last "-x.y.z-" or "-x.y.z." in the name: "Z-Axis-Collection-1.3.1-mac.zip")
        std::string found;
        for (size_t dash = name.find('-'); dash != std::string::npos; dash = name.find('-', dash + 1))
        {
            size_t end = dash + 1;
            while (end < name.size() && (std::isdigit(static_cast<unsigned char>(name[end])) || name[end] == '.'))
                ++end;
            std::string candidate = name.substr(dash + 1, end - dash - 1);
            while (!candidate.empty() && candidate.back() == '.')
                candidate.pop_back();
            int parts[3];
            if (versionParts(candidate, parts))
                found = candidate;
        }
        return found;
    }

    int compareVersions(const std::string &a, const std::string &b)
    {
        int pa[3], pb[3];
        const bool va = versionParts(a, pa), vb = versionParts(b, pb);
        if (!va || !vb)
            return va == vb ? 0 : (va ? 1 : -1);
        for (int i = 0; i < 3; ++i)
            if (pa[i] != pb[i])
                return pa[i] < pb[i] ? -1 : 1;
        return 0;
    }

    std::string fileSha256(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return {};
        zx::app::Sha256 hash;
        std::vector<char> buffer(1 << 20);
        while (in)
        {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (const std::streamsize got = in.gcount(); got > 0)
                hash.update(reinterpret_cast<const uint8_t *>(buffer.data()), static_cast<size_t>(got));
        }
        if (in.bad())
            return {};
        return hash.finishHex();
    }

    // ---- a folder's files replaced, and put back --------------------------------

    namespace
    {
        bool sameFile(const fs::path &a, const fs::path &b)
        {
            std::error_code ec;
            const auto sizeA = fs::file_size(a, ec);
            if (ec)
                return false;
            const auto sizeB = fs::file_size(b, ec);
            if (ec || sizeA != sizeB)
                return false;
            const std::string shaA = fileSha256(a);
            return !shaA.empty() && shaA == fileSha256(b);
        }

        std::string relativePath(const fs::path &path, const fs::path &base)
        {
            return path.lexically_relative(base).generic_string();
        }
    }

    bool replaceFiles(const fs::path &from, const fs::path &into, const std::vector<std::string> &removed,
                      const fs::path &undo, FolderChange *change, std::string *error)
    {
        FolderChange counts;
        std::vector<std::pair<fs::path, fs::path>> moves; // done, in order: undone backwards on a failure
        std::vector<std::string> added;
        std::error_code ec;
        auto fail = [&](const std::string &why)
        {
            for (auto it = moves.rbegin(); it != moves.rend(); ++it)
            {
                std::error_code back;
                fs::rename(it->second, it->first, back);
            }
            if (error)
                *error = why;
            return false;
        };
        auto move = [&](const fs::path &source, const fs::path &target)
        {
            std::error_code made;
            fs::create_directories(target.parent_path(), made);
            std::error_code moved;
            fs::rename(source, target, moved);
            if (moved)
                return false;
            moves.emplace_back(source, target);
            return true;
        };
        fs::create_directories(undo / "files", ec);
        if (ec)
            return fail("cannot make " + undo.string() + ": " + ec.message());
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file())
                files.push_back(it->path());
        if (ec)
            return fail("cannot read " + from.string() + ": " + ec.message());
        std::sort(files.begin(), files.end());
        for (const fs::path &file : files)
        {
            const std::string rel = relativePath(file, from);
            const fs::path target = into / fs::path(rel);
            if (fs::exists(target, ec))
            {
                if (sameFile(file, target))
                {
                    ++counts.unchanged;
                    continue;
                }
                // (moved aside, not written over: a running executable and its libraries can be moved, not changed)
                if (!move(target, undo / "files" / fs::path(rel)))
                    return fail("cannot move " + target.string() + " aside");
            }
            else
                added.push_back(rel);
            if (!move(file, target))
                return fail("cannot put " + target.string() + " in place");
            ++counts.written;
        }
        for (const std::string &rel : removed)
        {
            const fs::path target = into / fs::path(rel);
            if (!fs::exists(target, ec))
                continue;
            if (!move(target, undo / "files" / fs::path(rel)))
                return fail("cannot move " + target.string() + " aside");
            ++counts.removed;
        }
        {
            std::ofstream list(undo / "added.txt", std::ios::binary | std::ios::trunc);
            for (const std::string &rel : added)
                list << rel << "\n";
            if (!list)
                return fail("cannot write " + (undo / "added.txt").string());
        }
        if (change)
            *change = counts;
        return true;
    }

    std::vector<std::string> addedFiles(const fs::path &undo)
    {
        std::vector<std::string> files;
        std::ifstream list(undo / "added.txt", std::ios::binary);
        std::string line;
        while (std::getline(list, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // (only paths below the folder: never one that climbs out of it)
            if (!line.empty() && fs::path(line).is_relative() && line.find("..") == std::string::npos)
                files.push_back(line);
        }
        return files;
    }

    // ---- the player's settings, kept for an older version --------------------------

    namespace
    {
        std::string trimmed(std::string_view text)
        {
            size_t begin = 0, end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return std::string(text.substr(begin, end - begin));
        }

        std::string lowered(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // A text's lines, split at each '\n' (a '\r' kept on its line): joined with '\n' they are the text again.
        std::vector<std::string> splitLines(const std::string &text)
        {
            std::vector<std::string> lines;
            size_t at = 0;
            for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', at))
            {
                lines.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            lines.push_back(text.substr(at));
            return lines;
        }

        std::string joinLines(const std::vector<std::string> &lines)
        {
            std::string text;
            for (size_t i = 0; i < lines.size(); ++i)
                text += (i ? "\n" : "") + lines[i];
            return text;
        }

        // What a settings.ini line is: a [section], a comment, a key = value line, or none of them (blank).
        struct IniLineKind
        {
            bool section = false, comment = false, key = false;
            std::string name; // the section's or the key's, lower case
        };

        IniLineKind lineKind(const std::string &line)
        {
            IniLineKind kind;
            const std::string content = trimmed(line);
            if (content.empty())
                return kind;
            if (content[0] == '#' || content[0] == ';')
                kind.comment = true;
            else if (content.front() == '[' && content.back() == ']')
            {
                kind.section = true;
                kind.name = lowered(trimmed(std::string_view(content).substr(1, content.size() - 2)));
            }
            else if (const size_t equals = content.find('='); equals != std::string::npos)
            {
                kind.key = true;
                kind.name = lowered(trimmed(std::string_view(content).substr(0, equals)));
            }
            return kind;
        }

        // One key moved between the files: its line and the comment lines right above it.
        struct MovedKey
        {
            std::string section, key;
            std::vector<std::string> lines;
        };

        std::string keyName(const MovedKey &moved)
        {
            return "[" + moved.section + "] " + moved.key;
        }

        // The side file's keys, by section in the file's order.
        using SideSections = std::vector<std::pair<std::string, std::vector<MovedKey>>>;

        SideSections readSide(const std::string &text)
        {
            SideSections sections;
            std::vector<std::string> comments;
            for (const std::string &line : splitLines(text))
            {
                const IniLineKind kind = lineKind(line);
                if (kind.comment)
                {
                    if (!sections.empty()) // (those before the first section are the file's own, written anew)
                        comments.push_back(line);
                    continue;
                }
                if (kind.section)
                {
                    sections.push_back({kind.name, {}});
                }
                else if (kind.key && !sections.empty())
                {
                    MovedKey moved{sections.back().first, kind.name, std::move(comments)};
                    moved.lines.push_back(line);
                    sections.back().second.push_back(std::move(moved));
                }
                comments.clear();
            }
            return sections;
        }

        void addToSide(SideSections &sections, MovedKey moved)
        {
            auto section = std::find_if(sections.begin(), sections.end(), [&](const auto &each) { return each.first == moved.section; });
            if (section == sections.end())
            {
                sections.push_back({moved.section, {}});
                section = sections.end() - 1;
            }
            auto &keys = section->second;
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const MovedKey &each) { return each.key == moved.key; }), keys.end());
            keys.push_back(std::move(moved));
        }

        std::string formatSide(const SideSections &sections)
        {
            std::string text;
            for (const auto &[name, keys] : sections)
            {
                if (keys.empty())
                    continue;
                text += "\n[" + name + "]\n";
                for (const MovedKey &moved : keys)
                    for (const std::string &line : moved.lines)
                        text += line + "\n";
            }
            if (text.empty())
                return text;
            return "# Settings of a newer version of the app, kept here while an older one runs (it would drop them\n"
                   "# from settings.ini): the version that reads them again puts them back into settings.ini at its\n"
                   "# start, as you left them. Leave this file as it is.\n" +
                   text;
        }
    }

    fs::path newerSettingsFile(const fs::path &settingsFile)
    {
        return settingsFile.parent_path() / (settingsFile.stem().string() + ".newer" + settingsFile.extension().string());
    }

    SettingsMove setAsideSettings(const std::string &settings, const std::string &newer, const SettingsKeyTest &aside)
    {
        SettingsMove move;
        SideSections side = readSide(newer);
        std::vector<std::string> kept;
        std::string section;
        for (const std::string &line : splitLines(settings))
        {
            const IniLineKind kind = lineKind(line);
            if (kind.section)
                section = kind.name;
            if (kind.key && aside(section, kind.name))
            {
                MovedKey moved{section, kind.name, {}};
                size_t first = kept.size();
                while (first > 0 && lineKind(kept[first - 1]).comment)
                    --first;
                moved.lines.assign(kept.begin() + static_cast<std::ptrdiff_t>(first), kept.end());
                kept.resize(first);
                moved.lines.push_back(line);
                move.moved.push_back(keyName(moved));
                addToSide(side, std::move(moved));
                continue;
            }
            kept.push_back(line);
        }
        if (move.moved.empty())
        {
            move.settings = settings;
            move.newer = newer;
            return move;
        }
        move.settings = joinLines(kept);
        move.newer = formatSide(side);
        return move;
    }

    SettingsMove putBackSettings(const std::string &settings, const std::string &newer, const SettingsKeyTest &known)
    {
        SettingsMove move;
        std::vector<std::string> lines = splitLines(settings);
        auto has = [&](const std::string &wanted, const std::string &key)
        {
            std::string section;
            for (const std::string &line : lines)
            {
                const IniLineKind kind = lineKind(line);
                if (kind.section)
                    section = kind.name;
                else if (kind.key && section == wanted && kind.name == key)
                    return true;
            }
            return false;
        };
        // (at the end of the section: after its last line that is not blank)
        auto putBack = [&](const MovedKey &moved)
        {
            size_t header = lines.size();
            for (size_t i = 0; i < lines.size() && header == lines.size(); ++i)
                if (const IniLineKind kind = lineKind(lines[i]); kind.section && kind.name == moved.section)
                    header = i;
            if (header == lines.size())
            {
                std::vector<std::string> block{"", "[" + moved.section + "]"};
                block.insert(block.end(), moved.lines.begin(), moved.lines.end());
                if (lines.back().empty()) // (the text ends with its line end)
                    lines.insert(lines.end() - 1, block.begin(), block.end());
                else
                {
                    lines.insert(lines.end(), block.begin(), block.end());
                    lines.push_back("");
                }
                return;
            }
            size_t end = header + 1;
            while (end < lines.size() && !lineKind(lines[end]).section)
                ++end;
            while (end > header + 1 && trimmed(lines[end - 1]).empty())
                --end;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(end), moved.lines.begin(), moved.lines.end());
        };
        SideSections left;
        for (const auto &[name, keys] : readSide(newer))
            for (const MovedKey &moved : keys)
            {
                if (!known(moved.section, moved.key))
                {
                    addToSide(left, moved);
                    continue;
                }
                if (!has(moved.section, moved.key))
                    putBack(moved);
                move.moved.push_back(keyName(moved));
            }
        if (move.moved.empty())
        {
            move.settings = settings;
            move.newer = newer;
            return move;
        }
        move.settings = joinLines(lines);
        move.newer = formatSide(left);
        return move;
    }

    namespace
    {
        std::string joinedNames(const std::vector<std::string> &names)
        {
            std::string text;
            for (const std::string &name : names)
                text += (text.empty() ? "" : ", ") + name;
            return text;
        }
    }

    void setAsideNewerSettings(const fs::path &settingsFile, const std::string &version, std::vector<std::string> *log)
    {
        std::string settings, newer;
        if (!zx::app::readTextFile(settingsFile, settings))
            return;
        const fs::path side = newerSettingsFile(settingsFile);
        zx::app::readTextFile(side, newer);
        const SettingsMove move = setAsideSettings(settings, newer, [&version](const std::string &section, const std::string &key)
        {
            const std::string since = zx::app::rootSettingSince(section, key);
            return !since.empty() && compareVersions(since, version) > 0;
        });
        if (move.moved.empty())
            return;
        // (the side file first: a key is never in neither file; in both, putBackNewerSettings keeps settings.ini's)
        bool ok = zx::app::writeTextFile(side, move.newer) && zx::app::writeTextFile(settingsFile, move.settings);
        if (log)
            log->push_back(joinedNames(move.moved) + " (newer than " + version + ") " + settingsFile.string() + " -> " +
                           side.string() + (ok ? "" : " failed"));
    }

    void putBackNewerSettings(const fs::path &settingsFile, std::vector<std::string> *log)
    {
        std::string settings, newer;
        const fs::path side = newerSettingsFile(settingsFile);
        if (!zx::app::readTextFile(side, newer) || !zx::app::readTextFile(settingsFile, settings))
            return;
        const SettingsMove move = putBackSettings(settings, newer, [](const std::string &section, const std::string &key)
        { return zx::app::rootSettingKnown(section, key); });
        if (move.moved.empty())
            return;
        bool ok = zx::app::writeTextFile(settingsFile, move.settings);
        if (ok)
        {
            std::error_code ec;
            if (move.newer.empty())
                ok = fs::remove(side, ec) && !ec;
            else
                ok = zx::app::writeTextFile(side, move.newer);
        }
        if (log)
            log->push_back(joinedNames(move.moved) + " " + side.string() + " -> " + settingsFile.string() + (ok ? "" : " failed"));
    }

    // ---- the updater ------------------------------------------------------------

    namespace
    {
        std::string readLine(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            std::string line;
            std::getline(in, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            return line;
        }

        bool writeLine(const fs::path &file, const std::string &line)
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << line << "\n";
            return static_cast<bool>(out);
        }

        bool isUrl(const std::string &source)
        {
            return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
        }

        // A file:// URL's path, else the source as it is (a folder).
        fs::path sourceFolder(const std::string &source)
        {
            if (source.rfind("file://", 0) != 0)
                return fs::path(source);
            std::string path;
            const std::string rest = source.substr(7);
            for (size_t i = 0; i < rest.size(); ++i)
                if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(rest[i + 2])))
                {
                    path += static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else
                    path += rest[i];
#if defined(_WIN32)
            if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
                path.erase(0, 1);
#endif
            return fs::path(path);
        }

        // The single .app (Bundle) or top folder (Folder) an unpacked zip holds.
        fs::path unpackedTop(const fs::path &folder, bool bundle)
        {
            std::vector<fs::path> entries;
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().filename().string().rfind("__MACOSX", 0) != 0)
                    entries.push_back(it->path());
            if (bundle)
            {
                fs::path app;
                for (const fs::path &entry : entries)
                    if (entry.extension() == ".app" && fs::is_directory(entry, ec))
                    {
                        if (!app.empty())
                            return {};
                        app = entry;
                    }
                return app;
            }
            if (entries.size() == 1 && fs::is_directory(entries.front(), ec))
                return entries.front();
            return folder;
        }
    }

    const char *Updater::defaultSource()
    {
        return "https://github.com/catdevgame/z-axis/releases/latest/download";
    }

    Updater::Updater(UpdatePlatform platform, std::string appVersion, std::string source)
        : m_platform(std::move(platform)), m_appVersion(std::move(appVersion)),
          m_source(source.empty() ? std::string(defaultSource()) : std::move(source))
    {
        if (m_platform.install == UpdatePlatform::Install::None || m_platform.updateFolder.empty())
        {
            m_stage = Stage::Unsupported;
            return;
        }
        std::error_code ec;
        // (a download or unpacking cut off by a quit; an APK the installer took or refused)
        fs::remove_all(m_platform.updateFolder / "next", ec);
        fs::remove_all(m_platform.updateFolder / "previous.new", ec);
        if (m_platform.install == UpdatePlatform::Install::Package)
            fs::remove_all(m_platform.updateFolder / "download", ec);
    }

    Updater::~Updater()
    {
        if (m_worker.joinable())
            m_worker.join();
    }

    Updater::Stage Updater::stage() const
    {
        std::lock_guard lock(m_mutex);
        return m_stage;
    }

    std::string Updater::summary() const
    {
        std::lock_guard lock(m_mutex);
        switch (m_stage)
        {
        case Stage::Idle:
            return m_appVersion;
        case Stage::Checking:
            return "Checking...";
        case Stage::Current:
            return "Up to date";
        case Stage::Offered:
            return m_offeredVersion + (compareVersions(m_offeredVersion, m_appVersion) > 0 ? ": Enter to update" : ": Enter to go back");
        case Stage::Restart:
            return "Restarting...";
        case Stage::Handed:
            return "Installing";
        case Stage::Unsupported:
            return "Not on this platform";
        case Stage::Installing:
        case Stage::Failed:
            return m_message;
        }
        return {};
    }

    bool Updater::rollsBack() const
    {
        return m_platform.install == UpdatePlatform::Install::Bundle || m_platform.install == UpdatePlatform::Install::Folder;
    }

    std::string Updater::previousVersion() const
    {
        if (!rollsBack())
            return {};
        std::error_code ec;
        if (!fs::is_directory(previousFolder(), ec))
            return {};
        return readLine(previousFolder() / "version.txt");
    }

    std::string Updater::takeNotice()
    {
        std::lock_guard lock(m_mutex);
        return std::exchange(m_notice, std::string());
    }

    bool Updater::restartWanted() const
    {
        return stage() == Stage::Restart;
    }

    void Updater::set(Stage stage, const std::string &message, const std::string &notice)
    {
        {
            std::lock_guard lock(m_mutex);
            m_stage = stage;
            m_message = message;
            if (!notice.empty())
                m_notice = notice;
        }
        if (!notice.empty())
            std::cout << "[updater] " << notice << std::endl;
    }

    void Updater::run(Stage stage, const std::string &message, std::function<void()> work)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_stage == Stage::Checking || m_stage == Stage::Installing || m_stage == Stage::Restart ||
                m_stage == Stage::Handed || m_stage == Stage::Unsupported)
                return;
            // (set before the thread starts: a second Enter meanwhile starts nothing)
            m_stage = stage;
            m_message = message;
            m_notice = message; // (the menu redraws for it)
        }
        if (m_worker.joinable())
            m_worker.join();
        m_worker = std::thread(std::move(work));
    }

    void Updater::activate()
    {
        std::optional<PublishedFile> offered;
        std::string version;
        {
            std::lock_guard lock(m_mutex);
            if (m_stage == Stage::Offered)
            {
                offered = m_offered;
                version = m_offeredVersion;
                m_stage = Stage::Idle;
            }
        }
        if (offered)
            run(Stage::Installing, "Downloading " + version + "...", [this, file = *offered, version]() { install(file, version); });
        else
            run(Stage::Checking, "Checking for updates...", [this]() { check(); });
    }

    void Updater::rollBack()
    {
        if (!rollsBack())
        {
            set(stage(), summary(), "No Roll Back Update here: Android installs no older version over a newer one.");
            return;
        }
        if (previousVersion().empty())
        {
            set(stage(), summary(), "No earlier version is kept to roll back to.");
            return;
        }
        run(Stage::Installing, "Rolling back...", [this]() { putBack(); });
    }

    bool Updater::fetch(const std::string &name, const fs::path &to, std::string *error)
    {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        const fs::path part = fs::path(to.string() + ".part");
        fs::remove(part, ec);
        if (isUrl(m_source))
        {
            if (!m_platform.download)
            {
                if (error)
                    *error = "no downloads on this platform";
                return false;
            }
            std::string url = m_source;
            while (!url.empty() && url.back() == '/')
                url.pop_back();
            if (!m_platform.download(url + "/" + name, part, error))
            {
                fs::remove(part, ec);
                return false;
            }
        }
        else
        {
            const fs::path file = sourceFolder(m_source) / name;
            fs::copy_file(file, part, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                if (error)
                    *error = "cannot read " + file.string() + ": " + ec.message();
                fs::remove(part, ec);
                return false;
            }
        }
        fs::rename(part, to, ec);
        if (ec && error)
            *error = "cannot write " + to.string() + ": " + ec.message();
        return !ec;
    }

    void Updater::check()
    {
        std::cout << "[updater] checking " << m_source << " (this is " << m_appVersion << ")" << std::endl;
        const fs::path sums = m_platform.updateFolder / "download" / "SHA256SUMS";
        std::string error;
        if (!fetch("SHA256SUMS", sums, &error))
        {
            set(Stage::Failed, "No connection", "No update check: " + error);
            return;
        }
        std::ifstream in(sums, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const std::vector<PublishedFile> files = parseSha256Sums(text.str());
        auto mine = files.end();
        for (const std::string &suffix : m_platform.packageSuffixes)
        {
            mine = std::find_if(files.begin(), files.end(), [&suffix](const PublishedFile &file)
            { return file.name.size() > suffix.size() && file.name.compare(file.name.size() - suffix.size(), suffix.size(), suffix) == 0; });
            if (mine != files.end())
                break;
        }
        if (mine == files.end())
        {
            set(Stage::Failed, "Nothing for this platform",
                "The release has no package for this platform (*" +
                    (m_platform.packageSuffixes.empty() ? std::string() : m_platform.packageSuffixes.front()) + ").");
            return;
        }
        const std::string version = versionInName(mine->name);
        if (version.empty())
        {
            set(Stage::Failed, "No version", "The release's " + mine->name + " names no version.");
            return;
        }
        if (version == m_appVersion)
        {
            set(Stage::Current, "", "Up to date: " + m_appVersion + " is the release's version.");
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_offered = *mine;
            m_offeredVersion = version;
        }
        const bool newer = compareVersions(version, m_appVersion) > 0;
        if (!newer && !rollsBack())
        {
            set(Stage::Current, "", "The release is " + version + ", older than " + m_appVersion +
                                        ": Android installs no older version over a newer one, so this one stays.");
            return;
        }
        std::cout << "[updater] offered: " << mine->name << " (" << version << ", " << (newer ? "newer" : "older") << ")" << std::endl;
        set(Stage::Offered, "",
            newer ? version + " is out: Enter again to update; the app restarts."
                  : "The release is " + version + ", older than " + m_appVersion + ": Enter again to go back to it.");
    }

    void Updater::install(const PublishedFile &file, const std::string &version)
    {
        const fs::path package = m_platform.updateFolder / "download" / file.name;
        std::string error;
        std::cout << "[updater] downloading " << file.name << std::endl;
        if (!fetch(file.name, package, &error))
        {
            set(Stage::Failed, "Download failed", "The download of " + version + " failed: " + error + ". Nothing changed.");
            return;
        }
        const std::string sha = fileSha256(package);
        std::error_code ec;
        if (sha != file.sha256)
        {
            fs::remove(package, ec);
            set(Stage::Failed, "Download damaged",
                "The download of " + version + " is not the release's file (its SHA-256 " + sha + ", not " + file.sha256 + "). Nothing changed.");
            return;
        }
        std::cout << "[updater] " << file.name << ": SHA-256 " << sha << " as SHA256SUMS says" << std::endl;
        set(Stage::Installing, "Installing " + version + "...", "");
        const bool older = compareVersions(version, m_appVersion) < 0;
        if (!putInPlace(package, version, older, &error))
        {
            set(Stage::Failed, "Install failed", version + " was not installed: " + error + ". Nothing changed.");
            if (m_platform.install != UpdatePlatform::Install::Package)
                fs::remove(package, ec);
            return;
        }
        if (m_platform.install != UpdatePlatform::Install::Package)
        {
            fs::remove(package, ec);
            {
                std::lock_guard lock(m_mutex);
                m_restartVersion = version;
            }
            set(Stage::Restart, "", version + " is in place (" + m_appVersion +
                                        (previousVersion() == m_appVersion ? " kept for Roll Back Update" : " not kept: no Roll Back Update") +
                                        "): restarting.");
        }
    }

    bool Updater::putInPlace(const fs::path &package, const std::string &version, bool older, std::string *error)
    {
        const fs::path next = m_platform.updateFolder / "next";
        const fs::path keep = m_platform.updateFolder / "previous.new";
        const fs::path previous = previousFolder();
        // (the version kept before this one: moved out of the way by a rename before anything changes, since a file of
        // it still in use (Windows) can be renamed but not removed; put back when the update fails, removed once the
        // update is in place)
        const fs::path retired = m_platform.updateFolder / "previous.old";
        std::error_code ec;
        fs::remove_all(next, ec);
        fs::remove_all(keep, ec);
        auto fail = [&](const std::string &why)
        {
            std::error_code cleanup;
            fs::remove_all(next, cleanup);
            fs::remove_all(keep, cleanup);
            if (!fs::exists(previous, cleanup) && fs::exists(retired, cleanup))
                fs::rename(retired, previous, cleanup);
            if (error)
                *error = why;
            return false;
        };
        auto retire = [&]()
        {
            std::error_code done;
            fs::remove_all(retired, done); // (an earlier update's, when a file of it was in use then)
            if (!fs::exists(previous, done))
                return true;
            fs::rename(previous, retired, done);
            return !done;
        };
        auto versionOf = [&](const fs::path &what)
        {
            return m_platform.packageVersion ? m_platform.packageVersion(what) : std::string();
        };
        // (this version kept for Roll Back Update, in place of the one before; once the update is in place a failure
        // here changes nothing of it: the row then says no version is kept)
        auto keepThisVersion = [&]()
        {
            std::error_code done;
            if (!writeLine(keep / "version.txt", m_appVersion))
                return;
            fs::rename(keep, previous, done);
            if (done)
            {
                std::cout << "[updater] " << m_appVersion << " not kept in " << previous.string() << ": " << done.message() << std::endl;
                return;
            }
            fs::remove_all(retired, done); // (a file of it still in use: removed at the next update)
        };
        switch (m_platform.install)
        {
        case UpdatePlatform::Install::Bundle:
        {
            if (!m_platform.unpack || !m_platform.unpack(package, next, error))
                return fail(error && !error->empty() ? *error : "cannot unpack " + package.filename().string());
            const fs::path app = unpackedTop(next, true);
            if (app.empty())
                return fail(package.filename().string() + " holds no single .app");
            if (const std::string says = versionOf(app); !says.empty() && says != version)
                return fail(package.filename().string() + "'s app says it is " + says + ", not " + version);
            if (!retire())
                return fail("cannot move " + previous.string() + " aside (a file of it in use?)");
            fs::create_directories(keep, ec);
            const fs::path aside = keep / m_platform.installed.filename();
            fs::rename(m_platform.installed, aside, ec);
            if (ec)
                return fail("cannot move " + m_platform.installed.string() + " aside: " + ec.message());
            fs::rename(app, m_platform.installed, ec);
            if (ec)
            {
                std::error_code back;
                fs::rename(aside, m_platform.installed, back);
                return fail("cannot move the new app to " + m_platform.installed.string() + ": " + ec.message());
            }
            fs::remove_all(next, ec);
            std::cout << "[updater] " << m_platform.installed.string() << ": " << version << " (" << m_appVersion << " kept in "
                      << previous.string() << ")" << std::endl;
            keepThisVersion();
            return true;
        }
        case UpdatePlatform::Install::Folder:
        {
            if (!m_platform.unpack || !m_platform.unpack(package, next, error))
                return fail(error && !error->empty() ? *error : "cannot unpack " + package.filename().string());
            const fs::path top = unpackedTop(next, false);
            if (const std::string says = versionOf(top); !says.empty() && says != version)
                return fail(package.filename().string() + "'s app says it is " + says + ", not " + version);
            if (!retire())
                return fail("cannot move " + previous.string() + " aside (a file of it in use?)");
            FolderChange change;
            if (!replaceFiles(top, m_platform.installed, {}, keep, &change, error))
                return fail(error ? *error : "cannot replace the app's files");
            fs::remove_all(next, ec);
            std::cout << "[updater] " << m_platform.installed.string() << ": " << version << ", " << change.written << " files written, "
                      << change.unchanged << " the same (" << m_appVersion << "'s kept in " << previous.string() << ")" << std::endl;
            keepThisVersion();
            return true;
        }
        case UpdatePlatform::Install::Package:
        {
            // (an update only: the installer replaces an app with an older version nowhere a player's app can rely
            // on, so check() offers none and nothing is kept for a roll back)
            if (older)
                return fail("Android installs no older version over a newer one");
            if (const std::string says = versionOf(package); !says.empty() && says != version)
                return fail(package.filename().string() + " says it is " + says + ", not " + version);
            auto done = [this, version, package](std::string why)
            {
                if (why.empty())
                {
                    set(Stage::Handed, "", "The system installs " + version + ": the app closes; open it again.");
                    return;
                }
                std::error_code cleanup;
                fs::remove(package, cleanup);
                set(Stage::Failed, "Not installed", version + " was not installed: " + why + ". Nothing changed.");
            };
            // (handed over before the call: the installer's answer may come at once, on its own thread)
            set(Stage::Handed, "", "");
            std::string handError;
            if (!m_platform.installPackage || !m_platform.installPackage(package, done, &handError))
                done(handError.empty() ? "no package installer" : handError);
            return true; // (done says how it went)
        }
        case UpdatePlatform::Install::None:
            break;
        }
        return fail("no update on this platform");
    }

    void Updater::putBack()
    {
        const std::string version = previousVersion();
        const fs::path previous = previousFolder();
        const fs::path keep = m_platform.updateFolder / "previous.new";
        // (the version rolled back to, moved out of the way by a rename first (as in putInPlace) and put back from
        // there; it goes back in place when the roll back fails)
        const fs::path retired = m_platform.updateFolder / "previous.old";
        std::error_code ec;
        fs::remove_all(keep, ec);
        set(Stage::Installing, "Rolling back to " + version + "...", "");
        std::cout << "[updater] rolling back from " << m_appVersion << " to " << version << std::endl;
        auto failed = [&](const std::string &why)
        {
            std::error_code cleanup;
            fs::remove_all(keep, cleanup);
            if (!fs::exists(previous, cleanup) && fs::exists(retired, cleanup))
                fs::rename(retired, previous, cleanup);
            set(Stage::Failed, "Roll back failed", "Not rolled back to " + version + ": " + why + ". Nothing changed.");
        };
        auto kept = [&]()
        {
            std::error_code done;
            bool keptHere = writeLine(keep / "version.txt", m_appVersion);
            if (keptHere)
            {
                fs::rename(keep, previous, done);
                keptHere = !done;
            }
            if (keptHere)
                fs::remove_all(retired, done); // (a file of it still in use: removed at the next update)
            {
                std::lock_guard lock(m_mutex);
                m_restartVersion = version;
            }
            set(Stage::Restart, "", version + " is back (" + m_appVersion +
                                        (keptHere ? " kept for Roll Back Update" : " not kept: no Roll Back Update") + "): restarting.");
        };
        fs::remove_all(retired, ec); // (an earlier update's, when a file of it was in use then)
        fs::rename(previous, retired, ec);
        if (ec)
            return failed("cannot move " + previous.string() + " aside: " + ec.message());
        switch (m_platform.install)
        {
        case UpdatePlatform::Install::Bundle:
        {
            const fs::path app = unpackedTop(retired, true);
            if (app.empty())
                return failed("no app in " + previous.string());
            fs::create_directories(keep, ec);
            const fs::path aside = keep / m_platform.installed.filename();
            fs::rename(m_platform.installed, aside, ec);
            if (ec)
                return failed("cannot move " + m_platform.installed.string() + " aside: " + ec.message());
            fs::rename(app, m_platform.installed, ec);
            if (ec)
            {
                std::error_code back;
                fs::rename(aside, m_platform.installed, back);
                return failed("cannot move " + app.string() + " back: " + ec.message());
            }
            return kept();
        }
        case UpdatePlatform::Install::Folder:
        {
            FolderChange change;
            std::string error;
            if (!replaceFiles(retired / "files", m_platform.installed, addedFiles(retired), keep, &change, &error))
                return failed(error);
            std::cout << "[updater] " << change.written << " files put back, " << change.removed << " of " << m_appVersion
                      << "'s own moved aside" << std::endl;
            return kept();
        }
        case UpdatePlatform::Install::Package: // (no roll back: rollBack says so)
        case UpdatePlatform::Install::None:
            break;
        }
        failed("no roll back on this platform");
    }

    std::string Updater::restartVersion() const
    {
        std::lock_guard lock(m_mutex);
        return m_stage == Stage::Restart ? m_restartVersion : std::string();
    }

    void Updater::relaunchAtQuit(const fs::path &settingsFile)
    {
        if (!restartWanted())
            return;
        // (an older version: the settings it does not read set aside first, whether it starts now or the player opens it)
        if (const std::string version = restartVersion(); !settingsFile.empty() && compareVersions(version, m_appVersion) < 0)
        {
            std::vector<std::string> log;
            setAsideNewerSettings(settingsFile, version, &log);
            for (const std::string &line : log)
                std::cout << "[updater] settings: " << line << std::endl;
        }
        if (!m_platform.relaunch)
            return;
        const long pid = m_platform.relaunch();
        if (pid >= 0)
            std::cout << "[updater] restarted: PID " << pid << std::endl;
        else
            std::cout << "[updater] could not start " << m_platform.installed.string() << " again: open it yourself" << std::endl;
    }
}
