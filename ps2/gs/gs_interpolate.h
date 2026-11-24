#pragma once

// Frame interpolation (the Display setting Frame Interpolation): pictures
// between two of the game's frames, drawn on the render side only, so a
// display faster than the game's 60 frames a second shows motion at its own
// rate while the game's logic keeps its own.
//
// The GPU renderer keeps what it drew for the game's newest frame (each pass
// of draws, the fog and anti-aliasing steps, the picture's presentation) and
// the motion of the frame before (GSNativeMotion: each native draw's object
// and camera matrices). A picture at a point between the two is that frame
// drawn again into targets of its own, every native draw found in both
// frames moved to the point between its two places, the camera too; GS-path
// 3D primitives (particles, trails, shadow volumes) follow the camera; 2D,
// the fog and the previous-frame blend are as the newest frame has them. The
// game's RAM, its GS stream and its timing are untouched.
//
// This file: the arithmetic (matrices, the cut and match rules, the time of
// a display refresh in the game's frames), free of the renderer, for the
// renderer, the window loop and the unit tests. GPU_BACKEND.md "Frame
// interpolation" has the design.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace zx::gs_interpolate
{
    // Row vectors, as the games and GSNativeDraw: [x y z 1] * m.
    using Matrix = float[4][4];

    // ---- the setting ----------------------------------------------------------------------------------------

    // [display] frame_interpolation (ZX_INTERPOLATION=on|off), read by the window loop and the renderer.
    bool enabled();
    void setEnabled(bool on);

    // How a picture between two frames is made.
    enum class Method : uint32_t
    {
        // The newer frame's draws recorded again with every native draw and the camera moved (exact, but it costs
        // the GPU about what the frame did, and the game's own frame waits behind it).
        Replay,
        // The two finished pictures warped toward each other by the camera's motion at each pixel's depth, in one
        // pass: a fraction of a frame's GPU time (Android's: a game frame already takes most of a 60 Hz refresh).
        Warp,
    };
    // ZX_INTERP_METHOD=replay|warp; otherwise Warp on Android, Replay elsewhere.
    Method method();

    // ---- matrices -------------------------------------------------------------------------------------------

    // o = a x b.
    void multiply(const Matrix &a, const Matrix &b, Matrix &o);
    // The inverse of a general 4x4 matrix (projective too); false if singular.
    bool invert(const Matrix &m, Matrix &o);

    // An affine transform (column 3 = 0, 0, 0, 1) split for interpolating: its 3x3 part = rotation x stretch
    // (polar decomposition: the rotation the closest one to the part, the stretch symmetric), the rotation as a unit
    // quaternion, then the translation (row 3).
    struct Affine
    {
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
        float stretch[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        float translation[3] = {};
    };
    // False if the 3x3 part is singular (a node scaled to nothing) or column 3 is not 0, 0, 0, 1.
    bool decompose(const Matrix &m, Affine &out);
    void compose(const Affine &a, Matrix &out);
    // The transform at t between a (0) and b (1): the rotation by spherical interpolation (the shorter way), the
    // stretch and the translation linearly.
    Affine between(const Affine &a, const Affine &b, float t);
    // The same on matrices; false if either does not decompose.
    bool betweenAffine(const Matrix &a, const Matrix &b, float t, Matrix &out);
    // A camera's view matrix (world to camera, rigid) at t: the camera's own transform (its inverse) interpolated,
    // so its position moves on a straight line, then inverted again.
    bool betweenView(const Matrix &a, const Matrix &b, float t, Matrix &out);
    // Element by element (projections: their field of view and depth mapping).
    void lerp(const Matrix &a, const Matrix &b, float t, Matrix &out);

    // ---- what is not interpolated ---------------------------------------------------------------------------

    // The engine's own camera cut (zge/scene/camera.cpp cameraCut: the texture cache is flushed for it): the camera
    // moved more than kCutDistance or turned more than kCutTurnDegrees between two frames. Views given as world to
    // camera matrices. Here any turn past the angle counts (the engine compares |cos|).
    constexpr float kCutDistance = 7.0f;
    constexpr float kCutTurnCosine = 0.93969f; // 20 degrees
    bool cameraCut(const Matrix &viewA, const Matrix &viewB);
    // An object moved farther than the camera cut's distance in one frame (a respawn, a teleport): drawn where the
    // newer frame has it.
    bool jumped(const Matrix &worldA, const Matrix &worldB);

    // ---- GS-path 3D primitives --------------------------------------------------------------------------------

    // A GS-path vertex (GS window X, Y and Z, the context's offset not yet taken off) drawn with the camera
    // `fromCamera` (world to GS window space as a native draw's transform without its object: [x y z 1] * camera =
    // (X w, Y w, Z w, w)), moved to where `toCamera` draws the same world point. fromInverse = fromCamera's inverse.
    // wRatio: the old w over the new (a perspective texture's S, T, Q are scaled by it). False if the point is not
    // in front of either camera.
    bool reproject(const Matrix &fromInverse, const Matrix &fromCamera, const Matrix &toCamera, float xyz[3],
                   float &wRatio);

    // ---- when a refresh shows what ----------------------------------------------------------------------------

    // The game's frames are timed by the guest's vblanks (a frame made at vblank tick v), the window's refreshes by
    // the host clock. The picture a refresh shows is the game at `delay` before the refresh, in vblank ticks
    // (fractional): between the shown frame (tick a) and the next (tick b), at alpha = (tick - a) / (b - a).
    struct VblankClock
    {
        uint64_t tick = 0;    // the last vblank seen
        int64_t tickNs = 0;   // its host time
        int64_t periodNs = 16683333; // NTSC: 59.94 Hz
        // The fractional tick at host time ns.
        double tickAt(int64_t ns) const
        {
            return static_cast<double>(tick) + static_cast<double>(ns - tickNs) / static_cast<double>(periodNs);
        }
    };
    // EE scheduler, at each vblank (with interpolation on): its host deadline (paced). vblankClock: the last one and the
    // period between them (a running mean; the PS2's 59.94 Hz until known).
    void noteVblank(uint64_t tick, std::chrono::steady_clock::time_point at);
    VblankClock vblankClock();
    // Whether the window shows pictures between frames: only on a fixed-refresh display faster than the game (its
    // refresh `refreshNs` under 90 % of the game's frame `framePeriodNs`; 0: not known). With variable refresh in
    // effect (`variableRefresh`: the setting, with the frame queue) each game frame is shown when it is ready and
    // the display follows, which interpolation never improves on: it shows a frame one frame late and doubles
    // the renderer's work, so variable refresh wins.
    inline bool interpolates(bool variableRefresh, int64_t refreshNs, int64_t framePeriodNs)
    {
        return !variableRefresh && refreshNs > 0 && framePeriodNs > 0 && refreshNs * 10 < framePeriodNs * 9;
    }
    // How late the renderer makes the game's frames, for the delay of a refresh that is not phase-locked to the
    // vblanks (runtime/frame_interpolation.h): a refresh shows the game one frame plus this before it, so the next
    // frame is made when the display time reaches the one shown. made() takes each frame's lag (its picture made
    // that long after its vblank's host time: its real finish time); lagNs is a slow-falling maximum of them, at
    // most a quarter frame above the quickest of the last kRecent. Each frame waits in the frame queue about the
    // delay less its own lag, and a display time more than one and a half frames behind the newest frame has the
    // queue (two waiting) push frames out unshown, every other one while it lasts: with the quarter even the
    // quickest frame is at most a frame and a quarter behind when it is made, room for a window a refresh late. So
    // a steadily late renderer (frames 20 ms after their vblanks) gets a delay that waits for them, and one late
    // only now and then (a GPU-heavy view's frames, 1 to 70 ms) shows those as they are (not-ready) rather than
    // holding every frame back by the worst and losing frames out of the queue.
    struct FrameLag
    {
        static constexpr size_t kRecent = 16; // frames: a quarter second
        int64_t lagNs = 0;
        int64_t recent[kRecent]{};
        size_t count = 0; // frames seen (the last kRecent in recent)
        void made(int64_t frameLagNs, int64_t periodNs)
        {
            const int64_t lag = std::max<int64_t>(frameLagNs, 0);
            recent[count++ % kRecent] = lag;
            const int64_t quickest = *std::min_element(recent, recent + std::min(count, kRecent));
            lagNs = std::min(std::max(lag, lagNs - lagNs / 64), quickest + periodNs / 4);
        }
    };
    // alpha between frames made at ticks a < b for display tick `at` (0..1, clamped).
    inline float alphaBetween(uint64_t a, uint64_t b, double at)
    {
        if (b <= a)
            return 1.0f;
        const double t = (at - static_cast<double>(a)) / static_cast<double>(b - a);
        return static_cast<float>(t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t);
    }
    // Pictures this close to a frame are that frame (no replay).
    constexpr float kSnap = 0.02f;

    // ---- counts -----------------------------------------------------------------------------------------------

    // Why a refresh showed a game frame as it is instead of a picture between two (a run log line per second with
    // ZX_PERF_LOG, and at exit).
    enum class Skip : uint32_t
    {
        Exact,        // the refresh fell on a frame (alpha within kSnap of 0 or 1)
        Still,        // nothing moved between the two frames (menus, movies, pause)
        Cut,          // a camera cut
        Unreplayable, // the newer frame did something the replay does not repeat (a target loaded from memory, the
                      // CPU rasterizer, a texture rewritten while held)
        Budget,       // the picture between before the last is still on the GPU (one may be in flight)
        NotReady,     // the next frame was not made in time for the refresh
        NoPair,       // no frame before it (the first one, a frame queue reset)
        Unwarpable,   // the warp has nothing to go on: no depth of the scene, no single camera, or a picture made
                      // of two read circuits or one field
        kCount
    };
    const char *skipName(Skip s);
    // Per draw: drawn moved, drawn where the newer frame has it (no match in the frame before, a jump), or with
    // no identity at all.
    enum class Draw : uint32_t
    {
        Moved,
        Unmatched,
        Jumped,
        NoMotion,
        Reprojected, // GS-path 3D draws moved with the camera
        kCount
    };
    const char *drawName(Draw d);
}
