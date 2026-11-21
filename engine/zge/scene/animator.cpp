// The engine's animator (BMX XXX's engine has it as zanimUtil; AI's game
// carried its own copy): an AnimatorRecord for every animated object (in a
// tree by its object) with its tracks and this frame's pose layers over the
// engine's animation player (startAnimation ..); the animation table (each
// loaded archive's motions by name hash); playing, layering, pausing and
// timing an object's animation by its node; the frame's update (the tracks
// advanced with the animator's callback for each: the hips' root motion, a
// mirrored track's pose swapped left for right, a track whose blend ended
// freed; the layers blended in; the next track started when one ends). Its
// records come from the game's pools (zge/game.h). Names are inferred from
// what the code does.

#include "animator.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/debug/profiler.h"
#include "zge/game.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/memory/heaps.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/animator.cpp.h"

namespace zge
{
    namespace
    {
        // The track records' pool.
        inline ee::Global<uint32_t, sym::g_trackPool2> g_trackPool;
        // "Hips", hashed once.
        constexpr uint32_t kHipsName = sym::hipsName;
        inline ee::Global<uint32_t, sym::g_hashHips> g_hashHips;
        inline ee::Global<uint32_t, sym::g_hipsHash2> g_hipsHash;

        constexpr uint32_t kFormChunk = 0x4D524F46u; // "FORM"

        // The weight a track's blend has when it ends.
        constexpr float kBlendEnded = -1.0f;

        // ZGE4 runs an animator whose object is not drawn while its time is
        // still within this of 0 (a new animation's first pose is set even
        // off screen).
        constexpr float kFirstPoseTime = 1.0e-5f;

    }

    // (TrackRecord, PoseLayer: animator.h, with ZGE4's layouts beside them.)

    namespace
    {
        // (pool, block) back (a thunk to poolFree).

        // A record whose blend ended, out of its owner's list (if in it) and
        // its track's record cleared; it goes back to the pool after this.
        void releaseRecord(ee::Ptr<SceneObject> object, ee::Ptr<TrackRecord> record)
        {
            if (record->next || record->previous)
            {
                listRemove(&record->owner->tracks.list, record);
                record->next = {};
                record->previous = {};
            }
            setTrackRecord(object, record->track, 0u);
        }
    }

    // blendHips: the hips' root motion of a track: the Hips bone's
    // translation scaled by the track's weight (a weight above 0); a track
    // whose blend ended (-1) is freed.
    void blendHips(ee::Ptr<SceneObject> object, uint32_t track, ee::Float weight)
    {
        ee::Local<Vec4> translation;
        const ee::Ptr<TrackRecord> record(trackRecord(object, track)); // the track's game record: a word in zge's track
        if (*g_hashHips)
        {
            *g_hipsHash = nameHash(ee::Ptr<const char>(kHipsName));
            *g_hashHips = 0;
        }
        if (!record)
            return;
        if (weight == ee::Float(kBlendEnded))
        {
            releaseRecord(object, record);
            giveRecord(*g_trackPool, record);
            return;
        }
        if (!(ee::Float(0.0f) < record->rootMotion))
            return;
        const ee::Ptr<AnimationChannel> bone = findBoneChannel(object->animator, *g_hipsHash);
        const ee::Ptr<Vec4> motion = boneTranslation(bone);
        if (motion)
        {
            *translation = *motion * record->rootMotion;
            setBoneTranslation(bone, translation);
        }
    }
    READABLE_FUNCTION(blendHips, Verified);

    namespace
    {
        // (mirror map): the pose's paired bones swapped, left for right; what
        // zge's mirrorPose (void) leaves in the result is this hook's result.
        uint32_t mirrorPoseKeepingResult(ee::Address mirror) { return ee::tailCall<uint32_t>(sym::mirrorPose, mirror); }
    }

    // mirrorTrack (a mirrored track's hook): the pose mirrored with its
    // animator's mirror map, or the track freed when its blend ended.
    // Returns 0 when it is not mirrored.
    uint32_t mirrorTrack(ee::Ptr<SceneObject> object, uint32_t track, ee::Float weight)
    {
        const ee::Ptr<TrackRecord> record(trackRecord(object, track)); // the track's game record: a word in zge's track
        if (!record || !record->mirrorHook)
            return 0;
        const ee::Ptr<AnimatorRecord> owner = record->owner;
        if (!owner)
            return 0;
        const ee::Ptr<BoneMirror> mirror = owner->mirrorMap();
        if (!mirror)
            return 0;
        if (!(weight == ee::Float(kBlendEnded)))
            return mirrorPoseKeepingResult(mirror);
        releaseRecord(object, record);
        return game::tail(poolGive, *g_trackPool, record);
    }
    READABLE_FUNCTION(mirrorTrack, Verified);

    // onTrackBlend: the game's callback for each track the animation
    // player advances (object, track, its weight; -1: its blend ended).
    uint32_t onTrackBlend(ee::Ptr<SceneObject> object, uint32_t track, ee::Float weight)
    {
        blendHips(object, track, weight);
        return game::tail(mirrorTrack, object, track, weight);
    }

    // runTrackHooks: ZGE4's onTrackBlend: the track's hooks (its record, a
    // TrackHooks) run: onBlend each step, and onEnd once its blend has ended,
    // the record then cleared. (A track with no record whose blend ended has
    // its hooks read at address 0: kept.)
    void runTrackHooks(ee::Ptr<SceneObject> object, uint32_t track, ee::Float weight)
    {
        const ee::Ptr<TrackHooks> hooks(trackRecord(object, track));
        if (hooks && hooks->onBlend)
            hooks->onBlend(object, track, hooks->blendContext, weight);
        if (!(weight == ee::Float(kBlendEnded)) || !hooks->onEnd)
            return;
        hooks->onEnd(object, track, hooks->endContext);
        game::tail(setTrackRecord, object, track, 0u);
    }
    READABLE_FUNCTION_OWN(onTrackBlend, Verified, bmx, runTrackHooks);

    int32_t freeQueue(ee::Ptr<RecordList<TrackRecord>> queue);  // (below)
    int32_t freeLayers(ee::Ptr<RecordList<PoseLayer>> layers); // (below)
    int32_t freeAnimatorTracks(ee::Ptr<AnimatorRecord> animator); // (below: ZGE4's freeQueue)
    int32_t freeAnimatorLayers(ee::Ptr<AnimatorRecord> animator); // (below: ZGE4's freeLayers)

