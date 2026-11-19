#pragma once

// NPCs and the level's data points (docs/BRINGUP.md "Zoo ambient NPCs";
// game_overrides/aggressive_inline_zoo.cpp).

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include "sym/aggressive/npc.h.h"

namespace zge
{
    struct Follower; // (zge/scene/follower.h)
    struct Node;
    struct SceneObject;
    struct Motion;
    struct SweepBox;
    struct Vec4;
}

namespace aggressive::skater { struct Player; }

namespace aggressive
{
    // A row of the NPC type table: the type (name hash of the model's name,
    // e.g. "npc_willow") and five animations, each a name and what the
    // animation setup (setupNpcTypeAnimations) found for it at the level's load.
    struct NpcAnimationSlot
    {
        ee::Ptr<const char> name; // "zoo_assets.npc.willow_tree"
        ee::Ptr<zge::Motion> animation; // 0 until found (findAnimation)
    };
    struct NpcType
    {
        uint32_t typeHash;              // +0x00 zge::nameHash of the type's name
        NpcAnimationSlot animations[5]; // +0x04
    };
    static_assert(sizeof(NpcType) == 0x2C);
    // NpcType::animations by row (every row's name ends in these words).
    enum NpcAnimation : uint32_t
    {
        kNpcAnimationWalk = 0,
        kNpcAnimationIdle = 1,
        kNpcAnimationAngry = 2,
        kNpcAnimationCrash = 3,
        kNpcAnimationAttack = 4,
    };

    // The PS2's 62 rows (the Xbox release has 68: six Zoo types more),
    // searched by findNpcType.
    inline ee::Global<NpcType[62], sym::g_npcTypes> g_npcTypes;
    constexpr uint32_t kNpcTypeCount = 62;

    // A level data point (<LEVEL>.DAT: a FORM of "SDPT" records): a 0x2C-byte
    // header, then the class's own fields (an NPC's type hash at +0x2C).
    struct DataPoint
    {
        uint32_t _00;
        uint16_t classHash;  // +0x04 low 16 bits of the class name's hash ("NPC" 0x5343, "spawn" 0x46BE; 0: no object)
        uint16_t _06;
        uint32_t nameHash;   // +0x08 the point's name ("playerone", "respawn", ...)
        uint32_t id;         // +0x0C
        zge::Vec3 position;  // +0x10
        uint8_t _1c[0x10];
    };
    static_assert(sizeof(DataPoint) == 0x2C && offsetof(DataPoint, position) == 0x10);

    // The pointer over an NPC: it bobs up and down and spins. No shipped level
    // script makes one (createNPCPointer).
    struct NpcPointer
    {
        uint32_t owner;          // +0x00 who asked for it (createNpcPointerNode's object)
        ee::Ptr<zge::Node> node; // +0x04
        uint32_t _08[2];
        zge::Matrix matrix;      // +0x10 its rotation and place
        int32_t bob;             // +0x50 -8 .. 8
        uint32_t rising;         // +0x54
        uint32_t spin;           // +0x58 degrees, 0 .. 360
    };
    static_assert(offsetof(NpcPointer, matrix) == 0x10 && offsetof(NpcPointer, spin) == 0x58);
    constexpr uint32_t kNpcPointerBytes = 0x60;

    // The challenge bubble over an NPC (createNPCBubble).
    struct NpcBubble
    {
        uint32_t npcName;        // +0x00
        uint32_t alwaysOne;      // +0x04 1 here, read by nothing
        uint32_t alwaysTwo;      // +0x08 2 here, read by nothing
        uint32_t icon;           // +0x0C a row of bubbleIcons
        uint32_t scriptArgument5; // +0x10 kept, read by nothing
        uint32_t scriptArgument6; // +0x14
        ee::Ptr<zge::Node> node[4]; // +0x18 the bubble, its icon, two never made
        zge::Matrix matrix;      // +0x30 the bubble's
        uint8_t _70[0x80];
        uint32_t turn[3];        // +0xF0 cleared here, read by nothing
        uint32_t ready;          // +0xFC both nodes made
        uint8_t _100[0x10];
    };
    static_assert(offsetof(NpcBubble, node) == 0x18 && offsetof(NpcBubble, matrix) == 0x30);
    static_assert(offsetof(NpcBubble, turn) == 0xF0 && sizeof(NpcBubble) == 0x110);

