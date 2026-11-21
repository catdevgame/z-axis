// The host's services to the engine's disc device (disc_files.h).

#include "disc_files.h"
#include "ee/host_form.h"

#include "machine.h"
#include "syscalls.h"
#include "runtime/ee_scheduler.h"
#include "runtime/boot_config.h"
#include "runtime/frame_trace.h"
#include "runtime/game_files.h"
#include "runtime/session.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace game::host::disc
{
    namespace
    {
        namespace gf = zx::game_files;

        struct OpenFile
        {
            std::shared_ptr<gf::GameFile> file;
            uint64_t position = 0u;
            std::string path;
        };

        struct Disc
        {
            std::unique_ptr<gf::GameFileSystem> files;
            std::unordered_map<uint32_t, OpenFile> open;
            uint32_t nextHandle = 1u;
            int trace = 0;
            Additions additions;
            bool callbackSeen = false;
            std::vector<uint8_t> readBuffer; // a read's bytes on their way to the guest
        };
        Disc &disc() { return zx::session::state<Disc>(); }

        gf::GameFileSystem &files()
        {
            std::unique_ptr<gf::GameFileSystem> &fileSystem = disc().files;
            if (!fileSystem)
            {
                const PS2Machine::IoPaths &paths = PS2Machine::getIoPaths();
                gf::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Machine::cdImageFileSystem();
                if (const char *mods = zx::boot::text("ZX_MODS_DIR"); mods && *mods)
                    roots.mods = gf::modsRoots(mods);
                std::cout << "[disc] files from " << (roots.disc.empty() ? (roots.image ? roots.image->path().string() : std::string("(none)")) : roots.disc.string())
                          << (roots.mods.empty() ? std::string() : ", mods from " + gf::modsText(roots.mods)) << std::endl;
                fileSystem = std::make_unique<gf::GameFileSystem>(std::move(roots));
            }
            return *fileSystem;
        }

        OpenFile *find(uint32_t handle)
        {
            const auto it = disc().open.find(handle);
            return it == disc().open.end() ? nullptr : &it->second;
        }

        // Into guest memory page by page (RDRAM and its mirrors, or the
        // scratchpad).
        void copyToGuest(uint8_t *rdram, uint32_t address, const uint8_t *source, size_t size)
        {
            constexpr uint32_t kPageBytes = 1u << 12;
            while (size != 0u)
            {
                const size_t chunk = std::min<size_t>(size, kPageBytes - (address & (kPageBytes - 1u)));
                if (uint8_t *p = getMemPtr(rdram, address))
                    std::memcpy(p, source, chunk);
                address += static_cast<uint32_t>(chunk);
                source += chunk;
                size -= chunk;
            }
        }
    }

    void configure(int trace, const Additions &additions)
    {
        disc().trace = trace;
        disc().additions = additions;
    }

    void prepare() { files(); }

    bool exists(const std::string &path)
    {
        const bool found = files().exists(path);
        if (disc().trace)
            std::cout << "[disc] stat " << path << (found ? "" : " (missing)") << std::endl;
        return found;
    }

    uint32_t open(const std::string &path)
    {
        Disc &d = disc();
        zx::ftrace::Scope traceOpen(zx::ftrace::kDiscOpen, zx::ftrace::kDiscOpen + 100);
        std::shared_ptr<gf::GameFile> file = files().open(path);
        if (d.additions.fileOpened)
            d.additions.fileOpened(path);
        uint32_t handle = 0u;
        if (file)
        {
            handle = d.nextHandle++;
            if (d.nextHandle == 0u)
                d.nextHandle = 1u;
            d.open[handle] = OpenFile{file, 0u, path};
        }
        if (d.trace || !file || (file && file->origin() != gf::GameFile::Origin::Disc))
            std::cout << "[disc] open " << path << " -> "
                      << (file ? std::to_string(file->size()) + " bytes from " + file->source() : std::string("not found"))
                      << std::endl;
        return handle;
    }

    bool close(uint32_t handle) { return disc().open.erase(handle) != 0u; }

    bool isOpen(uint32_t handle) { return find(handle) != nullptr; }

    uint32_t read(uint32_t handle, uint32_t destination, uint32_t bytes)
    {
        OpenFile *open = find(handle);
        if (!open)
            return 0u;
        const uint64_t size = open->file->size();
        const uint64_t available = open->position < size ? size - open->position : 0u;
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, available));
        size_t got = 0u;
        if (count != 0u)
        {
            zx::ftrace::Scope traceRead(zx::ftrace::kDiscRead, zx::ftrace::kDiscRead + 100,
                                          static_cast<uint16_t>(std::min<size_t>(count >> 14, 65535u)));
            std::vector<uint8_t> &buffer = disc().readBuffer;
            buffer.resize(count);
            got = open->file->read(open->position, buffer.data(), count);
            copyToGuest(currentCall().rdram, destination, buffer.data(), got);
            if (disc().additions.fileRead)
                disc().additions.fileRead(open->path, open->position, destination, static_cast<uint32_t>(got));
            open->position += got;
        }
        if (disc().trace > 1)
            std::cout << "[disc] read " << open->path << " " << got << "/" << bytes << " -> 0x" << std::hex
                      << destination << std::dec << " pos " << open->position << std::endl;
        if (got != count)
            std::cerr << "[disc] read error in " << open->path << " (" << got << " of " << count << " bytes)" << std::endl;
        return static_cast<uint32_t>(got);
    }

    bool seek(uint32_t handle, int32_t offset, uint32_t origin)
    {
        OpenFile *open = find(handle);
        bool ok = false;
        if (open && origin <= 2u)
        {
            const int64_t base = origin == 0u ? 0 : origin == 1u ? static_cast<int64_t>(open->position)
                                                               : static_cast<int64_t>(open->file->size());
            const int64_t target = base + offset;
            if (target >= 0)
            {
                open->position = std::min<uint64_t>(static_cast<uint64_t>(target), open->file->size());
                ok = true;
            }
        }
        if (disc().trace > 1)
            std::cout << "[disc] seek " << (open ? open->path : std::string("?")) << " " << offset << "/" << origin
                      << (ok ? "" : " failed") << std::endl;
        return ok;
    }

    uint32_t position(uint32_t handle)
    {
        const OpenFile *open = find(handle);
        return open ? static_cast<uint32_t>(open->position) : 0u;
    }

    uint32_t size(uint32_t handle)
    {
        const OpenFile *open = find(handle);
        return open ? static_cast<uint32_t>(open->file->size()) : 0u;
    }

    std::shared_ptr<gf::GameFile> openGameFile(const std::string &path) { return files().open(path); }

    std::string guestPath(uint32_t address)
    {
        constexpr size_t kLimit = 256u;
        std::string out;
        for (size_t i = 0; i < kLimit; ++i)
        {
            const uint8_t *p = getConstMemPtr(currentCall().rdram, address + static_cast<uint32_t>(i));
            if (!p || *p == 0u)
                break;
            out.push_back(static_cast<char>(*p));
        }
        return out;
    }
}

