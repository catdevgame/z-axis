// The player's saves and settings in one file (collection/save_transfer.h).
#include "collection/save_transfer.h"
#include "collection/game_setup.h"
#include "strings.h"

#include <miniz.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace zx::app
{
    namespace fs = std::filesystem;

    namespace
    {
        // The export's first entry: what it is (an import takes no other zip).
        constexpr const char *kMarkerName = "z-axis-saves.txt";
        constexpr const char *kMarkerText = "z-axis saves 1\n";
        constexpr uint64_t kMaxFile = 1ull << 31; // (one file in memory at a time)

        std::vector<std::string> partsOf(const std::string &path)
        {
            std::vector<std::string> parts;
            std::stringstream in(path);
            for (std::string part; std::getline(in, part, '/');)
                if (!part.empty())
                    parts.push_back(part);
            return parts;
        }

        bool readWhole(const fs::path &file, std::vector<uint8_t> &bytes)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            bytes.assign(std::istreambuf_iterator<char>(in), {});
            return !in.bad();
        }

        // An entry's name as a place in the support folder: relative, no
        // way out of it, no drive or backslash (another system's zip).
        bool safeName(const std::string &name)
        {
            if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
                return false;
            for (const std::string &part : partsOf(name))
                if (part == "." || part == "..")
                    return false;
            return true;
        }
    }

    bool savedInTransfer(const std::string &path)
    {
        const std::vector<std::string> parts = partsOf(path);
        if (parts.empty())
            return false;
        const std::string first = lower(parts.front()), last = lower(parts.back());
        // Logs, and caches made again: the GPU pipelines' list, made game code.
        if (first == "logs" || first == "code" || last == "gpu_pipelines.txt" || last == kMarkerName)
            return false;
        // A game's files from its disc (games/<id>/disc..., its manifest; a
        // single game's app has them at the top): set up again from the disc.
        if ((parts.size() >= 3 && first == "games" && lower(parts[2]).rfind("disc", 0) == 0) || first.rfind("disc", 0) == 0)
            return false;
        return true;
    }

    TransferResult exportSaves(const fs::path &supportDir, const TransferWrite &write)
    {
        TransferResult result;
        std::vector<std::pair<std::string, fs::path>> files; // name in the zip, the file
        std::error_code ec;
        for (fs::recursive_directory_iterator it(supportDir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            const std::u8string relative = fs::relative(it->path(), supportDir, ec).generic_u8string();
            const std::string name(relative.begin(), relative.end());
            if (!ec && savedInTransfer(name))
                files.emplace_back(name, it->path());
        }
        std::sort(files.begin(), files.end());
        if (files.empty())
        {
            result.message = "There are no saves or settings to export yet.";
            return result;
        }

        mz_zip_archive zip{};
        zip.m_pWrite = [](void *opaque, mz_uint64 offset, const void *data, size_t size) -> size_t
        { return (*static_cast<const TransferWrite *>(opaque))(offset, data, size) ? size : 0u; };
        zip.m_pIO_opaque = const_cast<TransferWrite *>(&write);
        if (!mz_zip_writer_init(&zip, 0))
        {
            result.message = "The export could not start.";
            return result;
        }
        bool ok = mz_zip_writer_add_mem(&zip, kMarkerName, kMarkerText, std::char_traits<char>::length(kMarkerText),
                                        MZ_DEFAULT_COMPRESSION);
        for (const auto &[name, file] : files)
        {
            std::vector<uint8_t> bytes;
            if (!ok)
                break;
            if (!readWhole(file, bytes))
            {
                result.message = name + " cannot be read.";
                mz_zip_writer_end(&zip);
                return result;
            }
            ok = mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION);
            ++result.files;
            result.bytes += bytes.size();
        }
        ok = ok && mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
        if (!ok)
        {
            result.message = "The export could not be written. Check the free space where it goes.";
            return result;
        }
        result.ok = true;
        result.message = "Exported " + std::to_string(result.files) + " files (" + sizeText(result.bytes) + ").";
        return result;
    }

    TransferResult importSaves(const fs::path &supportDir, uint64_t size, const TransferRead &read)
    {
        TransferResult result;
        mz_zip_archive zip{};
        zip.m_pRead = [](void *opaque, mz_uint64 offset, void *data, size_t count) -> size_t
        { return (*static_cast<const TransferRead *>(opaque))(offset, data, count) ? count : 0u; };
        zip.m_pIO_opaque = const_cast<TransferRead *>(&read);
        if (!mz_zip_reader_init(&zip, size, 0))
        {
            result.message = "This file is not a saves export of the app.";
            return result;
        }
        const int marker = mz_zip_reader_locate_file(&zip, kMarkerName, nullptr, 0);
        if (marker < 0)
        {
            mz_zip_reader_end(&zip);
            result.message = "This file is not a saves export of the app.";
            return result;
        }
        // Every name first: a file that would land outside the support
        // folder (or nothing to take) stops the import before it writes.
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        std::vector<std::pair<mz_uint, std::string>> entries;
        for (mz_uint i = 0; i < count; ++i)
        {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory || static_cast<int>(i) == marker)
                continue;
            const std::string name = stat.m_filename;
            if (!safeName(name) || stat.m_uncomp_size > kMaxFile)
            {
                mz_zip_reader_end(&zip);
                result.message = "This export holds a file the app does not take (" + name + "). Nothing was imported.";
                return result;
            }
            if (savedInTransfer(name))
                entries.emplace_back(i, name);
        }
        for (const auto &[index, name] : entries)
        {
            size_t bytes = 0;
            void *data = mz_zip_reader_extract_to_heap(&zip, index, &bytes, 0);
            const fs::path target = supportDir / pathFromUtf8(name);
            const fs::path partial = target.parent_path() / (target.filename().string() + ".import");
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            bool written = false;
            if (data)
            {
                std::ofstream out(partial, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
                out.close();
                written = static_cast<bool>(out);
                mz_free(data);
            }
            if (written)
                fs::rename(partial, target, ec);
            if (!written || ec)
            {
                fs::remove(partial, ec);
                mz_zip_reader_end(&zip);
                result.message = "Importing " + name + " failed after " + std::to_string(result.files) + " files.";
                return result;
            }
            ++result.files;
            result.bytes += bytes;
        }
        mz_zip_reader_end(&zip);
        result.ok = true;
        result.message = "Imported " + std::to_string(result.files) + " files (" + sizeText(result.bytes) + ").";
        return result;
    }
}
