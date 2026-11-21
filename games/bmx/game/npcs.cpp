// BMX XXX's level NPCs (npcs.h): their states, and their replays kept in step
// with the game. BMX XXX's own code (game.c's block), DM2's line (games/dm2/game/npcs.cpp) over BMX XXX's smaller record.

#include "npcs.h"

#include "player_riders.h"
#include "round.h"

#include "bmx/camera/action_camera.h"
#include "bmx/action/camera_state.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/screens.h"
#include "bmx/profile/players.h"
#include "bmx/replay/replays.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/function_ptr.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/file.h"
#include "zge/assets/geo.h"
#include "zge/platform/platform.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/npcs.cpp.h"

namespace game::bmx
{
    namespace
    {
        inline ee::Global<Npc[kNpcs], sym::g_npcs> g_npcs;
        // How the NPCs' replays run with the game (setNpcPlay), and the replay
        // state a riding NPC's replay takes for each.
        inline ee::Global<uint8_t, sym::g_npcPlay> g_npcPlay;
        inline ee::Global<uint8_t[4], sym::g_npcReplayStates> g_npcReplayStates; // idle, playing, 3, playing

        Npc &npcAt(uint32_t npc)
        {
            return (*g_npcs)[npc & 0xFF];
        }

        // The game's table of the rider characters, a row a character: the
        // name the leaderboard lists is its fourth word.
        struct CharacterRow
        {
            uint8_t _00[0x0C];
            ee::Ptr<const char> boardName; // +0x0C
            uint8_t _10[0x14 - 0x10];
        };
        static_assert(offsetof(CharacterRow, boardName) == 0x0C && sizeof(CharacterRow) == 0x14);
        const ee::Ptr<CharacterRow> kCharacterRows(sym::g_characterNames);
    }

    void npcFacts(uint32_t npc, ee::Ptr<NpcFacts> facts)
    {
        const Npc &it = npcAt(npc);
        facts->character = it.record->character;
        facts->name = kCharacterRows[it.record->character].boardName;
        facts->objectiveName = ee::Ptr<const char>(it.record->objectiveName);
        facts->goal = it.goal;
        facts->meetings = it.meetings;
        facts->rider = it.rider;
    }
    READABLE_FUNCTION(npcFacts, Verified);

    // firstNpc: the NPCs fill the table from the first, so the level has one
    // when the first is there.
    uint32_t firstNpc()
    {
        return (*g_npcs)[0].rider != kNoNpc ? 0 : kNoNpc;
    }
    READABLE_FUNCTION(firstNpc, Draft);

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

    namespace
    {
        // Each state's handler (level 0), given the NPC: 0 when it changed
        // the state and the new one's handler runs too.
        constexpr uint32_t kNpcStates = 7;
        inline ee::Global<ee::FunctionPtr<uint32_t(uint32_t)>[kNpcStates], sym::g_npcStateFrames> g_npcStateFrames;
        // The frames the NPCs' frame has run.
        inline ee::Global<uint32_t, sym::g_npcFrames> g_npcFrames;
        // A word no code writes (0 in the data): bit 0 hides the NPCs'
        // models, bit 1 puts their icon's states off, bit 2 stops their
        // states and hides their icons.
        inline ee::Global<uint32_t, sym::g_npcFrameFlags> g_npcFrameFlags;
        constexpr uint32_t kNpcModelsHidden = 0x1;
        constexpr uint32_t kNpcIconStatesOff = 0x2;
        constexpr uint32_t kNpcStatesStopped = 0x4;
        // The NPCs' models draw their first level out to this depth.
        inline ee::Global<ee::Float, sym::g_npcDrawDistance> g_npcDrawDistance;
        // Whether the icon shows in each of the waiting state's sub-states.
        inline ee::Global<uint32_t[4], sym::g_npcIconSubStates> g_npcIconSubStates;
        // The objectives' icon models: the first is "NPR available"'s.
        inline ee::Global<ee::Ptr<zge::SceneObject>[2], sym::g_objectiveIcons> g_objectiveIcons;
        // The icon's place over the NPC, in its rider's rows.
        constexpr zge::Vec4 kIconOffset{0.0f, 1.75f, 0.25f, 0.0f};
        
        // A model's first level drawn out to the NPCs' depth.
        void setNpcDrawDistance(ee::Ptr<zge::SceneObject> model)
        {
            const ee::Ptr<zge::Node> root = zge::sceneRootNode(zge::sceneOf(model));
            zge::frameLevelPart(zge::classModel(root->model, 0), 0)->distance = *g_npcDrawDistance;
        }

