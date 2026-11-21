// The bottom screen's snapshot registry (runtime/panel_snapshot.h): the
// running game's capture and the folder of its panel files.
#include "runtime/panel_snapshot.h"

#include <mutex>
#include <string>

namespace panel
{
    namespace
    {
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

    void captureSnapshot(const uint8_t *guestRam, size_t ramBytes, Snapshot &out)
    {
        out.objectiveDoneMarks = 0; // (only BMX XXX's capture sets them)
        if (g_gameCapture)
        {
            g_gameCapture(guestRam, ramBytes, out);
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