    // updateAnimator (animator, float *seconds; the animators' tree visit,
    // updateAnimators'): an animator's frame (on screen only, unless set to
    // run off screen too; not while paused): the tracks advanced by the
    // animation player (onTrackBlend for each), the pose layers blended in
    // and let go, and at the animation's end the newest track started again
    // when it loops (time back to 0) or the tracks let go; then the time
    // moves on by the frame's. Returns 1. (ZGE4: no mirror; each layer's
    // hooks and each track's are its record, run by onTrackBlend.)
    namespace
    {
        // zge's blendLayer: its scaleWords quadword is what the original's
        // call leaves in that register, no value of the caller's, so the call
        // keeps the address form and names the other words only.
        void blendPoseLayer(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, uint32_t hook, ee::Ptr<BoneMirror> mirror, ee::Float time, ee::Float weight)
        {
            ee::game(sym::blendLayer, object, motion, hook, mirror, time, weight);
        }
        // ZGE4's: the layer's own hooks are its pass's record, which the
        // animator's onTrackBlend runs.
        void blendPoseLayerWithHooks(ee::Ptr<SceneObject> object, ee::Ptr<Motion> motion, ee::Ptr<TrackHooks> hooks, ee::Float time, ee::Float weight)
        {
            ee::game(sym::blendLayer, object, motion, sym::onTrackBlend, hooks, time, weight);
        }
    }

    uint32_t updateAnimator(ee::Ptr<AnimatorRecord> self, ee::Ptr<ee::Float> seconds)
    {
        AnimatorRecord &animator = *self;
        if (animator.paused != 0)
            return 1;
        if (animator.offScreen == 0 && nodeNotDrawn(objectLevelNode(animator.object)) != 0)
        {
            if constexpr (game::readable::kIsBmx) // ZGE4 sets the first pose (a time of about 0) even when not drawn
            {
                if (!(ee::abs(animator.time) < ee::Float(kFirstPoseTime)))
                    return 1;
            }
            else
                return 1;
        }
        if (animator.tracks.count <= 0)
            return 1;

        const ee::Float frameTime = *seconds;
        updateAnimation(animator.object, sym::onTrackBlend, animator.time);
        if (animator.layers.count > 0)
        {
            for (ee::Ptr<PoseLayer> layer = animator.layers.first; layer; layer = layer->next)
            {
                if constexpr (game::readable::kIsBmx) // ZGE4: the layer's hooks, no mirror
                    blendPoseLayerWithHooks(animator.object, layer->animation, &layer->hooks, layer->time, layer->weight);
                else
                    blendPoseLayer(animator.object, layer->animation, layer->mirrorHook, animator.mirrorMap(), layer->time, layer->weight);
            }
            if constexpr (game::readable::kIsBmx) // ZGE4's freeLayers takes the animator
                freeAnimatorLayers(self);
            else
                freeLayers(&animator.layers);
        }
        if (animationDone(animator.object, animator.time) != 0)
        {
            const ee::Ptr<TrackRecord> newest = animator.tracks.last;
            if (newest)
            {
                if (newest->loops != 0)
                {
                    stopTracks(animator.object);
                    animator.time = 0.0f;
                    if constexpr (game::readable::kIsBmx) // ZGE4 keeps neither the new track nor its record (its hooks)
                        startAnimation(animator.object, newest->animation, animator.time);
                    else
                    {
                        newest->track = startAnimation(animator.object, newest->animation, animator.time);
                        setTrackRecord(animator.object, newest->track, newest);
                    }
                }
                else
                {
                    if constexpr (game::readable::kIsBmx) // ZGE4's freeQueue takes the animator
                        freeAnimatorTracks(self);
                    else
                        freeQueue(&animator.tracks);
                    animator.paused = 0;
                }
            }
        }
        animator.time = animator.time + frameTime;
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateAnimator, Verified);
}

namespace zge
{
    namespace
    {
        // The animation table (the motions of every archive loaded, by name
        // hash) and the animators (by object), SearchTrees.
        constexpr uint32_t kAnimationTable = sym::animationTable;
        constexpr uint32_t kAnimators = sym::animators;
        // The pools: the table's entries (20 bytes, 256), the tracks (36,
        // 45), the pose layers (28, 3), the animators (52, 20).
        inline ee::Global<uint32_t, sym::g_animationEntryPool> g_animationEntryPool;
        inline ee::Global<uint32_t, sym::g_layerPool> g_layerPool;
        inline ee::Global<uint32_t, sym::g_animatorPool> g_animatorPool;
        inline ee::Global<int32_t, sym::g_archiveCount> g_archiveCount; // the next archive's number
        // Names hashed when first needed.
        constexpr uint32_t kNpcName = sym::npcName;        // "NPC": the level objects animated on screen only
        inline ee::Global<uint32_t, sym::g_npcHashed> g_npcHashed;
        inline ee::Global<uint32_t, sym::g_npcHash> g_npcHash;
        // The bone pairs a mirrored pose swaps (12 pairs of names, the
        // last 0: the spine's bones with themselves, each left one with
        // its right).
        struct MirrorPairs
        {
            BoneNamePair pairs[12];
        };
        static_assert(sizeof(MirrorPairs) == 0x60);
        inline ee::Global<MirrorPairs, sym::mirrorPairs> g_mirrorPairs;
        // The message an object's animator is freed on (its type 1).
        constexpr uint32_t kAnimatorGoneMessage = 1;

        // playAnimation's mode.
        constexpr uint32_t kModeLoops = 1u << 0;
        constexpr uint32_t kModeOnScreenOnly = 1u << 1;
        constexpr uint32_t kModeOffScreenToo = 1u << 2;
        constexpr uint32_t kModeMirrored = 1u << 3;

        inline ee::Global<SearchTree, sym::animationTable> g_animationTable;
        inline ee::Global<SearchTree, sym::animators> g_animators;
        ee::Ptr<SearchTree> animationTable() { return g_animationTable.get(); }
        ee::Ptr<SearchTree> animators() { return g_animators.get(); }

        // ZGE4's (BMX XXX): the animation libraries (AnimationLibraries), the animation event handlers by
        // kind, and the animators' profile bar ("zanimUtil": its update timed).
        inline ee::Global<AnimationLibraries, sym::bmx::g_animationLibraries> g_animationLibraries;
        inline ee::Global<SearchTree, sym::bmx::g_animationEventHandlers> g_animationEventHandlers;
        // Their records' pool (registerAnimationEventHandler makes it).
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_animationEventHandlerPool> g_animationEventHandlerPool;
        ee::Ptr<SearchTree> animationEventHandlers() { return g_animationEventHandlers.get(); }
        inline ee::Global<uint32_t, sym::bmx::g_animatorProfileBar> g_animatorProfileBar;
        // The profile bar's colour (ProfileBar::colour).
        struct BarColour
        {
            uint8_t red, green, blue, alpha;
        };

