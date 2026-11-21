#include "collision_data.h"

#include "geo_file.h"
#include "geo_meshes.h"

#include "ee/readable.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "zge/native/host.h"
#include "ee/vu0.h"
#include "runtime/ps2_boot_config.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "sym/zge/native/collision_data.cpp.h"

namespace zge::native
{
    namespace
    {
        struct Cache
        {
            std::mutex mutex;
            uint64_t epoch = ~0ull;
            std::unordered_map<uint32_t, std::shared_ptr<const HostCollisionPart>> parts;
            // Host parts by the file part they were made from (instances share one).
            std::unordered_map<const GeoFilePart *, std::shared_ptr<const HostCollisionPart>> made;
            // The positions streams the game wrote into, each with a host part of its own (notePartPosition).
            std::unordered_map<uint32_t, std::shared_ptr<HostCollisionPart>> own;
        };
        // The boot's (zx::boot::PerBoot): `made` is keyed by the file
        // parts' host addresses, and the next boot's file parts
        // (geo_meshes.cpp's registry, per boot too) are made once the last
        // boot's are freed, often at the same addresses: a cache kept from
        // the last boot answered a part with another part's positions (a
        // game booted again in one process drew and collided with them).
        Cache &cache()
        {
            static zx::boot::PerBoot<std::unique_ptr<Cache>> ofBoot([] { return std::make_unique<Cache>(); });
            return *ofBoot.get();
        }

        std::atomic<uint64_t> g_hostTests{0}, g_ramTests{0};
        // Counts the streams given or taken their own host part: with notedPartsEpoch, what the per-thread answers
        // below are current for.
        std::atomic<uint64_t> g_ownEpoch{0};
        uint64_t hostPartsEpoch() { return notedPartsEpoch() + g_ownEpoch.load(std::memory_order_acquire); }

        // The part's positions stream (its header's address; 0 for none).
        uint32_t positionsStreamOf(uint32_t partAddress) { return ee::at<ZgePart>(partAddress)->positions.address; }

        // Whether a stream's own host part still has its positions (each written one is RAM's).
        bool ownMatchesRam(uint32_t stream, const HostCollisionPart &own)
        {
            const ZgeStream &header = *ee::at<ZgeStream>(stream);
            const uint64_t bytes = own.positions.size() * sizeof(Vec4);
            return header.bytes >= sizeof(ZgeStream) + bytes &&
                   std::memcmp(own.positions.data(), ee::at<uint8_t>(stream + uint32_t(sizeof(ZgeStream))), bytes) == 0;
        }

        // The running game's level setup (setLevelSetupStep): none at a
        // boot's start, set by the game's install, read when a host part is
        // made.
        LevelSetupStep &levelSetupStep()
        {
            static zx::boot::PerBoot<LevelSetupStep> ofBoot([]() -> LevelSetupStep { return nullptr; });
            return ofBoot.get();
        }
    }

    void setLevelSetupStep(LevelSetupStep step) { levelSetupStep() = step; }

    namespace
    {
        std::shared_ptr<const HostCollisionPart> lookUp(uint32_t partAddress);
    }

    std::shared_ptr<const HostCollisionPart> hostCollisionPart(ee::Address part) { return hostCollisionPart(part.address); }

    std::shared_ptr<const HostCollisionPart> hostCollisionPart(uint32_t partAddress)
    {
        // The face tests ask for every node they test: a per-thread copy of
        // the answers, dropped when parts are noted again, spares the lock.
        thread_local uint64_t epoch = ~0ull;
        thread_local std::unordered_map<uint32_t, std::shared_ptr<const HostCollisionPart>> seen;
        const uint64_t now = hostPartsEpoch();
        if (now != epoch)
        {
            seen.clear();
            epoch = now;
        }
        if (auto it = seen.find(partAddress); it != seen.end())
            return it->second;
        auto part = lookUp(partAddress);
        if (hostPartsEpoch() == epoch)
            seen.emplace(partAddress, part);
        return part;
    }

