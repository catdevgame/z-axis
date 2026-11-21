// The ground's object (game objects of kind 1; gamehand12, were the
// sweep's drafts of groundObjectGetScore .. setObjectState): the trick object the skater
// has on the ground (made by the ground states' trick starter,
// state_tricks.cpp, and by the controls' steps), which picks the skater's
// ground animation: idle, coasting, pushing, braking, turning (left turns
// the right turn's animation mirrored), crouched, landing and the
// transfer to and from fakie. Its state block is (state, step): a row of
// the table groundSteps (five states of seven steps) holds each pair's
// handlers and animations; setObjectState moves between them.
//

#include "action_tricks.h"
#include "skater.h"

#include "aggressive/objects.h"
#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "aggressive/pads.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/skater/ground_object.cpp.h"

namespace aggressive::skater
{

    using namespace action_tricks;

    namespace
    {

        // The ground object's states and steps (its state block's pair).
        enum GroundState : uint32_t
        {
            kGroundIdle = 0,     // standing ("idle")
            kGroundRolling = 1,  // "coast", "skate", "brake", "turn", "sharpTurn"
            kGroundCrouched = 2, // the same crouched ("crouch", "crouchSkate")
            kGroundLanding = 3,  // "land"
            kGroundTransfer = 4, // "transferToFakie", "transferFromFakie"
            kGroundStateCount = 5,
            kGroundNone = 5,     // a new object's: none yet (setObjectState enters any)
        };
        enum GroundStep : uint32_t
        {
            kStepCoast = 0,
            kStepPush = 1,
            kStepBrake = 2,
            kStepTurnLeft = 3,   // the right turn's animation mirrored
            kStepTurnRight = 4,
            kStepSharpLeft = 5,  // with pad bit 0x200 or 0x100 held (the sliding turn)
            kStepSharpRight = 6,
            kGroundStepCount = 7,
            kStepNone = 7,
        };

        // A row of the table (0x34 bytes): the pair's handlers (0: none),
        // its animations by name (found at the class's setup) and as found.
        using StepUpdate = ee::FunctionPtr<void(ee::Ptr<GameObject> object, ee::Ptr<Player> player, ee::Float seconds)>;
        using StepEnded = ee::FunctionPtr<void(ee::Ptr<GameObject> object, uint32_t ended)>;
        struct GroundStepRow
        {
            // Its animation played, told the pair left.
            ee::FunctionPtr<void(ee::Ptr<GameObject> object, uint32_t player, uint32_t oldState, uint32_t oldStep)> enter;
            // 0: not yet (landing, transfer).
            ee::FunctionPtr<uint32_t(ee::Ptr<GameObject> object, uint32_t player, uint32_t state, uint32_t step)> canLeave;
            ee::FunctionPtr<void(ee::Ptr<GameObject> object, uint32_t player, uint32_t state, uint32_t step)> leave;
            // TrickUpdate.
            StepUpdate update;
            StepEnded animationEnded;
            uint32_t animationName;       // +0x14 "global_assets.cedwards.skate"
            ee::Ptr<zge::Motion> animation;           // +0x18
            uint32_t entryName;           // +0x1C "global_assets.cedwards.idleToSkate" (0: none)
            ee::Ptr<zge::Motion> entryAnimation;      // +0x20
            uint32_t fakieName;           // +0x24 "global_assets.cedwards.skateFakie" (0: the animation's)
            ee::Ptr<zge::Motion> fakieAnimation;      // +0x28
            uint32_t fakieEntryName;      // +0x2C (0: the entry's)
            ee::Ptr<zge::Motion> fakieEntryAnimation; // +0x30
        };
        static_assert(sizeof(GroundStepRow) == 0x34 && offsetof(GroundStepRow, animationName) == 0x14);
        static_assert(offsetof(GroundStepRow, fakieEntryAnimation) == 0x30);
        inline ee::Global<GroundStepRow[kGroundStateCount][kGroundStepCount], sym::groundSteps> g_groundSteps;

