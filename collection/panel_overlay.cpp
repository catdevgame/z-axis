#include "collection/panel_overlay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace panel
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto kPaintEvery = std::chrono::milliseconds(50); // about 20 Hz, as the second display
        constexpr double kCaptureStale = 1.0; // seconds without a capture: no game runs, the picture is blank
        constexpr size_t kPixels = static_cast<size_t>(kScreenWidth) * static_cast<size_t>(kScreenHeight);

        double secondsNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
    }

    Overlay &Overlay::shared()
    {
        static Overlay overlay;
        return overlay;
    }

    Overlay::~Overlay() { stopPainter(); }

    void Overlay::setSecondScreen(bool up)
    {
        secondScreen_.store(up, std::memory_order_release);
        if (up)
            std::fprintf(stderr, "[overlay] a second display shows the bottom screen: no overlay\n");
    }

    bool Overlay::secondScreen() const { return secondScreen_.load(std::memory_order_acquire); }

    bool Overlay::shown() const { return shown_.load(std::memory_order_acquire); }

    void Overlay::setShown(bool shown)
    {
        if (shown == this->shown())
            return;
        if (!shown)
        {
            shown_.store(false, std::memory_order_release);
            stopPainter();
            // A finger or button held over it lets go (applied as the next show starts).
            std::lock_guard<std::mutex> lock(inputMutex_);
            inputs_.clear();
            for (const int pointer : held_)
                inputs_.push_back(Input{false, TouchEvent{TouchPhase::Cancel, pointer, 0.0f, 0.0f, secondsNow()}, 0.0f});
            held_.clear();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(pictureMutex_);
            fresh_ = false; // (an old picture is not shown again)
            front_.assign(kPixels, 0u);
            back_.assign(kPixels, 0u);
        }
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            stopping_ = false;
        }
        shown_.store(true, std::memory_order_release);
        painter_ = std::thread([this] { paintLoop(); });
    }

    void Overlay::stopPainter()
    {
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            stopping_ = true;
        }
        stopSignal_.notify_all();
        if (painter_.joinable())
            painter_.join();
    }

    void Overlay::capture(const uint8_t *guestRam, size_t ramBytes)
    {
        if (!guestRam || !shown_.load(std::memory_order_acquire))
            return;
        if (!captured_)
            captured_ = std::make_unique<Snapshot>(); // (large: not on the game thread's stack)
        captureSnapshot(guestRam, ramBytes, *captured_);
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshot_ = *captured_;
        }
        capturedAt_.store(secondsNow(), std::memory_order_release);
    }

    void Overlay::touch(TouchPhase phase, int pointer, float x, float y)
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        const auto held = std::find(held_.begin(), held_.end(), pointer);
        if (phase == TouchPhase::Down && held == held_.end())
            held_.push_back(pointer);
        else if ((phase == TouchPhase::Up || phase == TouchPhase::Cancel) && held != held_.end())
            held_.erase(held);
        inputs_.push_back(Input{false, TouchEvent{phase, pointer, x, y, secondsNow()}, 0.0f});
    }

    void Overlay::wheel(float x, float y, float notches)
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        inputs_.push_back(Input{true, TouchEvent{TouchPhase::Down, 0, x, y, secondsNow()}, notches});
    }

    bool Overlay::takePicture(const std::function<void(const uint32_t *pixels, bool level, HudPieces shows)> &use)
    {
        std::lock_guard<std::mutex> lock(pictureMutex_);
        if (!fresh_ || front_.size() != kPixels)
            return false;
        fresh_ = false;
        use(front_.data(), frontLevel_, frontShows_);
        return true;
    }

    Overlay::Cost Overlay::takeCost()
    {
        std::lock_guard<std::mutex> lock(pictureMutex_);
        const Cost cost = cost_;
        cost_ = Cost{};
        return cost;
    }

    void Overlay::paintLoop()
    {
        if (!panel_)
            panel_ = std::make_unique<Panel>();
        if (!painted_)
            painted_ = std::make_unique<Snapshot>(); // (large: not on the painter's stack)
        Snapshot &snapshot = *painted_;
        std::vector<Input> inputs;
        auto next = Clock::now();
        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(stopMutex_);
                if (stopSignal_.wait_until(lock, next, [this] { return stopping_; }))
                    break;
            }
            next += kPaintEvery;
            if (const auto now = Clock::now(); next < now)
                next = now;

            // The running game's panel files (the app sets them as a game boots).
            if (const std::string folder = gameFiles(), fallback = gameFilesFallback();
                folder != dataFolder_ || fallback != dataFallback_)
            {
                dataFolder_ = folder;
                dataFallback_ = fallback;
                panel_->setDataFolder(folder, fallback);
            }
            {
                std::lock_guard<std::mutex> lock(inputMutex_);
                inputs.swap(inputs_);
            }
            for (const Input &input : inputs)
            {
                if (input.isWheel)
                    panel_->wheel(input.touch.x, input.touch.y, input.notches, input.touch.seconds);
                else
                    panel_->touch(input.touch);
            }
            inputs.clear();
            {
                std::lock_guard<std::mutex> lock(snapshotMutex_);
                snapshot = snapshot_;
            }
            if (secondsNow() - capturedAt_.load(std::memory_order_acquire) > kCaptureStale)
                snapshot.mode = GameMode::NoLevel; // nothing is running: blank, not the last level

            const auto begin = Clock::now();
            panel_->draw(snapshot, secondsNow(), Surface{back_.data(), kScreenWidth, kScreenHeight, kScreenWidth});
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            {
                std::lock_guard<std::mutex> lock(pictureMutex_);
                front_.swap(back_);
                frontLevel_ = snapshot.mode != GameMode::NoLevel;
                frontShows_ = panel_->shows();
                fresh_ = true;
                ++cost_.paints;
                cost_.totalMs += ms;
                cost_.worstMs = std::max(cost_.worstMs, ms);
            }
        }
    }
}
