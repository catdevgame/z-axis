#pragma once

// Functions of BMX XXX the path men call that are other modules' (the
// engine's, the game's, the objectives') and cannot be called by name on this
// line yet: the callee has no readable body in BMX XXX, takes other words in
// ZGE4 than the engine header says, or is declared in no header. Each is
// called where it is, by address.
// Scaffolding (address-call): a plain call once the callee is written.

#include "bmx/game/pickups.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/fast_paths.h"
#include "zge/scene/follower.h"
#include "zge/scene/message.h"
#include "zge/scene/objectives.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/pathman/pathman_calls.h.h"

namespace game::bmx::pathman
{
    // --- The engine (ZGE4) ---

    // ZGE4's playAnimation modes (zge/scene/animator.cpp's): the animation
    // starts again at its end; the object is animated only while it is on
    // screen (without it, off screen too).
    constexpr uint32_t kPlayLooped = 1u << 0;
    constexpr uint32_t kPlayOnScreenOnly = 1u << 1;

    // Whether the object has an animator, and the animator freed (the engine's).
    using zge::freeAnimatorOf;
    using zge::objectHasAnimator;

    // The root bone of the object's model: ZGE4's modelBoneParent answers
    // it whatever bone is asked about, and the path men ask about none (their
    // code leaves its own second argument in the bone's place; nothing reads
    // it).
    inline ee::Ptr<zge::SceneBone> modelRootBone(ee::Ptr<zge::SceneObject> object)
    {
        return zge::modelBoneParent(object, ee::Ptr<zge::SceneBone>());
    }

    // The name of the object's level node, 0 when it has no node.
    inline ee::Ptr<const char> objectLevelNodeName(ee::Ptr<zge::SceneObject> object)
    {
        return ee::game<ee::Ptr<const char>>(sym::objectLevelNodeName, object);
    }

    // --- The game ---

    // The messages BMX XXX's level objects take beside the engine's own.
    // The first four are the level's (the script commands ENTITY_ACTIVATE,
    // ENTITY_ENABLE and ENTITY_DISABLE send three of them); what sends the
    // others is not read yet, and they are named for what a path man does
    // with them.
    constexpr uint32_t kActivateMessage = 0x100;  // a rider activates the object (the data: an ActivationEvent)
    constexpr uint32_t kEnableMessage = 0x102;
    constexpr uint32_t kDisableMessage = 0x103;
    constexpr uint32_t kIsEnabledMessage = 0x10A; // the data points to a word for the answer
    constexpr uint32_t kTouchedMessage = 0x105;   // a rider has run into it
    constexpr uint32_t kPauseMessage = 0x106;     // it stops where it is, its animation paused
    constexpr uint32_t kResumeMessage = 0x107;
    constexpr uint32_t kRiderDidMessage = 0x109;  // the data: 0 a rider did a trick near it, 1 a rider crashed

    // kActivateMessage's data (0x20 bytes, cleared by its senders): the level's
    // ActivationEvent (level/entity_messages.h) and DM2's ActivationData, the
    // same record, with their names.
    struct ActivationEvent
    {
        uint32_t quiet;      // +0x00 0 from the game; 1 when a save's restore or the object's removal sends it again (an
                             //       attach then neither subscribes nor unsubscribes its update, nor speaks)
        uint32_t player;     // +0x04 the player whose rider activates it
        uint8_t _08[0x18];
    };
    static_assert(sizeof(ActivationEvent) == 0x20);

    // Every rider in play visited (rider::forEachRider): the original's tail
    // jump of it, measureRiders'.
    inline void forEachRiderTail(rider::RiderVisit visit, ee::Address context) { ee::tailCall<void>(sym::forEachRider, visit, context); }

    // The game's sound events (sound/events.h): event 0x49 plays the voice
    // sound of that name hash at the position.
    constexpr uint32_t kVoiceSoundEvent = 0x49;
}
