// BMX XXX's riders and their action states: a rider by its index, and a
// rider's state at one of its action state levels, read and set. DM2's
// (game/dm2/rider/rider_state.cpp).

#include "rider.h"
#include "game_calls.h"

#include "bmx/action/camera_state.h"
#include "bmx/create/menu_model.h"
#include "bmx/attach/attach_calls.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/pickups.h"
#include "bmx/game/player_riders.h"
#include "bmx/profile/players.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "bmx/level/level_collision.h"
#include "bmx/sound/roll_sound.h"
#include "bmx/weapon/weapon.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/collision/dynamics.h"
#include "zge/game.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/collision/sweep.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/animation.h"
#include "zge/scene/carried_objects.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/animator.h"
#include "zge/scene/model_records.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <algorithm>
#include <cstdint>
#include "sym/bmx/rider/rider_state.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // A dynamics force record doubles as a bail body's half extents (its vector is the record's first field).
        ee::Ptr<zge::BodyExtents> extentsOf(ee::Ptr<zge::DynamicsForce> force) { return force.cast<zge::BodyExtents>(); }
        // The riders by index (a pointer each).
        const ee::Ptr<ee::Ptr<Rider>> kRiders(sym::g_riders);

        // A level-0 change puts the camera's mode at level 1 back to its
        // first (which clears the levels below it).
        constexpr uint32_t kCameraActionLevel = 1;
        constexpr uint32_t kCameraFirstMode = 0;

        // The motion a name not found falls back to: "ground.coast.loop"'s
        // hash.
        constexpr uint32_t kGroundCoastLoopHash = 0x0A6D2580u;
    }

    namespace
    {
        // Each of the nine stats' range, in the rider's order from airStat (the riders' table, g_characterStats,
        // follows it).
        struct StatRange
        {
            ee::Float least, most;
        };
        inline ee::Global<StatRange[kCharacterStats], sym::g_statRanges> g_statRanges;
        // The bikes' stats: a line's five bikes, for each of the seven lines (the menu's bars show them too).
        constexpr uint32_t kBikeLines = 7, kBikesInLine = 5;
        inline ee::Global<uint8_t[kBikeLines][kBikesInLine][kCharacterStats], sym::g_bikeStats> g_bikeStats;
        // A stat's steps: the character's value and the bike's together, at most 12, are that many twelfths of
        // its range above the least.
        constexpr int32_t kStatSteps = 12;
        const ee::Float kStatStep(1.0f / 12.0f);
    }

    // setRiderBikeStats: the rider's nine stats (airStat ..) for its player's
    // character on a bike (a line's, which of its five): each the least of
    // its range and the character's and the bike's values together (at most
    // 12) twelfths of the range above it. Neither index is checked.
    void setRiderBikeStats(Rider *rider, uint32_t line, uint32_t bike)
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(rider->player);
        ee::Float *const stats = &rider->airStat; // the nine, airStat to wallRideStat
        for (uint32_t i = 0; i < kCharacterStats; ++i)
        {
            const int32_t value = std::min(character->stats[i] + (*g_bikeStats)[line][bike][i], kStatSteps);
            const StatRange &range = (*g_statRanges)[i];
            stats[i] = range.least + ee::toFloat(value) * (range.most - range.least) * kStatStep;
        }
    }
    READABLE_FUNCTION(setRiderBikeStats, Verified);

    // The rider's motion of a name hash into *motion; 1 when there is one.
    int32_t riderMotionByHash(uint32_t name, ee::Ptr<zge::Motion> *motion)
    {
        *motion = zge::findLibraryAnimation(name);
        return *motion ? 1 : 0;
    }
    READABLE_FUNCTION(riderMotionByHash, Verified);

    // The rider's motion of a name (its hash into *nameHash when asked
    // for, the motion into *motion); a name with no motion takes the
    // ground's coasting loop. Returns 1 when there is a motion.
    int32_t riderMotionByName(uint32_t name, uint32_t *nameHash, ee::Ptr<zge::Motion> *motion)
    {
        uint32_t hash = zge::nameHash(ee::pointerAt<char>(name));
        if (riderMotionByHash(hash, motion) == 0)
        {
            hash = kGroundCoastLoopHash;
            riderMotionByHash(hash, motion);
        }
        if (nameHash != nullptr)
            *nameHash = hash;
        return *motion ? 1 : 0;
    }
    READABLE_FUNCTION(riderMotionByName, Verified);

    void physicsInit(); // physics_state.cpp

    namespace
    {
        // The bones' name hashes, set up once (the model's bone names; DM2's
        // with the eyelids).
        struct BoneName
        {
            uint32_t name;   // the string
            uint32_t global; // where its hash goes
        };
        constexpr BoneName kBoneNames[] = {
            {sym::g_textHips, sym::g_hipsBoneHash},               // "Hips"
            {sym::g_textHead, sym::g_headBoneHash},               // "Head"
            {sym::g_textEyelids, sym::g_eyelidsBoneHash},         // "Eyelids"
            {sym::g_textLeftHand, sym::g_leftHandBoneHash},       // "LeftHand"
            {sym::g_textRightHand, sym::g_rightHandBoneHash},     // "RightHand"
            {sym::g_textLeftFoot, sym::g_leftFootBoneHash},       // "LeftFoot"
            {sym::g_textRightFoot, sym::g_rightFootBoneHash},     // "RightFoot"
            {sym::g_textLeftLowArm, sym::g_leftLowArmBoneHash},   // "LeftLowArm"
            {sym::g_textRightLowArm, sym::g_rightLowArmBoneHash}, // "RightLowArm"
            {sym::g_textHipsdeck, sym::g_hipsDeckBoneHash},       // "Hipsdeck"
            {sym::g_textHandlebars, sym::g_handlebarsBoneHash},   // "Handlebars"
            {sym::g_textSprocket, sym::g_sprocketBoneHash},       // "Sprocket"
            {sym::g_textFrontWheel, sym::g_frontWheelBoneHash},   // "FrontWheel"
            {sym::g_textBackWheel, sym::g_backWheelBoneHash},     // "BackWheel"
        };

        // Those three dynamics files ("dynamics.rider", "dynamics.riderAnim", "dynamics.bike"): each one's name,
        // its bytes as read, and the definition made from them (each rider's copies are made from it: newRider).
        struct RiderDynamicsFile
        {
            ee::Ptr<const char> name;
            ee::Address data;
            ee::Ptr<zge::Dynamics> definition;
        };
        constexpr int32_t kRiderDynamicsFiles = 3;
        inline ee::Global<RiderDynamicsFile[kRiderDynamicsFiles], sym::g_riderDynamicsFiles> g_riderDynamicsFiles;
        // Where freeRider leaves the bail body before its box leaves the sweep: far out on every axis (10000).
        inline ee::Global<ee::Float, sym::g_parkedBodyCoordinate> g_parkedBodyCoordinate;
        // The library the three are read from ("assets\\models.zal"), and the chunk form they are in.
        constexpr uint32_t kModelsLibrary = sym::textModelsLibrary;
        constexpr uint32_t kFormChunk = zge::chunkTag("FORM");
    }

    // loadRiderDynamics: the three dynamics files read from the models' library, each into a block of its own and
    // parsed into its definition; at the first that is missing or does not read (its block freed), the rest are
    // left. 1 when all three were read.
    uint32_t loadRiderDynamics()
    {
        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kModelsLibrary);
        if (!library)
            return 0;
        uint32_t read = 0;
        for (RiderDynamicsFile &dynamics : *g_riderDynamicsFiles)
        {
            read = 0;
            if (zge::findAsset(ee::Address(library), zge::nameHash(dynamics.name)) == 0)
                break;
            const uint32_t bytes = zge::zfileLibraryAssetBytes(library);
            dynamics.data = zge::heapAllocate(bytes);
            if (dynamics.data == 0)
                break;
            if (zge::readZfileLibraryAsset(library, dynamics.data) != 0)
            {
                const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(dynamics.data, bytes);
                read = 1;
                const ee::Ptr<zge::ChunkReader> reader = zge::openChunkFile(file, kFormChunk, 0);
                zge::parseChunks(reader.get(), &dynamics.definition);
                zge::closeChunkFile(reader.get());
                zge::closeFile(file, 0);
                if (!dynamics.definition)
                    read = 0;
            }
            if (read != 1)
            {
                zge::heapFree(dynamics.data);
                break;
            }
        }
        zge::closeZfileLibrary(library);
        return read;
    }
    READABLE_FUNCTION(loadRiderDynamics, Verified);

    // freeRiderDynamics: the three dynamics files' definitions freed (the last first) with their bytes.
    void freeRiderDynamics()
    {
        for (int32_t file = kRiderDynamicsFiles - 1; file >= 0; --file)
        {
            RiderDynamicsFile &dynamics = (*g_riderDynamicsFiles)[file];
            if (!dynamics.definition)
                continue;
            zge::freeDynamicsCopy(dynamics.definition);
            dynamics.definition = ee::Ptr<zge::Dynamics>();
            zge::heapFree(dynamics.data);
            dynamics.data = 0;
        }
    }
    READABLE_FUNCTION(freeRiderDynamics, Verified);

    // Once at start: the bone name hashes, the physics task, the rider
    // records cleared, the dynamics loaded (the original's tail jump). DM2
    // loads the riders' motions here instead.
    void riderSetup()
    {
        for (const BoneName &bone : kBoneNames)
            *ee::Ptr<uint32_t>(bone.global) = zge::nameHash(ee::pointerAt<char>(bone.name));
        physicsInit();
        zge::memoryFill(kRiders, 0, kRiderSlots * sizeof(uint32_t));
        game::tail(loadRiderDynamics);
    }
    READABLE_FUNCTION(riderSetup, Verified);

    // The rider of an index (its low byte).
    ee::Ptr<Rider> riderOf(uint32_t index)
    {
        return kRiders[index & 0xFFu];
    }
    READABLE_FUNCTION(riderOf, Verified);

    // The rider's action state at a level (0 .. 3).
    uint8_t actionState(const Rider *rider, uint32_t level)
    {
        return rider->actionStates[level];
    }
    READABLE_FUNCTION(actionState, Verified);

    // Sets an action state level: the deeper levels back to 0, the frame
    // counters from 0, the motion's rate back to 1. Entering a level-0
    // state resets what the last one left (taps, tricks, flags, the
    // balances) and the camera, unless the camera runs its own; the landing
    // keeps the rest. Levels 0 and 1 also clear the bars' lean and the trick
    // on top. Returns the level's state. DM2's, with BMX XXX's own states
    // and records: it clears the balance frames on a landing and in the
    // riding and other states (DM2 on a grind), and the balances only there.
    uint8_t setActionState(Rider *rider, uint32_t level, uint32_t state)
    {
        rider->actionStates[level] = static_cast<uint8_t>(state);
        rider->actionFrames[level] = 0;
        for (uint32_t deeper = level + 1; deeper < kActionLevels; ++deeper)
        {
            rider->actionStates[deeper] = 0;
            rider->actionFrames[deeper] = 0;
        }
        rider->motionFlags &= ~kMotionFlag4;
        rider->motionRate = 1.0f;

        if (level == 0)
        {
            switch (state)
            {
            case kActionAir:
                clearTaps(rider, 0u);
                clearTaps(rider, 1u);
                clearFlag(rider, kFlagOnOneWheel);
                rider->trickMotion = kNoTrick;
                clearFlag(rider, kFlagTrickTurn);
                rider->trick = kNoTrick;
                rider->trickLoops = 0;
                break;
            case kActionGrind:
            case kActionStall:
            case kActionLipTrick:
            case kActionWheelie:
            case kActionFlatland:
                clearFlag(rider, kFlagFellOff);
                break;
            case kActionLanding:
                rider->balanceFrames = 0;
                rider->grindBalance = 0.0f;
                rider->wheelieBalance = 0.0f;
                rider->clearedWithBalances = 0;
                return static_cast<uint8_t>(state);
            default: // riding, and BMX XXX's states past the landing
                clearFlag(rider, kFlagMirrored);
                clearTaps(rider, 1u);
                clearFlag(rider, kFlagTrickTurn);
                clearFlag(rider, kFlagLeftRail);
                rider->trickMotion = kNoTrick;
                clearFlag(rider, kFlagBalanceTurnedRound);
                rider->trick = kNoTrick;
                rider->trickLoops = 0;
                rider->grindBalance = 0.0f;
                rider->wheelieBalance = 0.0f;
                rider->clearedWithBalances = 0;
                rider->balanceFrames = 0;
                break;
            }
            rider->yawTurned = 0;
            if (!hasFlag(rider, kFlagNoPlayer) && cameraState() == 0)
                setCameraMode(&*ee::Ptr<ActionCamera>(playerCamera(uint32_t(rider->player))), kCameraActionLevel, kCameraFirstMode);
        }
        else if (level != 1)
        {
            rider->wheelDrag = 0.0f;
            return rider->actionStates[level];
        }
        clearFlag(rider, kFlagIdle);
        clearFlag(rider, kFlagTopTrick);
        rider->topTrickFrames = 0;
        rider->trickTurned = 0;
        rider->barLean[0] = 0.0f;
        rider->barLean[1] = 0.0f;
        rider->wheelDrag = 0.0f;
        return rider->actionStates[level];
    }
    READABLE_FUNCTION(setActionState, Verified);

    // forEachRider (visit, context): each rider there is, by index, visited until a visit returns 0.
    void forEachRider(RiderVisit visit, ee::Address context)
    {
        for (uint32_t index = 0; index < kRiderSlots; index = (index + 1) & 0xFFu)
        {
            if (kRiders[index] && visit(index, context) == 0)
                return;
        }
    }
    READABLE_FUNCTION(forEachRider, Verified);

    // playerIndexOfNode (node): the rider whose model or bike model is this engine object, or 255 for none. DM2's.
    uint32_t playerIndexOfNode(uint32_t node)
    {
        for (uint32_t index = 0; index < kRiderSlots; ++index)
        {
            const ee::Ptr<Rider> rider = kRiders[index];
            if (rider && (ee::Address(rider->messenger) == node || ee::Address(rider->bikeModel) == node))
                return index;
        }
        return 255;
    }
    READABLE_FUNCTION(playerIndexOfNode, Verified);

    namespace
    {
        // A limb's bones by name: the upper and the lower one, and the bone the chain ends at. In
        // Rider::limbTargets' order: the left arm ("LeftUpArm", "LeftLowArm", "LeftHand"), the right arm, the left
        // leg ("LeftUpLeg", "LeftLowLeg", "LeftFoot"), the right leg.
        struct LimbBoneNames
        {
            ee::Ptr<char> upper;
            ee::Ptr<char> lower;
            ee::Ptr<char> end;
        };
        constexpr uint32_t kLimbs = 4;
        inline ee::Global<LimbBoneNames[kLimbs], sym::g_limbBoneNames> g_limbBoneNames;


        // Where one of the model's bones sits in its parent at rest.
        void bindPosition(ee::Ptr<zge::SceneObject> model, ee::Ptr<char> boneName, ee::Ptr<zge::Vec4> out)
        {
            zge::modelBoneBindPosition(model, zge::findModelBone(model, zge::nameHash(boneName)), out);
        }
    }

    // setUpLimbChains (rider): each limb's chain made over the rider model's bones: the joint is where the lower
    // bone sits in the upper one, the end where the hand or foot sits in the lower one. DM2's.
    void setUpLimbChains(Rider *rider)
    {
        const ee::Ptr<zge::SceneObject> model(rider->messenger);
        for (uint32_t limb = 0; limb < kLimbs; ++limb)
        {
            const LimbBoneNames &names = (*g_limbBoneNames)[limb];
            ee::Local<zge::Vec4> joint;
            ee::Local<zge::Vec4> end;
            bindPosition(model, names.lower, joint);
            bindPosition(model, names.end, end);
            zge::setUpBoneChain(&rider->limbTargets[limb], model, ee::Address(names.upper), joint, ee::Address(names.lower), end);
        }
    }
    READABLE_FUNCTION(setUpLimbChains, Verified);

    namespace
    {
        // riderModelHasPart's question to each of the model's entries: the part's name and its hash, and the
        // answer.
        struct PartQuestion
        {
            ee::Ptr<const char> name;
            uint32_t nameHash;
            uint32_t found;
        };
        static_assert(offsetof(PartQuestion, found) == 8);
        // The night vision goggles, the male riders' part and the female riders'.
        constexpr uint32_t kNightVisionMale = sym::textNightVisionMale;     // "GMHAC_NIGHTVISION"
        constexpr uint32_t kNightVisionFemale = sym::textNightVisionFemale; // "GFHAC_NIGHTVISION"
        constexpr uint32_t kPartAspect = 0; // a model's aspect of parts (the MODEL script's PART)
    }

    // visitRiderModelPart (model, aspect, library, asset, rank, question): one of the model's entries asked
    // whether it is the part (by its name's hash): 0 when it is (the visits stop), else 1.
    int32_t visitRiderModelPart(ee::Ptr<zge::ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library,
                                ee::Ptr<const char> asset, int32_t rank, ee::Ptr<PartQuestion> question)
    {
        (void)model;
        (void)aspect;
        (void)library;
        (void)rank;
        if (zge::nameHash(asset) != question->nameHash)
            return 1;
        question->found = 1;
        return 0;
    }
    READABLE_FUNCTION(visitRiderModelPart, Verified);

    // riderModelHasPart (rider, aspect, part): 1 when the rider's model has an entry of the aspect by that name.
    uint32_t riderModelHasPart(Rider *rider, uint32_t aspect, ee::Ptr<const char> part)
    {
        ee::Local<PartQuestion> question;
        question->found = 0;
        question->name = part;
        question->nameHash = zge::nameHash(part);
        zge::forEachModelEntry(rider->model, aspect, zge::ModelEntryVisit(sym::visitRiderModelPart),
                               question.address());
        return question->found;
    }
    READABLE_FUNCTION(riderModelHasPart, Verified);

    // riderModelReady (rider): 1 unless the rider's model is still being built (kFlagModelChanging). The frame the
    // build ends: the model's binding let go, the limbs' chains made again, and (for a rider the camera follows)
    // the soft parts' rig and the limbs' rig made anew and the night vision goggles looked for.
    int32_t riderModelReady(Rider *rider)
    {
        if (!hasFlag(rider, kFlagModelChanging))
            return 1;
        const ee::Ptr<zge::SceneObject> model = rider->messenger;
        if (zge::updateObjectModel(model, 0u, 1u) != 1)
            return 0;
        zge::unbindObjectModel(rider->messenger);
        setUpLimbChains(rider);
        if (!hasFlag(rider, kFlagNoPlayer))
        {
            if (rider->softParts)
                zge::freeSoftPartsRig(rider->softParts);
            rider->softParts = zge::newSoftPartsRig(rider->messenger);
            if (rider->limbRig)
                zge::freeBodyRig(ee::Ptr<zge::BodyRig>(rider->limbRig));
            rider->limbRig = ee::Address(zge::newLimbRig(rider->messenger));
            if (riderModelHasPart(rider, kPartAspect, ee::Ptr<const char>(kNightVisionMale)) != 0 ||
                riderModelHasPart(rider, kPartAspect, ee::Ptr<const char>(kNightVisionFemale)) != 0)
                setFlag(rider, kFlagNightVision);
            else
                clearFlag(rider, kFlagNightVision);
        }
        clearFlag(rider, kFlagModelChanging);
        return 1;
    }
    READABLE_FUNCTION(riderModelReady, Verified);

    // freeRider (index): a rider's record let go at the level's end. A rider the camera follows first gives up
    // what only it has: its shooter, its bail body (parked far away, the models no longer carrying anything, the
    // bail's hands off whatever they held, its box taken out of the level's sweep, its blocks freed), its soft
    // parts' rig, its dynamics and its limbs' rig. Then the rolling sound stops, the bike's and the motions'
    // dynamics go, the bone mirrors and the two models are released, and the record is freed and its slot emptied.
    // DM2's, with BMX XXX's rigs, dynamics and shooter.
    void freeRider(uint32_t index)
    {
        index &= 0xFFu;
        const ee::Ptr<Rider> rider = riderOf(index);
        if (!hasFlag(rider.get(), kFlagNoPlayer))
        {
            freeShooter(ee::Ptr<Shooter>(rider->shooter).get());
            rider->bailBody->position.x = *g_parkedBodyCoordinate;
            rider->bailBody->position.y = *g_parkedBodyCoordinate;
            rider->bailBody->position.z = *g_parkedBodyCoordinate;
            zge::detachAnimal(rider->messenger);
            zge::detachAnimal(rider->bikeModel);
            clearFlag(rider.get(), kFlagBailHands);
            if (rider->hands[0].body != 0 || rider->hands[1].body != 0)
            {
                // (With one hand holding, the other's empty contact is let go too: ZGE4's removeBodyContact
                // returns at once with none.)
                zge::removeBodyContact(ee::Ptr<zge::RigidBody>(rider->hands[0].heldBody), ee::Ptr<zge::BodyContact>(rider->hands[0].body));
                zge::removeBodyContact(ee::Ptr<zge::RigidBody>(rider->hands[1].heldBody), ee::Ptr<zge::BodyContact>(rider->hands[1].body));
                rider->hands[0].body = 0;
                rider->hands[1].body = 0;
            }
            // The box's ends are re-sorted to the parked place (the far end of each axis), then cut off the axis
            // lists there.
            zge::sweepUpdateBody(levelSweep(), rider->bailBody);
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const ee::Ptr<zge::SweepEnd> low = rider->bailBody->sweepEntry->lowEnds[axis];
                if (low->prev)
                    low->prev->next = low->other->next;
                else
                    levelSweep()->axes[axis] = ee::Ptr<zge::SweepEnd>();
                zge::poolFree(ee::Ptr<zge::Pool>(levelSweep()->endPool), rider->bailBody->sweepEntry->lowEnds[axis]->other);
                zge::poolFree(ee::Ptr<zge::Pool>(levelSweep()->endPool), rider->bailBody->sweepEntry->lowEnds[axis]);
            }
            zge::heapFree(rider->bailBody->halfExtents);
            zge::heapFree(rider->bailBody->sweepEntry);
            zge::heapFree(rider->bailBody);
            if (rider->softParts)
                zge::freeSoftPartsRig(rider->softParts);
            rider->bailBody = ee::Ptr<zge::RigidBody>();
            zge::freeDynamicsCopy(rider->riderDynamics);
            if (rider->limbRig)
            {
                zge::freeBodyRig(ee::Ptr<zge::BodyRig>(rider->limbRig));
                rider->limbRig = 0;
            }
        }
        stopRollSound(rider->rollingVoice);
        zge::freeDynamicsCopy(rider->bikeDynamics);
        zge::freeDynamicsCopy(rider->riderAnimDynamics);
        zge::freeBoneMirror(rider->bikeMirror);
        zge::freeBoneMirror(rider->riderMirror);
        zge::releaseSceneModel(rider->bikeModel);
        zge::releaseSceneModel(rider->messenger);
        zge::heapFree(rider);
        kRiders[index] = ee::Ptr<Rider>();
    }
    READABLE_FUNCTION(freeRider, Verified);
}

