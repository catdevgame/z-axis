// DM2's level NPCs (npcs.h): their states, and their replays kept in step
// with the game. DM2's own code (game.c).

#include "npcs.h"

#include "goals.h"
#include "players.h"
#include "round.h"
#include "dm2/action/action_state.h"
#include "dm2/action/action.h"
#include "dm2/camera/camera.h"
#include "dm2/overlay/overlay.h"
#include "dm2/replay/replays.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/npcs.cpp.h"

namespace zge
{
    ChunkProgressHook chunkProgressHook();                                                          // assets/chunk_reader.cpp
    uint32_t findZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, uint32_t group, uint32_t asset); // dm2/zfile_library.cpp
    // (scene/pose.cpp) the engine's step to walkSceneBones, which visits a
    // scene's bones and does nothing with them.
    int32_t countSceneMeshStrips(ee::Ptr<Scene> scene);
}

namespace game::dm2
{
    namespace
    {
        inline ee::Global<Npc[kNpcs], sym::g_npcs> g_npcs;
        // The NPCs' frames counted (setupNpcs starts it).
        inline ee::Global<uint32_t, sym::g_npcFrame> g_npcFrame;
        // How the NPCs' replays run with the game (setNpcPlay), and the replay
        // state a riding NPC's replay takes for each.
        inline ee::Global<uint8_t, sym::g_npcPlay> g_npcPlay;
        inline ee::Global<uint8_t[4], sym::g_npcReplayStates> g_npcReplayStates; // idle, playing, 3, playing

        Npc &npcAt(uint32_t npc)
        {
            return (*g_npcs)[npc & 0xFF];
        }
    }

    namespace
    {
        // The NPCs of each ProQuest level, the five of NpcDefinition (the
        // last the stand-in).
        constexpr uint32_t kNpcLevels = 8;
        constexpr uint32_t kNpcDefinitions = kNpcs + 1;
        constexpr uint32_t kNpcStandIn = kNpcs;
        inline ee::Global<NpcDefinition[kNpcLevels][kNpcDefinitions], sym::g_npcDefinitions> g_npcDefinitions;
        // The icons' model names (none for an empty name).
        inline ee::Global<ee::Ptr<const char>[kNpcIcons], sym::g_npcIconNames> g_npcIconNames;
        // The NPCs' replays are the game's own, in its replay library: from
        // group 16, four a level.
        constexpr ee::Ptr<const char> kNpcReplayLibrary{sym::npcReplayLibraryPath}; // "assets\replay.zal"
        constexpr uint32_t kNpcReplayGroups = 0x10;
        constexpr uint32_t kNpcReplaysPerLevel = 4;
        constexpr uint32_t kNpcReplayBytes = 0x2800;
        // The load's progress: a step for each NPC.
        constexpr ee::Ptr<const char> kNpcProgressName{sym::npcProgressName}; // "NPC"
        constexpr ee::Ptr<const char> kNameFormat{sym::npcModelNameFormat};    // "%s"
        // g_npcPlay before the first setNpcPlay: no play.
        constexpr uint8_t kNpcPlayUnset = 0xFF;
    }

