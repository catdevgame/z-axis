#include "runtime/ps2_gs_texture_audit.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace ps2x::gs_texture_audit
{
    bool Audit::setLevel(const std::string &label)
    {
        if (m_current && label == m_level)
            return false;
        m_level = label;
        m_current = &m_levels[label];
        m_frameOverran = false;
        return true;
    }

    int Audit::noteAllocation(uint32_t address, uint32_t words, uint32_t clutCursor, const char *texture)
    {
        if (!m_current)
            setLevel(m_level);
        LevelStats &s = *m_current;
        const uint64_t endWord = static_cast<uint64_t>(address) + words;
        const uint32_t start = address / kWordsPerBlock;
        const uint32_t end = static_cast<uint32_t>((endWord + kWordsPerBlock - 1u) / kWordsPerBlock);
        ++s.allocations;
        if (start < s.lowestStart)
            s.lowestStart = start;
        if (end > s.peakEnd)
        {
            s.peakEnd = end;
            s.peakTexture = texture ? texture : "";
        }
        const uint32_t clutBlock = clutCursor / kWordsPerBlock;
        if (clutBlock < s.clutFloor)
            s.clutFloor = clutBlock;
        int result = 0;
        if (endWord > clutCursor)
        {
            ++s.overCluts;
            result = 1;
        }
        if (endWord > kGsWords)
        {
            ++s.pastEnd;
            result = 2;
        }
        if (result)
            m_frameOverran = true;
        return result;
    }

    void Audit::noteFrame()
    {
        if (!m_current)
            return;
        ++m_current->frames;
        if (m_frameOverran)
            ++m_current->overrunFrames;
        m_frameOverran = false;
    }

    const LevelStats &Audit::stats(const std::string &label) const
    {
        static const LevelStats none;
        auto it = m_levels.find(label);
        return it == m_levels.end() ? none : it->second;
    }

    std::string Audit::describe(const std::string &label, const LevelStats &s)
    {
        std::ostringstream o;
        o << "[gsmem] " << label << ": " << s.allocations << " allocations";
        if (s.allocations)
            o << ", blocks " << s.lowestStart << ".." << s.peakEnd << " (peak"
              << (s.peakTexture.empty() ? std::string() : ": " + s.peakTexture) << "), CLUTs from " << s.clutFloor;
        o << ", " << s.overCluts << " over the CLUTs, " << s.pastEnd << " past the end (" << kGsBlocks
          << "), overruns in " << s.overrunFrames << " of " << s.frames << " frames";
        return o.str();
    }

    namespace
    {
        struct State
        {
            std::mutex mutex;
            Audit audit;
            GameCache cache;
            LabelSource name;
            std::vector<LabelSource> sources;
            PS2Runtime::RecompiledFunction original = nullptr;
            int verbose = 0;
            // What was last printed of the current level (to print only changes).
            uint64_t printedAllocations = UINT64_MAX, printedOverruns = UINT64_MAX;
            bool firstOverrunLogged = false;
        };
        State &state()
        {
            static State s;
            return s;
        }

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }

        std::string textureName(const uint8_t *rdram, uint32_t object)
        {
            std::string name;
            for (uint32_t i = 0; i < 31u; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, object + 0x10u + i);
                if (!p || *p == 0u)
                    break;
                name += (*p >= 0x20u && *p < 0x7Fu) ? static_cast<char>(*p) : '?';
            }
            return name;
        }

        void printLevel(State &s, bool force)
        {
            const LevelStats &st = s.audit.stats(s.audit.level());
            const uint64_t overruns = st.overCluts;
            if (!force && st.allocations == s.printedAllocations && overruns == s.printedOverruns)
                return;
            if (st.allocations == 0u && st.frames == 0u)
                return;
            s.printedAllocations = st.allocations;
            s.printedOverruns = overruns;
            std::cout << Audit::describe(s.audit.level(), st) << std::endl;
        }

        void levelAllocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = state();
            const uint32_t gsTexture = getRegU32(ctx, 4); // texture object + 0x30 (maybe the uncached mirror)
            const uint32_t record = getRegU32(ctx, 5);
            s.original(rdram, ctx, runtime);
            const uint32_t address = readU32(rdram, record + 0x08u);
            const uint32_t words = readU32(rdram, record + 0x14u);
            const uint32_t clutCursor = readU32(rdram, s.cache.cacheState + 0x18u);
            std::lock_guard<std::mutex> lock(s.mutex);
            const uint32_t object = gsTexture - 0x30u;
            const uint64_t endWord = static_cast<uint64_t>(address) + words;
            const uint32_t endBlock = static_cast<uint32_t>((endWord + kWordsPerBlock - 1u) / kWordsPerBlock);
            // The name only when it is reported: a new peak or an overrun.
            const bool peak = endBlock > s.audit.stats(s.audit.level()).peakEnd;
            const bool over = endWord > clutCursor;
            const std::string name = peak || over ? textureName(rdram, object) : std::string();
            const int result = s.audit.noteAllocation(address, words, clutCursor, peak ? name.c_str() : nullptr);
            if (!result)
                return;
            if (!s.firstOverrunLogged)
            {
                s.firstOverrunLogged = true;
                std::cout << "[gsmem] " << s.audit.level() << ": first overrun: " << name << " at blocks "
                          << address / kWordsPerBlock << ".." << endBlock
                          << " with the CLUTs from " << clutCursor / kWordsPerBlock
                          << (result == 2 ? ", past the end of GS memory" : ", over the CLUTs") << std::endl;
            }
            else if (s.verbose > 1)
                std::cout << "[gsmem] " << s.audit.level() << ": " << name << " at blocks " << address / kWordsPerBlock
                          << ".." << endBlock << " (CLUTs from "
                          << clutCursor / kWordsPerBlock << ")" << (result == 2 ? " past the end" : " over the CLUTs")
                          << std::endl;
        }

        void vsync(uint8_t *rdram, uint64_t tick)
        {
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (tick % 30u == 0u)
            {
                std::string label;
                for (const LabelSource &source : s.sources)
                    if (label.empty() && source)
                        label = source(rdram);
                if (label.empty() && s.name)
                    label = s.name(rdram);
                if (label.empty())
                    label = "?";
                label = std::string(s.cache.game) + " " + label;
                if (label != s.audit.level())
                {
                    printLevel(s, false);
                    s.audit.setLevel(label);
                    const LevelStats &st = s.audit.stats(label);
                    s.printedAllocations = st.allocations;
                    s.printedOverruns = st.overCluts;
                    s.firstOverrunLogged = st.overCluts != 0u;
                }
            }
            s.audit.noteFrame();
            if (tick % 600u == 0u)
                printLevel(s, false);
        }
    }

    bool enabled()
    {
        const char *v = std::getenv("PS2X_GSMEM_AUDIT");
        return v && v[0] && v[0] != '0';
    }

    void addLabelSource(LabelSource source)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.sources.push_back(std::move(source));
    }

    bool install(PS2Runtime &runtime, const GameCache &cache, LabelSource name)
    {
        if (!enabled())
            return false;
        State &s = state();
        if (s.original)
            return true;
        s.verbose = std::atoi(std::getenv("PS2X_GSMEM_AUDIT"));
        s.cache = cache;
        s.name = std::move(name);
        if (!runtime.hasFunction(cache.levelAllocator) ||
            !(s.original = runtime.lookupFunction(cache.levelAllocator)) ||
            !runtime.replaceFunction(cache.levelAllocator, &levelAllocHook))
        {
            s.original = nullptr;
            std::cerr << "[gsmem] the texture cache's allocator 0x" << std::hex << cache.levelAllocator << std::dec
                      << " cannot be wrapped: no audit" << std::endl;
            return false;
        }
        runtime.addVSyncHook(&vsync);
        std::cout << "[gsmem] GS texture memory audit on (" << cache.game << ", allocator 0x" << std::hex
                  << cache.levelAllocator << ", cache 0x" << cache.cacheState << std::dec << "; GS memory ends at block "
                  << kGsBlocks << ")" << std::endl;
        return true;
    }
}
