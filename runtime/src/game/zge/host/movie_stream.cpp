// The engine's movie stream (zge/movie/ps2/ps2_movie.h) as the host plays
// it: the movie file decoded by the host (game/host/movies.h: the pictures
// and the sound, the Mods folder over the game's files), in place of the
// MPEG stream sample's demuxer and decoder threads and the sound's way
// through the IOP into the SPU2 (zge/movie/ps2/, the PS2 form). The native
// movies, the default, install these over the PS2 forms (ee/host_form.h);
// ZX_MOVIES=hle runs the PS2 forms over the emulated MPEG decoder and the
// IOP's sound driver, as on the console.
//
// The rest of the player stays the game's: open, play, stop and close, the
// per-frame task with its priority changes, the end handling, the sound
// driver's commands around a movie with sound, and the picture's upload into
// the movie texture (outputMovieFrame). So the host forms keep the player's
// state as the PS2 form leaves it:
//   - the movie's start (startMovie): its kind by the file's extension (a PSS
//     has sound), the decoder at its first phase, the two slots of the picture
//     ring empty in the player's frame buffer (uncached), and the stream's
//     words as after a stream thread that has read and demuxed it all;
//   - each frame (stepMovie, the movie's task): the next picture when it is
//     due on the vsync clock (late ones dropped, at most 30 for one shown),
//     written into the ring's next slot as the decoder leaves one, then
//     handed to the game's upload; at the end the ring emptied and the
//     decoder's state ended, so the task ends or loops the movie;
//   - its end (endMovie): the stream stopped, the caller's priority back at
//     the default (as after the PS2 form waited for and deleted the stream's
//     thread);
//   - the sound: always ready to start, started, stopped (its counts
//     cleared), taken down and its volume, as the host's mix plays it.
// The main thread no longer waits for the decoder, so the menus keep running
// at 60 frames per second during a movie.
//
// Skip Intro Movies: a movie of the game's boot chain the first time it plays
// ends before its first picture, so the chain goes on to the next as after a
// movie that ran out.

#include "host/disc_files.h"
#include "host/kernel.h"
#include "host/movies.h"
#include "ee/host_form.h"
#include "zge/movie/ps2/ps2_movie.h"

#include "ee/ee.h"
#include "ee/mirror.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include "sym/zge/host/movie_stream.cpp.h"

namespace zge::host
{
    namespace
    {
        namespace movies = ::game::host::movies;
        namespace platform = ::game::host;

        // The player's ring of decoded pictures (the sample's, just before its
        // video decoder): two slots in the player's frame buffer, each a
        // 512x256 RGBA picture, with their tags.
        inline ee::Global<VoBuf, sym::voBuf> g_moviePictureRing;
        constexpr uint32_t kPictureSlots = 2;
        constexpr uint32_t kPictureSlotBytes = 512u * 256u * 4u;
        constexpr uint32_t kPictureTagBytes = 0x31C80u / kPictureSlots; // the frame tags' share of each slot
        // A slot's tag, its first word: empty (0), or holding a whole decoded
        // picture (2).
        constexpr uint32_t kSlotEmpty = 0u, kSlotFull = 2u;

        // The priority the stream's end leaves its caller at (the player's
        // default).
        constexpr int kDefaultPriority = 10;

        MovieVideoDecoder &decoder() { return *g_movieVideoDecoder; }
        VoBuf &pictureRing() { return *g_moviePictureRing; }

        ee::Ptr<uint32_t> slotTag(uint32_t slot)
        {
            return ee::Ptr<uint32_t>(static_cast<uint32_t>(pictureRing().tag) + slot * kPictureTagBytes);
        }

        // The extension the path ends with (its last three characters), as
        // the PS2 form compares it: case aside.
        bool hasExtension(const std::string &path, const char *extension)
        {
            if (path.size() < 3u)
                return false;
            for (size_t i = 0; i < 3u; ++i)
            {
                const char c = path[path.size() - 3u + i];
                const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
                if (upper != extension[i])
                    return false;
            }
            return true;
        }

