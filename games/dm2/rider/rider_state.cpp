// DM2's rider records and action state levels (physics.c; action.c): the
// setup, the record by index, the four action state levels and a ride stat.
//
// Counterparts: DM2 GameCube, DM2 Xbox and
// BMX XXX PS2.

#include "rider.h"
#include "../game/players.h"
#include "../action/rider_files.h"
#include "../menu/player_creator.h"
#include "../level/level_collision.h"
#include "../sound/sound_events.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/scene/animation.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "sym/dm2/rider/rider_state.cpp.h"

namespace game::dm2
{
    void setCameraMode(uint32_t player, uint32_t level, uint32_t mode); // camera/camera.cpp
}

namespace game::dm2::rider
{
    void physicsInit();                         // physics_state.cpp
    void setUpLimbChains(Rider *rider);         // limb_chains.cpp
    void clearTaps(Rider *rider, uint32_t keepStick); // input.cpp

    namespace
    {
        // A dynamics force record doubles as a bail body's half extents (its vector is the record's first field).
        ee::Ptr<zge::BodyExtents> extentsOf(ee::Ptr<zge::DynamicsForce> force) { return force.cast<zge::BodyExtents>(); }

        // The rider records by index (16 slots).
        inline ee::Global<ee::Ptr<Rider>[16], sym::g_riders> g_riders;

        // Where freeRider leaves the bail body before its box leaves the
        // sweep: far out on every axis.
        constexpr ee::Float kParkedBodyCoordinate = 10000.0f;
        // The last four characters' model is a scene object of the rider's
        // own, deleted with the rider (which characters these are: not read
        // yet; the others' models are only released).
        constexpr uint8_t kFirstOwnObjectCharacter = 17;
        constexpr uint8_t kOwnObjectCharacters = 4;

        // The bones' name hashes, set up once (the model's bone names).
        struct BoneName
        {
            uint32_t name;   // the string
            uint32_t global; // where its hash goes
        };
        constexpr BoneName kBoneNames[] = {
            {sym::g_textHips, sym::g_hipsBoneHash}, // "Hips"
            {sym::g_textHead, sym::g_headBoneHash}, // "Head"
            {sym::g_textLeftHand, sym::g_leftHandBoneHash}, // "LeftHand"
            {sym::g_textRightHand, sym::g_rightHandBoneHash}, // "RightHand"
            {sym::g_textLeftFoot, sym::g_leftFootBoneHash}, // "LeftFoot"
            {sym::g_textRightFoot, sym::g_rightFootBoneHash}, // "RightFoot"
            {sym::g_textLeftLowArm, sym::g_leftLowArmBoneHash}, // "LeftLowArm"
            {sym::g_textRightLowArm, sym::g_rightLowArmBoneHash}, // "RightLowArm"
            {sym::g_textHipsdeck, sym::g_hipsDeckBoneHash}, // "Hipsdeck"
            {sym::g_textHandlebars, sym::g_handlebarsBoneHash}, // "Handlebars"
            {sym::g_textSprocket, sym::g_sprocketBoneHash}, // "Sprocket"
            {sym::g_textFrontWheel, sym::g_frontWheelBoneHash}, // "FrontWheel"
            {sym::g_textBackWheel, sym::g_backWheelBoneHash}, // "BackWheel"
        };
        constexpr uint32_t kMotion00 = sym::motion00; // "Motion00"
        inline ee::Global<ee::Ptr<zge::LoadedAsset>, sym::g_riderMotionGroup> g_riderMotionGroup;
        inline ee::Global<uint32_t, sym::g_riderMotionCount> g_riderMotionCount; // (inferred: what listMotions returns)
        // The riders' motions (0x218 bytes).
        inline ee::Global<ee::Ptr<zge::Motion>, sym::riderMotions> g_riderMotions;
        constexpr uint32_t kRiderMotionsSize = 0x218;

        // A frame's ride distance unit: 25 / 3 (8.333).
        inline ee::Global<ee::Float, sym::g_rideDistanceScale> g_rideDistanceScale;
    }

