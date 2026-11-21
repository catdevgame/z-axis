// Zips read in place (runtime/ps2_zip_archive.h), through miniz.
#include "runtime/ps2_zip_archive.h"

#include "runtime/ps2_game_files.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <system_error>

namespace zx::game_files
{
    struct ZipArchive::Reader
    {
        mz_zip_archive zip{};
        std::FILE *stream = nullptr;
        bool open = false;

        ~Reader()
        {
            if (open)
                mz_zip_reader_end(&zip);
            if (stream)
                std::fclose(stream);
        }
    };

    namespace
    {
        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // The zips open now, by path and what the file was when opened, so a
        // zip replaced on disk is read again.
        struct Opened
        {
            uint64_t size = 0;
            std::filesystem::file_time_type time{};
            std::weak_ptr<ZipArchive> zip;
        };
        std::mutex &openedMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        std::map<std::string, Opened> &opened()
        {
            static std::map<std::string, Opened> zips;
            return zips;
        }

        // Left out of a zip's files: what the Mac adds when it zips a folder.
        bool leftOut(const std::string &name)
        {
            if (name.compare(0, 9, "__MACOSX/") == 0)
                return true;
            const size_t slash = name.rfind('/');
            const std::string base = slash == std::string::npos ? name : name.substr(slash + 1u);
            return base == ".DS_Store";
        }
    }

    ZipArchive::~ZipArchive() = default;

    namespace
    {
        // A stored name with '/' between its parts, no empty or "." parts;
        // "" for a folder's entry or a name with ".." in it (never read: it
        // would name a file outside the zip's root).
        std::string clean(std::string name)
        {
            std::replace(name.begin(), name.end(), '\\', '/');
            if (!name.empty() && name.back() == '/')
                return {};
            std::string out;
            size_t start = 0;
            while (start <= name.size())
            {
                size_t end = name.find('/', start);
                if (end == std::string::npos)
                    end = name.size();
                const std::string part = name.substr(start, end - start);
                if (part == "..")
                    return {};
                if (!part.empty() && part != ".")
                    out += (out.empty() ? "" : "/") + part;
                start = end + 1u;
            }
            return out;
        }
    }

    std::shared_ptr<ZipArchive> ZipArchive::open(const std::filesystem::path &file, std::string *error)
    {
        auto fail = [error](const std::string &why) -> std::shared_ptr<ZipArchive>
        {
            if (error)
                *error = why;
            return nullptr;
        };
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return fail("no such file");
        const uint64_t size = std::filesystem::file_size(file, ec);
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, ec);
        const std::string key = std::filesystem::absolute(file, ec).lexically_normal().string();
        std::lock_guard<std::mutex> lock(openedMutex());
        if (auto it = opened().find(key); it != opened().end() && it->second.size == size && it->second.time == time)
            if (std::shared_ptr<ZipArchive> zip = it->second.zip.lock())
                return zip;

        std::shared_ptr<ZipArchive> zip(new ZipArchive());
        zip->m_file = file;
        zip->m_reader = std::make_unique<Reader>();
#if defined(_WIN32)
        zip->m_reader->stream = ::_wfopen(file.c_str(), L"rb");
#else
        zip->m_reader->stream = std::fopen(file.c_str(), "rb");
#endif
        if (!zip->m_reader->stream)
            return fail("cannot be opened");
        if (!mz_zip_reader_init_cfile(&zip->m_reader->zip, zip->m_reader->stream, size, 0))
            return fail(std::string("not a zip that can be read (") +
                        mz_zip_get_error_string(mz_zip_get_last_error(&zip->m_reader->zip)) + ")");
        zip->m_reader->open = true;

        // Its files, and the one folder all of them are in, if there is one.
        struct Found
        {
            uint32_t index;
            uint64_t size;
            std::string name;
        };
        std::vector<Found> found;
        const mz_uint count = mz_zip_reader_get_num_files(&zip->m_reader->zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip->m_reader->zip, i, &stat) || stat.m_is_directory)
                continue;
            std::string name = clean(stat.m_filename);
            if (name.empty() || leftOut(name))
                continue;
            found.push_back({static_cast<uint32_t>(i), static_cast<uint64_t>(stat.m_uncomp_size), std::move(name)});
        }
        std::string top;
        bool oneFolder = !found.empty();
        for (const Found &f : found)
        {
            const size_t slash = f.name.find('/');
            const std::string first = slash == std::string::npos ? std::string() : f.name.substr(0, slash + 1u);
            if (first.empty() || (!top.empty() && lower(first) != lower(top)))
            {
                oneFolder = false;
                break;
            }
            top = first;
        }
        for (Found &f : found)
        {
            std::string name = oneFolder ? f.name.substr(top.size()) : f.name;
            const std::string normalized = normalizePath(name);
            if (normalized.empty())
                continue;
            zip->m_entries.emplace(normalized, Entry{f.index, f.size, std::move(name)});
        }
        opened()[key] = Opened{size, time, zip};
        return zip;
    }

    bool ZipArchive::hasFile(const std::string &normalized) const
    {
        return m_entries.count(normalized) != 0u;
    }

    bool ZipArchive::hasFolder(const std::string &normalized) const
    {
        if (normalized.empty())
            return true;
        const std::string prefix = normalized + "/";
        auto it = m_entries.lower_bound(prefix);
        return it != m_entries.end() && it->first.compare(0, prefix.size(), prefix) == 0;
    }

    uint64_t ZipArchive::fileSize(const std::string &normalized) const
    {
        auto it = m_entries.find(normalized);
        return it == m_entries.end() ? 0u : it->second.size;
    }

    std::vector<std::string> ZipArchive::filesBelow(const std::string &normalizedFolder) const
    {
        std::vector<std::string> files;
        const std::string prefix = normalizedFolder.empty() ? std::string() : normalizedFolder + "/";
        for (auto it = m_entries.lower_bound(prefix); it != m_entries.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
        {
            // The name as stored, from where the folder ends (stored names
            // are clean: the folder's part of one is as long as the
            // normalized folder, case aside).
            const std::string &name = it->second.name;
            files.push_back(prefix.size() <= name.size() ? name.substr(prefix.size()) : name);
        }
        return files;
    }

    bool ZipArchive::read(const std::string &normalized, std::vector<uint8_t> &out) const
    {
        auto it = m_entries.find(normalized);
        if (it == m_entries.end())
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        out.resize(static_cast<size_t>(it->second.size));
        if (out.empty())
            return true;
        if (!mz_zip_reader_extract_to_mem(&m_reader->zip, it->second.index, out.data(), out.size(), 0))
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool isZipFile(const std::filesystem::path &path)
    {
        std::error_code ec;
        return lower(path.extension().string()) == ".zip" && std::filesystem::is_regular_file(path, ec);
    }

    ZipPath zipPathOf(const std::filesystem::path &path)
    {
        ZipPath out;
        std::error_code ec;
        if (path.empty() || std::filesystem::is_directory(path, ec))
            return out;
        std::filesystem::path walked;
        std::filesystem::path rest;
        bool inZip = false;
        for (const std::filesystem::path &part : path)
        {
            if (inZip)
            {
                rest /= part;
                continue;
            }
            walked /= part;
            if (isZipFile(walked))
                inZip = true;
        }
        if (!inZip)
            return out;
        out.zip = ZipArchive::open(walked);
        if (out.zip)
            out.inside = normalizePath(rest.generic_string());
        return out;
    }
}
