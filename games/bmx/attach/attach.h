#pragma once

// BMX XXX's attaches (the level object types "Attach" and "HumanAttach"):
// things and people a rider picks up and carries. An attach is a level
// object; once a rider activates it, it is carried by the rider (the
// engine's carried objects) and hangs on the bone of another carried object,
// its holder, swinging there on an animate system. A human attach also
// shows a figure: a model object of its own with a body rig or an animate
// system, animated with the attach's animations. BMX XXX's own code; the
// source file's name is not known.

#include "attach_calls.h"

#include "bmx/pathman/pathman_calls.h"
#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/attach/attach.h.h"

namespace game::bmx
{
    // The tag kinds of an attach's tag record, and where it keeps what.
    constexpr uint32_t kAttachTagBytes = 2;
    constexpr uint32_t kAttachTagWords = 8; // words and floats

    constexpr uint32_t kAttachTagCarried = 0x00;      // word: 1 while a rider carries it (saved)
    constexpr uint32_t kAttachTagPlayer = 0x04;       // word: the player whose rider carries it (saved)
    constexpr uint32_t kAttachTagSwingSystem = 0x0C;  // word: the animate system it swings on, 0: none
    constexpr uint32_t kAttachTagSecondSystem = 0x10; // word: a second animate system; nothing read so far makes one
    constexpr uint32_t kAttachTagFigureRig = 0x14;    // word: the figure's body rig or animate system (kAttachTagFigureHasRig)
    constexpr uint32_t kAttachTagCarryTag = 0x18;     // byte: its tag among what the rider carries (kCarryTag...)
    constexpr uint32_t kAttachTagHolderTag = 0x20;    // byte: the tag of the carried object it hangs on
    constexpr uint32_t kAttachTagHolderBone = 0x24;   // word: the name hash of the holder's bone it hangs on, 0: none
    constexpr uint32_t kAttachTagPlace = 0x28;        // vector: where it sits on that bone
    constexpr uint32_t kAttachTagHasAnimator = 0x34;  // byte: not 0, it gets an animator
    constexpr uint32_t kAttachTagSwings = 0x3C;       // byte: not 0, it gets an animator and swings on its bone
    constexpr uint32_t kAttachTagFigureName = 0x40;   // word: the name hash of the figure's model
    constexpr uint32_t kAttachTagFigureHasRig = 0x44; // byte: not 0, the figure has a body rig; 0, an animate system
    // A human attach's figure holds on with its hands (and, without a body
    // rig, stands with its feet), each on a bone of one of the carrier's
    // carried objects: that object's carry tag, the bone's name hash, and
    // the place on the bone.
    struct AttachGripTags
    {
        uint32_t carryTag; // byte
        uint32_t boneName; // word
        uint32_t place;    // vector
    };
    constexpr AttachGripTags kAttachTagLeftHand{0x45, 0x48, 0x4C};
    constexpr AttachGripTags kAttachTagRightHand{0x6C, 0x70, 0x74};
    constexpr AttachGripTags kAttachTagLeftFoot{0x58, 0x5C, 0x60};
    constexpr AttachGripTags kAttachTagRightFoot{0x80, 0x84, 0x88};

    // The figure's four animations: name hashes, and how often each is picked.
    constexpr uint32_t kAttachTagAnimations = 0x94;        // words at +0, +8, +0x10, +0x18
    constexpr uint32_t kAttachTagAnimationWeights = 0x98;  // floats at +0, +8, +0x10, +0x18
    constexpr uint32_t kAttachAnimationStride = 8;

    // The tags of a rider's carried objects (the engine keeps one word for
    // each). An attach is carried under tags 0xA to 0xC; what tells 0xA
    // from the others is not read.
    constexpr uint32_t kCarryTagLowest = 0xA;
    constexpr uint32_t kCarryTagBoxed = 0xB;  // what hangs on a holder of this tag is kept inside the holder's box
    constexpr uint32_t kCarryTagHuman = 0xC;  // a human attach: it has a figure and speaks
    constexpr uint32_t kCarryTagFigure = 0xD; // a human attach's figure with a body rig

    // The level's messages an attach takes beside pathman_calls.h's
    // kActivateMessage; what sends these two is not read, and they are named
    // for what an attach does with them. Both carry an ActivationEvent.
    constexpr uint32_t kReleaseMessage = 0x101; // its rider lets it go
    constexpr uint32_t kBlinkMessage = 0x104;   // a carried attach starts to blink

    // What a save keeps of an attach (its first two tag words).
    struct AttachState
    {
        uint32_t carried; // +0x00
        uint32_t player;  // +0x04
    };
    static_assert(sizeof(AttachState) == 8);

