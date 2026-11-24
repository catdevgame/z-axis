// Ghost riding in the app (collection/ghost.h).
#include "collection/ghost.h"

#include "runtime/ghost_rider.h"

#include <mutex>
#include <utility>

namespace zx::collection
{
    namespace
    {
        std::mutex g_mutex;
        std::optional<GhostChoice> g_choice;
    }

    bool offersGhosts(const std::string &game) { return game == "ai"; }

    namespace
    {
        // The stretch of the recording's track its ghost rides: the longest in its level (none: no track, or none
        // there).
        std::optional<zx::ghost_rider::StretchSummary> riddenStretch(const RecordingEntry &entry)
        {
            std::optional<zx::ghost_rider::StretchSummary> best;
            if (!entry.levelsNoted || entry.level.empty())
                return best;
            for (zx::ghost_rider::StretchSummary &stretch : zx::ghost_rider::trackStretches(zx::ghost_rider::trackOf(entry.file)))
                if (stretch.level == entry.level && stretch.frames > 0u && (!best || stretch.frames > best->frames))
                    best = std::move(stretch);
            return best;
        }
    }

    bool canRideAgainst(const RecordingEntry &entry)
    {
        return offersGhosts(entry.game) && riddenStretch(entry).has_value();
    }

    std::string ghostRider(const RecordingEntry &entry)
    {
        const std::optional<zx::ghost_rider::StretchSummary> stretch = riddenStretch(entry);
        return stretch ? stretch->rider : std::string();
    }

    void chooseGhost(const RecordingEntry &entry)
    {
        std::lock_guard lock(g_mutex);
        g_choice = GhostChoice{entry.game, entry.file, entry.level};
    }

    std::optional<GhostChoice> takeGhostChoice()
    {
        std::lock_guard lock(g_mutex);
        return std::exchange(g_choice, std::nullopt);
    }

    void ghostBootConfig(zx::BootConfig &config, const std::string &game, bool playback,
                         const std::optional<GhostChoice> &choice)
    {
        if (!offersGhosts(game))
            return;
        // (a playback's ZX_RECORD is off; its track goes beside the recording played, when it has none)
        if (playback || !config.text("ZX_RECORD").empty())
            config.set("ZX_GHOST_TRACK", "1");
        if (playback || !choice || choice->game != game)
            return;
        config.set("ZX_GHOST", choice->recording.string());
        config.set("ZX_GHOST_LEVEL", choice->level);
    }
}
