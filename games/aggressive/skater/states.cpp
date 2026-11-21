// The skater's state machine itself (skater.h State, StateRow): changing
// state (the old state's exit, the new one's enter, the scoring told), a
// move asked of the current state (its StateRow::request handler), and the
// small enters, exits and handlers of the Ground, Stand, Air, Air QPipe,
// Grind and Manual states.
//
// A move request comes from the trick input (it reads the
// button table: 4 and 5 grind tricks, 6 the double grind, 15 a manual, 18
// a cess slide) or the action button (10: grab a pole, vault, skitch).
// The state's handler enters the state the move leads to and returns it,
// or -1 when it refuses.

#include "skater.h"
#include "aggressive/level/double_rails.h"

#include "aggressive/camera.h"
#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sce/libgcc.h"
#include "moves.h"
#include "trick_objects.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/library.h"
#include "aggressive/game_start.h"
#include "aggressive/hud/hud.h"

#include "sym/aggressive/skater/states.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kImpactSound = 0x31;
        constexpr uint32_t kNoSurface = 0xFFFFFFFFu;

        // The objects' events.
        constexpr uint32_t kOnEnd = sym::onEnd;         // "OnEnd"
        constexpr uint32_t kManualEnd = sym::manualEnd; // "ManualEnd"
        constexpr uint32_t kOverEnd = sym::overEnd;     // "OverEnd"
        constexpr uint32_t kGrindEnd = sym::grindEnd;   // "GrindEnd"

        // Records (kUpdateLevelRecord).
        constexpr uint32_t kRecordGrindTime = 0;
        constexpr uint32_t kRecordGrindDistance = 1;
        constexpr uint32_t kRecordManualTime = 3;
        constexpr uint32_t kRecordManualDistance = 4;
        constexpr uint32_t kRecordAirTime = 7;

        constexpr int32_t kRefused = -1;

        // Tuning (aggressive/tuning.h; docs/TUNING.md "Entries").
        constexpr uint32_t kFX = 1, kCamera = 2, kPhysics = 3, kGrindWalls = 14, kVibration = 18;
        constexpr uint32_t kImpactRingSpeed = 11;         // FX
        constexpr uint32_t kNormalBlur = 42;              // Camera
        constexpr uint32_t kAcceleration = 1;             // Physics
        constexpr uint32_t kIdleVelocity = 28;            // Physics
        constexpr uint32_t kAccelSpeedDiff = 29;          // Physics
        constexpr uint32_t kManualBalanceDelay = 22;      // Grind/Walls
        constexpr uint32_t kGrindApproachAngle = 27;      // Grind/Walls
        constexpr uint32_t kManualMinSpeed = 31;          // Grind/Walls
        constexpr uint32_t kImpactBig = 15, kImpactSmall = 16, kImpactTime = 17; // Vibrate
        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        inline ee::Global<uint32_t, sym::g_levelNumber> g_level;
        // A manual starting with no balance yet tips by the speed's parity (1e-5).
        inline ee::Global<ee::Float, sym::g_balanceEpsilon> g_balanceEpsilon;
        // Leaving a rail lifts the skater this much (0.01 up, y grows downward).
        inline ee::Global<ee::Float, sym::g_railLift> g_railLift;

        // What "ManualEnd" and "GrindEnd" carry: the surface's kind (a grind:
        // the rail's event word) and the distance.
        struct EndEventData
        {
            uint32_t kind;
            ee::Float distance;
        };

        // Moves 6 (both states): a Double Grind when two rails are under
        // the skater and it heads along them within Grind Approach Angle
        // (its motion, or where it faces when hardly moving).
        int32_t tryDoubleGrind(ee::Ptr<Player> player)
        {
            if (!findDoubleRail(player))
                return kRefused;
            const zge::Vec4 heading = ee::abs(player->speed) < zge::kMathEpsilon
                                          ? player->axes.forward
                                          : zge::unit(player->velocity);
            const zge::Angle approach =
                static_cast<zge::Angle>(ee::toInt(tuning(kGrindWalls, kGrindApproachAngle) * zge::kDegreesToAngle));
            const ee::Float cosApproach = zge::angleCos(approach);
            if (!(cosApproach < ee::abs(zge::dot(heading, player->doubleGrindRails->direction))))
                return kRefused;
            setSkaterState(player, static_cast<uint32_t>(State::DoubleGrind));
            return player->state == State::DoubleGrind ? static_cast<int32_t>(State::DoubleGrind) : kRefused;
        }

        // Enters `state` and returns it if the skater is in it afterwards.
        int32_t enter(ee::Ptr<Player> player, State state)
        {
            setSkaterState(player, static_cast<uint32_t>(state));
            return player->state == state ? static_cast<int32_t>(state) : kRefused;
        }

    }

    // setSkaterState: the skater changes state: the time in the state starts
    // again (for a real change), the old state's exit runs, then the new
    // one's enter; the scoring hears of it unless the skater is down or the
    // state is a Vault (whose enter tells it later, pole_vault.cpp).
    void setSkaterState(ee::Ptr<Player> player, uint32_t state)
    {
        if (state != static_cast<uint8_t>(player->state))
            player->stateTime = 0.0f;
        if (const auto exit = (*g_states)[static_cast<uint8_t>(player->state)].exit)
            exit(player);
        player->previousState = static_cast<uint8_t>(player->state);
        player->state = static_cast<State>(state);
        if (const auto enter = (*g_states)[state & 0xFFu].enter)
            enter(player);
        if (player->flags & kDown)
            return;
        if (state == static_cast<uint32_t>(State::Vault))
            return;
        game::tail(scoreStateChange, player, player->previousState, static_cast<uint8_t>(player->state));
    }
    READABLE_FUNCTION_WAITS(setSkaterState, Verified);

    // requestMove: a move asked of the skater (a trick's kind, the action
    // button): the state's handler (StateRow::request) takes it and returns the
    // state it entered; -1 when the state has none or the skater is down.
    int32_t requestMove(ee::Ptr<Player> player, int32_t move)
    {
        const auto handler = (*g_states)[static_cast<uint8_t>(player->state)].request;
        if (!handler || (player->flags & kDown))
            return kRefused;
        return handler(player, move);
    }
    READABLE_FUNCTION_WAITS(requestMove, Verified);

    // groundRequest, the Ground state's move handler: grind tricks, the
    // Double Grind, the action button (a vertical pole, a vault, a skitch),
    // a manual above Manual Min Speed, a cess slide.
    int32_t groundRequest(ee::Ptr<Player> player, int32_t move)
    {
        ee::Local<int32_t> result;
        *result = kRefused;
        switch (move)
        {
        case kMoveGrind:
        case kMoveHandGrind:
            grindTrickMove(player, move, result);
            return *result;
        case kMoveDoubleGrind:
            return tryDoubleGrind(player);
        case kMoveAction:
        {
            const int32_t target = actionRequest(player);
            switch (target)
            {
            case static_cast<int32_t>(State::Vault):
                return enter(player, State::Vault);
            case static_cast<int32_t>(State::VerticalPole):
                return enter(player, State::VerticalPole);
            case static_cast<int32_t>(State::Skitch):
                return enter(player, State::Skitch);
            default:
                return kRefused;
            }
        }
        case kMoveManual:
            if (!(tuning(kGrindWalls, kManualMinSpeed) < player->speed))
                return kRefused;
            return enter(player, State::Manual);
        case kMoveCessSlide:
            if (player->state == State::CessSlide)
                return static_cast<int32_t>(State::CessSlide);
            return enter(player, State::CessSlide);
        default:
            return kRefused;
        }
    }
    READABLE_FUNCTION_WAITS(groundRequest, Verified);

    // airRequest, the Air state's move handler: grind tricks, the Double
    // Grind, and the action button (a vertical pole grabbed in the air, a
    // horizontal pole, or Air again for a bar out of reach).
    int32_t airRequest(ee::Ptr<Player> player, int32_t move)
    {
        ee::Local<int32_t> result;
        *result = kRefused;
        switch (move)
        {
        case kMoveGrind:
        case kMoveHandGrind:
            grindTrickMove(player, move, result);
            return *result;
        case kMoveDoubleGrind:
            return tryDoubleGrind(player);
        case kMoveAction:
        {
            const int32_t target = actionRequest(player);
            switch (target)
            {
            case static_cast<int32_t>(State::VerticalPole):
                // Grabbed from the air: the Vertical Pole A.
                return enter(player, State::VerticalPoleA);
            case static_cast<int32_t>(State::HorizontalPole):
                return enter(player, State::HorizontalPole);
            case static_cast<int32_t>(State::Air):
                return enter(player, State::Air);
            default:
                return kRefused;
            }
        }
        default:
            return kRefused;
        }
    }
    READABLE_FUNCTION_WAITS(airRequest, Verified);

    // groundExit, the Ground state's exit: off a moving surface; the
    // object rolled on hears "OnEnd" (with its surface's kind), and after
    // a manual "ManualEnd" (kind and distance; the skater's script too);
    // it becomes the last object.
    void groundExit(ee::Ptr<Player> player)
    {
        ee::Local<EndEventData> manualEnd;
        ee::Local<uint32_t> kind;
        if (player->onMovingSurface)
        {
            player->onMovingSurface = 0;
            player->movingObject = 0;
        }
        if (player->surface == kNoSurface || !player->object)
            return;
        *kind = surfaceKind(player->objectNode, player->surface);
        sendObjectEvent(player, player->objectNode, kOnEnd, kind);
        if (player->state == State::Manual)
        {
            manualEnd->distance = player->distance;
            manualEnd->kind = surfaceKind(player->objectNode, player->surface);
            sendObjectEvent(player, player->objectNode, kManualEnd, manualEnd);
            if (player->script)
                sendScriptEvent(player, player->script, kManualEnd, manualEnd);
        }
        player->lastObject = player->object;
    }
    READABLE_FUNCTION_WAITS(groundExit, Verified);

    // standEnter, the Stand state's enter: standing still (no turn, no
    // velocity, no speed), off an edge it hangs over, the ground under it
    // looked for. Returns 1.
    int32_t standEnter(ee::Ptr<Player> player)
    {
        player->pipeHeight = 0.0f;
        player->pipePeakHeight = 0.0f;
        player->rates = zge::kZero;
        player->velocity = zge::kZero;
        player->speed = 0.0f;
        slideOffEdge(player, player->speed);
        probeGround(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(standEnter, Verified);

    // standUpdate, the Stand state's second callback: a push (Physics /
    // Acceleration along forward) while slower than the push speed, marked
    // hard while more than Accel Speed Diff below it; the Ground physics on
    // a moving surface; rolling again (Ground) above Idle Velocity or once
    // pushing.
    void standUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<zge::Vec4> push;
        player->previousSpeed = player->speed;
        player->speed = zge::vec3Length(&player->velocity.xyz());
        if (player->speed < player->pushSpeed)
        {
            *push = player->axes.forward * (frameTime * tuning(kPhysics, kAcceleration));
            player->velocity = player->velocity + *push;
            player->previousSpeed = player->speed;
            player->speed = zge::vec3Length(&player->velocity.xyz());
            player->pushing = tuning(kPhysics, kAccelSpeedDiff) < player->pushSpeed - player->speed || player->pushing;
        }
        else
            player->pushing = 0;
        if (player->onMovingSurface)
            groundPhysics(player, frameTime);
        if (tuning(kPhysics, kIdleVelocity) < player->speed || player->pushSpeed != 0.0f)
            setSkaterState(player, static_cast<uint32_t>(State::Ground));
        probeGround(player);
    }
    READABLE_FUNCTION_WAITS(standUpdate, Verified);

    // airExit, the Air state's exit: what the skater flew over hears
    // "OverEnd"; a landing at Impact Ring Speed or more makes the ring
    // effect and the impact sound and rumbles (Vibrate / Impact Time,
    // Small, Big); the air time goes to the records.
    void airExit(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> data;
        if (player->overObject)
        {
            *data = player->overSurfaceKind;
            sendObjectEvent(player, player->overNode, kOverEnd, data);
        }
        player->overSurfaceKind = 0;
        player->overObject = 0;
        player->stateValue = 0.0f;
        if (tuning(kFX, kImpactRingSpeed) <= player->previousSpeed)
        {
            if (!player->splashed)
            {
                aggressive::fx::impactRing(&player->position, &player->drawnOrientation, &player->velocity);
                aggressive::soundEvent(kImpactSound, &player->position, 0, 0, 0);
            }
            const int32_t time = sce::libgcc::fptoui(tuning(kVibration, kImpactTime));
            aggressive::vibrate(&player->controls, time, tuning(kVibration, kImpactSmall), tuning(kVibration, kImpactBig));
        }
        aggressive::updateLevelRecord(kRecordAirTime, *g_level, player->name, player->stateDistance);
        player->stateDistance = 0.0f;
    }
    READABLE_FUNCTION_WAITS(airExit, Verified);

    // airQuarterPipeExit, the Air QPipe state's exit: with no spin turn the
    // skater faces along its motion again (rates stopped, forward the
    // velocity's direction, the axes made square); "OverEnd" to what it
    // flew over; the air time to the records.
    void airQuarterPipeExit(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> data;
        if (player->noSpinTurn)
        {
            player->rates = zge::kZero;
            player->axes.forward = zge::unit(player->velocity);
            orthonormalize(&player->axes.right, &player->axes.up, &player->axes.forward);
        }
        if (player->overObject)
        {
            *data = player->overSurfaceKind;
            sendObjectEvent(player, player->overNode, kOverEnd, data);
        }
        player->overSurfaceKind = 0;
        player->overObject = 0;
        player->qpipeTurn = 0.0f;
        aggressive::updateLevelRecord(kRecordAirTime, *g_level, player->name, player->stateDistance);
        player->stateDistance = 0.0f;
    }
    READABLE_FUNCTION_WAITS(airQuarterPipeExit, Verified);

    // grindExit, the Grind state's exit: the grind's HUD off; the rail
    // becomes the last rail; the rail's object and the skater's script hear
    // "GrindEnd" (with the distance); the rail let go (the skater lifted
    // off it a little), off a moving rail; the grind's time and distance
    // to the records; the camera's motion blur back to Normal Blur.
    void grindExit(ee::Ptr<Player> player)
    {
        ee::Local<EndEventData> grindEnd;
        aggressive::hud::hideGrindMeter(player);
        player->lastRail = player->grindRail;
        if (const ee::Ptr<zge::Node> object = player->objectNode)
        {
            grindEnd->kind = player->railEvent;
            grindEnd->distance = player->distance;
            sendObjectEvent(player, object, kGrindEnd, grindEnd);
            if (player->script)
                sendScriptEvent(player, player->script, kGrindEnd, grindEnd);
        }
        player->surface = kNoSurface;
        player->distance = 0.0f;
        player->object = 0;
        player->railEvent = 0;
        player->position.y = player->position.y - *g_railLift;
        player->airTime = 0.0f;
        player->railStart = zge::kZero;
        player->railEnd = zge::kZero;
        if (player->onMovingSurface)
        {
            player->onMovingSurface = 0;
            player->movingObject = 0;
        }
        aggressive::updateLevelRecord(kRecordGrindTime, *g_level, player->name, player->shortTime);
        aggressive::updateLevelRecord(kRecordGrindDistance, *g_level, player->name, player->stateDistance);
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
        const uint8_t blur = static_cast<uint8_t>(ee::toInt(tuning(kCamera, kNormalBlur)));
        aggressive::setCameraBlur(&(*g_followCameras)[player->number], blur, ee::Float(1.0f), ee::Float(1.0f));
    }
    READABLE_FUNCTION_WAITS(grindExit, Verified);

    // manualEnter, the Manual state's enter: the Ground's, the balance
    // meter shown; the balance drifts at the skater's manual balance stat,
    // the way it already leans (with no lean yet, by the speed's parity),
    // from half the lean it had; Manual Balance Delay before the stick
    // counts; the manual's time and distance from 0. Returns 1.
    int32_t manualEnter(ee::Ptr<Player> player)
    {
        groundEnter(player);
        aggressive::hud::showBalanceMeter(player);
        const ee::Float drift = skaterStat(player, kStatManual, kStatManualInit);
        const ee::Float lean = player->balance[1];
        bool positive;
        if (ee::abs(lean) < *g_balanceEpsilon)
            positive = (ee::toInt(player->speed) & 1) != 0;
        else
            positive = 0.0f < lean;
        player->balanceSpeed = positive ? drift : -drift;
        player->distance = 0.0f;
        player->balance[1] = lean / 2.0f;
        player->stateValue = tuning(kGrindWalls, kManualBalanceDelay);
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
        return 1;
    }
    READABLE_FUNCTION_WAITS(manualEnter, Verified);

    // manualExit, the Manual state's exit: the balance meter hidden, the
    // manual's time and distance to the records, then the Ground's exit.
    void manualExit(ee::Ptr<Player> player)
    {
        aggressive::hud::hideBalanceMeter(player);
        aggressive::updateLevelRecord(kRecordManualTime, *g_level, player->name, player->shortTime);
        aggressive::updateLevelRecord(kRecordManualDistance, *g_level, player->name, player->stateDistance);
        player->shortTime = 0.0f;
        player->stateDistance = 0.0f;
        game::tail(aggressive::skater::groundExit, player);
    }
    READABLE_FUNCTION_WAITS(manualExit, Verified);
}