namespace game::bmx::rider
{
    namespace calls
    {
        // libc's memset (no ported declaration for game code).
        inline void memset(ee::Address bytes, uint32_t value, uint32_t count) { ee::game(sym::memset, bytes, value, count); }
    }

    namespace
    {

        constexpr uint8_t kNoRiderSlot = 0xFF;
        constexpr uint8_t kNoRiderPlayer = 0xFF;
        constexpr uint32_t kRiderRecordBytes = sizeof(Rider);
        // NewRider::flags: the rider's model made by its name ("rider.NAME"); without it (every caller's request)
        // a "skeleton" the request's model record is bound to.
        constexpr uint8_t kRiderModelByName = 0x04;
        constexpr uint32_t kModelFlags = 1; // newSceneModelByName's first word, as the game gives it
        // A rider of no player's own tricks (tapTrick's 0x57 and 0x58).
        constexpr uint8_t kNoPlayerOwnTricks[2] = {12, 6};

        // The models' names: by the character's row ("rider.%s"), and by the bike's line and its number
        // ("bike.%s.%02d", from 1); the rider model's name as it is ("%s"), or "skeleton".
        struct CharacterRow
        {
            uint8_t _00[0x10];
            ee::Ptr<const char> modelName; // +0x10 "Mika"
        };
        static_assert(offsetof(CharacterRow, modelName) == 0x10 && sizeof(CharacterRow) == 0x14);
        const ee::Ptr<CharacterRow> kCharacterRows(sym::g_characterNames);
        struct BikeLine
        {
            uint8_t _00[4];
            ee::Ptr<const char> name; // +0x04 "Dirty"
        };
        static_assert(sizeof(BikeLine) == 0x08);
        const ee::Ptr<BikeLine> kBikeLineRows(sym::g_bikeLineNames);
        constexpr uint32_t kRiderModelName = sym::textRiderModelName;
        constexpr uint32_t kBikeModelName = sym::textBikeModelName;
        constexpr uint32_t kNameAsItIs = sym::textNameAsItIs;
        constexpr uint32_t kSkeletonModel = sym::textSkeletonModel;

