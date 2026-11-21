// Disc images and their files (collection/extras/disc_image.h): a plain
// image read as it is, an RVZ by its magic (rvz_image.cpp), and what the
// setup does with a disc's files (find, select, extract).
#include "collection/extras/disc_image.h"
#include "rvz_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <system_error>

namespace zx::app::extras
{
    namespace
    {
        char lowerAscii(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string lowerText(std::string_view text)
        {
            std::string out(text);
            for (char &c : out)
                c = lowerAscii(c);
            return out;
        }

        // A plain GCM/ISO (or XISO): the disc's bytes as the file holds them.
        class PlainImage final : public DiscImage
        {
        public:
            explicit PlainImage(std::unique_ptr<ImageFile> file) : m_file(std::move(file)) {}
            uint64_t size() const override { return m_file->size(); }
            bool read(uint64_t offset, size_t count, uint8_t *out) override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_file->readAt(offset, count, out);
            }
            std::string describe() const override { return "plain image, " + std::to_string(size()) + " bytes"; }

        private:
            std::unique_ptr<ImageFile> m_file;
            std::mutex m_mutex;
        };

        bool seekTo(std::FILE *file, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        // Python's fnmatch.fnmatchcase on lower-case text: `*` any run (also
        // across '/'), `?` one character, `[...]` a set (`!` first negates,
        // `a-z` a range); a `[` with no closing `]` is itself.
        bool fnmatchCase(std::string_view text, std::string_view pattern)
        {
            size_t t = 0, p = 0;
            size_t starP = std::string_view::npos, starT = 0;
            while (t < text.size())
            {
                if (p < pattern.size())
                {
                    const char c = pattern[p];
                    if (c == '*')
                    {
                        starP = p++;
                        starT = t;
                        continue;
                    }
                    if (c == '?')
                    {
                        ++p;
                        ++t;
                        continue;
                    }
                    if (c == '[')
                    {
                        size_t end = p + 1;
                        if (end < pattern.size() && pattern[end] == '!')
                            ++end;
                        if (end < pattern.size() && pattern[end] == ']')
                            ++end;
                        while (end < pattern.size() && pattern[end] != ']')
                            ++end;
                        if (end < pattern.size())
                        {
                            size_t at = p + 1;
                            const bool negate = pattern[at] == '!';
                            if (negate)
                                ++at;
                            bool in = false;
                            for (size_t i = at; i < end; ++i)
                            {
                                if (i + 2 < end && pattern[i + 1] == '-')
                                {
                                    in = in || (text[t] >= pattern[i] && text[t] <= pattern[i + 2]);
                                    i += 2;
                                }
                                else
                                    in = in || text[t] == pattern[i];
                            }
                            if (in != negate)
                            {
                                p = end + 1;
                                ++t;
                                continue;
                            }
                        }
                        else if (text[t] == '[')
                        {
                            ++p;
                            ++t;
                            continue;
                        }
                    }
                    else if (c == text[t])
                    {
                        ++p;
                        ++t;
                        continue;
                    }
                }
                if (starP == std::string_view::npos)
                    return false;
                p = starP + 1;
                t = ++starT;
            }
            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            return p == pattern.size();
        }
    }

    std::unique_ptr<ImageFile> ImageFile::open(const std::filesystem::path &path, std::string *error)
    {
#if defined(_WIN32)
        std::FILE *file = _wfopen(path.c_str(), L"rb");
#else
        std::FILE *file = std::fopen(path.c_str(), "rb");
#endif
        if (!file)
        {
            *error = "The disc image " + path.filename().string() + " cannot be read.";
            return nullptr;
        }
        return adopt(file, path.filename().string(), error);
    }

    std::unique_ptr<ImageFile> ImageFile::adopt(std::FILE *file, std::string name, std::string *error)
    {
        std::unique_ptr<ImageFile> image(new ImageFile());
        image->m_file = file;
        image->m_name = std::move(name);
        if (!file || std::fseek(file, 0, SEEK_END) != 0)
        {
            *error = "The disc image " + image->m_name + " cannot be read.";
            return nullptr;
        }
#if defined(_WIN32)
        const long long end = _ftelli64(file);
#else
        const long long end = ftello(file);
#endif
        if (end < 0)
        {
            *error = "The disc image " + image->m_name + " cannot be read.";
            return nullptr;
        }
        image->m_size = static_cast<uint64_t>(end);
        return image;
    }

