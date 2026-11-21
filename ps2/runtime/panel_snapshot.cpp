// The bottom screen's snapshot registry (runtime/panel_snapshot.h): the
// running game's capture and the folder of its panel files, and what the
// bottom screen shows for Clean Top Screen.
#include "runtime/panel_snapshot.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace panel
{
    namespace
    {
        // Clean Top Screen: the mode (-1 until the environment is read), the
        // pieces the bottom screen last said it shows, and when, and when a
        // capture last gave each piece (steady clock milliseconds; 0 never).
        std::atomic<int> g_cleanTopScreen{-1};
        std::atomic<bool> g_cleanFromEnvironment{false};
        std::atomic<HudPieces> g_bottomPieces{0};
        std::atomic<int64_t> g_bottomPaintedAt{0};
        std::atomic<int64_t> g_pieceGivenAt[static_cast<size_t>(HudPiece::Count)]{};
        constexpr int64_t kBottomScreenGoneMs = 1000; // no picture this long: no bottom screen
        constexpr int64_t kPieceGoneMs = 1000;        // a clock or a goal no capture gave this long: gone

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ZX_CLEAN_TOP_SCREEN's mode, read once.
        void readEnvironment()
        {
            if (g_cleanTopScreen.load(std::memory_order_relaxed) != -1)
                return;
            int expected = -1;
            const char *text = std::getenv("ZX_CLEAN_TOP_SCREEN");
            int mode = static_cast<int>(CleanTopScreen::Off);
            bool given = text && text[0];
            if (given && std::strcmp(text, "always") == 0)
                mode = static_cast<int>(CleanTopScreen::Always);
            else if (given && std::strcmp(text, "1") == 0)
                mode = static_cast<int>(CleanTopScreen::On);
            else if (given && std::strcmp(text, "0") != 0)
                given = false; // (not one of its values: the app's setting)
            g_cleanFromEnvironment.store(given); // (each first ask reads the same: whichever stores is right)
            g_cleanTopScreen.compare_exchange_strong(expected, mode);
        }

        GameCapture g_gameCapture = nullptr;
        std::mutex g_gameFilesMutex;
        std::string g_gameFiles;
        std::string g_gameFilesFallback;
        std::string g_servedLevelName;                               // setServedLevelFile's
        std::shared_ptr<const std::vector<uint8_t>> g_servedLevelFile;
    }

    void setGameCapture(GameCapture capture)
    {
        g_gameCapture = capture;
        for (std::atomic<int64_t> &given : g_pieceGivenAt)
            given.store(0); // (a new game: no piece given yet)
    }

    void setGameFiles(const std::string &folder, const std::string &fallback)
    {
        std::lock_guard<std::mutex> lock(g_gameFilesMutex);
        g_gameFiles = folder;
        g_gameFilesFallback = fallback;
    }

    std::string gameFiles()
    {
        std::lock_guard<std::mutex> lock(g_gameFilesMutex);
        return g_gameFiles;
    }

    std::string gameFilesFallback()
    {
        std::lock_guard<std::mutex> lock(g_gameFilesMutex);
        return g_gameFilesFallback;
    }

    void setServedLevelFile(const std::string &name, std::shared_ptr<const std::vector<uint8_t>> bytes)
    {
        std::lock_guard<std::mutex> lock(g_gameFilesMutex);
        g_servedLevelName = bytes ? name : std::string();
        g_servedLevelFile = name.empty() ? nullptr : std::move(bytes);
    }

    std::shared_ptr<const std::vector<uint8_t>> servedLevelFile(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(g_gameFilesMutex);
        return !name.empty() && name == g_servedLevelName ? g_servedLevelFile : nullptr;
    }

    void setCleanTopScreen(CleanTopScreen mode)
    {
        readEnvironment();
        if (!g_cleanFromEnvironment.load())
            g_cleanTopScreen.store(static_cast<int>(mode));
    }

    CleanTopScreen cleanTopScreen()
    {
        readEnvironment();
        return static_cast<CleanTopScreen>(g_cleanTopScreen.load(std::memory_order_relaxed));
    }

    void setBottomScreenShows(HudPieces pieces)
    {
        g_bottomPieces.store(pieces, std::memory_order_relaxed);
        g_bottomPaintedAt.store(nowMs(), std::memory_order_release);
    }

    bool bottomScreenUp()
    {
        const int64_t at = g_bottomPaintedAt.load(std::memory_order_acquire);
        return at != 0 && nowMs() - at <= kBottomScreenGoneMs;
    }

    bool hiddenOnTop(HudPiece piece)
    {
        switch (cleanTopScreen())
        {
        case CleanTopScreen::Off:
            return false;
        case CleanTopScreen::Always:
            return true;
        case CleanTopScreen::On:
            break;
        }
        if ((g_bottomPieces.load(std::memory_order_relaxed) & hudPiece(piece)) == 0 || !bottomScreenUp())
            return false;
        const int64_t given = g_pieceGivenAt[static_cast<size_t>(piece)].load(std::memory_order_relaxed);
        return given != 0 && (piece == HudPiece::TrickLine || nowMs() - given <= kPieceGoneMs);
    }

    void captureSnapshot(const uint8_t *guestRam, size_t ramBytes, Snapshot &out)
    {
        out.objectiveDoneMarks = 0; // (only BMX XXX's capture sets them)
        if (g_gameCapture)
        {
            g_gameCapture(guestRam, ramBytes, out);
            // What it gave the bottom screen (Clean Top Screen's hiddenOnTop).
            if (out.mode != GameMode::NoLevel)
            {
                const int64_t now = nowMs();
                auto given = [now](HudPiece piece) { g_pieceGivenAt[static_cast<size_t>(piece)].store(now, std::memory_order_relaxed); };
                if (out.timerSeconds >= 0.0f)
                    given(HudPiece::Clock);
                if (out.line[0] != 0 || out.pendingScore > 0.0f)
                    given(HudPiece::TrickLine);
                if (out.activeGoal >= 0)
                    given(HudPiece::Goal);
            }
            return;
        }
        // No game's capture: nothing is running.
        ++out.sequence;
        out.mode = GameMode::NoLevel;
        out.level = -1;
        out.levelFile[0] = 0;
        out.objectiveCount = 0;
        out.activeGoal = -1;
        out.goalStatus[0] = 0;
        out.goalTargetCount = 0;
        out.scriptWordCount = 0;
        out.scriptVariableBytes = 0;
        out.trackedCount = 0;
        out.collectableCount = 0;
    }
}
