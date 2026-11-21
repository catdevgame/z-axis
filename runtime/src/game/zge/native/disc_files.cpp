#include "disc_files.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_boot_config.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

namespace zge::native
{
    namespace gf = zx::game_files;

    std::string normalizeGamePath(const std::string &gamePath) { return gf::normalizePath(gamePath); }

    namespace
    {
        // One boot's files and the disc reads its guest memory came from
        // (a game booted after another in the same process reads its own
        // disc: zx::boot::PerBoot makes this at the boot's first use).
        struct DiscRead
        {
            uint32_t size = 0;
            std::string path;
            uint64_t offset = 0;
        };
        struct State
        {
            std::mutex filesMutex;
            std::unique_ptr<gf::GameFileSystem> files;
            std::mutex readsMutex;
            std::map<uint32_t, DiscRead> reads; // by guest start address, not overlapping
            std::map<std::string, uint64_t> fileReads; // normalized path -> reads started at offset 0
        };
        constexpr size_t kMaxReads = 65536;

        State &state()
        {
            static zx::boot::PerBoot<std::unique_ptr<State>> ofBoot([] { return std::make_unique<State>(); });
            return *ofBoot.get();
        }
    }

    bool readGameFile(const std::string &normalizedPath, std::vector<uint8_t> &out)
    {
        State &st = state();
        std::lock_guard<std::mutex> lock(st.filesMutex);
        std::unique_ptr<gf::GameFileSystem> &files = st.files;
        if (!files)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            gf::GameFileRoots roots;
            if (!paths.cdFromImage)
                roots.disc = paths.cdRoot;
            if (roots.disc.empty())
                roots.image = PS2Runtime::cdImageFileSystem();
            if (const char *mods = zx::boot::text("ZX_MODS_DIR"); mods && *mods)
                roots.mods = gf::modsRoots(mods);
            files = std::make_unique<gf::GameFileSystem>(std::move(roots));
        }
        const auto file = files->open(normalizedPath);
        return file && gf::readAll(*file, out);
    }

    void noteDiscRead(const std::string &path, uint64_t offset, uint32_t address, uint32_t bytes)
    {
        if (bytes == 0)
            return;
        const std::string normalized = gf::normalizePath(path);
        const uint32_t end = address + bytes;
        State &st = state();
        std::lock_guard<std::mutex> lock(st.readsMutex);
        if (offset == 0)
            ++st.fileReads[normalized];
        // Cut what this read overwrites out of the earlier ones.
        auto it = st.reads.lower_bound(address);
        if (it != st.reads.begin())
        {
            auto before = std::prev(it);
            const uint32_t beforeEnd = before->first + before->second.size;
            if (beforeEnd > address)
            {
                if (beforeEnd > end)
                {
                    DiscRead tail = before->second;
                    tail.offset += end - before->first;
                    tail.size = beforeEnd - end;
                    st.reads[end] = tail;
                }
                before->second.size = address - before->first;
            }
        }
        while (it != st.reads.end() && it->first < end)
        {
            const uint32_t itEnd = it->first + it->second.size;
            if (itEnd > end)
            {
                DiscRead tail = it->second;
                tail.offset += end - it->first;
                tail.size = itEnd - end;
                st.reads[end] = tail;
            }
            it = st.reads.erase(it);
        }
        st.reads[address] = DiscRead{bytes, normalized, offset};
        if (st.reads.size() > kMaxReads)
            st.reads.clear(); // a long session: start over rather than grow
    }

    bool discOrigin(uint32_t address, uint32_t size, std::string &path, uint64_t &offset)
    {
        State &st = state();
        std::lock_guard<std::mutex> lock(st.readsMutex);
        auto it = st.reads.upper_bound(address);
        if (it == st.reads.begin())
            return false;
        --it;
        if (it->first + it->second.size <= address)
            return false;
        path = it->second.path;
        offset = it->second.offset + (address - it->first);
        // The rest of the range: the same file's reads, back to back.
        uint32_t covered = it->first + it->second.size;
        uint64_t nextOffset = it->second.offset + it->second.size;
        while (covered < address + size)
        {
            auto next = st.reads.find(covered);
            if (next == st.reads.end() || next->second.path != path || next->second.offset != nextOffset)
                return false;
            covered += next->second.size;
            nextOffset += next->second.size;
        }
        return true;
    }

    uint64_t discFileReads(const std::string &normalizedPath)
    {
        State &st = state();
        std::lock_guard<std::mutex> lock(st.readsMutex);
        const auto it = st.fileReads.find(normalizedPath);
        return it == st.fileReads.end() ? 0u : it->second;
    }
}