    // A figure's animate system is given this many frames to settle: the
    // count is one over it at set-up, and the bones do not follow the
    // points until it has run out.
    constexpr uint32_t kFigureSettled = 100;

    // (attach_tags.cpp)
    void attachState(ee::Ptr<zge::SceneObject> attach, ee::Ptr<AttachState> out);
    void setAttachState(ee::Ptr<zge::SceneObject> attach, ee::Ptr<const AttachState> state);
    uint32_t attachCarryTag(ee::Ptr<zge::SceneObject> attach);
    uint32_t attachHolderTag(ee::Ptr<zge::SceneObject> attach);
    ee::Ptr<zge::SceneObject> attachHolder(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::SceneObject> carrier);
    uint32_t attachSwings(ee::Ptr<zge::SceneObject> attach);
    uint32_t attachHasAnimator(ee::Ptr<zge::SceneObject> attach);
    ee::Ptr<zge::SceneBone> attachHolderBone(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::SceneObject> carrier);
    ee::Ptr<zge::SceneObject> carriedObjectWithTag(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::SceneObject> carrier, uint32_t tag);
    ee::Ptr<zge::SceneBone> carriedObjectBone(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::SceneObject> carrier, uint32_t tag, uint32_t boneName);
    ee::Ptr<zge::Vec4> attachPlace(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::Vec4> out);
    void setAttachSecondSystem(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::AnimateSystem> system);
    void setAttachSwingSystem(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::AnimateSystem> system);
    ee::Ptr<zge::AnimateSystem> attachSecondSystem(ee::Ptr<zge::SceneObject> attach);
    ee::Ptr<zge::AnimateSystem> attachSwingSystem(ee::Ptr<zge::SceneObject> attach);
    uint32_t attachFigureName(ee::Ptr<zge::SceneObject> attach);
    ee::Ptr<zge::AnimateSystem> attachFigureSystem(ee::Ptr<zge::SceneObject> attach);
    void setAttachFigureSystem(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::AnimateSystem> system);
    uint32_t attachFigureHasRig(ee::Ptr<zge::SceneObject> attach);
    ee::Ptr<zge::BodyRig> attachFigureRig(ee::Ptr<zge::SceneObject> attach);
    void setAttachFigureRig(ee::Ptr<zge::SceneObject> attach, ee::Ptr<zge::BodyRig> rig);
    uint32_t pickFigureAnimation(ee::Ptr<zge::SceneObject> attach);

    // (attach_figure.cpp)
    void updateFigureAnimation(ee::Ptr<zge::SceneObject> attach, ee::Float step);
    ee::Ptr<zge::SceneObject> attachFigure(uint32_t nameHash);

    // The one carried attach that blinks (kBlinkMessage): the figure is
    // shown and hidden in turn, each time the attach has had more updates
    // than `period`, which shrinks by a tenth each turn, from 5 down to 1.
    struct AttachBlink
    {
        uint32_t on;                       // +0x00
        int32_t frames;                    // +0x04 the blinking attach's updates since the last turn
        ee::Ptr<zge::SceneObject> attach;  // +0x08
        uint32_t shown;                    // +0x0C what the figure was last set to (every bit turned each time)
        ee::Float period;                  // +0x10
    };
    static_assert(sizeof(AttachBlink) == 0x14);
    inline ee::Global<AttachBlink, sym::g_attachBlink> g_attachBlink;
    constexpr ee::Float kBlinkStartPeriod = 5.0f;
    constexpr ee::Float kBlinkPeriodStep = 0.1f;
    constexpr ee::Float kBlinkLeastPeriod = 1.0f;
    // No attach blinks.
    inline void stopAttachBlink()
    {
        g_attachBlink->on = 0;
        g_attachBlink->period = kBlinkStartPeriod;
        g_attachBlink->frames = 0;
        g_attachBlink->attach = {};
    }

    // (attach_rig.cpp)
    void buildFigureRig(ee::Ptr<zge::SceneObject> attach);
    void buildFigureSystem(ee::Ptr<zge::SceneObject> attach);

    // (attach_carried.cpp: what the engine calls for a carried attach)
    void setUpAttach(ee::Ptr<zge::SceneObject> attach, ee::Float step);
    void updateAttachSwing(ee::Ptr<zge::SceneObject> attach, ee::Float step);
    void updateAttachFigure(ee::Ptr<zge::SceneObject> attach, ee::Float step);

    // (attach_object.cpp)
    uint32_t registerAttachTypes();
}