    // Once at start: the bone name hashes, the riders' motions (Motion00),
    // the physics task, the rider records cleared.
    void riderSetup()
    {
        for (const BoneName &bone : kBoneNames)
            *ee::Ptr<uint32_t>(bone.global) = zge::nameHash(ee::pointerAt<char>(bone.name));
        *g_riderMotionGroup = zge::loadSceneAnimations(0, kMotion00); // the animations of that name
        *g_riderMotionCount = zge::listMotions(*g_riderMotionGroup, g_riderMotions.get(), kRiderMotionsSize);
        physicsInit();
        zge::memoryFill(g_riders.get(), 0, sizeof(*g_riders));
    }
    READABLE_FUNCTION(riderSetup, Verified);

    // The rider record of a rider index.
    ee::Ptr<Rider> riderOf(uint32_t index)
    {
        return (*g_riders)[index & 0xFFu];
    }
    READABLE_FUNCTION(riderOf, Verified);

    // The rider whose model or bike model is this engine object, or 255 for
    // none (AI's by the level's two players; DM2 searches its 16 records).
    uint32_t playerIndexOfNode(ee::Address node)
    {
        for (uint32_t index = 0; index < 16; ++index)
        {
            const ee::Ptr<Rider> rider = (*g_riders)[index];
            if (rider && (rider->messenger == node || rider->bikeModel == node))
                return index;
        }
        return 255;
    }
    READABLE_FUNCTION(playerIndexOfNode, Verified);

    // freeRider (index): a rider's record let go at the level's end. A rider
    // with a bail body (every rider but one with kFlagNoPlayer set) first
    // gives that up: the body parked far away, the bail's hands off whatever
    // they held, its box taken out of the level's sweep and its blocks
    // freed. Then the rolling sound stops, the bone mirrors and the two
    // models are released, a created rider's scene object is deleted, and
    // the record is freed and its slot emptied.
    void freeRider(uint32_t index)
    {
        index &= 0xFFu;
        const ee::Ptr<Rider> rider = riderOf(index);
        if (!(rider->flags & kFlagNoPlayer))
        {
            const ee::Ptr<zge::RigidBody> body = rider->bailBody;
            body->position.x = kParkedBodyCoordinate;
            body->position.y = kParkedBodyCoordinate;
            body->position.z = kParkedBodyCoordinate;
            rider->flags &= ~kFlagBailHands;
            for (BailHand &hand : rider->hands)
            {
                if (hand.body)
                    zge::removeBodyContact(ee::Ptr<zge::RigidBody>(hand.heldBody), ee::Ptr<zge::BodyContact>(hand.body));
            }
            // The box's ends are re-sorted to the parked place (the far end
            // of each axis), then cut off the axis lists there.
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
            rider->bailBody = ee::Ptr<zge::RigidBody>();
        }
        stopRollSound(rider->rollingVoice);
        zge::heapFree(rider->bikeMirror);
        zge::heapFree(rider->riderMirror);
        zge::releaseSceneModel(ee::Ptr<zge::SceneObject>(rider->bikeModel));
        zge::releaseSceneModel(ee::Ptr<zge::SceneObject>(rider->messenger));
        const uint8_t character = (*g_playerSetups)[rider->player].character;
        if (character >= kFirstOwnObjectCharacter && character < kFirstOwnObjectCharacter + kOwnObjectCharacters)
            zge::deleteSceneObject(ee::Ptr<zge::SceneObject>(rider->messenger));
        zge::heapFree(rider);
        (*g_riders)[index] = ee::Ptr<Rider>();
    }
    READABLE_FUNCTION(freeRider, Verified);

    // A rider motion by index (riderSetup lists them).
    ee::Ptr<zge::Motion> riderMotion(uint32_t index)
    {
        return ee::Ptr<ee::Ptr<zge::Motion>>(g_riderMotions.get())[index];
    }
    READABLE_FUNCTION(riderMotion, Verified);

    // An action state level's state.
    uint8_t actionState(const Rider *rider, uint32_t level)
    {
        return rider->actionStates[level];
    }
    READABLE_FUNCTION(actionState, Verified);

