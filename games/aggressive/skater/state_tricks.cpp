// The tricks a state starts (were the sweep's startGroundTricks, createJumpAirTrick
// and startWallRideTrick .. startVaultTrick): scoreStateChange (on every
// state change) picks one by the state entered: on the ground the landing
// (the line banked, a manual or a cess slide matched), in the air the
// jump's own trick, on a wall, a pole, a vault or a skitch that state's
// trick object (object kinds 6, 8, 7 and 11: objects.h), each started when
// the skater is free to act and the object agrees ("areYouPossible").
// scoreStateChange itself also closes what the state left needs closed.
//

#include "skater.h"
#include "trick_objects.h"

#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "zge/scene/animator.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/route.h"
#include "zge/text/cstring.h"
#include "action_tricks.h"
#include "moves.h"
#include "aggressive/pads.h"
#include "sym/aggressive/skater/state_tricks.cpp.h"

#include <initializer_list>

namespace aggressive
{
}

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSpinButton = 0x100, kOtherSpinButton = 0x200; // padHeld's: the shoulder buttons
        constexpr uint32_t kAnimationBlendTime = 2;          // Control tuning
        constexpr uint32_t kSkateAnimation = sym::g_textGlobalAssetsCedwardsSkate;    // "global_assets.cedwards.skate"

        // The states' trick objects' creators (() -> the object).
        constexpr uint32_t kNewWallRideTrick = sym::newWallTrick; // kind 6
        constexpr uint32_t kNewSkitchTrick = sym::newSkitch;      // kind 11
        constexpr uint32_t kNewPoleTrick = sym::newPoleTrick;     // kind 8
        constexpr uint32_t kNewVaultTrick = sym::newVaultTrick;   // kind 7

        // The manual's match takes the whole history (99999.9 s).
        inline ee::Global<ee::Float, sym::g_manualMatchTime> g_manualMatchTime;

        constexpr uint32_t kGroundSlot = 3; // Player::objectSlots: the ground object's
        constexpr uint32_t kCrashBail = 2;  // skaterCrash's kind when the trick in hand may not be cut short
        constexpr uint32_t kFlagBit0 = 0x1; // Player::flags (reset.cpp)

        // The four states' tricks: a trick object of the state's kind, started
        // when the skater is free to act and it may start.
        int32_t startStateTrick(ee::Ptr<Player> player, ee::FunctionPtr<ee::Ptr<GameObject>()> newTrick)
        {
            ee::Local<uint32_t> mayStart;
            int32_t result = static_cast<int32_t>(freeToAct(player));
            if (!result)
                return result;
            const ee::Ptr<GameObject> trick = newTrick();
            const uint32_t canStart = *g_msgAreYouPossible;
            *mayStart = 0;
            result = sendObjectMessage(ee::Ptr<GameObject>(trick), canStart, player, mayStart);
            if (*mayStart)
                result = startTrick(player, trick);
            return result;
        }
    }

    // startWallRideTrick: Wall Ride and Wall Tap: a wall ride trick.
    int32_t startWallRideTrick(ee::Ptr<Player> player)
    {
        return startStateTrick(player, kNewWallRideTrick);
    }
    READABLE_FUNCTION_WAITS(startWallRideTrick, Verified);

    // startSkitchTrick: Skitch: a skitch trick.
    int32_t startSkitchTrick(ee::Ptr<Player> player)
    {
        return startStateTrick(player, kNewSkitchTrick);
    }
    READABLE_FUNCTION_WAITS(startSkitchTrick, Verified);

    // startPoleTrick: the three pole states: a pole trick.
    int32_t startPoleTrick(ee::Ptr<Player> player)
    {
        return startStateTrick(player, kNewPoleTrick);
    }
    READABLE_FUNCTION_WAITS(startPoleTrick, Verified);

    // startVaultTrick: Vault: a vault trick.
    int32_t startVaultTrick(ee::Ptr<Player> player)
    {
        return startStateTrick(player, kNewVaultTrick);
    }
    READABLE_FUNCTION_WAITS(startVaultTrick, Verified);

    namespace
    {
        namespace tail
        {
        int32_t selectStateObjectKeepingResult(ee::Ptr<Player> player) { return ee::tailCall<int32_t>(sym::selectStateObject, player); }
        }
    }

    // createJumpAirTrick: the air states, from `from`: from another air state
    // nothing; else the object slots emptied, and a jump from the ground, a
    // stand or a manual is a trick of its own (the unnamed flip) in the
    // action slot; then the trick input's air state.
    int32_t createJumpAirTrick(ee::Ptr<Player> player, uint32_t from)
    {
        const bool fromAir = static_cast<int32_t>(from) < static_cast<int32_t>(State::AirSpine) + 1 &&
                             !(static_cast<int32_t>(from) < static_cast<int32_t>(State::Air));
        if (fromAir)
            return 0;
        for (uint32_t slot = 0; slot < 4; ++slot)
            emptySlot(player->objectSlots[slot]);
        if (from < static_cast<uint32_t>(State::Air) || from == static_cast<uint32_t>(State::Manual))
        {
            const ee::Ptr<GameObject> jump = newUnnamedFlip();
            putObjectInSlot(player->objectSlots[0], jump);
        }
        return tail::selectStateObjectKeepingResult(player);
    }
    READABLE_FUNCTION_WAITS(createJumpAirTrick, Verified);

    // startGroundTricks: the ground states, from `from` (an air state, a grind,
    // ...; not from the ground or a stand): the object slots emptied; the
    // trick in hand cut short, or a bail when it may not be; a cess slide
    // started if its button is down; from the air, the ground or a cess
    // slide a manual matched from the stick's history; the line banked and
    // its counters reset; the ground's object (landed from the air, else
    // rolling); the trick input's air state, and with the trick button held
    // the ground object's state (2, 1).
    int32_t startGroundTricks(ee::Ptr<Player> player, uint32_t from)
    {
        ee::Local<uint32_t> mayInterrupt;
        int32_t result = static_cast<int32_t>(from < static_cast<uint32_t>(State::Air));
        if (result)
            return result;
        uint32_t bail = 0;
        for (uint32_t slot = 0; slot < 4; ++slot)
            emptySlot(player->objectSlots[slot]);
        if (player->hasCurrentObject)
        {
            *mayInterrupt = 0;
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            sendObjectMessage(ee::Ptr<GameObject>(current), *g_msgAreYouFinished, player, mayInterrupt);
            bail = *mayInterrupt == 0 ? 1u : 0u;
            if (*mayInterrupt)
            {
                sendObjectMessage(ee::Ptr<GameObject>(current), *g_msgTrickFinished, player, 0);
                player->hasCurrentObject = 0;
            }
        }
        if (bail)
        {
            result = skaterCrash(player, kCrashBail);
            return result;
        }
        player->flags = player->flags & ~kFlagBit0;
        const bool cessSlideHeld = ee::Float(0.0f) < player->controls.cessSlideTime;
        if (cessSlideHeld)
        {
            const ee::Ptr<GameObject> slide = newCessSlide();
            result = startTrick(player, slide);
            if (result)
                return result;
            releaseGameObject(slide);
        }
        // A manual straight from the air, the ground or a cess slide.
        bool manual = from == static_cast<uint32_t>(State::Air);
        if (!manual)
        {
            if (static_cast<int32_t>(from) < static_cast<int32_t>(State::AirQuarterPipe))
                manual = from == static_cast<uint32_t>(State::Ground);
            else
                manual = from == static_cast<uint32_t>(State::CessSlide);
        }
        if (manual)
        {
            const ee::Ptr<GameObject> matched = matchManualTrick(player, *g_manualMatchTime);
            if (matched)
            {
                result = startTrick(player, matched);
                if (result)
                    return result;
                releaseGameObject(matched);
            }
        }
        endTrickChain(player);
        resetLineCounters(player);
        ee::Ptr<GameObject> ground;
        if (from - static_cast<uint32_t>(State::Air) < 3u)
            ground = newGroundObjectForAir();
        else
            ground = newGroundObject();
        putObjectInSlot(player->objectSlots[kGroundSlot], ground);
        selectStateObject(player);
        result = aggressive::layoutButtonHeld(&player->controls);
        if (result)
            result = setObjectState(player, 0, 2, 1);
        return result;
    }
    READABLE_FUNCTION_WAITS(startGroundTricks, Verified);
    namespace
    {
        constexpr uint32_t kCrashOffManual = 3;                // skaterCrash's kind: off a manual on the wrong foot
        constexpr uint32_t kFakieStance = 4;                    // a grind trick's stance that lands fakie (TrickState::stance)
        // The pole tricks' own message (pole_trick.cpp): nameHash("ForcePoleRelease").
        inline ee::Global<uint32_t, sym::g_msgForcePoleRelease> g_msgForcePoleRelease;
    }

    // scoreStateChange (player, from, to): the scoring's side of a state change
    // (setSkaterState's last call). What the state left needs: a manual's
    // stick history cleared (and onto the ground off a manual whose foot the
    // balance does not match, a crash); a cess slide's press forgotten; a
    // wall's state object made current; a pole let go (ForcePoleRelease); a
    // grind's trick in the fakie stance lands fakie. Then the state entered
    // starts its trick (scoreStateChange's table enteringTable): on the ground
    // the landing's (startGroundTricks), in the air the jump's
    // (createJumpAirTrick), on a pole, a wall, a vault or a skitch that
    // state's trick object.
    void scoreStateChange(ee::Ptr<Player> player, uint32_t from, uint32_t to)
    {
        ee::Local<uint8_t[0x60]> poleReleased;
        if (from - static_cast<uint32_t>(State::Grind) < 11u)
        {
            switch (static_cast<State>(from))
            {
            case State::Manual:
                player->controls.historyCount = 0;
                zge::memoryFill(player->controls.history, 0, sizeof(Controls::history));
                if (to < 2u) // onto the ground
                {
                    const ee::Ptr<GameObject> manual = skaterCurrentObject(player);
                    if (!manualMatchesBalance(manual, player->balance[1]))
                    {
                        skaterCrash(player, kCrashOffManual);
                        return;
                    }
                }
                break;
            case State::CessSlide:
                player->controls.cessSlideTime = -1.0f;
                break;
            case State::WallRide:
            case State::WallTap:
                selectStateObject(player);
                break;
            case State::VerticalPole:
            case State::VerticalPoleA:
            case State::HorizontalPole:
            {
                const ee::Ptr<GameObject> pole = skaterCurrentObject(player);
                sendObjectMessage(ee::Ptr<GameObject>(pole), *g_msgForcePoleRelease, player, poleReleased);
                break;
            }
            case State::Grind:
            case State::HandGrind:
            case State::DoubleGrind:
            {
                const ee::Ptr<GameObject> current = skaterCurrentObject(player);
                if (current)
                {
                    if (gameObjectKind(ee::Ptr<GameObject>(current)) == kObjectTrick)
                    {
                        const ee::Ptr<TrickState> trick(gameObjectState(ee::Ptr<GameObject>(current)));
                        if (trick->stance == kFakieStance)
                        {
                            zge::stopAnimation(player->node);
                            player->drawnRefresh = 1;
                            player->fakie = 1;
                        }
                        else
                        {
                            player->fakie = 0;
                            player->drawnRefresh = 1;
                        }
                    }
                }
                break;
            }
            default: // Stall
                break;
            }
        }
        if (to >= kStateCount)
            return;
        switch (static_cast<State>(to))
        {
        case State::Ground:
        case State::Stand:
            startGroundTricks(player, from);
            break;
        case State::Skitch:
            startSkitchTrick(player);
            break;
        case State::WallRide:
        case State::WallTap:
            startWallRideTrick(player);
            break;
        case State::VerticalPole:
        case State::VerticalPoleA:
        case State::HorizontalPole:
            startPoleTrick(player);
            break;
        case State::Air:
        case State::AirQuarterPipe:
        case State::AirSpine:
            createJumpAirTrick(player, from);
            break;
        case State::Vault:
            startVaultTrick(player);
            break;
        default: // the grinds, the stall, the manual, the cess slide: nothing to start
            break;
        }
    }
    READABLE_FUNCTION_WAITS(scoreStateChange, Verified);

    // queueDefaultStateObject: what the skater does with no object queued for its state,
    // unless it is down: on the ground or a cess slide the ground object goes
    // to its slot and becomes current; in the air it plays the spin animation
    // for a shoulder button held (mirrored for the second), else the fall,
    // fakie or not; on a pole, a wall, in a manual, a vault or a skitch (past
    // the original's table) it plays the skating animation. The grinds and
    // the stall have nothing to do. The result is the animation's, or whether
    // the object was put.
    uint32_t queueDefaultStateObject(ee::Ptr<Player> player)
    {
        ee::Local<ee::Ptr<zge::Motion>> fakieAnimation, plainAnimation;
        constexpr uint8_t kTableStates = 16;
        constexpr uint32_t kPlayAnimationMode = 5, kMirroredMode = 13;
        constexpr ee::Float kPlayOnce = -1.0f;
        const auto blendTime = [] { return tuningValue(kTuningControl, kAnimationBlendTime); };

        // The air's animation: the spin's for a shoulder button held (the
        // second one mirrors it), else the fall.
        const auto playInAir = [&](const ee::Local<ee::Ptr<zge::Motion>> &animation, auto fallHash) {
            uint32_t mode = kPlayAnimationMode;
            *animation = {};
            const auto findSpin = [&]() {
                const uint32_t hash = airSpinAnimationHash();
                zge::findAnimationByHash(hash, animation);
            };

            if (padHeld(kSpinButton, player->controls.port) != 0)
                findSpin();
            else
            {
                const uint32_t port = player->controls.port;
                const int32_t otherHeld = padHeld(kOtherSpinButton, port);
                if (otherHeld != 0)
                {
                    findSpin();
                    if (*animation)
                        mode = kMirroredMode;
                }
            }
            if (!*animation)
            {
                const uint32_t hash = fallHash();
                zge::findAnimationByHash(hash, animation);
            }
            const uint32_t result = zge::playAnimation(player->node, *animation, mode, blendTime(), kPlayOnce);
            return result;
        };

        const auto playSkating = [&] {
            const uint32_t played = zge::playAnimationNamed(player->node, kSkateAnimation, kPlayAnimationMode, blendTime(), kPlayOnce);
            return played;
        };

        uint32_t result = 0;
        if ((player->flags & kDown) == 0)
        {
            const uint8_t state = static_cast<uint8_t>(player->state);
            if (state >= kTableStates)
                return playSkating();
            switch (static_cast<State>(state))
            {
            case State::Ground:
            case State::Stand:
            case State::CessSlide:
            {
                const ee::Ptr<GameObject> object = newGroundObject();
                if (object != 0)
                {
                    putObjectInSlot(player->objectSlots[3], object);
                    selectStateObject(player);
                    result = 1;
                }
                break;
            }
            case State::Air:
            case State::AirQuarterPipe:
            case State::AirSpine:
                if (player->fakie != 0)
                    return playInAir(fakieAnimation, [] { return airFakieAnimationHash(); });
                return playInAir(plainAnimation, [] { return airAnimationHash(); });
            case State::VerticalPole:
            case State::VerticalPoleA:
            case State::HorizontalPole:
            case State::WallRide:
            case State::WallTap:
            case State::Manual:
                return playSkating();
            default: // the grinds, the stall
                break;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(queueDefaultStateObject, Verified);

    namespace
    {
        // Player::objectSlots, named for the states that take from them
        // first (selectStateObject's order); kGroundSlot is the fourth.
        constexpr uint32_t kActionSlot = 0, kManualSlot = 1, kGrindSlot = 2;

        // A note for the programmers that a state has no row in the table
        // ("... handle physics state %d to fix it."): formatted, never shown.
        constexpr uint32_t kUnhandledStateNote = sym::g_textLetPeepKnowWhatYouDidAndTell;
        constexpr uint32_t kNoteBytes = 0x40;

    }

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Address bytes, uint32_t value, uint32_t count) { ee::game(sym::memset, bytes, value, count); }
    }

    // selectStateObject: when the skater is free to act, the trick object
    // its state starts next: the first slot holding one, in the state's
    // order, gives up its front object, which is started; one that will
    // not start is dropped and the next is tried. On the ground, standing
    // or in a cess slide the order is the ground's slot, the manual's, the
    // grind's; in the air or a vault the action's, the manual's, the
    // grind's; in a manual the manual's, the grind's; in a grind or a stall
    // the grind's alone. With no object found the current one hears
    // "TrickFinished" and is let go, and queueDefaultStateObject does the
    // state's default. On a pole or a wall nothing changes. Skitch has no
    // row in the original's table: it formats a note for the programmers
    // that nothing shows, then does the default.
    void selectStateObject(ee::Ptr<Player> player)
    {
        ee::Local<char[kNoteBytes]> note;
        if (!freeToAct(player))
            return;

        // The front object of the first slot in `order` that holds one.
        const auto takeFirst = [&](std::initializer_list<uint32_t> order) -> ee::Ptr<GameObject> {
            for (const uint32_t slot : order)
            {
                if (!slotNodeEmpty(player->objectSlots[slot]))
                    return takeFromSlot(player->objectSlots[slot]);
            }
            return {};
        };

        ee::Ptr<GameObject> trick;
        switch (player->state)
        {
        case State::Ground:
        case State::Stand:
        case State::CessSlide:
            trick = takeFirst({kGroundSlot, kManualSlot, kGrindSlot});
            break;
        case State::Air:
        case State::AirQuarterPipe:
        case State::AirSpine:
        case State::Vault:
            trick = takeFirst({kActionSlot, kManualSlot, kGrindSlot});
            break;
        case State::Manual:
            trick = takeFirst({kManualSlot, kGrindSlot});
            break;
        case State::Grind:
        case State::HandGrind:
        case State::DoubleGrind:
        case State::Stall:
            trick = takeFirst({kGrindSlot});
            break;
        case State::VerticalPole:
        case State::VerticalPoleA:
        case State::HorizontalPole:
        case State::WallRide:
        case State::WallTap:
            return;
        default: // Skitch, past the table
            memset(note, 0, kNoteBytes);
            zge::formatString(note, kUnhandledStateNote, static_cast<uint32_t>(player->state));
            break;
        }

        if (trick)
        {
            if (!startTrick(player, trick))
                aggressive::skater::selectStateObject(player);
            return;
        }
        if (player->hasCurrentObject)
        {
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            sendObjectMessage(ee::Ptr<GameObject>(current), *g_msgTrickFinished, player, 0);
            player->hasCurrentObject = 0;
        }
        aggressive::skater::queueDefaultStateObject(player);
    }
    READABLE_FUNCTION_WAITS(selectStateObject, Verified);
}
