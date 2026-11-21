// Sound streams (zge/sound/streams.h; the ps2Chan block soundStreamStubZeroA-soundStreamStubZeroG,
// DM2 and BMX XXX have them word for word): the stream slots and the
// requests they send to the IOP sound driver.
//
// The requests are the driver's stream commands (zge/sound/driver.h): a
// stream opened (the reply: its id), closed, played, stopped, paused; and the
// stream request (zaudio::StreamRequest) with its source file, its loop
// count, its volume or a fade.

#include "streams.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/random.h"
#include "zge/sound/bank.h"
#include "zge/sound/driver.h"
#include "zge/text/cstring.h"
#include "zge/sound/sound.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/streams.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kFlushCache = sym::flushCache;      // FlushCache(0): the data cache written back (the IOP wrote the state)

        // 1 once the streams are set up (initSoundStreams).
        inline ee::Global<uint8_t, sym::g_soundStreamsReady> g_soundStreamsReady;

        // The errors the opens return.
        constexpr uint32_t kErrorNoFile = 513;
        constexpr uint32_t kErrorNoSlot = 514;

        // The disc path around a stream file's name: "" + name + ";1",
        // upper case.
        constexpr uint32_t kPathPrefix = sym::pathPrefix; // ""
        constexpr uint32_t kPathSuffix = sym::pathSuffix; // ";1"

        inline ee::Global<SoundStream[kSoundStreamCount], kSoundStreams> g_soundStreams;

        // The driver's pitch for a sample rate (4096 = 44100 Hz): the usual
        // rates from a table of the compiled switch, others worked out.
        uint32_t pitchOf(uint32_t rate)
        {
            if (rate == 22050)
                return 0x800;
            if (rate < 22051)
            {
                if (rate == 11025)
                    return 0x400;
                if (rate == 16000)
                    return 1486;
            }
            else
            {
                if (rate == 32000)
                    return 2972;
                if (rate == 44100)
                    return 0x1000;
            }
            const uint32_t pitch = (rate << 12) / 44100u;
            return pitch;
        }

        // The stream's source: the file, as the driver's disc path.
        void sendFileRequest(ee::Local<zaudio::StreamRequest> &request, const SoundStream &stream, const StreamFile &file)
        {
            request->flags = zaudio::kRequestSource;
            request->id = stream.id;
            request->length = file.size;
            request->offset = file.offset;
            const ee::Ptr<char> path(request->path);
            if constexpr (game::readable::kIsDm2) // ZGE2 puts no prefix before the name
                stringCopy(path, ee::Ptr<const char>(file.name));
            else
            {
                stringCopy(path, kPathPrefix);
                appendString(path, ee::Ptr<const char>(file.name));
            }
            appendString(path, kPathSuffix);
            stringToUpper(path);
            zaudio::request(request);
        }
    }

    // initSoundStreams (initSoundChannels): the stream slots, the stream files
    // and the table after them cleared. Returns 0.
    uint32_t initSoundStreams()
    {
        memoryFill(kSoundStreams, 0, kSoundStreamCount * sizeof(SoundStream));
        memoryFill(kStreamFiles, 0, 16 * sizeof(StreamFile));
        memoryFill(sym::g_streamFileSpare, 0, 0x100); // [unknown: the 256 bytes after the files]
        *g_soundStreamsReady = 1;
        return 0;
    }
    READABLE_FUNCTION(initSoundStreams, Verified);

    // closeAllSoundStreams (no caller in the game): every open stream closed.
    void closeAllSoundStreams()
    {
        for (SoundStream &stream : *g_soundStreams)
            if (stream.flags & kStreamInUse)
                closeSoundStream(ee::Ptr<SoundStream>(&stream));
        *g_soundStreamsReady = 0;
    }
    READABLE_FUNCTION_WAITS(closeAllSoundStreams, Verified);

    // streamsFrame (updateGameFrame): the streams' frame (nothing to do:
    // the IOP streams by itself). Returns 0.
    uint32_t streamsFrame() { return 0; }
    READABLE_FUNCTION(streamsFrame, Verified);

    namespace
    {
        constexpr uint32_t kStreamFileCount = 16;
        inline ee::Global<StreamFile[kStreamFileCount], sym::streamFiles> g_streamFiles;
    }

    // addStreamFile: a free stream file (one whose size is 0) given a wave's
    // place (its file, offset and size) and the open's rate and channels;
    // 0 when all 16 are in use.
    ee::Ptr<StreamFile> addStreamFile(ee::Ptr<WaveLocation> wave, uint32_t flags)
    {
        ee::Ptr<StreamFile> file;
        for (StreamFile &record : *g_streamFiles)
        {
            if (record.size == 0)
            {
                file = &record;
                break;
            }
        }
        if (!file)
            return file;
        file->offset = wave->offset;
        file->size = wave->size;
        stringCopy(ee::Ptr<char>(file->name), wave->path);
        if (flags & kStream44100)
            file->rate = 44100;
        else if (flags & kStream11025)
            file->rate = 11025;
        else if (flags & kStream16000)
            file->rate = 16000;
        else
            file->rate = 22050;
        file->channels = (flags & kStreamStereo) ? 2 : 1;
        return file;
    }
    READABLE_FUNCTION(addStreamFile, Verified);

    // openSoundStream: a stream opened for a bank sample (`sample`: its
    // record; `flags` StreamFlags): a free slot, a stream file for the
    // sample, the driver's stream (its pitch by the file's rate), then the
    // file request. *out the slot (0 until it is opened). Returns 0, or
    // kErrorNoSlot with all four slots in use (bug kept: it then stores 1
    // at address 0, the slot pointer). A slot whose file cannot be had
    // stays marked in use and 0 is returned (bug kept: *out stays 0).
    uint32_t openSoundStream(ee::Ptr<ee::Ptr<SoundStream>> out, ee::Ptr<WaveLocation> sample, uint32_t, uint32_t, uint32_t flags)
    {
        ee::Local<zaudio::StreamOpen> open;
        ee::Local<zaudio::StreamRequest> request;
        *out = ee::Ptr<SoundStream>();

        ee::Ptr<SoundStream> stream;
        for (SoundStream &slot : *g_soundStreams)
        {
            if ((slot.flags & kStreamInUse) == 0)
            {
                slot.flags = kStreamInUse;
                stream = &slot;
                break;
            }
        }
        if (!stream)
        {
            // Bug kept: with no slot free the mark still goes through the
            // empty slot pointer, into the word at address 0.
            stream[0].flags = kStreamInUse;
            return kErrorNoSlot;
        }

        // Bug kept: with every file record taken the slot stays marked in
        // use and the call reports no error.
        const ee::Ptr<StreamFile> file = addStreamFile(sample, flags);
        if (!file)
            return 0;
        stream->file = file;
        stream->loops = 0;
        stream->clearedAtOpen[0] = 0;
        stream->clearedAtOpen[1] = 0;

        open->state = ee::Address(&stream->state);
        open->alwaysZero = 0;
        open->slot = ee::Address(stream);
        open->flags = (flags & kStreamLooped) ? zaudio::kOpenLooped : 0u;
        if (file->channels == 2)
            open->flags |= zaudio::kOpenStereo;
        open->flags |= (flags & kStreamFlag1000) ? zaudio::kOpenNoSource : zaudio::kOpenDiscSectors;
        open->pitch = pitchOf(file->rate);
        stream->id = zaudio::open(open);
        *out = stream;
        sendFileRequest(request, *stream, *file);
        return 0;
    }
    READABLE_FUNCTION_WAITS(openSoundStream, Verified);

    // freeStreamFile (was findFreeStream): a stream file free again (its
    // size 0), if it is one of the 16.
    void freeStreamFile(ee::Ptr<StreamFile> file)
    {
        for (StreamFile &record : *g_streamFiles)
        {
            if (file == ee::Ptr<StreamFile>(&record))
            {
                record.size = 0;
                return;
            }
        }
    }
    READABLE_FUNCTION(freeStreamFile, Verified);

    // closeSoundStream: an open stream closed: its file freed, the driver's
    // stream closed, the slot free. Returns 0.
    uint32_t closeSoundStream(ee::Ptr<SoundStream> stream)
    {
        ee::Local<uint32_t> streamId;
        if ((stream->flags & kStreamInUse) == 0)
            return 0;
        if (stream->file)
        {
            freeStreamFile(stream->file);
            stream->file = ee::Ptr<StreamFile>();
        }
        *streamId = stream->id;
        zaudio::close(streamId);
        stream->flags &= static_cast<uint16_t>(~kStreamInUse);
        return 0;
    }
    READABLE_FUNCTION_WAITS(closeSoundStream, Verified);

    // openStream: an open stream given another sample's file (its old
    // file freed), the file request sent. Returns 0, or kErrorNoFile.
    uint32_t openStream(ee::Ptr<SoundStream> stream, ee::Ptr<WaveLocation> sample, uint32_t flags)
    {
        ee::Local<zaudio::StreamRequest> request;
        if (stream->file)
        {
            freeStreamFile(stream->file);
            stream->file = ee::Ptr<StreamFile>();
        }
        const ee::Ptr<StreamFile> file = addStreamFile(sample, flags);
        if (!file)
            return kErrorNoFile;
        stream->file = file;
        stream->loops = 0;
        stream->clearedAtOpen[0] = 0;
        stream->clearedAtOpen[1] = 0;
        sendFileRequest(request, *stream, *file);
        return 0;
    }
    READABLE_FUNCTION_WAITS(openStream, Verified);

    // setSoundStreamLoops (openMusicTrack; was setSoundStreamPosition): how many
    // times the stream's source plays, kept in the slot: a loops request. Returns 0.
    uint32_t setSoundStreamLoops(ee::Ptr<SoundStream> stream, uint32_t loops)
    {
        ee::Local<zaudio::StreamRequest> request;
        request->loops = loops;
        request->flags = zaudio::kRequestLoops;
        request->id = stream->id;
        stream->loops = loops;
        zaudio::request(request);
        return 0;
    }
    READABLE_FUNCTION_WAITS(setSoundStreamLoops, Draft);

    // updateSoundStream (updateChannelStream): a stream's update (nothing to do
    // on the PS2). Returns 0.
    uint32_t updateSoundStream() { return 0; }
    READABLE_FUNCTION(updateSoundStream, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
    }

    // streamStatus: the stream's state as the game's status: the driver's
    // states 0 .. 8 to 1, 2, 4, 2, 2, 1, 3, 1, 2,
    // anything else 5. Returns 0. ZGE2 (DM2) answers 1 for state 2 and
    // leaves the status as it was for a state past 8.
    uint32_t streamStatus(ee::Ptr<SoundStream> stream, ee::Ptr<uint32_t> status)
    {
        constexpr uint32_t kStatus[9] = {1, 2, game::readable::perGame(4u, 1u, 4u), 2, 2, 1, 3, 1, 2};
        flushCache(0);
        if constexpr (!game::readable::kIsDm2)
            *status = 5;
        const uint32_t state = stream->state;
        if (state < 9)
            *status = kStatus[state];
        return 0;
    }
    READABLE_FUNCTION(streamStatus, Verified);

    // playStream, stopStream: the stream played, stopped. Return 0.
    uint32_t playStream(ee::Ptr<SoundStream> stream)
    {
        ee::Local<uint32_t> streamId;
        *streamId = stream->id;
        zaudio::play(streamId);
        return 0;
    }
    READABLE_FUNCTION_WAITS(playStream, Verified);

    uint32_t stopStream(ee::Ptr<SoundStream> stream)
    {
        ee::Local<uint32_t> streamId;
        *streamId = stream->id;
        zaudio::stop(streamId);
        return 0;
    }
    READABLE_FUNCTION_WAITS(stopStream, Verified);

    // pauseSoundStream (was setSoundStreamVolume): the stream paused (1) or
    // playing again (0): the driver's pause command. Returns 0.
    uint32_t pauseSoundStream(ee::Ptr<SoundStream> stream, uint32_t paused)
    {
        ee::Local<zaudio::StreamPause> pause;
        pause->paused = paused;
        pause->id = stream->id;
        zaudio::pause(pause);
        return 0;
    }
    READABLE_FUNCTION_WAITS(pauseSoundStream, Verified);

    // playSoundStreamSample (no caller in the game): a sample played as a stream at
    // a volume: opened looped (`openArgument` handed on as the open's fourth argument,
    // which it does not read), its volume set (`pan` handed on as a third
    // argument setStreamVolume does not read), played. The first non-zero
    // result, else 0.
    uint32_t playSoundStreamSample(ee::Ptr<WaveLocation> sample, uint32_t openArgument, uint32_t volume, uint32_t pan)
    {
        (void)pan; // the game works it out and the volume call does not read it
        ee::Local<ee::Ptr<SoundStream>> stream;
        uint32_t result = openSoundStream(stream, sample, 0, openArgument, static_cast<uint32_t>(kStreamLooped));
        if (result == 0)
        {
            result = setStreamVolume(*stream, volume & 0xFFu);
            if (result == 0)
                result = playStream(*stream);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(playSoundStreamSample, Verified);

    // setStreamVolume: the stream's volume (0 .. 255) as a volume request.
    // Returns 0.
    uint32_t setStreamVolume(ee::Ptr<SoundStream> stream, uint32_t volume)
    {
        ee::Local<zaudio::StreamRequest> request;
        const uint32_t level = volume & 0xFFu;
        request->flags = zaudio::kRequestVolume;
        request->id = stream->id;
        request->volume = zaudio::streamVolume(level);
        zaudio::request(request);
        return 0;
    }
    READABLE_FUNCTION_WAITS(setStreamVolume, Verified);

    // rampStreamVolume (setPlayPhase): the stream's volume faded to `volume`
    // (0 .. 255) over `milliseconds`, `fadeFlags` what happens at the end
    // (the driver's own flags): a fade request. Returns 0.
    uint32_t rampStreamVolume(ee::Ptr<SoundStream> stream, uint32_t milliseconds, uint32_t volume, uint32_t fadeFlags)
    {
        ee::Local<zaudio::StreamRequest> request;
        const uint32_t level = volume & 0xFFu;
        request->flags = zaudio::kRequestFade;
        request->fadeFlags = fadeFlags;
        request->id = stream->id;
        request->fadeTarget = zaudio::streamVolume(level);
        request->fadeMilliseconds = milliseconds;
        zaudio::request(request);
        return 0;
    }
    READABLE_FUNCTION_WAITS(rampStreamVolume, Draft);

    // The rest of the stream interface, no caller in the game: five that
    // return 0 and one that does nothing (DM2 has the same).
    uint32_t soundStreamStubZeroA() { return 0; }
    READABLE_FUNCTION(soundStreamStubZeroA, Draft);
    void soundStreamNoop() {}
    READABLE_FUNCTION(soundStreamNoop, Draft);
    uint32_t soundStreamStubZeroB() { return 0; }
    READABLE_FUNCTION(soundStreamStubZeroB, Draft);
    uint32_t soundStreamStubZeroC() { return 0; }
    READABLE_FUNCTION(soundStreamStubZeroC, Draft);
    uint32_t soundStreamStubZeroD() { return 0; }
    READABLE_FUNCTION(soundStreamStubZeroD, Draft);
    uint32_t soundStreamStubZeroE() { return 0; }
    READABLE_FUNCTION(soundStreamStubZeroE, Draft);

    namespace
    {
        namespace routed
        {
        }
    }

    uint32_t soundStreamStubZeroG()
    {
        return 0;
    }
    READABLE_FUNCTION(soundStreamStubZeroG, Draft);

    uint32_t soundStreamStubZeroH()
    {
        return 0;
    }
    READABLE_FUNCTION(soundStreamStubZeroH, Draft);

    namespace
    {
        // Music plays on two stream channels, with what the game wants next
        // queued until both have finished.
        struct StreamChannel
        {
            uint8_t openFlags;   // +0x00 startStreamSample's flags
            uint8_t streamFlags; // +0x01 what its stream is opened with (StreamFlags)
            uint8_t startMode;   // +0x02 the playing playlist's kPlaylistStartMask bits
            uint8_t _03;
            uint16_t bank;       // +0x04
            uint16_t wave;       // +0x06 the last wave it was given
            ee::Ptr<SoundStream> stream; // +0x08 0: free
        };
        static_assert(sizeof(StreamChannel) == 0xC);

        struct QueuedPlaylist
        {
            uint8_t channel;  // +0x00 kNoQueuedChannel: free
            uint8_t _01[3];
            ee::Ptr<Playlist> playlist;   // +0x04
            ee::Ptr<QueuedPlaylist> next; // +0x08
        };
        static_assert(sizeof(QueuedPlaylist) == 0xC);

        constexpr uint32_t kStreamChannels = 2;
        constexpr uint32_t kMusicParameters = 16;
        constexpr uint32_t kQueuedPlaylists = 32;
        struct MusicState
        {
            StreamChannel channels[kStreamChannels];
            uint32_t parameters[kMusicParameters]; // the game's: which wave a chosen or blended track plays
            QueuedPlaylist queue[kQueuedPlaylists];
        };
        static_assert(offsetof(MusicState, parameters) == 0x18 && offsetof(MusicState, queue) == 0x58);
        inline ee::Global<MusicState, sym::g_streamChannelRecords> g_music;

        inline ee::Global<int32_t, sym::g_streamChannelsOpen> g_streamChannelsOpen;
        inline ee::Global<uint32_t, sym::g_streamChannelStarted> g_queueSearchStart; // nothing moves it on
        inline ee::Global<ee::Ptr<QueuedPlaylist>, sym::g_streamChannels> g_queueHead;
        inline ee::Global<int32_t, sym::g_streamChannelPlaying> g_queueBusy;

        constexpr uint8_t kNoQueuedChannel = 255;
        constexpr uint32_t kNoStreamChannel = 255;
        constexpr uint16_t kNoLastWave = 0xFFFF;
        constexpr uint32_t kMostParameterValue = 0x40;
        constexpr uint32_t kBlendAtHalf = 101, kBlendBelowHalf = 102, kBlendAboveHalf = 103; // a blended track's three waves past its hundred

        // What a stream's status says of it: nothing that needs waiting for.
        constexpr uint32_t kStreamStopped = 1, kStreamEnded = 4;
        bool streamIdle(uint32_t status) { return status == kStreamStopped || status == kStreamEnded; }

        enum : uint32_t
        {
            kPlayed = 0,
            kStreamChannelBusy = 0x601,
            kNothingToPlay = 0x602,
            kQueueFull = 0x603,
        };

        enum : uint8_t
        {
            kStartIfIdle = 0x00,   // refused while a stream plays
            kStartOverLoop = 0x40, // stops a stream with no start mode of its own, else refused
            kStartQueued = 0x80,   // stops such a stream, else waits its turn
            kStartNow = 0xC0,      // stops whatever plays and empties the queue
        };

        constexpr uint32_t kOpenFlagsPlaying = 1, kOpenFlagsStereo = 3;

        bool isSet(const uint8_t *bits, uint32_t index) { return (bits[index >> 3] >> (index & 7) & 1) != 0; }
        void setBit(uint8_t *bits, uint32_t index) { bits[index >> 3] |= static_cast<uint8_t>(1u << (index & 7)); }
        void clearPlayed(uint8_t *played, uint32_t bytes)
        {
            for (uint32_t i = 0; i < bytes; ++i)
                played[i] = 0;
        }

        // The first of `count` not played yet, from a random place on and
        // round; `tried` is how many it had to pass, so count if all have.
        uint32_t pickUnplayed(const uint8_t *played, uint32_t count, uint32_t &tried)
        {
            uint32_t index = randomNumber() % count;
            tried = 0;
            if (count == 0)
                return index;
            while (isSet(played, index))
            {
                ++tried;
                index = index + 1 == count ? 0 : index + 1;
                if (tried >= count)
                    break;
            }
            return index;
        }

        // Every queued playlist dropped.
        void clearQueue()
        {
            ee::Ptr<QueuedPlaylist> queued = *g_queueHead;
            *g_queueBusy = 1;
            for (; queued; queued = queued->next)
                queued->channel = kNoQueuedChannel;
            *g_queueHead = nullptr;
            *g_queueBusy = 0;
        }

        // A free place in the queue, from where the search starts and round.
        ee::Ptr<QueuedPlaylist> freeQueuePlace()
        {
            const uint32_t first = *g_queueSearchStart;
            uint32_t index = first;
            do
            {
                if (g_music->queue[index].channel == kNoQueuedChannel)
                    return ee::Ptr<QueuedPlaylist>(&g_music->queue[index]);
                index = index + 1 == kQueuedPlaylists ? 0 : index + 1;
            } while (index != first);
            return nullptr;
        }

        // A random track's next wave: 1, or 0 if none will do.
        uint32_t pickRandomTrackWave(const StreamChannel &channel, Playlist &playlist, uint32_t section, PlaylistTrack &track, ee::Ptr<uint32_t> wave)
        {
            uint32_t tried = 0;
            uint32_t index = pickUnplayed(track.played, track.count, tried);
            const bool noRepeat = (playlist.flags & kPlaylistNoRepeat) != 0;
            if (tried == track.count)
            {
                if (noRepeat)
                {
                    // The section has played all it can.
                    setBit(playlist.played, section);
                    return 0;
                }
                clearPlayed(track.played, sizeof(track.played));
            }
            if (!noRepeat)
            {
                *wave = track.firstWave + index;
                if (*wave == channel.wave)
                {
                    if (track.count < 2)
                        return 0;
                    index = (index + 1) % track.count;
                }
            }
            *wave = track.firstWave + index;
            setBit(track.played, index);
            return 1;
        }

        uint32_t pickChosenTrackWave(const StreamChannel &channel, const Playlist &playlist, const PlaylistTrack &track, ee::Ptr<uint32_t> wave)
        {
            const uint32_t value = g_music->parameters[track.count];
            *wave = track.firstWave + value;
            if ((playlist.flags & kPlaylistNoRepeat) != 0 && value < kMostParameterValue && isSet(track.played, value))
                return 0;
            return *wave != channel.wave ? 1 : 0;
        }

        // By the parameter: up to 100 and at 200 a wave alone; between them
        // and on to 299 a wave and a second one the rest picks: 2. Past that 0.
        uint32_t pickBlendedTrackWave(const PlaylistTrack &track, ee::Ptr<uint32_t> wave, ee::Ptr<uint32_t> start)
        {
            const uint32_t value = g_music->parameters[track.count];
            if (value <= 100)
            {
                *wave = track.firstWave + value;
                return 1;
            }
            if (value < 200)
            {
                *wave = track.firstWave + kBlendBelowHalf;
                *start = track.firstWave - 100 + value;
                return 2;
            }
            if (value == 200)
            {
                *wave = track.firstWave + kBlendAtHalf;
                return 1;
            }
            if (value < 300)
            {
                *wave = track.firstWave + kBlendAboveHalf;
                *start = track.firstWave - 200 + value;
                return 2;
            }
            return 0;
        }
    }

    // Which section a pick among all the sections' waves lands in: a random
    // track counts for its waves, the others for one.
    uint32_t sectionOfPick(ee::Ptr<Playlist> playlist, uint32_t pick)
    {
        uint32_t section = 0;
        for (; section < playlist->count; ++section)
        {
            const PlaylistTrack &first = playlist->sections[section].tracks[0];
            if (first.kind == kTrackRandom)
            {
                const bool here = pick < first.count;
                pick -= first.count;
                if (here)
                    return section;
            }
            else if (first.kind == kTrackChosen || first.kind == kTrackBlended)
            {
                if (pick == 0)
                    return section;
                --pick;
            }
        }
        return section;
    }
    READABLE_FUNCTION(sectionOfPick, Draft);

    uint32_t pickTrackWave(ee::Ptr<StreamChannel> channel, ee::Ptr<Playlist> playlist, uint32_t section, ee::Ptr<PlaylistTrack> track, ee::Ptr<uint32_t> wave, ee::Ptr<uint32_t> start)
    {
        switch (track->kind)
        {
        case kTrackRandom:
            return pickRandomTrackWave(*channel, *playlist, section, *track, wave);
        case kTrackChosen:
            return pickChosenTrackWave(*channel, *playlist, *track, wave);
        case kTrackBlended:
            return pickBlendedTrackWave(*track, wave, start);
        default:
            return 0;
        }
    }
    READABLE_FUNCTION(pickTrackWave, Draft);

    // A channel's stream given a wave of a bank, and what the channel plays.
    namespace
    {
        void appendWave(StreamChannel &channel, uint32_t wave)
        {
            bankSampleInfo(channel.bank, wave);
            zge::updateSoundStream();
            channel.wave = static_cast<uint16_t>(wave);
        }

        // Each track's wave for the section, the first opening the stream.
        uint32_t startSection(StreamChannel &channel, ee::Ptr<StreamChannel> channelAddress, ee::Ptr<Playlist> playlist, uint32_t section, ee::Local<uint32_t> &wave, ee::Local<uint32_t> &start)
        {
            const PlaylistSection &tracks = playlist->sections[section];
            const uint32_t picked = pickTrackWave(channelAddress, playlist, section, tracks.tracks, wave, start);
            if (picked == 0)
                return kNothingToPlay;
            zge::openStream(channel.stream, bankSampleInfo(channel.bank, *wave), channel.streamFlags);
            channel.wave = static_cast<uint16_t>(*wave);
            if (picked >= 2)
                appendWave(channel, *start);
            for (uint32_t i = 1; i < tracks.trackCount; ++i)
            {
                const uint32_t more = pickTrackWave(channelAddress, playlist, section, tracks.tracks + i, wave, start);
                if (more == 0)
                    continue;
                appendWave(channel, *wave);
                if (more >= 2)
                    appendWave(channel, *start);
            }
            return kPlayed;
        }

        // How many waves a pick among the sections chooses from.
        uint32_t sectionPicks(const Playlist &playlist)
        {
            uint32_t picks = 0;
            for (uint32_t i = 0; i < playlist.count; ++i)
            {
                const PlaylistTrack &first = playlist.sections[i].tracks[0];
                if (first.kind == kTrackRandom)
                    picks += first.count;
                else if (first.kind <= kTrackBlended)
                    ++picks;
            }
            return picks;
        }

        // A section to play, or kNothingToPlay. With kPlaylistNoRepeat one the
        // playlist has not finished.
        uint32_t pickSection(ee::Ptr<Playlist> playlist, uint32_t picks, uint32_t &section)
        {
            uint32_t pick = randomNumber() % picks;
            section = sectionOfPick(playlist, pick);
            if ((playlist->flags & kPlaylistNoRepeat) == 0)
                return kPlayed;
            uint32_t tried = 0;
            for (;;)
            {
                ++pick;
                if (!isSet(playlist->played, section))
                    break;
                ++tried;
                pick = pick == picks ? 0 : pick;
                section = sectionOfPick(playlist, pick);
                if (tried >= picks)
                    break;
            }
            return tried == picks ? kNothingToPlay : kPlayed;
        }

        // One wave of a plain playlist, not the last one if it can help it.
        uint32_t startPlainWave(StreamChannel &channel, Playlist &playlist, ee::Local<uint32_t> &wave)
        {
            uint32_t tried = 0;
            uint32_t index = pickUnplayed(playlist.played, playlist.count, tried);
            const bool noRepeat = (playlist.flags & kPlaylistNoRepeat) != 0;
            if (tried == playlist.count)
            {
                if (noRepeat)
                    return kNothingToPlay;
                clearPlayed(playlist.played, sizeof(playlist.played));
            }
            if (!noRepeat)
            {
                *wave = playlist.firstWave + index;
                if (*wave == channel.wave)
                {
                    if (playlist.count < 2 && (channel.openFlags & 1) == 0)
                        return kNothingToPlay;
                    index = (index + 1) % playlist.count;
                }
            }
            *wave = playlist.firstWave + index;
            zge::openStream(channel.stream, bankSampleInfo(channel.bank, *wave), channel.streamFlags);
            channel.wave = static_cast<uint16_t>(*wave);
            setBit(playlist.played, index);
            return kPlayed;
        }
    }

    // The next wave of a playlist started on a stream channel, or why not.
    int32_t updateChannelStream(uint8_t channelIndex, ee::Ptr<Playlist> playlist)
    {
        ee::Local<uint32_t> wave;
        ee::Local<uint32_t> start;
        StreamChannel &channel = g_music->channels[channelIndex];

        uint32_t result = kNothingToPlay;
        if ((playlist->flags & kPlaylistSections) == 0)
            result = startPlainWave(channel, *playlist, wave);
        else
        {
            const uint32_t picks = sectionPicks(*playlist);
            if (picks != 0)
            {
                uint32_t section = 0;
                result = pickSection(playlist, picks, section);
                if (result == kPlayed)
                    result = startSection(channel, ee::Ptr<StreamChannel>(&channel), playlist, section, wave, start);
            }
        }
        if (result != kPlayed)
            return static_cast<int32_t>(result);

        playStream(channel.stream);
        channel.startMode = playlist->flags & kPlaylistStartMask;
        return kPlayed;
    }
    READABLE_FUNCTION_WAITS(updateChannelStream, Verified);

    // resetStreamChannelPlaylists (ZGE4 only; ZGE3 does it inside
    // startStreamSample): every playlist of the channel's bank made as new, as
    // startStreamSample's loop below does it. 0.
    inline uint32_t resetStreamChannelPlaylists(uint32_t channelIndex)
    {
        const StreamChannel &channel = g_music->channels[channelIndex & 0xFFu];
        const uint32_t playlists = bankPlaylistCount(channel.bank);
        for (uint32_t i = 0; i < playlists; ++i)
        {
            const ee::Ptr<Playlist> playlist = bankPlaylist(channel.bank, i);
            if ((playlist->flags & kPlaylistSections) == 0)
            {
                clearPlayed(playlist->played, sizeof(playlist->played));
                continue;
            }
            for (uint32_t section = 0; section < playlist->count; ++section)
            {
                const PlaylistSection &tracks = playlist->sections[section];
                for (uint32_t track = 0; track < tracks.trackCount; ++track)
                {
                    PlaylistTrack &played = tracks.tracks[track];
                    if (played.kind == kTrackRandom || played.kind == kTrackChosen)
                        clearPlayed(played.played, sizeof(played.played));
                }
            }
            clearPlayed(playlist->played, 4); // a section's bit past the 32nd stays
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, resetStreamChannelPlaylists);

    // A free stream channel opened for a bank's music: its stream at the
    // rate `flags` asks for, and every playlist of the bank made as new (ZGE4
    // by resetStreamChannelPlaylists). The channel's number, or 255 with none
    // free or no stream to be had.
    uint32_t startStreamSample(uint16_t bank, uint32_t first, uint32_t second, uint32_t flags)
    {
        for (uint32_t index = 0; index < kStreamChannels; ++index)
        {
            StreamChannel &channel = g_music->channels[index];
            if (channel.stream)
                continue;
            channel.streamFlags = (flags & kStreamStereo) != 0 ? kOpenFlagsStereo : kOpenFlagsPlaying;
            channel.openFlags = static_cast<uint8_t>(flags);
            if ((flags & kStream11025) != 0)
                channel.streamFlags |= kStream11025;
            else if ((flags & kStream16000) != 0)
                channel.streamFlags |= kStream16000;
            else if ((flags & kStream22050) != 0)
                channel.streamFlags |= kStream22050;
            else if ((flags & kStream44100) != 0)
                channel.streamFlags |= kStream44100;

            if (openSoundStream(ee::Ptr<ee::Ptr<SoundStream>>(&channel.stream), bankSampleInfo(bank, 0), first, second, channel.streamFlags) != 0)
            {
                channel.stream = nullptr;
                continue;
            }
            channel.bank = bank;
            channel.wave = kNoLastWave;
            if constexpr (game::readable::kIsBmx)
            {
                resetStreamChannelPlaylists(index);
                return index;
            }

            const uint32_t playlists = bankPlaylistCount(bank);
            for (uint32_t i = 0; i < playlists; ++i)
            {
                const ee::Ptr<Playlist> playlist = bankPlaylist(bank, i);
                if ((playlist->flags & kPlaylistSections) == 0)
                {
                    clearPlayed(playlist->played, sizeof(playlist->played));
                    continue;
                }
                for (uint32_t section = 0; section < playlist->count; ++section)
                {
                    const PlaylistSection &tracks = playlist->sections[section];
                    for (uint32_t track = 0; track < tracks.trackCount; ++track)
                    {
                        PlaylistTrack &played = tracks.tracks[track];
                        if (played.kind == kTrackRandom || played.kind == kTrackChosen)
                            clearPlayed(played.played, sizeof(played.played));
                    }
                }
                clearPlayed(playlist->played, 4); // a section's bit past the 32nd stays
            }
            return index;
        }
        return kNoStreamChannel;
    }
    // noStreamSample: ZGE2's startStreamSample, a stub that opens no channel and reads none of its arguments
    // (DM2's playStreamSample, which nothing of DM2's calls, reaches it). 0.
    uint32_t noStreamSample()
    {
        return 0;
    }
    READABLE_FUNCTION_WAITS_OWN(startStreamSample, Verified, dm2, noStreamSample);

    uint32_t playStreamSample(uint16_t bank, uint32_t second, uint32_t flags)
    {
        return startStreamSample(bank, 0, second, flags);
    }
    READABLE_FUNCTION_WAITS(playStreamSample, Verified);

    // Every frame: once both channels have finished, the next queued playlist.
    uint32_t soundFrame()
    {
        ee::Local<uint32_t> status;
        if (*g_streamChannelsOpen == 0 || *g_queueBusy != 0 || !*g_queueHead)
            return 0;
        for (uint32_t i = 0; i < kStreamChannels; ++i)
        {
            const StreamChannel &channel = g_music->channels[i];
            if (!channel.stream)
                continue;
            zge::streamStatus(channel.stream, ee::Ptr<uint32_t>(status.address()));
            if (!streamIdle(*status))
                return 0;
        }
        updateChannelStream((*g_queueHead)->channel, (*g_queueHead)->playlist);
        (*g_queueHead)->channel = kNoQueuedChannel;
        *g_queueHead = (*g_queueHead)->next;
        return 0;
    }
    READABLE_FUNCTION_WAITS(soundFrame, Verified);

    // A playlist asked for on a stream channel. What plays on either channel
    // decides by the playlist's start mode: 0 if it started or was queued, the
    // reason if not. The queue full leaves g_queueBusy set for good: a queue
    // that long stops being served.
    int32_t playChannelStream(uint8_t channelIndex, ee::Ptr<Playlist> playlist)
    {
        ee::Local<uint32_t> status;
        for (uint32_t i = 0; i < kStreamChannels; ++i)
        {
            const StreamChannel &playing = g_music->channels[i];
            if (!playing.stream)
                continue;
            zge::streamStatus(playing.stream, ee::Ptr<uint32_t>(status.address()));
            if (streamIdle(*status))
                continue;

            const uint8_t mode = playlist->flags & kPlaylistStartMask;
            if (mode == kStartNow)
            {
                zge::stopStream(playing.stream);
                clearQueue();
                continue;
            }
            if (mode == kStartIfIdle || (mode == kStartOverLoop && playing.startMode != 0))
                return kStreamChannelBusy;
            if (playing.startMode == 0)
            {
                zge::stopStream(playing.stream);
                continue;
            }

            // Queued behind what is there.
            *g_queueBusy = 1;
            const ee::Ptr<QueuedPlaylist> place = freeQueuePlace();
            if (!place)
                return kQueueFull;
            place->channel = channelIndex;
            place->playlist = playlist;
            place->next = nullptr;
            if (!*g_queueHead)
                *g_queueHead = place;
            else
            {
                ee::Ptr<QueuedPlaylist> last = *g_queueHead;
                while (last->next)
                    last = last->next;
                last->next = place;
            }
            *g_queueBusy = 0;
            return kPlayed;
        }
        return updateChannelStream(channelIndex, playlist);
    }
    READABLE_FUNCTION_WAITS(playChannelStream, Verified);

    // A channel's stream stopped if it is still going, and the queue emptied.
    uint32_t stopFinishedStream(uint8_t channelIndex)
    {
        ee::Local<uint32_t> status;
        const StreamChannel &channel = g_music->channels[channelIndex];
        zge::streamStatus(channel.stream, ee::Ptr<uint32_t>(status.address()));
        if (!streamIdle(*status))
        {
            zge::stopStream(channel.stream);
            clearQueue();
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(stopFinishedStream, Draft);

    // A channel closed and what was queued for it taken out. A place taken
    // out stays the `previous` of the next, so of two in a row the second
    // stays linked in the queue, marked free.
    void stopStreamChannel(uint8_t channelIndex)
    {
        StreamChannel &channel = g_music->channels[channelIndex];
        closeSoundStream(channel.stream);
        channel.stream = nullptr;
        *g_queueBusy = 1;
        ee::Ptr<QueuedPlaylist> previous;
        for (ee::Ptr<QueuedPlaylist> queued = *g_queueHead; queued; queued = queued->next)
        {
            if (queued->channel == channelIndex)
            {
                if (previous)
                    previous->next = queued->next;
                else
                    *g_queueHead = queued->next;
                queued->channel = kNoQueuedChannel;
            }
            previous = queued;
        }
        *g_queueBusy = 0;
    }
    READABLE_FUNCTION_WAITS(stopStreamChannel, Verified);

    void resetStreamChannels()
    {
        for (StreamChannel &channel : g_music->channels)
            channel.stream = nullptr;
        for (QueuedPlaylist &queued : g_music->queue)
        {
            queued.channel = kNoQueuedChannel;
            queued.next = nullptr;
        }
        *g_queueSearchStart = 0;
        *g_streamChannelsOpen = 1;
        *g_queueHead = nullptr;
        *g_queueBusy = 0;
    }
    READABLE_FUNCTION(resetStreamChannels, Verified);

    void closeStreamChannels()
    {
        for (StreamChannel &channel : g_music->channels)
        {
            if (channel.stream)
            {
                closeSoundStream(channel.stream);
                channel.stream = nullptr;
            }
        }
        *g_streamChannelsOpen = 0;
    }
    READABLE_FUNCTION_WAITS(closeStreamChannels, Verified);

    // A channel's stream's status, or why it could not be asked.
    int32_t soundVoiceState(uint8_t channelIndex)
    {
        ee::Local<uint32_t> status;
        const int32_t failed = static_cast<int32_t>(zge::streamStatus(g_music->channels[channelIndex].stream, ee::Ptr<uint32_t>(status.address())));
        return failed != 0 ? failed : static_cast<int32_t>(*status);
    }
    READABLE_FUNCTION(soundVoiceState, Verified);

    // (The callers' third word is a pan nothing reads.)
    void setChannelVolume(uint8_t channelIndex, uint8_t volume, uint32_t /*pan*/)
    {
        setStreamVolume(g_music->channels[channelIndex].stream, volume);
    }
    READABLE_FUNCTION_WAITS(setChannelVolume, Verified);

    // A stream channel's stream paused, playing again (the games call them at
    // their pause and resume; were resumeStreamVoice and pauseStreamVoice, the
    // other way round).
    void pauseChannelStream(uint8_t channelIndex)
    {
        zge::pauseSoundStream(g_music->channels[channelIndex].stream, 1);
    }
    READABLE_FUNCTION_WAITS(pauseChannelStream, Verified);

    void resumeChannelStream(uint8_t channelIndex)
    {
        zge::pauseSoundStream(g_music->channels[channelIndex].stream, 0);
    }
    READABLE_FUNCTION_WAITS(resumeChannelStream, Verified);

    ee::Ptr<SoundStream> streamChannelStream(uint8_t channelIndex) { return g_music->channels[channelIndex].stream; }
    READABLE_FUNCTION(streamChannelStream, Verified);

    // A music parameter set; the next chosen or blended track reads it.
    void setMusicParameter(uint32_t parameter, uint32_t value) { g_music->parameters[parameter] = value; }
    READABLE_FUNCTION(setMusicParameter, Verified);

    uint32_t musicParameter(uint32_t parameter) { return g_music->parameters[parameter]; }
    READABLE_FUNCTION(musicParameter, Draft);
}