        // The rider model's lean bones, and the bones the two models mirror by in left, right pairs (0 after the
        // last), as newRider copies them onto its frame.
        constexpr uint32_t kLeanBones = 17;
        inline ee::Global<ee::Ptr<const char>[kLeanBones], sym::g_leanBoneNames> g_leanBoneNames;
        inline ee::Global<ee::Ptr<const char>[23], sym::g_riderMirrorBoneNames> g_riderMirrorBoneNames;
        inline ee::Global<ee::Ptr<const char>[9], sym::g_bikeMirrorBoneNames> g_bikeMirrorBoneNames;

        // A player's bail body: a box of half extents 0.5 and mass 70, its record the rider's slot marked
        // 0xF0000000. The rider dynamics' first body made 0.56 tall; the bail's hands hold to it 0.14 to either
        // side and 0.55 down, the hold keyed by the hand's name's hash, with a slack of 1.
        constexpr ee::Float kBailBodyHalfExtent(0.5f);
        constexpr ee::Float kBailBodyMass(70.0f);
        constexpr uint32_t kBailBodyRecord = 0xF0000000u;
        constexpr uint32_t kSweepBody = 0;
        constexpr ee::Float kHoldBodyHalfHeight(0.56f);
        constexpr ee::Float kLeftHandReach(-0.14f), kRightHandReach(0.14f), kHandReachDown(0.55f);
        constexpr ee::Float kHandSlack(1.0f);
        constexpr uint32_t kLeftHandContact = sym::textLeftHandContact;   // "LEFTHAND"
        constexpr uint32_t kRightHandContact = sym::textRightHandContact; // "RIGHTHAND"
        constexpr uint32_t kLeftHandBone = sym::g_textLeftHand;           // "LeftHand"
        // The carried objects' tags of the rider's model and the bike's.
        constexpr uint32_t kCarriedRiderModel = 0, kCarriedBikeModel = 1;

