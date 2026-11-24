// Aggressive Inline (SLUS-20327): the ZAudio sound driver on the host
// (zge_sound.cpp, the three games' one driver) with AI's ZAUDIO.IRX, and
// AI's own extras: the French speech from the Xbox release and the game's
// volumes in the settings model.
//
// AI's ZAUDIO (the reference the other two were compared with): loaded at
// IOP 0x14B00, its sample table of 128 entries, Alloc replies the handle,
// ChanSetParam takes a sample's handle, a fade with flag 2 ends its stream;
// ZGE3's streams, each with its own SPU rings.
//
// ZX_SOUND=spu2: ZAUDIO on the IOP and the emulated SPU2 (docs/SPU2.md);
// ZX_MOVIES=hle needs it too (the movie sound's ring is the IOP's to drain).
#include "aggressive_inline_sound.h"
#include "sym/game_overrides/aggressive_inline_sound.cpp.h"

#include "aggressive_inline_language.h"
#include "game_overrides.h"
#include "machine.h"
#include "runtime/audio_file.h"
#include "runtime/game_files.h"
#include "runtime/session.h"
#include "runtime/xbox_audio.h"
#include "runtime/xbox_files.h"
#include "overrides/zge_sound.h"
#include "overrides/zge_volumes.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace aggressive_inline::native_sound
{
    namespace
    {
        namespace gf = zx::game_files;
        using zge_host::sound::Replacement;
        using zge_host::sound::StreamSource;

        // ---- French speech (the Xbox release's speech01.zsd) ----------------------------

        // A decoded line as a reader: mono, both channels the same.
        class PcmReader final : public zx::audio_file::Reader
        {
        public:
            explicit PcmReader(std::shared_ptr<const std::vector<int16_t>> pcm) : m_pcm(std::move(pcm)) {}
            size_t read(int16_t *out, size_t frames) override
            {
                const size_t n = std::min(frames, m_pcm->size() - m_at);
                for (size_t i = 0; i < n; ++i)
                    out[2 * i] = out[2 * i + 1] = (*m_pcm)[m_at + i];
                m_at += n;
                return n;
            }
            bool rewind() override
            {
                m_at = 0;
                return true;
            }
            uint32_t rate() const override { return 0; }
            uint64_t frames() const override { return m_pcm->size(); }

        private:
            std::shared_ptr<const std::vector<int16_t>> m_pcm;
            size_t m_at = 0;
        };

        // SPEECH.ZSD's waves (PS2) and speech01.zsd's (Xbox, French): the
        // same lines in the same order.
        struct FrenchSpeech
        {
            std::mutex mutex;
            bool tried = false;
            std::vector<uint8_t> bank;
            std::vector<zx::xbox_audio::Wave> waves;
            std::vector<zx::xbox_audio::Wave> ps2;
            std::map<uint32_t, std::shared_ptr<const std::vector<int16_t>>> decoded;
        };
        FrenchSpeech &frenchSpeech() { return zx::session::state<FrenchSpeech>(); }

        // The wave of the PS2 bank `file` starting in the sector at `start`,
        // or -1; loads both tables the first time.
        int frenchSpeechWave(gf::GameFile &file, uint64_t start)
        {
            FrenchSpeech &f = frenchSpeech();
            std::lock_guard<std::mutex> lock(f.mutex);
            if (!f.tried)
            {
                f.tried = true;
                std::vector<uint8_t> head(0x28);
                bool xbox = true;
                if (file.read(0, head.data(), head.size()) == head.size())
                {
                    const size_t end = zx::xbox_audio::waveTableEnd(head.data(), head.size());
                    head.resize(end);
                    if (end && file.read(0, head.data(), end) == end)
                        zx::xbox_audio::readWaveTable(head, f.ps2, &xbox);
                }
                if (!zx::xbox_files::readFile("audio/speech01.zsd", f.bank) ||
                    !zx::xbox_audio::readWaveTable(f.bank, f.waves, &xbox) || !xbox || f.waves.size() != f.ps2.size())
                {
                    std::cerr << "[sound] French speech not available (" << f.ps2.size() << " PS2 lines, "
                              << f.waves.size() << " Xbox)" << std::endl;
                    f.waves.clear();
                    f.bank.clear();
                }
                else
                {
                    std::cout << "[sound] French speech: " << f.waves.size() << " lines from the Xbox speech01.zsd"
                              << std::endl;
                }
            }
            if (f.waves.empty())
                return -1;
            for (size_t i = 0; i < f.ps2.size(); ++i)
                if ((static_cast<uint64_t>(f.ps2[i].offset) >> 11 << 11) == start)
                    return static_cast<int>(i);
            return -1;
        }

        std::shared_ptr<const std::vector<int16_t>> frenchSpeechLine(int wave)
        {
            FrenchSpeech &f = frenchSpeech();
            std::lock_guard<std::mutex> lock(f.mutex);
            if (wave < 0 || static_cast<size_t>(wave) >= f.waves.size())
                return nullptr;
            auto &slot = f.decoded[static_cast<uint32_t>(wave)];
            if (!slot)
            {
                const auto &w = f.waves[static_cast<size_t>(wave)];
                if (static_cast<uint64_t>(w.offset) + w.size > f.bank.size())
                    return nullptr;
                slot = std::make_shared<const std::vector<int16_t>>(
                    zx::xbox_audio::decodeAdpcm(f.bank.data() + w.offset, w.size));
            }
            return slot;
        }

        // A SPEECH.ZSD line in French when the French voices are chosen: the
        // Xbox line at the game's pitch (both banks have the same rates).
        Replacement frenchLine(const StreamSource &source)
        {
            Replacement replacement;
            if (source.bus != zge_host::sound::Speech || !source.file || !aggressive_inline::language::frenchVoices())
                return replacement;
            if (auto pcm = frenchSpeechLine(frenchSpeechWave(*source.file, source.start)))
                replacement.reader = std::make_unique<PcmReader>(std::move(pcm));
            return replacement;
        }

        // ---- the game's volumes in the settings model ------------------------------------

        // The game scales everything it sends by its own SFX, music and
        // speech volumes (0-127, three bytes from g_masterVolume, in its
        // save; set by soundEvent's event 0, which also re-applies them to
        // what is playing): zge_volumes.cpp shows them as rows and passes a
        // change to event 0 from the ZAudio per-frame task (soundTask, main
        // thread, every frame), as its Options > Sound page does. Before the
        // game runs (g_gameMode -1) there is no sound yet: just the byte.
        constexpr zge_host::volumes::Volume kVolumes[] = {
            {"audio.music_volume", "Music Volume", "The game's own music volume.", 1u, 0u,
             sym::g_masterVolume + 1u},
            {"audio.effects_volume", "Effects Volume", "The game's own effects volume.", 0u, 0u,
             sym::g_masterVolume + 0u},
            {"audio.speech_volume", "Speech Volume", "The game's own speech volume.", 2u, 0u,
             sym::g_masterVolume + 2u},
        };
        constexpr zge_host::volumes::Game kGameVolumes{
            .tag = "[aggressive-inline]",
            .frame = sym::soundTask,
            .soundEvent = sym::soundEvent,
            .fullVolume = 127,
            .soundMode = sym::g_gameMode,
            .volumes = kVolumes,
            .count = 3u,
        };

        // ---- AI's ZAUDIO ----------------------------------------------------------------

        constexpr uint32_t kZaudioBase = 0x00014B00u; // where AI's IOP loads ZAUDIO.IRX
        constexpr zge_host::sound::Game kSound{
            .rpc = sym::soundRpc,
            .bind = sym::bindSoundRpc,
            .replyBuffer = sym::soundReply,
            .sampleTable = kZaudioBase + 0x99A0u,
            .streamTable = kZaudioBase + 0x64D0u,
            .streamStride = 0x90u,
            .sampleSlots = 128,
            .replyHasSpuAddress = false,
            .channelSample = zge_host::sound::ChannelSample::Handle,
            .fadeEndStops = 2u,
            .fadeEndHolds = 0u,
            .rings = zge_host::sound::StreamRings::PerStream,
            // The playlist's 13 songs, MUSIC.ZSD's first waves; the pause
            // menu's Sound Options shows their titles.
            .songs = 13,
            .setPlaylistEntry = sym::setPlaylistEntry,
            .gameSoundtrack = sym::g_music,
            .resolve = &frenchLine,
        };
    }
}

namespace aggressive_inline
{
    bool installNativeSound(PS2Machine &runtime)
    {
        using namespace native_sound;
        // The game's volumes in the settings model, with either driver.
        zge_host::volumes::install(runtime, kGameVolumes);
        const bool native = zge_host::sound::install(runtime, kSound);
        // The soundtrack's controls, with either driver (after it: the songs mods add are the native one's).
        installSoundtrack(runtime);
        return native;
    }
}