        ee::Ptr<AnimatorRecord> findAnimator(ee::Address key)
        {
            return searchTreeEntry<AnimatorRecord>(searchTreeFind(animators(), key));
        }
    }

    // keepMotion (motion, out; the motion loader's hook,
    // loadArchiveAnimations sets it): the motion read kept in *out.
    // Returns 1.
    uint32_t keepMotion(ee::Ptr<Motion> motion, ee::Ptr<ee::Ptr<Motion>> out)
    {
        *out = motion;
        return 1;
    }
    READABLE_FUNCTION(keepMotion, Verified);

    // freeAnimationEntry (entry, the table; a visit): an animation table entry's
    // motion freed, the entry out of the table and back to its pool.
    // Returns 1 (go on).
    uint32_t freeAnimationEntry(ee::Ptr<AnimationEntry> entry, ee::Ptr<SearchTree> table)
    {
        freeMotion(entry->motion);
        searchTreeRemove(table, &entry->name);
        giveRecord(*g_animationEntryPool, entry);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeAnimationEntry, Verified);

    // forEachLibraryEntryOf: every asset of an archive through loadFormFile.
    uint32_t forEachLibraryEntryOf(uint32_t library, ee::Ptr<SearchTree> table, uint32_t archive)
    {
        ee::Local<ArchiveLoad> load;
        memoryFill(load, 0, sizeof(ArchiveLoad));
        load->table = table;
        load->archive = archive;
        forEachAsset(library, sym::loadFormFile, load);
        return 1;
    }
    READABLE_FUNCTION_WAITS(forEachLibraryEntryOf, Verified);

    namespace
    {
        // A record from a pool (an untyped block: the pool's record of its kind).
        template <typename T> ee::Ptr<T> takeFromPool(uint32_t pool) { return ee::Ptr<T>(takeRecord(pool)); }
    }

    // loadFormFile (library, name, bytes, the table and archive): a "FORM"
    // file in a buffer of the asset's alignment, read, its motion parsed
    // into a new table entry.
    uint32_t loadFormFile(uint32_t library, uint32_t name, uint32_t bytes, ee::Ptr<ArchiveLoad> load)
    {
        const uint64_t alignment = currentAssetAlignment(library);
        const uint32_t align = static_cast<uint32_t>(alignment);
        const uint32_t buffer = heapAllocate((bytes - 1 + align) / align * align);
        readAsset(library, buffer, 0);
        const ee::Ptr<AnimationEntry> entry = takeFromPool<AnimationEntry>(*g_animationEntryPool);
        entry->name = name;
        entry->archive = load->archive;
        const ee::Ptr<ZFile> file = openMemoryFile(buffer, bytes);
        const ee::Ptr<ChunkReader> reader(openChunkFile(file, kFormChunk, 0));
        setMotionLoadedHook(sym::keepMotion, &entry->motion);
        parseChunks(reader, 0);
        closeChunkFile(reader);
        closeFile(file, 0);
        if (buffer)
            heapFree(buffer);
        return game::tail(searchTreeInsert, load->table, &entry->node);
    }

    // animationLibrary (ZGE4 only: number 1 ..): the animation library of
    // that number (unchecked: 0 gives the 16 bytes before the first).
    inline ee::Ptr<AnimationLibrary> animationLibrary(uint32_t number)
    {
        return &g_animationLibraries->library[0] + (number - 1);
    }
    READABLE_FUNCTION_ONLY(bmx, animationLibrary);

    // freeAnimationLibrary (ZGE4 only): everything loadAnimationArchive put in the library of that number given
    // back: each entry's motion freed and the entry taken out of the table, the table made empty again, and the
    // entries' pool destroyed (the entries with it).
    inline void freeAnimationLibrary(uint32_t number)
    {
        const ee::Ptr<AnimationLibrary> library = animationLibrary(number);
        while (library->table.count != 0)
        {
            const ee::Ptr<AnimationEntry> entry = treeRecordOf<AnimationEntry>(library->table.root);
            freeMotion(entry->motion);
            searchTreeRemove(&library->table, &entry->name);
        }
        initSearchTree(&library->table, sym::lessWord);
        destroyPool(ee::Ptr<Pool>(library->entryPool));
        library->entryPool = 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeAnimationLibrary);

    // loadFormFileIntoLibrary: ZGE4's loadFormFile: the entry from the
    // archive's library's own pool (its number the load's archive), the file
    // read into a buffer of its size from the platform's memory (freed with
    // no test).
    uint32_t loadFormFileIntoLibrary(uint32_t library, uint32_t name, uint32_t bytes, ee::Ptr<ArchiveLoad> load)
    {
        const ee::Ptr<AnimationLibrary> into = animationLibrary(load->archive);
        const uint32_t buffer = allocateMemory(bytes);
        readAsset(library, buffer, 0);
        const ee::Ptr<AnimationEntry> entry = takeFromPool<AnimationEntry>(into->entryPool);
        entry->name = name;
        entry->archive = load->archive;
        const ee::Ptr<ZFile> file = openMemoryFile(buffer, bytes);
        const ee::Ptr<ChunkReader> reader(openChunkFile(file, kFormChunk, 0));
        setMotionLoadedHook(sym::keepMotion, &entry->motion);
        parseChunks(reader, 0);
        closeChunkFile(reader);
        closeFile(file, 0);
        freeMemory(buffer);
        return static_cast<uint32_t>(game::tail(searchTreeInsert, load->table, &entry->node));
    }
    READABLE_FUNCTION_WAITS_OWN(loadFormFile, Verified, bmx, loadFormFileIntoLibrary);

    int32_t freeAnimatorRecord(ee::Ptr<AnimatorRecord> animator); // (below)

    // freeAnimatorVisit (animator; a visit): it freed. Returns 1 (go on).
    uint32_t freeAnimatorVisit(ee::Ptr<AnimatorRecord> animator)
    {
        freeAnimatorRecord(animator);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeAnimatorVisit, Verified);

    namespace
    {
        namespace tail
        {
            int32_t initList(ee::Ptr<List> list) { return ee::tailCall<int32_t>(sym::initList, list); }
        }
    }

