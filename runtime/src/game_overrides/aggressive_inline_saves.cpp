// Aggressive Inline (SLUS-20327): native save files instead of the memory
// card.
//
// The save jobs are the engine's on the host (zge/host/save_jobs.cpp, one
// body with BMX XXX's) at AI's save system (zsaveapi.c); this is the game's
// part, where its saves are, and its autosave:
//   <saves>/<profile>.sav          career (459780 bytes: the byte sum, then
//                                  the buffer gameSavePrepare serializes
//                                  into, g_gameSaveData)
//   <saves>/Parks/Park NNN.sav      park editor parks (8196 bytes: the park
//                                  buffer {size, data, client} at
//                                  g_parkSaveBuffer), NNN = the card folder's
//                                  number (BASLUS-20327IPARKNNN)
// (game/host/save_files.h). The boot's quiet load opens no save screen
// (game/aggressive/host/save_screen.cpp). The Ape's career and the extra
// levels' filters are added to every career save written
// (addCareerBodyFilter).
//
// Autosave: career progress is written when a level ends (after the game
// commits the session into the skater records, from the level exit,
// shutdownLevelSession, at its call of freeHudMessages) and on quit if a save
// exists, with a host copy of the game's own serializer.
//
// PS2X_SAVES=card: none of this is installed (the reference tools compare
// against PCSX2, whose timing includes the card dialogs). PS2X_AUTOSAVE=0:
// no autosave. PS2X_TRACE_SAVES=1: checks the game's buffer against RAM.
#include "aggressive_inline_saves.h"
#include "sym/game_overrides/aggressive_inline_saves.cpp.h"
#include "zge_saves.h"
#include "zge_sites.h"