        // g_riderDynamicsFiles' three: "dynamics.rider", "dynamics.riderAnim", "dynamics.bike".
        constexpr uint32_t kRiderFile = 0, kRiderAnimFile = 1, kBikeFile = 2;

        // A table of names copied onto the frame.
        template <size_t N>
        void copyNames(const ee::Local<ee::Ptr<const char>[N]> &to, const ee::Ptr<const char> (&from)[N])
        {
            for (size_t i = 0; i < N; ++i)
                to[i] = from[i];
        }
    }

    // riderProjectileEvent (projectile, message): the handler of a rider's own shooter (newRider's): the event
    // goes to riderHitByProjectile with the shooter's rider's player. playerWeaponProjectileEvent's twin.
    uint32_t riderProjectileEvent(ee::Ptr<Projectile> projectile, ee::Ptr<ProjectileMessage> message)
    {
        const ee::Ptr<Rider> rider = riderOf(static_cast<uint8_t>(projectile->shooter->ownerData));
        return riderHitByProjectile(rider->player, projectile, message);
    }
    READABLE_FUNCTION(riderProjectileEvent, Draft);

    namespace
    {

        // A projectile's touch of an object (kMessageProjectileTouch's data): taken, the projectile's velocity, the
        // shooter's player.
        struct ProjectileTouch
        {
            int32_t accept;        // +0x00
            uint8_t _04[0x0C];
            ee::Float velocity[4]; // +0x10
            uint32_t player;       // +0x20
            uint8_t _24[0x0C];
        };
        static_assert(sizeof(ProjectileTouch) == 0x30 && offsetof(ProjectileTouch, player) == 0x20);
        constexpr uint32_t kMessageProjectileTouch = 0x105;
        // A hit takes 86 of the rider's health, a blast 128 times its strength; the rider is thrown by a blast at
        // (its strength + 0.25) / 4 away from it and 1/15 up; either is a bail of kind 14. The rider's voice
        // (sound 37: kind 1, bank 3, female or not) and then 60 frames before it again.
        constexpr int32_t kHitHealth = 0x56;
        constexpr ee::Float kBlastHealth(128.0f);
        constexpr ee::Float kBlastThrowBase(0.25f);
        inline ee::Global<ee::Float, sym::g_blastThrowLift> g_blastThrowLift;
        constexpr uint8_t kBailShot = 14;
        constexpr uint32_t kSoundRiderVoice = 37;
        constexpr uint8_t kVoiceWaitAfterHit = 60;
        // The pad's rumble (the large motor): firing the first weapon 200, a blast 255.
        constexpr uint32_t kRumbleLargeMotor = 1;
        constexpr uint8_t kFireRumble = 200, kBlastRumble = 255;
        constexpr uint8_t kRumbledWeapon = 1;
        // Each player's bullet marks' colour.
        inline ee::Global<zge::VertexColour[2], sym::g_playerMarkColours> g_playerMarkColours;
    }