    enum NpcMode : uint32_t
    {
        kNpcLocked = 0,    // the level's script moves it
        kNpcWalking = 1,
        kNpcKnocked = 2,   // pushed aside by the skater: the crash animation
        kNpcAttacking = 3, // plays its attack on contact; nothing in the PS2 build sets this mode
        kNpcStanding = 4,
        kNpcAngry = 5,     // got up from a knock: the angry animation until it is off screen
    };

    // An NPC's block (kTagNpc in its object's component).
    struct Npc
    {
        uint32_t typeHash;                // +0x00 its NpcType
        ee::Ptr<NpcBubble> bubble;        // +0x04
        ee::Ptr<NpcPointer> pointers[4];  // +0x08
        ee::Ptr<zge::SceneObject> object; // +0x18
        ee::Ptr<zge::SweepBox> box;       // +0x1C
        ee::Ptr<zge::Follower> follower;  // +0x20 its path follower (walkPickup)
        uint32_t locked;                  // +0x24 nonzero: it does not walk
        ee::Ptr<zge::Motion> scriptAnimation; // +0x28 what the script last played on a locked NPC
        uint32_t scriptAnimationMode;     // +0x2C
        uint32_t invisible;               // +0x30 a type that is never shown, with no box and no sound
        ee::Float animationSpeed;         // +0x34 by the model's height; -1: the animations' own
        uint32_t mode;                    // +0x38 NpcMode
        uint32_t nextMode;                // +0x3C
        zge::Vec4 home;                   // +0x40 where a knock took it from
        zge::Quaternion homeRotation;     // +0x50
        uint32_t shownBeforeFlyThru;      // +0x60
        ee::Float walkSpeed;              // +0x64 0: it stands
        ee::Float knockedTime;            // +0x68
        uint32_t _6c;
        zge::Vec4 knockVelocity;          // +0x70
        uint32_t speaks;                  // +0x80 nonzero: it speaks when hit
        uint8_t soundSample;              // +0x84 its looping sound's sample
        uint8_t voiceKind;                // +0x85
        uint16_t sound;                   // +0x86 the looping sound's source
    };
    static_assert(offsetof(Npc, object) == 0x18 && offsetof(Npc, mode) == 0x38);
    static_assert(offsetof(Npc, home) == 0x40 && offsetof(Npc, knockVelocity) == 0x70);
    static_assert(offsetof(Npc, sound) == 0x86);
}

namespace aggressive
{
    // (pickups.cpp)
    int32_t createNpcBubble(uint32_t name, uint32_t /*unused*/, uint32_t /*unused*/, uint32_t icon, uint32_t argument5, uint32_t argument6);
    int32_t createNpcPointerNode(uint32_t name, uint32_t owner);
    int32_t destroyNpcBubble(uint32_t name);
    int32_t destroyNpcPointerNode(uint32_t name, uint32_t owner);
    int32_t hideNpcsForFlyThru();
    uint32_t npcSaveSize();
    int32_t npcVelocity(ee::Ptr<Npc> npc, ee::Ptr<zge::Vec4> velocity);
    ee::Ptr<Npc> pickupOf(ee::Ptr<zge::SceneObject> object);
    uint32_t restoreNpcState(uint32_t buffer);
    uint32_t saveNpcState(uint32_t buffer, ee::Ptr<Npc> npc);
    int32_t showNpcsAfterFlyThru();
}

namespace aggressive
{
    // (pickups.cpp)
    int32_t lockNpc(ee::Ptr<zge::SceneObject> object);
    int32_t unlockNpc(ee::Ptr<zge::SceneObject> object);
    int32_t npcSkaterCollision(ee::Ptr<Npc> npc, ee::Ptr<skater::Player> player);
}
