#include "native_textures.h"

#include "disc_files.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_boot_config.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace zge::native
{
    namespace tr = zx::texture_replace;
    namespace gf = zx::game_files;

    FileTextures texturesForFile(const std::string &relativePath, const std::vector<uint8_t> &ps2File,
                                 const std::vector<uint8_t> &xboxFile)
    {
        FileTextures out;
        const std::vector<LibraryTexture> ps2 = readTextureLibrary(ps2File);
        const std::vector<LibraryTexture> xbox = readTextureLibrary(xboxFile);
        out.ps2Textures = static_cast<size_t>(
            std::count_if(ps2.begin(), ps2.end(), [](const LibraryTexture &t) { return t.kind != "TXPR"; }));
        const std::vector<TexturePair> pairs = pairTextures(relativePath, ps2, xbox, &out.rejected);
        out.pairs = pairs.size();

        // One key per texture as the GS holds it; the PS2 mip levels show the
        // same picture (the GS samples them with the same coordinates).
        std::set<std::tuple<int, int, uint64_t>> seen;
        auto add = [&](const Ps2Picture &level, const tr::PicturePtr &picture)
        {
            if (std::all_of(level.indices.begin(), level.indices.end(), [&](uint8_t i) { return i == level.indices[0]; }))
                return;
            const uint64_t hash = indexHash(level);
            if (!seen.insert({level.width, level.height, hash}).second)
                return;
            tr::Entry e;
            e.width = static_cast<uint32_t>(level.width);
            e.height = static_cast<uint32_t>(level.height);
            e.indexHash = hash;
            e.picture = picture;
            out.entries.push_back(std::move(e));
        };
        for (const TexturePair &pair : pairs)
        {
            const Ps2Picture &base = pair.ps2->ps2;
            if (seen.count({base.width, base.height, indexHash(base)}))
                continue;
            auto picture = std::make_shared<tr::Picture>();
            picture->width = static_cast<uint32_t>(pair.xbox->xbox.width);
            picture->height = static_cast<uint32_t>(pair.xbox->xbox.height);
            picture->mips = static_cast<uint32_t>(pair.xbox->xbox.mips);
            picture->format = tr::Format::BC2;
            picture->data = pair.xbox->xbox.levels;
            picture->name = pair.ps2->name;
            add(base, picture);
            for (const Ps2Picture &level : pair.ps2->ps2Mips)
                add(level, picture);
        }
        return out;
    }

    namespace
    {
        struct Service
        {
            bool checked = false;
            bool on = false;
            int trace = 0;
            std::string pack; // a build-time pack's textures folder, if any
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<std::string> queue;
            std::set<std::string> seen;
        };

        Service &service()
        {
            static Service s;
            return s;
        }

        void load(Service &s, const std::string &normalized)
        {
            // "ASSETS/LEVELS/TUT/TUT.TXR" -> "LEVELS/TUT/TUT.TXR" below the
            // Xbox assets folder (looked up in any case).
            const std::string relative = normalized.substr(std::string("ASSETS/").size());
            std::vector<uint8_t> ps2, xbox;
            if (!readGameFile(normalized, ps2) || !ps2_xbox_disc::readFile(relative, xbox))
                return;
            const FileTextures textures = texturesForFile(relative, ps2, xbox);
            if (s.trace)
            {
                std::cout << "[native-textures] " << normalized << ": " << textures.pairs << " of "
                          << textures.ps2Textures << " textures from the Xbox file, " << textures.entries.size()
                          << " keys" << std::endl;
                for (const Rejected &r : textures.rejected)
                    std::cout << "[native-textures]   " << r.ps2 << " -> " << r.xbox << ": " << r.why << std::endl;
            }
            if (!textures.entries.empty())
                tr::setSource("native:" + normalized, textures.entries);
        }

        void worker()
        {
            Service &s = service();
            for (;;)
            {
                std::string path;
                {
                    std::unique_lock<std::mutex> lock(s.mutex);
                    s.cv.wait(lock, [&] { return !s.queue.empty(); });
                    path = std::move(s.queue.front());
                    s.queue.pop_front();
                }
                load(s, path);
            }
        }

        bool start(Service &s)
        {
            s.checked = true;
            // Opt-in (developers): the app's Xbox textures come from the
            // build-time pack.
            const char *on = std::getenv("ZX_NATIVE_TEXTURES");
            if (!on || on[0] != '1')
                return false;
            if (const char *v = std::getenv("ZX_TRACE_NATIVE_TEXTURES"))
                s.trace = std::atoi(v);
            if (!ps2_xbox_disc::init())
                return false;
            if (const char *pack = zx::boot::text("ZX_XBOX_PACK"); pack && *pack)
                s.pack = (std::filesystem::path(pack) / "textures").string();
            std::thread(worker).detach();
            std::cout << "[native-textures] the Xbox release's textures are read from " << ps2_xbox_disc::description()
                      << " as the game loads its libraries" << std::endl;
            return true;
        }
    }

    void noteTextureFile(const std::string &gamePath)
    {
        Service &s = service();
        if (!s.checked)
            s.on = start(s);
        if (!s.on)
            return;
        const std::string normalized = gf::normalizePath(gamePath);
        const bool library = normalized.size() > 4 &&
                             (normalized.compare(normalized.size() - 4, 4, ".TXR") == 0 ||
                              normalized.compare(normalized.size() - 4, 4, ".ZIT") == 0);
        if (!library || normalized.rfind("ASSETS/", 0) != 0)
            return;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.seen.insert(normalized).second)
            return;
        // A pack made at build time covers it already.
        std::error_code ec;
        if (!s.pack.empty() && std::filesystem::exists(std::filesystem::path(s.pack) / (normalized + ".xtp"), ec))
            return;
        s.queue.push_back(normalized);
        s.cv.notify_one();
    }
}
