#include "runtime/ps2_texture_pack.h"

#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_game_files.h"

#include <SDL3/SDL.h>

#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ps2x::texture_pack
{
    namespace
    {
        // ---- GS memory as PCSX2's texture reads see it ------------------------

        struct BlockSize
        {
            int32_t x, y;
        };

        // A block's size in texels (GSLocalMemory's psm_t::bs).
        BlockSize blockSize(uint32_t psm)
        {
            switch (psm)
            {
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
                return {16, 8};
            case GS_PSM_T8:
                return {16, 16};
            case GS_PSM_T4:
                return {32, 16};
            default:
                return {8, 8};
            }
        }

        // Every bit of the stored texel is the texture's (psm_t::fmsk is all
        // ones): PCSX2 then hashes the memory blocks as they are.
        bool wholeTexels(uint32_t psm)
        {
            return psm == GS_PSM_CT32 || psm == GS_PSM_Z32 || psm == GS_PSM_T8 || psm == GS_PSM_T4;
        }

        // The texel's stored value (an index for the palette formats).
        uint32_t readTexel(const uint8_t *vram, uint32_t psm, uint32_t tbp, uint32_t tbw, uint32_t x, uint32_t y)
        {
            uint8_t *m = const_cast<uint8_t *>(vram);
            switch (psm)
            {
            case GS_PSM_CT32: return GSMem::ReadCT32(m, tbp, tbw, x, y);
            case GS_PSM_CT24: return GSMem::ReadCT24(m, tbp, tbw, x, y) & 0x00FFFFFFu;
            case GS_PSM_CT16: return GSMem::ReadCT16(m, tbp, tbw, x, y) & 0xFFFFu;
            case GS_PSM_CT16S: return GSMem::ReadCT16S(m, tbp, tbw, x, y) & 0xFFFFu;
            case GS_PSM_T8: return GSMem::ReadP8(m, tbp, tbw, x, y) & 0xFFu;
            case GS_PSM_T4: return GSMem::ReadP4(m, tbp, tbw, x, y) & 0x0Fu;
            case GS_PSM_T8H: return GSMem::ReadP8H(m, tbp, tbw, x, y) & 0xFFu;
            case GS_PSM_T4HL: return GSMem::ReadP4HL(m, tbp, tbw, x, y) & 0x0Fu;
            case GS_PSM_T4HH: return GSMem::ReadP4HH(m, tbp, tbw, x, y) & 0x0Fu;
            case GS_PSM_Z32: return GSMem::ReadZ32(m, tbp, tbw, x, y);
            case GS_PSM_Z24: return GSMem::ReadZ24(m, tbp, tbw, x, y) & 0x00FFFFFFu;
            case GS_PSM_Z16: return GSMem::ReadZ16(m, tbp, tbw, x, y) & 0xFFFFu;
            case GS_PSM_Z16S: return GSMem::ReadZ16S(m, tbp, tbw, x, y) & 0xFFFFu;
            default: return 0u;
            }
        }

        // A 16-bit colour as 32 bits (GSClut::Expand16, GSBlock::ExpandBlock16).
        uint32_t expand16(uint32_t c, const GSTexaReg &texa)
        {
            const uint32_t rgb = ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19);
            uint32_t a = (c & 0x8000u) ? texa.ta1 : texa.ta0;
            if (texa.aem && c == 0u)
                a = 0u;
            return rgb | (a << 24);
        }

        // A 24-bit colour as 32 bits (GSBlock::ExpandBlock24).
        uint32_t expand24(uint32_t c, const GSTexaReg &texa)
        {
            c &= 0x00FFFFFFu;
            const uint32_t a = (texa.aem && c == 0u) ? 0u : texa.ta0;
            return c | (a << 24);
        }

        // psm_t::rtx: the texel as RGBA (TEXA applied).
        uint32_t texelColor(uint32_t psm, uint32_t raw, const GSTexaReg &texa)
        {
            switch (psm)
            {
            case GS_PSM_CT24:
            case GS_PSM_Z24:
                return expand24(raw, texa);
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
                return expand16(raw, texa);
            default:
                return raw;
            }
        }

        // ---- the packs ----------------------------------------------------------

        using PicturePtr = texture_replace::PicturePtr;
        using Picture = texture_replace::Picture;

        // A pack's files, from a folder or a folder inside a zip alike (the
        // mod manager's: zips are read in place).
        std::vector<std::string> listFiles(const std::filesystem::path &folder)
        {
            return ps2x::game_files::listFilesBelow(folder);
        }

        bool readFile(const std::filesystem::path &folder, const std::string &relative, std::vector<uint8_t> &out)
        {
            return ps2x::game_files::readFileBelow(folder, relative, out);
        }

        struct Item
        {
            uint32_t folder = 0;
            std::string file;                // level 0
            std::vector<std::string> mips;   // mips[k - 1]: level k's file ("" for none)
            // Unread (not asked for, or dropped), Queued (asked for: in the
            // queue), Reading (a loader has it), Ready, Failed.
            enum class State : uint8_t { Unread, Queued, Reading, Ready, Failed } state = State::Unread;
            PicturePtr picture;
            size_t bytes = 0;
            uint64_t lastUse = 0;
        };

        struct DumpJob
        {
            std::string file;
            uint32_t width = 0, height = 0;
            std::vector<uint32_t> colors;
        };

        struct Store
        {
            std::mutex mutex;
            std::condition_variable wake;  // loaders and the writer
            std::condition_variable idle;  // waitIdle
            std::vector<std::filesystem::path> folders;
            std::unordered_map<Name, Item, NameHash> items;
            std::unordered_set<uint64_t> shapes; // shapeKey of every name the packs have
            std::deque<Name> queue;       // first use: read first
            std::deque<Name> preloadQueue;
            size_t busy = 0;              // loaders reading now
            size_t writing = 0;           // dumps being written now
            size_t bytes = 0;
            size_t budget = 0;
            bool preload = true;
            uint64_t useClock = 0;
            bool stop = false;
            std::vector<std::thread> loaders;
            Stats counts;

            std::filesystem::path dumpFolder;
            bool dumpDirect = false;
            std::unordered_set<Name, NameHash> dumped;
            std::deque<DumpJob> dumps;
            std::thread writer;

            std::vector<std::string> xtpSources; // texture_replace sources set from the packs' .xtp files
            uint64_t readEpoch = 0;       // setFolders: reads in flight for an older set are dropped
            uint64_t configuredBoot = 0;  // configureFromBoot: the boot it read
            bool configured = false;

            Store() { budget = defaultBudget(); }
            ~Store()
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stop = true;
                }
                wake.notify_all();
                for (std::thread &t : loaders)
                    if (t.joinable())
                        t.join();
                if (writer.joinable())
                    writer.join();
            }
        };

        // A name's bits and region, its hashes aside (mayHave).
        uint64_t shapeKey(const Name &n)
        {
            return static_cast<uint64_t>(n.bits) ^ (static_cast<uint64_t>(n.regionWidth & 0xFFFFu) << 32) ^
                   (static_cast<uint64_t>(n.regionHeight & 0xFFFFu) << 48);
        }

        std::atomic<bool> g_active{false};
        std::atomic<bool> g_hasPictures{false};
        std::atomic<bool> g_dumping{false};
        std::atomic<uint64_t> g_generation{1};

        Store &store()
        {
            static Store s;
            return s;
        }

        void bumpGeneration() { g_generation.fetch_add(1u, std::memory_order_acq_rel); }

        void updateFlags(Store &s)
        {
            const bool pictures = !s.items.empty();
            const bool dumping = !s.dumpFolder.empty();
            g_hasPictures.store(pictures, std::memory_order_release);
            g_dumping.store(dumping, std::memory_order_release);
            g_active.store(pictures || dumping, std::memory_order_release);
        }

        // RGBA8 levels below level 0 by a 2 x 2 box (the last row or column
        // of an odd size counted once).
        void makeMips(Picture &p, uint32_t fromLevel)
        {
            for (uint32_t level = fromLevel; level < p.mips; ++level)
            {
                const uint32_t sw = p.levelWidth(level - 1u), sh = p.levelHeight(level - 1u);
                const uint32_t dw = p.levelWidth(level), dh = p.levelHeight(level);
                const uint8_t *src = p.data.data() + p.levelOffset(level - 1u);
                uint8_t *dst = p.data.data() + p.levelOffset(level);
                for (uint32_t y = 0; y < dh; ++y)
                    for (uint32_t x = 0; x < dw; ++x)
                    {
                        const uint32_t x0 = std::min(2u * x, sw - 1u), x1 = std::min(2u * x + 1u, sw - 1u);
                        const uint32_t y0 = std::min(2u * y, sh - 1u), y1 = std::min(2u * y + 1u, sh - 1u);
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            const uint32_t sum = src[(y0 * sw + x0) * 4u + c] + src[(y0 * sw + x1) * 4u + c] +
                                                 src[(y1 * sw + x0) * 4u + c] + src[(y1 * sw + x1) * 4u + c];
                            dst[(y * dw + x) * 4u + c] = static_cast<uint8_t>((sum + 2u) / 4u);
                        }
                    }
            }
        }

        // A PNG as RGBA8 (R first in memory), or false.
        bool decodePng(const std::vector<uint8_t> &bytes, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
        {
            SDL_IOStream *io = SDL_IOFromConstMem(bytes.data(), bytes.size());
            if (!io)
                return false;
            SDL_Surface *surface = SDL_LoadPNG_IO(io, true);
            if (!surface)
                return false;
            SDL_Surface *converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(surface);
            if (!converted)
                return false;
            width = static_cast<uint32_t>(converted->w);
            height = static_cast<uint32_t>(converted->h);
            rgba.resize(static_cast<size_t>(width) * height * 4u);
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4u,
                            static_cast<const uint8_t *>(converted->pixels) + static_cast<size_t>(y) * converted->pitch,
                            static_cast<size_t>(width) * 4u);
            SDL_DestroySurface(converted);
            return true;
        }

        // Reads an item's files into a picture with its mip chain: the
        // pack's own levels (-mip<n>, each the size it should be) where it
        // has them, the rest made from the level above.
        PicturePtr readPicture(const std::filesystem::path &folder, const std::string &file,
                               const std::vector<std::string> &mipFiles)
        {
            std::vector<uint8_t> bytes, rgba;
            uint32_t w = 0, h = 0;
            if (!readFile(folder, file, bytes) || !decodePng(bytes, w, h, rgba))
                return nullptr;
            if (w == 0u || h == 0u || w > 4096u || h > 4096u)
                return nullptr;
            auto p = std::make_shared<Picture>();
            p->width = w;
            p->height = h;
            p->format = texture_replace::Format::RGBA8;
            uint32_t mips = 1;
            while ((std::max(w, h) >> mips) != 0u)
                ++mips;
            p->mips = mips;
            p->name = file;
            p->data.resize(p->levelOffset(mips));
            std::memcpy(p->data.data(), rgba.data(), rgba.size());
            uint32_t level = 1;
            for (; level < mips; ++level)
            {
                const std::string *mf = level - 1u < mipFiles.size() ? &mipFiles[level - 1u] : nullptr;
                uint32_t mw = 0, mh = 0;
                if (!mf || mf->empty() || !readFile(folder, *mf, bytes) || !decodePng(bytes, mw, mh, rgba) ||
                    mw != p->levelWidth(level) || mh != p->levelHeight(level))
                    break;
                std::memcpy(p->data.data() + p->levelOffset(level), rgba.data(), rgba.size());
            }
            if (level < mips)
                makeMips(*p, level); // the rest from the last level there is
            return p;
        }

        // Drops the least recently used pictures until `needed` more bytes fit.
        void makeRoom(Store &s, size_t needed)
        {
            while (s.bytes + needed > s.budget)
            {
                Item *oldest = nullptr;
                for (auto &kv : s.items)
                    if (kv.second.state == Item::State::Ready && (!oldest || kv.second.lastUse < oldest->lastUse))
                        oldest = &kv.second;
                if (!oldest)
                    return;
                s.bytes -= std::min(s.bytes, oldest->bytes);
                oldest->bytes = 0;
                oldest->picture.reset();
                oldest->state = Item::State::Unread;
                --s.counts.loaded;
                bumpGeneration();
            }
        }

        void loaderLoop()
        {
            Store &s = store();
            std::unique_lock<std::mutex> lock(s.mutex);
            for (;;)
            {
                s.wake.wait(lock, [&] { return s.stop || !s.queue.empty() || (!s.preloadQueue.empty() && s.preload); });
                if (s.stop)
                    return;
                const bool preloading = s.queue.empty();
                const Name name = preloading ? s.preloadQueue.front() : s.queue.front();
                (preloading ? s.preloadQueue : s.queue).pop_front();
                auto it = s.items.find(name);
                if (it == s.items.end() || it->second.state == Item::State::Reading ||
                    it->second.state == Item::State::Ready || it->second.state == Item::State::Failed)
                    continue;
                // Preloading stops at the budget: what is left is read when drawn.
                if (preloading && s.bytes >= s.budget - s.budget / 8u)
                {
                    s.preloadQueue.clear();
                    s.idle.notify_all();
                    continue;
                }
                it->second.state = Item::State::Reading;
                const std::filesystem::path folder = s.folders[it->second.folder];
                const std::string file = it->second.file;
                const std::vector<std::string> mips = it->second.mips;
                const uint64_t epoch = s.readEpoch;
                ++s.busy;
                lock.unlock();
                const auto start = std::chrono::steady_clock::now();
                PicturePtr picture = readPicture(folder, file, mips);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                lock.lock();
                --s.busy;
                s.counts.loadSeconds += seconds;
                it = s.items.find(name);
                if (epoch == s.readEpoch && it != s.items.end())
                {
                    if (!picture)
                    {
                        it->second.state = Item::State::Failed;
                        ++s.counts.failed;
                        std::cerr << "[texture-pack] " << file << ": not a picture that can be used (PNG, at most 4096 x 4096)"
                                  << std::endl;
                    }
                    else
                    {
                        const size_t size = picture->data.size();
                        makeRoom(s, size);
                        it->second.picture = std::move(picture);
                        it->second.bytes = size;
                        it->second.state = Item::State::Ready;
                        it->second.lastUse = preloading ? 0u : ++s.useClock;
                        s.bytes += size;
                        ++s.counts.loaded;
                        bumpGeneration();
                    }
                }
                if (s.queue.empty() && (s.preloadQueue.empty() || !s.preload) && s.busy == 0u)
                    s.idle.notify_all();
            }
        }

        void writerLoop()
        {
            Store &s = store();
            std::unique_lock<std::mutex> lock(s.mutex);
            for (;;)
            {
                s.wake.wait(lock, [&] { return s.stop || !s.dumps.empty(); });
                if (s.dumps.empty())
                    return;
                DumpJob job = std::move(s.dumps.front());
                s.dumps.pop_front();
                const std::filesystem::path path = s.dumpFolder / job.file;
                ++s.writing;
                lock.unlock();
                std::error_code ec;
                bool written = false;
                if (!std::filesystem::exists(path, ec))
                {
                    std::filesystem::create_directories(path.parent_path(), ec);
                    if (SDL_Surface *surface = SDL_CreateSurfaceFrom(static_cast<int>(job.width), static_cast<int>(job.height),
                                                                     SDL_PIXELFORMAT_RGBA32, job.colors.data(),
                                                                     static_cast<int>(job.width * 4u)))
                    {
                        written = SDL_SavePNG(surface, path.string().c_str());
                        SDL_DestroySurface(surface);
                    }
                }
                lock.lock();
                --s.writing;
                if (written)
                    ++s.counts.dumped;
                if (s.dumps.empty())
                    s.idle.notify_all();
            }
        }

        // Loaders and the writer, started once something needs them.
        void startThreads(Store &s)
        {
            if (s.loaders.empty() && !s.items.empty())
            {
                const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
                const unsigned count = std::min(4u, cores / 2u);
                for (unsigned i = 0; i < count; ++i)
                    s.loaders.emplace_back(loaderLoop);
            }
            if (!s.writer.joinable() && !s.dumpFolder.empty())
                s.writer = std::thread(writerLoop);
        }

        bool envOff(const char *name)
        {
            const char *v = std::getenv(name);
            return v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
        }
    }

    // ---- names -------------------------------------------------------------------

    size_t NameHash::operator()(const Name &n) const
    {
        uint64_t h = n.textureHash ^ (n.clutHash * 0x9E3779B97F4A7C15ull);
        h ^= (static_cast<uint64_t>(n.bits) << 1) ^ (static_cast<uint64_t>(n.regionWidth) << 33) ^
             (static_cast<uint64_t>(n.regionHeight) << 47);
        h *= 0xFF51AFD7ED558CCDull;
        return static_cast<size_t>(h ^ (h >> 29));
    }

    Region Region::atLevel(uint32_t level) const
    {
        Region r;
        if (hasX)
        {
            r.hasX = true;
            r.minX = minX >> level;
            r.maxX = r.minX + std::max((maxX - minX) >> level, 1);
        }
        if (hasY)
        {
            r.hasY = true;
            r.minY = minY >> level;
            r.maxY = r.minY + std::max((maxY - minY) >> level, 1);
        }
        return r;
    }

    Region regionOf(uint32_t twLog2, uint32_t thLog2, uint64_t clamp)
    {
        const uint32_t wms = static_cast<uint32_t>(clamp & 3u), wmt = static_cast<uint32_t>((clamp >> 2) & 3u);
        const uint32_t minU = static_cast<uint32_t>((clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((clamp >> 14) & 0x3FFu);
        const uint32_t minV = static_cast<uint32_t>((clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((clamp >> 34) & 0x3FFu);
        const uint32_t tw = 1u << twLog2, th = 1u << thLog2;
        Region r;
        // REGION_CLAMP (2) narrower than the texture or reaching past it;
        // REGION_REPEAT (3): MINU is the mask, MAXU the fixed bits.
        if (wms == 2u && maxU >= minU)
        {
            const uint32_t rw = maxU - minU + 1u;
            if (rw < tw || maxU >= tw)
                r.hasX = true, r.minX = static_cast<int32_t>(minU), r.maxX = static_cast<int32_t>(maxU + 1u);
        }
        else if (wms == 3u && minU != 0u)
        {
            const uint32_t rw = ((minU | maxU) - maxU) + 1u;
            if (rw < tw || (maxU != 0u && rw <= tw))
                r.hasX = true, r.minX = static_cast<int32_t>(maxU), r.maxX = static_cast<int32_t>((minU | maxU) + 1u);
        }
        if (wmt == 2u && maxV >= minV)
        {
            const uint32_t rh = maxV - minV + 1u;
            if (rh < th || maxV >= th)
                r.hasY = true, r.minY = static_cast<int32_t>(minV), r.maxY = static_cast<int32_t>(maxV + 1u);
        }
        else if (wmt == 3u && minV != 0u)
        {
            const uint32_t rh = ((minV | maxV) - maxV) + 1u;
            if (rh < th || (maxV != 0u && rh <= th))
                r.hasY = true, r.minY = static_cast<int32_t>(maxV), r.maxY = static_cast<int32_t>((minV | maxV) + 1u);
        }
        return r;
    }

    bool hasPalette(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    uint32_t nameBits(uint32_t psm, uint32_t twLog2, uint32_t thLog2, const GSTexaReg &texa)
    {
        uint32_t bits = (psm & 0x3Fu) | ((twLog2 & 0xFu) << 6) | ((thLog2 & 0xFu) << 10);
        // TEXA counts for 24 and 16-bit texels only (psm_t::fmt > 0, no palette).
        const bool texaCounts = psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z24 ||
                                psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
        if (texaCounts)
            bits |= (static_cast<uint32_t>(texa.ta0) << 15) | ((texa.aem ? 1u : 0u) << 23) |
                    (static_cast<uint32_t>(texa.ta1) << 24);
        return bits;
    }

    uint64_t textureHash(const uint8_t *vram, uint32_t psm, const Level *levels, uint32_t levelCount, const Region &region,
                         const GSTexaReg &texa)
    {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        const BlockSize bs = blockSize(psm);
        const bool palette = hasPalette(psm);
        std::vector<uint8_t> rows;
        for (uint32_t k = 0; k < levelCount; ++k)
        {
            const Level &lv = levels[k];
            const Region rg = k == 0u ? region : region.atLevel(k);
            const int32_t tw = rg.hasX ? rg.maxX - rg.minX : (1 << lv.twLog2);
            const int32_t th = rg.hasY ? rg.maxY - rg.minY : (1 << lv.thLog2);
            const int32_t left = rg.hasX ? rg.minX : 0, top = rg.hasY ? rg.minY : 0;
            const int32_t right = rg.hasX ? rg.maxX : tw, bottom = rg.hasY ? rg.maxY : th;
            // The rectangle out to whole blocks.
            const int32_t bl = left & ~(bs.x - 1), bt = top & ~(bs.y - 1);
            const int32_t br = (right + bs.x - 1) & ~(bs.x - 1), bb = (bottom + bs.y - 1) & ~(bs.y - 1);
            const uint32_t tbw = std::max<uint32_t>(lv.tbw, 1u);
            // (PCSX2's test, GetMaxX() > 0 || GetMinY() > 0, as it is.)
            const bool regionTest = rg.hasX || (rg.hasY && rg.minY > 0);
            if (tw < bs.x || th < bs.y || !wholeTexels(psm) || regionTest)
            {
                // The texels read out (indices a byte each, colours as RGBA)
                // over the block rectangle, each row then hashed from the
                // rectangle's left: in bytes, for colours too (PCSX2's offset).
                const uint32_t texelBytes = palette ? 1u : 4u;
                const uint32_t width = static_cast<uint32_t>(br - bl);
                const size_t rowBytes = static_cast<size_t>(width) * texelBytes;
                rows.assign(rowBytes, 0);
                const size_t offset = static_cast<size_t>(left - bl);
                const size_t rowSize = static_cast<size_t>(tw) * texelBytes;
                for (int32_t y = 0; y < th; ++y)
                {
                    const uint32_t gy = static_cast<uint32_t>(top + y);
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t raw = readTexel(vram, psm, lv.tbp, tbw, static_cast<uint32_t>(bl) + x, gy);
                        if (palette)
                            rows[x] = static_cast<uint8_t>(raw);
                        else
                        {
                            const uint32_t c = texelColor(psm, raw, texa);
                            std::memcpy(rows.data() + static_cast<size_t>(x) * 4u, &c, 4u);
                        }
                    }
                    XXH3_64bits_update(&state, rows.data() + offset, std::min(rowSize, rowBytes - offset));
                }
            }
            else
            {
                // The blocks themselves, row after row of blocks.
                for (int32_t by = bt; by < bb; by += bs.y)
                    for (int32_t bx = bl; bx < br; bx += bs.x)
                    {
                        const uint32_t address = GSMem::ByteAddress(psm, lv.tbp, tbw, static_cast<uint32_t>(bx),
                                                                    static_cast<uint32_t>(by));
                        XXH3_64bits_update(&state, vram + (address & ~0xFFu), 256u);
                    }
            }
        }
        return XXH3_64bits_digest(&state);
    }

    uint32_t candidateShapes(uint32_t psm, const Level *levels, uint32_t levelCount, uint64_t clamp, const GSTexaReg &texa,
                             uint64_t clutHash, Candidate out[8])
    {
        levelCount = std::clamp<uint32_t>(levelCount, 1u, 7u);
        const uint32_t first = (levelCount > 1u && levels[0].tbp == 0u) ? 1u : 0u;
        const bool palette = hasPalette(psm);
        uint32_t n = 0;
        for (uint32_t b = first; b < levelCount && n < 8u; ++b)
        {
            // CLAMP's fields at the base level (GSRendererHW's MIP_CLAMP).
            const auto field = [&](uint32_t shift) -> uint64_t { return (((clamp >> shift) & 0x3FFu) >> b) << shift; };
            const uint64_t baseClamp = (clamp & 0xFu) | field(4) | field(14) | field(24) | field(34);
            const Level &lv = levels[b];
            Candidate c;
            c.base = b;
            c.region = regionOf(lv.twLog2, lv.thLog2, baseClamp);
            const Region &r = c.region;
            c.name.clutHash = palette ? clutHash : 0u;
            c.name.bits = nameBits(psm, lv.twLog2, lv.thLog2, texa);
            c.name.regionWidth = r.hasX ? static_cast<uint32_t>(r.maxX - r.minX) : 0u;
            c.name.regionHeight = r.hasY ? static_cast<uint32_t>(r.maxY - r.minY) : 0u;
            c.region0.hasX = r.hasX;
            c.region0.hasY = r.hasY;
            c.region0.minX = r.minX << b;
            c.region0.maxX = r.maxX << b;
            c.region0.minY = r.minY << b;
            c.region0.maxY = r.maxY << b;
            const uint32_t chain = levelCount - b;
            if (b == first || chain == 1u)
            {
                c.levels = 1;
                out[n++] = c;
            }
            if (chain > 1u && n < 8u)
            {
                c.levels = chain;
                out[n++] = c;
            }
        }
        return n;
    }

    void hashCandidate(const uint8_t *vram, uint32_t psm, const Level *levels, const GSTexaReg &texa, Candidate &c)
    {
        if (c.hashed)
            return;
        c.name.textureHash = textureHash(vram, psm, levels + c.base, c.levels, c.region, texa);
        c.hashed = true;
    }

    uint32_t candidateNames(const uint8_t *vram, uint32_t psm, const Level *levels, uint32_t levelCount, uint64_t clamp,
                            const GSTexaReg &texa, uint64_t clutHash, Candidate out[8], bool firstOnly)
    {
        uint32_t n = candidateShapes(psm, levels, levelCount, clamp, texa, clutHash, out);
        if (firstOnly)
            n = std::min(n, 1u);
        for (uint32_t i = 0; i < n; ++i)
            hashCandidate(vram, psm, levels, texa, out[i]);
        return n;
    }

    uint32_t readClut(const uint16_t *clut, uint32_t psm, uint32_t cpsm, uint32_t csa, const GSTexaReg &texa,
                      uint32_t out[256])
    {
        if (!hasPalette(psm))
            return 0u;
        const bool eight = psm == GS_PSM_T8 || psm == GS_PSM_T8H;
        const uint32_t count = eight ? 256u : 16u;
        if (cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S)
        {
            const uint32_t base = (csa & 0x1Fu) << 4;
            for (uint32_t i = 0; i < count; ++i)
                out[i] = expand16(clut[(base + i) & 0x1FFu], texa);
            return count;
        }
        // CT32 and CT24: the 32 bits as stored (GSClut::ReadCLUT_T32_I8 /
        // _I4). An 8-bit texture's offset clamps at the last 16 entries.
        const uint32_t offset = (csa & 0x0Fu) << 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = eight ? std::min((i & 0xF0u) + offset, 240u) + (i & 0x0Fu) : offset + i;
            out[i] = static_cast<uint32_t>(clut[at & 0xFFu]) | (static_cast<uint32_t>(clut[(at & 0xFFu) + 256u]) << 16);
        }
        return count;
    }

    uint64_t clutHash(const uint32_t *entries, uint32_t count)
    {
        return XXH3_64bits(entries, static_cast<size_t>(count) * 4u);
    }

    std::vector<uint32_t> readColors(const uint8_t *vram, uint32_t psm, const Level &level, const Region &region,
                                     const GSTexaReg &texa, const uint32_t *clut)
    {
        const int32_t tw = region.width(1u << level.twLog2), th = region.height(1u << level.thLog2);
        const int32_t left = region.hasX ? region.minX : 0, top = region.hasY ? region.minY : 0;
        const uint32_t tbw = std::max<uint32_t>(level.tbw, 1u);
        const bool palette = hasPalette(psm);
        std::vector<uint32_t> out(static_cast<size_t>(std::max(tw, 0)) * static_cast<size_t>(std::max(th, 0)));
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                const uint32_t raw = readTexel(vram, psm, level.tbp, tbw, static_cast<uint32_t>(left + x),
                                               static_cast<uint32_t>(top + y));
                out[static_cast<size_t>(y) * tw + x] = palette ? (clut ? clut[raw & 0xFFu] : raw) : texelColor(psm, raw, texa);
            }
        return out;
    }

    std::string fileName(const Name &n, uint32_t mip)
    {
        char buffer[128];
        const unsigned long long t = n.textureHash, c = n.clutHash;
        const bool palette = hasPalette(n.bits & 0x3Fu);
        int len;
        if (n.hasRegion())
            len = palette ? std::snprintf(buffer, sizeof(buffer), "%llx-%llx-r%ux%u-%08x", t, c, n.regionWidth, n.regionHeight, n.bits)
                          : std::snprintf(buffer, sizeof(buffer), "%llx-r%ux%u-%08x", t, n.regionWidth, n.regionHeight, n.bits);
        else
            len = palette ? std::snprintf(buffer, sizeof(buffer), "%llx-%llx-%08x", t, c, n.bits)
                          : std::snprintf(buffer, sizeof(buffer), "%llx-%08x", t, n.bits);
        std::string out(buffer, static_cast<size_t>(std::max(len, 0)));
        if (mip > 0u)
            out += "-mip" + std::to_string(mip);
        return out + ".png";
    }

    std::optional<Name> parseFileName(std::string_view fileName, uint32_t *mip)
    {
        // The parts between '-' up to the extension's dot.
        const size_t dot = fileName.rfind('.');
        if (dot == std::string_view::npos || dot == 0)
            return std::nullopt;
        std::string_view stem = fileName.substr(0, dot);
        std::vector<std::string_view> parts;
        for (size_t start = 0;;)
        {
            const size_t dash = stem.find('-', start);
            parts.push_back(stem.substr(start, dash == std::string_view::npos ? std::string_view::npos : dash - start));
            if (dash == std::string_view::npos)
                break;
            start = dash + 1;
        }
        uint32_t level = 0;
        if (parts.size() > 1 && parts.back().size() > 3 && parts.back().substr(0, 3) == "mip")
        {
            unsigned long v = 0;
            const std::string digits(parts.back().substr(3));
            char *end = nullptr;
            v = std::strtoul(digits.c_str(), &end, 10);
            if (!end || *end != '\0' || v == 0 || v > 12)
                return std::nullopt;
            level = static_cast<uint32_t>(v);
            parts.pop_back();
        }
        const auto hex = [](std::string_view s, uint64_t &out, size_t maxDigits)
        {
            if (s.empty() || s.size() > maxDigits)
                return false;
            uint64_t v = 0;
            for (char ch : s)
            {
                uint32_t d;
                if (ch >= '0' && ch <= '9')
                    d = static_cast<uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f')
                    d = static_cast<uint32_t>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F')
                    d = static_cast<uint32_t>(ch - 'A' + 10);
                else
                    return false;
                v = (v << 4) | d;
            }
            out = v;
            return true;
        };
        const auto region = [](std::string_view s, uint32_t &w, uint32_t &h)
        {
            if (s.size() < 4 || s[0] != 'r')
                return false;
            const size_t x = s.find('x');
            if (x == std::string_view::npos)
                return false;
            const std::string a(s.substr(1, x - 1)), b(s.substr(x + 1));
            if (a.empty() || b.empty() || a.find_first_not_of("0123456789") != std::string::npos ||
                b.find_first_not_of("0123456789") != std::string::npos)
                return false;
            w = static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 10));
            h = static_cast<uint32_t>(std::strtoul(b.c_str(), nullptr, 10));
            return true;
        };
        Name n;
        uint64_t bits = 0;
        bool ok = false;
        if (parts.size() == 4 && region(parts[2], n.regionWidth, n.regionHeight))
            ok = hex(parts[0], n.textureHash, 16) && hex(parts[1], n.clutHash, 16) && hex(parts[3], bits, 8);
        else if (parts.size() == 3 && region(parts[1], n.regionWidth, n.regionHeight))
            ok = hex(parts[0], n.textureHash, 16) && hex(parts[2], bits, 8);
        else if (parts.size() == 3)
            ok = hex(parts[0], n.textureHash, 16) && hex(parts[1], n.clutHash, 16) && hex(parts[2], bits, 8);
        else if (parts.size() == 2)
            ok = hex(parts[0], n.textureHash, 16) && hex(parts[1], bits, 8);
        if (!ok)
            return std::nullopt;
        // (bit 14 was TCC in older names: PCSX2 drops it, RemoveUnusedBits)
        n.bits = static_cast<uint32_t>(bits) & ~(1u << 14);
        if (!hasPalette(n.bits & 0x3Fu))
            n.clutHash = 0;
        if (mip)
            *mip = level;
        return n;
    }

    // ---- packs ---------------------------------------------------------------------

    void configureFromBoot()
    {
        Store &s = store();
        const uint64_t boot = ps2x::boot::generation();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.configured && s.configuredBoot == boot)
                return;
        }
        std::vector<std::filesystem::path> folders;
        if (const char *packs = ps2x::boot::text("PS2X_TEXTURE_PACKS"); packs && *packs)
            folders = ps2x::game_files::modsRoots(packs);
        std::filesystem::path dump;
        if (const char *d = ps2x::boot::text("PS2X_TEXTURE_DUMP"); d && *d)
            dump = d;
        const char *direct = std::getenv("PS2X_TEXTURE_DUMP_DIRECT");
        setPreload(!envOff("PS2X_TEXTURE_PRELOAD"));
        setDumpFolder(dump, direct && std::strcmp(direct, "1") == 0);
        setFolders(folders);
        // PS2X_TEXTURE_PRELOAD=wait: the boot waits until the pictures that
        // fit are read (replays, measurements; otherwise they are read while
        // the game boots and shows its menus).
        if (const char *p = std::getenv("PS2X_TEXTURE_PRELOAD"); p && std::strcmp(p, "wait") == 0 && !folders.empty())
        {
            const auto start = std::chrono::steady_clock::now();
            waitIdle();
            const Stats st = stats();
            std::cerr << "[texture-pack] preloaded " << st.loaded << " pictures, " << (st.bytes >> 20) << " MB, in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s" << std::endl;
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        s.configured = true;
        s.configuredBoot = boot;
    }

    void setFolders(const std::vector<std::filesystem::path> &folders)
    {
        Store &s = store();
        // The index first (file system work outside the lock).
        std::unordered_map<Name, Item, NameHash> items;
        std::vector<std::pair<Name, std::pair<uint32_t, std::string>>> mipFiles;
        std::vector<std::pair<uint32_t, std::string>> xtpFiles;
        for (uint32_t f = 0; f < folders.size(); ++f)
            for (const std::string &file : listFiles(folders[f]))
            {
                const size_t slash = file.rfind('/');
                const std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
                if (base.size() < 5)
                    continue;
                std::string ext = base.substr(base.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (ext == ".xtp")
                    xtpFiles.push_back({f, file});
                if (ext != ".png")
                    continue;
                uint32_t mip = 0;
                std::optional<Name> name = parseFileName(base, &mip);
                if (!name)
                    continue;
                if (mip > 0u)
                {
                    mipFiles.push_back({*name, {mip, file}});
                    continue;
                }
                Item item;
                item.folder = f;
                item.file = file;
                items[*name] = std::move(item); // a later folder's wins
            }
        // .xtp files (texture_replace's packs, keyed by the indices as the
        // Xbox pack is): a source each, in pack order (a later one wins).
        std::vector<std::string> oldSources;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            oldSources.swap(s.xtpSources);
        }
        for (const std::string &source : oldSources)
            texture_replace::removeSource(source);
        std::vector<std::string> sources;
        for (const auto &[f, file] : xtpFiles)
        {
            std::vector<uint8_t> bytes;
            std::vector<texture_replace::Entry> entries;
            std::string error;
            if (!readFile(folders[f], file, bytes) || !texture_replace::parsePack(bytes, entries, &error))
            {
                std::cerr << "[texture-pack] " << file << ": not a texture pack file (" << error << ")" << std::endl;
                continue;
            }
            const std::string source = "texture-pack:" + (folders[f] / file).generic_string();
            texture_replace::setSource(source, std::move(entries));
            sources.push_back(source);
        }
        for (auto &[name, mf] : mipFiles)
        {
            auto it = items.find(name);
            if (it == items.end())
                continue;
            if (it->second.mips.size() < mf.first)
                it->second.mips.resize(mf.first);
            it->second.mips[mf.first - 1u] = mf.second;
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.readEpoch; // reads in flight are for the old set
            s.xtpSources = std::move(sources);
            s.folders = folders;
            s.items = std::move(items);
            s.shapes.clear();
            for (const auto &kv : s.items)
                s.shapes.insert(shapeKey(kv.first));
            s.queue.clear();
            s.preloadQueue.clear();
            s.bytes = 0;
            s.counts = Stats{};
            for (auto &kv : s.items)
                s.preloadQueue.push_back(kv.first);
            // In file order: a pack's pictures are read as they lie.
            std::sort(s.preloadQueue.begin(), s.preloadQueue.end(), [&](const Name &a, const Name &b)
                      { const Item &x = s.items.at(a), &y = s.items.at(b);
                        return x.folder != y.folder ? x.folder > y.folder : x.file < y.file; });
            updateFlags(s);
            startThreads(s);
        }
        bumpGeneration();
        s.wake.notify_all();
        if (!folders.empty())
            std::cerr << "[texture-pack] " << folders.size() << " pack folder(s), " << stats().names << " pictures" << std::endl;
    }

    void setDumpFolder(const std::filesystem::path &folder, bool direct)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.dumpFolder = folder;
        s.dumpDirect = direct;
        s.dumped.clear();
        updateFlags(s);
        startThreads(s);
    }

    bool active() { return g_active.load(std::memory_order_relaxed); }
    bool hasPictures() { return g_hasPictures.load(std::memory_order_relaxed); }
    bool dumping() { return g_dumping.load(std::memory_order_relaxed); }
    bool dumpingDirect()
    {
        if (!dumping())
            return false;
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.dumpDirect;
    }

    texture_replace::PicturePtr find(const Name &name, bool *known)
    {
        if (known)
            *known = false;
        if (!hasPictures())
            return nullptr;
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.items.find(name);
        if (it == s.items.end())
            return nullptr;
        if (known)
            *known = true;
        Item &item = it->second;
        item.lastUse = ++s.useClock;
        if (item.state == Item::State::Ready)
            return item.picture;
        if (item.state == Item::State::Unread)
        {
            item.state = Item::State::Queued;
            s.queue.push_back(name);
            ++s.counts.requests;
            s.wake.notify_all();
        }
        return nullptr;
    }

    uint64_t generation() { return g_generation.load(std::memory_order_acquire); }

    bool mayHave(const Name &shape)
    {
        if (!hasPictures())
            return false;
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.shapes.count(shapeKey(shape)) != 0u;
    }

    void dump(const Name &name, uint32_t width, uint32_t height, std::vector<uint32_t> colors)
    {
        if (!dumping() || width == 0u || height == 0u || colors.size() < static_cast<size_t>(width) * height)
            return;
        Store &s = store();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.dumpFolder.empty() || s.items.count(name) || !s.dumped.insert(name).second)
                return;
            s.dumps.push_back({fileName(name), width, height, std::move(colors)});
        }
        s.wake.notify_all();
    }

    void setPreload(bool on)
    {
        Store &s = store();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.preload = on;
        }
        s.wake.notify_all();
    }

    void setBudget(size_t bytes)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.budget = std::max<size_t>(bytes, 1u << 20);
        makeRoom(s, 0);
    }

    size_t defaultBudget()
    {
        if (const char *mb = std::getenv("PS2X_TEXTURE_PACK_MB"); mb && *mb)
            return static_cast<size_t>(std::max(16l, std::strtol(mb, nullptr, 10))) << 20;
#if defined(__ANDROID__)
        return size_t{256} << 20;
#else
        return size_t{1024} << 20;
#endif
    }

    void waitIdle()
    {
        Store &s = store();
        std::unique_lock<std::mutex> lock(s.mutex);
        s.idle.wait(lock, [&]
                    { return s.queue.empty() && (s.preloadQueue.empty() || !s.preload) && s.busy == 0u && s.dumps.empty() && s.writing == 0u; });
    }

    Stats stats()
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        Stats out = s.counts;
        out.folders = s.folders.size();
        out.names = s.items.size();
        out.bytes = s.bytes;
        return out;
    }

    std::vector<uint32_t> colorsAt(const texture_replace::Picture &p, uint32_t width, uint32_t height)
    {
        std::vector<uint32_t> out(static_cast<size_t>(width) * height, 0u);
        if (p.format != texture_replace::Format::RGBA8 || width == 0u || height == 0u || !p.valid())
            return out;
        // The smallest level still as large as the PS2 texture.
        uint32_t level = 0;
        while (level + 1u < p.mips && p.levelWidth(level + 1u) >= width && p.levelHeight(level + 1u) >= height)
            ++level;
        const uint32_t lw = p.levelWidth(level), lh = p.levelHeight(level);
        const uint8_t *src = p.data.data() + p.levelOffset(level);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * lh / height);
            const uint32_t y1 = std::max(y0 + 1u, static_cast<uint32_t>(static_cast<uint64_t>(y + 1u) * lh / height));
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * lw / width);
                const uint32_t x1 = std::max(x0 + 1u, static_cast<uint32_t>(static_cast<uint64_t>(x + 1u) * lw / width));
                uint32_t sum[4] = {0, 0, 0, 0};
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx)
                        for (uint32_t c = 0; c < 4u; ++c)
                            sum[c] += src[(static_cast<size_t>(sy) * lw + sx) * 4u + c];
                const uint32_t n = (y1 - y0) * (x1 - x0);
                uint32_t v = 0;
                for (uint32_t c = 0; c < 4u; ++c)
                    v |= ((sum[c] + n / 2u) / n) << (8u * c);
                out[static_cast<size_t>(y) * width + x] = v;
            }
        }
        return out;
    }
}
