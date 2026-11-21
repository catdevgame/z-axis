// A game set up from the player's own disc (collection/game_setup.h).
#include "collection/game_setup.h"
#include "collection/disc_check.h"
#include "strings.h"

#include "disc/cue_sheet.h"
#include "disc/iso9660.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace zx::app
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t kChunk = 4u * 1024u * 1024u;
        constexpr size_t kMaxSystemCnf = 64u * 1024u;
        constexpr size_t kMaxElf = 64u * 1024u * 1024u;
        constexpr const char *kManifestHeader = "z-axis game files 1";

        bool readFileBytes(const fs::path &file, size_t limit, std::vector<uint8_t> &bytes)
        {
            std::error_code ec;
            const uintmax_t size = fs::file_size(file, ec);
            if (ec || size > limit)
                return false;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            bytes.resize(static_cast<size_t>(size));
            in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return in.gcount() == static_cast<std::streamsize>(bytes.size());
        }

        // What an image that is not ISO9660 is, for the player: the other
        // releases' discs (the extras), or nothing known.
        using ImageHead = std::array<uint8_t, 0x10020>;
        std::string otherImageKind(const ImageHead &head, size_t got);

        std::string otherImageKind(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return {};
            ImageHead head{};
            in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
            return otherImageKind(head, static_cast<size_t>(in.gcount()));
        }

        std::string otherImageKind(const ImageHead &head, size_t got)
        {
            if (got >= 4 && std::memcmp(head.data(), "RVZ\x01", 4) == 0)
                return "a GameCube disc";
            if (got >= 4 && std::memcmp(head.data(), "WIA\x01", 4) == 0)
                return "a GameCube disc";
            if (got >= 0x20 && head[0x1C] == 0xC2 && head[0x1D] == 0x33 && head[0x1E] == 0x9F && head[0x1F] == 0x3D)
                return "a GameCube disc";
            if (got >= 0x10014 && std::memcmp(head.data() + 0x10000, "MICROSOFT*XBOX*MEDIA", 20) == 0)
                return "an Xbox disc";
            return {};
        }

        bool hiddenName(const std::string &name) { return !name.empty() && name.front() == '.'; }

        // ---- documents (DocumentAccess) ----

        std::atomic<const DocumentAccess *> g_documents{nullptr};

        struct FileCloser
        {
            void operator()(std::FILE *file) const
            {
                if (file)
                    std::fclose(file);
            }
        };
        using OpenFile = std::unique_ptr<std::FILE, FileCloser>;

        // A document's bytes, at most `limit` (more: false).
        bool readDocument(const DocumentAccess &documents, const std::string &uri, size_t limit, std::vector<uint8_t> &bytes)
        {
            const OpenFile file(documents.open(uri));
            if (!file)
                return false;
            bytes.clear();
            std::vector<uint8_t> buffer(64u * 1024u);
            for (size_t got; (got = std::fread(buffer.data(), 1, buffer.size(), file.get())) != 0u;)
            {
                if (bytes.size() + got > limit)
                    return false;
                bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
            }
            return !std::ferror(file.get());
        }

        // A CUE sheet's text, told by what it says (not its name): false
        // for any other file.
        constexpr size_t kMaxCueBytes = 256u * 1024u;
        bool cueSheetText(std::vector<uint8_t> bytes, std::string *text)
        {
            std::string sheet(bytes.begin(), bytes.end());
            if (!zx::disc::isCueSheet(sheet))
                return false;
            if (text)
                *text = std::move(sheet);
            return true;
        }
        bool cueSheetFile(const fs::path &path, std::string *text = nullptr)
        {
            std::vector<uint8_t> bytes;
            return readFileBytes(path, kMaxCueBytes, bytes) && cueSheetText(std::move(bytes), text);
        }
        bool cueSheetDocument(const DocumentAccess &documents, const DocumentAccess::Entry &entry, std::string *text = nullptr)
        {
            std::vector<uint8_t> bytes;
            return !entry.folder && entry.size <= kMaxCueBytes && readDocument(documents, entry.uri, kMaxCueBytes, bytes) &&
                   cueSheetText(std::move(bytes), text);
        }

        std::string otherDocumentKind(const DocumentAccess &documents, const std::string &uri)
        {
            const OpenFile file(documents.open(uri));
            if (!file)
                return {};
            ImageHead head{};
            return otherImageKind(head, std::fread(head.data(), 1, head.size(), file.get()));
        }

        bool hasSystemCnf(const std::vector<DocumentAccess::Entry> &entries)
        {
            return std::any_of(entries.begin(), entries.end(), [](const DocumentAccess::Entry &entry)
                               { return !entry.folder && lower(entry.name) == "system.cnf"; });
        }

        // A folder document's files, every level, each named by its path in
        // the folder.
        void documentFiles(const DocumentAccess &documents, const std::string &folderUri, const std::string &dir, int depth,
                           std::vector<DocumentAccess::Entry> &out)
        {
            if (depth > 32)
                return;
            for (DocumentAccess::Entry &entry : documents.list(folderUri))
            {
                if (hiddenName(entry.name))
                    continue;
                const std::string path = dir.empty() ? entry.name : dir + "/" + entry.name;
                if (entry.folder)
                    documentFiles(documents, entry.uri, path, depth + 1, out);
                else
                {
                    entry.name = path;
                    out.push_back(std::move(entry));
                }
            }
        }

        // The documents of a folder that may be disc images (each file; which
        // one is, its bytes tell), CUE sheets first (each reads its BIN),
        // then by name.
        std::vector<DocumentAccess::Entry> imageDocuments(const DocumentAccess &documents,
                                                          const std::vector<DocumentAccess::Entry> &entries)
        {
            std::vector<std::pair<bool, DocumentAccess::Entry>> images;
            for (const DocumentAccess::Entry &entry : entries)
                if (!entry.folder && !hiddenName(entry.name))
                    images.emplace_back(cueSheetDocument(documents, entry), entry);
            std::sort(images.begin(), images.end(), [](const auto &a, const auto &b)
                      { return a.first != b.first ? a.first : a.second.name < b.second.name; });
            std::vector<DocumentAccess::Entry> out;
            for (auto &image : images)
                out.push_back(std::move(image.second));
            return out;
        }

        // The same for a folder's path.
        std::vector<fs::path> imageFilesIn(const fs::path &folder)
        {
            std::vector<std::pair<bool, fs::path>> images;
            std::error_code ec;
            for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec))
                if (entry.is_regular_file(ec) && !hiddenName(entry.path().filename().string()))
                    images.emplace_back(cueSheetFile(entry.path()), entry.path());
            std::sort(images.begin(), images.end(), [](const auto &a, const auto &b)
                      { return a.first != b.first ? a.first : a.second < b.second; });
            std::vector<fs::path> out;
            for (auto &image : images)
                out.push_back(std::move(image.second));
            return out;
        }

        // The player's words for an image that does not open.
        std::string notAnImage(const std::string &name, const std::string &other)
        {
            if (!other.empty())
                return name + " is " + other + ". Use the PS2 disc.";
            return name + " is not a disc image.";
        }

        // A folder's files (relative, '/'-separated), every level.
        std::vector<DiscSource::File> folderFiles(const fs::path &root)
        {
            std::vector<DiscSource::File> files;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (hiddenName(name))
                {
                    if (it->is_directory(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec))
                    continue;
                const std::u8string relative = fs::relative(it->path(), root, ec).generic_u8string();
                files.push_back({std::string(relative.begin(), relative.end()), static_cast<uint64_t>(it->file_size(ec))});
            }
            std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
            return files;
        }

        void imageFiles(const zx::disc::Iso9660Image &image, const std::string &dir, int depth, std::vector<DiscSource::File> &out)
        {
            if (depth > 32)
                return;
            for (const zx::disc::IsoEntry &entry : image.list(dir.empty() ? "/" : dir))
            {
                const std::string path = dir.empty() ? entry.name : dir + "/" + entry.name;
                if (entry.directory)
                    imageFiles(image, path, depth + 1, out);
                else
                    out.push_back({path, entry.size});
            }
        }

        // A folder's own path for a file name of the disc, whatever case
        // the folder has it in (a disc folder copied by hand).
        fs::path folderFile(const fs::path &root, const std::string &path)
        {
            fs::path direct = root / pathFromUtf8(path);
            std::error_code ec;
            if (fs::exists(direct, ec))
                return direct;
            fs::path at = root;
            std::stringstream parts(path);
            std::string part;
            while (std::getline(parts, part, '/'))
            {
                fs::path next;
                for (const fs::directory_entry &entry : fs::directory_iterator(at, ec))
                    if (lower(entry.path().filename().string()) == lower(part))
                    {
                        next = entry.path();
                        break;
                    }
                if (next.empty())
                    return direct;
                at = next;
            }
            return at;
        }

        std::string gameList(const std::vector<SetupGame> &games)
        {
            std::string out;
            for (size_t i = 0; i < games.size(); ++i)
                out += (i == 0 ? "" : i + 1 == games.size() ? " or " : ", ") + games[i].title;
            return out;
        }

        bool writeText(const fs::path &file, const std::string &text)
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
            out.close();
            return static_cast<bool>(out);
        }

        struct Manifest
        {
            std::string game, elf, sha256, source, kind;
            struct Entry
            {
                std::string path;
                uint64_t size = 0;
                std::string sha256;
            };
            std::vector<Entry> files;
            uint64_t bytes = 0;
        };

        // The manifest: a header line, "key value" lines, then one
        // "file <sha256> <size> <path>" line a file.
        std::string formatManifest(const Manifest &m)
        {
            std::ostringstream out;
            out << kManifestHeader << "\n"
                << "game " << m.game << "\n"
                << "elf " << m.elf << "\n"
                << "sha256 " << m.sha256 << "\n"
                << "source " << m.source << "\n"
                << "kind " << m.kind << "\n"
                << "files " << m.files.size() << "\n"
                << "bytes " << m.bytes << "\n";
            for (const Manifest::Entry &e : m.files)
                out << "file " << e.sha256 << " " << e.size << " " << e.path << "\n";
            return out.str();
        }

        bool readManifest(const fs::path &file, Manifest &m)
        {
            std::ifstream in(file, std::ios::binary);
            std::string line;
            if (!in || !std::getline(in, line) || trim(line) != kManifestHeader)
                return false;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t space = line.find(' ');
                if (space == std::string::npos)
                    continue;
                const std::string key = line.substr(0, space), value = line.substr(space + 1);
                if (key == "game")
                    m.game = value;
                else if (key == "elf")
                    m.elf = value;
                else if (key == "sha256")
                    m.sha256 = value;
                else if (key == "source")
                    m.source = value;
                else if (key == "kind")
                    m.kind = value;
                else if (key == "bytes")
                    m.bytes = std::strtoull(value.c_str(), nullptr, 10);
                else if (key == "file")
                {
                    std::istringstream fields(value);
                    Manifest::Entry e;
                    fields >> e.sha256 >> e.size;
                    std::getline(fields >> std::ws, e.path);
                    if (!e.path.empty())
                        m.files.push_back(std::move(e));
                }
            }
            return !m.files.empty();
        }

        // Stops a step when the player cancels: progress said no.
        bool report(const SetupProgressFn &progress, const SetupProgress &p) { return !progress || progress(p); }

        bool hashFile(const fs::path &file, std::string &hex, const std::function<bool(size_t)> &advanced)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            Sha256 hash;
            std::vector<uint8_t> buffer(kChunk);
            while (in)
            {
                in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                const size_t got = static_cast<size_t>(in.gcount());
                if (got == 0u)
                    break;
                hash.update(buffer.data(), got);
                if (advanced && !advanced(got))
                    return false;
            }
            hex = hash.finishHex();
            return true;
        }
    }

    // ---- sources ---------------------------------------------------------------

    DiscSource::DiscSource() = default;
    DiscSource::~DiscSource() = default;
    DiscSource::DiscSource(DiscSource &&) noexcept = default;
    DiscSource &DiscSource::operator=(DiscSource &&) noexcept = default;

    namespace
    {
        constexpr const char *kCueUnreadable = "The CUE file cannot be read.";
        constexpr const char *kCueNamesNoData = "The CUE file names no data track.";

        std::string notBesideCue(const std::string &name)
        {
            return "The CUE file names " + name + ", which is not beside it. Keep the BIN and CUE files together.";
        }
    }

    fs::path binOfCue(const fs::path &cue, std::string *error)
    {
        std::vector<uint8_t> text;
        if (!readFileBytes(cue, kMaxCueBytes, text))
        {
            if (error)
                *error = kCueUnreadable;
            return {};
        }
        const std::string name = zx::disc::cueDataTrackFile(std::string(text.begin(), text.end()));
        if (name.empty())
        {
            if (error)
                *error = kCueNamesNoData;
            return {};
        }
        const fs::path bin = cue.parent_path() / pathFromUtf8(name);
        std::error_code ec;
        if (!fs::is_regular_file(bin, ec))
        {
            if (error)
                *error = notBesideCue(name);
            return {};
        }
        return bin;
    }

    void installDocumentAccess(const DocumentAccess *access) { g_documents.store(access); }
    const DocumentAccess *documentAccess() { return g_documents.load(); }

    bool isDocument(const fs::path &source)
    {
        const DocumentAccess *documents = g_documents.load();
        return documents && documents->isDocument(utf8Of(source));
    }

    bool DiscSource::open(const fs::path &path, std::string *error)
    {
        auto fail = [&](const std::string &message)
        {
            if (error)
                *error = message;
            m_kind = Kind::None;
            m_image.reset();
            return false;
        };
        m_image.reset();
        m_folderDocuments.clear();
        if (const DocumentAccess *documents = g_documents.load(); documents && documents->isDocument(utf8Of(path)))
        {
            DocumentAccess::Entry document;
            m_picked = m_readFrom = path;
            m_name.clear();
            if (!documents->stat(utf8Of(path), document))
                return fail("The picked file or folder cannot be read any more. Pick it again.");
            return openDocument(document, nullptr, error);
        }
        m_picked = path;
        m_readFrom = path;
        m_name = path.filename().string();
        std::error_code ec;
        if (fs::is_directory(path, ec))
        {
            if (fs::is_regular_file(path / "SYSTEM.CNF", ec) || fs::is_regular_file(path / "system.cnf", ec))
            {
                m_kind = Kind::Folder;
                return true;
            }
            // A folder holding a disc image (a game's folder of dumps): the
            // first of its images that opens as a PS2 disc, a CUE before
            // its BIN.
            for (const fs::path &image : imageFilesIn(path))
            {
                DiscSource inner;
                std::vector<uint8_t> cnf;
                if (inner.open(image, nullptr) && inner.readFile("SYSTEM.CNF", cnf, kMaxSystemCnf))
                {
                    *this = std::move(inner);
                    m_picked = path;
                    m_name = path.filename().string();
                    return true;
                }
            }
            return fail("No PS2 disc found in " + path.filename().string() + ".");
        }
        if (!fs::is_regular_file(path, ec))
            return fail(path.string() + " not found.");
        fs::path imagePath = path;
        if (cueSheetFile(path))
        {
            std::string why;
            imagePath = binOfCue(path, &why);
            if (imagePath.empty())
                return fail(why);
        }
        auto image = std::make_shared<zx::disc::Iso9660Image>();
        std::string why;
        if (!image->open(imagePath, &why))
            return fail(notAnImage(path.filename().string(), otherImageKind(imagePath)));
        m_readFrom = imagePath;
        m_kind = image->fileSectorSize() == zx::disc::Iso9660Image::kSectorSize ? Kind::IsoImage : Kind::RawImage;
        m_image = std::move(image);
        return true;
    }

    bool DiscSource::openDocument(const DocumentAccess::Entry &document, const std::vector<DocumentAccess::Entry> *siblings,
                                  std::string *error)
    {
        auto fail = [&](const std::string &message)
        {
            if (error)
                *error = message;
            m_kind = Kind::None;
            m_image.reset();
            m_folderDocuments.clear();
            return false;
        };
        const DocumentAccess &documents = *g_documents.load();
        m_picked = m_readFrom = pathFromUtf8(document.uri);
        m_name = document.name;
        m_image.reset();
        m_folderDocuments.clear();
        if (document.folder)
        {
            const std::vector<DocumentAccess::Entry> top = documents.list(document.uri);
            if (hasSystemCnf(top))
            {
                documentFiles(documents, document.uri, "", 0, m_folderDocuments);
                std::sort(m_folderDocuments.begin(), m_folderDocuments.end(),
                          [](const DocumentAccess::Entry &a, const DocumentAccess::Entry &b) { return a.name < b.name; });
                m_kind = Kind::Folder;
                return true;
            }
            // A folder holding a disc image, as with a folder's path.
            for (const DocumentAccess::Entry &image : imageDocuments(documents, top))
            {
                DiscSource inner;
                std::vector<uint8_t> cnf;
                if (inner.openDocument(image, &top, nullptr) && inner.readFile("SYSTEM.CNF", cnf, kMaxSystemCnf))
                {
                    *this = std::move(inner);
                    m_picked = pathFromUtf8(document.uri);
                    m_name = document.name;
                    return true;
                }
            }
            return fail("No PS2 disc found in " + document.name + ".");
        }

        DocumentAccess::Entry image = document;
        if (std::string cue; cueSheetDocument(documents, document, &cue))
        {
            // Its BIN among the documents beside it: a CUE picked alone has
            // none the app may read.
            if (!siblings)
                return fail("Pick the BIN with the CUE, or the BIN alone.");
            const std::string data = zx::disc::cueDataTrackFile(cue);
            if (data.empty())
                return fail(kCueNamesNoData);
            const auto beside = std::find_if(siblings->begin(), siblings->end(), [&](const DocumentAccess::Entry &entry)
                                             { return !entry.folder && entry.name == data; });
            if (beside == siblings->end())
                return fail(notBesideCue(data));
            image = *beside;
        }
        auto opened = std::make_shared<zx::disc::Iso9660Image>();
        std::string why;
        if (!opened->open(documents.open(image.uri), pathFromUtf8(image.name), &why))
            return fail(notAnImage(document.name, otherDocumentKind(documents, image.uri)));
        m_readFrom = pathFromUtf8(image.uri);
        m_kind = opened->fileSectorSize() == zx::disc::Iso9660Image::kSectorSize ? Kind::IsoImage : Kind::RawImage;
        m_image = std::move(opened);
        return true;
    }

    const DocumentAccess::Entry *DiscSource::folderDocument(const std::string &path) const
    {
        const auto exact = std::lower_bound(m_folderDocuments.begin(), m_folderDocuments.end(), path,
                                            [](const DocumentAccess::Entry &entry, const std::string &key) { return entry.name < key; });
        if (exact != m_folderDocuments.end() && exact->name == path)
            return &*exact;
        // (a disc folder copied by hand may have its names in another case)
        const std::string wanted = lower(path);
        for (const DocumentAccess::Entry &entry : m_folderDocuments)
            if (lower(entry.name) == wanted)
                return &entry;
        return nullptr;
    }

    std::string DiscSource::kindText() const
    {
        if (m_image && m_image->format() != zx::disc::DiscImageFile::Format::Plain)
            return std::string(zx::disc::discImageFormatName(m_image->format())) + " disc image";
        switch (m_kind)
        {
        case Kind::Folder:
            return "folder";
        case Kind::IsoImage:
            return "disc image";
        case Kind::RawImage:
            return "BIN/CUE disc image";
        case Kind::None:
            break;
        }
        return "nothing";
    }

    std::vector<DiscSource::File> DiscSource::files() const
    {
        std::vector<File> out;
        if (m_kind == Kind::Folder && !m_folderDocuments.empty())
        {
            for (const DocumentAccess::Entry &entry : m_folderDocuments)
                out.push_back({entry.name, entry.size});
            return out;
        }
        if (m_kind == Kind::Folder)
            return folderFiles(m_readFrom);
        if (m_image)
            imageFiles(*m_image, "", 0, out);
        return out;
    }

    bool DiscSource::readFile(const std::string &path, std::vector<uint8_t> &out, size_t maxBytes) const
    {
        if (m_kind == Kind::Folder && !m_folderDocuments.empty())
        {
            const DocumentAccess::Entry *document = folderDocument(path);
            return document && readDocument(*g_documents.load(), document->uri, maxBytes, out);
        }
        if (m_kind == Kind::Folder)
            return readFileBytes(folderFile(m_readFrom, path), maxBytes, out);
        if (!m_image)
            return false;
        const std::optional<zx::disc::IsoEntry> entry = m_image->find(path);
        return entry && m_image->readFile(*entry, out, maxBytes);
    }

    bool DiscSource::streamFile(const std::string &path, const std::function<bool(const uint8_t *, size_t)> &sink) const
    {
        std::vector<uint8_t> buffer(kChunk);
        if (m_kind == Kind::Folder && !m_folderDocuments.empty())
        {
            const DocumentAccess::Entry *document = folderDocument(path);
            const OpenFile file(document ? g_documents.load()->open(document->uri) : nullptr);
            if (!file)
                return false;
            for (size_t got; (got = std::fread(buffer.data(), 1, buffer.size(), file.get())) != 0u;)
                if (!sink(buffer.data(), got))
                    return false;
            return !std::ferror(file.get());
        }
        if (m_kind == Kind::Folder)
        {
            std::ifstream in(folderFile(m_readFrom, path), std::ios::binary);
            if (!in)
                return false;
            while (in)
            {
                in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                const size_t got = static_cast<size_t>(in.gcount());
                if (got != 0u && !sink(buffer.data(), got))
                    return false;
            }
            return in.eof();
        }
        if (!m_image)
            return false;
        const std::optional<zx::disc::IsoEntry> entry = m_image->find(path);
        if (!entry || entry->directory)
            return false;
        const uint64_t start = static_cast<uint64_t>(entry->lsn) * zx::disc::Iso9660Image::kSectorSize;
        for (uint64_t done = 0; done < entry->size;)
        {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry->size - done));
            if (!m_image->read(start + done, buffer.data(), count) || !sink(buffer.data(), count))
                return false;
            done += count;
        }
        return true;
    }

    bool isCueSheetSource(const fs::path &source)
    {
        if (const DocumentAccess *documents = g_documents.load(); documents && documents->isDocument(utf8Of(source)))
        {
            DocumentAccess::Entry document;
            return documents->stat(utf8Of(source), document) && cueSheetDocument(*documents, document);
        }
        return cueSheetFile(source);
    }

    bool sourceReadable(const fs::path &source)
    {
        if (const DocumentAccess *documents = g_documents.load(); documents && documents->isDocument(utf8Of(source)))
        {
            DocumentAccess::Entry document;
            return documents->stat(utf8Of(source), document);
        }
        std::error_code ec;
        return !source.empty() && fs::exists(source, ec);
    }

    std::string sourceName(const fs::path &source)
    {
        if (const DocumentAccess *documents = g_documents.load(); documents && documents->isDocument(utf8Of(source)))
        {
            DocumentAccess::Entry document;
            return documents->stat(utf8Of(source), document) ? document.name : std::string("the picked disc");
        }
        return source.filename().string();
    }

    // ---- which game ------------------------------------------------------------

    const OtherRelease *otherRelease(std::string_view bootElf)
    {
        // Redump's PS2 discs of the three games other than the USA releases
        // the collection runs (their boot ELF named for the serial, as PS2
        // discs do).
        static constexpr OtherRelease kReleases[] = {
            {"SLES_504.80", "ai", "the European release"},
            {"SLUS_290.21", "ai", "a demo disc"},
            {"SLED_510.74", "ai", "a demo disc"},
            {"SLES_502.17", "dm2", "the European release"},
            {"SLES_508.80", "bmx", "the European release"},
            {"SLES_513.65", "bmx", "a European release"},
        };
        for (const OtherRelease &release : kReleases)
            if (lower(release.elf) == lower(std::string(bootElf)))
                return &release;
        return nullptr;
    }

    SourceCheck checkSource(const DiscSource &source, const std::vector<SetupGame> &games, int wanted)
    {
        SourceCheck check;
        const std::string name = source.name();
        if (source.kind() == DiscSource::Kind::None)
        {
            check.message = "No disc to check.";
            return check;
        }
        std::vector<uint8_t> cnf;
        if (!source.readFile("SYSTEM.CNF", cnf, kMaxSystemCnf) ||
            (check.bootElf = bootElfFromSystemCnf(std::string(cnf.begin(), cnf.end()))).empty())
        {
            check.status = SourceCheck::Status::NotPs2Disc;
            check.message = name + " is not a PS2 game disc.";
            return check;
        }
        std::string elfPath = check.bootElf;
        std::replace(elfPath.begin(), elfPath.end(), '\\', '/');
        std::vector<uint8_t> elf;
        if (!source.readFile(elfPath, elf, kMaxElf))
        {
            check.status = SourceCheck::Status::NotPs2Disc;
            check.message = name + " is damaged, " + check.bootElf + " cannot be read.";
            return check;
        }
        check.elfSha256 = sha256Hex(elf.data(), elf.size());
        for (const DiscSource::File &file : source.files())
        {
            check.bytes += file.size;
            ++check.files;
        }

        int byName = -1;
        for (size_t i = 0; i < games.size(); ++i)
            if (lower(games[i].elf) == lower(check.bootElf))
                byName = static_cast<int>(i);
        const std::string asked = wanted >= 0 && wanted < static_cast<int>(games.size()) ? games[static_cast<size_t>(wanted)].title : "";
        if (byName < 0)
        {
            if (const OtherRelease *release = otherRelease(check.bootElf))
            {
                for (size_t i = 0; i < games.size(); ++i)
                    if (games[i].id == release->game)
                    {
                        check.status = SourceCheck::Status::OtherRelease;
                        check.game = static_cast<int>(i);
                        check.message = name + " is " + release->what + " of " + games[i].title + ". Use the USA disc.";
                        return check;
                    }
            }
            check.status = SourceCheck::Status::UnknownGame;
            check.message = name + " is not " + gameList(games) + ".";
            return check;
        }
        const SetupGame &game = games[static_cast<size_t>(byName)];
        check.game = byName;
        if (!game.elfSha256.empty() && lower(game.elfSha256) != check.elfSha256)
        {
            check.status = SourceCheck::Status::WrongVersion;
            check.message = name + " is another version of " + game.title + ". Use the USA release.";
            return check;
        }
        if (wanted >= 0 && wanted != byName)
        {
            check.status = SourceCheck::Status::OtherGame;
            check.message = name + " is " + game.title + ", not " + asked + ".";
            return check;
        }
        check.status = SourceCheck::Status::Ok;
        check.message = name + ": " + game.title + ", " + sizeText(check.bytes) + ".";
        return check;
    }

    // ---- the code step ---------------------------------------------------------

    namespace
    {
        bool discElfHash(const SetupGame &game, const fs::path &discFolder, std::string &hex)
        {
            std::vector<uint8_t> elf;
            if (!readFileBytes(folderFile(discFolder, game.elf), kMaxElf, elf))
                return false;
            hex = sha256Hex(elf.data(), elf.size());
            return true;
        }
    }

    bool PrebuiltGameCode::ready(const SetupGame &game, const fs::path &discFolder, const fs::path &) const
    {
        std::string hex;
        return discElfHash(game, discFolder, hex) && (game.elfSha256.empty() || lower(game.elfSha256) == hex);
    }

    bool PrebuiltGameCode::make(const SetupGame &game, const fs::path &discFolder, const fs::path &gameDir,
                                const SetupProgressFn &progress, std::string *error)
    {
        if (!report(progress, {name(), game.elf, 0, 1}))
            return false;
        if (!ready(game, discFolder, gameDir))
        {
            if (error)
                *error = "This disc does not match this app's " + game.title + ".";
            return false;
        }
        report(progress, {name(), game.elf, 1, 1});
        return true;
    }

    std::unique_ptr<GameCodeStep> makeGameCodeStep() { return std::make_unique<PrebuiltGameCode>(); }

    // ---- installing ------------------------------------------------------------

    fs::path installedDiscFolder(const fs::path &gameDir) { return gameDir / "disc"; }
    fs::path installManifestFile(const fs::path &gameDir) { return gameDir / "disc.manifest"; }

    InstallResult installGame(const DiscSource &source, const SetupGame &game, const fs::path &gameDir, GameCodeStep *code,
                              const SetupProgressFn &progress)
    {
        InstallResult result;
        const SourceCheck check = checkSource(source, {game}, 0);
        if (check.status != SourceCheck::Status::Ok)
        {
            result.message = check.message;
            return result;
        }

        std::error_code ec;
        fs::create_directories(gameDir, ec);
        if (ec)
        {
            result.message = "The folder " + gameDir.string() + " cannot be made (" + ec.message() + ").";
            return result;
        }
        const fs::path staging = gameDir / "disc.partial";
        fs::remove_all(staging, ec);
        const fs::space_info space = fs::space(gameDir, ec);
        if (!ec && space.available < check.bytes + 64u * 1024u * 1024u)
        {
            result.message = "Not enough free disk space for " + game.title + ": it needs " + sizeText(check.bytes) + ", " +
                             sizeText(space.available) + " is free.";
            return result;
        }

        Manifest manifest;
        manifest.game = game.id;
        manifest.elf = check.bootElf;
        manifest.sha256 = check.elfSha256;
        manifest.source = isDocument(source.picked()) ? utf8Of(source.picked()) : utf8Of(fs::absolute(source.picked(), ec));
        manifest.kind = source.kindText();
        const std::vector<DiscSource::File> files = source.files();
        const std::string step = "Copying files";
        uint64_t done = 0;
        bool cancelled = false;
        for (const DiscSource::File &file : files)
        {
            const fs::path target = staging / pathFromUtf8(file.path);
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                result.message = "Cannot write " + target.string() + ".";
                fs::remove_all(staging, ec);
                return result;
            }
            Sha256 hash;
            uint64_t written = 0;
            const bool copied = source.streamFile(file.path, [&](const uint8_t *data, size_t size)
            {
                out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
                hash.update(data, size);
                written += size;
                done += size;
                if (!report(progress, {step, file.path, done, check.bytes}))
                {
                    cancelled = true;
                    return false;
                }
                return static_cast<bool>(out);
            });
            out.close();
            if (cancelled)
            {
                fs::remove_all(staging, ec);
                result.cancelled = true;
                result.message = "Stopped. " + game.title + " was not set up.";
                return result;
            }
            if (!copied || !out || written != file.size)
            {
                fs::remove_all(staging, ec);
                result.message = "Copying " + file.path + " failed. Check the disc image and the free space.";
                return result;
            }
            manifest.files.push_back({file.path, file.size, hash.finishHex()});
            manifest.bytes += file.size;
        }

        // The copy's boot ELF is the one checked.
        std::string copiedHash;
        if (!discElfHash(game, staging, copiedHash) || copiedHash != check.elfSha256)
        {
            fs::remove_all(staging, ec);
            result.message = "The copied files of " + game.title + " do not match the disc. Try again.";
            return result;
        }

        // In place: the manifest goes first (an install without one is not
        // complete), the earlier files aside, the new ones in, then the
        // manifest last.
        const fs::path disc = installedDiscFolder(gameDir), old = gameDir / "disc.old";
        fs::remove(installManifestFile(gameDir), ec);
        fs::remove_all(old, ec);
        if (fs::exists(disc, ec))
            fs::rename(disc, old, ec);
        fs::rename(staging, disc, ec);
        if (ec)
        {
            result.message = "The game files could not be put in place (" + ec.message() + ").";
            return result;
        }
        fs::remove_all(old, ec);
        const fs::path manifestTmp = gameDir / "disc.manifest.tmp";
        if (!writeText(manifestTmp, formatManifest(manifest)))
        {
            result.message = "The list of the game files could not be written in " + gameDir.string() + ".";
            return result;
        }
        fs::rename(manifestTmp, installManifestFile(gameDir), ec);
        result.filesInstalled = !ec;
        result.files = manifest.files.size();
        result.bytes = manifest.bytes;

        if (code)
        {
            std::string why;
            if (!code->make(game, disc, gameDir, progress, &why))
            {
                result.cancelled = why.empty();
                result.message = why.empty() ? "Stopped while " + lower(code->name()) + "." : why;
                return result;
            }
        }
        result.ok = true;
        result.message = game.title + " set up.";
        return result;
    }

    InstalledCheck checkInstalled(const fs::path &gameDir, const SetupGame &game, bool full, const GameCodeStep *code,
                                  const SetupProgressFn &progress)
    {
        InstalledCheck check;
        const fs::path disc = installedDiscFolder(gameDir);
        std::error_code ec;
        Manifest manifest;
        if (!readManifest(installManifestFile(gameDir), manifest))
        {
            if (fs::is_regular_file(disc / "SYSTEM.CNF", ec))
            {
                const DiscCheck byHand = checkDiscFolder(disc, game.title, game.elf, game.elfSha256);
                check.status = byHand.status == DiscCheck::Status::Ok ? InstalledCheck::Status::ByHand : InstalledCheck::Status::Damaged;
                check.message = byHand.status == DiscCheck::Status::Ok
                                    ? game.title + ": files added by hand."
                                    : byHand.message;
                return check;
            }
            check.message = game.title + " is not set up. Add its PS2 disc.";
            return check;
        }
        check.source = pathFromUtf8(manifest.source);
        if (manifest.game != game.id || lower(manifest.elf) != lower(game.elf) ||
            (!game.elfSha256.empty() && lower(manifest.sha256) != lower(game.elfSha256)))
        {
            check.status = InstalledCheck::Status::Damaged;
            check.message = game.title + "'s files are from another disc. Set it up again.";
            return check;
        }
        uint64_t total = 0, done = 0;
        for (const Manifest::Entry &e : manifest.files)
            total += e.size;
        const std::string step = "Checking files";
        for (const Manifest::Entry &e : manifest.files)
        {
            const fs::path file = disc / pathFromUtf8(e.path);
            const uintmax_t size = fs::file_size(file, ec);
            std::string problem;
            const bool hashThis = full || lower(e.path) == lower(game.elf);
            if (ec)
                problem = e.path + " is missing";
            else if (size != e.size)
                problem = e.path + " has the wrong size";
            else if (hashThis)
            {
                std::string hex;
                bool stopped = false;
                const bool hashed = hashFile(file, hex, [&](size_t got)
                {
                    done += got;
                    stopped = !report(progress, {step, e.path, done, total});
                    return !stopped;
                });
                if (stopped)
                {
                    check.status = InstalledCheck::Status::Ok;
                    check.message = "Check stopped.";
                    return check;
                }
                if (!hashed)
                    problem = e.path + " cannot be read";
                else if (hex != e.sha256)
                    problem = e.path + " is damaged";
            }
            if (!hashThis || !problem.empty())
                report(progress, {step, e.path, done += hashThis ? 0u : e.size, total});
            if (!problem.empty())
            {
                if (check.problems.size() < 5)
                    check.problems.push_back(problem);
                check.status = InstalledCheck::Status::Damaged;
            }
            ++check.files;
            check.bytes += e.size;
        }
        if (check.status == InstalledCheck::Status::Damaged)
        {
            check.message = game.title + "'s files are damaged. Repair them from its disc.";
            return check;
        }
        if (code && !code->ready(game, disc, gameDir))
        {
            check.status = InstalledCheck::Status::CodeMissing;
            check.message = game.title + "'s game code is not ready.";
            return check;
        }
        check.status = InstalledCheck::Status::Ok;
        check.message = "Check complete. All files match.";
        return check;
    }

    bool removeInstalled(const fs::path &gameDir, std::string *error)
    {
        std::error_code ec;
        fs::remove(installManifestFile(gameDir), ec);
        for (const char *name : {"disc", "disc.partial", "disc.old"})
        {
            fs::remove_all(gameDir / name, ec);
            if (ec)
            {
                if (error)
                    *error = "Cannot remove " + (gameDir / name).string() + " (" + ec.message() + ").";
                return false;
            }
        }
        return true;
    }

    fs::path pathFromUtf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

    std::string utf8Of(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::string sizeText(uint64_t bytes)
    {
        char text[32];
        if (bytes >= 1000ull * 1000ull * 1000ull)
            std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
        else if (bytes >= 1000ull * 1000ull)
            std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / 1e6);
        else if (bytes >= 1000ull)
            std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1e3);
        else
            std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
        return text;
    }
}