    // Sets an action state level: the deeper levels back to 0, the frame
    // counters from 0, the motion's rate back to 1. Entering a level-0
    // state resets what the last one left (taps, tricks, flags) and the
    // camera; levels 0 and 1 also clear the bars' lean. Returns the level's
    // state. (The GameCube's has one test more.)
    uint8_t setActionState(Rider *rider, uint32_t level, uint32_t state)
    {
        rider->actionStates[level] = static_cast<uint8_t>(state);
        rider->actionFrames[level] = 0;
        if (level + 1 < kActionLevels)
        {
            for (uint32_t deeper = level + 1; deeper < kActionLevels; ++deeper)
            {
                rider->actionStates[deeper] = 0;
                rider->actionFrames[deeper] = 0;
            }
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
                rider->flags = rider->flags & ~kFlagManual & ~kFlagNoseWheelie & ~kFlagTrickTurn;
                rider->trickMotion = kNoTrick;
                rider->trick = kNoTrick;
                rider->trickLoops = 0;
                break;
            case kActionGrind:
                rider->balanceFrames = 0;
                [[fallthrough]];
            case kActionStall:
            case kActionLipTrick:
                rider->flags &= ~kFlagFellOff;
                break;
            case kActionLanding:
                return static_cast<uint8_t>(state);
            default: // riding, the wheelies, bail
                rider->flags &= ~kFlagMirrored;
                clearTaps(rider, 1u);
                rider->flags = rider->flags & ~kFlagTrickTurn & ~kFlagLeftRail;
                rider->trickMotion = kNoTrick;
                rider->trick = kNoTrick;
                rider->trickLoops = 0;
                break;
            }
            rider->yawTurned = 0;
            if (!(rider->flags & kFlagNoPlayer))
                setCameraMode(uint32_t(rider->player), 1u, 0u);
        }
        else if (level != 1)
        {
            rider->wheelDrag = 0;
            return rider->actionStates[level];
        }
        rider->trickTurned = 0;
        rider->barLean[0] = 0.0f;
        rider->flags = rider->flags & ~kFlagTopTrick & ~kFlagIdle;
        rider->barLean[1] = 0.0f;
        rider->wheelDrag = 0;
        return rider->actionStates[level];
    }
    READABLE_FUNCTION(setActionState, Verified);

    // The ride distance += distance x 25 / 3, whole (a ride stat; inferred).
    void addRideDistance(Rider *rider, ee::Float distance)
    {
        rider->rideDistance += ee::toInt(distance * *g_rideDistanceScale);
    }
    READABLE_FUNCTION(addRideDistance, Verified);
}

namespace game::dm2::rider
{
    namespace
    {
        // Scaffolding (address-call): libc's memset (no ported declaration
        // for game code).
        namespace calls
        {
            void memset(ee::Address bytes, uint32_t value, uint32_t count) { ee::game(sym::memset, bytes, value, count); }
        }

        constexpr uint32_t kRiderSlots = 16;
        constexpr uint8_t kNoRiderSlot = 0xFF;
        constexpr uint8_t kNoRiderPlayer = 0xFF;
        constexpr uint32_t kRiderRecordBytes = 0x1680;
        constexpr uint32_t kRiderHeadBytes = sizeof(RiderHead);

        // The characters made in the front end's rider creation (17 .. 20):
        // their model is the rider's own object, which newRider does not make.
        constexpr bool createdCharacter(uint8_t character)
        {
            return static_cast<uint8_t>(character - kBuiltInCharacters) < kCharacters - kBuiltInCharacters;
        }
        // The bike model of each built-in character (its own number).
        inline ee::Global<uint8_t[kCharacters], sym::g_characterBikeModels> g_characterBikeModels;
        constexpr uint8_t kLastBike = 3;

        // The eight stats' least and most (the stat's level 0 and 12): a
        // character's level in a stat is its bar points and its bike's.
        struct StatRange
        {
            ee::Float least, most;
        };
        inline ee::Global<StatRange[8], sym::g_riderStatRanges> g_riderStatRanges;
        constexpr uint32_t kStatLevels = 12;
        constexpr float kStatLevelShare = 1.0f / 12.0f;
        constexpr uint32_t kBikesPerModel = 5, kStats = 8;
        uint8_t bikeStatBars(uint32_t bikeModel, uint32_t bike, uint32_t stat)
        {
            return ee::Ptr<uint8_t>(sym::g_bikeStatBars)[stat + bike * kStats + bikeModel * (kBikesPerModel * kStats)];
        }
        // A rider of no player's own tricks (tapTrick's 0x57 and 0x58).
        constexpr uint8_t kNoPlayerOwnTricks[2] = {12, 6};