    // riderHitByProjectile (player, projectile, message): a projectile's event for a player's rider's shooter (the
    // game mode first says whether it counts: else 0). Fired: the first weapon rumbles the pad. A hit on a rider:
    // his health less 86, a bail. A hit on an object: the projectile's touch sent to it (unless the game takes
    // it: 0). A blast: the rider's health less by its strength, thrown away from it and up, a bail, the pad
    // rumbled. A hit or blast sounds the rider's voice (when not just heard) and sets kFlagDowned at no health. A
    // bullet's mark takes the player's colour. Returns 1.
    uint32_t riderHitByProjectile(uint32_t player, ee::Ptr<Projectile> projectile, ee::Ptr<ProjectileMessage> message)
    {
        ee::Local<zge::Vec4> away;
        ee::Local<ProjectileTouch> touch;
        ee::Local<zge::EngineMessage> engineMessage;
        ee::Local<calls::Rumble> rumble;
        ee::Local<zge::VertexColour[2]> markColours;
        markColours[0] = (*g_playerMarkColours)[0];
        markColours[1] = (*g_playerMarkColours)[1];
        if (!riderHitAllowed(static_cast<uint8_t>(player), ee::Address(projectile), message.get()))
            return 0;
        const auto shot = [&](Rider *rider) {
            if (rider->voiceWait == 0)
            {
                soundEvent(kSoundRiderVoice, rider->position, ee::Address(1u), 3u, riderIsFemale(rider));
                rider->voiceWait = kVoiceWaitAfterHit;
            }
            if (rider->health == 0 && !hasFlag(rider, kFlagDowned))
                setFlag(rider, kFlagDowned);
            return 1u;
        };
        switch (message->event)
        {
        case kProjectileFired:
            if (projectile->shooter->weapon == kRumbledWeapon)
            {
                rumble->motor = kRumbleLargeMotor;
                rumble->strength = kFireRumble;
                rumble->timing = 0;
                calls::rumble(player, rumble);
            }
            return 1;
        case kProjectileHitsRider:
        {
            Rider *rider = riderOf(static_cast<uint8_t>(message->subject)).get();
            const int32_t left = int32_t(rider->health) - kHitHealth;
            rider->health = static_cast<uint8_t>(left > -1 ? left : 0);
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailShot;
            return shot(rider);
        }
        case kProjectileHitsObject:
        {
            touch->accept = 1;
            *vec4(touch->velocity) = projectile->velocity;
            touch->player = player;
            engineMessage->target = message->subject;
            engineMessage->sender = ee::Address(riderOf(riderIndexOf(player))->messenger);
            engineMessage->type = kMessageProjectileTouch;
            engineMessage->flags = zge::kMessageNow;
            engineMessage->data = touch;
            if (riderActivatedObject(engineMessage) != 0)
                return 0;
            zge::sendEngineMessage(engineMessage);
            return 1;
        }
        case kProjectileBlastReachesRider:
        {
            Rider *rider = riderOf(static_cast<uint8_t>(message->subject)).get();
            const int32_t left = int32_t(rider->health) - ee::toInt(message->strength * kBlastHealth);
            rider->health = static_cast<uint8_t>(left > -1 ? left : 0);
            *away = *vec4(rider->position) + *vec4(rider->rootOffset);
            *away = *away - projectile->matrix.row(3);
            *away = zge::unit(*away);
            *away = *away * ((message->strength + kBlastThrowBase) * kBlastThrowBase);
            away->y = away->y - *g_blastThrowLift;
            *vec4(rider->velocity) = *vec4(rider->velocity) + *away;
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailShot;
            rumble->motor = kRumbleLargeMotor;
            rumble->strength = kBlastRumble;
            rumble->timing = 0;
            calls::rumble(player, rumble);
            return shot(rider);
        }
        case kProjectileMarksFace:
            message->colour = markColours[player];
            return 1;
        default:
            return 1;
        }
    }
    READABLE_FUNCTION(riderHitByProjectile, Verified);