    // freeQueue (list): an animator's tracks back to the track pool,
    // the list emptied.
    int32_t freeQueue(ee::Ptr<RecordList<TrackRecord>> queue)
    {
        for (ee::Ptr<TrackRecord> track = queue->first; track;)
        {
            const ee::Ptr<TrackRecord> freed = track;
            track = track->next;
            giveRecord(*g_trackPool, freed);
        }
        return tail::initList(&queue->list);
    }

    // freeAnimatorTracks: ZGE4's freeQueue (the animator): its tracks' records
    // cleared too, each back to the track pool, the queue emptied.
    int32_t freeAnimatorTracks(ee::Ptr<AnimatorRecord> animator)
    {
        for (ee::Ptr<TrackRecord> track = animator->tracks.first; track;)
        {
            const ee::Ptr<TrackRecord> freed = track;
            track = track->next;
            setTrackRecord(animator->object, freed->track, 0u);
            giveRecord(*g_trackPool, freed);
        }
        return tail::initList(&animator->tracks.list);
    }
    READABLE_FUNCTION_OWN(freeQueue, Verified, bmx, freeAnimatorTracks);

    // freeLayers (list): an animator's pose layers back to the layer
    // pool, the list emptied.
    int32_t freeLayers(ee::Ptr<RecordList<PoseLayer>> layers)
    {
        for (ee::Ptr<PoseLayer> layer = layers->first; layer;)
        {
            const ee::Ptr<PoseLayer> freed = layer;
            layer = layer->next;
            giveRecord(*g_layerPool, freed);
        }
        return tail::initList(&layers->list);
    }

    // freeAnimatorLayers: ZGE4's freeLayers (the animator): its pose layers
    // back to the layer pool, the list emptied.
    int32_t freeAnimatorLayers(ee::Ptr<AnimatorRecord> animator)
    {
        for (ee::Ptr<PoseLayer> layer = animator->layers.first; layer;)
        {
            const ee::Ptr<PoseLayer> freed = layer;
            layer = layer->next;
            giveRecord(*g_layerPool, freed);
        }
        return tail::initList(&animator->layers.list);
    }
    READABLE_FUNCTION_OWN(freeLayers, Verified, bmx, freeAnimatorLayers);

    // freeEndedTrack (ZGE4 only; a track's onEnd hook, playAnimation sets
    // it: object, track, the track record): a queued track whose blend has
    // ended out of its animator's queue, its record cleared, back to the
    // track pool. A track in no queue, or of an object with no animator, is
    // left as it is.
    inline void freeEndedTrack(ee::Ptr<SceneObject> object, uint32_t, ee::Ptr<TrackRecord> record)
    {
        ee::Local<ee::Ptr<SceneObject>> key;
        *key = object;
        if (!record->next && !record->previous)
            return;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(key);
        if (!animator)
            return;
        setTrackRecord(*key, record->track, 0u);
        listRemove(&animator->tracks.list, record);
        giveRecord(*g_trackPool, record);
    }
    READABLE_FUNCTION_ONLY(bmx, freeEndedTrack);

    // freeAnimatorOnMessage (an EngineMessage of type 1 to the object): its
    // animator freed. Returns 0.
    uint32_t freeAnimatorOnMessage(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<AnimatorRecord> animator = findAnimator(&message->target);
        if (animator)
            freeAnimatorRecord(animator);
        return 0;
    }
    READABLE_FUNCTION_WAITS(freeAnimatorOnMessage, Verified);

    void freeMirrorMap(ee::Ptr<AnimatorRecord> animator);

    namespace
    {
        // (object, message type, handler)
        namespace tail
        {
            int32_t poolFree(uint32_t pool, ee::Ptr<AnimatorRecord> record) { return ee::tailCall<int32_t>(sym::poolFree, pool, record); }
        }
    }

    // freeAnimator2 (animator): its object's tracks stopped, it out of the
    // tree, its tracks and layers freed, its mirror map too; the object's
    // message 1 no longer handled; it back to the pool.
    int32_t freeAnimatorRecord(ee::Ptr<AnimatorRecord> animator)
    {
        stopTracks(animator->object);
        searchTreeRemove(animators(), &animator->object);
        if constexpr (game::readable::kIsBmx) // ZGE4's take the animator; it has no mirror map
        {
            freeAnimatorTracks(animator);
            freeAnimatorLayers(animator);
        }
        else
        {
            freeQueue(&animator->tracks);
            freeLayers(&animator->layers);
            if (animator->mirrorMap())
                freeMirrorMap(animator);
        }
        unsubscribe(animator->object, kAnimatorGoneMessage, sym::freeAnimatorOnMessage);
        return tail::poolFree(*g_animatorPool, animator);
    }
    READABLE_FUNCTION_WAITS(sym::freeAnimator2, freeAnimatorRecord, Verified);

    // buildMirrorMap (animator): its object's mirror map made from the bone
    // pairs (once).
    void buildMirrorMap(ee::Ptr<AnimatorRecord> animator)
    {
        ee::Local<MirrorPairs> pairs;
        const ee::Ptr<BoneMirror> mirror = animator->mirrorMap();
        *pairs = *g_mirrorPairs;
        if (!mirror)
            animator->setMirrorMap(ee::Ptr<BoneMirror>(newBoneMirror(animator->object, &pairs->pairs[0]))); // zge returns the map as a word
    }
    READABLE_FUNCTION_WAITS(buildMirrorMap, Verified);

    // freeMirrorMap (animator): its mirror map freed. 
    void freeMirrorMap(ee::Ptr<AnimatorRecord> animator)
    {
        const ee::Ptr<BoneMirror> mirror = animator->mirrorMap();
        if (mirror)
        {
            freeBoneMirror(mirror);
            animator->setMirrorMap({});
        }
    }
    READABLE_FUNCTION_WAITS(freeMirrorMap, Verified);

    // initAnimators: the animation system started: its four pools and two
    // trees. Returns 1.
    uint32_t initAnimators()
    {
        *g_animationEntryPool = newRecordPool(sizeof(AnimationEntry), 0x100, 0);
        *g_trackPool = newRecordPool(kTrackRecordBytes, 45, 0);
        *g_layerPool = newRecordPool(kPoseLayerBytes, 3, 0);
        *g_animatorPool = newRecordPool(sizeof(AnimatorRecord), 20, 0);
        initSearchTree(animationTable(), sym::lessWord);
        initSearchTree(animators(), sym::lessWord);
        return 1;
    }

