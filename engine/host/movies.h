#pragma once

// The host's services to the engine's movie player on the host (zge/host/
// movie_stream.cpp): a movie file decoded into pictures and sound. One boot's
// state (zx::session): a game booted after another in the same process
// starts with none.
//
// The movie the game opens is its file as the disc device would serve it
// (the Mods folder over the game's own files; ZX_MODS_DIR), decoded by
// FFmpeg (runtime/pss_movie.h): its pictures as RGBA at the stream's
// size and rate, its sound (a PSS's, 48 kHz 16-bit) mixed into the game's
// output in emulated time (runtime/audio_mix.h) while it plays, at the
// volume the game sets.
//
// Skip Intro Movies (game.skip_intro_movies, settings.ini [general]
// skip_intro_movies): each of the game's boot chain movies (Game::introMovies)
// is marked skipped the first time it opens, so the player ends it at its
// first frame; a later play of the same file (an Extras menu) runs as ever.
//
// Xbox movies (a game whose Game has xboxMovies: AI's; ZX_XBOX_PACK/movies,
// made at build time from the user's Xbox disc by tools/modding/xbox_pack.py: the
// Bink file for each PS2 movie, named after its PS2 path): when the setting is
// on (settings row display.xbox_movies, else ZX_XBOX_MOVIES, default on) the
// Bink file plays in place of the PS2 one. The pictures the game gets are at
// the PS2 movie's size (512x256, scaled down from 640x480), so its buffers and
// texture stay as they are; the full 640x480 picture of each frame goes to the
// GPU renderer as the replacement for that frame (runtime/
// texture_replace.h, keyed by the RGB of the frame the game uploads),
// which draws it in place of the small one. The CPU renderer shows the scaled
// frame. Sound: the Bink file's (48 kHz), for movies the game plays with
// sound.
//
// ZX_TRACE_MOVIES=1 logs movie starts, ends and dropped frames.

#include <cstddef>
#include <cstdint>
#include <string>

namespace game::host::movies
{
    // What a game adds to the player.
    struct Game
    {
        const char *name = "";   // for the log
        // The Xbox release's movies (ZX_XBOX_PACK/movies) in place of the
        // PS2's, with their pictures at full size for the GPU renderer.
        bool xboxMovies = false;
        // Skip Intro Movies: the boot chain's movies by file name
        // ("ACCLAIM.PSS"). Empty: the game has none.
        static constexpr size_t kIntroMovies = 4;
        const char *introMovies[kIntroMovies] = {};
    };
    // The boot's game and whether to trace (before its first movie).
    void configure(const Game &game, bool trace);
    // From the player's install until the boot ends: the movie's sound in
    // the game's mix, its pictures among the GPU renderer's replacements, and
    // the Xbox movies' folder (with the developer runner's settings row).
    void attach();
    bool tracing();

    // ---- the movie --------------------------------------------------------

    // A movie the game names that the host plays from a file of its own,
    // for this boot: another release's movie as that disc has it (a Bink or
    // XMV file the player's extras disc gave), where the PS2 disc has no
    // movie of that name (the Zoo's flythrough, DM2's Retro movie, BMX XXX's
    // level previews and logo movie). `gamePath` as the game names it
    // ("assets\\Movies\\Flythrus\\Zoo.PSM", any case); the pictures the game
    // gets are scaled to `width` x `height` (its PS2 movies' size), the full
    // ones go to the GPU renderer, as the Xbox movies' do; its sound plays
    // for a movie the game plays with sound.
    void setHostMovie(const std::string &gamePath, const std::string &file, int width, int height);
    // Whether the host plays the games' movies (not ZX_MOVIES=hle, where
    // the PS2's own decoder does and a host movie cannot play).
    bool playedOnHost();
    // The size of the games' PS2 movies, which host movies are scaled to.
    constexpr int kPs2MovieWidth = 512;
    constexpr int kPs2MovieHeight = 256;

    // The movie at `path` (as the game gave it) becomes the one playing, its
    // sound stopped: Skip Intro Movies' mark taken, the file opened (Mods
    // first, the Xbox movie in its place when there is one). The pictures the
    // last one left with the GPU renderer are dropped. False (and logged)
    // when there is no such file: the movie then has no pictures. A host
    // movie (setHostMovie) of that path plays in its place.
    bool open(const std::string &path);
    // The movie playing again from its start (a loop), its sound stopped.
    void playAgain();
    // Skip Intro Movies ends this one at its first frame.
    bool skipped();
    const std::string &path();

    // ---- its pictures, on the vsync clock -------------------------------------

    // The first picture is due at `vsync`.
    void startPictures(uint64_t vsync);
    // False when the movie has no pictures (no file).
    bool hasPictures();
    enum class Next
    {
        notYet,  // the next picture is not due yet
        picture, // the picture due now (pictures due before it dropped)
        ended,   // none left
    };
    Next nextPicture(uint64_t vsync);
    // The picture nextPicture gave: its size, and its pixels written to
    // guest `address` as the game's upload of a decoded picture reads them
    // (16-pixel wide columns, each the full height rounded up to 16 rows,
    // RGBA with alpha 0x80).
    uint32_t pictureWidth();
    uint32_t pictureHeight();
    void writePicture(uint32_t address);
    uint64_t picturesShown();
    // The GPU renderer's full-size pictures of the movie let go.
    void endPictures();

    // ---- its sound --------------------------------------------------------------

    // Mixed into the game's output from now (at full volume) or not; the
    // movie's sound plays to its end once started.
    void startSound();
    void stopSound();
    // The game's volume for it, 0 to 255.
    void setSoundVolume(uint32_t volume);

    // ---- the log ----------------------------------------------------------------

    // (ZX_TRACE_MOVIES) The movie opened, with sound or not; played again;
    // ended (after how many pictures).
    void traceOpened(bool withSound);
    void tracePlayAgain();
    void traceEnded();
    // Always: the movie ended at its first frame by Skip Intro Movies; a
    // movie that asks for a sound bank of its own (not supported).
    void logSkipped();
    void logSoundBankAsked();
}
