// The discs in the places a player gives the setup (collection/disc_finder.h).
#include "collection/disc_finder.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace zx::app
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t kMaxSystemCnf = 64u * 1024u;

        bool hiddenName(const std::string &name) { return !name.empty() && name.front() == '.'; }

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        // A file or folder of a place, a path's or a document's.
        struct Entry
        {
            fs::path source; // a path, or a document's URI as a path
            std::string name;
            bool folder = false;
            bool document = false;
        };

        bool entryOf(const fs::path &place, Entry &out)
        {
            if (isDocument(place))
            {
                DocumentAccess::Entry document;
                if (!documentAccess()->stat(utf8Of(place), document))
                    return false;
                out = {place, document.name, document.folder, true};
                return true;
            }
            std::error_code ec;
            if (!fs::exists(place, ec))
                return false;
            out = {place, place.filename().string(), fs::is_directory(place, ec), false};
            if (out.name.empty())
                out.name = place.parent_path().filename().string();
            return true;
        }

        std::vector<Entry> childrenOf(const Entry &folder)
        {
            std::vector<Entry> out;
            if (folder.document)
            {
                for (const DocumentAccess::Entry &child : documentAccess()->list(utf8Of(folder.source)))
                    out.push_back({pathFromUtf8(child.uri), child.name, child.folder, true});
                return out;
            }
            std::error_code ec;
            for (const fs::directory_entry &child : fs::directory_iterator(folder.source, ec))
            {
                const bool isFolder = child.is_directory(ec);
                if (isFolder || child.is_regular_file(ec))
                    out.push_back({child.path(), child.path().filename().string(), isFolder, false});
            }
            return out;
        }

        // The search of one call: what it found, and the BINs the CUEs found read.
        class Search
        {
        public:
            Search(const std::vector<SetupGame> &games, const extras::ImageOpener &extrasOpener, const SearchProgressFn &progress)
                : m_games(games), m_extrasOpener(extrasOpener), m_progress(progress)
            {
            }

            void visit(const Entry &entry, int depth)
            {
                if (m_out.cancelled)
                    return;
                ++m_out.looked;
                if (m_progress && !m_progress({m_out.looked, entry.name}))
                {
                    m_out.cancelled = true;
                    return;
                }
                if (!entry.folder)
                {
                    lookAtFile(entry);
                    return;
                }
                std::vector<Entry> children = childrenOf(entry);
                const bool disc = std::any_of(children.begin(), children.end(), [](const Entry &child)
                                              { return !child.folder && lowerAscii(child.name) == "system.cnf"; });
                if (disc)
                {
                    lookAtPs2(entry);
                    return;
                }
                // CUE sheets first (each reads its BIN), then by name; a
                // folder's files before its folders. A CUE document alone
                // cannot read the BIN beside it: there its BIN stands for it.
                std::vector<std::pair<bool, Entry>> files;
                std::vector<Entry> folders;
                for (Entry &child : children)
                {
                    if (hiddenName(child.name))
                        continue;
                    if (child.folder)
                        folders.push_back(std::move(child));
                    else
                    {
                        const bool cue = isCueSheetSource(child.source);
                        if (!(cue && child.document))
                            files.emplace_back(cue, std::move(child));
                    }
                }
                // (by name as a person reads them: case aside)
                std::stable_sort(files.begin(), files.end(), [](const auto &a, const auto &b)
                                 { return a.first != b.first ? a.first : lowerAscii(a.second.name) < lowerAscii(b.second.name); });
                std::sort(folders.begin(), folders.end(), [](const Entry &a, const Entry &b) { return lowerAscii(a.name) < lowerAscii(b.name); });
                for (const auto &file : files)
                    visit(file.second, depth + 1);
                if (depth < kSearchDepth)
                    for (const Entry &folder : folders)
                        visit(folder, depth + 1);
            }

            DiscSearch take() { return std::move(m_out); }

        private:
            bool alreadyRead(const fs::path &source) const
            {
                return std::find(m_read.begin(), m_read.end(), source) != m_read.end();
            }

            // A PS2 disc when its SYSTEM.CNF reads; true then.
            bool lookAtPs2(const Entry &entry)
            {
                DiscSource disc;
                std::vector<uint8_t> cnf;
                if (!disc.open(entry.source, nullptr) || !disc.readFile("SYSTEM.CNF", cnf, kMaxSystemCnf))
                    return false;
                m_read.push_back(disc.readFrom());
                const SourceCheck check = checkSource(disc, m_games);
                FoundDisc found;
                found.kind = FoundDisc::Kind::Ps2;
                found.source = entry.source;
                found.name = entry.name;
                found.game = check.game;
                found.status = check.status;
                found.bytes = check.bytes;
                found.message = check.message;
                m_out.discs.push_back(std::move(found));
                return true;
            }

            void lookAtFile(const Entry &entry)
            {
                if (alreadyRead(entry.source) || lookAtPs2(entry) || !m_extrasOpener)
                    return;
                const extras::ExtrasSource checked = extras::checkExtrasSource(utf8Of(entry.source), "", m_extrasOpener);
                if (!checked.gameDisc)
                    return; // (no disc: any other file)
                FoundDisc found;
                found.kind = FoundDisc::Kind::Extras;
                found.source = entry.source;
                found.name = entry.name;
                found.extras = checked.disc;
                found.message = checked.message;
                m_out.discs.push_back(std::move(found));
            }

            const std::vector<SetupGame> &m_games;
            const extras::ImageOpener &m_extrasOpener;
            const SearchProgressFn &m_progress;
            DiscSearch m_out;
            std::vector<fs::path> m_read;
        };
    }

    const FoundDisc *DiscSearch::ps2Of(int game) const
    {
        for (const FoundDisc &disc : discs)
            if (disc.kind == FoundDisc::Kind::Ps2 && disc.game == game && game >= 0 && disc.status == SourceCheck::Status::Ok)
                return &disc;
        return nullptr;
    }

    const FoundDisc *DiscSearch::otherReleaseOf(int game) const
    {
        for (const FoundDisc &disc : discs)
            if (disc.kind == FoundDisc::Kind::Ps2 && disc.game == game && game >= 0 && disc.status != SourceCheck::Status::Ok)
                return &disc;
        return nullptr;
    }

    std::vector<const FoundDisc *> DiscSearch::extrasOf(const std::string &game) const
    {
        std::vector<const FoundDisc *> out;
        for (const FoundDisc &disc : discs)
            if (disc.kind == FoundDisc::Kind::Extras && disc.extras && game == disc.extras->game &&
                std::none_of(out.begin(), out.end(), [&](const FoundDisc *kept)
                             { return std::string(kept->extras->release) == disc.extras->release; }))
                out.push_back(&disc);
        return out;
    }

    void DiscSearch::add(DiscSearch more)
    {
        for (FoundDisc &disc : more.discs)
            if (std::none_of(discs.begin(), discs.end(), [&](const FoundDisc &kept) { return kept.source == disc.source; }))
                discs.push_back(std::move(disc));
        looked += more.looked;
        cancelled = more.cancelled;
    }

    DiscSearch findDiscs(const std::vector<fs::path> &places, const std::vector<SetupGame> &games,
                         const extras::ImageOpener &extrasOpener, const SearchProgressFn &progress)
    {
        Search search(games, extrasOpener, progress);
        for (const fs::path &place : places)
            if (Entry entry; entryOf(place, entry))
                search.visit(entry, 0);
        return search.take();
    }

    std::vector<fs::path> discSourcesIn(const fs::path &path)
    {
        const DiscSearch search = findDiscs({path}, {}, {});
        std::vector<fs::path> out;
        for (const FoundDisc &disc : search.discs)
            out.push_back(disc.source);
        return out.empty() ? std::vector<fs::path>{path} : out;
    }
}