    namespace // the game functions below call: ee/plain.h
    {
        // libc's memset (ZGE4's initAnimators clears its bar colour with it, not memoryFill).
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
    }

    // initAnimatorsAndEventHandlers: ZGE4's initAnimators: the track, layer
    // and animator pools (the libraries make their own entries' pools), the
    // animators' and the event handlers' trees, the animator's event hook
    // installed, and its profile bar (black). Returns 1.
    uint32_t initAnimatorsAndEventHandlers()
    {
        ee::Local<BarColour> colour;
        memset(ee::Address(colour), 0, sizeof(BarColour));
        colour->alpha = 0xFF;
        *g_trackPool = newRecordPool(kTrackRecordBytes, 25, 0);
        *g_layerPool = newRecordPool(kPoseLayerBytes, 3, 0);
        *g_animatorPool = newRecordPool(sizeof(AnimatorRecord), 20, 0);
        initSearchTree(animators(), sym::lessWord);
        initSearchTree(animationEventHandlers(), sym::lessWord);
        toggleAnimationEventHook(sym::bmx::onAnimationEvent);
        *g_animatorProfileBar = newProfileBar(0, 0, colour, sym::bmx::zanimUtilName);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(initAnimators, Verified, bmx, initAnimatorsAndEventHandlers);

    // shutdownAnimators: its end: every motion and animator freed, the pools.
    void shutdownAnimators()
    {
        searchTreeClear(animationTable(), sym::freeAnimationEntry, kAnimationTable);
        searchTreeClear(animators(), sym::freeAnimatorVisit, kAnimators);
        freeRecordPool(*g_animationEntryPool);
        freeRecordPool(*g_trackPool);
        freeRecordPool(*g_layerPool);
        freeRecordPool(*g_animatorPool);
    }
    READABLE_FUNCTION_WAITS_OWN(shutdownAnimators, Verified, bmx, shutdownAnimatorsAndLibraries);

    // shutdownAnimatorsAndLibraries: ZGE4's shutdownAnimators: each animation
    // library in use freed (by its number, 1 ..), every animator freed and
    // their tree emptied, the event handlers' pool destroyed (its word kept)
    // and their tree emptied, the animator's event hook taken off, its
    // profile bar freed, and the track, layer and animator pools destroyed
    // (their words kept).
    void shutdownAnimatorsAndLibraries()
    {
        for (uint32_t library = 0; library < kAnimationLibraries; ++library)
            if (g_animationLibraries->library[library].entryPool != 0)
                freeAnimationLibrary(library + 1);
        while (animators()->count != 0)
            freeAnimatorRecord(searchTreeEntry<AnimatorRecord>(animators()->root));
        initSearchTree(animators(), sym::lessWord);
        if (*g_animationEventHandlerPool)
            destroyPool(*g_animationEventHandlerPool);
        initSearchTree(animationEventHandlers(), sym::lessWord);
        toggleAnimationEventHook(0);
        freeProfileBar(*g_animatorProfileBar);
        destroyPool(ee::Ptr<Pool>(*g_trackPool));
        destroyPool(ee::Ptr<Pool>(*g_layerPool));
        destroyPool(ee::Ptr<Pool>(*g_animatorPool));
    }

    // loadAnimationArchive (path): an archive's (.ZAL) motions into the animation
    // table. Returns its number, or 0 when none was read.
    int32_t loadAnimationArchive(ee::Address path)
    {
        const uint32_t library = openAssetLibrary(path);
        const uint32_t loaded = forEachLibraryEntryOf(library, animationTable(), *g_archiveCount);
        closeAssetLibrary(library);
        const int32_t archive = *g_archiveCount;
        if (loaded != 0)
        {
            *g_archiveCount = archive + 1;
            return archive;
        }
        return 0;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(loadAnimationArchive, Verified)); // BMX XXX's: games/bmx/scene/animation_libraries.cpp

    // updateAnimators (seconds): the animators' frame (updateAnimator
    // each), then every animator with no track left freed. Returns the
    // last freeing's (0 with none).
    int32_t updateAnimators(ee::Float seconds)
    {
        ee::Local<ee::Float> context;
        if constexpr (game::readable::kIsBmx) // ZGE4 times the update
            startProfileBar(*g_animatorProfileBar);
        *context = seconds;
        searchTreeForEach(animators(), sym::updateAnimator, context.address());
        ee::Ptr<ListNode> link = firstPoolLink(*g_animatorPool);
        int32_t result = 0;
        while (link)
        {
            const ee::Ptr<AnimatorRecord> animator = poolRecordAs<AnimatorRecord>(link);
            link = nextPoolLink(link);
            result = 0;
            if (animator->tracks.count == 0)
                result = freeAnimatorRecord(animator);
        }
        if constexpr (game::readable::kIsBmx)
            return static_cast<int32_t>(stopProfileBar(*g_animatorProfileBar));
        return result;
    }
    READABLE_FUNCTION_WAITS(updateAnimators, Verified);

    // playAnimationNamed (node, name, mode, blend seconds, root
    // motion): the animation of that name played (playAnimation). Returns
    // its, or 0 when there is none.
    uint32_t playAnimationNamed(ee::Ptr<SceneObject> node, uint32_t name, uint32_t mode, ee::Float blend, ee::Float rootMotion)
    {
        ee::Local<ee::Ptr<Motion>> motion;
        *motion = {};
        const uint32_t found = static_cast<uint32_t>(findAnimation(name, motion));
        if (found == 0)
            return 0;
        return playAnimation(node, *motion, mode, blend, rootMotion);
    }
    READABLE_FUNCTION_WAITS(playAnimationNamed, Verified);

    // addPoseLayerNamed (node, name, flags, time, weight): the
    // animation of that name's pose layered (addPoseLayer). Returns its,
    // or 0 when there is none.
    uint32_t addPoseLayerNamed(ee::Ptr<SceneObject> node, uint32_t name, uint32_t flags, uint32_t, uint32_t, ee::Float time, ee::Float weight)
    {
        ee::Local<ee::Ptr<Motion>> motion;
        *motion = {};
        const uint32_t found = static_cast<uint32_t>(findAnimation(name, motion));
        if (found == 0)
            return 0;
        const uint32_t result = addPoseLayer(node, *motion, flags, time, weight);
        return result;
    }
    READABLE_FUNCTION_WAITS(addPoseLayerNamed, Verified);

    // addPoseLayer (node, animation, flags, time, weight): a pose
    // of the animation at `time` (taken into its length) blended over the
    // node's animation this frame at `weight` (0 .. 1), mirrored with flag
    // 8. Returns 1, or 0 when the node is not animated.
    uint32_t addPoseLayer(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t flags, ee::Float time, ee::Float weight)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        if (!animation)
            return 0;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        const ee::Float length = motionLength(animation);
        const ee::Ptr<PoseLayer> layer = takeFromPool<PoseLayer>(*g_layerPool);
        memoryFill(layer, 0, kPoseLayerBytes);
        layer->animation = animation;
        layer->time = time;
        while (length < layer->time)
            layer->time = layer->time - length;
        layer->owner = animator;
        layer->weight = ee::max(ee::min(weight, ee::Float(1.0f)), ee::Float(0.0f));
        if (flags & kModeMirrored)
        {
            if (!animator->mirrorMap())
                buildMirrorMap(animator);
            layer->mirrorHook = sym::mirrorLayer;
        }
        else
            layer->mirrorHook = 0;
        listPush(&animator->layers.list, layer);
        return 1;
    }
    READABLE_FUNCTION_WAITS(addPoseLayer, Verified);

