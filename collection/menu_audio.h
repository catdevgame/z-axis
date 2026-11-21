#pragma once

// The collection menu's sound: the chosen game's own front-end music under
// its card, crossfaded when the choice moves, and the game's own move,
// select and back sounds (collection/sound_bank.h says where each game keeps
// them). Everything is read from the game's disc folder (<support>/games/
// <id>/disc); a game whose files are not there is silent.
//
// render() makes 48 kHz stereo on whatever thread the app's audio output
// calls it from; the other calls come from the menu's thread. One mutex
// keeps them apart (render holds it for one buffer).

#include "collection/sound_bank.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace zx::movie
{
    class PssAudio;
}

namespace zx::collection
{
    class MenuAudio
    {
    public:
        static constexpr int kRate = 48000;
        // The crossfade between two games' music, and the fade in and out.
        static constexpr size_t kFadeFrames = kRate / 2;
        // How many menu sounds can overlap (the oldest is cut).
        static constexpr size_t kSoundVoices = 4;

        MenuAudio();
        ~MenuAudio();

        // Reads the game's front-end music and menu sounds from its disc
        // folder. False with *error (the game stays silent) when its banks
        // are missing or are not banks.
        bool load(std::string_view game, const std::filesystem::path &disc, std::string *error = nullptr);
        bool loaded(std::string_view game) const;

        // The game's music from its start, faded in over the music playing
        // now (faded out); "" or a game not loaded fades the music out.
        // The same game again leaves it playing.
        void playMusic(std::string_view game);
        // One of the game's menu sounds (nothing for a game not loaded).
        void play(std::string_view game, MenuSound sound);

        // A movie's sound (the intro's, 48 kHz stereo) over the menu's from
        // now, for `frames` frames (0: to its end), its last tenth of a
        // second faded out; null stops the one playing.
        void playMovie(std::shared_ptr<zx::movie::PssAudio> sound, size_t frames = 0);
        bool moviePlaying() const;

        // Levels 0..127 (the games' own volume scale), applied as the games
        // apply theirs: a share of each sample's own volume.
        void setLevels(uint32_t music, uint32_t effects);

        // Interleaved stereo frames (left, right), `frames` of them,
        // overwritten.
        void render(int16_t *out, size_t frames);

        // The music's decoded loop for a loaded game (its pcm and loopPcm at
        // BankSound::rate()), nullptr otherwise: for tests and the setup
        // page's check.
        const BankSound *music(std::string_view game) const;

    private:
        struct GameSound
        {
            BankSound music;
            std::vector<BankSound> sounds; // by MenuSound
        };
        struct MusicSlot
        {
            zx::spu::Voice voice;
            std::string game;
            int32_t gain = 0;      // 0..kFadeFrames
            int32_t direction = 0; // +1 fading in, -1 fading out, 0 steady
        };

        void startMusic(MusicSlot &slot, const std::string &game);
        void applyLevels();

        mutable std::mutex m_mutex;
        std::shared_ptr<zx::movie::PssAudio> m_movie;
        size_t m_movieLeft = 0; // frames of it still to play (SIZE_MAX: to its end)
        std::vector<int16_t> m_movieBuffer;
        std::map<std::string, GameSound, std::less<>> m_games;
        MusicSlot m_music[2];
        size_t m_current = 0; // the slot of the music fading in or playing
        zx::spu::Voice m_sounds[kSoundVoices];
        size_t m_nextSound = 0;
        uint32_t m_musicLevel = 127, m_effectsLevel = 127;
    };
}
