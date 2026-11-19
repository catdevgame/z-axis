// The host's service to the class draws' host form (native_draws.h).

#include "runtime/gs/gs_interpolate.h"
#include "native_draws.h"

#include "ee/abi.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_quality.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_session.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_set>

namespace game::host::native_draws
{
    namespace
    {
        // The runtime (process-wide: the GS side runs on the GS's thread) and the boot's wish (a boot that does not
        // attach draws no records).
        std::atomic<PS2Runtime *> g_runtime{nullptr};
        struct Boot
        {
            std::atomic<bool> records{false};
            // Records whenever frame interpolation is on (attach), unless the Native Meshes row is off.
            bool followInterpolation = false;
            std::atomic<bool> meshesOn{true};
            ps2x::zaxis::RigidDrawForm rigidDraw;
            ps2x::zaxis::SkinnedDrawForm skinnedDraw;
            std::unordered_set<uint32_t> noMeshSeen; // parts noteNoMesh printed
            std::unordered_set<uint32_t> streamsSeen; // parts noteStreamsChanged printed
        };
        Boot &boot() { return ps2x::session::state<Boot>(); }
        std::array<std::atomic<uint64_t>, static_cast<size_t>(Count::kCount)> g_counts{};
        std::atomic<uint64_t> g_draws{0}, g_declined{0};
        int g_trace = 0;

        const char *const kCountNames[] = {"rigid", "sky", "terrain", "skinned", "strict: no mesh",
                                           "strict: streams changed", "strict: effect", "strict: lights",
                                           "strict: kind"};
        static_assert(std::size(kCountNames) == static_cast<size_t>(Count::kCount));

        void printCounts()
        {
            PS2Runtime *runtime = g_runtime.load();
            std::cout << "[native-records] draws " << g_draws.load();
            for (size_t i = 0; i < g_counts.size(); ++i)
                std::cout << (i ? ", " : ": ") << kCountNames[i] << " " << g_counts[i].load();
            std::cout << " | anchors declined " << g_declined.load();
            if (runtime)
            {
                const PS2Memory::Vif1AnchorStats a = runtime->memory().vif1AnchorStats();
                std::cout << ", added " << a.added << ", reached " << a.reached << ", replaced " << a.replaced;
            }
            std::cout << std::endl;
        }
    }

    void attach(PS2Runtime &runtime, bool records, const ps2x::zaxis::RigidDrawForm &rigidDraw,
                const ps2x::zaxis::SkinnedDrawForm &skinnedDraw, bool followInterpolation)
    {
        g_runtime.store(&runtime);
        boot().records.store(records);
        boot().followInterpolation = followInterpolation;
        boot().meshesOn.store(true);
        boot().rigidDraw = rigidDraw;
        boot().skinnedDraw = skinnedDraw;
        for (auto &c : g_counts)
            c.store(0);
        g_draws.store(0);
        g_declined.store(0);
        const char *v = std::getenv("PS2X_TRACE_NATIVE");
        g_trace = v ? std::atoi(v) : 0;
    }

    const ps2x::zaxis::RigidDrawForm &rigidDraw() { return boot().rigidDraw; }
    const ps2x::zaxis::SkinnedDrawForm &skinnedDraw() { return boot().skinnedDraw; }

    void setRecords(bool on) { boot().records.store(on); }
    void setMeshesOn(bool on) { boot().meshesOn.store(on); }
    bool recordsWanted()
    {
        const Boot &b = boot();
        return b.records.load(std::memory_order_relaxed) ||
               (b.followInterpolation && b.meshesOn.load(std::memory_order_relaxed) && ps2x::gs_interpolate::enabled());
    }

    bool fromRecords() { return !ee::t_ee.callHandler && recordsWanted(); }

    uint32_t anchorCode(Begin begin)
    {
        PS2Memory &memory = g_runtime.load()->memory();
        return PS2Memory::vif1NativeAnchorCode(memory.addVif1NativeAnchor(std::move(begin)));
    }

    void gifPacket(const uint8_t *bytes, uint32_t size) { g_runtime.load()->gs().processGIFPacket(bytes, size); }
    void setRegister(uint8_t reg, uint64_t value) { g_runtime.load()->gs().writeRegister(reg, value); }
    void draw(const GSNativeDraw &draw) { g_runtime.load()->gs().drawNative(draw); }

    bool xboxCharacterLighting()
    {
        PS2Runtime *runtime = g_runtime.load();
        return ps2x::gs_quality::xboxLighting() && runtime && runtime->gs().nativeDrawsOnBackend();
    }

    void count(Count what)
    {
        g_counts[static_cast<size_t>(what)].fetch_add(1, std::memory_order_relaxed);
        const uint64_t n = g_draws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace && n % 20000u == 0u)
            printCounts();
    }

    void noteNoMesh(uint32_t partAddress, uint32_t renderType, const char *why)
    {
        if (g_trace < 2)
            return;
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        if (!boot().noMeshSeen.insert(partAddress).second)
            return;
        std::cout << "[native-records] no mesh: part 0x" << std::hex << partAddress << " type 0x" << renderType << std::dec
                  << ": " << why << std::endl;
    }

    void noteStreamsChanged(uint32_t partAddress, uint32_t renderType, uint32_t streams)
    {
        if (g_trace < 2)
            return;
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        if (!boot().streamsSeen.insert(partAddress).second)
            return;
        std::cout << "[native-records] streams changed: part 0x" << std::hex << partAddress << " type 0x" << renderType
                  << " streams 0x" << streams << std::dec << std::endl;
    }

    uint64_t draws() { return g_draws.load(std::memory_order_relaxed); }
    void countDeclined() { g_declined.fetch_add(1, std::memory_order_relaxed); }
}