        // A pair's row (kGroundNone's is past the table: read, as the game does).
        ee::Ptr<GroundStepRow> stepRow(uint32_t state, uint32_t step)
        {
            return ee::Ptr<GroundStepRow>((*g_groundSteps)[0]) + state * kGroundStepCount + step;
        }

        // A state and step the input asks for (readGroundObjectInput).
        struct GroundPair
        {
            uint32_t state;
            uint32_t step;
        };

        // The layout's button +0x0C (the crouch).

        // The object's state block (0x10 bytes from the pool).
        struct GroundObjectState
        {
            uint32_t state;      // +0x00 GroundState
            uint32_t step;       // +0x04 GroundStep
            uint32_t player;     // +0x08 (TrickStarted's)
            ee::Float holdTime;  // +0x0C the crouch's animation time, kept while held
        };
        static_assert(sizeof(GroundObjectState) == 0x10);
        inline ee::Global<uint32_t, sym::g_groundObjectPool> g_groundObjectPool; // 5 blocks

        // The transfer's "FrameFlag" event: its data 5 flips the stance.
        constexpr uint32_t kFrameFlagName = sym::frameFlagName; // "FrameFlag"
        constexpr uint32_t kStanceFlipped = 5;
        // The crouch let go: its animation time steps back by these.
        inline ee::Global<ee::Float, sym::g_halfFrameSeconds> g_halfFrameSeconds; // 1 / 120
        inline ee::Global<ee::Float, sym::g_frameSeconds> g_frameSeconds;     // 1 / 60
        // The idle animation played again near its end (its time past
        // 32400 - 324).
        inline ee::Global<ee::Float, sym::g_idleEnd> g_idleEnd;          // 32400
        constexpr float kIdleRestartLead = 324.0f;

        constexpr uint32_t kSharpTurnBits[2] = {0x200, 0x100}; // the pad bits of the sliding turn
        constexpr uint32_t kPlayMode = 5;                       // playAnimation's mode

        // Control / Controller Deadzone (tuning category 5, entry 0).
        ee::Float controllerDeadzone()
        {
            return (*g_tuningCategories)[kTuningControl].entries[0].value;
        }
    }

    void playObjectStateAnimation(ee::Ptr<GameObject> object, ee::Ptr<Player> player);
    void readGroundObjectInput(ee::Ptr<Player> player, ee::Ptr<uint32_t> stateOut, ee::Ptr<uint32_t> stepOut);
    uint32_t freeGroundObject(ee::Ptr<GameObject> object);
    uint32_t setObjectState(ee::Address player, ee::Ptr<GameObject> object, uint32_t state, uint32_t step);

    // The handlers (the class's message table).

    // groundObjectGetScore ("getScore"): none.
    uint32_t groundObjectGetScore(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(groundObjectGetScore, Verified);

    // groundObjectGetMultiplierBonus ("getMultiplierBonus"): none.
    uint32_t groundObjectGetMultiplierBonus(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, 0);
    }
    READABLE_FUNCTION(groundObjectGetMultiplierBonus, Verified);

    // groundObjectGetName ("getName"): none (not taken).
    uint32_t groundObjectGetName()
    {
        return 0;
    }
    READABLE_FUNCTION(groundObjectGetName, Verified);

