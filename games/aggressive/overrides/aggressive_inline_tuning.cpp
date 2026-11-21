// Aggressive Inline (SLUS-20327): the Xbox release's gameplay balance, as a
// choice (ZX_TUNING=xbox; the app: Settings > Game > Gameplay Tuning,
// settings.ini [gameplay] tuning = ps2|xbox, default ps2).
//
// What differs (both executables and level scripts compared; nothing else
// does: the 405-entry tuning table is the same, value for value, at PS2
// 0x24AD28 and Xbox 0x1D8390, also as the running Xbox game holds it; the
// challenge scripts' timers, time limits, counts and point targets are the
// same but for one):
//   - experience for loot and tricks (PS2 func_1C50D0, Xbox 0x31xxx): the
//     eight "key" loot items give +500,000 on the PS2 (0x1C568C ..
//     0x1C58F4, 0x58 apart, each func_1B50C0(skater, func_1B50B8(skater) +
//     500000)) and nothing on the Xbox; the seven special tricks give
//     nothing on the PS2 and +300,000 on the Xbox (default.xbe 0x31C35,
//     0x31C6A, 0x31C9D, after the "Special trick unlocked!" message: every
//     trick's branch ends in one of the three). Tangible loot (+25,000),
//     skates (+200,000) and the +100,000 award are the same.
//   - Civic Center's "350,000 Point Trick" (DaveHelper04): the PS2 script
//     wants more than 350,000, the Xbox's more than 349,999, so a trick of
//     exactly 350,000 passes there (CIVIC.BIN 0xCEB0: 350000 -> 349999).
// Not here (see docs/SETTINGS.md "Aggressive Inline's extras"): the Xbox scripts'
// bug fixes (a Hangar NPC left waiting, a Civic preview camera, ...), its
// free skate doors in the Boardwalk and the Cannery, and its closer camera
// (the same tuning, a different camera code; Settings > Camera > Camera
// Distance about 75 % looks alike).
// With ps2 (the default) nothing is hooked.

#include "aggressive_inline_tuning.h"
#include "sym/game_overrides/aggressive_inline_tuning.cpp.h"
#include "overrides/zge_sites.h"

#include "machine.h"
#include "runtime/disc_patch.h"
#include "runtime/boot_config.h"
#include "runtime/session.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace aggressive_inline::tuning
{
    namespace
    {
        constexpr uint32_t kGetPoints = sym::skaterPoints; // (skater) -> its experience points
        constexpr uint32_t kSetPoints = sym::setSkaterPoints; // (skater, points): also tells the overlay
        constexpr uint32_t kShowMessage = sym::showMessage; // (text, seconds)
        // The loot handler (func_1C50D0, the readable zoneEnterHandler(id,
        // player)): its eight key items add 500,000 (func_1B50C0(skater,
        // func_1B50B8(skater) + 500000)), its special tricks show "Special
        // trick unlocked!" (0x269478). The hooks know those calls by running
        // inside it (it is wrapped) and by their arguments, never by a return
        // address (a readable caller gives none for a callee it calls from
        // several places).
        constexpr uint32_t kZoneEnter = sym::zoneEnterHandler;
        constexpr uint32_t kKeyPoints = 500000u;
        constexpr uint32_t kTrickUnlockedText = sym::trickUnlockedText;
        constexpr uint32_t kSpecialTrickPoints = 300000u;

        struct Hooks
        {
            PS2Machine::RecompiledFunction zoneEnter = nullptr;
            uint32_t zonePlayer = 0; // zoneEnterHandler's player while it runs, else 0
            PS2Machine::RecompiledFunction setPoints = nullptr;
            PS2Machine::RecompiledFunction showMessage = nullptr;
        };
        Hooks &hooks() { return zx::session::state<Hooks>(); }

        void zoneEnterHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t outer = hooks().zonePlayer;
            hooks().zonePlayer = getRegU32(ctx, 5);
            hooks().zoneEnter(rdram, ctx, runtime);
            hooks().zonePlayer = outer;
        }

        // func_1B50C0 from a key item (inside the loot handler, the skater's
        // points + 500,000): no points on the Xbox.
        void setPointsHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t skater = getRegU32(ctx, 4);
            uint32_t points = 0;
            if (hooks().zonePlayer && skater &&
                runtime->callGuestFunction(rdram, ctx, kGetPoints, {skater}, getRegU32(ctx, 29), &points) &&
                getRegU32(ctx, 5) - points == kKeyPoints)
            {
                zge_host::sites::seen("ai.tuning.key_item");
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            hooks().setPoints(rdram, ctx, runtime);
        }

        // The special trick message: +300,000 for the skater, as the Xbox
        // gives right after it.
        void showMessageHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            if (hooks().zonePlayer && getRegU32(ctx, 4) == kTrickUnlockedText)
            {
                zge_host::sites::seen("ai.tuning.special_trick");
                const uint32_t skater = hooks().zonePlayer;
                const uint32_t sp = getRegU32(ctx, 29);
                uint32_t points = 0;
                if (skater && runtime->callGuestFunction(rdram, ctx, kGetPoints, {skater}, sp, &points))
                    runtime->callGuestFunction(rdram, ctx, kSetPoints, {skater, points + kSpecialTrickPoints}, sp);
            }
            hooks().showMessage(rdram, ctx, runtime);
        }

        // CIVIC.BIN (career script): DaveHelper04's threshold.
        bool patchCivic(std::vector<uint8_t> &bytes)
        {
            constexpr size_t kAt = 0xCEB0u;
            constexpr uint32_t kPs2 = 350000u, kXbox = 349999u;
            uint32_t value = 0;
            if (bytes.size() < kAt + 4u)
                return false;
            std::memcpy(&value, bytes.data() + kAt, 4);
            if (value != kPs2)
                return false; // another CIVIC.BIN (split screen's), or not the PS2 disc's
            std::memcpy(bytes.data() + kAt, &kXbox, 4);
            std::cout << "[tuning] CIVIC.BIN: the 350,000 point trick passes at 350,000, as on the Xbox" << std::endl;
            return true;
        }
    }

    void install(PS2Machine &runtime)
    {
        const char *which = zx::boot::text("ZX_TUNING");
        if (!which || std::string(which) != "xbox")
            return;
        if (!(hooks().setPoints = runtime.lookupFunction(kSetPoints)) || !(hooks().showMessage = runtime.lookupFunction(kShowMessage)) ||
            !(hooks().zoneEnter = runtime.lookupFunction(kZoneEnter)) ||
            !runtime.replaceFunction(kSetPoints, &setPointsHook) || !runtime.replaceFunction(kShowMessage, &showMessageHook) ||
            !runtime.replaceFunction(kZoneEnter, &zoneEnterHook))
        {
            std::cerr << "[tuning] the experience awards were not wrapped" << std::endl;
            return;
        }
        zx::disc_patch::registerPatch("CIVIC.BIN", &patchCivic, "ai");
        std::cout << "[tuning] the Xbox release's balance: special tricks +300,000, key loot none, Civic's 350,000 trick"
                  << std::endl;
    }
}

namespace aggressive_inline
{
    void installXboxTuning(PS2Machine &runtime)
    {
        tuning::install(runtime);
    }
}