#include "aggressive_inline_ape.h"

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_save_store.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace aggressive_inline
{
    namespace
    {
        namespace zs = zge_host::saves;
        using Bytes = std::vector<uint8_t>;

        // The level exit calls freeHudMessages right after it committed the
        // session (career only).
        constexpr uint32_t kAfterLevelCommit = sym::freeHudMessages;
        constexpr uint32_t kLevelExit = sym::shutdownLevelSession;
        constexpr uint32_t kGameType = sym::g_gameTypeByte; // u8, 1 = career

        // The career save (the buffer gameSavePrepare allocates and
        // serializes into) and the parks (the park buffer {size, data,
        // client}).
        constexpr uint32_t kCareerBuffer = sym::g_gameSaveData; // pointer
        constexpr uint32_t kCareerSize = 0x70400u;
        constexpr uint32_t kParkHandle = sym::g_parkSaveBuffer;
        constexpr uint32_t kParkSize = 0x2000u;

        // The game's serializer (gameSavePrepare): 0xF74 bytes of globals
        // (unlocks, cheats, volumes, soundtrack), then skaters 1..19: their
        // 0x2A8-byte record and for 10 levels six words (five blob sizes and
        // one more) and the five blobs.
        constexpr uint32_t kGlobals = sym::g_unlockedSkaters;
        constexpr uint32_t kGlobalsSize = 0xF74u;
        constexpr uint32_t kSkaterRecords = sym::g_skaterRecords;
        constexpr uint32_t kSkaterStride = 0x510u;
        constexpr uint32_t kSkaterRecordSize = 0x2A8u;
        constexpr uint32_t kLevelRecords = sym::g_levelRecords;
        constexpr uint32_t kLevelStride = 0x2Cu;

        constexpr const char *kCareerCardName = "BASLUS-20327CEAIGAME";
        constexpr const char *kParkCardPrefix = "BASLUS-20327IPARK";

        struct Saving
        {
            PS2Runtime::RecompiledFunction afterLevelCommit = nullptr;
            PS2Runtime::RecompiledFunction levelExit = nullptr; // shutdownLevelSession
            bool inLevelExit = false;   // it is running and has not yet called afterLevelCommit
            std::vector<CareerBodyFilter> bodyFilters; // addCareerBodyFilter
        };
        Saving &saving() { return ps2x::session::state<Saving>(); }
        void filterBody(Bytes &body)
        {
            for (const CareerBodyFilter &filter : saving().bodyFilters)
                filter(body);
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            uint32_t v = 0;
            if (p)
                std::memcpy(&v, p, 4);
            return v;
        }

        // A guest range as one host span, or null.
        const uint8_t *span(const uint8_t *rdram, uint32_t address, uint32_t size)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || size == 0)
                return p;
            const uint8_t *last = getConstMemPtr(rdram, address + size - 1u);
            return last == p + size - 1u ? p : nullptr;
        }

        // ---- the career serializer (autosave) --------------------------

        // The body gameSavePrepare would write, zero-padded to 0x70400;
        // used = the bytes it fills. False when a pointer or size is off.
        bool serializeCareer(const uint8_t *rdram, Bytes &body, size_t &used)
        {
            body.assign(kCareerSize, 0u);
            used = 0;
            auto copy = [&](uint32_t address, uint32_t size) -> bool
            {
                if (size > kCareerSize - used)
                    return false;
                const uint8_t *p = span(rdram, address, size);
                if (!p)
                    return false;
                std::memcpy(body.data() + used, p, size);
                used += size;
                return true;
            };
            if (!copy(kGlobals, kGlobalsSize))
                return false;
            for (uint32_t skater = 1; skater < 20; ++skater)
            {
                if (!copy(kSkaterRecords + skater * kSkaterStride, kSkaterRecordSize))
                    return false;
                for (uint32_t level = 0; level < 10; ++level)
                {
                    const uint32_t record = kLevelRecords + skater * kSkaterStride + level * kLevelStride;
                    for (uint32_t field : {4u, 0xCu, 0x14u, 0x1Cu, 0x24u, 0x28u})
                        if (!copy(record + field, 4u))
                            return false;
                    for (uint32_t blob = 0; blob < 5; ++blob)
                    {
                        const uint32_t size = rd32(rdram, record + blob * 8u + 4u);
                        if (size && !copy(rd32(rdram, record + blob * 8u), size))
                            return false;
                    }
                }
            }
            // The Ape's career (skater 0) after the game's, as the game's
            // serializer has it with the Ape on (aggressive_inline_ape.cpp).
            ape::appendCareer(rdram, body, used);
            filterBody(body);
            return true;
        }

        // PS2X_AUTOSAVE=0: only the game's own saves write (test runs).
        bool autosaveEnabled()
        {
            static const bool on = []
            {
                const char *v = std::getenv("PS2X_AUTOSAVE");
                return !(v && v[0] == '0');
            }();
            return on;
        }

        void autosave(const uint8_t *rdram, const char *why)
        {
            if (!zs::saveSeen() || zs::jobRunning() || !autosaveEnabled())
                return;
            Bytes body;
            size_t used = 0;
            if (!serializeCareer(rdram, body, used))
            {
                std::cerr << "[saves] " << why << ": career data out of range, not saved" << std::endl;
                return;
            }
            if (zs::sameAsLast(body, used))
                return; // nothing new
            zs::writeSave(body.data(), why);
        }

        bool traceSaves()
        {
            static const bool on = []
            {
                const char *v = std::getenv("PS2X_TRACE_SAVES");
                return v && v[0] == '1';
            }();
            return on;
        }

        // PS2X_TRACE_SAVES=1: the game's serialized buffer against the host
        // serializer run on RAM now.
        void traceBuffer(const uint8_t *rdram, PS2Runtime *runtime, const uint8_t *body, const char *what)
        {
            Bytes now;
            size_t used = 0;
            if (!body || !serializeCareer(rdram, now, used))
                return;
            size_t first = used;
            for (size_t i = 0; i < used; ++i)
                if (body[i] != now[i])
                {
                    first = i;
                    break;
                }
            std::cout << "[saves] trace " << what << ": " << used << " bytes of career data, "
                      << (first == used ? std::string("buffer = RAM")
                                        : "buffer differs from RAM at body offset 0x" + [&]
                                          {
                                              char t[16];
                                              std::snprintf(t, sizeof(t), "%zX", first);
                                              return std::string(t);
                                          }())
                      << ", vsync " << ps2_syscalls::GetCurrentVSyncTick(runtime) << std::endl;
        }

        void traceSave(const uint8_t *rdram, PS2Runtime *runtime, const uint8_t *body)
        {
            traceBuffer(rdram, runtime, body, "save");
        }

        // The Ape's career after the game's (aggressive_inline_ape.cpp), then the filters.
        void completeSave(const uint8_t *rdram, Bytes &body)
        {
            size_t used = 0;
            ape::appendCareer(rdram, body, used);
            filterBody(body);
        }

        // Before freeHudMessages in the level exit: career progress was just
        // committed to the skater records.
        void levelExitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            saving().inLevelExit = true;
            saving().levelExit(rdram, ctx, runtime);
            saving().inLevelExit = false;
        }

        void afterLevelCommit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (inside the level exit, which is wrapped: never known by the
            // call's return address)
            if (saving().inLevelExit)
            {
                saving().inLevelExit = false; // its one call
                zge_host::sites::seen("ai.saves.level_end");
                const uint8_t *type = getConstMemPtr(rdram, kGameType);
                if (type && *type == 1u)
                    autosave(rdram, "autosave (level end)");
            }
            saving().afterLevelCommit(rdram, ctx, runtime);
        }

        void onQuit(PS2Runtime &runtime)
        {
            std::error_code ec;
            if (!zs::saveSeen() || !std::filesystem::exists(zs::saveFile(), ec))
                return; // never create a save from the defaults
            autosave(runtime.memory().getRDRAM(), "autosave (quit)");
        }
    }

    std::filesystem::path nativeSavesFolder()
    {
        return zs::folder();
    }

    void addCareerBodyFilter(CareerBodyFilter filter)
    {
        if (filter)
            saving().bodyFilters.push_back(std::move(filter));
    }

    std::string nativeSaveProfile()
    {
        return zs::profile();
    }

    void installNativeSaves(PS2Runtime &runtime)
    {
        if (!zs::install(runtime, game::Game::Ai,
                         {.gameSaveBuffer = kCareerBuffer,
                          .gameSaveBytes = kCareerSize,
                          .gameSaveCardFolder = kCareerCardName,
                          .parkBuffer = kParkHandle,
                          .parkBytes = kParkSize,
                          .parkCardFolderPrefix = kParkCardPrefix,
                          .completeSave = &completeSave,
                          .traceSave = &traceSave}))
            return;
        saving().afterLevelCommit = runtime.lookupFunction(kAfterLevelCommit);
        saving().levelExit = runtime.lookupFunction(kLevelExit);
        if (saving().levelExit)
            runtime.replaceFunction(kLevelExit, &levelExitHook);
        if (!saving().afterLevelCommit || !runtime.replaceFunction(kAfterLevelCommit, &afterLevelCommit))
        {
            std::cerr << "[saves] the level-end autosave hook not installed" << std::endl;
            return;
        }
        ps2x::session::addShutdownHook(&onQuit);
    }
}
