#pragma once

// Ghost riding: a recorded run's rider drawn beside the live one, see-through, while the player rides the same level.
//
// A recording (runtime/play_recording.h) holds what the game read, not where its rider went. Its ghost track
// (<recording>.zghost, beside it) does: for every frame of play in a level, each skinned part the rider was drawn
// with, its node's world matrix and its bones, as the class renderer handed them to VU1. A track is made beside a
// recording as the recording is made, or as an older recording is played back (it then has none yet).
//
// The ghost is drawn with the live rider's own model. At each draw of a skinned part of the live rider, right after
// it in the GS stream (with that material's GS state, texture and lights as the live draw has them), the same part
// is drawn again with the ghost's bones and place, in the live camera's view, blended see-through
// (GSNativeDraw::seeThrough). Nothing of the ghost is in the game: it writes no RAM, it collides with nothing, no VU1
// work is added; only the GS draws it. A part the live rider does not draw (another model) has no ghost.
//
// Only the recorded rider's ghost is drawn: a run with another skater rides without one (the track names its
// rider, Stretch::rider).
//
// The clock: frames of play (the panel snapshot's GameMode::Play, runtime/panel_snapshot.h) since the level began,
// that is since its first frame of play after the game was out of a level. A pause stops it; leaving the level and
// coming back starts it again. The ghost rides the track's longest stretch of play in that level.
//
// What a boot does (runtime/boot_config.h names, the environment for a developer run):
//   ZX_GHOST=<file>         ride against this track, or this recording's (its .zghost)
//   ZX_GHOST_LEVEL=<level>  only in this level (the panel file's name, as the recordings list has it: "MOVIELOT");
//                           unset: in any level the track has
//   ZX_GHOST_TRACK=<file>   write this run's track there; "1": beside the recording being made, or beside the one
//                           being replayed when it has no track yet
// Off (none set), nothing is read, made or drawn.
//
// The game's override installs it with the game's rider (install): player one's model objects. The
// engine's skinned draws note the rider's parts and draw its ghost (engine/zge/host/class_draws.cpp, and Aggressive
// Inline's own native draws, aggressive_inline_native.cpp).

#include "gs/gs_native.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class PS2Machine;

namespace zx::ghost_rider
{
    // One skinned part of the rider as it was drawn in a frame.
    struct PartPose
    {
        // Which part: its vertex and bone counts (the live rider's draw of a part with the same counts takes it).
        uint32_t vertices = 0;
        uint32_t bones = 0;
        // Object -> world: rows X, Y and Z (the axes), then the position; the w column 0, 0, 0, 1.
        float world[4][4] = {};
        // Bone k's matrix in the model's space, as VU1 gets it (GSNativeSkinning::palette rows 4k .. 4k + 3, xyz):
        // its rotation's rows, then its translation (the pose's, less the bind position turned by the pose). 12
        // floats a bone.
        std::vector<float> palette;
    };
    // A frame's parts, in the order they were drawn.
    using Frame = std::vector<PartPose>;

    // One stretch of play in a level: frames[k] is the k-th frame of play since the level began (empty: the rider
    // was not drawn in it).
    struct Stretch
    {
        std::string level; // the panel file's name
        // Who rode it: the skater's name as the game has it (the panel snapshot's skaterName; "" a game that names
        // none). Only the same rider's ghost is drawn: the ghost is the live rider's model in the recorded poses.
        std::string rider;
        std::vector<Frame> frames;
    };
    struct Track
    {
        std::vector<Stretch> stretches;
    };

    // The track beside a recording: <recording>.zghost.
    std::filesystem::path trackOf(const std::filesystem::path &recording);
    // A track's file: written whole, or a stretch added at its end (a file not there yet gets its header first).
    bool writeTrack(const std::filesystem::path &file, const Track &track, std::string *error = nullptr);
    bool appendStretch(const std::filesystem::path &file, const Stretch &stretch, std::string *error = nullptr);
    // A track read back (a file cut short: its whole stretches). False for a file that is not a track.
    bool readTrack(const std::filesystem::path &file, Track &out, std::string *error = nullptr);
    // What a track holds, its frames not read (none: no track there).
    struct StretchSummary
    {
        std::string level, rider;
        uint32_t frames = 0;
    };
    std::vector<StretchSummary> trackStretches(const std::filesystem::path &file);
    // The stretch a ghost rides in `level`: the longest of that level (null: none).
    const Stretch *stretchFor(const Track &track, const std::string &level);

    // ---- a boot ---------------------------------------------------------------------------------------------

    // Player one's model objects this frame (the engine's scene objects: the rider's, a bike's), read from the game's
    // RAM: how many (0: none, outside a level).
    constexpr size_t kMaxRiderObjects = 4;
    using RiderObjects = size_t (*)(const uint8_t *rdram, uint32_t (&objects)[kMaxRiderObjects]);
    // The game's override, at its install: the clock (a VSync hook) and the track and ghost the boot asks for.
    // Nothing when none is asked for.
    void install(PS2Machine &runtime, RiderObjects rider);
    // A track is being made or a ghost ridden against this boot.
    bool active();
    // The rider's model objects this frame (count: how many).
    const uint32_t *riderObjects(size_t &count);

    // ---- at a skinned draw of the rider's (the EE thread) ---------------------------------------------------

    // The ghost's pose of a part, for one live draw of that part.
    struct Ghost
    {
        PartPose pose;
        // The live draw's lights are turned by its node's world (a node lit by its own lights; the scene's lights
        // are sent as they are): the ghost's are then turned by its own world instead.
        bool lightsTurnedByWorld = false;
    };
    // The rider's part `part` of node `node` drawn now, with `pose`: kept for the track being made, and the ghost's
    // pose of the same part now returned (null: no ghost, or none of this part now). Kept for ghostNoted too.
    std::shared_ptr<const Ghost> noteRiderPart(uint32_t node, uint32_t part, PartPose pose, bool lightsTurnedByWorld);
    // The ghost noteRiderPart returned for this node and part at its last draw this frame (a draw that reaches the
    // GS by another path than the engine's records: Aggressive Inline's native draws).
    std::shared_ptr<const Ghost> ghostNoted(uint32_t node, uint32_t part);

    // ---- the ghost's draw ------------------------------------------------------------------------------------

    // How solid the ghost is drawn (GSNativeDraw::seeThrough).
    constexpr int kSeeThrough = 0x50;
    // The ghost's draw of the part a live skinned draw draws (`live`: its skin, lights and run set): the same mesh
    // and lights, the ghost's bones, its place in the live view (`screen`: world -> GS window, the view's matrix
    // with its Z not yet times 16; `clip`: world -> the clip volume), see-through, frame interpolation's identity
    // its own.
    GSNativeDraw ghostDraw(const GSNativeDraw &live, const Ghost &ghost, const float screen[4][4],
                           const float clip[4][4]);
    // The ghost's draw of one run of the live draw (a material's triangles: its first, count and UVs).
    GSNativeDraw ofRun(const GSNativeDraw &ghost, const GSNativeDraw &liveRun);
}