    // groundObjectAreYouPossible ("areYouPossible", the player): on the ground (Ground or
    // Stand) and not down; during a transfer, only when the skater's
    // current object agrees it is finished.
    uint32_t groundObjectAreYouPossible(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<uint32_t> finished;
        const ee::Ptr<Player> player(note->argument);
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        uint32_t possible = player->state == State::Ground || player->state == State::Stand ? 1 : 0;
        if (state->state == kGroundTransfer)
        {
            if (possible == 0)
                return 0;
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            if (current != 0)
            {
                *finished = 0;
                sendObjectMessage(current, *g_msgAreYouFinished, player, finished);
                possible = *finished;
            }
        }
        uint32_t result = 0;
        if (possible != 0)
            result = (player->flags & kDown) == 0 ? 1 : 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(groundObjectAreYouPossible, Verified);

    // groundObjectAreYouFinished ("areYouFinished"): always.
    uint32_t groundObjectAreYouFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(groundObjectAreYouFinished, Verified);

    // groundObjectOnTrickStarted ("TrickStarted", the player): the object is the
    // player's; landing, its animation played; else the pair the input
    // asks for entered.
    uint32_t groundObjectOnTrickStarted(ee::Ptr<ObjectMessageNote> note)
    {
        ee::Local<GroundPair> wanted;
        const ee::Ptr<Player> player(note->argument);
        wanted->state = kGroundNone;
        wanted->step = kStepNone;
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        const uint32_t current = state->state;
        state->player = note->argument;
        if (current == kGroundLanding)
            playObjectStateAnimation(note->object, player);
        else
            readGroundObjectInput(player, &wanted->state, &wanted->step);
        setObjectState(note->argument, note->object, wanted->state, wanted->step);
        return 1;
    }
    READABLE_FUNCTION_WAITS(groundObjectOnTrickStarted, Verified);

    // groundObjectOnTrickFinished ("TrickFinished"): nothing to do.
    uint32_t groundObjectOnTrickFinished()
    {
        return 1;
    }
    READABLE_FUNCTION(groundObjectOnTrickFinished, Verified);

    // groundObjectSpaceNeededForSaving ("GetSpaceNeededForSaving"): the state block.
    uint32_t groundObjectSpaceNeededForSaving(ee::Ptr<ObjectMessageNote> note)
    {
        return answerWord(note, sizeof(GroundObjectState));
    }
    READABLE_FUNCTION(groundObjectSpaceNeededForSaving, Verified);

    // groundObjectSaveToBuffer ("SaveToBuffer"): the state block copied out.
    uint32_t groundObjectSaveToBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<SaveRequest> request(note->argument);
        const uint32_t state = gameObjectState(note->object);
        zge::memoryCopy(request->buffer, state, static_cast<uint32_t>(sizeof(GroundObjectState)));
        return 1;
    }
    READABLE_FUNCTION(groundObjectSaveToBuffer, Verified);

