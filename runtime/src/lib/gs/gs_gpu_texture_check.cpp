#include "runtime/gs/gs_gpu_texture_check.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace zx::gpu_texture_check
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int kStackDepth = 24;
        constexpr uint64_t kDetailedReports = 40; // with stacks; then a line every 1000

        struct Stack
        {
            void *frames[kStackDepth] = {};
            int count = 0;
        };

        void capture(Stack &s)
        {
#if defined(_WIN32)
            s.count = static_cast<int>(RtlCaptureStackBackTrace(2, kStackDepth, s.frames, nullptr));
#else
            void *frames[kStackDepth + 2];
            const int n = backtrace(frames, kStackDepth + 2);
            s.count = n > 2 ? n - 2 : 0;
            std::memcpy(s.frames, frames + 2, sizeof(void *) * static_cast<size_t>(s.count));
#endif
        }

        // One line per frame: module+offset (for atos / a link map), and the
        // symbol where the platform knows it.
        std::string describe(const Stack &s)
        {
            std::string out;
            char line[640];
#if defined(_WIN32)
            static bool symInit = false;
            HANDLE process = GetCurrentProcess();
            if (!symInit)
            {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                SymInitialize(process, nullptr, TRUE);
                symInit = true;
            }
            for (int i = 0; i < s.count; ++i)
            {
                const DWORD64 addr = reinterpret_cast<DWORD64>(s.frames[i]);
                HMODULE module = nullptr;
                char path[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(s.frames[i]), &module))
                    GetModuleFileNameA(module, path, MAX_PATH);
                const char *base = std::strrchr(path, '\\');
                base = base ? base + 1 : path;
                const unsigned long long offset = addr - reinterpret_cast<DWORD64>(module);
                alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                IMAGEHLP_LINE64 src{};
                src.SizeOfStruct = sizeof(src);
                DWORD lineDisp = 0;
                if (SymFromAddr(process, addr, &disp, sym))
                {
                    if (SymGetLineFromAddr64(process, addr, &lineDisp, &src))
                        std::snprintf(line, sizeof(line), "    #%d %s+0x%llx %s+0x%llx (%s:%lu)\n", i, base, offset, sym->Name,
                                      static_cast<unsigned long long>(disp), src.FileName, src.LineNumber);
                    else
                        std::snprintf(line, sizeof(line), "    #%d %s+0x%llx %s+0x%llx\n", i, base, offset, sym->Name,
                                      static_cast<unsigned long long>(disp));
                }
                else
                    std::snprintf(line, sizeof(line), "    #%d %s+0x%llx\n", i, base, offset);
                out += line;
            }
#else
            for (int i = 0; i < s.count; ++i)
            {
                Dl_info info{};
                if (dladdr(s.frames[i], &info) && info.dli_fname)
                {
                    const char *base = std::strrchr(info.dli_fname, '/');
                    base = base ? base + 1 : info.dli_fname;
                    const unsigned long long offset = static_cast<unsigned long long>(
                        static_cast<const char *>(s.frames[i]) - static_cast<const char *>(info.dli_fbase));
                    if (info.dli_sname)
                        std::snprintf(line, sizeof(line), "    #%d %s+0x%llx %s+0x%llx\n", i, base, offset, info.dli_sname,
                                      static_cast<unsigned long long>(static_cast<const char *>(s.frames[i]) -
                                                                      static_cast<const char *>(info.dli_saddr)));
                    else
                        std::snprintf(line, sizeof(line), "    #%d %s+0x%llx\n", i, base, offset);
                }
                else
                    std::snprintf(line, sizeof(line), "    #%d %p\n", i, s.frames[i]);
                out += line;
            }
