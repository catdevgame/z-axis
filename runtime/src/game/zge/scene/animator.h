#pragma once

// The engine's animator (animator.cpp): the animation table (every archive's
// motions by name hash) and an animator per animated scene object (its queue
// of tracks, this frame's pose layers, its mirror map), over the engine's
// animation player (animation.h).

#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct BoneMirror;
    struct Motion;
    struct PoseLayer;
    struct SceneObject;
    struct TrackRecord;
    struct Vec4;
    struct AnimationEvent;

    // A List of records: its nodes are the records' first words.
    template <typename Record>
    union RecordList
    {
        List list;
        struct
        {
            ee::Ptr<Record> first;
            ee::Ptr<Record> last;
            int32_t count;
        };
    };

    // ZGE4's track hooks (BMX XXX): what a track's or a pose layer's record
    // points at (setTrackRecord). The animator's callback for each step of
    // the animation player (onTrackBlend; ZGE4's runTrackHooks) calls onBlend
    // every step and onEnd when the blend has ended. ZGE3 keeps the hooks in
    // the animator's own code (blendHips, mirrorTrack) instead.
    // A track's blend hook: (the object, the track, the caller's context, the track's weight) each step.
    using BlendHook = ee::FunctionPtr<void(ee::Ptr<SceneObject> object, uint32_t track, uint32_t context, ee::Float weight)>;
    struct TrackHooks
    {
        // +0x00 each step (the context, the track's weight), 0: none (playAnimation's caller's)
        BlendHook onBlend;
        uint32_t blendContext; // +0x04
        uint32_t endContext;   // +0x08 (playAnimation: the track record)
        // +0x0C once the blend has ended (weight -1), 0: none (playAnimation: freeEndedTrack)
        ee::FunctionPtr<void(ee::Ptr<SceneObject> object, uint32_t track, uint32_t context)> onEnd;
    };
    static_assert(sizeof(TrackHooks) == 16 && offsetof(TrackHooks, onEnd) == 0x0C);

    // An animated object's animator record (from the animator pool;
    // playAnimation makes it): in the animators' tree by its object.
    // (zge::Animator, animation.h, is the animation player's own.) BMX
    // XXX's animator mirrors nothing: its record has no mirror map, so the
    // fields after it sit a word lower and the record is a word shorter.
    struct AnimatorRecord
    {
        SearchTreeNode node; // +0x00 the tree's links
        ee::Ptr<SceneObject> object; // +0x08 the key
        ee::Float time;         // +0x0C seconds into the animation
        RecordList<TrackRecord> tracks; // +0x10 the last is the newest; none: the animator is freed
        RecordList<PoseLayer> layers;       // +0x1C this frame's pose layers
#if !PS2X_GAME_BMX
        ee::Ptr<BoneMirror> mirror; // +0x28 the bone mirror map (made for its first mirrored animation)
        ee::Ptr<BoneMirror> mirrorMap() const { return mirror; }
        void setMirrorMap(ee::Ptr<BoneMirror> map) { mirror = map; }
#else
        // BMX XXX's animator keeps no mirror map: none is ever made or freed.
        ee::Ptr<BoneMirror> mirrorMap() const { return {}; }
        void setMirrorMap(ee::Ptr<BoneMirror>) {}
#endif
        uint32_t paused;        // +0x2C (BMX +0x28)
        uint32_t offScreen;     // +0x30 (BMX +0x2C) animated when not drawn too
    };
    // (DM2's engine has no animator: its instance keeps AI's shape.)
    static_assert(sizeof(AnimatorRecord) == game::readable::perGame(52u, 52u, 48u) &&
                  offsetof(AnimatorRecord, object) == 0x08 && offsetof(AnimatorRecord, tracks) == 0x10 &&
                  offsetof(AnimatorRecord, layers) == 0x1C);
    static_assert(offsetof(AnimatorRecord, paused) == game::readable::perGame(0x2Cu, 0x2Cu, 0x28u) &&
                  offsetof(AnimatorRecord, offScreen) == game::readable::perGame(0x30u, 0x30u, 0x2Cu));

    // A track an animator plays (from the track pool; playAnimation makes
    // it): in its animator's queue, the engine's track's record (zge's
    // setTrackRecord; ZGE4's: its hooks). ZGE3's is 36 bytes with its
    // mirror hook, owner and root motion; ZGE4's 40, with the motion
    // straight after the blend time and the track hooks in their place (the
    // other version's fields stand past the end, so the shared code
    // compiles: kTrackRecordBytes is the record's size).
    struct TrackRecord
    {
        ee::Ptr<TrackRecord> next;     // +0x00 the queue's links
        ee::Ptr<TrackRecord> previous; // +0x04
        uint32_t loops;      // +0x08 started again at its end (playAnimation's mode bit 0)
        ee::Float blendTime; // +0x0C seconds it blends in over (0: started at once)
#if !PS2X_GAME_BMX
        uint32_t mirrorHook; // +0x10 mirrorTrack when it plays mirrored (mode bit 3), else 0
        ee::Ptr<AnimatorRecord> owner; // +0x14
        ee::Float rootMotion; // +0x18 the Hips bone's translation scaled by it (blendHips; 0: none)
        ee::Ptr<Motion> animation; // +0x1C the motion
        uint32_t track;      // +0x20 the engine's track
        TrackHooks hooks;    // (ZGE4's; past the end)
#else
        ee::Ptr<Motion> animation; // BMX +0x10 the motion
        TrackHooks hooks;    // BMX +0x14 its record: the caller's hook and freeEndedTrack
        uint32_t track;      // BMX +0x24 the engine's track
        uint32_t mirrorHook; // (ZGE3's; past the end)
        ee::Ptr<AnimatorRecord> owner;
        ee::Float rootMotion;
#endif
    };
    constexpr uint32_t kTrackRecordBytes = game::readable::perGame(36u, 36u, 40u);
    static_assert(offsetof(TrackRecord, loops) == 0x08 && offsetof(TrackRecord, blendTime) == 0x0C);
    static_assert(offsetof(TrackRecord, animation) == game::readable::perGame(0x1Cu, 0x1Cu, 0x10u) &&
                  offsetof(TrackRecord, track) == game::readable::perGame(0x20u, 0x20u, 0x24u) &&
                  offsetof(TrackRecord, hooks) == game::readable::perGame(0x24u, 0x24u, 0x14u) &&
                  offsetof(TrackRecord, mirrorHook) == game::readable::perGame(0x10u, 0x10u, 0x28u) &&
                  offsetof(TrackRecord, rootMotion) == game::readable::perGame(0x18u, 0x18u, 0x30u));

    // A pose layer (from the layer pool; addPoseLayer): an animation's pose
    // at a time blended over the tracks for one frame. ZGE3's is 28 bytes
    // with its mirror hook and owner; ZGE4's 36, the track hooks in their
    // place (the other version's fields past the end: kPoseLayerBytes).
    struct PoseLayer
    {
        ee::Ptr<PoseLayer> next;     // +0x00 the list's links
        ee::Ptr<PoseLayer> previous; // +0x04
        ee::Ptr<Motion> animation; // +0x08
        ee::Float time;      // +0x0C within the animation's length
        ee::Float weight;    // +0x10 0 .. 1
#if !PS2X_GAME_BMX
        uint32_t mirrorHook; // +0x14 mirrorLayer when mirrored, else 0
        ee::Ptr<AnimatorRecord> owner; // +0x18
        TrackHooks hooks;    // (ZGE4's; past the end)
#else
        TrackHooks hooks;    // BMX +0x14 its record while it is blended in (updateAnimator)
        uint32_t mirrorHook; // (ZGE3's; past the end)
        ee::Ptr<AnimatorRecord> owner;
#endif
    };
    constexpr uint32_t kPoseLayerBytes = game::readable::perGame(28u, 28u, 36u);
    static_assert(offsetof(PoseLayer, animation) == 0x08 && offsetof(PoseLayer, weight) == 0x10);
    static_assert(offsetof(PoseLayer, mirrorHook) == game::readable::perGame(0x14u, 0x14u, 0x24u) &&
                  offsetof(PoseLayer, owner) == game::readable::perGame(0x18u, 0x18u, 0x28u) &&
                  offsetof(PoseLayer, hooks) == game::readable::perGame(0x1Cu, 0x1Cu, 0x14u));

    // ZGE4's animation library (BMX XXX: 16 of them, each archive loaded
    // in the first one free, numbered from 1): its motions by name hash, its
    // entries' pool (0: the library is free). ZGE3 keeps one animation table
    // for every archive.
    struct AnimationLibrary
    {
        SearchTree table;    // +0x00 AnimationEntry by name hash
        uint32_t entryPool;  // +0x0C the table's entries (one an asset of the archive), 0: free
    };
    static_assert(sizeof(AnimationLibrary) == 16 && offsetof(AnimationLibrary, entryPool) == 0x0C);
    constexpr uint32_t kAnimationLibraries = 16;
    // ZGE4's (BMX XXX) animation libraries: an archive's motions each, in place of the one table.
    struct AnimationLibraries
    {
        AnimationLibrary library[kAnimationLibraries];
    };

    // What an archive's files load into (forEachLibraryEntryOf, loadFormFile's context): the table, and the
    // archive's number (ZGE4: its library's).
    struct ArchiveLoad
    {
        ee::Ptr<SearchTree> table;
        uint32_t archive;
    };
    static_assert(sizeof(ArchiveLoad) == 8);

    // ZGE4's animation event handler (BMX XXX, 0x54 bytes): the handler of
    // an event kind by its name's hash, in the event handlers' tree; the
    // animator's event hook (onAnimationEvent) calls it.
    struct AnimationEventHandler
    {
        SearchTreeNode node; // +0x00 the tree's links
        uint32_t kind;       // +0x08 the key: the name's hash (an event's kind, eventKind)
        char name[0x40];     // +0x0C
        // +0x4C (object, event, context, the event's time)
        ee::FunctionPtr<void(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event, uint32_t context, ee::Float time)> handler;
        uint32_t context;    // +0x50
    };
    static_assert(sizeof(AnimationEventHandler) == 0x54 && offsetof(AnimationEventHandler, handler) == 0x4C);

    // An animation table entry (20 bytes): a motion by its name.
    struct AnimationEntry
    {
        SearchTreeNode node;    // +0x00 the tree's links
        uint32_t name;          // +0x08 the key: the motion's name hash
        ee::Ptr<Motion> motion; // +0x0C
        uint32_t archive;       // +0x10 the archive it came from
    };
    static_assert(sizeof(AnimationEntry) == 20 && offsetof(AnimationEntry, motion) == 0x0C);

    // (animator.cpp)
    uint32_t addPoseLayer(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t flags, ee::Float time, ee::Float weight);
    int32_t animationPaused(ee::Ptr<SceneObject> node, ee::Ptr<uint32_t> pausedOut);
    int32_t animationTime(ee::Ptr<SceneObject> node, ee::Ptr<ee::Float> timeOut);
    void animatorBoneOffset(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out);
    int32_t findAnimation(ee::Address name, ee::Ptr<ee::Ptr<Motion>> motionOut);
    uint32_t findAnimationByHash(uint32_t hash, ee::Ptr<ee::Ptr<Motion>> motionOut);
    // ZGE4's findAnimationByHash (BMX XXX's own body, findAnimationByHash's _OWN): the motion of that name hash in
    // the first animation library in use that has it, or 0.
    ee::Ptr<Motion> findLibraryAnimation(uint32_t hash);
    // ZGE4's findAnimation and playAnimation (BMX XXX's own bodies, their _OWN): the motion of that name in the
    // animation libraries in use, or 0; and a play with the caller's blend hook and its context (TrackHooks).
    ee::Ptr<Motion> findAnimationInLibraries(ee::Address name);
    uint32_t playAnimationWithHooks(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t mode, BlendHook onBlend,
                                    ee::Address blendContext, ee::Float blend);
    uint32_t initAnimators();
    int32_t loadAnimationArchive(ee::Address path);
    void freeAnimationLibrary(uint32_t number); // ZGE4 only: what loadAnimationArchive loaded into that library, freed
    int32_t animationOver(ee::Ptr<SceneObject> node, ee::Ptr<int32_t> doneOut);
    int32_t pauseAnimation(ee::Ptr<SceneObject> node);
    uint32_t playAnimation(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t mode, ee::Float blend, ee::Float rootMotion);
    uint32_t playAnimationNamed(ee::Ptr<SceneObject> node, uint32_t name, uint32_t mode, ee::Float blend, ee::Float rootMotion);
    int32_t resumeAnimation(ee::Ptr<SceneObject> node);
    int32_t setAnimationTime(ee::Ptr<SceneObject> node, ee::Float time);
    void shutdownAnimators();
    // (ZGE4 only) the handler of the events of a name, with the word it is called with.
    void registerAnimationEventHandler(ee::Ptr<const char> name, uint32_t handler, uint32_t context);
    // (ZGE4 only) whether the object has an animator; its animator freed; its one track run to its end; the mode
    // of its newest track (always 0: a bug kept) and that mode set (playAnimation's mode bits 0 and 1).
    uint32_t objectHasAnimator(ee::Ptr<SceneObject> object);
    uint32_t freeAnimatorOf(ee::Ptr<SceneObject> object);
    uint32_t finishAnimation(ee::Ptr<SceneObject> object);
    uint32_t animationMode(ee::Ptr<SceneObject> object);
    uint32_t setAnimationMode(ee::Ptr<SceneObject> object, uint32_t mode);
    // (ZGE4 only) a motion read from an asset library's entry by its id; the length of the track the object
    // started last (0 without an animator); and its animation time moved on by a random share of that.
    ee::Ptr<Motion> loadLibraryMotion(uint32_t library, uint32_t id);
    ee::Float animationLength(ee::Ptr<SceneObject> object);
    void randomizeAnimationTime(ee::Ptr<SceneObject> object);
    // playAnimation's (and setAnimationMode's) mode: the track starts again at its end; the object is animated
    // only while it is on screen (without it, off screen too).
    constexpr uint32_t kAnimationLooped = 1u << 0;
    constexpr uint32_t kAnimationOnScreenOnly = 1u << 1;
    int32_t stopAnimation(ee::Ptr<SceneObject> node);
    int32_t updateAnimators(ee::Float seconds);
}