    // setupNpcs (bmxnpcInit): a ProQuest level's NPCs made as it loads, in a
    // single player's level-goals game: each defined NPC's rider (no shadows)
    // made from its definition's models, its icons, and its replay read from
    // the game's library. An NPC who is the player's own character gives its
    // goals to the level's stand-in, who rides in its place (a created rider
    // is none of them). An NPC whose rider, replay or library asset fails is
    // left out (no rider). Answers 1.
    uint32_t setupNpcs()
    {
        ee::Local<NewRider> request;
        ee::Local<ReplaySetup> replaySetup;
        ee::Local<uint32_t> savedPart;
        ee::Local<uint32_t> savedBytes;
        const ee::Ptr<const char> iconNames[kNpcIcons] = {(*g_npcIconNames)[0], (*g_npcIconNames)[1], (*g_npcIconNames)[2],
                                                          (*g_npcIconNames)[3], (*g_npcIconNames)[4]};
        if (*g_multiplayerPlayers != 1 || *g_sessionLevel >= kNpcLevels || *g_sessionSinglePlayerMode != kLevelGoals)
        {
            (*g_npcs)[0].rider = kNoNpc;
            zge::chunkProgressHook()(kNpcProgressName, 1, 1);
            return 1;
        }

        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kNpcReplayLibrary);
        zge::memoryFill(request, 0, sizeof(NewRider));
        request->flags |= kRiderNoPlayer | kRiderFromHead;
        for (uint32_t npc = 0; npc < kNpcs; ++npc)
        {
            Npc &record = (*g_npcs)[npc];
            zge::chunkProgressHook()(kNpcProgressName, npc, kNpcs);
            NpcDefinition *definition = &(*g_npcDefinitions)[*g_sessionLevel][npc];
            if (zge::stringLength(definition->name) == 0)
            {
                record.rider = kNoNpc;
                break;
            }
            const uint8_t character = (*g_playerSetups)[*g_currentPlayer].character;
            if (static_cast<uint8_t>(character - kBuiltInCharacters) >= kCharacters - kBuiltInCharacters &&
                character == definition->character)
            {
                NpcDefinition &standIn = (*g_npcDefinitions)[*g_sessionLevel][kNpcStandIn];
                standIn.challenge = definition->challenge;
                standIn.firstGoal = definition->firstGoal;
                standIn.lastGoal = definition->lastGoal;
                definition = &standIn;
            }
            record.definition = definition;
            zge::formatString(request.address() + offsetof(NewRider, head.riderModel), kNameFormat, definition->riderModel);
            zge::formatString(request.address() + offsetof(NewRider, head.bikeModel), kNameFormat, definition->bikeModel);
            record.rider = rider::newRider(request);
            if (record.rider == kNoNpc)
                continue;

            const ee::Ptr<rider::Rider> made = rider::riderOf(record.rider);
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(made->messenger), 0);
            zge::setObjectCastsShadow(ee::Ptr<zge::SceneObject>(made->bikeModel), 0);
            for (uint32_t icon = 0; icon < kNpcIcons; ++icon)
            {
                if (iconNames[icon][0] == 0)
                    record.icons[icon] = 0;
                else
                {
                    const ee::Ptr<zge::SceneObject> model = zge::newSceneModelFromName(0, iconNames[icon]);
                    record.icons[icon] = ee::Address(model);
                    zge::countSceneMeshStrips(zge::sceneOf(model));
                }
            }