    namespace
    {
    std::shared_ptr<const HostCollisionPart> lookUp(uint32_t partAddress)
    {
        Cache &c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.own.empty())
            if (auto it = c.own.find(positionsStreamOf(partAddress)); it != c.own.end())
            {
                // (as RAM holds the stream: else the stream went and another took its place, unseen)
                if (ownMatchesRam(it->first, *it->second))
                    return it->second;
                c.own.erase(it);
                g_ownEpoch.fetch_add(1, std::memory_order_acq_rel);
            }
        const uint64_t epoch = notedPartsEpoch();
        if (epoch != c.epoch)
        {
            c.parts.clear();
            c.epoch = epoch;
        }
        if (auto it = c.parts.find(partAddress); it != c.parts.end())
            return it->second;
        bool fromDisc = true;
        std::shared_ptr<const GeoFilePart> file = discFilePartFor(partAddress);
        if (!file)
        {
            file = filePartFor(partAddress);
            fromDisc = false;
        }
        std::shared_ptr<const HostCollisionPart> made;
        if (file)
        {
            auto known = c.made.find(file.get());
            if (known != c.made.end())
                made = known->second;
            else
            {
                std::shared_ptr<HostCollisionPart> part = makeHostCollisionPart(*file, *g_tagIndexWidth, levelSetupStep());
                if (part)
                    part->fromDisc = fromDisc;
                made = part;
                c.made[file.get()] = made;
            }
        }
        c.parts[partAddress] = made;
        if (!made && std::getenv("ZX_NATIVE_COLLISION_DEBUG"))
        {
            std::string path;
            uint64_t offset = 0;
            const bool origin = partDiscOrigin(partAddress, path, offset);
            const auto stream = filePartFor(partAddress);
            std::cout << "[native-collision] part 0x" << std::hex << partAddress << std::dec << ": no host part ("
                      << (file ? "its file part is not modelled" : "no file part") << "; disc origin "
                      << (origin ? path : std::string("unknown")) << " +0x" << std::hex << offset << std::dec
                      << ", stream part " << (stream ? "found" : "not found") << ")" << std::endl;
        }
        return made;
    }
    }

    void notePartPosition(uint32_t partAddress, uint32_t index)
    {
        const uint32_t stream = positionsStreamOf(partAddress);
        if (!stream)
            return;
        Vec4 position;
        std::memcpy(&position, ee::at<uint8_t>(stream + uint32_t(sizeof(ZgeStream)) + index * uint32_t(sizeof(Vec4))), sizeof(Vec4));
        Cache &c = cache();
        {
            // A stream written to before: the position goes into its own host part. (Not through lookUp, which
            // would find it no longer RAM's, since RAM has this position and it has not yet.)
            std::lock_guard<std::mutex> lock(c.mutex);
            if (auto it = c.own.find(stream); it != c.own.end())
            {
                if (index < it->second->positions.size())
                    it->second->positions[index] = position;
                return;
            }
        }
        const std::shared_ptr<const HostCollisionPart> host = hostCollisionPart(partAddress);
        if (!host || index >= host->positions.size())
            return; // (no host part, or a vertex past the file's positions: the face tests read RAM there)
        std::lock_guard<std::mutex> lock(c.mutex);
        std::shared_ptr<HostCollisionPart> &own = c.own[stream];
        if (!own)
        {
            own = std::make_shared<HostCollisionPart>(*host);
            g_ownEpoch.fetch_add(1, std::memory_order_acq_rel);
        }
        own->positions[index] = position;
    }

    namespace
    {
        std::atomic<PS2Runtime::RecompiledFunction> g_setPartPosition{nullptr};

        // setPartPosition(part, index, position), whatever is installed for it (its readable body or the
        // original), then the host side told.
        void setPartPositionWatched(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4), index = getRegU32(ctx, 5);
            g_setPartPosition.load(std::memory_order_acquire)(rdram, ctx, runtime);
            if (hostExtrasOn())
                notePartPosition(part, index);
        }

        // Once the running game's override has applied (its readable functions installed, its hooks set):
        // this instance's game only, with the switch on. A readable caller of setPartPosition then calls
        // through the table (readable.cpp noteOverrides), so its calls are watched too.
        void watchPartPositions(PS2Runtime &runtime)
        {
            if (game::readable::runningGame() != game::readable::kInstanceGame || !nativeCollisionEnabled())
                return;
            const PS2Runtime::RecompiledFunction installed = runtime.lookupFunction(sym::setPartPosition);
            if (!installed || installed == &setPartPositionWatched)
                return;
            g_setPartPosition.store(installed, std::memory_order_release);
            if (runtime.replaceFunction(sym::setPartPosition, &setPartPositionWatched))
                std::cout << "[native-collision] the positions setPartPosition writes are followed (0x" << std::hex
                          << sym::setPartPosition << std::dec << ")" << std::endl;
        }

        const bool g_watchRegistered = (ps2_game_overrides::addAfterApplyHook(&watchPartPositions), true);
        const bool g_hostPartHooks = (setOwnHostPartHooks(&forgetOwnHostPart, &noteOwnHostPartClone), true);
    }

    void forgetOwnHostPart(uint32_t partAddress)
    {
        Cache &c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.own.empty() && c.own.erase(positionsStreamOf(partAddress)))
            g_ownEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void noteOwnHostPartClone(uint32_t cloneAddress, uint32_t sourceAddress)
    {
        Cache &c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        const uint32_t clone = positionsStreamOf(cloneAddress), source = positionsStreamOf(sourceAddress);
        if (c.own.empty() || clone == source)
            return; // (a shared stream: the clone is served what its source is)
        bool changed = c.own.erase(clone) != 0;
        if (auto it = c.own.find(source); it != c.own.end())
        {
            c.own[clone] = std::make_shared<HostCollisionPart>(*it->second);
            changed = true;
        }
        if (changed)
            g_ownEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    namespace
    {
        // 0 off, 1 on, 2 verify. The default: on, but in BMX XXX, whose host
        // data differs from its RAM copy in some reads (verify on s-root: the
        // physics and the frame packets then differ from the original's). The
        // game running, not this source's instance: zge/native is built once,
        // for every game of the collection.
        int nativeCollisionMode()
        {
            static const int mode = []
            {
                const int byDefault = game::readable::runningGame() == game::Game::Bmx ? 0 : 1;
                const char *e = std::getenv("ZX_NATIVE_COLLISION");
                const int v = !e || !*e ? byDefault : !std::strcmp(e, "verify") ? 2 : std::atoi(e) != 0 ? 1 : 0;
                if (e && *e)
                    std::cout << "[native-collision] ZX_NATIVE_COLLISION=" << e
                              << (v ? ": the readable collision code reads the collision data made on the host"
                                    : ": the readable collision code reads the game's RAM copy")
                              << (v == 2 ? ", each read compared with RAM" : "") << std::endl;
                return v;
            }();
            return mode;
        }
        std::atomic<uint64_t> g_readsSame{0}, g_readsDiffer{0};
        std::atomic<uint64_t> g_tagReadsHost{0}, g_tagReadsRam{0};
    }

    bool nativeCollisionEnabled() { return nativeCollisionMode() != 0; }

    bool nativeCollisionVerify() { return nativeCollisionMode() == 2; }

    void noteVerifiedRead(const void *host, const void *ram, size_t bytes)
    {
        if (std::memcmp(host, ram, bytes) == 0)
            g_readsSame.fetch_add(1, std::memory_order_relaxed);
        else if (g_readsDiffer.fetch_add(1, std::memory_order_relaxed) < 5)
            std::cout << "[native-collision] verify: a read of " << bytes << " bytes differs from RAM" << std::endl;
    }

    void countNativeCollisionTest(bool host)
    {
        const uint64_t n = (host ? g_hostTests : g_ramTests).fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t hostTests = g_hostTests.load(std::memory_order_relaxed);
        const uint64_t ramTests = g_ramTests.load(std::memory_order_relaxed);
        if ((hostTests + ramTests) % 20000u == 0 || (!host && n <= 3))
        {
            std::cout << "[native-collision] face tests on host data " << hostTests << ", on RAM " << ramTests;
            std::cout << "; tag reads from mirrors " << g_tagReadsHost.load() << ", tables without one "
                      << g_tagReadsRam.load();
            if (nativeCollisionVerify())
                std::cout << "; reads compared with RAM " << g_readsSame.load() + g_readsDiffer.load() << ", differing "
                          << g_readsDiffer.load();
            std::cout << std::endl;
        }
    }

    // ---- tag table mirrors ------------------------------------------------------

    namespace
    {
        struct TagEntry
        {
            uint32_t end = 0; // past the body
            uint32_t part = 0;
            TagTableKind kind = TagTableKind::FaceTags;
            bool tried = false; // the mirror was looked for (made, or none)
            std::unique_ptr<TagMirror> mirror;
        };
        // By the table's address. The EE thread's (the loaders run on it, on
        // fibers); the lock is for the odd host caller.
        struct TagTables
        {
            std::mutex mutex;
            std::map<uint32_t, TagEntry> byTable;
        };
        // The boot's: keyed by guest addresses, which the next boot's
        // tables take again (an entry the last boot left would serve its
        // part's tags for whatever lies there now).
        TagTables &tagTables()
        {
            static zx::boot::PerBoot<std::unique_ptr<TagTables>> ofBoot([] { return std::make_unique<TagTables>(); });
            return *ofBoot.get();
        }

        TagMirror *made(TagEntry &entry, uint32_t table)
        {
            if (entry.tried)
                return entry.mirror.get();
            entry.tried = true;
            const auto part = hostCollisionPart(entry.part);
            if (!part)
                return nullptr;
            const HostTagTable &t = entry.kind == TagTableKind::FaceTags ? part->faceTags : part->faceEdges;
            if (!t.present || t.body.size() != entry.end - (table + 0x10u))
            {
                if (std::getenv("ZX_NATIVE_COLLISION_DEBUG"))
                    std::cout << "[native-collision] tag table 0x" << std::hex << table << " of part 0x" << entry.part
                              << std::dec << ": the host part's table " << (t.present ? "differs in size" : "is missing")
                              << std::endl;
                return nullptr;
            }
            auto mirror = std::make_unique<TagMirror>();
            mirror->table = table;
            mirror->descriptor = t.descriptor;
            mirror->count = t.count;
            mirror->body = t.body;
            entry.mirror = std::move(mirror);
            return entry.mirror.get();
        }
    }

    void noteTagTable(uint32_t table, uint32_t part, TagTableKind kind, uint32_t bodyBytes)
    {
        TagTables &t = tagTables();
        std::lock_guard<std::mutex> lock(t.mutex);
        const uint32_t end = table + 0x10u + bodyBytes;
        // A table made where earlier ones were (their memory freed without a
        // release this side saw) replaces them.
        // (Entries never overlap, so only the one before can reach in.)
        auto it = t.byTable.lower_bound(table);
        if (it != t.byTable.begin() && std::prev(it)->second.end > table)
            t.byTable.erase(std::prev(it));
        while (it != t.byTable.end() && it->first < end)
            it = t.byTable.erase(it);
        TagEntry &entry = t.byTable[table];
        entry.end = end;
        entry.part = part;
        entry.kind = kind;
    }

    void forgetTagTable(uint32_t table)
    {
        TagTables &t = tagTables();
        std::lock_guard<std::mutex> lock(t.mutex);
        t.byTable.erase(table);
    }

    void noteTagTable(ee::Address table, ee::Address part, TagTableKind kind, uint32_t bodyBytes)
    {
        noteTagTable(table.address, part.address, kind, bodyBytes);
    }
    void forgetTagTable(ee::Address table) { forgetTagTable(table.address); }
    TagMirror *tagMirror(ee::Address table) { return tagMirror(table.address); }
    TagMirror *tagMirrorHolding(ee::Address address, uint32_t bytes) { return tagMirrorHolding(address.address, bytes); }
    uint8_t *TagMirror::at(ee::Address address) { return at(address.address); }

    TagMirror *tagMirror(uint32_t table)
    {
        TagTables &t = tagTables();
        std::lock_guard<std::mutex> lock(t.mutex);
        auto it = t.byTable.find(table);
        return it == t.byTable.end() ? nullptr : made(it->second, table);
    }

    TagMirror *tagMirrorHolding(uint32_t address, uint32_t bytes)
    {
        TagTables &t = tagTables();
        std::lock_guard<std::mutex> lock(t.mutex);
        auto it = t.byTable.upper_bound(address);
        if (it == t.byTable.begin())
            return nullptr;
        --it;
        if (uint64_t(address) + bytes > it->second.end || address < it->first + 0x10u)
            return nullptr;
        return made(it->second, it->first);
    }

    void countTagRead(bool host) { (host ? g_tagReadsHost : g_tagReadsRam).fetch_add(1, std::memory_order_relaxed); }

    bool nativeCollisionPoison()
    {
        static const bool on = []
        {
            const char *e = std::getenv("ZX_NATIVE_COLLISION_POISON");
            const bool v = e && *e && std::atoi(e) != 0 && nativeCollisionEnabled();
            if (v)
                std::cout << "[native-collision] ZX_NATIVE_COLLISION_POISON=1: the RAM copy of what the host serves "
                             "is overwritten once a part is read from the host"
                          << std::endl;
            return v;
        }();
        return on;
    }

    void poisonRamCopy(ee::Address part, ee::Address tree) { poisonRamCopy(part.address, tree.address); }

    void poisonRamCopy(uint32_t partAddress, uint32_t treeAddress)
    {
        static std::mutex mutex;
        static std::unordered_map<uint32_t, uint64_t> done; // part -> epoch
        const auto host = hostCollisionPart(partAddress);
        if (!host)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto [it, fresh] = done.emplace(partAddress, notedPartsEpoch());
            if (!fresh && it->second == notedPartsEpoch())
                return;
            it->second = notedPartsEpoch();
        }
        auto poison = [](uint32_t address, uint32_t bytes)
        {
            if (address && bytes)
                std::memset(ee::at<uint8_t>(address), 0x7F, bytes);
        };
        const ZgePart &part = *ee::at<ZgePart>(partAddress);
        uint64_t bytes = 0;
        // Positions: as many as the file has (faces past them read RAM).
        poison(part.positions.address + 0x10u, uint32_t(host->positions.size() * sizeof(Vec4)));
        bytes += host->positions.size() * sizeof(Vec4);
        // Face normals: each face's, through the stream's indices.
        if (host->hasFaceNormals && part.faceNormals.address)
        {
            const ZgeStream &normals = *ee::at<ZgeStream>(part.faceNormals.address);
            for (uint32_t face = 0; face < host->faceCount; ++face)
            {
                uint32_t index = face;
                if (normals.indices.address)
                    index = (normals.wideIndices & 1u) ? *ee::at<uint16_t>(normals.indices.address + face * 2u)
                                                       : *ee::at<uint8_t>(normals.indices.address + face);
                poison(part.faceNormals.address + 0x10u + index * uint32_t(sizeof(Vec3)), sizeof(Vec3));
            }
            bytes += host->faceCount * sizeof(Vec3);
        }
        if (host->hasTree && treeAddress)
        {
            poison(treeAddress, uint32_t(host->tree.size()));
            bytes += host->tree.size();
        }
        // Tag tables: made into mirrors first.
        uint32_t tables = 0;
        for (uint32_t table : {part.faceTags.address, part.faceEdges.address})
            if (TagMirror *mirror = table ? tagMirror(table) : nullptr)
            {
                poison(mirror->bodyAt(), uint32_t(mirror->body.size()));
                bytes += mirror->body.size();
                ++tables;
            }
        static std::atomic<uint64_t> parts{0}, total{0};
        const uint64_t n = parts.fetch_add(1) + 1;
        total += bytes;
        if (n <= 3 || n % 10 == 0)
            std::cout << "[native-collision] poisoned " << n << " parts' RAM copies, " << total.load() << " bytes (part 0x"
                      << std::hex << partAddress << std::dec << ": " << tables << " tag tables)" << std::endl;
    }
}
