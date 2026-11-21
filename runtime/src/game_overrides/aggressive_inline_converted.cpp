#include "aggressive_inline_converted.h"

#include "runtime/ps2_texture_replace.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

namespace aggressive_inline::converted
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace tr = zx::texture_replace;

        const fs::path &folder()
        {
            static zx::boot::PerBoot<fs::path> path([]() -> fs::path
            {
                const char *env = zx::boot::text("ZX_XBOX_CONVERTED");
                std::error_code ec;
                if (env && env[0] && fs::is_directory(env, ec))
                    return env;
                return {};
            });
            return path.get();
        }

        bool readHost(const fs::path &file, std::vector<uint8_t> &out)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
                return false;
            in.seekg(0);
            out.resize(static_cast<size_t>(size));
            return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), size));
        }
    }

    bool available()
    {
        return !folder().empty();
    }

    std::string description()
    {
        if (available())
            return folder().string();
        return "no files made from the Xbox release (ZX_XBOX_CONVERTED; the app: Resources/xbox-converted)";
    }

    bool exists(const std::string &gamePath)
    {
        std::error_code ec;
        return available() && fs::is_regular_file(folder() / fs::path(gamePath), ec);
    }

    std::shared_ptr<const std::vector<uint8_t>> read(const std::string &gamePath)
    {
        if (!available())
            return nullptr;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (!readHost(folder() / fs::path(gamePath), *bytes))
            return nullptr;
        return bytes;
    }

    namespace
    {
        // Pictures made from one file shared by later ones (shareAlike), by
        // name, size and bytes.
        struct SharedPictures
        {
            std::mutex mutex;
            std::map<std::string, tr::PicturePtr> alike;
        };
    }

    Pictures registerPictures(const std::string &gamePath, const std::string &source, bool shareAlike)
    {
        Pictures result;
        std::vector<uint8_t> bytes;
        if (!available() || !readHost(folder() / "textures" / fs::path(gamePath + ".xtp"), bytes))
        {
            std::cerr << "[xbox-converted] no Xbox originals for " << gamePath << std::endl;
            return result;
        }
        std::vector<tr::Entry> entries;
        std::string error;
        if (!tr::parsePack(bytes, entries, &error))
        {
            std::cerr << "[xbox-converted] " << gamePath << ".xtp: " << error << std::endl;
            return result;
        }
        SharedPictures &shared = zx::session::state<SharedPictures>();
        std::lock_guard<std::mutex> lock(shared.mutex);
        std::map<std::string, tr::PicturePtr> &alike = shared.alike;
        std::set<const tr::Picture *> fresh;
        for (tr::Entry &e : entries)
        {
            if (shareAlike)
            {
                tr::PicturePtr &known = alike[e.picture->name + ":" + std::to_string(e.picture->width) + "x" +
                                             std::to_string(e.picture->height) + ":" +
                                             std::to_string(e.picture->data.size())];
                if (known)
                    e.picture = known;
                else
                {
                    known = e.picture;
                    fresh.insert(e.picture.get());
                }
            }
            else
                fresh.insert(e.picture.get());
            result.larger += e.picture->width > e.width || e.picture->height > e.height ? 1u : 0u;
        }
        for (const tr::Picture *p : fresh)
            result.newBytes += p->data.size();
        result.count = entries.size();
        result.ok = true;
        tr::setSource(source, std::move(entries));
        return result;
    }
}