    // findAnimation (name, &motion): the animation of that name (the
    // table's). Returns 1 when there is one.
    int32_t findAnimation(ee::Address name, ee::Ptr<ee::Ptr<Motion>> motionOut)
    {
        const uint32_t hash = nameHash(ee::Ptr<const char>(name));
        return static_cast<int32_t>(findAnimationByHash(hash, motionOut));
    }

    ee::Ptr<Motion> findLibraryAnimation(uint32_t hash); // (below: ZGE4's findAnimationByHash)

    // findAnimationInLibraries: ZGE4's findAnimation (name): the motion of
    // that name in the animation libraries, or 0.
    ee::Ptr<Motion> findAnimationInLibraries(ee::Address name)
    {
        return findLibraryAnimation(nameHash(ee::Ptr<const char>(name)));
    }
    READABLE_FUNCTION_WAITS_OWN(findAnimation, Verified, bmx, findAnimationInLibraries);

    // findAnimationByHash (name hash, motion * or 0): the table's motion of that
    // name. Returns 1 when there is one.
    uint32_t findAnimationByHash(uint32_t hash, ee::Ptr<ee::Ptr<Motion>> motionOut)
    {
        ee::Local<uint32_t> key;
        *key = hash;
        const ee::Ptr<AnimationEntry> entry = searchTreeEntry<AnimationEntry>(searchTreeFind(animationTable(), key));
        if (!entry)
            return 0;
        if (motionOut)
            *motionOut = entry->motion;
        return 1;
    }

    // findLibraryAnimation: ZGE4's findAnimationByHash (name hash): the
    // motion of that name in the first animation library in use that has
    // it, or 0.
    ee::Ptr<Motion> findLibraryAnimation(uint32_t hash)
    {
        ee::Local<uint32_t> key;
        *key = hash;
        for (uint32_t index = 0; index < kAnimationLibraries; ++index)
        {
            const ee::Ptr<AnimationLibrary> library = &g_animationLibraries->library[0] + index;
            if (library->entryPool == 0)
                continue;
            const ee::Ptr<AnimationEntry> entry = searchTreeEntry<AnimationEntry>(searchTreeFind(&library->table, key));
            if (entry)
                return entry->motion;
        }
        return {};
    }
    READABLE_FUNCTION_WAITS_OWN(findAnimationByHash, Verified, bmx, findLibraryAnimation);

    // playAnimation (node, animation, mode, blend seconds, root
    // motion): the animation played on the node (its animator made the
    // first time: in the tree, freed on the object's message 1, animated
    // off screen unless it is a level "NPC"; mode bit 1 on screen only, bit
    // 2 off screen too): a track for it (bit 0 loops, bit 3 mirrored),
    // blended in over `blend` seconds, or at once (the queue and layers let
    // go, the time back to 0); queued as the newest. Returns 1, or 0 with no
    // animation or modes 6.
    uint32_t playAnimation(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t mode, ee::Float blend, ee::Float rootMotion)
    {
        ee::Local<ee::Ptr<SceneObject>> object;
        *object = node;
        if (!animation || (mode & (kModeOnScreenOnly | kModeOffScreenToo)) == (kModeOnScreenOnly | kModeOffScreenToo))
            return 0;
        ee::Ptr<AnimatorRecord> self = findAnimator(object);
        if (!self)
        {
            // A new animator.
            self = takeFromPool<AnimatorRecord>(*g_animatorPool);
            memoryFill(self, 0, sizeof(AnimatorRecord));
            self->time = 0.0f;
            self->paused = 0;
            self->object = *object;
            initList(&self->tracks.list);
            initList(&self->layers.list);
            subscribe(*object, kAnimatorGoneMessage, sym::freeAnimatorOnMessage);
            const uint32_t levelObject = isLevelObject(*object);
            uint32_t offScreen = 1;
            if (levelObject != 0)
            {
                if (*g_npcHashed == 0)
                {
                    *g_npcHash = nameHash(ee::Ptr<const char>(kNpcName));
                    *g_npcHashed = 1;
                }
                offScreen = levelObjectTypeHash(*object) != *g_npcHash ? 1 : 0;
            }
            self->offScreen = offScreen;
            searchTreeInsert(animators(), &self->node);
        }
        AnimatorRecord &animator = *self;
        if (mode & kModeOnScreenOnly)
            animator.offScreen = 0;
        else if (mode & kModeOffScreenToo)
            animator.offScreen = 1;

        // Its track.
        const ee::Ptr<TrackRecord> track = takeFromPool<TrackRecord>(*g_trackPool);
        memoryFill(track, 0, kTrackRecordBytes);
        track->rootMotion = rootMotion;
        track->loops = mode & kModeLoops;
        track->blendTime = blend;
        track->animation = animation;
        track->track = 0;
        track->owner = self;
        if (mode & kModeMirrored)
        {
            if (!animator.mirrorMap())
                buildMirrorMap(self);
            track->mirrorHook = sym::mirrorTrack;
        }
        else
            track->mirrorHook = 0;
        if (ee::Float(0.0f) < blend)
            track->track = crossfadeAnimation(*object, animation, animator.time, blend);
        else
        {
            // At once: the queue and layers let go, from the start.
            freeQueue(&animator.tracks);
            freeLayers(&animator.layers);
            animator.time = 0.0f;
            animator.paused = 0;
            track->track = startAnimation(*object, animation, ee::Float(0.0f));
        }
        setTrackRecord(*object, track->track, track);
        listPush(&animator.tracks.list, track);
        return 1;
    }

