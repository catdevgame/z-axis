// The game's own volumes in the settings model (zge_volumes.h), one body for
// the games built on the Z-Axis engine.

#include "zge_volumes.h"

#include "machine.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>

namespace zge_host::volumes
{
    namespace
    {
        constexpr uint32_t kSetVolumeEvent = 0u;
        constexpr uint32_t kSoundNotUp = 0xFFFFFFFFu;
        constexpr int kRegSp = 29;
        constexpr uint32_t kMaxVolumes = 4u;

        struct Volumes
        {
            const Game *game = nullptr;
            std::atomic<uint8_t *> rdram{nullptr};
            std::array<std::atomic<int>, kMaxVolumes> pending{-1, -1, -1, -1}; // percent, by row
            PS2Machine::RecompiledFunction frame = nullptr;
        };
        Volumes &volumes() { return zx::session::state<Volumes>(); }

        // The byte the game's options show.
        uint32_t shown(const Volume &volume) { return volume.saved ? volume.saved : volume.heard; }

        // The row's value: a change not yet passed to the game is the row's
        // (the settings model reads this right after its apply, on the window
        // thread, before the game's next frame has written the bytes), else
        // the byte the game's options show.
        int percent(const Game &game, uint32_t row)
        {
            if (const int asked = volumes().pending[row].load(); asked >= 0)
                return asked;
            const Volume &volume = game.volumes[row];
            uint8_t *rdram = volumes().rdram.load(std::memory_order_acquire);
            const uint8_t *p = rdram ? getConstMemPtr(rdram, shown(volume)) : nullptr;
            return p ? (std::min<int>(*p, game.fullVolume) * 100 + game.fullVolume / 2) / game.fullVolume : 100;
        }

        void writeByte(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = address ? getMemPtr(rdram, address) : nullptr)
                *p = static_cast<uint8_t>(value);
        }

        bool soundIsUp(uint8_t *rdram, const Game &game)
        {
            uint32_t mode = 0;
            if (const uint8_t *p = game.soundMode ? getConstMemPtr(rdram, game.soundMode) : nullptr)
                std::memcpy(&mode, p, sizeof(mode));
            return mode != kSoundNotUp;
        }

        // The game's frame function: a changed row passed to the game as its
        // options menu passes it, then the game's own frame.
        void frame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Volumes &v = volumes();
            const Game &game = *v.game;
            v.rdram.store(rdram, std::memory_order_release);
            for (uint32_t row = 0; row < game.count; ++row)
            {
                int wanted = v.pending[row].load();
                if (wanted < 0)
                    continue;
                const Volume &volume = game.volumes[row];
                const uint32_t value =
                    static_cast<uint32_t>((std::clamp(wanted, 0, 100) * game.fullVolume + 50) / 100);
                if (uint8_t *p = volume.menuWord ? getMemPtr(rdram, volume.menuWord) : nullptr)
                    std::memcpy(p, &value, sizeof(value));
                writeByte(rdram, volume.saved, value);
                if (!soundIsUp(rdram, game))
                    writeByte(rdram, volume.heard, value); // the event would do nothing yet: just the setting
                else
                {
                    runtime->callGuestFunction(rdram, ctx, game.soundEvent,
                                               {kSetVolumeEvent, volume.kind, value, 0u, 0u},
                                               (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu);
                    std::cout << game.tag << " " << volume.label << " " << value << " of " << game.fullVolume
                              << std::endl;
                }
                // Passed: the row is the bytes again, unless another change came meanwhile (the next frame's).
                v.pending[row].compare_exchange_strong(wanted, -1);
            }
            v.frame(rdram, ctx, runtime);
        }

        void addRows()
        {
            namespace st = zx::settings;
            const Game &game = *volumes().game;
            for (uint32_t row = 0; row < game.count; ++row)
            {
                const Volume &volume = game.volumes[row];
                st::Setting setting;
                setting.key = volume.key;
                setting.label = volume.label;
                setting.kind = st::Kind::Range;
                setting.minValue = 0;
                setting.maxValue = 100;
                setting.step = 10;
                setting.unit = "%";
                setting.help = volume.help;
                setting.current = [&game, row]() { return percent(game, row); };
                setting.apply = [row](int value) { volumes().pending[row].store(std::clamp(value, 0, 100)); };
                zx::session::addSetting(setting);
            }
        }
    }

    bool install(PS2Machine &runtime, const Game &game)
    {
        Volumes &v = volumes();
        if (game.count > kMaxVolumes)
            return false;
        v.game = &game;
        v.rdram = runtime.memory().getRDRAM();
        v.frame = runtime.lookupFunction(game.frame);
        if (!v.frame || !runtime.replaceFunction(game.frame, &frame))
        {
            std::cerr << game.tag << " the volume rows' hook not installed" << std::endl;
            return false;
        }
        zx::settings::post(&addRows);
        return true;
    }
}
