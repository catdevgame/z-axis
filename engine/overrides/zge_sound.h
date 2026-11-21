#pragma once

// The ZAudio sound driver on the host, shared by the games built on the
// Z-Axis engine (zge_sound.cpp). On the PS2 every sound command goes through
// the game's one RPC function, soundRpc(fn, send, size, wantReply), to
// Z-Axis' ZAUDIO.IRX on the IOP, which plays sample banks on 40 SPU2 voices,
// streams music and speech from the disc into four more voice pairs and
// writes status back into EE RAM sixty times a second. Here soundRpc and its
// bind loop are host functions and the driver is native: the same commands,
// the same replies and status, the sound made in guest time and mixed into
// effects, music and speech buses for the host's output.
//
// The games differ in their ZAUDIO.IRX: where its tables are, how many
// samples it holds, what Alloc replies and ChanSetParam takes, a fade's end
// flags, and above all its streams (StreamRings). Each game's override
// describes its own in a Game record and installs the driver with it:
// aggressive_inline_sound.cpp, dave_mirra_sound.cpp, bmx_xxx_sound.cpp.
//
// The extras every game gets: the custom soundtrack (<mods>/Music/NN*.wav,
// .ogg, .flac or .mp3 in place of MUSIC.ZSD's song NN) and its songs' titles
// wherever the game shows them (its playlist's text, or its song info's
// string keys). A game can add its own replacement of a stream's
// source (Game::resolve: AI's French speech).

#include <cstdint>
#include <memory>

class PS2Machine;

namespace zx::audio_file
{
    class Reader;
}

namespace zx::game_files
{
    class GameFile;
}

namespace zge_host::sound
{
    // The two ZAUDIO.IRX stream designs.
    enum class StreamRings
    {
        // ZGE3's (Aggressive Inline, BMX XXX): StrmAlloc gives each stream a
        // 64 KB SPU ring per channel; a refill reads half a ring per channel
        // from the source; the stream ends when its voices have played it.
        PerStream,
        // Dave Mirra BMX 2's older ZAUDIO: two 64 KB rings made at the first
        // Init and shared by the streams; Play reads two 64 KB pieces, every
        // refill one more, whatever the channel count; the end is "sectors
        // read >= total - 1", and the last piece is not heard; the volume
        // command sets the left voice's left side and the right voice's right
        // side only, so a mono stream plays on the left.
        Shared,
    };

    // What ChanSetParam's sample word is.
    enum class ChannelSample
    {
        Handle,     // a sample's handle: the channel plays it from its start
        SpuAddress, // an SPU address: a sound anywhere inside an allocation (a bank kept as one)
    };

    // The buses the driver mixes, a stream's by its file's name.
    enum Bus
    {
        Effects,
        Music,
        Speech,
        BusCount,
    };

    // A stream about to play: its source on the disc.
    struct StreamSource
    {
        Bus bus = Effects;
        std::shared_ptr<zx::game_files::GameFile> file;
        uint64_t start = 0; // the source's first byte in the file
        bool stereo = false;
    };

    // What plays instead of a stream's source (no reader: the disc's).
    struct Replacement
    {
        std::unique_ptr<zx::audio_file::Reader> reader;
        // At the reader's own rate (a song of the player's), or at the
        // stream's pitch (a line from another release, made at the same rate).
        bool ownRate = false;
    };

    // A game's ZAUDIO and what its EE side calls, by symbol.
    struct Game
    {
        uint32_t rpc = 0;          // soundRpc(fn, send, size, wantReply): every command
        uint32_t bind = 0;         // bindSoundRpc: binds ZAUDIO's RPC server, looped until it answers
        uint32_t replyBuffer = 0;  // soundReply: where soundRpc's reply arrives

        // ZAudio's handles are addresses in its sample and stream tables
        // (where the game's IOP loads the module); opaque to the EE.
        uint32_t sampleTable = 0;
        uint32_t streamTable = 0;
        uint32_t streamStride = 0;   // a stream table entry's size
        uint32_t sampleSlots = 0;    // the sample table's entries
        // Alloc's reply has a second word: the SPU address of the last
        // sample allocated (it stays in the reply record).
        bool replyHasSpuAddress = false;
        ChannelSample channelSample = ChannelSample::Handle;
        // A fade's end (its flags, fn 10): ends the stream (state 8, keyed
        // off at the next ring half), or leaves it in state 6 (as paused, at
        // its pitch: no refills; on the SPU2 the voices go round the ring,
        // here on through the source, at volume 0).
        uint32_t fadeEndStops = 0;
        uint32_t fadeEndHolds = 0;
        StreamRings rings = StreamRings::PerStream;

        // The custom soundtrack: MUSIC.ZSD's waves 0 .. songs - 1 are the
        // playlist's songs, replaced by <mods>/Music/NN*.* (0: none). Where
        // a bank file's header has its wave count and its wave table's
        // offset ({offset, size} a wave; ZGE4 moved them for its hash tables).
        uint32_t songs = 0;
        uint32_t bankWaveCount = 0x20u;
        uint32_t bankWaves = 0x24u;
        // Their titles, where the game keeps them as text in its playlist:
        // setPlaylistEntry(soundtrack, index, title, artist) and the global
        // holding the game's own soundtrack (0: none).
        uint32_t setPlaylistEntry = 0;
        uint32_t gameSoundtrack = 0;
        // Or where the game names them by string keys (its playlist and its
        // song info show "$KSTR_SONGINFO_<n>_SONGNAME" and the like):
        // stringTableText(key's nameHash), and the keys' common start, a
        // printf format of the song's index ("KSTR_SONGINFO_%u_"), to which
        // SONGNAME, ARTIST, ALBUM and LABEL are added (null: none).
        uint32_t stringTableText = 0;
        const char *songKey = nullptr;

        // The game's own replacement of a stream's source, after the custom
        // soundtrack's (null: none).
        Replacement (*resolve)(const StreamSource &source) = nullptr;
    };

    // The native driver, unless ZX_SOUND=spu2 or ZX_MOVIES=hle (then
    // ZAUDIO runs on the IOP with the emulated SPU2, and ZX_SOUND_LOG still
    // logs its commands). True when installed: ZAUDIO.IRX and LIBSD.IRX are
    // then not needed.
    //
    // ZX_SOUND_LOG=<file>: every command and the channel status bytes when
    // they change, with the vsync tick, in either mode. ZX_SOUND_MEMDUMP=
    // <file>: the sample memory. ZX_TRACE_SOUND=1: banks, streams, status
    // switches; 2: every command. ZX_SOUND_BUS_GAIN=e,m,s: debug gains.
    bool install(PS2Machine &runtime, const Game &game);
}