        // The picture due now into the ring's next slot (as the decoder fills
        // one) and handed to the game's upload after the step returns; false
        // when none is due yet, or the movie has ended or is to stop (ended
        // then says which).
        bool showPicture(bool &ended)
        {
            ended = false;
            if (*g_mpegPictureReady != 0 || decoder().state == kMovieAbort || !movies::hasPictures())
            {
                ended = true;
                return false;
            }
            const movies::Next next = movies::nextPicture(platform::vsyncNow());
            if (next == movies::Next::notYet)
                return false;
            if (next == movies::Next::ended)
            {
                ended = true;
                return false;
            }
            VoBuf &ring = pictureRing();
            const uint32_t slot = static_cast<uint32_t>(ring.write) % kPictureSlots;
            const uint32_t pixels = static_cast<uint32_t>(ring.data) + slot * kPictureSlotBytes;
            movies::writePicture(pixels);
            *slotTag(slot) = kSlotFull;
            ring.count = static_cast<int32_t>(static_cast<uint32_t>(ring.count) + 1u);
            ring.write = static_cast<int32_t>((slot + 1u) % kPictureSlots);
            MovieVideoDecoder &movie = decoder();
            movie.width = static_cast<int32_t>(movies::pictureWidth());
            movie.height = static_cast<int32_t>(movies::pictureHeight());
            movie.pixels = pixels;
            movie.needPicture = 0;
            movie.uploadPending = 1;
            movie.dataWaits = 0;
            platform::callGameFunctionAfterReturn(sym::outputMovieFrame, g_movieVideoDecoder.address, 1);
            return true;
        }
    }

    // startMovieOnHost(path, or 0 to start the open one again): the movie
    // opened (or its stream back at its start), the player's state as the PS2
    // form's start leaves it. 0.
    int32_t startMovieOnHost(ee::Ptr<const char> path)
    {
        MovieVideoDecoder &movie = decoder();
        if (path)
        {
            const std::string file = ::game::host::disc::guestPath(path.address);
            movies::open(file);
            // By the extension, as the PS2 form: a PSS has sound.
            if (!hasExtension(file, "PSM"))
            {
                if (hasExtension(file, "PSS"))
                {
                    *g_movieStreamKind = kStreamPss;
                    *g_movieHasAudio = 1;
                }
                else if (hasExtension(file, "PSA"))
                {
                    *g_movieStreamKind = kStreamPsa;
                    *g_movieHasBank = 0;
                }
                else if (hasExtension(file, "PSB"))
                {
                    *g_movieStreamKind = kStreamPsb;
                    *g_movieHasAudio = 0;
                }
                else
                {
                    *g_movieHasAudio = 0;
                }
            }
            else
            {
                *g_movieHasAudio = 0;
            }
            movies::traceOpened(*g_movieHasAudio != 0);
        }
        else
        {
            movies::playAgain();
            movies::tracePlayAgain();
        }
        if (*g_movieHasBank != 0)
            movies::logSoundBankAsked();
        // The decoder as the PS2 form's reset leaves it.
        movie.dataWaits = 0;
        movie.needPicture = 1;
        movie.state = kMovieNormal;
        movie.phase = kMoviePhaseStart;
        movie.uploadPending = 0;
        movie.streamCallback = 0u;
        movie.texture = ee::Ptr<Texture>(g_moviePlayer->texture);
        *g_movieAudioStarted = 0;
        // The picture ring's two slots, empty, in the player's frame buffer
        // (uncached).
        VoBuf &ring = pictureRing();
        const uint32_t tags = g_moviePlayer->frameTags;
        ring.count = 0;
        ring.data = static_cast<int32_t>(ee::uncachedMirror(ee::physical(ee::Ptr<uint8_t>(g_moviePlayer->frameData))).address);
        ring.tag = static_cast<int32_t>(tags);
        ring.size = kPictureSlots;
        ring.write = 0;
        for (uint32_t slot = 0; slot < kPictureSlots; ++slot)
            *slotTag(slot) = kSlotEmpty;
        // The stream's words as after a stream thread that has read and
        // demuxed the whole file and parked.
        *g_movieDefaultKeepsRunning = 1;
        *g_movieDemuxLeft = *g_movieStreamBytes;
        *g_movieReadLeft = *g_movieStreamBytes;
        *g_movieDefaultParked = 1;
        *g_movieDefaultThread = 0;
        movie.dataWaits = 0;
        movies::stopSound();
        return 0;
    }

