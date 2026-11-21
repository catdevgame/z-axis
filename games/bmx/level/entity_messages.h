#pragma once
// The messages BMX XXX's level objects take from the game and its scripts
// (the script commands ENTITY_ACTIVATE, ENTITY_ENABLE and ENTITY_DISABLE send
// the first three by an object's name), beside the engine's own
// (zge/scene/message.h). BMX XXX's own numbers; the names are the script
// commands' and the objects' sound names ("%s.%s.activate").

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A rider (or a script) activates the object: the data is an ActivationEvent.
    constexpr uint32_t kActivateMessage = 0x100;
    // The object is enabled / disabled.
    constexpr uint32_t kEnableMessage = 0x102;
    constexpr uint32_t kDisableMessage = 0x103;
    // Asks an object whether it is enabled: the data points to a word, 1
    // until the object answers.
    constexpr uint32_t kIsEnabledMessage = 0x10A;

    // The level tells every object to hold its animation, and to let it go
    // on again (pauseObjectAnimations, resumeObjectAnimations: entities.cpp).
    // Named from what the animated objects do with them.
    constexpr uint32_t kPauseAnimationMessage = 0x106;
    constexpr uint32_t kResumeAnimationMessage = 0x107;

    // A rider, or a moving body of the level, touches the object (the rider's
    // collision and the level's dynamics contacts send it): the data is a RiderTouch.
    constexpr uint32_t kRiderTouchMessage = 0x104;
    // Scaffolding (unnamed-field): a second message the breakable objects
    // take as a touch that always breaks them, whatever its speed; its
    // data's velocity is not read. The rider's code sends it (not read yet).
    constexpr uint32_t kMessage105 = 0x105;

    // How a rider touches an object; the level's sounds name them ("bump", "tap", "grind", "crash").
    enum TouchKind : uint32_t
    {
        kTouchNone = 0,
        kTouchBump = 1,
        kTouchBump2 = 2, // a second kind the sounds call "bump"
        kTouch3 = 3,     // no sound name
        kTouchTap = 4,
        kTouchGrind = 5,
        kTouchCrash = 6,
        kTouchKinds = 7,
    };

    // kRiderTouchMessage's data, as far as the level objects read it.
    struct RiderTouch
    {
        uint32_t pushOff;    // +0x00 the sender's 1: the rider is pushed off; an object clears it to let the rider through
        uint8_t _04[0xC];
        zge::Vec4 velocity;  // +0x10 the rider's
        TouchKind kind;      // +0x20
        uint8_t _24[0x0C];
        zge::Vec4 point;     // +0x30 where it touches (a face's centre), from a body's face contact
        zge::Vec4 normal;    // +0x40 the touched face's, when hasNormal is set
        uint32_t hasNormal;  // +0x50
        uint32_t face;       // +0x54 the face touched, of node
        uint32_t node;       // +0x58 the collision node touched (0: none)
    };
    static_assert(offsetof(RiderTouch, velocity) == 0x10 && offsetof(RiderTouch, kind) == 0x20);
    static_assert(offsetof(RiderTouch, point) == 0x30 && offsetof(RiderTouch, normal) == 0x40 && offsetof(RiderTouch, node) == 0x58);
    static_assert(sizeof(RiderTouch) == 0x60);

    // kActivateMessage's data (0x20 bytes, cleared by its senders).
    struct ActivationEvent
    {
        // +0x00 0 from a touch; 1 from the objectives' own activations
        // (objectives/collect_type.cpp) and an attach's restore
        // (pathman::ActivationEvent, the same record): an object activated
        // with it set goes to its activated state without its animation and
        // sound (a pickup is taken, a mover put at its end, glass gone with
        // no debris). The names are DM2's ActivationData's (the same record,
        // dm2/level/breakable_save.h, where an exploder does nothing).
        uint32_t quiet;
        uint32_t player;     // +0x04 the player whose rider activates it
        uint8_t _08[8];
        zge::Vec4 velocity;  // +0x10 the touch's velocity from a collision, (0, 0, 0, 1) from the script command
    };
    static_assert(offsetof(ActivationEvent, player) == 4 && offsetof(ActivationEvent, velocity) == 0x10 && sizeof(ActivationEvent) == 0x20);
}
