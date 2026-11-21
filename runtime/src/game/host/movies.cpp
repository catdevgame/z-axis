// The host's services to the engine's movie player on the host (movies.h).

#include "movies.h"
#include "ee/host_form.h"

#include "ps2_runtime.h"
#include "runtime/ps2_audio_file.h"
#include "runtime/ps2_audio_mix.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_pss_movie.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_texture_replace.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace game::host::movies
{
    namespace
    {
        namespace gf = zx::game_files;
        namespace mv = zx::movie;

        constexpr double kVsyncHz = 60000.0 / 1001.0;
        // At most this many late pictures dropped for one shown.
        constexpr int kMaxDropPerStep = 30;
        // The full-size pictures kept for the GPU renderer: the game's two
        // decoded pictures and the one on screen.
        constexpr size_t kFullPicturesKept = 3u;

        // A movie's sound as 48 kHz stereo frames.
        struct MovieAudio
        {
            virtual ~MovieAudio() = default;
            virtual bool hasSound() = 0;
            virtual size_t read(int16_t *stereo, size_t frames) = 0;
            virtual void rewind() = 0;
        };

        struct PssSound final : MovieAudio
        {
            explicit PssSound(std::shared_ptr<gf::GameFile> file) : audio(std::move(file)) {}
            bool hasSound() override { return audio.hasSound(); }
            size_t read(int16_t *stereo, size_t frames) override { return audio.read(stereo, frames); }
            void rewind() override { audio.rewind(); }
            mv::PssAudio audio;
        };

        // The sound of a movie file FFmpeg reads (Bink), resampled to 48 kHz
        // (linear) if it has another rate.
        struct FileSound final : MovieAudio
        {
            explicit FileSound(std::unique_ptr<zx::audio_file::Reader> r) : reader(std::move(r)) {}
            bool hasSound() override { return reader != nullptr; }
            size_t read(int16_t *stereo, size_t frames) override
            {
                if (!reader)
                    return 0;
                const uint32_t rate = reader->rate();
                if (rate == mv::PssAudio::kOutputRate)
                    return reader->read(stereo, frames);
                const double step = static_cast<double>(rate) / mv::PssAudio::kOutputRate;
                size_t out = 0;
                while (out < frames)
                {
                    while (phase + 1.0 >= static_cast<double>(have))
                    {
                        // Keep the last frame, read more after it.
                        if (have > 0)
                        {
                            buffer[0] = buffer[(have - 1) * 2];
                            buffer[1] = buffer[(have - 1) * 2 + 1];
                            phase -= static_cast<double>(have - 1);
                            have = 1;
                        }
                        buffer.resize(2 + 2048 * 2);
                        const size_t got = reader->read(buffer.data() + have * 2, 2048);
                        if (got == 0)
                            return out;
                        have += got;
                        buffer.resize(have * 2);
                    }
                    const size_t i = static_cast<size_t>(phase);
                    const double f = phase - static_cast<double>(i);
                    for (int c = 0; c < 2; ++c)
                        stereo[out * 2 + c] = static_cast<int16_t>(
                            std::lround(buffer[i * 2 + c] * (1.0 - f) + buffer[(i + 1) * 2 + c] * f));
                    ++out;
                    phase += step;
                }
                return out;
            }
            void rewind() override
            {
                if (reader)
                    reader->rewind();
                buffer.clear();
                have = 0;
                phase = 0.0;
            }
            std::unique_ptr<zx::audio_file::Reader> reader;
            std::vector<int16_t> buffer;
            size_t have = 0;
            double phase = 0.0;
        };

        struct Movie
        {
            std::string path;
            std::shared_ptr<gf::GameFile> file;
            std::unique_ptr<mv::VideoSource> video;
            std::unique_ptr<MovieAudio> audio;
            mv::FileVideo *xbox = nullptr; // video, when it is a host file (the Xbox movie, a host movie)
            std::deque<zx::texture_replace::Entry> frames; // the last full-size pictures shown
            mv::Picture picture; // the one nextPicture gave
            uint64_t startTick = 0u;
            uint64_t shown = 0u;
            bool skip = false; // Skip Intro Movies: ends at its first frame
        };

        // One boot's player: its game, the movie playing, its files and its
        // sound.
        struct State
        {
            Game game{};
            Movie movie;
            // Which of the game's intro movies have played (or been skipped) once.
            bool introSeen[Game::kIntroMovies] = {};
            std::unique_ptr<gf::GameFileSystem> files;
            std::mutex soundMutex; // the sound source runs where the SPU2's frames are made
            bool soundPlaying = false;
            float soundGain = 1.0f;
            int soundSource = 0; // the boot's mix source (removed when the boot ends)
            // setHostMovie's: by the game's path (normalized), the host file
            // and the size the game gets its pictures at.
            struct HostMovie
            {
                std::filesystem::path file;
                int width = 0, height = 0;
            };
            std::map<std::string, HostMovie> hostMovies;
            bool trace = false;
        };

        State &state() { return zx::session::state<State>(); }

        bool skipAsIntro(const std::string &path)
        {
            State &st = state();
            std::string name = path.substr(path.find_last_of("/\\:") + 1u);
            name = name.substr(0, name.find(';'));
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (size_t i = 0; i < Game::kIntroMovies && st.game.introMovies[i]; ++i)
                if (name == st.game.introMovies[i] && !st.introSeen[i])
                {
                    st.introSeen[i] = true;
                    return zx::settings::value("game.skip_intro_movies", 0) != 0;
                }
            return false;
        }

        gf::GameFileSystem &files()
        {
            std::unique_ptr<gf::GameFileSystem> &fileSystem = state().files;
            if (!fileSystem)
            {
                const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
                gf::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Runtime::cdImageFileSystem();
                if (const char *mods = zx::boot::text("ZX_MODS_DIR"); mods && *mods)
                    roots.mods = gf::modsRoots(mods);
                fileSystem = std::make_unique<gf::GameFileSystem>(std::move(roots));
            }
            return *fileSystem;
        }

        // ---- sound -------------------------------------------------------------

        void mixSound(int32_t *mix, size_t frames)
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.soundMutex);
            if (!st.soundPlaying || !st.movie.audio)
                return;
            thread_local std::vector<int16_t> pcm;
            pcm.resize(frames * 2u);
            const size_t got = st.movie.audio->read(pcm.data(), frames);
            for (size_t i = 0; i < got * 2u; ++i)
                mix[i] += static_cast<int32_t>(std::lround(pcm[i] * st.soundGain));
            if (got < frames)
                st.soundPlaying = false;
        }

        void setSoundPlaying(bool playing)
        {
            State &st = state();
            std::lock_guard<std::mutex> lock(st.soundMutex);
            st.soundPlaying = playing && st.movie.audio && st.movie.audio->hasSound();
        }

        // ---- Xbox movies ------------------------------------------------------------

        constexpr const char *kXboxMoviesKey = "display.xbox_movies";
        struct XboxMovies
        {
            std::atomic<int> on{1}; // the developer runner's row (the app has its own)
            std::filesystem::path folder; // <pack>/movies, if any
        };
        XboxMovies &xbox() { return zx::session::state<XboxMovies>(); }

        bool xboxMoviesOn()
        {
            if (zx::settings::has(kXboxMoviesKey))
                return zx::settings::value(kXboxMoviesKey, 1) != 0;
            return xbox().on.load() != 0;
        }

        // The Bink file for a PS2 movie, played at the PS2 movie's size.
        void openXbox(Movie &movie)
        {
            if (xbox().folder.empty() || !xboxMoviesOn())
                return;
            const std::filesystem::path bink = xbox().folder / (gf::normalizePath(movie.path) + ".bik");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(bink, ec))
                return;
            // The PS2 movie's picture size: what the game's buffers and texture are made for.
            mv::Picture first;
            {
                mv::PssVideo ps2(movie.file);
                if (!ps2.next(first))
                    return;
            }
            auto video = std::make_unique<mv::FileVideo>(bink);
            if (!video->ok())
                return;
            video->setScaledSize(first.width, first.height);
            movie.xbox = video.get();
            movie.video = std::move(video);
            std::string error;
            movie.audio = std::make_unique<FileSound>(zx::audio_file::open(bink, &error));
            std::cout << "[movie] " << movie.path << ": Xbox movie " << movie.xbox->width() << "x" << movie.xbox->height()
                      << " (" << first.width << "x" << first.height << " for the game)"
                      << (movie.audio->hasSound() ? "" : ", no sound") << std::endl;
        }

        // The full-size picture of the frame just made, as the GPU renderer's
        // replacement for the scaled one the game uploads.
        void registerFrame(const mv::Picture &shown)
        {
            if (!state().movie.xbox)
                return;
            namespace tr = zx::texture_replace;
            const mv::Picture &full = state().movie.xbox->full();
            if (full.width == shown.width && full.height == shown.height)
                return; // nothing to gain (Z-AXIS is 512x256 on the Xbox too)
            auto picture = std::make_shared<tr::Picture>();
            picture->width = static_cast<uint32_t>(full.width);
            picture->height = static_cast<uint32_t>(full.height);
            picture->format = tr::Format::RGBA8;
            picture->data = full.rgba;
            picture->name = "movie";
            tr::Entry entry;
            entry.width = static_cast<uint32_t>(shown.width);
            entry.height = static_cast<uint32_t>(shown.height);
            entry.rgb = true;
            entry.indexHash = tr::rgbHash(reinterpret_cast<const uint32_t *>(shown.rgba.data()),
                                          static_cast<uint32_t>(shown.width), static_cast<uint32_t>(shown.height));
            entry.picture = std::move(picture);
            if (state().trace)
                std::cout << "[movie] frame " << state().movie.shown << " " << entry.width << "x" << entry.height << " hash " << std::hex
                          << entry.indexHash << std::dec << std::endl;
            state().movie.frames.push_back(std::move(entry));
            while (state().movie.frames.size() > kFullPicturesKept)
                state().movie.frames.pop_front();
            tr::setSource("movie", std::vector<tr::Entry>(state().movie.frames.begin(), state().movie.frames.end()));
        }
    }

    void configure(const Game &game, bool trace)
    {
        State &st = state();
        st.trace = trace;
        st.game = game;
    }

    void attach()
    {
        State &st = state();
        // The movie's sound (mixed where the SPU2's frames are made) and its
        // pictures (texture replacements) leave with the game.
        st.soundSource = zx::audio_mix::addSource(&mixSound);
        zx::session::onEnd([]()
        {
            zx::audio_mix::removeSource(state().soundSource);
            if (state().game.xboxMovies || !state().hostMovies.empty())
                zx::texture_replace::removeSource("movie");
        });
        if (const char *pack = zx::boot::text("ZX_XBOX_PACK"); st.game.xboxMovies && pack && pack[0])
        {
            std::error_code ec;
            const std::filesystem::path movies = std::filesystem::path(pack) / "movies";
            if (std::filesystem::is_directory(movies, ec))
            {
                xbox().folder = movies;
                const char *on = zx::boot::text("ZX_XBOX_MOVIES");
                xbox().on = (on && on[0] == '0') ? 0 : 1;
                std::cout << "[movie] Xbox movies in " << movies.string() << " (" << (xboxMoviesOn() ? "on" : "off")
                          << ")" << std::endl;
                // The app adds the row (with settings.ini); the developer runner gets this one.
                if (!zx::settings::has(kXboxMoviesKey))
                {
                    zx::settings::Setting row;
                    row.key = kXboxMoviesKey;
                    row.label = "Xbox Movies";
                    row.help = "Sharper movies.";
                    row.current = []() { return xbox().on.load(); };
                    row.apply = [](int value) { xbox().on = value != 0 ? 1 : 0; };
                    zx::session::addSetting(row);
                }
            }
        }
    }

    bool tracing() { return state().trace; }

    void setHostMovie(const std::string &gamePath, const std::string &file, int width, int height)
    {
        State &st = state();
        State::HostMovie &movie = st.hostMovies[gf::normalizePath(gamePath)];
        movie.file = std::filesystem::path(std::u8string(file.begin(), file.end()));
        movie.width = width;
        movie.height = height;
        std::cout << "[movie] " << gf::normalizePath(gamePath) << " plays " << file << std::endl;
    }

    bool playedOnHost()
    {
        const char *mode = std::getenv("ZX_MOVIES");
        return !(mode && std::string(mode) == "hle");
    }

    bool open(const std::string &path)
    {
        State &st = state();
        Movie movie;
        movie.path = path;
        movie.skip = skipAsIntro(path);
        const auto host = st.hostMovies.find(gf::normalizePath(path));
        if (host != st.hostMovies.end())
        {
            // Another release's movie as its disc has it, at the size the
            // game's PS2 movies have.
            auto video = std::make_unique<mv::FileVideo>(host->second.file);
            if (video->ok())
            {
                video->setScaledSize(host->second.width, host->second.height);
                movie.xbox = video.get();
                movie.video = std::move(video);
                std::string error;
                movie.audio = std::make_unique<FileSound>(zx::audio_file::open(host->second.file, &error));
                std::cout << "[movie] " << movie.path << ": " << host->second.file.filename().string() << " "
                          << movie.xbox->width() << "x" << movie.xbox->height() << " (" << host->second.width << "x"
                          << host->second.height << " for the game)" << (movie.audio->hasSound() ? "" : ", no sound")
                          << std::endl;
            }
            else
                std::cerr << "[movie] cannot play " << host->second.file.string() << " for " << path << std::endl;
        }
        else
            movie.file = files().open(path);
        if (movie.file)
        {
            openXbox(movie);
            if (!movie.video)
            {
                movie.video = std::make_unique<mv::PssVideo>(movie.file);
                movie.audio = std::make_unique<PssSound>(movie.file);
            }
        }
        else if (!movie.video)
        {
            std::cerr << "[movie] cannot open " << path << std::endl;
        }
        endPictures();
        const bool opened = movie.file != nullptr || movie.video != nullptr;
        {
            std::lock_guard<std::mutex> lock(st.soundMutex);
            st.soundPlaying = false;
            st.movie = std::move(movie);
        }
        return opened;
    }

    void playAgain()
    {
        State &st = state();
        if (st.movie.video)
            st.movie.video->rewind();
        std::lock_guard<std::mutex> lock(st.soundMutex);
        st.soundPlaying = false;
        if (st.movie.audio)
            st.movie.audio->rewind();
    }

    bool skipped() { return state().movie.skip; }
    const std::string &path() { return state().movie.path; }

    void startPictures(uint64_t vsync)
    {
        state().movie.startTick = vsync;
        state().movie.shown = 0u;
    }

    bool hasPictures() { return state().movie.video != nullptr; }

    Next nextPicture(uint64_t vsync)
    {
        Movie &movie = state().movie;
        const double interval = kVsyncHz / movie.video->framesPerSecond();
        auto due = [&](uint64_t frame)
        { return movie.startTick + static_cast<uint64_t>(std::floor(static_cast<double>(frame) * interval + 1e-6)); };
        if (vsync < due(movie.shown))
            return Next::notYet;
        mv::Picture picture;
        if (!movie.video->next(picture))
            return Next::ended;
        ++movie.shown;
        int dropped = 0;
        while (dropped < kMaxDropPerStep && vsync >= due(movie.shown))
        {
            mv::Picture later;
            if (!movie.video->next(later))
                break;
            picture = std::move(later);
            ++movie.shown;
            ++dropped;
        }
        if (state().trace && dropped)
            std::cout << "[movie] " << dropped << " pictures dropped at " << movie.shown << std::endl;
        registerFrame(picture);
        movie.picture = std::move(picture);
        return Next::picture;
    }

    uint32_t pictureWidth() { return static_cast<uint32_t>(state().movie.picture.width); }
    uint32_t pictureHeight() { return static_cast<uint32_t>(state().movie.picture.height); }

    void writePicture(uint32_t address)
    {
        constexpr uint32_t kColumnPixels = 16u, kRowsRoundedTo = 16u, kBytesPerPixel = 4u;
        constexpr uint8_t kAlpha = 0x80u;
        uint8_t *rdram = currentCall().rdram;
        const mv::Picture &picture = state().movie.picture;
        const uint32_t width = static_cast<uint32_t>(picture.width);
        const uint32_t height = static_cast<uint32_t>(picture.height);
        const uint32_t outHeight = (height + kRowsRoundedTo - 1u) & ~(kRowsRoundedTo - 1u);
        const uint32_t columns = (width + kColumnPixels - 1u) / kColumnPixels;
        uint8_t row[kColumnPixels * kBytesPerPixel];
        for (uint32_t column = 0; column < columns; ++column)
        {
            for (uint32_t y = 0; y < outHeight; ++y)
            {
                for (uint32_t x = 0; x < kColumnPixels; ++x)
                {
                    const uint32_t sx = column * kColumnPixels + x;
                    uint8_t *d = row + x * kBytesPerPixel;
                    if (sx < width && y < height)
                    {
                        const uint8_t *s = picture.rgba.data() + (static_cast<size_t>(y) * width + sx) * kBytesPerPixel;
                        d[0] = s[0];
                        d[1] = s[1];
                        d[2] = s[2];
                    }
                    else
                    {
                        d[0] = d[1] = d[2] = 0u;
                    }
                    d[3] = kAlpha;
                }
                const uint32_t at = address + (column * outHeight + y) * static_cast<uint32_t>(sizeof(row));
                if (uint8_t *p = getMemPtr(rdram, at))
                    std::memcpy(p, row, sizeof(row));
            }
        }
    }

    uint64_t picturesShown() { return state().movie.shown; }

    void endPictures()
    {
        if (state().movie.frames.empty())
            return;
        state().movie.frames.clear();
        zx::texture_replace::removeSource("movie");
    }

    void startSound()
    {
        {
            std::lock_guard<std::mutex> lock(state().soundMutex);
            state().soundGain = 1.0f;
        }
        setSoundPlaying(true);
    }

    void stopSound() { setSoundPlaying(false); }

    void setSoundVolume(uint32_t volume)
    {
        // As the SPU2's input volume the game sets it to: volume * 63 of 32767.
        constexpr uint32_t kInputVolumePerStep = 0x3Fu;
        constexpr float kFullInputVolume = 32767.0f;
        std::lock_guard<std::mutex> lock(state().soundMutex);
        state().soundGain = static_cast<float>((volume & 0xFFu) * kInputVolumePerStep) / kFullInputVolume;
    }

    void traceOpened(bool withSound)
    {
        if (state().trace)
            std::cout << "[movie] " << state().movie.path << (state().movie.file || state().movie.video ? "" : " (missing)")
                      << (withSound ? ", with sound" : "") << std::endl;
    }

    void tracePlayAgain()
    {
        if (state().trace)
            std::cout << "[movie] " << state().movie.path << " again" << std::endl;
    }

    void traceEnded()
    {
        if (state().trace)
            std::cout << "[movie] ended after " << state().movie.shown << " pictures" << std::endl;
    }

    void logSkipped() { std::cout << "[movie] " << state().movie.path << " skipped (Skip Intro Movies)" << std::endl; }

    void logSoundBankAsked() { std::cerr << "[movie] a sound bank for the movie is not supported" << std::endl; }
}