    // playAnimationWithHooks: ZGE4's playAnimation (node, animation, mode,
    // the caller's onBlend hook and its context, blend seconds): no mirror,
    // root motion or "NPC" test; the animator on screen only with mode bit
    // 1, else off screen too, at every play; the track's record its hooks
    // (onBlend, and freeEndedTrack once its blend has ended). Returns 1.
    uint32_t playAnimationWithHooks(ee::Ptr<SceneObject> node, ee::Ptr<Motion> animation, uint32_t mode, BlendHook onBlend,
                                    ee::Address blendContext, ee::Float blend)
    {
        ee::Local<ee::Ptr<SceneObject>> object;
        *object = node;
        ee::Ptr<AnimatorRecord> self = findAnimator(object);
        if (!self)
        {
            // A new animator.
            self = takeFromPool<AnimatorRecord>(*g_animatorPool);
            memoryFill(self, 0, sizeof(AnimatorRecord));
            self->time = 0.0f;
            self->paused = 0;
            self->object = *object;
            initList(&self->tracks.list);
            initList(&self->layers.list);
            subscribe(*object, kAnimatorGoneMessage, sym::freeAnimatorOnMessage);
            searchTreeInsert(animators(), &self->node);
        }
        AnimatorRecord &animator = *self;
        animator.offScreen = (mode & kModeOnScreenOnly) ? 0 : 1;

        // Its track.
        const ee::Ptr<TrackRecord> track = takeFromPool<TrackRecord>(*g_trackPool);
        memoryFill(track, 0, kTrackRecordBytes);
        track->loops = mode & kModeLoops;
        track->hooks.onBlend = onBlend;
        track->hooks.blendContext = blendContext;
        track->hooks.onEnd = sym::bmx::freeEndedTrack;
        track->blendTime = blend;
        track->animation = animation;
        track->hooks.endContext = ee::Address(track);
        if (ee::Float(0.0f) < blend)
            track->track = crossfadeAnimation(*object, animation, animator.time, blend);
        else
        {
            // At once: the queue and layers let go, from the start.
            freeAnimatorTracks(self);
            freeAnimatorLayers(self);
            animator.time = 0.0f;
            animator.paused = 0;
            track->track = startAnimation(*object, animation, ee::Float(0.0f));
        }
        setTrackRecord(*object, track->track, &track->hooks);
        listPush(&animator.tracks.list, track);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(playAnimation, Verified, bmx, playAnimationWithHooks);

    // animationOver (node, &done): whether its animation is over (the
    // animation player's animationDone at its animator's time). Returns 1
    // when the node is animated.
    int32_t animationOver(ee::Ptr<SceneObject> node, ee::Ptr<int32_t> doneOut)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        *doneOut = animationDone(*nodeSlot, animator->time);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sym::animationDone2, animationOver, Verified);

    // stopAnimation (node): its animation stopped (its tracks and layers let
    // go, unpaused). Returns 1 when the node is animated.
    int32_t stopAnimation(ee::Ptr<SceneObject> node)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        freeQueue(&animator->tracks);
        freeLayers(&animator->layers);
        stopTracks(*nodeSlot);
        animator->paused = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(stopAnimation, Verified);

    // animatorBoneOffset (object, Vec4 *out): the object's own bone (the one
    // named as it) moved as the object turns: its translation by the
    // object's rotation, into *out.
    void animatorBoneOffset(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out)
    {
        ee::Local<Matrix> rotation;
        const uint32_t name = objectNameHash(object);
        const ee::Ptr<AnimationChannel> bone = findAnimatorBone(object, name);
        if (bone == 0)
            return;
        const ee::Ptr<Vec4> translation = boneTranslation(bone);
        const ee::Ptr<Quaternion> quaternion = objectOrientation(object);
        quaternionToMatrix(rotation, quaternion);
        *out = transformed(*rotation, *translation);
    }
    READABLE_FUNCTION(animatorBoneOffset, Verified);