namespace game::host
{
    namespace
    {
        constexpr int kRegA0 = 4, kRegSp = 29, kRegRa = 31;
        // The report's place below the invocation's stack top, and the
        // callback's stack below that.
        constexpr uint32_t kReportBelowTop = 0x20u, kCallbackStackBelowTop = 0x40u;

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (address == 0u)
                return;
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        uint32_t readWord(uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }
    }

    void reportToCallback(uint32_t completion, uint32_t subject, bool succeeded, uint32_t value)
    {
        afterReturn([completion, subject, succeeded, value](const Call &call)
        {
            const uint32_t function = readWord(call.rdram, completion);
            if (!disc::disc().callbackSeen)
            {
                disc::disc().callbackSeen = true;
                std::cout << "[disc] completion callback 0x" << std::hex << function << std::dec << " (async use)" << std::endl;
            }
            if (!call.runtime->hasFunction(function))
                return;
            EeScheduler &ee = call.runtime->eeScheduler();
            const uint32_t top = ee.invocationStackTop();
            const uint32_t report = top - kReportBelowTop;
            writeWord(call.rdram, report + 0x0u, readWord(call.rdram, completion + 4u));
            writeWord(call.rdram, report + 0x4u, subject);
            writeWord(call.rdram, report + 0x8u, succeeded ? 1u : 0u);
            writeWord(call.rdram, report + 0xCu, value);
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::HleCall;
            invocation.context = *call.ctx;
            invocation.context.pc = function;
            SET_GPR_U32(&invocation.context, kRegA0, report);
            SET_GPR_U32(&invocation.context, kRegSp, top - kCallbackStackBelowTop);
            SET_GPR_U32(&invocation.context, kRegRa, 0u);
            invocation.onComplete = [](const R5900Context &, R5900Context &) {};
            ee.invokeCurrent(std::move(invocation));
        });
    }
}