#endif
            return out;
        }

        struct Held
        {
            SDL_GPUDevice *device = nullptr; // null: an owner's token (ownerGone), not an SDL handle
            Clock::time_point when;
            SDL_ThreadID thread = 0;
            const char *owner = "";
            Stack stack;
        };

        struct State
        {
            std::mutex mutex;
            bool on = false;
            double holdSeconds = 2.0;
            void (*release)(SDL_GPUDevice *, SDL_GPUTexture *) = nullptr;
            std::unordered_set<const SDL_GPUTexture *> live; // made here, not released
            std::unordered_map<const SDL_GPUTexture *, Held> held;
            std::deque<SDL_GPUTexture *> order; // oldest release first
            uint64_t created = 0, released = 0, misuses = 0;
        };

        State &state()
        {
            static State *s = []()
            {
                State *st = new State; // never destroyed: used until exit
                const char *v = std::getenv("ZX_GPU_TEXTURE_CHECK");
                st->on = v && v[0] == '1';
                if (st->on)
                    std::fprintf(stderr, "[gpu-check] ZX_GPU_TEXTURE_CHECK=1: released GPU textures kept %.1f s, uses "
                                         "of them reported\n",
                                 st->holdSeconds);
                return st;
            }();
            return *s;
        }

        std::atomic<bool> g_on{false};
        std::atomic<bool> g_inited{false};

        bool on()
        {
            if (!g_inited.load(std::memory_order_acquire))
            {
                g_on.store(state().on, std::memory_order_relaxed);
                g_inited.store(true, std::memory_order_release);
            }
            return g_on.load(std::memory_order_relaxed);
        }

        void giveToSdl(State &s, SDL_GPUDevice *device, SDL_GPUTexture *texture)
        {
            if (s.release)
                s.release(device, texture);
            else
                SDL_ReleaseGPUTexture(device, texture);
        }

        // Handles kept longer than holdSeconds go to SDL. Under s.mutex.
        void expire(State &s, Clock::time_point now)
        {
            const auto hold = std::chrono::duration<double>(s.holdSeconds);
            while (!s.order.empty())
            {
                SDL_GPUTexture *t = s.order.front();
                auto it = s.held.find(t);
                if (it == s.held.end())
                {
                    s.order.pop_front(); // given back early (releaseHeld)
                    continue;
                }
                if (now - it->second.when < hold)
                    break;
                SDL_GPUDevice *device = it->second.device;
                s.held.erase(it);
                s.order.pop_front();
                if (device)
                    giveToSdl(s, device, t);
                else
                    delete[] reinterpret_cast<char *>(t); // a token
            }
        }

        // h: the release (null for a handle never made here or long gone).
        void report(State &s, const SDL_GPUTexture *texture, const Held *h, const char *what, const char *kind)
        {
            const uint64_t n = ++s.misuses;
            const double age = h ? std::chrono::duration<double>(Clock::now() - h->when).count() : 0.0;
            if (n > kDetailedReports)
            {
                if (n % 1000u == 0u)
                    std::fprintf(stderr, "[gpu-check] %llu uses of released textures so far\n",
                                 static_cast<unsigned long long>(n));
                return;
            }
            Stack use;
            capture(use);
            std::string text;
            char head[400];
            if (h)
                std::snprintf(head, sizeof(head),
                              "[gpu-check] %s %s texture %p %s %.3f s ago (use #%llu, thread %llu; on thread %llu)\n"
                              "  use:\n",
                              kind, what, static_cast<const void *>(texture),
                              h->device ? "released" : "(a token) its owner destroyed", age, static_cast<unsigned long long>(n),
                              static_cast<unsigned long long>(SDL_GetCurrentThreadID()),
                              static_cast<unsigned long long>(h->thread));
            else
                std::snprintf(head, sizeof(head), "[gpu-check] %s %s texture %p (use #%llu, thread %llu)\n  use:\n", kind,
                              what, static_cast<const void *>(texture), static_cast<unsigned long long>(n),
                              static_cast<unsigned long long>(SDL_GetCurrentThreadID()));
            text = head;
            text += describe(use);
            if (h)
            {
                text += h->device ? "  released at:\n" : "  its owner destroyed at:\n";
                text += describe(h->stack);
            }
            std::fputs(text.c_str(), stderr);
            std::fflush(stderr);
        }
    }

    bool enabled() { return on(); }

    SDL_GPUTexture *createTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *info)
    {
        SDL_GPUTexture *t = SDL_CreateGPUTexture(device, info);
        if (on() && t)
        {
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.created;
            s.live.insert(t);
            s.held.erase(t); // stale: an address SDL gave out again
        }
        return t;
    }

    void releaseTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture)
    {
        if (!texture)
            return;
        if (!on())
        {
            SDL_ReleaseGPUTexture(device, texture);
            return;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const Clock::time_point now = Clock::now();
        auto it = s.held.find(texture);
        if (it != s.held.end())
        {
            report(s, texture, &it->second, "release of", "TWICE:");
            return;
        }
        s.live.erase(texture);
        ++s.released;
        Held &h = s.held[texture];
        h.device = device;
        h.when = now;
        h.thread = SDL_GetCurrentThreadID();
        capture(h.stack);
        s.order.push_back(texture);
        expire(s, now);
    }

    void releaseHeld(SDL_GPUDevice *device)
    {
        if (!on())
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.release)
            std::fprintf(stderr, "[gpu-check] device %p goes: %llu textures created, %llu released, %llu uses of "
                                 "released ones\n",
                         static_cast<void *>(device), static_cast<unsigned long long>(s.created),
                         static_cast<unsigned long long>(s.released), static_cast<unsigned long long>(s.misuses));
        for (auto it = s.held.begin(); it != s.held.end();)
        {
            if (it->second.device != device)
            {
                ++it;
                continue;
            }
            SDL_GPUTexture *t = const_cast<SDL_GPUTexture *>(it->first);
            it = s.held.erase(it);
            giveToSdl(s, device, t);
        }
    }

    namespace
    {
        // 0: fine; 1: a misuse of a real handle (reported); 2: a token (reported; must not reach SDL).
        int checkOne(const SDL_GPUTexture *texture, const char *what, Need need);
    }

    bool check(const SDL_GPUTexture *texture, const char *what, Need need) { return checkOne(texture, what, need) == 0; }

    SDL_GPUTexture *ownerGone(const char *owner)
    {
        if (!on())
            return nullptr;
        char *token = new char[1];
        SDL_GPUTexture *t = reinterpret_cast<SDL_GPUTexture *>(token);
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const Clock::time_point now = Clock::now();
        Held &h = s.held[t];
        h.device = nullptr;
        h.when = now;
        h.thread = SDL_GetCurrentThreadID();
        h.owner = owner;
        capture(h.stack);
        s.order.push_back(t);
        expire(s, now);
        return t;
    }

    namespace
    {
    int checkOne(const SDL_GPUTexture *texture, const char *what, Need need)
    {
        if (!on())
            return 0;
        if (!texture)
        {
            if (need == Need::Optional)
                return 0;
            State &s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            report(s, texture, nullptr, what, "NULL:");
            return 1;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.held.find(texture);
        if (it != s.held.end())
        {
            if (!it->second.device)
            {
                const std::string kind = std::string("STALE OWNER (read through a destroyed ") + it->second.owner + "):";
                report(s, texture, &it->second, what, kind.c_str());
                return 2;
            }
            report(s, texture, &it->second, what, "USE AFTER RELEASE:");
            return 1;
        }
        if (need == Need::MadeHere && !s.live.count(texture))
        {
            report(s, texture, nullptr, what, "UNKNOWN (not made here, or released long ago):");
            return 1;
        }
        return 0;
    }
    }

    void bindFragmentSamplers(SDL_GPURenderPass *pass, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings,
                              Uint32 count)
    {
        if (on())
        {
            bool token = false;
            for (Uint32 i = 0; i < count; ++i)
                token |= checkOne(bindings[i].texture, "bind (fragment sampler) of", Need::MadeHere) == 2;
            if (token)
                return; // not an SDL handle: reported, the bind left out
        }
        SDL_BindGPUFragmentSamplers(pass, firstSlot, bindings, count);
    }

    SDL_GPURenderPass *beginRenderPass(SDL_GPUCommandBuffer *commands, const SDL_GPUColorTargetInfo *colorTargets,
                                       Uint32 colorCount, const SDL_GPUDepthStencilTargetInfo *depthTarget)
    {
        if (on())
        {
            for (Uint32 i = 0; i < colorCount; ++i)
            {
                check(colorTargets[i].texture, "render target", Need::Present);
                check(colorTargets[i].resolve_texture, "resolve target", Need::Optional);
            }
            if (depthTarget)
                check(depthTarget->texture, "depth target", Need::Present);
        }
        return SDL_BeginGPURenderPass(commands, colorTargets, colorCount, depthTarget);
    }

    void blitTexture(SDL_GPUCommandBuffer *commands, const SDL_GPUBlitInfo *info)
    {
        if (on())
        {
            const bool token = (checkOne(info->source.texture, "blit from", Need::MadeHere) == 2) |
                               (checkOne(info->destination.texture, "blit to", Need::Present) == 2);
            if (token)
                return;
        }
        SDL_BlitGPUTexture(commands, info);
    }

    void copyTextureToTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureLocation *source,
                              const SDL_GPUTextureLocation *destination, Uint32 w, Uint32 h, Uint32 d, bool cycle)
    {
        if (on())
        {
            const bool token = (checkOne(source->texture, "copy from", Need::MadeHere) == 2) |
                               (checkOne(destination->texture, "copy to", Need::MadeHere) == 2);
            if (token)
                return;
        }
        SDL_CopyGPUTextureToTexture(pass, source, destination, w, h, d, cycle);
    }

    void uploadToTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureTransferInfo *source,
                         const SDL_GPUTextureRegion *destination, bool cycle)
    {
        if (on() && checkOne(destination->texture, "upload to", Need::MadeHere) == 2)
            return;
        SDL_UploadToGPUTexture(pass, source, destination, cycle);
    }

    void downloadFromTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureRegion *source,
                             const SDL_GPUTextureTransferInfo *destination)
    {
        if (on() && checkOne(source->texture, "download from", Need::MadeHere) == 2)
            return;
        SDL_DownloadFromGPUTexture(pass, source, destination);
    }

    void generateMipmaps(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *texture)
    {
        if (on() && checkOne(texture, "mipmaps of", Need::MadeHere) == 2)
            return;
        SDL_GenerateMipmapsForGPUTexture(commands, texture);
    }

    Counts counts()
    {
        Counts c;
        if (!on())
            return c;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        c.created = s.created;
        c.released = s.released;
        c.held = s.held.size();
        c.misuses = s.misuses;
        return c;
    }

    void configureForTest(bool enable, double holdSeconds, void (*release)(SDL_GPUDevice *, SDL_GPUTexture *))
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.on = enable;
        s.holdSeconds = holdSeconds;
        s.release = release;
        for (auto &kv : s.held)
            if (!kv.second.device)
                delete[] reinterpret_cast<char *>(const_cast<SDL_GPUTexture *>(kv.first));
        s.held.clear();
        s.live.clear();
        s.order.clear();
        s.created = s.released = s.misuses = 0;
        g_on.store(enable, std::memory_order_relaxed);
        g_inited.store(true, std::memory_order_release);
    }
}