    // newRider (request): a rider made in the first free of the 16 slots: its record zeroed, its rows the identity
    // turned upside down (up and forward the other way), the body's the same, at the origin; no player's rider
    // (kRiderNpc) gets kFlagNoPlayer. A player's rider takes the player's controller layout, its bike's stats
    // (setRiderBikeStats) and its character's own tricks. The head: a replay's (kRiderFromReplay), else the models'
    // names ("rider.NAME" by the player's character, "bike.LINE.NN"). The bike's set-up; the rider model (a
    // "skeleton" the request's model record is bound to: kFlagModelChanging, kFlagFemale by the model) and its lean
    // bones; the bike model; both bone mirrors; the riderAnim and bike dynamics with their contact handlers; the
    // rolling sound. A player's rider then: its limbs' chains, both models carried by it, the rider dynamics (its
    // first body alone) with its handler, the bail's body (its box, mass and the hands' holds on that body, in the
    // level's sweep), the soft parts' and limbs' rigs, its shooter. Returns the slot, or 0xFF (no slot, no memory,
    // a model, mirror or dynamics not made: what was made is freed). DM2's newRider with BMX XXX's models, dynamics,
    // rigs and shooter.
    uint8_t newRider(ee::Ptr<NewRider> request)
    {
        ee::Local<char[0x80]> modelText;
        ee::Local<ShooterOwner> owner;
        ee::Local<ee::Ptr<const char>[kLeanBones]> leanBones;
        ee::Local<ee::Ptr<const char>[23]> riderMirrorBones;
        ee::Local<ee::Ptr<const char>[9]> bikeMirrorBones;
        ee::Local<zge::Vec4> lowCorner;
        ee::Local<zge::Vec4> halfExtents;
        ee::Local<zge::Vec4> handReach;
        ee::Local<zge::Vec4> holdBox;
        copyNames(leanBones, *g_leanBoneNames);
        copyNames(riderMirrorBones, *g_riderMirrorBoneNames);
        copyNames(bikeMirrorBones, *g_bikeMirrorBoneNames);

        uint8_t slot = 0;
        if (kRiders[0])
        {
            do
            {
                slot = static_cast<uint8_t>(slot + 1);
                if (!(slot < kRiderSlots))
                    return kNoRiderSlot;
            } while (kRiders[slot]);
        }
        const ee::Ptr<Rider> rider(zge::heapAllocate(kRiderRecordBytes));
        if (!rider)
            return kNoRiderSlot;
        zge::memoryFill(rider, 0, kRiderRecordBytes);
        rider->player = kNoRiderPlayer;
        zge::matrixIdentity(matrix(rider->right));
        zge::vec3Negate(&vec4(rider->up)->xyz(), &vec4(rider->up)->xyz());
        zge::vec3Negate(&vec4(rider->forward)->xyz(), &vec4(rider->forward)->xyz());
        *matrix(rider->body) = *matrix(rider->right);
        *vec4(rider->position) = zge::kZero;
        *vec4(rider->drawnPosition) = zge::kZero;
        if (request->flags & kRiderNpc)
            setFlag(rider.get(), kFlagNoPlayer);
        else
            clearFlag(rider.get(), kFlagNoPlayer);

        // A player's: the controller layout, the bike's stats, the character's own tricks.
        ee::Ptr<PlayerSetup> setup;
        if (!(request->flags & kRiderNpc))
        {
            setup = playerSetup(request->player);
            const ee::Ptr<RiderCharacter> character = playerCharacter(request->player);
            rider->player = request->player;
            rider->controllerLayout = setup->controllerLayout;
            setRiderBikeStats(rider.get(), request->characterBike[0], request->characterBike[1]);
            rider->ownTricks[0] = character->ownTricks[0];
            rider->ownTricks[1] = character->ownTricks[1];
        }
        else
        {
            rider->ownTricks[0] = kNoPlayerOwnTricks[0];
            rider->ownTricks[1] = kNoPlayerOwnTricks[1];
        }

        // The head: a replay's, else the models' names (no player's rider with no replay head reads the character
        // through no setup: address 3, as the original does).
        if (request->flags & kRiderFromReplay)
        {
            zge::memoryCopy(rider, &request->riderModel, kReplayRiderHeadBytes);
        }
        else
        {
            zge::formatString(rider->riderModelName, kRiderModelName, kCharacterRows[setup->character].modelName);
            zge::formatString(rider->bikeModelName, kBikeModelName, kBikeLineRows[request->characterBike[0]].name,
                              static_cast<uint32_t>(request->characterBike[1]) + 1u);
        }

        // The bike's set-up.
        rider->barsTilt = -0xAAA;
        rider->massHeight = ee::Float(0.5f);
        rider->mass = ee::Float(5.0f);
        rider->springMass = ee::Float(0.2f);
        rider->slip = ee::Float(1.0f);
        rider->backWheelHeight = ee::Float(0.24645f);
        rider->grip = ee::Float(1.0f);
        rider->frontWheelHeight = ee::Float(0.24645f);
        rider->frontWheelPoint[0] = ee::Float(0.0f);
        rider->frontWheelPoint[2] = ee::Float(0.55253f);
        rider->frontWheelPoint[1] = ee::Float(-0.05596f);
        rider->backWheelPoint[0] = ee::Float(0.0f);
        rider->backWheelPoint[2] = ee::Float(-0.3652f);
        rider->backWheelPoint[1] = ee::Float(-0.05596f);
        // (The seat grip is set twice: (0, -0.4955, 0.2463), then the y and z below.)
        rider->seatGrip[0] = ee::Float(0.0f);
        rider->seatGrip[1] = ee::Float(-0.4955f);
        rider->seatGrip[2] = ee::Float(0.2463f);
        rider->seatGrip[0] = ee::Float(0.0f);
        rider->seatGrip[2] = ee::Float(0.05f);
        rider->seatGrip[1] = ee::Float(-0.23f);
        rider->grips[0][0] = ee::Float(0.2846f);
        rider->grips[0][2] = ee::Float(-0.09847f);
        rider->grips[0][1] = ee::Float(-0.3755f);
        rider->grips[1][0] = ee::Float(-0.2846f);
        rider->grips[1][2] = ee::Float(-0.09847f);
        rider->grips[1][1] = ee::Float(-0.3755f);
        rider->wheelHeightLeast = ee::Float(0.0f);
        rider->wheelHeightMost = ee::Float(0.0f);
        rider->springStiffness = ee::Float(0.0f);
        rider->springDamping[0] = ee::Float(0.0f);
        rider->springDamping[1] = ee::Float(0.0f);

        // The rider model: a skeleton bound to the request's model record (or by its name), and its lean bones.
        zge::formatString(modelText, kNameAsItIs, rider->riderModelName);
        if (!(request->flags & kRiderModelByName))
            zge::formatString(modelText, kNameAsItIs, kSkeletonModel);
        rider->messenger = zge::newSceneModelByName(kModelFlags, modelText);
        if (!rider->messenger)
        {
            zge::heapFree(rider);
            return kNoRiderSlot;
        }
        zge::setNodeCollides(zge::sceneRootNode(zge::sceneOf(rider->messenger)), 0, 1);
        if (!(request->flags & kRiderModelByName))
        {
            rider->model = request->model;
            zge::bindModelToObject(rider->messenger, request->model);
            setFlag(rider.get(), kFlagModelChanging);
            if (modelIsFemale(rider->model))
                setFlag(rider.get(), kFlagFemale);
            else
                clearFlag(rider.get(), kFlagFemale);
        }
        for (uint32_t bone = 0; bone < kLeanBones; ++bone)
        {
            const uint32_t name = zge::nameHash(leanBones[bone].get());
            const ee::Ptr<zge::Scene> scene = zge::sceneOf(rider->messenger);
            if (zge::findSceneModelBoneThunk(scene, name)) // (BMX XXX's game asks through the engine's thunk)
            {
                rider->leanWeights[0][bone].bone = name;
                rider->leanWeights[1][bone].bone = name;
            }
        }
        rider->leanWeightLists[0].weights = &rider->leanWeights[0][0];
        rider->leanWeightLists[1].count = kLeanBones;
        rider->leanWeightLists[1].weights = &rider->leanWeights[1][0];
        rider->leanWeightLists[0].count = kLeanBones;

        // The bike model, the mirrors, the riderAnim and bike dynamics with their contact handlers; one not made
        // frees what was (the last made first).
        enum Made { kMadeRiderModel, kMadeBikeModel, kMadeRiderMirror, kMadeBikeMirror };
        const auto giveUp = [&](Made made) {
            if (made >= kMadeBikeMirror)
                zge::freeBoneMirror(rider->bikeMirror);
            if (made >= kMadeRiderMirror)
                zge::freeBoneMirror(rider->riderMirror);
            if (made >= kMadeBikeModel)
                zge::releaseSceneModel(rider->bikeModel);
            zge::releaseSceneModel(rider->messenger);
            zge::heapFree(rider);
            return kNoRiderSlot;
        };
        rider->bikeModel = zge::newSceneModelByName(kModelFlags, rider->bikeModelName);
        if (!rider->bikeModel)
            return giveUp(kMadeRiderModel);
        zge::setNodeCollides(zge::sceneRootNode(zge::sceneOf(rider->bikeModel)), 0, 1);
        rider->riderMirror = ee::Ptr<zge::BoneMirror>(zge::newBoneMirror(rider->messenger, ee::Ptr<zge::BoneNamePair>(riderMirrorBones.address())));
        if (!rider->riderMirror)
            return giveUp(kMadeBikeModel);
        rider->bikeMirror = ee::Ptr<zge::BoneMirror>(zge::newBoneMirror(rider->bikeModel, ee::Ptr<zge::BoneNamePair>(bikeMirrorBones.address())));
        if (!rider->bikeMirror)
            return giveUp(kMadeRiderMirror);
        rider->riderAnimDynamics = zge::copyDynamicsDefinition(ee::Address(rider->messenger), (*g_riderDynamicsFiles)[kRiderAnimFile].definition);
        if (!rider->riderAnimDynamics)
            return giveUp(kMadeBikeMirror);
        rider->riderAnimDynamics->contactCallback = zge::ContactCallback(sym::riderAirMotionsContact);
        rider->riderAnimDynamics->owner = ee::Address(rider);
        rider->bikeDynamics = zge::copyDynamicsDefinition(ee::Address(rider->bikeModel), (*g_riderDynamicsFiles)[kBikeFile].definition);
        if (!rider->bikeDynamics)
            return giveUp(kMadeBikeMirror);
        rider->bikeDynamics->contactCallback = zge::ContactCallback(sym::bikeDynamicsContact);
        rider->bikeDynamics->owner = ee::Address(rider);
        rider->rollingVoice = static_cast<uint32_t>(startRollSound());

        // A player's rider: its limbs, both models carried by it, the rider dynamics, the bail's body, the rigs and
        // the shooter.
        if (!hasFlag(rider.get(), kFlagNoPlayer))
        {
            calls::memset(handReach, 0, sizeof(zge::Vec4));
            setUpLimbChains(rider.get());
            zge::attachCarriedObject(rider, rider->messenger, {}, {}, {}, {});
            zge::attachCarriedObject(rider, rider->bikeModel, {}, {}, {}, {});
            zge::setCarriedAnimalTag(ee::Address(rider->messenger), kCarriedRiderModel);
            zge::setCarriedAnimalTag(ee::Address(rider->bikeModel), kCarriedBikeModel);
            rider->riderDynamics = zge::copyDynamicsFirstBody(ee::Address(rider->messenger), (*g_riderDynamicsFiles)[kRiderFile].definition);
            if (rider->riderDynamics)
            {
                rider->riderDynamics->contactCallback = zge::ContactCallback(sym::riderDynamicsContact);
                rider->riderDynamics->owner = ee::Address(rider);
                // The bail's body: a box of half extents 0.5 (the engine's newDynamicsForce makes its record: the
                // same record), its mass 70.
                const ee::Ptr<zge::RigidBody> body(zge::heapAllocate(zge::kRigidBodyBytes));
                rider->bailBody = body;
                body->shape = 0;
                halfExtents->z = kBailBodyHalfExtent;
                halfExtents->x = kBailBodyHalfExtent;
                halfExtents->y = kBailBodyHalfExtent;
                body->halfExtents = extentsOf(zge::newDynamicsForce(halfExtents));
                body->mass = kBailBodyMass;
                // The hands hold on to the rider dynamics' one body, made 0.56 tall.
                if (const ee::Ptr<zge::RigidBody> held = rider->riderDynamics->bodies)
                {
                    zge::boxHalfExtents(held, holdBox);
                    holdBox->y = kHoldBodyHalfHeight;
                    zge::setBoxHalfExtents(held, holdBox);
                    const auto leftHandBone = [&] {
                        const ee::Ptr<zge::Scene> scene = zge::sceneOf(rider->messenger);
                        return zge::findSceneModelBoneThunk(scene, zge::nameHash(ee::pointerAt<char>(kLeftHandBone)));
                    };
                    handReach->x = kLeftHandReach;
                    handReach->y = kHandReachDown;
                    handReach->z = ee::Float(0.0f);
                    const ee::Ptr<zge::RigidBody> leftKey(zge::nameHash(ee::pointerAt<char>(kLeftHandContact)));
                    rider->hands[0].body = ee::Address(zge::newBodyContactRecordWithSlack(held, handReach, handReach, leftKey, kHandSlack));
                    rider->hands[0].heldBody = ee::Address(held);
                    rider->hands[0].bone = leftHandBone();
                    handReach->x = kRightHandReach;
                    handReach->y = kHandReachDown;
                    handReach->z = ee::Float(0.0f);
                    const ee::Ptr<zge::RigidBody> rightKey(zge::nameHash(ee::pointerAt<char>(kRightHandContact)));
                    rider->hands[1].body = ee::Address(zge::newBodyContactRecordWithSlack(held, handReach, handReach, rightKey, kHandSlack));
                    rider->hands[1].heldBody = ee::Address(held);
                    // (The right hand's bone: the original looks up "LeftHand" again, into the left hand's: bug kept.)
                    rider->hands[0].bone = leftHandBone();
                }
                // Its inertia the mass's (the rows times it, the inverse's over it), in the world too.
                zge::matrixIdentity(&body->inertia);
                zge::matrixIdentity(&body->inverseInertia);
                for (uint32_t row = 0; row < 3; ++row)
                    body->inertia.row(row) *= body->mass;
                for (uint32_t row = 0; row < 3; ++row)
                    body->inverseInertia.row(row) *= ee::Float(1.0f) / body->mass;
                body->worldInertia = body->inertia;
                body->worldInverseInertia = body->inverseInertia;
                // Its box in the level's sweep; its record the slot's.
                zge::vec3Negate(&lowCorner->xyz(), &halfExtents->xyz());
                const ee::Ptr<zge::Sweep> sweep = levelSweep();
                body->sweepEntry = zge::sweepAddBody(sweep, kSweepBody, body, ee::Ptr<const ee::Float>(lowCorner.address()),
                                                     ee::Ptr<const ee::Float>(halfExtents.address()));
                body->record = slot | kBailBodyRecord;
                // The rigs: the soft parts', the limbs' (its hands' points not kinds yet).
                rider->softParts = zge::newSoftPartsRig(rider->messenger);
                const ee::Ptr<zge::BodyRig> limbs = zge::newLimbRig(rider->messenger);
                rider->limbRig = ee::Address(limbs);
                if (limbs)
                {
                    limbs->leftHand->kind = 0;
                    limbs->rightHand->kind = 0;
                }
                // The shooter, its projectiles' events to riderProjectileEvent (the owner's flags are not set: the
                // frame's word as it is).
                owner->buttons = ee::Ptr<ButtonRecord>(&rider->input);
                owner->handler = ProjectileHandler(sym::riderProjectileEvent);
                owner->data = slot;
                rider->shooter = ee::Address(newShooter(owner));
            }
        }
        kRiders[slot] = rider;
        return slot;
    }
    READABLE_FUNCTION(newRider, Verified);

