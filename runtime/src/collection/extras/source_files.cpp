// The files a converter reads (collection/extras/source_files.h).
#include "collection/extras/source_files.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace ps2x::app::extras
{
    namespace
    {
        std::vector<std::string_view> components(std::string_view path)
        {
            std::vector<std::string_view> out;
            size_t start = 0;
            for (size_t i = 0; i <= path.size(); ++i)
                if (i == path.size() || path[i] == '/')
                {
                    out.push_back(path.substr(start, i - start));
                    start = i + 1;
                }
            return out;
        }

        std::string generic(const std::filesystem::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }
    }

    std::string upperAscii(std::string_view text)
    {
        std::string out(text);
        for (char &c : out)
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        return out;
    }

    std::string lowerAscii(std::string_view text)
    {
        std::string out(text);
        for (char &c : out)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return out;
    }

    bool sourceOrderLess(std::string_view a, std::string_view b)
    {
        const std::vector<std::string_view> x = components(a), y = components(b);
        for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        {
            const bool xFile = i + 1 == x.size(), yFile = i + 1 == y.size();
            if (xFile != yFile)
                return xFile; // a folder's files come before its folders
            if (x[i] != y[i])
                return x[i] < y[i];
        }
        return x.size() < y.size();
    }

    void SourceFiles::setFiles(std::vector<std::string> files)
    {
        std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) { return sourceOrderLess(a, b); });
        m_files = std::move(files);
        m_byLower.clear();
        for (const std::string &file : m_files)
            m_byLower.emplace(lowerAscii(file), file);
    }

    std::optional<std::string> SourceFiles::find(std::string_view path) const
    {
        const auto it = m_byLower.find(lowerAscii(path));
        if (it == m_byLower.end())
            return std::nullopt;
        return it->second;
    }

    bool SourceFiles::hasFolder(std::string_view path) const
    {
        std::string prefix = lowerAscii(path);
        if (!prefix.empty() && prefix.back() != '/')
            prefix += '/';
        const auto it = m_byLower.lower_bound(prefix);
        return it != m_byLower.end() && it->first.compare(0, prefix.size(), prefix) == 0;
    }

    bool SourceFiles::readAny(std::string_view path, Bytes &out) const
    {
        const std::optional<std::string> found = find(path);
        return found && read(*found, out);
    }

    bool FolderFiles::open(const std::filesystem::path &root, const std::vector<std::string> &markers)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            return false;
        // The candidates: the folder itself, its assets, its media/assets (any case).
        auto child = [&](const std::filesystem::path &folder, std::string_view name) -> std::filesystem::path
        {
            for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
                if (lowerAscii(generic(entry.path().filename())) == name && entry.is_directory(ec))
                    return entry.path();
            return {};
        };
        std::vector<std::filesystem::path> candidates = {root};
        if (std::filesystem::path assets = child(root, "assets"); !assets.empty())
            candidates.push_back(assets);
        if (std::filesystem::path media = child(root, "media"); !media.empty())
            if (std::filesystem::path assets = child(media, "assets"); !assets.empty())
                candidates.push_back(assets);
        for (const std::filesystem::path &candidate : candidates)
        {
            bool marked = markers.empty();
            for (const std::string &marker : markers)
                marked = marked || !child(candidate, lowerAscii(marker)).empty();
            if (!marked)
                continue;
            m_assets = candidate;
            std::vector<std::string> files;
            for (auto it = std::filesystem::recursive_directory_iterator(candidate, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec))
                    files.push_back(generic(it->path().lexically_relative(candidate)));
            setFiles(std::move(files));
            return true;
        }
        return false;
    }

    bool FolderFiles::read(const std::string &path, Bytes &out) const
    {
        std::ifstream in(m_assets / std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    uint64_t FolderFiles::size(const std::string &path) const
    {
        std::error_code ec;
        const uint64_t bytes =
            std::filesystem::file_size(m_assets / std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
        return ec ? 0 : bytes;
    }

    bool ImageFiles::open(DiscFiles &disc, std::string_view prefix)
    {
        m_disc = &disc;
        m_entries.clear();
        const std::string lowPrefix = lowerAscii(prefix);
        std::vector<std::string> files;
        for (const DiscFile &file : disc.files())
            if (lowerAscii(file.path).compare(0, lowPrefix.size(), lowPrefix) == 0 && file.path.size() > lowPrefix.size())
            {
                std::string relative = file.path.substr(lowPrefix.size());
                m_entries.emplace(relative, &file);
                files.push_back(std::move(relative));
            }
        setFiles(std::move(files));
        return !m_entries.empty();
    }

    bool ImageFiles::read(const std::string &path, Bytes &out) const
    {
        const auto it = m_entries.find(path);
        return it != m_entries.end() && m_disc->readFile(*it->second, out);
    }

    uint64_t ImageFiles::size(const std::string &path) const
    {
        const auto it = m_entries.find(path);
        return it == m_entries.end() ? 0 : it->second->size;
    }

    bool writeFile(const std::filesystem::path &path, std::string_view text, std::string *error)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path partial = path;
        partial += ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(text.data(), std::streamsize(text.size()));
            if (!out)
            {
                *error = "Could not write " + path.string() + ".";
                return false;
            }
        }
        std::filesystem::rename(partial, path, ec);
        if (ec)
        {
            *error = "Could not write " + path.string() + ": " + ec.message() + ".";
            return false;
        }
        return true;
    }

    bool writeFile(const std::filesystem::path &path, const Bytes &data, std::string *error)
    {
        return writeFile(path, std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), error);
    }
}