    // animationTime (node, float *time): its animation's time. Returns 1
    // when the node is animated.
    int32_t animationTime(ee::Ptr<SceneObject> node, ee::Ptr<ee::Float> timeOut)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        *timeOut = animator->time;
        return 1;
    }
    READABLE_FUNCTION_WAITS(animationTime, Verified);

    // setAnimationTime (node, time): its animation's time set. Returns 1
    // when the node is animated.
    int32_t setAnimationTime(ee::Ptr<SceneObject> node, ee::Float time)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        animator->time = time;
        return 1;
    }
    READABLE_FUNCTION_WAITS(setAnimationTime, Verified);

    // advanceAnimationTime (node, seconds): its animation's time moved on.
    // Returns 1 when the node is animated.
    int32_t advanceAnimationTime(ee::Ptr<SceneObject> node, ee::Float seconds)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        animator->time = animator->time + seconds;
        return 1;
    }
    READABLE_FUNCTION_WAITS(advanceAnimationTime, Verified);

    // pauseAnimation (node): its animation paused. Returns 1 when the node is
    // animated.
    int32_t pauseAnimation(ee::Ptr<SceneObject> node)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        animator->paused = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(pauseAnimation, Verified);

    // resumeAnimation (node): its animation going again. Returns 1 when the
    // node is animated.
    int32_t resumeAnimation(ee::Ptr<SceneObject> node)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (!animator)
            return 0;
        animator->paused = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(resumeAnimation, Verified);

    // animationPaused (node, &paused): whether its animation is paused (0 when
    // the node is not animated). Returns 1 when it is.
    int32_t animationPaused(ee::Ptr<SceneObject> node, ee::Ptr<uint32_t> pausedOut)
    {
        ee::Local<ee::Ptr<SceneObject>> nodeSlot;
        *nodeSlot = node;
        *pausedOut = 0;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(nodeSlot);
        if (animator)
        {
            *pausedOut = animator->paused;
            return 1;
        }
        *pausedOut = 0;
        return 0;
    }
    READABLE_FUNCTION_WAITS(animationPaused, Verified);

    // ---- ZGE4: an object's animator asked about and set (BMX XXX only) ------

    // objectHasAnimator: whether the object has an animator.
    inline uint32_t objectHasAnimator(ee::Ptr<SceneObject> object)
    {
        ee::Local<ee::Ptr<SceneObject>> objectSlot;
        *objectSlot = object;
        return findAnimator(objectSlot) ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, objectHasAnimator);

    // freeAnimatorOf: the object's animator freed (its tracks and layers let go). Returns 1 when it had one.
    inline uint32_t freeAnimatorOf(ee::Ptr<SceneObject> object)
    {
        ee::Local<ee::Ptr<SceneObject>> objectSlot;
        *objectSlot = object;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(objectSlot);
        if (!animator)
            return 0;
        freeAnimatorRecord(animator);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeAnimatorOf);

    // finishAnimation: an object playing one track alone posed at that motion's end (its time the motion's
    // length). Returns 1 when it did; 0 without an animator or with more tracks than one (or none).
    inline uint32_t finishAnimation(ee::Ptr<SceneObject> object)
    {
        ee::Local<ee::Ptr<SceneObject>> objectSlot;
        *objectSlot = object;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(objectSlot);
        if (!animator || animator->tracks.count != 1)
            return 0;
        animator->time = motionLength(animator->tracks.first->animation);
        updateAnimation(animator->object, sym::onTrackBlend, animator->time);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, finishAnimation);

    // animationMode: always 0. Bug kept: the original looks the object's newest track up and returns nothing of
    // it, so the mode setAnimationMode gave it never comes back.
    inline uint32_t animationMode(ee::Ptr<SceneObject> object)
    {
        ee::Local<ee::Ptr<SceneObject>> objectSlot;
        *objectSlot = object;
        findAnimator(objectSlot);
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, animationMode);

    // setAnimationMode: the object's newest track looped or not (kModeLoops), and the object animated off screen
    // too unless kModeOnScreenOnly is set, as playAnimation's mode does it. Returns 1, 0 without an animator or a
    // track.
    inline uint32_t setAnimationMode(ee::Ptr<SceneObject> object, uint32_t mode)
    {
        ee::Local<ee::Ptr<SceneObject>> objectSlot;
        *objectSlot = object;
        const ee::Ptr<AnimatorRecord> animator = findAnimator(objectSlot);
        if (!animator)
            return 0;
        const ee::Ptr<TrackRecord> newest = animator->tracks.last;
        if (!newest)
            return 0;
        animator->offScreen = (mode & kModeOnScreenOnly) ? 0 : 1;
        newest->loops = mode & kModeLoops;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setAnimationMode);

    // loadLibraryMotion (ZGE4 only): the motion of an asset library's entry (by its id: the motion's name hash),
    // read; 0 without the library, the entry or a motion read.
    inline ee::Ptr<Motion> loadLibraryMotion(uint32_t library, uint32_t id)
    {
        ee::Local<ee::Ptr<Motion>> loaded;
        *loaded = ee::Ptr<Motion>();
        if (library == 0 || findAsset(library, id) == 0)
            return {};
        setMotionLoadedHook(sym::keepMotion, loaded);
        if (parseCurrentAsset(library, 0) == 0)
            return {};
        return *loaded;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadLibraryMotion);

    // animationLength (ZGE4 only): the length of the track the object started last (newestTrackLength); 0 when it
    // has no animator.
    inline ee::Float animationLength(ee::Ptr<SceneObject> object)
    {
        if (objectHasAnimator(object) == 0)
            return ee::Float(0.0f);
        return newestTrackLength(object);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, animationLength);

    // randomizeAnimationTime (ZGE4 only): the object's animation time moved on by a random share (0 to 1) of
    // animationLength, so that objects looping one motion do not move together.
    inline void randomizeAnimationTime(ee::Ptr<SceneObject> object)
    {
        constexpr ee::Float kRandomShare(1.0f / 2147483648.0f); // a 31-bit random number to 0 .. 1
        ee::Local<ee::Float> time;
        const ee::Float length = animationLength(object);
        if (objectHasAnimator(object) == 0)
            return;
        animationTime(object, time);
        const int32_t random = static_cast<int32_t>(randomNumber() & 0x7FFFFFFFu);
        *time = *time + length * (ee::toFloat(random) * kRandomShare);
        setAnimationTime(object, *time);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, randomizeAnimationTime);

    namespace
    {
        // A layer's track's record is the mirror map.
        namespace tail
        {
            int32_t setTrackRecord(ee::Ptr<SceneObject> object, uint32_t track) { return ee::tailCall<int32_t>(sym::setTrackRecord, object, track, 0u); }
        }
    }

    // mirrorLayer (object, track, weight; a mirrored layer's hook):
    // the layer's pose mirrored (its track's record is the mirror map), or
    // the record cleared when its blend ended.
    int32_t mirrorLayer(ee::Ptr<SceneObject> object, uint32_t track, ee::Float weight)
    {
        const uint32_t mirror = trackRecord(object, track);
        if (mirror == 0)
            return 0;
        if (!(weight == ee::Float(kBlendEnded)))
            return static_cast<int32_t>(mirrorPoseKeepingResult(mirror));
        return tail::setTrackRecord(object, track);
    }
    READABLE_FUNCTION(mirrorLayer, Verified);

    // onAnimationEvent (ZGE4 only; the animation player's event hook,
    // initAnimators installs it: object, event, its time): the handler
    // registered for the event's kind called (object, event, its context,
    // time), when there is one.
    inline void onAnimationEvent(ee::Ptr<SceneObject> object, ee::Ptr<AnimationEvent> event, ee::Float time)
    {
        ee::Local<uint32_t> kind;
        *kind = eventKind(event);
        const ee::Ptr<AnimationEventHandler> handler =
            searchTreeEntry<AnimationEventHandler>(searchTreeFind(animationEventHandlers(), kind));
        if (handler)
            handler->handler(object, event, handler->context, time);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, onAnimationEvent);

    // registerAnimationEventHandler (ZGE4 only): the handler of the events of
    // a name (their kind: the name's hash), with the word it is called with,
    // unless the kind has one already.
    inline void registerAnimationEventHandler(ee::Ptr<const char> name, uint32_t handler, uint32_t context)
    {
        ee::Local<uint32_t> kind;
        *kind = static_cast<uint32_t>(nameHash(name));
        if (searchTreeFind(animationEventHandlers(), kind))
            return;
        const ee::Ptr<AnimationEventHandler> record(
            poolTakeCreating(&*g_animationEventHandlerPool, sizeof(AnimationEventHandler)));
        record->kind = *kind;
        stringCopy(record->name, name);
        record->handler = handler;
        record->context = context;
        searchTreeInsert(animationEventHandlers(), &record->node);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, registerAnimationEventHandler);
}
