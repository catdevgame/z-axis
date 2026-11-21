// The collection menu's sound (collection/menu_audio.h).
#include "collection/menu_audio.h"

#include "runtime/ps2_pss_movie.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace zx::collection
{
    namespace
    {
        // A menu sound: its wave once. The banks end each one with a block
        // that loops on itself (flags 7), which the games' sounds never
        // reach audibly; played once, the voice is free at the wave's end.
        class OnceSource final : public zx::spu::BlockSource
        {
        public:
            explicit OnceSource(std::shared_ptr<const zx::spu::DecodedSample> sample) : m_sample(std::move(sample)) {}
            bool nextBlock(int16_t *out) override
            {
                const std::vector<int16_t> &pcm = m_sample->pcm;
                if (m_position + zx::spu::kSamplesPerBlock > pcm.size())
                    return false;
                std::memcpy(out, pcm.data() + m_position, zx::spu::kSamplesPerBlock * sizeof(int16_t));
                m_position += zx::spu::kSamplesPerBlock;
                return true;
            }

        private:
            std::shared_ptr<const zx::spu::DecodedSample> m_sample;
            size_t m_position = 0;
        };

        bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        void setUp(zx::spu::Voice &voice, const BankSound &sound, uint32_t level)
        {
            uint16_t left = 0, right = 0;
            sound.voiceVolumes(level, left, right);
            voice.setPitch(sound.voicePitch);
            voice.setVolume(left, right);
            voice.setEnvelope(sound.adsr1, sound.adsr2);
        }
    }

    MenuAudio::MenuAudio() = default;
    MenuAudio::~MenuAudio() = default;

    bool MenuAudio::load(std::string_view game, const std::filesystem::path &disc, std::string *error)
    {
        const MenuSoundSource *source = menuSoundSource(game);
        BankLayout layout{};
        if (!source || !bankLayoutOf(game, layout))
        {
            if (error)
                *error = "no menu sound known for this game";
            return false;
        }
        std::vector<uint8_t> file;
        std::vector<BankSound> music, common;
        std::string why;
        if (!readFile(disc / source->musicBank, file))
            why = std::string(source->musicBank) + " not found";
        else if (!readSoundBank(file, layout, music, &why))
            why = std::string(source->musicBank) + ": " + why;
        else if (source->musicSample >= music.size() || !music[source->musicSample].wave)
            why = std::string(source->musicBank) + ": no music sample";
        else if (!readFile(disc / source->soundBank, file))
            why = std::string(source->soundBank) + " not found";
        else if (!readSoundBank(file, layout, common, &why))
            why = std::string(source->soundBank) + ": " + why;
        if (!why.empty())
        {
            if (error)
                *error = why;
            return false;
        }
        GameSound sound;
        sound.music = music[source->musicSample];
        for (const uint32_t index : source->sounds)
            sound.sounds.push_back(index < common.size() ? common[index] : BankSound{});
        std::lock_guard lock(m_mutex);
        m_games.insert_or_assign(std::string(game), std::move(sound));
        return true;
    }

    bool MenuAudio::loaded(std::string_view game) const
    {
        std::lock_guard lock(m_mutex);
        return m_games.find(game) != m_games.end();
    }

    const BankSound *MenuAudio::music(std::string_view game) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_games.find(game);
        return it == m_games.end() ? nullptr : &it->second.music;
    }

    void MenuAudio::startMusic(MusicSlot &slot, const std::string &game)
    {
        slot.game = game;
        const auto it = m_games.find(game);
        if (it == m_games.end() || !it->second.music.wave)
        {
            slot.voice.stop();
            return;
        }
        setUp(slot.voice, it->second.music, m_musicLevel);
        slot.voice.keyOn(std::make_unique<zx::spu::SampleSource>(it->second.music.wave));
    }

    void MenuAudio::playMusic(std::string_view game)
    {
        std::lock_guard lock(m_mutex);
        MusicSlot &current = m_music[m_current];
        if (!game.empty() && current.game == game && current.direction >= 0 && current.voice.active())
            return;
        // What plays now fades out; the new music fades in on the other slot.
        current.direction = -1;
        m_current ^= 1u;
        MusicSlot &next = m_music[m_current];
        next.gain = 0;
        next.direction = game.empty() ? 0 : 1;
        if (game.empty())
        {
            next.voice.stop();
            next.game.clear();
            return;
        }
        startMusic(next, std::string(game));
    }

    void MenuAudio::play(std::string_view game, MenuSound sound)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_games.find(game);
        if (it == m_games.end())
            return;
        const BankSound &s = it->second.sounds[static_cast<size_t>(sound)];
        if (!s.wave)
            return;
        zx::spu::Voice &voice = m_sounds[m_nextSound];
        m_nextSound = (m_nextSound + 1u) % kSoundVoices;
        setUp(voice, s, m_effectsLevel);
        voice.keyOn(std::make_unique<OnceSource>(s.wave));
    }

    void MenuAudio::playMovie(std::shared_ptr<zx::movie::PssAudio> sound, size_t frames)
    {
        std::lock_guard lock(m_mutex);
        m_movie = std::move(sound);
        m_movieLeft = !m_movie ? 0 : frames == 0 ? std::numeric_limits<size_t>::max() : frames;
    }

    bool MenuAudio::moviePlaying() const
    {
        std::lock_guard lock(m_mutex);
        return m_movie != nullptr;
    }

    void MenuAudio::setLevels(uint32_t music, uint32_t effects)
    {
        std::lock_guard lock(m_mutex);
        m_musicLevel = std::min<uint32_t>(music, 127u);
        m_effectsLevel = std::min<uint32_t>(effects, 127u);
        applyLevels();
    }

    void MenuAudio::applyLevels()
    {
        for (MusicSlot &slot : m_music)
        {
            const auto it = m_games.find(slot.game);
            if (it == m_games.end())
                continue;
            uint16_t left = 0, right = 0;
            it->second.music.voiceVolumes(m_musicLevel, left, right);
            slot.voice.setVolume(left, right);
        }
    }

    void MenuAudio::render(int16_t *out, size_t frames)
    {
        std::lock_guard lock(m_mutex);
        constexpr int32_t kFull = static_cast<int32_t>(kFadeFrames);
        for (size_t f = 0; f < frames; ++f)
        {
            int64_t left = 0, right = 0;
            for (MusicSlot &slot : m_music)
            {
                if (slot.direction != 0)
                {
                    slot.gain = std::clamp(slot.gain + slot.direction, 0, kFull);
                    if (slot.gain == kFull && slot.direction > 0)
                        slot.direction = 0;
                    if (slot.gain == 0 && slot.direction < 0)
                    {
                        slot.direction = 0;
                        slot.voice.stop();
                        slot.game.clear();
                    }
                }
                if (!slot.voice.active() || slot.gain == 0)
                    continue;
                int32_t l = 0, r = 0;
                slot.voice.mix(l, r);
                left += static_cast<int64_t>(l) * slot.gain / kFull;
                right += static_cast<int64_t>(r) * slot.gain / kFull;
            }
            for (zx::spu::Voice &voice : m_sounds)
            {
                int32_t l = 0, r = 0;
                voice.mix(l, r);
                left += l;
                right += r;
            }
            out[2 * f] = static_cast<int16_t>(std::clamp<int64_t>(left, -32768, 32767));
            out[2 * f + 1] = static_cast<int16_t>(std::clamp<int64_t>(right, -32768, 32767));
        }
        if (!m_movie)
            return;
        // The movie's sound over it, as the game plays it, faded at its end.
        const size_t wanted = std::min(frames, m_movieLeft);
        m_movieBuffer.assign(wanted * 2, 0);
        const size_t got = m_movie->read(m_movieBuffer.data(), wanted);
        constexpr size_t kTail = kRate / 10;
        for (size_t f = 0; f < got; ++f)
        {
            const size_t left = m_movieLeft - f;
            const int32_t gain = left < kTail ? static_cast<int32_t>(left * 256 / kTail) : 256;
            for (size_t c = 0; c < 2; ++c)
            {
                const int32_t mixed = out[2 * f + c] + m_movieBuffer[2 * f + c] * gain / 256;
                out[2 * f + c] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
            }
        }
        m_movieLeft -= got;
        if (got < frames || m_movieLeft == 0)
        {
            m_movie.reset();
            m_movieLeft = 0;
        }
    }
}