    ImageFile::~ImageFile()
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool ImageFile::readAt(uint64_t offset, size_t count, uint8_t *out)
    {
        if (offset > m_size || count > m_size - offset)
            return false;
        return seekTo(m_file, offset) && std::fread(out, 1, count, m_file) == count;
    }

    std::unique_ptr<DiscImage> openDiscImage(const std::filesystem::path &path, std::string *error)
    {
        std::unique_ptr<ImageFile> file = ImageFile::open(path, error);
        return file ? openDiscImage(std::move(file), error) : nullptr;
    }

    std::unique_ptr<DiscImage> openDiscImage(std::unique_ptr<ImageFile> file, std::string *error)
    {
        char magic[4] = {};
        if (!file->readAt(0, 4, reinterpret_cast<uint8_t *>(magic)))
        {
            *error = "The disc image " + file->name() + " cannot be read.";
            return nullptr;
        }
        if (std::memcmp(magic, "RVZ\x01", 4) == 0 || std::memcmp(magic, "WIA\x01", 4) == 0)
            return openRvzImage(std::move(file), error);
        return std::make_unique<PlainImage>(std::move(file));
    }

    bool globMatches(std::string_view text, std::string_view pattern) { return fnmatchCase(text, pattern); }

    const DiscFile *DiscFiles::find(std::string_view path) const
    {
        const std::string wanted = lowerText(path);
        for (const DiscFile &file : m_files)
            if (lowerText(file.path) == wanted)
                return &file;
        return nullptr;
    }

    std::vector<const DiscFile *> DiscFiles::select(const std::vector<std::string> &patterns) const
    {
        return selectFiles(m_files, patterns);
    }

    bool DiscFiles::streamFile(const DiscFile &file, const std::function<bool(const uint8_t *, size_t)> &sink)
    {
        constexpr size_t kPiece = 4u << 20;
        Bytes piece(static_cast<size_t>(std::min<uint64_t>(file.size, kPiece)));
        for (uint64_t done = 0; done < file.size;)
        {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(file.size - done, kPiece));
            if (!m_image->read(file.offset + done, count, piece.data()) || !sink(piece.data(), count))
                return false;
            done += count;
        }
        return true;
    }

    bool DiscFiles::readFile(const DiscFile &file, Bytes &out)
    {
        out.resize(static_cast<size_t>(file.size));
        return file.size == 0 || m_image->read(file.offset, out.size(), out.data());
    }

    bool pathMatches(std::string_view path, std::string_view pattern)
    {
        const std::string low = lowerText(path);
        std::string_view p = pattern;
        while (!p.empty() && p.front() == '/')
            p.remove_prefix(1);
        while (!p.empty() && p.back() == '/')
            p.remove_suffix(1);
        const std::string pat = lowerText(p);
        if (low == pat || (low.size() > pat.size() && low.compare(0, pat.size(), pat) == 0 && low[pat.size()] == '/'))
            return true;
        return fnmatchCase(low, pat);
    }

    std::vector<const DiscFile *> selectFiles(const std::vector<DiscFile> &files,
                                              const std::vector<std::string> &patterns)
    {
        std::vector<const DiscFile *> chosen;
        for (const DiscFile &file : files)
            for (const std::string &pattern : patterns)
                if (pathMatches(file.path, pattern))
                {
                    chosen.push_back(&file);
                    break;
                }
        return chosen;
    }

    bool extractFiles(DiscFiles &disc, const std::vector<const DiscFile *> &files, const std::filesystem::path &folder,
                      std::string *error)
    {
        for (const DiscFile *file : files)
        {
            const std::filesystem::path relative = std::filesystem::path(file->path).lexically_normal();
            if (relative.is_absolute() || relative.empty() || *relative.begin() == "..")
            {
                *error = "The disc names a file outside its folders (" + file->path + ").";
                return false;
            }
            const std::filesystem::path target = folder / relative;
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            std::filesystem::path partial = target;
            partial += ".partial";
            {
                std::ofstream out(partial, std::ios::binary | std::ios::trunc);
                const bool read = disc.streamFile(*file, [&](const uint8_t *data, size_t size)
                {
                    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
                    return static_cast<bool>(out);
                });
                out.close();
                if (!read || !out)
                {
                    std::filesystem::remove(partial, ec);
                    *error = "Could not copy " + file->path + " from the disc image.";
                    return false;
                }
            }
            std::filesystem::rename(partial, target, ec);
            if (ec)
            {
                *error = "Could not write " + target.string() + ": " + ec.message() + ".";
                return false;
            }
        }
        return true;
    }
}