    // stepMovieOnHost(decoder): the movie's step for this frame: started at
    // its first, then the picture due each frame, then the ring emptied and
    // the decoder ended. 1.
    uint32_t stepMovieOnHost(ee::Ptr<MovieVideoDecoder>)
    {
        MovieVideoDecoder &movie = decoder();
        if (movie.phase == kMoviePhaseEnded)
        {
            if (movie.state != kMovieEnd)
                movies::traceEnded();
            movies::endPictures();
            pictureRing().count = 0;
            movie.state = kMovieEnd;
            return 1;
        }
        if (movie.phase == kMoviePhaseStart && movies::skipped())
        {
            // Skip Intro Movies: ended before its first picture; the chain
            // fades to the next as after a movie that ran out.
            movies::logSkipped();
            movie.phase = kMoviePhaseEnded;
            return 1;
        }
        if (movie.phase == kMoviePhaseStart)
        {
            pictureRing().write = 0;
            pictureRing().count = 0;
            *g_mpegPictureReady = 0;
            movie.phase = kMoviePhaseDecoding;
            movies::startPictures(platform::vsyncNow());
        }
        else if (movie.phase != kMoviePhaseDecoding)
        {
            return 1;
        }
        bool ended = false;
        if (!showPicture(ended) && ended)
            movie.phase = kMoviePhaseEnded;
        return 1;
    }

    // endMovieOnHost: the stream stopped (the sound, the picture ring, the
    // stream's thread no longer to run), the caller at the default priority
    // and a thread made ready by that let run. 0.
    int32_t endMovieOnHost()
    {
        movies::stopSound();
        movies::endPictures();
        *g_movieDefaultKeepsRunning = 0;
        pictureRing().count = 0;
        pictureRing().write = 0;
        platform::changeOwnPriority(kDefaultPriority);
        platform::letReadyThreadRun();
        return 0;
    }

    // movieSoundReadyOnHost: the movie's sound can start (the host's needs
    // no filling first). 1.
    uint32_t movieSoundReadyOnHost()
    {
        return 1;
    }

    // startMovieSoundOnHost(sound): mixed in from now at full volume; the
    // sound playing. 0.
    int32_t startMovieSoundOnHost(ee::Ptr<AudioDec> sound)
    {
        movies::startSound();
        sound->state = kAudioDecPlaying;
        return 0;
    }

    // stopMovieSoundOnHost(sound): no longer mixed in; its counts cleared, as
    // the PS2 form's reset clears them. 0.
    int32_t stopMovieSoundOnHost(ee::Ptr<AudioDec> sound)
    {
        movies::stopSound();
        sound->iopPausePosition = 0;
        sound->state = kAudioDecIdle;
        sound->headerCount = 0;
        sound->putOffset = 0;
        sound->count = 0;
        sound->totalBytes = 0;
        sound->totalBytesSent = 0;
        sound->iopLastPosition = 0;
        return 0;
    }

    // deleteMovieSoundOnHost(sound): no longer mixed in. 1.
    uint32_t deleteMovieSoundOnHost(ee::Ptr<AudioDec>)
    {
        movies::stopSound();
        return 1;
    }

    // setMovieSoundVolumeOnHost(sound, volume 0-255): the mix's volume for it.
    // 0.
    int32_t setMovieSoundVolumeOnHost(ee::Ptr<AudioDec>, uint32_t volume)
    {
        movies::setSoundVolume(volume);
        return 0;
    }

    HOST_FORM(movies, sym::startMovie, startMovieOnHost);
    HOST_FORM(movies, sym::stepMovie, stepMovieOnHost);
    HOST_FORM(movies, sym::endMovie, endMovieOnHost);
    HOST_FORM(movies, sym::movieSoundReady, movieSoundReadyOnHost);
    HOST_FORM(movies, sym::audioDecStart, startMovieSoundOnHost);
    HOST_FORM(movies, sym::audioDecStartThunk, startMovieSoundOnHost);
    HOST_FORM(movies, sym::audioDecStop, stopMovieSoundOnHost);
    HOST_FORM(movies, sym::audioDecDelete, deleteMovieSoundOnHost);
    HOST_FORM(movies, sym::setAudioDecVolume, setMovieSoundVolumeOnHost); // (none in BMX XXX: not installed there)
}
