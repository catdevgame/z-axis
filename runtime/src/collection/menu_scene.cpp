// What moves in the collection's menu (collection/menu_scene.h).
#include "collection/menu_scene.h"

#include "runtime/ps2_game_files.h"
#include "runtime/ps2_pss_movie.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps2x::collection
{
    namespace
    {
        // The movie readers read through the games' file system: the file's
        // folder as a disc of one file.
        std::shared_ptr<ps2x::game_files::GameFile> openHostFile(const std::filesystem::path &file)
        {
            ps2x::game_files::GameFileRoots roots;
            roots.disc = file.parent_path();
            ps2x::game_files::GameFileSystem files(std::move(roots));
            return files.open(file.filename().string());
        }

        // A movie's picture as the canvas's (both RGBA bytes, R first).
        void toImage(const ps2x::movie::Picture &picture, Image &out)
        {
            out.width = picture.width;
            out.height = picture.height;
            out.pixels.resize(static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height));
            for (size_t i = 0; i < out.pixels.size(); ++i)
            {
                const uint8_t *p = picture.rgba.data() + i * 4;
                out.pixels[i] = rgba(p[0], p[1], p[2]);
            }
        }
    }

    std::unique_ptr<ps2x::movie::PssVideo> openMovieVideo(const std::filesystem::path &file)
    {
        std::shared_ptr<ps2x::game_files::GameFile> opened = openHostFile(file);
        if (!opened)
            return nullptr;
        return std::make_unique<ps2x::movie::PssVideo>(std::move(opened));
    }

    // ---- MenuScene -------------------------------------------------------------

    MenuScene::MenuScene() = default;
    MenuScene::~MenuScene() = default;

    void MenuScene::setGames(std::shared_ptr<const std::vector<FrontGame>> games)
    {
        m_games = std::move(games);
        const int shown = m_shown;
        m_shown = -1;
        m_video.reset();
        m_picture = {};
        if (shown >= 0)
            show(shown, m_shownAt);
    }

    void MenuScene::openLoop(size_t index)
    {
        m_video.reset();
        const FrontGame &game = (*m_games)[static_cast<size_t>(m_shown)];
        if (index < game.loops.size())
        {
            m_loop = index;
            m_video = openMovieVideo(game.loops[index]);
        }
    }

    void MenuScene::show(int game, double now)
    {
        if (game == m_shown)
            return;
        m_shown = m_games && game >= 0 && game < static_cast<int>(m_games->size()) ? game : -1;
        m_shownAt = now;
        m_video.reset();
        m_picture = {};
        m_ended = false;
        m_nextAt = now;
        if (m_shown >= 0)
            openLoop(0);
    }

    bool MenuScene::update(double now)
    {
        if (m_shown < 0 || m_ended || now < m_nextAt || (!m_moving && !m_picture.empty()))
            return false;
        const FrontGame &game = (*m_games)[static_cast<size_t>(m_shown)];
        ps2x::movie::Picture picture;
        // The loops one after another: at the end of one the next, over
        // and over (a loop that gives no picture at all ends the movie).
        for (size_t tries = 0; tries <= game.loops.size(); ++tries)
        {
            if (m_video && m_video->next(picture))
            {
                toImage(picture, m_picture);
                const double fps = m_video->framesPerSecond() > 0.0 ? m_video->framesPerSecond() : 30000.0 / 1001.0;
                // (a menu that was not drawn for a while does not catch up)
                m_nextAt = std::max(m_nextAt + 1.0 / fps, now - 0.1);
                return true;
            }
            openLoop(game.loops.empty() ? 0 : (m_loop + 1) % game.loops.size());
        }
        m_ended = true;
        return false;
    }

    Backdrop MenuScene::backdrop(double now) const
    {
        Backdrop out;
        if (m_shown < 0)
            return out;
        const FrontGame &game = (*m_games)[static_cast<size_t>(m_shown)];
        if (!m_picture.empty())
            out.picture = &m_picture;
        else if (!game.still.empty())
        {
            // Four fifths of the still's part's width (FrontGame::stillPart),
            // panned across it and back; nothing outside the part shows.
            out.picture = &game.still;
            const Rect part = game.stillPart.empty() ? Rect{0, 0, game.still.width, game.still.height} : game.stillPart;
            const int width = part.width * 4 / 5;
            const double phase = std::fmod(std::max(0.0, now - m_shownAt), kStillPan) / kStillPan;
            const double across = phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0;
            const double eased = 0.5 - 0.5 * std::cos(across * 3.14159265358979);
            out.part = {part.x + static_cast<int>(std::lround((part.width - width) * eased)), part.y, width, part.height};
        }
        // The riders two at a time, the pairs in the roster's order, each
        // fading in and out at the ends of its turn.
        if (!game.riders.empty())
        {
            const size_t pairs = (game.riders.size() + 1) / 2;
            const double since = std::max(0.0, now - m_shownAt);
            const size_t pair = static_cast<size_t>(since / kRiderTurn) % pairs;
            const double into = std::fmod(since, kRiderTurn);
            out.riders[0] = &game.riders[pair * 2];
            out.riders[1] = pair * 2 + 1 < game.riders.size() ? &game.riders[pair * 2 + 1] : nullptr;
            if (pairs > 1)
            {
                const double faded = std::min(since < kRiderTurn ? kRiderFade : into, kRiderTurn - into); // (the first pair comes at once)
                out.riderOpacity = static_cast<float>(std::clamp(faded / kRiderFade, 0.0, 1.0));
            }
        }
        return out;
    }

    // ---- IntroMovie ------------------------------------------------------------

    IntroMovie::IntroMovie() = default;
    IntroMovie::~IntroMovie() = default;

    bool IntroMovie::open(const MovieClip &clip)
    {
        m_video.reset();
        m_sound.reset();
        if (clip.empty())
            return false;
        std::shared_ptr<ps2x::game_files::GameFile> file = openHostFile(clip.movie);
        if (!file)
            return false;
        m_video = std::make_unique<ps2x::movie::PssVideo>(file);
        auto sound = std::make_shared<ps2x::movie::PssAudio>(file);
        if (sound->hasSound())
            m_sound = std::move(sound);
        ps2x::movie::Picture picture;
        if (!m_video->next(picture))
        {
            m_video.reset();
            m_sound.reset();
            return false;
        }
        toImage(picture, m_picture);
        m_fps = m_video->framesPerSecond() > 0.0 ? m_video->framesPerSecond() : 30000.0 / 1001.0;
        m_length = clip.end;
        m_shown = 1;
        m_startedAt = -1.0;
        return true;
    }

    bool IntroMovie::update(double now)
    {
        if (!m_video)
            return false;
        if (m_startedAt < 0.0)
            m_startedAt = now;
        const double at = now - m_startedAt;
        if (m_length > 0.0 && at >= m_length)
            return false;
        // The pictures due by now (a late frame skips to the one due).
        const size_t due = static_cast<size_t>(at * m_fps) + 1;
        ps2x::movie::Picture picture;
        bool more = true;
        while (m_shown < due && (more = m_video->next(picture)))
        {
            ++m_shown;
            if (m_shown == due)
                toImage(picture, m_picture);
        }
        return more;
    }

    float IntroMovie::black(double now) const
    {
        if (m_length <= 0.0 || m_startedAt < 0.0)
            return 0.0f;
        const double left = m_length - (now - m_startedAt);
        return static_cast<float>(std::clamp(1.0 - left / kFadeOut, 0.0, 1.0));
    }

    size_t IntroMovie::soundFrames() const
    {
        return m_length > 0.0 ? static_cast<size_t>(m_length * 48000.0) : 0u;
    }
}