    // groundObjectLoadFromBuffer ("LoadFromBuffer"): a new state block from the pool,
    // copied in.
    uint32_t groundObjectLoadFromBuffer(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<SaveRequest> request(note->argument);
        const uint32_t state = poolTake(*g_groundObjectPool);
        zge::memoryCopy(state, request->buffer, static_cast<uint32_t>(sizeof(GroundObjectState)));
        aggressive::setGameObjectState(note->object, state, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(groundObjectLoadFromBuffer, Verified);

    // groundObjectOnTrickUpdate ("TrickUpdate"): the pair's update handler.
    uint32_t groundObjectOnTrickUpdate(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<TrickUpdate> update(note->argument);
        const ee::Ptr<Player> player = update->player;
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        const StepUpdate handler = stepRow(state->state, state->step)->update;
        if (handler)
            handler(note->object, player, update->seconds);
        return 1;
    }
    READABLE_FUNCTION(groundObjectOnTrickUpdate, Verified);

    // groundObjectOnTrickStartHolding ("TrickStartHolding", {player, time}): the time kept.
    uint32_t groundObjectOnTrickStartHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        state->holdTime = ee::Ptr<TrickUpdate>(note->argument)->seconds;
        return 1;
    }
    READABLE_FUNCTION(groundObjectOnTrickStartHolding, Verified);

    // groundObjectOnTrickStoppedHolding ("TrickStoppedHolding", {player, time}): crouched with
    // the button still held, nothing; else the kept time steps back a frame
    // and a half (not below 0) and the skater's animation is put there.
    uint32_t groundObjectOnTrickStoppedHolding(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        const ee::Ptr<TrickUpdate> holding(note->argument);
        if (state->state == kGroundCrouched)
        {
            const uint32_t held = aggressive::layoutButtonHeld(&ee::Ptr<Player>(holding->player)->controls);
            if (held == 0)
                return 1;
        }
        const ee::Float time = state->holdTime;
        const ee::Float back = time - *g_halfFrameSeconds - *g_frameSeconds;
        state->holdTime = back;
        if (back < ee::Float(0.0f))
            state->holdTime = 0.0f;
        const uint32_t result = zge::setAnimationTime(ee::Ptr<Player>(holding->player)->node, state->holdTime);
        return result;
    }
    READABLE_FUNCTION_WAITS(groundObjectOnTrickStoppedHolding, Verified);

    // groundObjectOnAnimationEnded ("AnimationEnded", an AnimationEnd): the pair's handler.
    uint32_t groundObjectOnAnimationEnded(ee::Ptr<ObjectMessageNote> note)
    {
        const ee::Ptr<GroundObjectState> state(gameObjectState(note->object));
        const StepEnded handler = stepRow(state->state, state->step)->animationEnded;
        if (handler)
            handler(note->object, note->argument);
        return 1;
    }
    READABLE_FUNCTION(groundObjectOnAnimationEnded, Verified);

    // groundObjectOnTrickDroppedFromQueue ("TrickDroppedFromQueue"): the object freed.
    uint32_t groundObjectOnTrickDroppedFromQueue(ee::Ptr<ObjectMessageNote> note)
    {
        return freeGroundObject(note->object);
    }
    READABLE_FUNCTION(groundObjectOnTrickDroppedFromQueue, Verified);

    // The pairs' handlers (the table's).

    // playObjectStateAnimation (an enter): the pair's animation (fakie's when fakie).
    void playObjectStateAnimation(ee::Ptr<GameObject> object, ee::Ptr<Player> player)
    {
        const ee::Ptr<GroundObjectState> state(gameObjectState(object));
        const ee::Ptr<GroundStepRow> row = stepRow(state->state, state->step);
        ee::Ptr<zge::Motion> animation;
        if (player->fakie != 0)
            animation = row->fakieAnimation;
        else
            animation = row->animation;
        if (animation != 0)
            game::tail(zge::playAnimation, player->node, animation, kPlayMode, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(playObjectStateAnimation, Verified);

    // playGroundStepEntryAnimation (the left turns' enter): the pair's animation mirrored.
    void playGroundStepEntryAnimation(ee::Ptr<GameObject> object, ee::Ptr<Player> player)
    {
        const ee::Ptr<GroundObjectState> state(gameObjectState(object));
        const ee::Ptr<GroundStepRow> row = stepRow(state->state, state->step);
        ee::Ptr<zge::Motion> animation;
        if (player->fakie != 0)
            animation = row->fakieAnimation;
        else
            animation = row->animation;
        if (animation != 0)
            game::tail(zge::playAnimation, player->node, animation, kHalfMirrored, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(playGroundStepEntryAnimation, Verified);

    // playGroundStepAnimation (the push's and the crouch's enter, `oldState` the state
    // left): from idle the pair's entry animation (idle to skate), from any
    // other state the pair's animation.
    void playGroundStepAnimation(ee::Ptr<GameObject> object, ee::Ptr<Player> player, uint32_t oldState)
    {
        if (oldState != 0)
            return game::tail(playObjectStateAnimation, object, player);
        const ee::Ptr<GroundObjectState> state(gameObjectState(object));
        const ee::Ptr<GroundStepRow> row = stepRow(state->state, state->step);
        ee::Ptr<zge::Motion> animation;
        if (player->fakie != 0)
            animation = row->fakieEntryAnimation;
        else
            animation = row->entryAnimation;
        if (animation != 0)
            zge::playAnimation(player->node, animation, kPlayMode, animationBlendTime(), kPlayOnce);
    }
    READABLE_FUNCTION_WAITS(playGroundStepAnimation, Verified);

    // groundObjectStepInput (the rolling and crouched states' update): the step the
    // input asks for, in the same state (idle when the skater stands).
    void groundObjectStepInput(ee::Ptr<GameObject> object, ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> stepOut;
        *stepOut = kStepCoast;
        const ee::Ptr<GroundObjectState> state(gameObjectState(object));
        const uint32_t current = state->state;
        readGroundObjectInput(player, ee::Ptr<uint32_t>(), stepOut);
        setObjectState(player, object, player->state != State::Stand ? current : static_cast<uint32_t>(kGroundIdle), *stepOut);
    }
    READABLE_FUNCTION_WAITS(groundObjectStepInput, Verified);

    // readGroundObjectInput: what the input asks for. The state: idle when the
    // skater stands, else rolling, crouched with the layout's crouch button
    // held. The step: the stick across past the deadzone turns (sharp with
    // a sliding turn button held), pushing pushes, else the stick pulled
    // back past the deadzone brakes (not standing); else coasting.
    void readGroundObjectInput(ee::Ptr<Player> player, ee::Ptr<uint32_t> stateOut, ee::Ptr<uint32_t> stepOut)
    {
        if (stateOut)
        {
            if (player->state == State::Stand)
                *stateOut = kGroundIdle;
            else
                *stateOut = kGroundRolling;
            if (aggressive::layoutButtonHeld(&player->controls) != 0)
                *stateOut = kGroundCrouched;
        }
        if (!stepOut)
            return;
        uint32_t sharp = 0;
        const ee::Float deadzone = controllerDeadzone();
        bool held = padHeld(kSharpTurnBits[0], player->controls.port) != 0;
        if (!held)
            held = padHeld(kSharpTurnBits[1], player->controls.port) != 0;
        if (held)
            sharp = 1;
        *stepOut = kStepCoast;
        const ee::Float across = player->controls.stickX;
        bool turning = true;
        uint32_t step = 0;
        if (deadzone < across)
            step = sharp ? kStepSharpRight : kStepTurnRight;
        else
        {
            const ee::Float left = -deadzone;
            if (across < left)
                step = sharp ? kStepSharpLeft : kStepTurnLeft;
            else
                turning = false;
        }
        if (turning)
            *stepOut = step;
        const uint8_t pushing = player->pushing;
        if (pushing == 0)
        {
            if (!(deadzone < player->controls.stickY) || player->state == State::Stand)
                return;
            step = kStepBrake;
        }
        else
            step = kStepPush;
        *stepOut = step;
    }
    READABLE_FUNCTION(readGroundObjectInput, Verified);

    // groundObjectStepFrameFlag (the landing's and the transfer's animation end): the
    // pair the input asks for entered; the transfer's "FrameFlag" event
    // (data 5) flips the skater's stance first. When the pair cannot be
    // entered, the object is made new (none) and entered again.
    void groundObjectStepFrameFlag(ee::Ptr<GameObject> object, ee::Ptr<AnimationEnd> ended)
    {
        ee::Local<GroundPair> wanted;
        const ee::Ptr<GroundObjectState> state(gameObjectState(object));
        const ee::Ptr<Player> player(ended->player);
        readGroundObjectInput(player, &wanted->state, &wanted->step);
        if (zge::eventInt(ee::Ptr<zge::AnimationEvent>(ended->event), zge::nameHash(ee::Ptr<const char>(kFrameFlagName))) == kStanceFlipped) // the event's word
        {
            zge::stopAnimation(player->node);
            player->fakie = player->fakie < 1 ? 1 : 0;
            player->drawnRefresh = 1;
        }
        if (setObjectState(ended->player, object, wanted->state, wanted->step) == 0)
        {
            state->state = kGroundNone;
            state->step = kStepNone;
            setObjectState(ended->player, object, wanted->state, wanted->step);
        }
    }
    READABLE_FUNCTION_WAITS(groundObjectStepFrameFlag, Verified);

    // groundObjectStepNone (the landing's and the transfer's canLeave): not until
    // their animation ends.
    uint32_t groundObjectStepNone()
    {
        return 0;
    }
    READABLE_FUNCTION(groundObjectStepNone, Verified);

    // groundObjectStepStand (the idle state's update): off Stand, the pair the input
    // asks for; standing, the idle animation started again near its end.
    void groundObjectStepStand(ee::Ptr<GameObject> object, ee::Ptr<Player> player)
    {
        ee::Local<GroundPair> wanted;
        ee::Local<ee::Float> time;
        wanted->state = kGroundNone;
        wanted->step = kStepCoast;
        if (player->state != State::Stand)
        {
            readGroundObjectInput(player, &wanted->state, &wanted->step);
            setObjectState(player, object, wanted->state, wanted->step);
            return;
        }
        *time = 0.0f;
        zge::animationTime(player->node, time);
        if (*g_idleEnd < *time + ee::Float(kIdleRestartLead))
        {
            zge::stopAnimation(player->node);
            playObjectStateAnimation(object, player);
        }
    }
    READABLE_FUNCTION_WAITS(groundObjectStepStand, Verified);

    // registerGroundObjectClass (the skater's setup): the class registered with its
    // pool (five state blocks) and handlers, and every row's animations
    // found by name (a fakie one missing is the row's own). Returns the
    // last find's result (the setup fails on 0).
    uint32_t registerGroundObjectClass()
    {
        uint32_t state = 0;
        *g_groundObjectPool = newPool(static_cast<uint32_t>(sizeof(GroundObjectState)), kGroundStateCount, 0);
        aggressive::registerObjectClass(kObjectGround);
        uint32_t result = 0;
        const auto handle = [&](uint32_t message, uint32_t handler) {
            result = aggressive::setClassHandler(kObjectGround, message, handler);
        };
        handle(*g_msgGetScore, sym::groundObjectGetScore);
        handle(*g_msgGetMultiplierBonus, sym::groundObjectGetMultiplierBonus);
        handle(*g_msgGetName, sym::groundObjectGetName);
        handle(*g_msgAreYouPossible, sym::groundObjectAreYouPossible);
        handle(*g_msgAreYouFinished, sym::groundObjectAreYouFinished);
        handle(*g_msgGetSpaceNeededForSaving, sym::groundObjectSpaceNeededForSaving);
        handle(*g_msgSaveToBuffer, sym::groundObjectSaveToBuffer);
        handle(*g_msgLoadFromBuffer, sym::groundObjectLoadFromBuffer);
        handle(*g_msgTrickStarted, sym::groundObjectOnTrickStarted);
        handle(*g_msgTrickFinished, sym::groundObjectOnTrickFinished);
        handle(*g_msgTrickUpdate, sym::groundObjectOnTrickUpdate);
        handle(*g_msgTrickStartHolding, sym::groundObjectOnTrickStartHolding);
        handle(*g_msgTrickStoppedHolding, sym::groundObjectOnTrickStoppedHolding);
        handle(*g_msgAnimationEnded, sym::groundObjectOnAnimationEnded);
        handle(*g_msgTrickDroppedFromQueue, sym::groundObjectOnTrickDroppedFromQueue);
        for (;;)
        {
            const uint32_t nextState = state + 1;
            for (uint32_t step = 0;;)
            {
                GroundStepRow &row = (*g_groundSteps)[state][step];
                if (row.animationName != 0)
                    result = zge::findAnimation(row.animationName, &row.animation);
                else
                    row.animation = 0;
                if (row.entryName != 0)
                    result = zge::findAnimation(row.entryName, &row.entryAnimation);
                else
                    row.entryAnimation = 0;
                if (row.fakieName != 0)
                    result = zge::findAnimation(row.fakieName, &row.fakieAnimation);
                else
                    row.fakieAnimation = row.animation;
                if (row.fakieEntryName != 0)
                    result = zge::findAnimation(row.fakieEntryName, &row.fakieEntryAnimation);
                else
                    row.fakieEntryAnimation = row.entryAnimation;
                step = step + 1;
                if (!(step < kGroundStepCount))
                    break;
            }
            state = nextState;
            if (!(state < kGroundStateCount))
                return result;
        }
    }
    READABLE_FUNCTION_WAITS(registerGroundObjectClass, Verified);

    // unregisterGroundObjectClass (the skater's shutdown): the pool freed (0 when it
    // could not be) and the class unregistered.
    uint32_t unregisterGroundObjectClass()
    {
        if (freePool(*g_groundObjectPool) == 0)
            return 0;
        *g_groundObjectPool = 0;
        const uint32_t result = static_cast<uint32_t>(aggressive::unregisterObjectClass(kObjectGround));
        return result;
    }
    READABLE_FUNCTION_WAITS(unregisterGroundObjectClass, Verified);

    // newGroundObjectForAir: a ground object landing (the air states' landing).
    ee::Ptr<GameObject> newGroundObjectForAir()
    {
        const uint32_t block = poolTake(*g_groundObjectPool);
        const ee::Ptr<GroundObjectState> state(block);
        const ee::Ptr<GameObject> object = aggressive::newGameObject(kObjectGround, block);
        state->holdTime = 0.0f;
        state->state = kGroundLanding;
        state->step = kStepCoast;
        state->player = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newGroundObjectForAir, Verified);

    // newGroundObject: a ground object with no pair yet.
    ee::Ptr<GameObject> newGroundObject()
    {
        const uint32_t block = poolTake(*g_groundObjectPool);
        const ee::Ptr<GroundObjectState> state(block);
        const ee::Ptr<GameObject> object = aggressive::newGameObject(kObjectGround, block);
        state->state = kGroundNone;
        state->step = kStepNone;
        state->holdTime = 0.0f;
        state->player = 0;
        return object;
    }
    READABLE_FUNCTION_WAITS(newGroundObject, Verified);

    // freeGroundObject: its state block back to the pool, the object's cleared.
    uint32_t freeGroundObject(ee::Ptr<GameObject> object)
    {
        const uint32_t state = gameObjectState(object);
        const uint32_t pool = *g_groundObjectPool;
        if (state != 0)
            poolGive(pool, state);
        aggressive::setGameObjectState(object, 0, 0);
        return 1;
    }
    READABLE_FUNCTION(freeGroundObject, Verified);

    // setObjectState (player, object; either 0: the other's): the ground
    // object moved to (state, step): nothing when it is there already; the
    // pair left (its canLeave may refuse: 0), then the new one entered (its
    // enter told the pair left). Returns 1 when moved.
    uint32_t setObjectState(ee::Address player, ee::Ptr<GameObject> object, uint32_t state, uint32_t step)
    {
        if (player != 0 && object == 0)
            object = skaterCurrentObject(ee::Ptr<Player>(player));
        const ee::Ptr<GroundObjectState> block(gameObjectState(object));
        if (player == 0)
            player = block->player;
        const uint32_t oldState = block->state;
        const uint32_t oldStep = block->step;
        if (oldState == state && oldStep == step)
            return 0;
        if (oldState != kGroundNone)
        {
            const ee::Ptr<GroundStepRow> old = stepRow(oldState, oldStep);
            if (old->canLeave && old->canLeave(object, player, state, step) == 0)
                return 0;
            if (old->leave)
                old->leave(object, player, state, step);
        }
        block->step = step;
        block->state = state;
        const ee::Ptr<GroundStepRow> row = stepRow(state, step);
        if (row->enter)
            row->enter(object, player, oldState, oldStep);
        return 1;
    }
    READABLE_FUNCTION(setObjectState, Verified);
}
