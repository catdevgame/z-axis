#pragma once

// What moves in the collection's menu, from the games' own front-end files
// (collection/front_art.h): the shown game's menu movie played over and over
// behind the menu (its loops one after another), or its still with a slow
// pan, and its riders in pairs that take turns; and the intro, the Z-Axis
// logo part of a game's developer movie with its sound, played once at
// launch. Movies are decoded off the menu's thread (runtime/ps2_pss_movie.h)
// and taken at their own rate; everything else is plain arithmetic on the
// time the menu gives.

#include "collection/front_art.h"
#include "collection/menu.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace zx::movie
{
    class PssAudio;
    class PssVideo;
}

namespace zx::collection
{
    class MenuScene
    {
    public:
        // A rider pair's time on screen, and its fade in and out.
        static constexpr double kRiderTurn = 8.0, kRiderFade = 0.6;
        // A still's pan, there and back.
        static constexpr double kStillPan = 24.0;

        MenuScene();
        ~MenuScene();
        MenuScene(const MenuScene &) = delete;
        MenuScene &operator=(const MenuScene &) = delete;

        // The games, in the menu's order (loadFrontGames); after a setup,
        // their files as they are now (the shown game's movie starts again).
        void setGames(std::shared_ptr<const std::vector<FrontGame>> games);

        // The game shown behind the menu (its index; -1 none), from `now`
        // (seconds, any steady clock): its movie from the start, its riders'
        // first pair. The same game again changes nothing.
        void show(int game, double now);
        int shown() const { return m_shown; }

        // Whether movies play (false: the first picture of each, still).
        void setMoving(bool on) { m_moving = on; }

        // Takes the movie's next picture when its time has come. True when
        // the picture behind the menu changed (to draw again).
        bool update(double now);

        // What to draw behind the menu now (Backdrop's picture, part and
        // riders; its fades are the app's).
        Backdrop backdrop(double now) const;

    private:
        void openLoop(size_t index);

        std::shared_ptr<const std::vector<FrontGame>> m_games;
        int m_shown = -1;
        double m_shownAt = 0.0;
        bool m_moving = true;
        std::unique_ptr<zx::movie::PssVideo> m_video;
        size_t m_loop = 0;     // the shown game's loop playing
        double m_nextAt = 0.0; // when its next picture is due
        Image m_picture;       // its picture now
        bool m_ended = false;  // no loop gives a picture: the last one stays
    };

    // The intro: a clip of a movie with its sound, from its start to
    // `clip.end` (0: the movie's end). Pictures come at the movie's rate from
    // the time of the first one; the sound goes to the menu's audio.
    class IntroMovie
    {
    public:
        // The last part of the clip fades to black (and its sound out).
        static constexpr double kFadeOut = 0.4;

        IntroMovie();
        ~IntroMovie();
        IntroMovie(const IntroMovie &) = delete;
        IntroMovie &operator=(const IntroMovie &) = delete;

        // False when the movie cannot be opened or gives no picture.
        bool open(const MovieClip &clip);
        // The clip's length in seconds as its pictures count it.
        double length() const { return m_length; }
        // The picture for `now` (the first call starts the clock); false
        // after the clip's end.
        bool update(double now);
        const Image &picture() const { return m_picture; }
        // 0-1, black over the picture as the clip ends.
        float black(double now) const;
        // The clip's sound (48 kHz stereo, MenuAudio::playMovie), null for a
        // movie without one.
        std::shared_ptr<zx::movie::PssAudio> sound() const { return m_sound; }
        // The frames of sound the clip lasts (its end).
        size_t soundFrames() const;

    private:
        std::unique_ptr<zx::movie::PssVideo> m_video;
        std::shared_ptr<zx::movie::PssAudio> m_sound;
        Image m_picture;
        double m_fps = 30000.0 / 1001.0;
        double m_length = 0.0;
        double m_startedAt = -1.0;
        size_t m_shown = 0; // pictures given
    };

    // A movie file on the host for the movie readers (runtime/ps2_pss_movie.h).
    std::unique_ptr<zx::movie::PssVideo> openMovieVideo(const std::filesystem::path &file);
}