        // The models' names.
        constexpr ee::Ptr<const char> kRiderModelFormat{sym::riderModelFormat};             // "Rider%02d"
        constexpr ee::Ptr<const char> kRiderOutfitModelFormat{sym::riderOutfitModelFormat}; // "Rider%02dC"
        constexpr ee::Ptr<const char> kCreatedRiderModel{sym::createdRiderModelName};       // "Rider00"
        constexpr ee::Ptr<const char> kBikeModelFormat{sym::riderBikeModelFormat};          // "Bike%02d_%d"
        constexpr uint32_t kModelFlags = 1;

        // The rider model's lean bones, and the bones the two models mirror
        // by in left, right pairs (0 after the last), as newRider copies
        // them onto its frame.
        constexpr uint32_t kLeanBones = 16;
        inline ee::Global<ee::Ptr<const char>[kLeanBones], sym::leanBoneNames> g_leanBoneNames;
        inline ee::Global<ee::Ptr<const char>[21], sym::riderMirrorBoneNames> g_riderMirrorBoneNames;
        inline ee::Global<ee::Ptr<const char>[9], sym::bikeMirrorBoneNames> g_bikeMirrorBoneNames;

        // The rider model's and the bike's dynamics files: "FORM" chunks.
        constexpr uint32_t kFormTag = 0x4D524F46u;
        // How much of a contact's speed the dynamics' bodies keep: 0.4, or
        // 2.5 in a multiplayer game with the settings' bit 2.
        // Scaffolding (unnamed-field): what sets that bit is not read yet.
        inline ee::Global<uint16_t, sym::g_settingFlags> g_settingFlags;
        constexpr uint16_t kSettingFlag4 = 0x4;
        constexpr float kRestitution = 0.4f, kSettingRestitution = 2.5f;

        // The bail's body: a box of half extents 0.5 and mass 70, its record
        // the rider's slot marked 0xF0000000. Its hands hold to the rider
        // model's lower arms' bodies 0.2 out along x, the hold keyed by the
        // hand's name's hash.
        inline ee::Global<uint32_t, sym::g_leftLowArmBoneHash> g_leftLowArmBone;
        inline ee::Global<uint32_t, sym::g_rightLowArmBoneHash> g_rightLowArmBone;
        constexpr ee::Ptr<const char> kLeftHandName{sym::leftHandContactName};   // "LEFTHAND"
        constexpr ee::Ptr<const char> kRightHandName{sym::rightHandContactName}; // "RIGHTHAND"
        constexpr float kHandReach = 0.2f;
        constexpr float kBailBodyHalfExtent = 0.5f, kBailBodyMass = 70.0f;
        constexpr uint32_t kBailBodyRecord = 0xF0000000u;
        constexpr uint32_t kBoxBody = 0, kSweepBody = 0;

        // A table of names copied onto the frame.
        template <size_t N>
        void copyNames(const ee::Local<ee::Ptr<const char>[N]> &to, const ee::Ptr<const char> (&from)[N])
        {
            for (size_t i = 0; i < N; ++i)
                to[i] = from[i];
        }

        // The rider's dynamics files read into a dynamics record (0: none).
        uint32_t readDynamics(uint32_t data, uint32_t bytes, ee::Ptr<uint32_t> dynamics)
        {
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(data, bytes);
            const ee::Ptr<zge::ChunkReader> chunks(zge::openChunkFile(file, kFormTag, 0));
            *dynamics = 0;
            zge::parseChunks(chunks.get(), dynamics);
            zge::closeChunkFile(chunks.get());
            zge::closeFile(file, 0);
            return *dynamics;
        }
    }