    namespace
    {
        // The ride distance's units a unit of distance (0.45), and the most one player's ride counts (30).
        inline ee::Global<ee::Float, sym::g_rideDistanceScale> g_rideDistanceScale;
        constexpr int32_t kOnePlayerMostRide = 30;
        constexpr uint8_t kSessionParkTest = 4; // g_sessionKind: a park's test from the editor
    }

    // addRideDistance (rider, distance): the ride distance += distance x 0.45, whole (none in a park's test; one
    // player: never past 30 in all, never less); the same taken off the rider's health (not below 0), which at 0
    // sets kFlagDowned. DM2's, with the cap and the count.
    void addRideDistance(Rider *rider, ee::Float distance)
    {
        if (*g_sessionKind == kSessionParkTest)
            return;
        int32_t ridden = ee::toInt(distance * *g_rideDistanceScale);
        if (*g_playerCount == 1)
        {
            const ee::Float room = ee::toFloat(kOnePlayerMostRide - rider->rideDistance);
            ridden = ee::toInt(ee::toFloat(ridden) < room ? ee::toFloat(ridden) : room);
            ridden = ee::toInt(ee::Float(0.0f) < ee::toFloat(ridden) ? ee::toFloat(ridden) : ee::Float(0.0f));
        }
        rider->rideDistance += ridden;
        const int32_t left = int32_t(rider->health) - ridden;
        rider->health = static_cast<uint8_t>(left > -1 ? left : 0);
        if (rider->health == 0 && !hasFlag(rider, kFlagDowned))
            setFlag(rider, kFlagDowned);
    }
    READABLE_FUNCTION(addRideDistance, Verified);

    // riderIsFemale (rider): 1 when the rider's model is a female rider's (kFlagFemale).
    uint32_t riderIsFemale(const Rider *rider)
    {
        return hasFlag(rider, kFlagFemale) ? 1u : 0u;
    }
    READABLE_FUNCTION(riderIsFemale, Verified);

    // physicsShutdown (physics.c): the level's end for the physics: every rider still in a record freed, the three
    // dynamics files' definitions freed, then the physics task deleted (whose result, 1, it answers). DM2's, with
    // the dynamics.
    uint32_t physicsShutdown()
    {
        for (uint32_t index = 0; index < kRiderSlots; ++index)
        {
            if (kRiders[index])
                freeRider(index & 0xFFu);
        }
        freeRiderDynamics();
        return game::tail(deletePhysicsTask);
    }
    READABLE_FUNCTION(physicsShutdown, Verified);
}
