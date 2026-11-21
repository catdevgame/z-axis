#include "runtime/xbox_disc.h"
#include "runtime/boot_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace ps2_xbox_disc
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        // "levels\\ZOO/zoo.TXR" -> {"LEVELS", "ZOO", "ZOO.TXR"}.
        std::vector<std::string> components(const std::string &path)
        {
            std::vector<std::string> out;
            std::string part;
            for (char c : path)
            {
                if (c == '/' || c == '\\')
                {
                    if (!part.empty() && part != ".")
                        out.push_back(upper(part));
                    part.clear();
                }
                else
                {
                    part.push_back(c);
                }
            }
            if (!part.empty() && part != ".")
                out.push_back(upper(part));
            return out;
        }

        class Disc
        {
        public:
            virtual ~Disc() = default;
            virtual bool read(const std::vector<std::string> &path, std::vector<uint8_t> &out) = 0;
            virtual bool exists(const std::vector<std::string> &path) = 0;
            virtual std::optional<std::vector<std::string>> list(const std::vector<std::string> &path) = 0;
            virtual fs::path hostPath(const std::vector<std::string> &path) = 0;
            std::string description;
        };

        // An extracted folder (any case on a case-sensitive file system).
        class FolderDisc final : public Disc
        {
        public:
            explicit FolderDisc(fs::path assets) : m_assets(std::move(assets)) {}

            bool read(const std::vector<std::string> &path, std::vector<uint8_t> &out) override
            {
                const fs::path host = resolve(path);
                std::error_code ec;
                if (host.empty() || !fs::is_regular_file(host, ec))
                    return false;
                std::ifstream in(host, std::ios::binary);
                if (!in)
                    return false;
                const uint64_t size = fs::file_size(host, ec);
                if (ec)
                    return false;
                out.resize(static_cast<size_t>(size));
                in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
                return static_cast<uint64_t>(in.gcount()) == size;
            }

            bool exists(const std::vector<std::string> &path) override { return !resolve(path).empty(); }

            fs::path hostPath(const std::vector<std::string> &path) override
            {
                const fs::path host = resolve(path);
                std::error_code ec;
                return !host.empty() && fs::is_regular_file(host, ec) ? host : fs::path();
            }

            std::optional<std::vector<std::string>> list(const std::vector<std::string> &path) override
            {
                const fs::path host = resolve(path);
                std::error_code ec;
                if (host.empty() || !fs::is_directory(host, ec))
                    return std::nullopt;
                std::vector<std::string> names;
                for (const auto &entry : listing(host))
                    names.push_back(entry.second);
                return names;
            }

        private:
            const std::map<std::string, std::string> &listing(const fs::path &dir)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_listings.find(dir.string());
                if (it == m_listings.end())
                {
                    std::map<std::string, std::string> names;
                    std::error_code ec;
                    for (fs::directory_iterator i(dir, ec), end; !ec && i != end; i.increment(ec))
                    {
                        const std::string name = i->path().filename().string();
                        names.emplace(upper(name), name);
                    }
                    it = m_listings.emplace(dir.string(), std::move(names)).first;
                }
                return it->second;
            }

            fs::path resolve(const std::vector<std::string> &path)
            {
                fs::path current = m_assets;
                for (const std::string &part : path)
                {
                    if (part == "..")
                        return {};
                    const auto &names = listing(current);
                    const auto found = names.find(part);
                    if (found == names.end())
                        return {};
                    current /= found->second;
                }
                return current;
            }

            fs::path m_assets;
            std::mutex m_mutex;
            std::unordered_map<std::string, std::map<std::string, std::string>> m_listings;
        };

        std::unique_ptr<Disc> openFolder(const fs::path &path, std::string &why)
        {
            std::error_code ec;
            // The disc root, the media folder or the assets folder, any case.
            auto child = [&](const fs::path &dir, const char *name) -> fs::path
            {
                for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                    if (it->is_directory(ec) && upper(it->path().filename().string()) == name)
                        return it->path();
                return {};
            };
            fs::path assets;
            if (const fs::path media = child(path, "MEDIA"); !media.empty())
                assets = child(media, "ASSETS");
            if (assets.empty())
                assets = child(path, "ASSETS");
            if (assets.empty() && upper(path.filename().string()) == "ASSETS")
                assets = path;
            if (assets.empty() || !fs::is_directory(assets, ec))
            {
                why = path.string() + ": no media/assets folder in it";
                return nullptr;
            }
            auto disc = std::make_unique<FolderDisc>(assets);
            disc->description = "Xbox files in " + path.string();
            return disc;
        }

        std::unique_ptr<Disc> openAny(const std::string &text, std::string &why)
        {
            const fs::path path(std::u8string(text.begin(), text.end())); // (UTF-8 text; fs::u8path is deprecated in C++20)
            std::error_code ec;
            if (fs::is_directory(path, ec))
                return openFolder(path, why);
            why = "not a folder: " + text;
            return nullptr;
        }

        struct State
        {
            std::mutex mutex;
            bool initialized = false;
            uint64_t boot = 0; // the boot that opened it (each its own ZX_XBOX_DIR)
            std::shared_ptr<Disc> disc;
            std::string description = "no Xbox files";
        };

        State &state()
        {
            static State instance;
            return instance;
        }

        std::shared_ptr<Disc> current()
        {
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            return s.disc;
        }
    }

    bool init()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.initialized && s.boot == zx::boot::generation())
            return s.disc != nullptr;
        s.initialized = true;
        s.boot = zx::boot::generation();
        s.disc.reset();
        s.description = "no Xbox files";
        std::string path;
        if (const char *env = zx::boot::text("ZX_XBOX_DIR"); env && env[0])
            path = env;
        if (path.empty())
        {
            s.description = "no Xbox files (ZX_XBOX_DIR; the app: Resources/xbox)";
            return false;
        }
        std::string why;
        std::unique_ptr<Disc> disc = openAny(path, why);
        if (!disc)
        {
            s.description = "Xbox files not usable: " + why;
            return false;
        }
        s.description = disc->description;
        s.disc = std::move(disc);
        return true;
    }

    bool openPath(const std::string &path)
    {
        std::string why;
        std::unique_ptr<Disc> disc = openAny(path, why);
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.initialized = true;
        s.boot = zx::boot::generation();
        s.disc.reset();
        if (!disc)
        {
            s.description = "Xbox files not usable: " + why;
            return false;
        }
        s.description = disc->description;
        s.disc = std::move(disc);
        return true;
    }

    void reset()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.initialized = false;
        s.disc.reset();
        s.description = "no Xbox files";
    }

    bool available()
    {
        return current() != nullptr;
    }

    bool exists(const std::string &path)
    {
        const std::shared_ptr<Disc> disc = current();
        return disc && disc->exists(components(path));
    }

    bool readFile(const std::string &path, std::vector<uint8_t> &out)
    {
        const std::shared_ptr<Disc> disc = current();
        return disc && disc->read(components(path), out);
    }

    std::string hostPath(const std::string &path)
    {
        const std::shared_ptr<Disc> disc = current();
        if (!disc)
            return {};
        const std::u8string text = disc->hostPath(components(path)).u8string();
        return std::string(text.begin(), text.end());
    }

    std::vector<std::string> list(const std::string &dir)
    {
        const std::shared_ptr<Disc> disc = current();
        if (!disc)
            return {};
        return disc->list(components(dir)).value_or(std::vector<std::string>{});
    }

    std::string description()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.description;
    }
}
