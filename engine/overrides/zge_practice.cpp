// Practice's host side (zge_practice.h): the keys, the tries' clock and the
// player's lines.

#include "zge_practice.h"

#include "runtime/boot_config.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace zge_host::practice
{
    namespace
    {
        // The app's row (collection/practice.h).
        constexpr const char *kSettingKey = "game.practice";
        // How long a line of ours stays (seconds), and the running clock's
        // refresh: a short line, put up again every few frames, so it goes
        // soon after the try stops being timed (a pause, the level left).
        constexpr double kLineSeconds = 2.5;
        constexpr double kClockSeconds = 0.3;
        constexpr uint32_t kClockEveryFrames = 6;
        // Frames a line (the point made, a try's time) is left up before the
        // running clock takes the notice again.
        constexpr uint32_t kLineFrames = 150;
    }

    bool enabled()
    {
        if (zx::settings::has(kSettingKey))
            return zx::settings::value(kSettingKey, 0) != 0;
        const char *env = zx::boot::text("ZX_PRACTICE");
        return env && env[0] == '1';
    }

    std::string timeText(double seconds)
    {
        const long hundredths = static_cast<long>(std::max(0.0, seconds) * 100.0 + 0.5);
        const long minutes = hundredths / 6000;
        const long rest = hundredths % 6000;
        char text[32];
        if (minutes > 0)
            std::snprintf(text, sizeof(text), "%ld:%02ld.%02ld", minutes, rest / 100, rest % 100);
        else
            std::snprintf(text, sizeof(text), "%ld.%02ld", rest / 100, rest % 100);
        return text;
    }

    Practice &current() { return zx::session::state<Practice>(); }

    void Practice::show(const std::string &text, double seconds)
    {
        // Another's notice (a screenshot saved, photo mode's help) is left up.
        const std::string now = zx::settings::notice();
        if (!now.empty() && now != shown_)
            return;
        zx::settings::notify(text, seconds);
        shown_ = text;
    }

    bool Practice::onVsync(bool pointPressed, bool restartPressed)
    {
        if (!enabled())
        {
            if (hasPoint_)
                leave();
            pending_ = Request::None;
            return false;
        }
        if (pointPressed)
            pending_ = Request::SetPoint;
        if (restartPressed && hasPoint_)
        {
            pending_ = Request::Return;
            return true;
        }
        return false;
    }

    Request Practice::take()
    {
        const Request request = pending_;
        pending_ = Request::None;
        return request;
    }

    void Practice::pointSet()
    {
        hasPoint_ = true;
        elapsed_ = 0.0;
        tries_.clear();
        holdFrames_ = kLineFrames;
        std::cout << "[practice] restart point set" << std::endl;
        show("Restart point set", kLineSeconds);
    }

    void Practice::pointRefused(const std::string &why)
    {
        std::cout << "[practice] no restart point: " << why << std::endl;
        holdFrames_ = kLineFrames;
        show(why, kLineSeconds);
    }

    void Practice::returned()
    {
        tries_.push_back(elapsed_);
        const double best = *std::min_element(tries_.begin(), tries_.end());
        std::cout << "[practice] try " << tries_.size() << ": " << timeText(elapsed_) << " s (best " << timeText(best)
                  << " s)" << std::endl;
        show("Try " + std::to_string(tries_.size()) + ": " + timeText(elapsed_) + "   Best " + timeText(best),
             kLineSeconds);
        holdFrames_ = kLineFrames;
        elapsed_ = 0.0;
    }

    void Practice::returnFailed(const std::string &why)
    {
        std::cout << "[practice] not back at the point: " << why << std::endl;
        holdFrames_ = kLineFrames;
        show(why, kLineSeconds);
    }

    void Practice::played(float seconds)
    {
        if (!hasPoint_)
            return;
        elapsed_ += seconds;
        ++frames_;
        if (holdFrames_ > 0)
        {
            --holdFrames_;
            return;
        }
        if (frames_ % kClockEveryFrames == 0)
            show("Try " + std::to_string(tries_.size() + 1) + "   " + timeText(elapsed_), kClockSeconds);
    }

    void Practice::leave()
    {
        if (hasPoint_)
            std::cout << "[practice] restart point dropped after " << tries_.size() << " tries" << std::endl;
        pending_ = Request::None;
        hasPoint_ = false;
        elapsed_ = 0.0;
        tries_.clear();
        holdFrames_ = 0;
    }
}
