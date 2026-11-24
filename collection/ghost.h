#pragma once

// Ghost riding in the app (runtime/ghost_rider.h): ride against one of your own recordings.
//
// Aggressive Inline only (1.4): DM2's and BMX XXX's riders are noted and drawn by the same code (their overrides'
// rider objects), but the app neither makes their tracks nor offers their ghosts until a picture shows their ghost
// drawn, bike included (offersGhosts).
//
// The ghost is the live rider's model in the recorded rider's poses: it is drawn only when the player rides the
// recording's skater, and its row says which.
//
// Every recording the app makes (in a game that offers ghosts) gets its ghost track beside it (Record Play on: the rider's parts as they were
// drawn), and a recording played back that has none gets one as it plays. On the Recordings page a recording with a
// track has a row under it, Ride Against: the game boots as ever, the player rides live, and in the recording's
// level (the one it played longest, as its row names it) the recorded rider rides beside them, see-through, from
// the level's first frame of play. In other levels there is no ghost. Off unless chosen; the ghost never touches the
// game (nothing of it is in the game's memory).
//
// Plain C++: the app makes the rows and the boot; this file says what the boot is given.

#include "collection/recordings.h"
#include "runtime/boot_config.h"

#include <optional>
#include <string>

namespace zx::collection
{
    // A recording ridden against: its file and the level its ghost rides in.
    struct GhostChoice
    {
        std::string game; // the collection's id
        std::filesystem::path recording;
        std::string level;
    };
    // Whether the app makes ghost tracks and offers Ride Against in this game (the collection's id).
    bool offersGhosts(const std::string &game);
    // Whether a recording can be ridden against: its game offers ghosts, its level is noted and its track has a
    // stretch of play there.
    bool canRideAgainst(const RecordingEntry &entry);
    // Who rode the recording in its level (the stretch its ghost rides): the skater's name as the game has it, ""
    // when the track names none.
    std::string ghostRider(const RecordingEntry &entry);
    // The choice of the row's Ride Against (the app's thread), for the boot that follows; takeGhostChoice hands it
    // to that boot once.
    void chooseGhost(const RecordingEntry &entry);
    std::optional<GhostChoice> takeGhostChoice();

    // What a boot of `game` is given for ghost riding, in a game that offers ghosts: a track made beside a recording
    // being made (ZX_RECORD set) or played back (`playback`: made when it has none), and the ghost of `choice` when
    // it is this game's and the boot is not a playback.
    void ghostBootConfig(zx::BootConfig &config, const std::string &game, bool playback,
                         const std::optional<GhostChoice> &choice);
}