    // newRider (request): a rider made in the first free of the 16 slots:
    // its record zeroed; a player's rider takes its character's stats (the
    // character's level in each, with its bike's, up to 12 of the way from
    // the stat's least to its most), own tricks and tricks off, and the
    // player's bike; the models' names (a head's, or "Rider%02d" by the
    // character, "Bike%02d_%d"); the bike's set-up (its suspension, grips,
    // wheels); the rider model (its lean bones) and the bike model; both
    // dynamics (rider_files.cpp's files) with their contact handlers; both
    // bone mirrors; the limbs' chains; a rolling sound; a player's rider its
    // bail's body, hands and sweep box. Returns the slot, or 0xFF (no slot,
    // no memory, a model, dynamics or mirror not made: what was made is
    // freed; the dynamics are not). A created character's rider (17 .. 20)
    // gets its model from its look once Create Player has started (its
    // outfits read: g_playerCreatorOutfits); the shipped game never starts
    // it, so such a rider is never made there.
    uint8_t newRider(ee::Ptr<NewRider> request)
    {
        ee::Local<ee::Ptr<const char>[kLeanBones]> leanBones;
        ee::Local<ee::Ptr<const char>[21]> riderMirrorBones;
        ee::Local<ee::Ptr<const char>[9]> bikeMirrorBones;
        ee::Local<zge::Vec4> handReach;
        ee::Local<zge::Vec4> halfExtents;
        ee::Local<zge::Vec4> lowCorner;
        ee::Local<uint32_t> fileBytes;
        copyNames(leanBones, *g_leanBoneNames);
        copyNames(riderMirrorBones, *g_riderMirrorBoneNames);
        copyNames(bikeMirrorBones, *g_bikeMirrorBoneNames);

        uint8_t slot = 0;
        if ((*g_riders)[0])
        {
            do
            {
                slot = static_cast<uint8_t>(slot + 1);
                if (!(slot < kRiderSlots))
                    return kNoRiderSlot;
            } while ((*g_riders)[slot]);
        }
        const ee::Ptr<Rider> rider(zge::heapAllocate(kRiderRecordBytes));
        if (!rider)
            return kNoRiderSlot;
        zge::memoryFill(rider, 0, kRiderRecordBytes);
        rider->player = kNoRiderPlayer;
        rider->flags &= ~kFlagBailHands;
        rider->hands[0].holding = 0;
        rider->hands[1].holding = 0;

        // A player's: its character's stats and tricks, its bike.
        ee::Ptr<PlayerSetup> setup;
        if (!(request->flags & kRiderNoPlayer))
        {
            setup = ee::Ptr<PlayerSetup>(sym::g_playerSetups) + request->player;
            const ee::Ptr<RiderCharacter> character = playerCharacter(request->player);
            rider->player = request->player;
            rider->controllerLayout = setup->controllerLayout;
            if (!createdCharacter(setup->character))
            {
                request->bikeModel = (*g_characterBikeModels)[setup->character];
                request->bike = setup->bike;
            }
            for (uint32_t stat = 0; stat < kStats; ++stat)
            {
                const StatRange &range = (*g_riderStatRanges)[stat];
                uint32_t level = character->stats[stat] + bikeStatBars(character->bikeModel, character->bike, stat);
                if (!(static_cast<int32_t>(level) < static_cast<int32_t>(kStatLevels) + 1))
                    level = kStatLevels;
                rider->stat(stat) =
                    range.least + ee::toFloat(static_cast<int32_t>(level)) * (range.most - range.least) * ee::Float(kStatLevelShare);
            }
            rider->ownTricks[0] = character->tricks[0];
            rider->ownTricks[1] = character->tricks[1];
            zge::memoryCopy(rider->tricksOff, character->tricksOff, sizeof(rider->tricksOff));
        }
        else
        {
            rider->ownTricks[0] = kNoPlayerOwnTricks[0];
            rider->ownTricks[1] = kNoPlayerOwnTricks[1];
        }

        // The models' names: the request's head's, else made.
        if (request->flags & kRiderFromHead)
        {
            zge::memoryCopy(rider, &request->head, kRiderHeadBytes);
        }
        else
        {
            if (createdCharacter(setup->character))
                zge::formatString(rider->riderModelName, kCreatedRiderModel);
            else
                zge::formatString(rider->riderModelName, setup->outfit ? kRiderOutfitModelFormat : kRiderModelFormat,
                                  static_cast<uint32_t>(setup->character));
            zge::formatString(rider->bikeModelName, kBikeModelFormat, static_cast<uint32_t>(request->bikeModel),
                              static_cast<uint32_t>(request->bike < kLastBike + 1 ? request->bike : kLastBike));
        }

        // The bike's set-up.
        rider->massHeight = ee::Float(0.5f);
        rider->mass = ee::Float(5.0f);
        rider->springMass = ee::Float(0.2f);
        rider->slip = ee::Float(1.0f);
        rider->backWheelHeight = ee::Float(0.24645f);
        rider->grip = ee::Float(1.0f);
        rider->frontWheelHeight = ee::Float(0.24645f);
        rider->barsTilt = -0xAAA;
        rider->frontWheelPoint[0] = ee::Float(0.0f);
        rider->frontWheelPoint[2] = ee::Float(0.55253f);
        rider->frontWheelPoint[1] = ee::Float(-0.05596f);
        rider->backWheelPoint[0] = ee::Float(0.0f);
        rider->backWheelPoint[2] = ee::Float(-0.3652f);
        rider->backWheelPoint[1] = ee::Float(-0.05596f);
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

        // The rider model and its lean bones.
        if (!setup || !createdCharacter(setup->character))
            rider->messenger = ee::Address(zge::newSceneModelFromName(kModelFlags, rider->riderModelName));
        else if (kCreatorOutfits->count != 0)
            rider->messenger =
                ee::Address(zge::newModelObject(newCreatedRiderModel(setup->character - kFirstCreatedCharacter, setup->outfit), 0));
        if (!rider->messenger)
        {
            zge::heapFree(rider);
            return kNoRiderSlot;
        }
        for (uint32_t bone = 0; bone < kLeanBones; ++bone)
        {
            const uint32_t name = zge::nameHash(leanBones[bone].get());
            if (zge::findSceneModelBone(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->messenger)), name))
            {
                rider->leanWeights[0][bone].bone = name;
                rider->leanWeights[1][bone].bone = name;
            }
        }
        rider->leanWeightLists[0].weights = &rider->leanWeights[0][0];
        rider->leanWeightLists[1].weights = &rider->leanWeights[1][0];
        rider->leanWeightLists[1].count = kLeanBones;
        rider->leanWeightLists[0].count = kLeanBones;

        // The bike model.
        const auto freeModels = [&] {
            zge::releaseSceneModel(ee::Ptr<zge::SceneObject>(rider->bikeModel));
            zge::releaseSceneModel(ee::Ptr<zge::SceneObject>(rider->messenger));
            zge::heapFree(rider);
            return kNoRiderSlot;
        };
        rider->bikeModel = ee::Address(zge::newSceneModelFromName(kModelFlags, rider->bikeModelName));
        if (!rider->bikeModel)
        {
            zge::releaseSceneModel(ee::Ptr<zge::SceneObject>(rider->messenger));
            zge::heapFree(rider);
            return kNoRiderSlot;
        }

        // The rider model's dynamics, with its contact handler and its
        // bodies' restitution.
        rider->riderDynamics = 0;
        if (const uint32_t data = riderDataFile(fileBytes))
            readDynamics(data, *fileBytes, &rider->riderDynamics);
        if (!rider->riderDynamics)
            return freeModels();
        const ee::Ptr<zge::Dynamics> riderDynamics(rider->riderDynamics);
        riderDynamics->contactCallback = zge::ContactCallback(sym::riderDynamicsContact);
        ee::Float restitution(kRestitution);
        if ((*g_settingFlags & kSettingFlag4) && *g_multiplayerPlayers != 0)
            restitution = ee::Float(kSettingRestitution);
        zge::setDynamicsRestitution(riderDynamics, restitution);

        // The bike's dynamics, the two models' mirrors, the bike's contact
        // handler, the limbs, the rolling sound.
        rider->bikeDynamics = 0;
        if (const uint32_t data = bikeDataFile(fileBytes))
            readDynamics(data, *fileBytes, &rider->bikeDynamics);
        if (!rider->bikeDynamics)
            return freeModels();
        rider->riderMirror = ee::Ptr<zge::BoneMirror>(
            zge::newBoneMirror(ee::Ptr<zge::SceneObject>(rider->messenger), ee::Ptr<zge::BoneNamePair>(riderMirrorBones.address())));
        if (!rider->riderMirror)
            return freeModels();
        rider->bikeMirror = ee::Ptr<zge::BoneMirror>(
            zge::newBoneMirror(ee::Ptr<zge::SceneObject>(rider->bikeModel), ee::Ptr<zge::BoneNamePair>(bikeMirrorBones.address())));
        if (!rider->bikeMirror)
        {
            zge::heapFree(rider->riderMirror);
            return freeModels();
        }
        ee::Ptr<zge::Dynamics>(rider->bikeDynamics)->contactCallback = zge::ContactCallback(sym::bikeDynamicsContact);
        setUpLimbChains(rider.get());
        rider->rollingVoice = static_cast<uint32_t>(startRollSound());
        if (request->flags & kRiderNoPlayer)
            rider->flags |= kFlagNoPlayer;
        else
            rider->flags &= ~kFlagNoPlayer;

        // A player's bail body: its box, its hands' holds on the lower
        // arms, its mass (the inertia's rows times it, the inverse's over
        // it) and its box in the level's sweep.
        if (rider->flags & kFlagNoPlayer)
        {
            rider->bailBody = ee::Ptr<zge::RigidBody>();
        }
        else
        {
            calls::memset(handReach, 0, sizeof(zge::Vec4));
            const ee::Ptr<zge::RigidBody> body(zge::heapAllocate(zge::kRigidBodyBytes));
            rider->bailBody = body;
            body->shape = kBoxBody;
            halfExtents->z = ee::Float(kBailBodyHalfExtent);
            halfExtents->x = ee::Float(kBailBodyHalfExtent);
            halfExtents->y = ee::Float(kBailBodyHalfExtent);
            // (The engine's newDynamicsForce makes it: the same record.)
            body->halfExtents = extentsOf(zge::newDynamicsForce(halfExtents));
            body->mass = ee::Float(kBailBodyMass);
            for (ee::Ptr<zge::RigidBody> part = riderDynamics->bodies; part; part = part->next)
            {
                if (part->nameHash == *g_leftLowArmBone)
                {
                    handReach->x = ee::Float(kHandReach);
                    const ee::Ptr<zge::RigidBody> key(zge::nameHash(kLeftHandName.get()));
                    rider->hands[0].body = ee::Address(zge::newBodyContactRecord(part, handReach, handReach, key));
                    rider->hands[0].heldBody = ee::Address(part);
                }
                if (part->nameHash == *g_rightLowArmBone)
                {
                    handReach->x = ee::Float(-kHandReach);
                    const ee::Ptr<zge::RigidBody> key(zge::nameHash(kRightHandName.get()));
                    rider->hands[1].body = ee::Address(zge::newBodyContactRecord(part, handReach, handReach, key));
                    rider->hands[1].heldBody = ee::Address(part);
                }
            }
            zge::matrixIdentity(&body->inertia);
            zge::matrixIdentity(&body->inverseInertia);
            const ee::Float mass = body->mass;
            for (int row = 0; row < 3; ++row)
                body->inertia.row(row) *= mass;
            const ee::Float inverseMass = ee::Float(1.0f) / mass;
            for (int row = 0; row < 3; ++row)
                body->inverseInertia.row(row) *= inverseMass;
            zge::copyRotationAndTranslation(&body->worldInertia, &body->inertia);
            zge::copyRotationAndTranslation(&body->worldInverseInertia, &body->inverseInertia);
            zge::vec3Negate(&lowCorner->xyz(), &halfExtents->xyz());
            body->sweepEntry = zge::sweepAddBody(levelSweep(), kSweepBody, body, &lowCorner->x, &halfExtents->x);
            body->record = slot | kBailBodyRecord;
            body->clearIgnored();
        }
        (*g_riders)[slot] = rider;
        return slot;
    }
    READABLE_FUNCTION_WAITS(newRider, Verified);
}