            replaySetup->dataBytes = kNpcReplayBytes;
            replaySetup->flags = kReplayPlaysOnly;
            replaySetup->rider = record.rider;
            record.replay = newReplay(replaySetup);
            if (record.replay != kNoNpcReplay)
            {
                replaySavedPart(record.replay, savedPart, savedBytes);
                if (zge::findZfileLibraryAsset(library, *g_sessionLevel * kNpcReplaysPerLevel + npc + kNpcReplayGroups, 0) != 0 &&
                    zge::zfileLibraryAssetBytes(library) <= *savedBytes)
                {
                    readLibraryReplay(library, *savedPart);
                    continue;
                }
                freeReplay(record.replay);
                record.replay = kNoNpcReplay;
            }
            rider::freeRider(record.rider);
            record.rider = kNoNpc;
        }
        zge::closeZfileLibrary(library);
        *g_npcFrame = 0;
        *g_npcPlay = kNpcPlayUnset;
        setNpcPlay(kNpcPlayStopped);
        zge::chunkProgressHook()(kNpcProgressName, kNpcs, kNpcs);
        return 1;
    }
    READABLE_FUNCTION(setupNpcs, Verified);

    // setNpcPlay: the NPCs told how the game runs. As a turn starts, every
    // NPC goes back to waiting; each one's replay then follows.
    uint32_t setNpcPlay(uint32_t play)
    {
        const uint8_t was = *g_npcPlay;
        *g_npcPlay = static_cast<uint8_t>(play);
        for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
        {
            if ((play == kNpcPlayTurn || play == kNpcPlayStartToo) && was != kNpcPlayKept)
                setNpcState(npc, 0, kNpcWaiting);
            updateNpcReplay(npc);
        }
        return play;
    }
    READABLE_FUNCTION(setNpcPlay, Verified);

    uint8_t npcState(uint32_t npc, uint32_t level)
    {
        return npcAt(npc).states[level];
    }
    READABLE_FUNCTION(npcState, Verified);

    // setNpcState: an NPC's state at a level: its frames start over, the
    // levels after it are cleared, its replay follows, and its meetings are
    // forgotten.
    uint32_t setNpcState(uint32_t npc, uint32_t level, uint32_t state)
    {
        Npc &record = npcAt(npc);
        record.states[level] = static_cast<uint8_t>(state);
        record.stateFrames[level] = 0;
        for (uint32_t after = level + 1; after < kNpcStateLevels; ++after)
        {
            record.states[after] = 0;
            record.stateFrames[after] = 0;
        }
        updateNpcReplay(npc & 0xFF);
        record.meetings = 0;
        return state;
    }
    READABLE_FUNCTION(setNpcState, Verified);

    void npcInfo(uint32_t npc, ee::Ptr<NpcInfo> info)
    {
        const Npc &record = npcAt(npc);
        info->character = record.definition->character;
        info->name = record.definition->name;
        info->challenge = record.definition->challenge;
        info->firstGoal = record.definition->firstGoal;
        info->lastGoal = record.definition->lastGoal;
        info->goal = record.goal;
        info->meetings = record.meetings;
        info->rider = record.rider;
    }
    READABLE_FUNCTION(npcInfo, Verified);

    uint8_t npcForChallenge(uint32_t challenge)
    {
        for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
            if ((*g_npcs)[npc].definition->challenge == challenge)
                return static_cast<uint8_t>(npc);
        return kNoNpc;
    }
    READABLE_FUNCTION(npcForChallenge, Verified);

    // updateNpcReplay: a riding NPC's replay takes the state the game's play
    // asks for; any other NPC's is idle.
    void updateNpcReplay(uint32_t npc)
    {
        ee::Local<uint8_t[4]> replayStates;
        for (uint32_t i = 0; i < 4; ++i)
            replayStates[i] = (*g_npcReplayStates)[i];
        const Npc &record = npcAt(npc);
        if (record.states[0] == kNpcRiding)
            setReplayState(record.replay, replayStates[*g_npcPlay]);
        else
            setReplayState(record.replay, kReplayIdle);
    }
    READABLE_FUNCTION(updateNpcReplay, Verified);

    // freeNpcs: as the level ends, the NPCs' replays are stopped and freed,
    // and their riders freed.
    void freeNpcs()
    {
        setNpcPlay(kNpcPlayStopped);
        for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
        {
            Npc &record = (*g_npcs)[npc];
            freeReplay(record.replay);
            record.replay = kNoNpcReplay;
            rider::freeRider(record.rider);
            record.rider = kNoNpc;
        }
    }
    READABLE_FUNCTION(freeNpcs, Verified);

    namespace
    {
        // Each state's handler (by Npc::states[0]: 0 .. 5); it answers 0 when
        // the state changed and the new one's handler is to run this frame.
        constexpr uint32_t kNpcStates = 6;
        using NpcStateHandler = ee::FunctionPtr<uint32_t(uint32_t npc)>;
        inline ee::Global<NpcStateHandler[kNpcStates], sym::g_npcStateHandlers> g_npcStateHandlers;
        // The icon each state shows; state 0 shows its second level's.
        inline ee::Global<uint8_t[8], sym::g_npcIconByState> g_npcIconByState;
        inline ee::Global<uint8_t[8], sym::g_npcIconByFirstState> g_npcIconByFirstState;
        // How the NPCs are shown: bit 0 hides their riders, bit 1 their icons;
        // bit 2 stills them (no state handlers, no icons).
        inline ee::Global<uint32_t, sym::g_npcDisplayFlags> g_npcDisplayFlags;
        constexpr uint32_t kNpcRidersHidden = 0x1, kNpcIconsHidden = 0x2, kNpcsStill = 0x4;

        // An icon's place over its NPC's head, in the rider's rows (y the
        // rows' up), and how it bobs and turns: a bob and a turn each so
        // many turns a frame.
        inline ee::Global<ee::Float, sym::g_npcIconHeight> g_npcIconHeight; // 1.35
        constexpr ee::Float kNpcIconAhead = 0.25f;
        inline ee::Global<ee::Float, sym::g_npcIconBobRate> g_npcIconBobRate;   // 0.0058333 (a bob in 171 frames)
        inline ee::Global<ee::Float, sym::g_npcIconTurnRate> g_npcIconTurnRate; // 0.0083333 (a turn in 120 frames)
        constexpr ee::Float kAngleTurn = 65536.0f;
        constexpr ee::Float kBobBase = 1.5f, kBobScale = 0.125f;

        zge::Angle npcAngle(ee::Float rate)
        {
            return static_cast<zge::Angle>(ee::toInt(ee::toFloat(static_cast<int32_t>(*g_npcFrame)) * rate * kAngleTurn));
        }
    }

    namespace rider
    {
        int32_t riderFrame(uint32_t index); // actions/frame.cpp
        namespace physics
        {
            void riderReset(uint32_t index, uint32_t shown); // respawn.cpp
            void poseBikeGround(Rider *rider);               // pose.cpp
        }
    }

    namespace
    {
        // The frames a bailing NPC's rider lies before its ride is over.
        constexpr int16_t kNpcBailFrames = 180;
        // Rider::flags: set while an NPC's rider bails.
        // Scaffolding (unnamed-field): what reads the bit is not read yet.
        constexpr uint32_t kFlagNpcBailing = 0x4u;

        // A standing NPC's wait before it rides: offering something else
        // than a goal (its second level not 0), after a meeting, or at first.
        constexpr uint16_t kNpcWaitOffering = 900, kNpcWaitAfterMeeting = 300, kNpcWaitAtFirst = 2700;
        // How many NPCs may be out of standing (riding, bailing, over, waiting) at once.
        inline ee::Global<uint32_t, sym::g_busyNpcsMost> g_busyNpcsMost;
        // The NPC the player is with (its screen's and the camera's).
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;
        // The camera mode (level 0) that looks at the shown NPC (the follow
        // camera takes its rider in modes 6 and 7).
        constexpr uint32_t kCameraOnShownNpc = 6;
    }

    namespace
    {
        // The reach (squared) of a rider bumping into an NPC: 1.5.
        constexpr float kTouchReachSquared = 2.25f;
    }

    uint8_t riderTouchingNpc(uint32_t npc)
    {
        ee::Local<zge::Vec4> apart;   // (its w is never written: the frame's)
        ee::Local<zge::Vec4> closing;
        const ee::Ptr<rider::Rider> npcRider = rider::riderOf((*g_npcs)[npc & 0xFF].rider);
        for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf((*g_playerRiders)[player]);
            // The drawn bodies' places (the position and the root's offset) apart.
            for (uint32_t i = 0; i < 3; ++i)
                (&apart->x)[i] = (rider->position[i] + rider->rootOffset[i]) - (npcRider->position[i] + npcRider->rootOffset[i]);
            if (rider::collision::engine::lengthSquared(&apart->x) < ee::Float(kTouchReachSquared))
            {
                *closing = *rider::vec4(rider->velocity) - *rider::vec4(npcRider->velocity);
                if (zge::dot(*apart, *closing) <= ee::Float(0.0f))
                    return (*g_playerRiders)[player];
            }
        }
        return kNoNpc;
    }
    READABLE_FUNCTION_VU0(riderTouchingNpc, Verified);

    namespace
    {
        // Rider::bailKind of a player's rider that ran into an NPC.
        constexpr uint8_t kBailRanIntoNpc = 3;

        // v's x, y and z less `along` times `share` (the FPU's, lane by lane).
        void takeAlong(ee::Float *v, const zge::Vec4 &along, ee::Float share)
        {
            v[0] = v[0] - along.x * share;
            v[1] = v[1] - along.y * share;
            v[2] = v[2] - along.z * share;
        }
        void addAlong(ee::Float *v, const zge::Vec4 &along, ee::Float share)
        {
            v[0] = v[0] + along.x * share;
            v[1] = v[1] + along.y * share;
            v[2] = v[2] + along.z * share;
        }
    }

    uint8_t npcCollision(uint32_t npc, uint32_t riderIndex)
    {
        ee::Local<zge::Vec4> npcPlace;
        ee::Local<zge::Vec4> riderPlace;
        ee::Local<zge::Vec4> along;
        ee::Local<zge::Vec4> closing;
        const uint8_t index = static_cast<uint8_t>(npc & 0xFF);
        const uint8_t bumped = static_cast<uint8_t>(riderIndex & 0xFF);
        const Npc &record = (*g_npcs)[index];
        const ee::Ptr<rider::Rider> npcRider = rider::riderOf(record.rider);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(bumped);
        // The drawn bodies' places, the line from the NPC to the rider, and
        // how fast they close along it.
        *npcPlace = *rider::vec4(npcRider->position) + *rider::vec4(npcRider->rootOffset);
        *riderPlace = *rider::vec4(rider->position) + *rider::vec4(rider->rootOffset);
        *along = *riderPlace - *npcPlace;
        *along = zge::unit(*along);
        *closing = *rider::vec4(rider->velocity) - *rider::vec4(npcRider->velocity);
        const ee::Float closingSpeed = zge::dot(*closing, *along);
        if (npcPlace->y < riderPlace->y)
        {
            const ee::Float speedAlong = zge::dot(*rider::vec4(rider->velocity), *along);
            takeAlong(rider->velocity, *along, speedAlong);
            takeAlong(rider->velocity, *along, closingSpeed);
            rider::setPhysicsState(rider::riderOf(bumped).get(), rider::kPhysicsBail);
            rider::riderOf(bumped)->bailKind = kBailRanIntoNpc;
            return bumped;
        }
        const ee::Float speedAlong = zge::dot(*rider::vec4(npcRider->velocity), *along);
        takeAlong(npcRider->velocity, *along, speedAlong);
        addAlong(npcRider->velocity, *along, closingSpeed);
        setNpcState(index, 0, kNpcBailing);
        return record.rider;
    }
    READABLE_FUNCTION_VU0(npcCollision, Verified);

    namespace
    {
        // A meeting's reach (squared: 4), and the rider's place by the NPC in
        // the NPC's rows: 1.75 to its right, 4.5 ahead.
        constexpr float kMeetReachSquared = 16.0f;
        constexpr float kMeetRight = 1.75f, kMeetAhead = 4.5f;
    }

    uint8_t meetNpc(uint32_t player)
    {
        // The NPC from the rider; then the rider's place by it (its w is the
        // one the first left there).
        ee::Local<zge::Vec4> place;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        for (uint32_t npc = 0; npc < kNpcs; ++npc)
        {
            Npc &record = (*g_npcs)[npc];
            if (record.rider == kNoNpc)
                return kNoNpc;
            const ee::Ptr<rider::Rider> npcRider = rider::riderOf(record.rider);
            *place = *rider::vec4(npcRider->position) - *rider::vec4(rider->position);
            if (!(rider::collision::engine::lengthSquared(&place->x) < ee::Float(kMeetReachSquared)) ||
                record.states[0] != kNpcStanding)
                continue;

            const uint8_t met = static_cast<uint8_t>(npc & 0xFF);
            npcWaitingFrame(met);
            const uint8_t offers = npcState(met, 1);
            setNpcState(met, 0, kNpcMet);
            setNpcState(met, 1, offers);
            // (setNpcState cleared the meetings: this makes them 1.)
            ++record.meetings;
            place->x = kMeetRight;
            place->y = 0.0f;
            place->z = kMeetAhead;
            zge::transformPoint(place, rider::matrix(npcRider->right), place);
            *place = *place + *rider::vec4(npcRider->position);
            *rider::vec4(rider->position) = *place;
            *rider::vec4(rider->drawnPosition) = *place;
            // Facing the NPC, its up row kept.
            zge::Vec4 &forward = *rider::vec4(rider->forward);
            forward = *rider::vec4(npcRider->position) - *rider::vec4(rider->position);
            forward = zge::unit(forward);
            *rider::vec4(rider->right) = zge::cross(*rider::vec4(rider->up), forward);
            *rider::vec4(rider->rowsOrigin) = zge::kZero;
            rider::physics::engine::copyMatrix(rider->body, rider->right);
            rider::physics::riderReset(riderIndexOf(player), 1);
            return met;
        }
        return kNoNpc;
    }
    READABLE_FUNCTION_VU0(meetNpc, Verified);

    // npcStandingFrame (kNpcStanding's handler): once its wait is over and
    // few enough NPCs are busy, it starts riding its replay. A player riding
    // into it (not kFlagNoPlayer) meets it: in play the NPC screen comes up
    // with the camera on it; in the other action modes its goal is given at
    // once (when it offers one), it goes to waiting, and the ground camera
    // is placed.
    uint32_t npcStandingFrame(uint32_t npc)
    {
        const uint8_t index = static_cast<uint8_t>(npc & 0xFF);
        const Npc &record = (*g_npcs)[index];
        uint16_t wait = kNpcWaitOffering;
        if (record.states[1] == 0)
            wait = record.meetings != 0 ? kNpcWaitAfterMeeting : kNpcWaitAtFirst;
        if (static_cast<uint16_t>(record.stateFrames[0]) >= wait)
        {
            uint32_t busy = 0;
            for (uint32_t other = 0; other < kNpcs; ++other)
            {
                const uint8_t state = (*g_npcs)[other].states[0];
                if (state < kNpcWaiting + 1 && state >= kNpcRiding)
                    ++busy;
            }
            if (busy < *g_busyNpcsMost)
            {
                setReplayState(record.replay, kReplayPlaying);
                setNpcState(index, 0, kNpcRiding);
                return 0;
            }
        }

        const uint8_t touching = riderTouchingNpc(index);
        if (touching == kNoNpc || (rider::riderOf(touching)->flags & rider::kFlagNoPlayer))
            return 1;
        *g_shownNpc = meetNpc(rider::riderOf(touching)->player);
        if (*g_shownNpc == kNoNpc)
            return 1;
        if (actionMode() == 0)
        {
            showGameScreen(kScreenNpc);
            setCameraMode(*g_currentPlayer, 0, kCameraOnShownNpc);
            return 1;
        }
        if (npcState(*g_shownNpc, 1) == 0)
        {
            ee::Local<NpcInfo> info;
            npcInfo(*g_shownNpc, info);
            giveGoal(*g_currentPlayer, info->goal);
        }
        setNpcState(*g_shownNpc, 0, kNpcWaiting);
        const uint8_t player = *g_currentPlayer;
        placeGroundCamera(player, groundCameraKind(player));
        return 1;
    }
    READABLE_FUNCTION(npcStandingFrame, Verified);

    // npcRidingFrame (kNpcRiding's handler): while its replay plays, its
    // rider has its frame; a player riding into it collides with it, and a
    // bail of its own sets it bailing. Its replay played out ends its ride.
    uint32_t npcRidingFrame(uint32_t npc)
    {
        const Npc &record = (*g_npcs)[npc & 0xFF];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(record.rider);
        const uint8_t state = replayState(record.replay);
        if (state == kReplayPlaying)
        {
            rider::riderFrame(record.rider);
            const uint8_t touching = riderTouchingNpc(npc & 0xFF);
            if (touching != kNoNpc)
            {
                npcCollision(npc & 0xFF, touching);
                return 0;
            }
            if (rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionBail)
                return 1;
            setNpcState(npc & 0xFF, 0, kNpcBailing);
            return 0;
        }
        if (state != kReplayPlayedOut)
            return 1;
        setNpcState(npc & 0xFF, 0, kNpcRideOver);
        return 0;
    }
    READABLE_FUNCTION(npcRidingFrame, Verified);

    // npcBailFrame (kNpcBailing's handler): at the state's first frame its
    // rider falls (the bail's physics); after 180 frames its ride is over;
    // in between its rider has its frame.
    uint32_t npcBailFrame(uint32_t npc)
    {
        const Npc &record = (*g_npcs)[npc & 0xFF];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(record.rider);
        if (record.stateFrames[0] == 0)
        {
            rider::setPhysicsState(rider.get(), rider::kPhysicsBail);
            rider->flags |= kFlagNpcBailing;
        }
        else if (record.stateFrames[0] == kNpcBailFrames)
        {
            rider->flags &= ~kFlagNpcBailing;
            setNpcState(npc & 0xFF, 0, kNpcRideOver);
            return 0;
        }
        rider::riderFrame(record.rider);
        return 1;
    }
    READABLE_FUNCTION(npcBailFrame, Verified);

    namespace
    {
        // kNpcRideOver's steps (Npc::states[1]): put at the start at once;
        // riding back to it (no setter of this step is read yet); turning
        // to face as the replay starts.
        constexpr uint8_t kNpcPutAtStart = 0, kNpcRidingBack = 1, kNpcTurningAtStart = 2;
        // Riding back: a step of g_npcHomeStep a frame towards the start,
        // the body's up row tipped by kGroundTilt of the way to the ground
        // under it; within 1 of the start, or after 600 frames, it drifts
        // in by g_npcHomeDrift of the way instead. Home within
        // g_npcHomeReached (squared). Turning: its forward and up rows turned
        // by g_npcHomeTurn of the way to the replay's start rows, until its
        // right row is within kNpcFacingTurn of theirs.
        inline ee::Global<ee::Float, sym::g_npcHomeDrift> g_npcHomeDrift;     // 0.05
        inline ee::Global<ee::Float, sym::g_npcHomeStep> g_npcHomeStep;       // 0.1
        inline ee::Global<ee::Float, sym::g_npcHomeReached> g_npcHomeReached; // 0.0025
        inline ee::Global<ee::Float, sym::g_npcHomeTurn> g_npcHomeTurn;       // 0.05
        constexpr float kNpcNearHome = 1.0f;
        constexpr uint16_t kNpcRideBackFrames = 600;
        constexpr float kGroundTilt = 0.125f;
        constexpr int32_t kNpcFacingTurn = 0x16C; // (a turn is 0x10000: about two degrees)

        // A blend of `to` into `v`'s x, y and z by `rate` of the way (the FPU's, lane by lane).
        void blendTowards(zge::Vec4 &v, const ee::Float *to, ee::Float rate)
        {
            v.x = v.x + (to[0] - v.x) * rate;
            v.y = v.y + (to[1] - v.y) * rate;
            v.z = v.z + (to[2] - v.z) * rate;
        }
    }

    uint32_t npcRideOverFrame(uint32_t npc)
    {
        ee::Local<zge::Matrix> startRows;
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> toStart;
        ee::Local<rider::GroundHit> ground;
        const uint8_t index = static_cast<uint8_t>(npc & 0xFF);
        Npc &record = (*g_npcs)[index];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(record.rider);
        zge::Vec4 &drawn = *rider::vec4(rider->drawnPosition);
        zge::Matrix &body = *rider::matrix(rider->body);
        zge::Vec4 &right = body.row(0);
        zge::Vec4 &up = body.row(1);
        zge::Vec4 &forward = body.row(2);
        *start = *rider::vec4(replayRiderPosition(record.replay));

        const uint8_t step = record.states[1];
        if (step == kNpcPutAtStart)
        {
            drawn = *start;
            setNpcState(index, 1, kNpcTurningAtStart);
            return 0;
        }
        if (step == kNpcRidingBack)
        {
            *toStart = *start - drawn;
            if (rider::collision::engine::lengthSquared(&toStart->x) < ee::Float(kNpcNearHome) ||
                static_cast<uint16_t>(record.stateFrames[1]) > kNpcRideBackFrames)
            {
                blendTowards(drawn, &start->x, *g_npcHomeDrift);
            }
            else
            {
                *toStart = zge::unit(*toStart);
                *toStart = *toStart * *g_npcHomeStep;
                drawn = drawn + *toStart;
                *toStart = *start - drawn;
                forward = zge::unit(*toStart);
                if (rider::groundBelow(&drawn.x, ground) != 0)
                {
                    drawn = *rider::vec4(ground->point);
                    blendTowards(up, ground->plane, ee::Float(kGroundTilt));
                    *toStart = up * rider->frontWheelHeight;
                    drawn = drawn + *toStart;
                }
                right = zge::cross(up, forward);
                right = zge::unit(right);
                forward = zge::cross(right, up);
                rider::physics::engine::copyMatrix(rider->right, rider->body);
                *rider::vec4(rider->position) = drawn;
            }
            *toStart = *start - drawn;
            if (rider::collision::engine::lengthSquared(&toStart->x) < *g_npcHomeReached)
            {
                setNpcState(index, 1, kNpcTurningAtStart);
                return 0;
            }
        }
        else if (step == kNpcTurningAtStart)
        {
            rider::physics::engine::copyMatrix(startRows, replayRiderRows(record.replay));
            blendTowards(forward, &startRows->row(2).x, *g_npcHomeTurn);
            blendTowards(up, &startRows->row(1).x, *g_npcHomeTurn);
            right = zge::cross(up, forward);
            right = zge::unit(right);
            if (zge::angleAcos(zge::dot(right, startRows->row(0))) < kNpcFacingTurn)
            {
                setNpcState(index, 0, kNpcWaiting);
                return 0;
            }
        }

        rider::physics::poseBikeGround(rider.get());
        const uint8_t touching = riderTouchingNpc(index);
        if (touching != kNoNpc)
        {
            npcCollision(index, touching);
            return 0;
        }
        ++record.stateFrames[1];
        return 1;
    }
    READABLE_FUNCTION_VU0(npcRideOverFrame, Verified);

    uint32_t npcWaitingFrame(uint32_t npc)
    {
        const uint8_t index = static_cast<uint8_t>(npc & 0xFF);
        Npc &record = (*g_npcs)[index];
        const ee::Ptr<rider::Rider> rider = rider::riderOf(record.rider);
        zge::copyRotationAndTranslation(rider::matrix(rider->right), replayRiderRows(record.replay));
        const ee::Ptr<ee::Float> start = replayRiderPosition(record.replay);
        for (uint32_t i = 0; i < 4; ++i)
            rider->position[i] = start[i];
        rider::physics::riderReset(record.rider, 1);
        rider::riderFrame(record.rider);
        if (multiplayerMode() != 0 || singlePlayerMode() != kLevelGoals)
            return 0;

        record.goal = 0;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(*g_currentPlayer, *g_sessionLevel);
        setNpcState(index, 0, kNpcStanding);
        const NpcDefinition &definition = *record.definition;
        if (goals->challenge < definition.challenge)
        {
            setNpcState(index, 1, kNpcChallengeAhead);
            return 0;
        }
        if (definition.challenge < goals->challenge)
        {
            setNpcState(index, 1, kNpcGoalsBehind);
            return 0;
        }
        // The first goal of its range the player has not done.
        uint32_t goal = definition.firstGoal;
        while (goal <= definition.lastGoal && (goals->challenges[goals->challenge].goals[goal].flags & kGoalDone))
            ++goal;
        if (goal <= definition.lastGoal)
        {
            setNpcState(index, 1, kNpcOffersGoal);
            record.goal = static_cast<uint8_t>(goal);
            return 0;
        }
        setNpcState(index, 1, kNpcGoalsBehind);
        return 0;
    }
    READABLE_FUNCTION(npcWaitingFrame, Verified);

    // npcMetFrame (state kNpcMet's handler): nothing while the player is
    // with the NPC (its screen is up); the state stays.
    uint32_t npcMetFrame(uint32_t /*npc*/)
    {
        return 1;
    }
    READABLE_FUNCTION(npcMetFrame, Draft);

    void npcsFrame()
    {
        ee::Local<zge::Vec4> place;
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Quaternion> orientation;
        if (*g_npcPlay == kNpcPlayTurn || *g_npcPlay == kNpcPlayStartToo)
        {
            for (uint32_t npc = 0; npc < kNpcs; ++npc)
            {
                Npc &record = (*g_npcs)[npc];
                if (record.rider == kNoNpc)
                    break;
                const ee::Ptr<rider::Rider> rider = rider::riderOf(record.rider);
                const uint32_t ridersShown = (*g_npcDisplayFlags ^ kNpcRidersHidden) & 1u;
                zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), ridersShown);
                zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->bikeModel), (*g_npcDisplayFlags ^ kNpcRidersHidden) & 1u);
                if (*g_npcDisplayFlags & kNpcsStill)
                {
                    for (uint32_t icon = 0; icon < kNpcIcons; ++icon)
                        if (record.icons[icon] != 0)
                            zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(record.icons[icon]), 0);
                    continue;
                }
                // The state's handler, again while it changes the state.
                while (const NpcStateHandler handler = (*g_npcStateHandlers)[record.states[0]])
                    if (handler(npc) != 0)
                        break;

                for (uint32_t icon = 0; icon < kNpcIcons; ++icon)
                {
                    if (record.icons[icon] == 0)
                        continue;
                    const uint8_t shown = record.states[0] == 0 ? (*g_npcIconByFirstState)[record.states[1]]
                                                                : (*g_npcIconByState)[record.states[0]];
                    const ee::Ptr<zge::SceneObject> object(record.icons[icon]);
                    if (icon != shown)
                    {
                        zge::setSceneObjectShown(object, 0);
                        continue;
                    }
                    // (The place's fourth word is the frame's leftover, as the original's.)
                    place->x = 0.0f;
                    place->y = *g_npcIconHeight;
                    place->z = kNpcIconAhead;
                    // (The rider's rows, right .. its rows' origin, are a matrix.)
                    zge::transformPoint(place, rider::matrix(rider->right), place);
                    *place = *place + *rider::vec4(rider->position);
                    *place = *place + *rider::vec4(rider->rootOffset);
                    const ee::Float bob = zge::angleCos(npcAngle(*g_npcIconBobRate));
                    place->y = place->y - (bob + kBobBase) * kBobScale;
                    zge::setObjectPosition(object, place);
                    zge::matrixIdentity(turn);
                    zge::matrixRotateY(turn, turn, npcAngle(*g_npcIconTurnRate));
                    zge::matrixToQuaternion(orientation, turn);
                    zge::setObjectOrientation(object, orientation);
                    zge::setSceneObjectShown(object, ((*g_npcDisplayFlags >> 1) ^ 1u) & 1u);
                }
                ++record.stateFrames[0];
            }
        }
        ++*g_npcFrame;
    }
    READABLE_FUNCTION(npcsFrame, Verified);
}
