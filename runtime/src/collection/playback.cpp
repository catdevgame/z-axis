// A recording played back: its controls and the second screen's strip
// (collection/playback.h).
#include "collection/playback.h"

#include "collection/recordings.h"
#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace zx::collection
{
    PlaybackCommand playbackCommand(const std::vector<int> &keysPressed, uint16_t padButtonsPressed, bool paused)
    {
        namespace hi = ps2_host_input;
        static const int kSpace = hi::hostCodeByName("Space"), kEscape = hi::hostCodeByName("Escape");
        static const int kEnter = hi::hostCodeByName("Enter"), kKeypadEnter = hi::hostCodeByName("KP_Enter");
        auto key = [&](int code) { return code >= 0 && std::find(keysPressed.begin(), keysPressed.end(), code) != keysPressed.end(); };
        auto pad = [padButtonsPressed](int button) { return (padButtonsPressed & (1u << button)) != 0; };
        if (!paused)
            return pad(hi::kStart) || key(kSpace) || key(kEscape) ? PlaybackCommand::Pause : PlaybackCommand::None;
        // (Leave first: Escape both pauses and, paused, leaves)
        if (pad(hi::kCircle) || key(kEscape))
            return PlaybackCommand::Leave;
        if (pad(hi::kStart) || pad(hi::kCross) || key(kSpace) || key(kEnter) || key(kKeypadEnter))
            return PlaybackCommand::Resume;
        return PlaybackCommand::None;
    }

    std::string playbackPosition(uint64_t vsync, uint64_t endVsync)
    {
        return lengthText(std::min(vsync, endVsync)) + " / " + lengthText(endVsync);
    }

    std::string playbackLine(uint64_t vsync, uint64_t endVsync, bool paused, HintDevice device)
    {
        std::string line = std::string(paused ? "Paused " : "Playback ") + playbackPosition(vsync, endVsync);
        // The buttons as the menu's bottom line names them: Start pauses (on
        // the keyboard Space), paused the choose button goes on, the back
        // button leaves.
        std::string buttons;
        if (!paused)
            buttons = device == HintDevice::Touch      ? std::string()
                      : device == HintDevice::Keyboard ? std::string("Space Pause")
                                                       : std::string("Start Pause");
        else
            buttons = buttonHint(device, "Resume", "Leave");
        return buttons.empty() ? line : line + "   " + buttons;
    }

    namespace
    {
        constexpr int kDesignWidth = 1240, kDesignHeight = 1080;
        constexpr int kMargin = 40, kGap = 20, kBand = 150, kInset = 15, kButtonWidth = 260;

        void centred(Canvas &canvas, const BitmapFont &font, const Rect &box, const std::string &text, float scale, uint32_t colour)
        {
            const FittedLine line = fitLine(font, text, box.width - box.width / 10, scale, scale * 0.6f);
            const int height = static_cast<int>(std::lround(font.height * line.scale));
            canvas.text(font, box.x + (box.width - line.width) / 2, box.y + (box.height - height) / 2, line.text, line.scale, colour);
        }
    }

    StripLayout stripLayout(int width, int height)
    {
        const float sx = static_cast<float>(width) / kDesignWidth, sy = static_cast<float>(height) / kDesignHeight;
        const auto X = [sx](int v) { return static_cast<int>(std::lround(v * sx)); };
        // The buttons at least 120 pixels each way, for a thumb, with a
        // margin above and below them in the band.
        const int inset = std::max(4, static_cast<int>(std::lround(kInset * sy)));
        const int band = std::min(height, std::max(static_cast<int>(std::lround(kBand * sy)), 120 + 2 * inset));
        const int buttonHeight = std::max(0, band - 2 * inset);
        const int buttonWidth = std::max(120, X(kButtonWidth));
        StripLayout out;
        out.strip = {0, height - band, width, band};
        const int top = out.strip.y + inset;
        out.leave = {width - X(kMargin) - buttonWidth, top, buttonWidth, buttonHeight};
        out.pause = {out.leave.x - X(kGap) - buttonWidth, top, buttonWidth, buttonHeight};
        out.line = {X(kMargin), top, std::max(0, out.pause.x - X(kGap) - X(kMargin)), buttonHeight};
        return out;
    }

    void drawStrip(Canvas &band, const StripLayout &layout, const StripView &view, const BitmapFont &font)
    {
        const float scale = static_cast<float>(layout.strip.width) / kDesignWidth;
        const int frame = std::max(1, static_cast<int>(std::lround(3 * scale)));
        auto local = [&](Rect r)
        {
            r.y -= layout.strip.y;
            return r;
        };
        band.clear(rgba(12, 12, 14));
        band.fill({0, 0, band.width(), frame}, rgba(220, 220, 220));
        const Rect line = local(layout.line);
        const FittedLine text = fitLine(font, std::string(view.paused ? "Paused " : "Playback ") + view.position, line.width,
                                        1.6f * scale, 1.0f * scale);
        band.text(font, line.x, line.y + (line.height - static_cast<int>(std::lround(font.height * text.scale))) / 2, text.text,
                  text.scale, rgba(235, 235, 235));
        for (const auto &[rect, label] : {std::pair<Rect, const char *>{local(layout.pause), view.paused ? "Resume" : "Pause"},
                                          std::pair<Rect, const char *>{local(layout.leave), "Leave"}})
        {
            band.fill(rect, rgba(255, 255, 255, 40));
            band.frame(rect, frame, rgba(220, 220, 220));
            centred(band, font, rect, label, 1.5f * scale, rgba(235, 235, 235));
        }
    }

    // ---- PlaybackStrip ---------------------------------------------------------

    PlaybackStrip &PlaybackStrip::shared()
    {
        static PlaybackStrip strip;
        return strip;
    }

    void PlaybackStrip::show(std::optional<StripView> view, std::shared_ptr<const BitmapFont> font)
    {
        std::lock_guard lock(m_mutex);
        if (!view)
        {
            m_down = false;
            m_taps.clear();
        }
        m_view = std::move(view);
        if (font)
            m_font = std::move(font);
    }

    bool PlaybackStrip::shown() const
    {
        std::lock_guard lock(m_mutex);
        return m_view.has_value();
    }

    bool PlaybackStrip::draw(Canvas &band, int width, int height)
    {
        std::optional<StripView> view;
        std::shared_ptr<const BitmapFont> font;
        {
            std::lock_guard lock(m_mutex);
            view = m_view;
            font = m_font;
        }
        if (!view)
            return false;
        static const BitmapFont kNoFont;
        drawStrip(band, stripLayout(width, height), *view, font ? *font : kNoFont);
        return true;
    }

    PlaybackCommand PlaybackStrip::hitAt(float x, float y, int width, int height) const
    {
        if (!m_view)
            return PlaybackCommand::None;
        const StripLayout layout = stripLayout(width, height);
        if (layout.pause.contains(x, y))
            return m_view->paused ? PlaybackCommand::Resume : PlaybackCommand::Pause;
        if (layout.leave.contains(x, y))
            return PlaybackCommand::Leave;
        return PlaybackCommand::None;
    }

    bool PlaybackStrip::press(float x, float y, int width, int height)
    {
        std::lock_guard lock(m_mutex);
        if (!m_view || !stripLayout(width, height).strip.contains(x, y))
            return false;
        m_pressed = hitAt(x, y, width, height);
        m_down = true;
        return true;
    }

    bool PlaybackStrip::release(float x, float y, int width, int height)
    {
        std::lock_guard lock(m_mutex);
        if (!m_down)
            return false;
        m_down = false;
        const PlaybackCommand hit = hitAt(x, y, width, height);
        if (hit != PlaybackCommand::None && hit == m_pressed)
            m_taps.push_back(hit);
        return true;
    }

    std::vector<PlaybackCommand> PlaybackStrip::takeTaps()
    {
        std::lock_guard lock(m_mutex);
        return std::exchange(m_taps, {});
    }
}