        // Whether any player has an objective with a time limit running.
        bool timedObjectiveRunning()
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                if (objectives::riderObjectives(playerRider(player))->timedObjective)
                    return true;
            }
            return false;
        }

        // The NPC's icon shown over its rider, posed as the objectives'
        // icon is.
        void showNpcIcon(const Npc &npc, const rider::Rider *rider, ee::Local<zge::Vec4> &place)
        {
            zge::copyScenePose(npc.icon, zge::sceneOf((*g_objectiveIcons)[0]));
            *place = zge::transformed(*rider::matrix(rider->right), kIconOffset);
            *place = *place + *rider::vec4(rider->position);
            *place = *place + *rider::vec4(rider->rootOffset);
            zge::setScenePosition(npc.icon, place);
            zge::poseScene(npc.icon);
            zge::changeSceneState(npc.icon, 0, (*g_npcFrameFlags & kNpcIconStatesOff) == 0 ? 1 : 0);
        }
    }

    // npcsFrame: while a turn plays the NPCs, each one's rider and bike cast
    // no shadow, show (unless kNpcModelsHidden) and draw out to the NPCs'
    // depth; its state's handlers run, and its icon shows while it waits to
    // be met (in a sub-state that shows it, and in the challenge only
    // while no player has a timed objective running). With the NPCs kept,
    // their models and icons are hidden.
    void npcsFrame()
    {
        ee::Local<zge::Vec4> place;
        const bool timedRunning = gameMode() == kChallengeMode && timedObjectiveRunning();
        const uint8_t play = *g_npcPlay;
        if (play == kNpcPlayKept)
        {
            for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
            {
                Npc &it = (*g_npcs)[npc];
                rider::Rider *const rider = &*rider::riderOf(it.rider);
                rider::clearFlag(rider, rider::kFlagRiderModelShown);
                rider::clearFlag(rider, rider::kFlagBikeModelShown);
                if (it.icon)
                    zge::changeSceneState(it.icon, 1, 0);
            }
        }
        else if (play == kNpcPlayTurn || play == kNpcPlayStartToo)
        {
            for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
            {
                Npc &it = (*g_npcs)[npc];
                rider::Rider *const rider = &*rider::riderOf(it.rider);
                zge::setObjectCastsShadow(rider->messenger, 0);
                zge::setObjectCastsShadow(rider->bikeModel, 0);
                if ((*g_npcFrameFlags & kNpcModelsHidden) == 0)
                    rider::setFlag(rider, rider::kFlagRiderModelShown);
                else
                    rider::clearFlag(rider, rider::kFlagRiderModelShown);
                if ((*g_npcFrameFlags & kNpcModelsHidden) == 0)
                    rider::setFlag(rider, rider::kFlagBikeModelShown);
                else
                    rider::clearFlag(rider, rider::kFlagBikeModelShown);
                setNpcDrawDistance(rider->messenger);
                setNpcDrawDistance(rider->bikeModel);

                if ((*g_npcFrameFlags & kNpcStatesStopped) != 0)
                {
                    if (it.icon)
                        zge::changeSceneState(it.icon, 1, 0);
                    continue;
                }
                for (;;)
                {
                    const ee::FunctionPtr<uint32_t(uint32_t)> handler = (*g_npcStateFrames)[it.states[0]];
                    if (!handler || handler(npc) != 0)
                        break;
                }
                if (it.icon)
                {
                    if (it.states[0] == kNpcStanding && (*g_npcIconSubStates)[it.states[1]] != 0 && !timedRunning)
                        showNpcIcon(it, rider, place);
                    else
                        zge::changeSceneState(it.icon, 1, 0);
                }
                it.stateFrames[0] = static_cast<int16_t>(it.stateFrames[0] + 1);
            }
        }
        *g_npcFrames = *g_npcFrames + 1;
    }
    READABLE_FUNCTION(npcsFrame, Verified);

    namespace
    {
        // A rider counts as near an NPC within 5 units, as touching it
        // within 1.5.
        constexpr ee::Float kNearSquared = 25.0f;
        constexpr ee::Float kTouchSquared = 2.25f;
    }

    // nearestRiderToNpc (inferred name): the players' rider nearest the
    // NPC (both by their drawn place, position plus the root's offset)
    // within 5 units, its index in `nearest` (kNoNpc: none). kNpcTouching
    // when it is within 1.5 units and their velocities close the gap (or
    // keep it), kNpcNear otherwise.
    uint32_t nearestRiderToNpc(uint32_t npc, ee::Ptr<uint8_t> nearest)
    {
        const Npc &it = npcAt(npc);
        const rider::Rider *const npcRider = &*rider::riderOf(it.rider);
        uint8_t found = kNoNpc;
        ee::Float best = kNearSquared;
        zge::Vec4 offset;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const rider::Rider *const other = &*playerRider(player);
            zge::Vec4 apart;
            apart.x = (other->position[0] + other->rootOffset[0]) - (npcRider->position[0] + npcRider->rootOffset[0]);
            apart.y = (other->position[1] + other->rootOffset[1]) - (npcRider->position[1] + npcRider->rootOffset[1]);
            apart.z = (other->position[2] + other->rootOffset[2]) - (npcRider->position[2] + npcRider->rootOffset[2]);
            const ee::Float squared = zge::squaredLength(apart);
            if (squared < best)
            {
                found = riderIndexOf(player);
                best = squared;
                offset = apart;
            }
        }
        *nearest = found;
        if (found == kNoNpc)
            return kNpcNoneNear;
        if (!(best < kTouchSquared))
            return kNpcNear;
        const zge::Vec4 closing = *rider::vec4(rider::riderOf(found)->velocity) - *rider::vec4(npcRider->velocity);
        return zge::dot(offset, closing) <= ee::Float(0.0f) ? kNpcTouching : kNpcNear;
    }
    READABLE_FUNCTION(nearestRiderToNpc, Verified);

    namespace
    {

        // How long an NPC stands before it rides: the first time 2700
        // frames, after a meeting 300; in a sub-state of its own 900.
        constexpr uint32_t kFirstStandFrames = 2700, kStandAfterMeetingFrames = 300, kStandSubStateFrames = 900;
        // How many NPCs may ride (or wait, met) at once.
        inline ee::Global<uint32_t, sym::g_mostRidingNpcs> g_mostRidingNpcs;
        constexpr uint8_t kFirstBusyState = kNpcRiding, kBusyStatesEnd = 6;
        // The standing motions ("ground.stand.%02d"): every 1024 frames the
        // next of four, each for its first 64 frames, else the first.
        constexpr uint32_t kStandMotion = sym::g_textNpcStandMotion; // "ground.stand.%02d"
        constexpr uint32_t kStandMotionFrames = 0x1C0, kStandMotionShift = 10, kStandMotionMask = 3;
        inline ee::Global<ee::Float, sym::g_npcStandMotionRate> g_npcStandMotionRate; // 1/60 s: a frame
        // The game's NPC being shown (the NPC screen's, the camera's).
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;

        // When the motion before has played, the standing motion for the
        // state's frames started.
        void playStandingMotion(rider::Rider *rider, uint32_t frames, ee::Local<char[0x20]> &motionName)
        {
            if (!rider::hasFlag(rider, rider::kFlagMotionEnded))
                return;
            const uint32_t motion = (frames & kStandMotionFrames) != 0 ? 0 : (frames >> kStandMotionShift) & kStandMotionMask;
            zge::formatString(motionName, kStandMotion, motion);
            rider::riderMotionByName(motionName.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= rider::kMotionRestart;
        }

        ActionCamera *cameraOf(uint32_t player)
        {
            return &*ee::Ptr<ActionCamera>(playerCamera(player));
        }
    }

    namespace
    {
        // A rider who bounces off an NPC from below bails this way.
        constexpr uint8_t kBounceBail = 3; // Rider::bailKind
        // v less its part along `along` (a unit row) times `by`, lane by lane.
        void takeAlong(ee::Float *v, const zge::Vec4 &along, ee::Float by)
        {
            v[0] = v[0] - along.x * by;
            v[1] = v[1] - along.y * by;
            v[2] = v[2] - along.z * by;
        }
        void addAlong(ee::Float *v, const zge::Vec4 &along, ee::Float by)
        {
            v[0] = v[0] + along.x * by;
            v[1] = v[1] + along.y * by;
            v[2] = v[2] + along.z * by;
        }
    }

    // bounceRiderOffNpc (inferred name): a rider touching an NPC. The one
    // lower down (y grows downwards) loses its velocity along the line
    // between their drawn places and takes their closing speed the other
    // way, and bails: the rider (bail kind 3), or the NPC (kNpcBailed).
    // Returns that one's rider index.
    uint32_t bounceRiderOffNpc(uint32_t npc, uint32_t riderIndex)
    {
        npc &= 0xFF;
        riderIndex &= 0xFF;
        Npc &it = npcAt(npc);
        rider::Rider *const npcRider = &*rider::riderOf(it.rider);
        rider::Rider *const other = &*rider::riderOf(riderIndex);
        const zge::Vec4 npcPlace = *rider::vec4(npcRider->position) + *rider::vec4(npcRider->rootOffset);
        const zge::Vec4 otherPlace = *rider::vec4(other->position) + *rider::vec4(other->rootOffset);
        zge::Vec4 line = otherPlace - npcPlace;
        line = zge::unit(line);
        const zge::Vec4 closing = *rider::vec4(other->velocity) - *rider::vec4(npcRider->velocity);
        const ee::Float closingSpeed = zge::dot(closing, line);
        if (npcPlace.y < otherPlace.y)
        {
            const ee::Float along = zge::dot(*rider::vec4(other->velocity), line);
            takeAlong(other->velocity, line, along);
            takeAlong(other->velocity, line, closingSpeed);
            rider::setPhysicsState(&*rider::riderOf(riderIndex), rider::kPhysicsBail);
            rider::riderOf(riderIndex)->bailKind = kBounceBail;
            return riderIndex;
        }
        const ee::Float along = zge::dot(*rider::vec4(npcRider->velocity), line);
        takeAlong(npcRider->velocity, line, along);
        addAlong(npcRider->velocity, line, closingSpeed);
        setNpcState(npc, 0, kNpcBailed);
        return it.rider;
    }
    READABLE_FUNCTION(bounceRiderOffNpc, Verified);

    namespace
    {
        // A player meets an NPC within 5 units.
        constexpr ee::Float kMeetSquared = 25.0f;
        // The player's rider is put this far before the NPC (in the NPC's
        // rider's rows), facing it.
        constexpr zge::Vec4 kMeetingPlace{1.75f, 0.0f, 4.5f, 0.0f};
    }

    // meetNpc (inferred name): a player meets a standing NPC: within 5
    // units, with kFlagObjectiveAsked set, no timed objective of the player's
    // running, and the NPC's objective one the player can take now. The NPC
    // is put back at its start and met (its sub-state kept), its objective's
    // goal noted and its meetings counted; the player's rider is placed
    // before it, facing it, reset and run a frame. 1 when they met.
    uint32_t meetNpc(uint32_t npc, uint32_t player)
    {
        npc &= 0xFF;
        rider::Rider *const meeting = &*playerRider(player);
        Npc &it = npcAt(npc);
        const rider::Rider *const npcRider = &*rider::riderOf(it.rider);
        const zge::Vec4 apart = *rider::vec4(npcRider->position) - *rider::vec4(meeting->position);
        if (!(zge::squaredLength(apart) < kMeetSquared))
            return 0;
        if (!rider::hasFlag(meeting, rider::kFlagObjectiveAsked) || it.states[0] != kNpcStanding)
            return 0;
        if (objectives::riderObjectives(ee::Ptr<rider::Rider>(meeting))->timedObjective)
            return 0;
        const ee::Ptr<const char> objective(it.record->objectiveName);
        if (it.record->objectiveName[0] == 0 || !objectives::playerCanActivateObjectiveByName(static_cast<uint8_t>(player), objective))
            return 0;

        npcWaitFrame(npc);
        const uint8_t subState = npcState(npc, 1);
        setNpcState(npc, 0, kNpcMet);
        setNpcState(npc, 1, subState);
        const uint8_t current = *g_currentPlayer;
        const int32_t goal = objectives::goalIndexOf(current, zge::nameHash(it.record->objectiveName));
        it.meetings = static_cast<uint8_t>(it.meetings + 1);
        it.goal = static_cast<uint8_t>(goal);

        zge::Vec4 place = zge::transformed(*rider::matrix(npcRider->right), kMeetingPlace);
        place = place + *rider::vec4(npcRider->position);
        *rider::vec4(meeting->position) = place;
        *rider::vec4(meeting->drawnPosition) = place;
        *rider::vec4(meeting->forward) = *rider::vec4(npcRider->position) - *rider::vec4(meeting->position);
        *rider::vec4(meeting->forward) = zge::unit(*rider::vec4(meeting->forward));
        *rider::vec4(meeting->right) = zge::cross(*rider::vec4(meeting->up), *rider::vec4(meeting->forward));
        *rider::vec4(meeting->rowsOrigin) = zge::kZero;
        *rider::matrix(meeting->body) = *rider::matrix(meeting->right);
        rider::riderReset(riderIndexOf(player), 1);
        rider::riderFrame(riderIndexOf(player));
        return 1;
    }
    READABLE_FUNCTION(meetNpc, Verified);

    // npcStandFrame (state 0, inferred name): the NPC stands, playing the
    // standing motions, until its time is up; then it rides when fewer
    // than the most NPCs are busy. A rider near it who has not
    // kFlagNoPlayer set meets it (the NPC screen in play, else its
    // objective taken and it waits), or bounces off it when touching.
    // Returns 0 when the state changed and the new state's frame runs too.
    uint32_t npcStandFrame(uint32_t npc)
    {
        ee::Local<char[0x20]> motionName;
        ee::Local<uint8_t> nearest;
        npc &= 0xFF;
        Npc &it = npcAt(npc);
        rider::Rider *const rider = &*rider::riderOf(it.rider);
        uint32_t standFrames = kStandSubStateFrames;
        if (it.states[1] == 0)
            standFrames = it.meetings == 0 ? kFirstStandFrames : kStandAfterMeetingFrames;
        const uint32_t frames = static_cast<uint16_t>(it.stateFrames[0]);
        if (frames < standFrames)
        {
            playStandingMotion(rider, frames, motionName);
            rider::stepRiderMotion(rider, *g_npcStandMotionRate);
        }
        else
        {
            uint32_t busy = 0;
            for (uint32_t other = 0; other < kNpcs; ++other)
            {
                const uint8_t state = (*g_npcs)[other].states[0];
                if (state < kBusyStatesEnd && state >= kFirstBusyState)
                    ++busy;
            }
            if (busy < *g_mostRidingNpcs)
            {
                setReplayState(it.replay, kReplayPlaying);
                setNpcState(npc, 0, kNpcRiding);
                return 0;
            }
        }

        const uint32_t contact = nearestRiderToNpc(npc, nearest);
        if (contact >= kNpcContacts || contact == kNpcNoneNear)
            return 1;
        if (rider::hasFlag(&*rider::riderOf(*nearest), rider::kFlagNoPlayer))
            return 1;
        if (meetNpc(npc, rider::riderOf(*nearest)->player) != 0)
        {
            *g_shownNpc = static_cast<uint8_t>(npc);
            if (cameraState() == kCameraStatePlay)
            {
                showGameScreen(kScreenNpr);
                *g_modalPlayerNum = *g_currentPlayer;
                setCameraMode(cameraOf(*g_currentPlayer), 0, static_cast<uint32_t>(CameraMode::NpcAlone));
                return 1;
            }
            if (npcState(npc, 1) == 0)
                objectives::activatePlayerObjectiveByName(*g_currentPlayer, ee::Ptr<const char>(it.record->objectiveName));
            setNpcState(npc, 0, kNpcWaiting);
            ActionCamera *const camera = cameraOf(*g_currentPlayer);
            setCameraShot(camera, cameraShot(cameraOf(*g_currentPlayer)));
            return 1;
        }
        if (contact != kNpcTouching)
            return 1;
        bounceRiderOffNpc(npc, *nearest);
        return (*g_npcStateFrames)[it.states[0]] == ee::FunctionPtr<uint32_t(uint32_t)>(sym::npcStandFrame) ? 1 : 0;
    }
    READABLE_FUNCTION(npcStandFrame, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_npcMetMotionRate> g_npcMetMotionRate; // 1/60 s: a frame
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        // A bailed NPC lies 180 frames.
        constexpr int16_t kBailFrames = 180;
        // Npc::states[1] in state kNpcStanding: 0 when the player can take
        // the NPC's objective (as the NPC is put back), else 1.
        constexpr uint8_t kNpcObjectiveOpen = 0, kNpcObjectiveNotOpen = 1;
    }

    // npcMetFrame (state kNpcMet, inferred name): the NPC stands, playing
    // the standing motions.
    uint32_t npcMetFrame(uint32_t npc)
    {
        ee::Local<char[0x20]> motionName;
        const Npc &it = npcAt(npc);
        rider::Rider *const rider = &*rider::riderOf(it.rider);
        playStandingMotion(rider, static_cast<uint16_t>(it.stateFrames[0]), motionName);
        rider::stepRiderMotion(rider, *g_npcMetMotionRate);
        return 1;
    }
    READABLE_FUNCTION(npcMetFrame, Verified);

    // npcRideFrame (state kNpcRiding, inferred name): while its replay
    // plays, a rider touching the NPC bounces off it, and the NPC's bail
    // takes it to kNpcBailed; the replay's end takes it to kNpcRideOver.
    uint32_t npcRideFrame(uint32_t npc)
    {
        ee::Local<uint8_t> nearest;
        npc &= 0xFF;
        const Npc &it = npcAt(npc);
        const rider::Rider *const rider = &*rider::riderOf(it.rider);
        const uint8_t replay = replayState(it.replay);
        if (replay == kReplayPlaying)
        {
            if (nearestRiderToNpc(npc, nearest) == kNpcTouching)
            {
                bounceRiderOffNpc(npc, *nearest);
                if ((*g_npcStateFrames)[it.states[0]] != ee::FunctionPtr<uint32_t(uint32_t)>(sym::npcRideFrame))
                    return 0;
            }
            if (rider::actionState(rider, rider::kLevelAction) != rider::kActionBail)
                return 1;
            setNpcState(npc, 0, kNpcBailed);
            return 0;
        }
        if (replay == kReplayPlayedOut)
        {
            setNpcState(npc, 0, kNpcRideOver);
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(npcRideFrame, Verified);

    // npcBailFrame (state kNpcBailed, inferred name): the rider goes into
    // its bail (kFlagDowned set), runs its frames for 180 frames, then the
    // NPC's ride is over.
    uint32_t npcBailFrame(uint32_t npc)
    {
        npc &= 0xFF;
        const Npc &it = npcAt(npc);
        rider::Rider *const rider = &*rider::riderOf(it.rider);
        if (it.stateFrames[0] == 0)
        {
            rider::setPhysicsState(rider, rider::kPhysicsBail);
            rider::setFlag(rider, rider::kFlagDowned);
        }
        else if (it.stateFrames[0] == kBailFrames)
        {
            rider::clearFlag(rider, rider::kFlagDowned);
            setNpcState(npc, 0, kNpcRideOver);
            return 0;
        }
        rider::riderFrame(it.rider);
        return 1;
    }
    READABLE_FUNCTION(npcBailFrame, Verified);

    // npcRideOverFrame (state kNpcRideOver, inferred name): with no
    // sub-state the NPC goes to kNpcWaiting at once; else its bike is posed
    // on the ground where its replay left it, a rider touching it bounces
    // off it, and the sub-state's frames are counted.
    uint32_t npcRideOverFrame(uint32_t npc)
    {
        ee::Local<uint8_t> nearest;
        npc &= 0xFF;
        Npc &it = npcAt(npc);
        rider::Rider *const rider = &*rider::riderOf(it.rider);
        replayRiderPosition(it.replay);
        if (it.states[1] == 0)
        {
            setNpcState(npc, 0, kNpcWaiting);
            return 0;
        }
        rider::poseBikeGround(rider);
        if (nearestRiderToNpc(npc, nearest) == kNpcTouching)
        {
            bounceRiderOffNpc(npc, *nearest);
            if ((*g_npcStateFrames)[it.states[0]] != ee::FunctionPtr<uint32_t(uint32_t)>(sym::npcRideOverFrame))
                return 0;
        }
        it.stateFrames[1] = static_cast<int16_t>(it.stateFrames[1] + 1);
        return 1;
    }
    READABLE_FUNCTION(npcRideOverFrame, Verified);

    // npcWaitFrame (state kNpcWaiting, inferred name): the NPC's rider put
    // back where its replay starts (its rows and position), reset and
    // shown; the NPC stands, its objective open (sub-state 0) when, in the
    // first mode, the NPC has one the current player can take.
    uint32_t npcWaitFrame(uint32_t npc)
    {
        npc &= 0xFF;
        const Npc &it = npcAt(npc);
        rider::Rider *const rider = &*rider::riderOf(it.rider);
        *rider::matrix(rider->right) = *rider::matrix(replayRiderRows(it.replay));
        *rider::vec4(rider->position) = *rider::vec4(replayRiderPosition(it.replay));
        rider::riderReset(it.rider, 1);
        rider::riderFrame(it.rider);
        setNpcState(npc, 0, kNpcStanding);
        uint8_t objective = kNpcObjectiveNotOpen;
        if (gameMode() == kChallengeMode)
        {
            playerLevelGoals(*g_currentPlayer, *g_currentLevel);
            if (it.record->objectiveName[0] != 0 &&
                objectives::playerCanActivateObjectiveByName(*g_currentPlayer, ee::Ptr<const char>(it.record->objectiveName)))
                objective = kNpcObjectiveOpen;
        }
        setNpcState(npc, 1, objective);
        return 0;
    }
    READABLE_FUNCTION(npcWaitFrame, Verified);

    namespace
    {
        // The NPR models' names ("NPR%02d", 1 .. 17).
        constexpr uint32_t kNprModelName = sym::g_textNprModelName; // "NPR%02d"
        constexpr uint32_t kNprModels = 17;
    }

    namespace
    {
        // The twelve levels (the park editor's, 12, has no NPCs).
        constexpr uint8_t kLevelsWithNpcs = 12;
        // The NPCs' records, kNpcRecordsPerLevel a level.
        inline ee::Global<NpcRecord[kLevelsWithNpcs][kNpcRecordsPerLevel], sym::g_npcRecords> g_npcRecords;
        // The NPCs' replays' archive, and each one's asset name.
        constexpr ee::Ptr<const char> kReplayArchive{sym::g_textNpcReplayArchive};   // "assets\\replay.zal"
        constexpr uint32_t kTextFormat = sym::g_textNpcTextFormat;                    // "%s"
        constexpr uint32_t kNpcReplayName = sym::g_textNpcReplayName;                 // "NPR.Lev%s.Rider%02d"
        // The levels' table: each level's short name (the replays' names
        // take it).
        struct LevelNames
        {
            ee::Ptr<const char> shortName; // +0x00 ("MY")
            ee::Ptr<const char> folder;    // +0x04
            uint8_t _08[8];
        };
        static_assert(sizeof(LevelNames) == 0x10);
        inline ee::Global<LevelNames[kLevelsWithNpcs + 1], sym::g_levels> g_levelNames;
        // The NPCs' replays: of their rider, motion recordings of this size.
        constexpr uint8_t kNpcReplayFormat = 1;
        constexpr uint8_t kNpcReplayFlags = 2;
        constexpr uint32_t kNpcReplayBytes = 0x10800;
        // The rider characters a player can pick (the first 13).
        constexpr uint8_t kPickableCharacters = 13;
        // The multiplayer session (player_riders.h's kinds; inferred: no NPCs
        // ride in it).
        constexpr uint8_t kMultiplayerSession = 3;
        // An NPC's rider freed again (its model record too), when its replay
        // cannot be had.
        void dropNpcRider(Npc &it)
        {
            rider::freeRider(it.rider);
            it.rider = kNoNpc;
            zge::freeModelRecord(it.model);
        }
    }

    namespace
    {
        // The models' definitions' archive, and the level's NPR models'.
        constexpr ee::Ptr<const char> kDefinitionsArchive{sym::g_textModelDefinitionsArchive}; // "assets\\create\\defs.zal"
        constexpr uint32_t kNprDefinitionName = sym::g_textNprDefinitionName;                   // "def.NPR.%s"
    }

    // loadNprModels (inferred name): the level's NPR models' definitions
    // ("def.NPR.<level folder>" of the definitions archive) read from a copy
    // in memory, when the archive has them.
    void loadNprModels()
    {
        ee::Local<char[0x40]> name;
        const uint32_t library = zge::openAssetLibrary(kDefinitionsArchive);
        zge::formatString(name, kNprDefinitionName, (*g_levelNames)[*g_currentLevel].folder);
        if (zge::findAsset(library, zge::nameHash(&name[0])))
        {
            const uint32_t bytes = zge::currentAssetLength(library);
            const uint32_t buffer = zge::allocateMemory(bytes);
            zge::readAsset(library, buffer, 0);
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(buffer, bytes);
            zge::loadModelScript(ee::Address(file));
            zge::closeFile(file, 0);
            zge::freeMemory(buffer);
        }
        zge::closeAssetLibrary(library);
    }
    READABLE_FUNCTION(loadNprModels, Verified);

    // setupNpcs (DM2's name): with no NPCs in multiplayer or the park
    // editor, the list is empty. Else the level's NPR models are loaded and
    // each of the level's NPC records with a rider model makes an NPC: its
    // record (the stand-in record when the NPC is the current player's own
    // character: its rider and model are the stand-in's too), its rider
    // from its NPR model, its icon, and its replay
    // from the replays' archive ("NPR.Lev<level>.Rider<n>"); an NPC whose
    // model, rider or replay cannot be had is dropped. The NPCs start
    // stopped.
    uint32_t setupNpcs()
    {
        ee::Local<NewRider> request;
        ee::Local<ReplaySetup> replaySetup;
        ee::Local<char[0x40]> name;
        ee::Local<uint32_t> savedPart;
        ee::Local<uint32_t> savedBytes;
        if (*g_sessionKind == kMultiplayerSession || *g_currentLevel >= kLevelsWithNpcs)
        {
            (*g_npcs)[0].rider = kNoNpc;
            return 1;
        }
        loadNprModels();
        const uint32_t library = zge::openAssetLibrary(kReplayArchive);
        zge::memoryFill(request, 0, sizeof(NewRider));
        request->flags |= kRiderFromReplay | kRiderNpc;
        for (uint32_t npc = 0; npc < kNpcs; ++npc)
        {
            Npc &it = (*g_npcs)[npc];
            NpcRecord *slot = &(*g_npcRecords)[*g_currentLevel][npc];
            if (!slot->riderModel || slot->riderModel[0] == 0)
            {
                it.rider = kNoNpc;
                break;
            }
            const uint8_t picked = playerSetup(*g_currentPlayer)->character;
            if (picked < kPickableCharacters && picked == slot->character)
            {
                NpcRecord &standIn = (*g_npcRecords)[*g_currentLevel][kStandInRecord];
                zge::stringCopy(standIn.objectiveName, slot->objectiveName);
                slot = &standIn;
                it.record = ee::Ptr<const NpcRecord>(slot);
            }
            else
            {
                it.record = ee::Ptr<const NpcRecord>(slot);
            }
            zge::formatString(request->riderModel, kTextFormat, slot->riderModel);
            zge::formatString(request->bikeModel, kTextFormat, slot->bikeModel);
            zge::formatString(name, kNprModelName, slot->character + 1u);
            it.model = zge::findModelByName(&name[0]);
            if (!it.model)
                continue;
            request->model = it.model;
            it.rider = rider::newRider(request);
            if (it.rider == kNoNpc)
            {
                zge::freeModelRecord(it.model);
                continue;
            }
            rider::riderOf(it.rider);
            if ((*g_objectiveIcons)[0])
                it.icon = zge::cloneScene(zge::sceneOf((*g_objectiveIcons)[0]), 0);
            else
                it.icon = {};

            replaySetup->format = kNpcReplayFormat;
            replaySetup->flags = kNpcReplayFlags;
            replaySetup->rider = it.rider;
            replaySetup->weapon = 0;
            replaySetup->dataBytes = kNpcReplayBytes;
            it.replay = newReplay(replaySetup);
            if (it.replay == kNoReplay)
            {
                dropNpcRider(it);
                continue;
            }
            replaySavedPart(it.replay, savedPart, savedBytes);
            zge::formatString(name, kNpcReplayName, (*g_levelNames)[*g_currentLevel].shortName, npc);
            if (zge::findAsset(library, zge::nameHash(&name[0])) && !(*savedBytes < zge::currentAssetLength(library)))
            {
                readLibraryReplay(ee::Ptr<zge::ZfileLibrary>(library), *savedPart);
                continue;
            }
            freeReplay(it.replay);
            it.replay = kNoReplay;
            dropNpcRider(it);
        }
        zge::closeAssetLibrary(library);
        *g_npcFrames = 0;
        *g_npcPlay = 0xFF;
        setNpcPlay(kNpcPlayStopped);
        return 1;
    }
    READABLE_FUNCTION(setupNpcs, Verified);

    // freeNprModels (inferred name): every NPR model the engine has loaded
    // freed, by its name.
    void freeNprModels()
    {
        ee::Local<char[0x40]> name;
        for (uint32_t number = 1; number <= kNprModels; ++number)
        {
            zge::formatString(name, kNprModelName, number);
            if (const ee::Ptr<zge::ModelRecord> model = zge::findModelByName(&name[0]))
                zge::freeModelRecord(model);
        }
    }
    READABLE_FUNCTION(freeNprModels, Verified);

    // freeNpcs (DM2's name): the NPCs stopped, then each one's replay, its
    // icon, its rider and its model freed, and the NPR models.
    void freeNpcs()
    {
        setNpcPlay(kNpcPlayStopped);
        for (uint32_t npc = 0; npc < kNpcs && (*g_npcs)[npc].rider != kNoNpc; ++npc)
        {
            Npc &it = (*g_npcs)[npc];
            freeReplay(it.replay);
            it.replay = kNoReplay;
            if (it.icon)
                zge::releaseScene(it.icon);
            rider::freeRider(it.rider);
            it.rider = kNoNpc;
            zge::freeModelRecord(it.model);
            it.model = {};
        }
        game::tail(freeNprModels);
    }
    READABLE_FUNCTION(freeNpcs, Verified);
}
