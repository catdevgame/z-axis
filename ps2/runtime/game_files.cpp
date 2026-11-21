#include "runtime/game_files.h"
#include "runtime/host_write.h"

#include "runtime/disc_patch.h"
#include "runtime/boot_config.h"
#include "runtime/zip_archive.h"
#include "disc/iso9660.h"
#include "zal/zal_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <system_error>

namespace zx::game_files
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

        class HostFile final : public GameFile
        {
        public:
            HostFile(std::FILE *stream, uint64_t size) : m_stream(stream), m_size(size) {}
            ~HostFile() override
            {
                if (m_stream)
                    std::fclose(m_stream);
            }
            uint64_t size() const override { return m_size; }
            size_t read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset >= m_size || size == 0u)
                    return 0u;
                size = static_cast<size_t>(std::min<uint64_t>(size, m_size - offset));
                std::lock_guard<std::mutex> lock(m_mutex); // (one stream, read from more than one thread)
                if (offset != m_position)
                {
#if defined(_WIN32)
                    if (_fseeki64(m_stream, static_cast<long long>(offset), SEEK_SET) != 0)
#else
                    if (fseeko(m_stream, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
                    {
                        m_position = ~0ull;
                        return 0u;
                    }
                }
                ps2BeforeHostWrite(destination, size);
                const size_t got = std::fread(destination, 1u, size, m_stream);
                m_position = offset + got;
                return got;
            }

        private:
            std::mutex m_mutex;
            std::FILE *m_stream = nullptr;
            uint64_t m_size = 0u;
            uint64_t m_position = 0u;
        };

        class ImageFile final : public GameFile
        {
        public:
            ImageFile(std::shared_ptr<const zx::disc::Iso9660Image> image, uint64_t offset, uint64_t size)
                : m_image(std::move(image)), m_offset(offset), m_size(size)
            {
            }
            uint64_t size() const override { return m_size; }
            size_t read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset >= m_size || size == 0u)
                    return 0u;
                size = static_cast<size_t>(std::min<uint64_t>(size, m_size - offset));
                return m_image->read(m_offset + offset, destination, size) ? size : 0u;
            }

        private:
            std::shared_ptr<const zx::disc::Iso9660Image> m_image;
            uint64_t m_offset = 0u;
            uint64_t m_size = 0u;
        };

        class MemoryFile final : public GameFile
        {
        public:
            explicit MemoryFile(std::shared_ptr<const std::vector<uint8_t>> bytes) : m_bytes(std::move(bytes)) {}
            uint64_t size() const override { return m_bytes->size(); }
            size_t read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset >= m_bytes->size() || size == 0u)
                    return 0u;
                size = static_cast<size_t>(std::min<uint64_t>(size, m_bytes->size() - offset));
                std::memcpy(destination, m_bytes->data() + offset, size);
                return size;
            }

        private:
            std::shared_ptr<const std::vector<uint8_t>> m_bytes;
        };

        // A file with its first bytes replaced (registerHead): the head from
        // memory, the rest the file's own.
        class HeadFile final : public GameFile
        {
        public:
            HeadFile(std::shared_ptr<const std::vector<uint8_t>> head, std::shared_ptr<GameFile> rest)
                : m_head(std::move(head)), m_rest(std::move(rest))
            {
            }
            uint64_t size() const override { return std::max<uint64_t>(m_head->size(), m_rest->size()); }
            size_t read(uint64_t offset, void *destination, size_t size) override
            {
                size_t done = 0;
                auto *out = static_cast<uint8_t *>(destination);
                if (offset < m_head->size())
                {
                    done = static_cast<size_t>(std::min<uint64_t>(size, m_head->size() - offset));
                    std::memcpy(out, m_head->data() + offset, done);
                }
                if (done < size)
                    done += m_rest->read(offset + done, out + done, size - done);
                return done;
            }

        private:
            std::shared_ptr<const std::vector<uint8_t>> m_head;
            std::shared_ptr<GameFile> m_rest;
        };

        struct HeadEntry
        {
            std::string path; // normalized
            Head make;
            std::string name;
        };
        // The boot's heads, and those made (by the path, the file's source
        // and size: null when its maker left it as it is).
        std::vector<HeadEntry> &heads()
        {
            static zx::boot::PerBoot<std::vector<HeadEntry>> ofBoot([] { return std::vector<HeadEntry>{}; });
            return ofBoot.get();
        }
        using MadeHeads = std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>;
        MadeHeads &madeHeads()
        {
            static zx::boot::PerBoot<MadeHeads> ofBoot([] { return MadeHeads{}; });
            return ofBoot.get();
        }

        struct ProviderEntry
        {
            std::string folder;
            Provider provide;
            std::string name;
        };

        std::mutex &providerMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        // The boot's providers: each game boot registers its own at its
        // install, so a game booted after another in the same process is
        // not served the other's files (runtime/boot_config.h).
        std::vector<ProviderEntry> &providers()
        {
            static zx::boot::PerBoot<std::vector<ProviderEntry>> ofBoot([] { return std::vector<ProviderEntry>{}; });
            return ofBoot.get();
        }

        // The first provider's file for a normalized path, and its name.
        std::shared_ptr<const std::vector<uint8_t>> provided(const std::string &normalized, std::string *name)
        {
            std::vector<ProviderEntry> list;
            {
                std::lock_guard<std::mutex> lock(providerMutex());
                list = providers();
            }
            for (const ProviderEntry &entry : list)
            {
                if (normalized.size() <= entry.folder.size() || normalized.compare(0, entry.folder.size(), entry.folder) != 0)
                    continue;
                if (std::shared_ptr<const std::vector<uint8_t>> bytes = entry.provide(normalized))
                {
                    if (name)
                        *name = entry.name;
                    return bytes;
                }
            }
            return nullptr;
        }

        struct MemoryFolderEntry
        {
            std::string name;
            MemoryFolder files;
        };

        // The memory folders, the last registered last. Not per boot: the
        // host that registers one removes it.
        std::vector<MemoryFolderEntry> &memoryFolders()
        {
            static std::vector<MemoryFolderEntry> folders;
            return folders;
        }

        // The newest memory folder's file for a normalized path, and its folder's name.
        std::shared_ptr<const std::vector<uint8_t>> inMemory(const std::string &normalized, std::string *name)
        {
            std::lock_guard<std::mutex> lock(providerMutex());
            const std::vector<MemoryFolderEntry> &folders = memoryFolders();
            for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder)
            {
                auto it = folder->files.find(normalized);
                if (it != folder->files.end())
                {
                    if (name)
                        *name = folder->name;
                    return it->second;
                }
            }
            return nullptr;
        }

        // Whether a memory folder has a file below the normalized directory.
        bool directoryInMemory(const std::string &normalized)
        {
            std::lock_guard<std::mutex> lock(providerMutex());
            const std::string prefix = normalized + "/";
            for (const MemoryFolderEntry &folder : memoryFolders())
            {
                auto it = folder.files.lower_bound(prefix);
                if (it != folder.files.end() && it->first.compare(0, prefix.size(), prefix) == 0)
                    return true;
            }
            return false;
        }

        std::shared_ptr<GameFile> openHost(const std::filesystem::path &path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return nullptr;
            const uint64_t size = std::filesystem::file_size(path, ec);
            if (ec)
                return nullptr;
#if defined(_WIN32)
            std::FILE *stream = ::_wfopen(path.c_str(), L"rb");
#else
            std::FILE *stream = std::fopen(path.c_str(), "rb");
#endif
            if (!stream)
                return nullptr;
            return std::make_shared<HostFile>(stream, size);
        }

        // The archives made with mods' entries (GameFileSystem::withEntries),
        // for the boot: by the archive's path, where the file under them came
        // from and every entry's bytes, so a file system over the same
        // folders makes one once.
        struct MadeArchive
        {
            std::shared_ptr<const std::vector<uint8_t>> bytes;
            std::string source;
        };
        using MadeArchives = std::map<std::string, MadeArchive>;
        MadeArchives &madeArchives()
        {
            static zx::boot::PerBoot<MadeArchives> ofBoot([] { return MadeArchives{}; });
            return ofBoot.get();
        }
        std::mutex &madeArchivesMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    }

    bool isArchivePath(std::string_view normalized)
    {
        return normalized.size() > 4 && upper(normalized.substr(normalized.size() - 4)) == ".ZAL";
    }

    bool ignoredInArchiveFolder(std::string_view name)
    {
        return name.empty() || name.front() == '.' || upper(name) == upper(zx::zal::kListingFile);
    }

    std::string normalizePath(std::string_view path)
    {
        // Device prefix: "dvd0:", "cdrom0:", "host:" ... (a colon before any
        // separator).
        const size_t colon = path.find(':');
        if (colon != std::string_view::npos && path.substr(0, colon).find_first_of("/\\") == std::string_view::npos)
            path.remove_prefix(colon + 1u);
        std::vector<std::string> parts;
        size_t start = 0u;
        while (start <= path.size())
        {
            size_t end = path.find_first_of("/\\", start);
            if (end == std::string_view::npos)
                end = path.size();
            std::string_view part = path.substr(start, end - start);
            if (const size_t semicolon = part.find(';'); semicolon != std::string_view::npos)
                part = part.substr(0, semicolon);
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else if (!part.empty() && part != ".")
            {
                parts.push_back(upper(part));
            }
            start = end + 1u;
        }
        std::string out;
        for (const std::string &part : parts)
        {
            if (!out.empty())
                out += '/';
            out += part;
        }
        return out;
    }

    void registerProvider(const std::string &folder, Provider provide, std::string name)
    {
        std::string prefix = normalizePath(folder);
        if (!prefix.empty())
            prefix += '/';
        std::lock_guard<std::mutex> lock(providerMutex());
        providers().push_back({std::move(prefix), std::move(provide), std::move(name)});
    }

    void removeProviders(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        auto &list = providers();
        list.erase(std::remove_if(list.begin(), list.end(), [&name](const auto &entry) { return entry.name == name; }),
                   list.end());
    }

    void clearProviders()
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        providers().clear();
    }

    void registerHead(const std::string &path, Head make, std::string name)
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        heads().push_back({normalizePath(path), std::move(make), std::move(name)});
    }

    void removeHeads(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        auto &list = heads();
        list.erase(std::remove_if(list.begin(), list.end(), [&name](const auto &entry) { return entry.name == name; }), list.end());
    }

    void registerMemoryFolder(const std::string &name, MemoryFolder files)
    {
        MemoryFolder normalized;
        for (auto &[path, bytes] : files)
            normalized[normalizePath(path)] = std::move(bytes);
        std::lock_guard<std::mutex> lock(providerMutex());
        std::vector<MemoryFolderEntry> &folders = memoryFolders();
        folders.erase(std::remove_if(folders.begin(), folders.end(), [&name](const MemoryFolderEntry &f) { return f.name == name; }),
                      folders.end());
        folders.push_back({name, std::move(normalized)});
    }

    void removeMemoryFolder(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        std::vector<MemoryFolderEntry> &folders = memoryFolders();
        folders.erase(std::remove_if(folders.begin(), folders.end(), [&name](const MemoryFolderEntry &f) { return f.name == name; }),
                      folders.end());
    }

    bool readAll(GameFile &file, std::vector<uint8_t> &bytes)
    {
        bytes.resize(static_cast<size_t>(file.size()));
        return file.read(0u, bytes.data(), bytes.size()) == bytes.size();
    }

    std::vector<std::filesystem::path> modsRoots(std::string_view text)
    {
        std::vector<std::filesystem::path> roots;
        for (size_t start = 0; start <= text.size();)
        {
            size_t end = text.find(kModsSeparator, start);
            if (end == std::string_view::npos)
                end = text.size();
            if (end > start)
                roots.emplace_back(std::string(text.substr(start, end - start)));
            start = end + 1u;
        }
        return roots;
    }

    std::string modsText(const std::vector<std::filesystem::path> &roots)
    {
        std::string text;
        for (const std::filesystem::path &root : roots)
        {
            if (!text.empty())
                text += kModsSeparator;
            text += root.string();
        }
        return text;
    }

    std::vector<std::string> listFilesBelow(const std::filesystem::path &folder)
    {
        std::vector<std::string> files;
        std::error_code ec;
        if (std::filesystem::is_directory(folder, ec))
        {
            for (std::filesystem::recursive_directory_iterator it(folder, ec), last; !ec && it != last; it.increment(ec))
                if (it->is_regular_file(ec))
                    files.push_back(it->path().lexically_relative(folder).generic_string());
            std::sort(files.begin(), files.end());
            return files;
        }
        const ZipPath zip = zipPathOf(folder);
        if (zip.zip)
            files = zip.zip->filesBelow(zip.inside);
        return files;
    }

    bool readFileBelow(const std::filesystem::path &folder, const std::string &relative, std::vector<uint8_t> &out)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(folder, ec))
        {
            GameFileRoots roots;
            roots.disc = folder;
            GameFileSystem below(std::move(roots));
            std::shared_ptr<GameFile> file = below.open(relative);
            return file && readAll(*file, out);
        }
        const ZipPath zip = zipPathOf(folder);
        if (!zip.zip)
            return false;
        const std::string inside = normalizePath(relative);
        return !inside.empty() && zip.zip->read(zip.inside.empty() ? inside : zip.inside + "/" + inside, out);
    }

    GameFileSystem::GameFileSystem(GameFileRoots roots) : m_roots(std::move(roots)) {}

    const ZipPath &GameFileSystem::zipRoot(const std::filesystem::path &root)
    {
        const std::string key = root.string();
        auto it = m_zipRoots.find(key);
        if (it == m_zipRoots.end())
            it = m_zipRoots.emplace(key, std::make_shared<ZipPath>(zipPathOf(root))).first;
        return *it->second;
    }

    std::filesystem::path GameFileSystem::resolveHost(const std::filesystem::path &root, const std::string &normalized,
                                                      bool wantDirectory)
    {
        if (root.empty() || normalized.empty())
            return {};
        std::error_code ec;
        // As given first (disc folders extracted from the image keep its
        // upper-case names; macOS volumes are case-insensitive anyway).
        const std::filesystem::path direct = root / std::filesystem::path(normalized);
        if (wantDirectory ? std::filesystem::is_directory(direct, ec) : std::filesystem::is_regular_file(direct, ec))
            return direct;
        // Then component by component, ignoring case.
        std::filesystem::path current = root;
        size_t start = 0u;
        while (start < normalized.size())
        {
            size_t end = normalized.find('/', start);
            if (end == std::string::npos)
                end = normalized.size();
            const std::string part = normalized.substr(start, end - start);
            const std::string key = current.string();
            auto listing = m_listings.find(key);
            if (listing == m_listings.end())
            {
                std::map<std::string, std::string> names;
                for (std::filesystem::directory_iterator it(current, ec), last; !ec && it != last; it.increment(ec))
                {
                    const std::string name = it->path().filename().string();
                    names.emplace(upper(name), name);
                }
                listing = m_listings.emplace(key, std::move(names)).first;
            }
            const auto found = listing->second.find(part);
            if (found == listing->second.end())
                return {};
            current /= found->second;
            start = end + 1u;
        }
        if (wantDirectory ? std::filesystem::is_directory(current, ec) : std::filesystem::is_regular_file(current, ec))
            return current;
        return {};
    }

    std::shared_ptr<GameFile> GameFileSystem::openInMod(const std::filesystem::path &root, const std::string &normalized)
    {
        if (const ZipPath &zip = zipRoot(root); zip.zip)
        {
            const std::string inside = zip.inside.empty() ? normalized : zip.inside + "/" + normalized;
            std::vector<uint8_t> bytes;
            if (!zip.zip->hasFile(inside) || !zip.zip->read(inside, bytes))
                return nullptr;
            auto file = std::make_shared<MemoryFile>(std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
            file->m_origin = GameFile::Origin::Mod;
            file->m_source = zip.zip->file().string() + ":" + inside;
            return file;
        }
        const std::filesystem::path mod = resolveHost(root, normalized);
        if (mod.empty())
            return nullptr;
        std::shared_ptr<GameFile> file = openHost(mod);
        if (file)
        {
            file->m_origin = GameFile::Origin::Mod;
            file->m_source = mod.string();
        }
        return file;
    }

    std::filesystem::path GameFileSystem::entryFolderIn(const std::filesystem::path &root, const std::string &normalized)
    {
        if (!isArchivePath(normalized))
            return {};
        if (const ZipPath &zip = zipRoot(root); zip.zip)
        {
            const std::string inside = zip.inside.empty() ? normalized : zip.inside + "/" + normalized;
            return zip.zip->hasFolder(inside) ? root / std::filesystem::path(normalized) : std::filesystem::path();
        }
        return resolveHost(root, normalized, true);
    }

    std::shared_ptr<GameFile> GameFileSystem::withEntries(const std::string &normalized, std::shared_ptr<GameFile> base,
                                                          const std::vector<std::filesystem::path> &folders)
    {
        namespace zal = zx::zal;
        // Every mod's entries, the lowest mod's first: their bytes read now
        // (they are the archive's key with the file under them).
        struct Given
        {
            std::filesystem::path folder;
            std::string name;
            std::vector<uint8_t> bytes;
        };
        std::vector<Given> given;
        std::string key = normalized + "\n" + base->source() + "\n" + std::to_string(base->size());
        for (const std::filesystem::path &folder : folders)
            for (const std::string &name : listFilesBelow(folder))
            {
                if (name.find('/') != std::string::npos)
                {
                    std::cout << "[mods] " << normalized << ": " << (folder / name).string()
                              << " is in a folder of its own, not an entry: not read" << std::endl;
                    continue;
                }
                if (ignoredInArchiveFolder(name))
                    continue;
                Given entry{folder, name, {}};
                if (!readFileBelow(folder, name, entry.bytes))
                    continue;
                key += "\n" + (folder / name).string() + " " + std::to_string(entry.bytes.size()) + " " +
                       std::to_string(std::hash<std::string_view>()(std::string_view(reinterpret_cast<const char *>(entry.bytes.data()), entry.bytes.size())));
                given.push_back(std::move(entry));
            }
        {
            std::lock_guard<std::mutex> lock(madeArchivesMutex());
            const MadeArchives &made = madeArchives();
            if (auto it = made.find(key); it != made.end())
            {
                auto file = std::make_shared<MemoryFile>(it->second.bytes);
                file->m_origin = GameFile::Origin::Mod;
                file->m_source = it->second.source;
                return file;
            }
        }
        std::vector<uint8_t> bytes;
        zal::Archive archive;
        std::string error;
        if (!readAll(*base, bytes) || !zal::read(bytes, archive, &error))
        {
            std::cout << "[mods] " << normalized << ": mods have entries for it, but " << base->source() << " is not an archive ("
                      << error << "): they are not read" << std::endl;
            return base;
        }
        size_t replaced = 0;
        for (Given &entry : given)
        {
            const std::optional<size_t> index = zal::findEntry(archive, entry.name);
            if (!index)
            {
                std::cout << "[mods] " << normalized << ": it has no entry " << entry.name << ": "
                          << (entry.folder / entry.name).string() << " is not read" << std::endl;
                continue;
            }
            archive.entries[*index].data = std::move(entry.bytes);
            ++replaced;
        }
        std::vector<uint8_t> out;
        if (!zal::write(archive, out, &error))
        {
            std::cout << "[mods] " << normalized << ": the archive with the mods' entries cannot be made (" << error
                      << "): they are not read" << std::endl;
            return base;
        }
        MadeArchive made{std::make_shared<const std::vector<uint8_t>>(std::move(out)),
                         base->source() + " with " + std::to_string(replaced) + " entries of mods"};
        std::cout << "[mods] " << normalized << ": " << made.source << std::endl;
        {
            std::lock_guard<std::mutex> lock(madeArchivesMutex());
            madeArchives()[key] = made;
        }
        auto file = std::make_shared<MemoryFile>(made.bytes);
        file->m_origin = GameFile::Origin::Mod;
        file->m_source = made.source;
        return file;
    }

    std::shared_ptr<GameFile> GameFileSystem::openOriginal(const std::string &normalized)
    {
        {
            std::string name;
            if (std::shared_ptr<const std::vector<uint8_t>> bytes = inMemory(normalized, &name))
            {
                auto file = std::make_shared<MemoryFile>(std::move(bytes));
                file->m_origin = GameFile::Origin::Mod;
                file->m_source = "memory:" + name + ":" + normalized;
                return file;
            }
        }
        // The last mod's file wins; the folders of single entries above it
        // are put in it (the highest first here).
        std::vector<std::filesystem::path> entryFolders;
        for (auto root = m_roots.mods.rbegin(); root != m_roots.mods.rend(); ++root)
        {
            if (std::shared_ptr<GameFile> file = openInMod(*root, normalized))
                return entryFolders.empty() ? file : withEntries(normalized, std::move(file), {entryFolders.rbegin(), entryFolders.rend()});
            if (std::filesystem::path folder = entryFolderIn(*root, normalized); !folder.empty())
                entryFolders.push_back(std::move(folder));
        }
        std::shared_ptr<GameFile> file = openBelowMods(normalized);
        if (!file || entryFolders.empty())
            return file;
        return withEntries(normalized, std::move(file), {entryFolders.rbegin(), entryFolders.rend()});
    }

    std::shared_ptr<GameFile> GameFileSystem::openBelowMods(const std::string &normalized)
    {
        {
            std::string name;
            if (std::shared_ptr<const std::vector<uint8_t>> bytes = provided(normalized, &name))
            {
                auto file = std::make_shared<MemoryFile>(std::move(bytes));
                file->m_origin = GameFile::Origin::Provided;
                file->m_source = (name.empty() ? std::string("memory") : name) + ":" + normalized;
                return file;
            }
        }
        if (!m_roots.disc.empty())
        {
            const std::filesystem::path path = resolveHost(m_roots.disc, normalized);
            std::shared_ptr<GameFile> file = path.empty() ? nullptr : openHost(path);
            if (file)
                file->m_source = path.string();
            return file;
        }
        if (m_roots.image)
        {
            const std::optional<zx::disc::IsoEntry> entry = m_roots.image->find(normalized);
            if (!entry || entry->directory)
                return nullptr;
            auto file = std::make_shared<ImageFile>(m_roots.image,
                                                    static_cast<uint64_t>(entry->lsn) * zx::disc::Iso9660Image::kSectorSize,
                                                    entry->size);
            file->m_source = m_roots.image->path().string() + ":" + normalized;
            return file;
        }
        return nullptr;
    }

    std::shared_ptr<GameFile> GameFileSystem::open(std::string_view path)
    {
        const std::string normalized = normalizePath(path);
        if (normalized.empty())
            return nullptr;
        std::shared_ptr<GameFile> file = openOriginal(normalized);
        if (!file)
            return nullptr;
        // A head for this path (registerHead), made once for the file found.
        Head make;
        {
            std::lock_guard<std::mutex> lock(providerMutex());
            for (const HeadEntry &entry : heads())
                if (entry.path == normalized)
                    make = entry.make;
        }
        if (make)
        {
            const std::string key = normalized + "\n" + file->source() + "\n" + std::to_string(file->size());
            std::shared_ptr<const std::vector<uint8_t>> head;
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(madeArchivesMutex());
                if (auto it = madeHeads().find(key); it != madeHeads().end())
                {
                    head = it->second;
                    known = true;
                }
            }
            if (!known)
            {
                head = make(*file);
                std::lock_guard<std::mutex> lock(madeArchivesMutex());
                madeHeads()[key] = head;
            }
            if (head)
            {
                auto headed = std::make_shared<HeadFile>(head, file);
                headed->m_origin = GameFile::Origin::Patched;
                headed->m_source = file->source() + " (its first " + std::to_string(head->size()) + " bytes made)";
                return headed;
            }
        }
        // An in-memory patch for this file name (a game override's script
        // edits, say), made from whichever file was found.
        if (zx::disc_patch::hasPatch(normalized))
        {
            std::shared_ptr<const std::vector<uint8_t>> patched =
                zx::disc_patch::contents(normalized, [&](std::vector<uint8_t> &bytes) { return readAll(*file, bytes); });
            if (patched)
            {
                auto memory = std::make_shared<MemoryFile>(std::move(patched));
                memory->m_origin = GameFile::Origin::Patched;
                memory->m_source = file->source() + " (patched)";
                return memory;
            }
        }
        return file;
    }

    bool GameFileSystem::exists(std::string_view path)
    {
        const std::string normalized = normalizePath(path);
        if (normalized.empty())
            return !m_roots.disc.empty() || m_roots.image != nullptr;
        if (inMemory(normalized, nullptr) || directoryInMemory(normalized))
            return true;
        for (const std::filesystem::path &root : m_roots.mods)
        {
            if (const ZipPath &zip = zipRoot(root); zip.zip)
            {
                const std::string inside = zip.inside.empty() ? normalized : zip.inside + "/" + normalized;
                if (zip.zip->hasFile(inside) || zip.zip->hasFolder(inside))
                    return true;
                continue;
            }
            if (!resolveHost(root, normalized).empty() || !resolveHost(root, normalized, true).empty())
                return true;
        }
        if (provided(normalized, nullptr))
            return true;
        if (!m_roots.disc.empty())
            return !resolveHost(m_roots.disc, normalized).empty() || !resolveHost(m_roots.disc, normalized, true).empty();
        return m_roots.image && m_roots.image->find(normalized).has_value();
    }
}
