#pragma once

// Game objects (newGameObject ... objectInSlot): what the skater acts with (a trick,
// a pole, a vehicle) and other things told about events by messages.
//
// An object is a pair of words from a pool (g_objectPool): its
// class's registry entry (+8 into it) and its state block. Classes are
// registered by kind (a hash table at classRegistry, keyed by the kind as a
// 64-bit word); a class entry is {u64 kind, the class's message table}:
// a hash table of {message name hash, ..., handler at +0xC}. A message
// goes to the handler with a two-word note {object, sender} (its address)
// and, when asked for, the handler's result.
//
// Objects in a slot (the skater's current action object, Player +0x8A8):
// the slot is a list (engine listPush pushes to its front); each entry
// a node from another pool (g_slotNodePool) holding the object at +8.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"

#include <cstdint>
#include "sym/aggressive/objects.h.h"

namespace aggressive::skater
{
    struct Player;
}

namespace aggressive
{
    struct GameObject
    {
        ee::Ptr<uint64_t> classEntry; // +0x00 the kind in its class's registry entry (objects.cpp ClassEntry)
        uint32_t state;      // +0x04 the state block
    };
    static_assert(sizeof(GameObject) == 8);

    // The note a message hands its handler (on the sender's stack): the
    // object, and the word the sender passes with the message (the player,
    // or the address of what the handler answers into: see the messages).
    struct ObjectMessageNote
    {
        ee::Ptr<GameObject> object; // +0x00
        uint32_t argument;          // +0x04
    };

    // The objects' messages: the hashes of their names (nameHash, made at
    // the skater's setup), each with what its argument is.
    inline ee::Global<uint32_t, sym::g_msgGetScore> g_msgGetScore;          // "getScore": -> uint32_t, the trick's points
    inline ee::Global<uint32_t, sym::g_msgGetMultiplierBonus> g_msgGetMultiplierBonus; // "getMultiplierBonus": -> uint32_t
    inline ee::Global<uint32_t, sym::g_msgGetName> g_msgGetName;           // "getName": -> NameRequest
    inline ee::Global<uint32_t, sym::g_msgAreYouPossible> g_msgAreYouPossible;    // "areYouPossible": the player; result: may it start
    inline ee::Global<uint32_t, sym::g_msgAreYouFinished> g_msgAreYouFinished;    // "areYouFinished": result: may it be cut short
    inline ee::Global<uint32_t, sym::g_msgGetSpaceNeededForSaving> g_msgGetSpaceNeededForSaving; // "GetSpaceNeededForSaving": -> uint32_t bytes
    inline ee::Global<uint32_t, sym::g_msgSaveToBuffer> g_msgSaveToBuffer;      // "SaveToBuffer": -> SaveRequest
    inline ee::Global<uint32_t, sym::g_msgLoadFromBuffer> g_msgLoadFromBuffer;    // "LoadFromBuffer": -> SaveRequest
    inline ee::Global<uint32_t, sym::g_msgTrickStarted> g_msgTrickStarted;      // "TrickStarted": the player
    inline ee::Global<uint32_t, sym::g_msgTrickFinished> g_msgTrickFinished;     // "TrickFinished": the player
    inline ee::Global<uint32_t, sym::g_msgTrickUpdate> g_msgTrickUpdate;       // "TrickUpdate": -> TrickUpdate
    inline ee::Global<uint32_t, sym::g_msgButtonReleased> g_msgButtonReleased;    // "ButtonReleased"
    inline ee::Global<uint32_t, sym::g_msgTrickStartHolding> g_msgTrickStartHolding; // "TrickStartHolding"
    inline ee::Global<uint32_t, sym::g_msgTrickStoppedHolding> g_msgTrickStoppedHolding; // "TrickStoppedHolding"
    inline ee::Global<uint32_t, sym::g_msgAnimationEnded> g_msgAnimationEnded;    // "AnimationEnded": -> AnimationEnd
    inline ee::Global<uint32_t, sym::g_msgTrickDroppedFromQueue> g_msgTrickDroppedFromQueue; // "TrickDroppedFromQueue"

    // What getName answers into: the text, at most `size` bytes.
    struct NameRequest
    {
        uint32_t out;  // +0x00
        uint32_t size; // +0x04
    };
    // What SaveToBuffer and LoadFromBuffer use: the buffer at +0x04.
    struct SaveRequest
    {
        uint32_t bytes;  // +0x00 the room left in the buffer (saveGameObject's, past the kind)
        uint32_t buffer; // +0x04 the object's state block, copied
    };
    // TrickUpdate's: the player and the frame's seconds.
    struct TrickUpdate
    {
        ee::Ptr<skater::Player> player; // +0x00
        ee::Float seconds; // +0x04
    };
    // AnimationEnded's (sendAnimationEnded): the player, the
    // animation's time and the animation event that ended it
    // (animEventData's, eventInt).
    struct AnimationEnd
    {
        ee::Ptr<skater::Player> player; // +0x00
        ee::Float time;                 // +0x04
        uint32_t event;                 // +0x08
    };

    // An object kind (the registry's key).
    enum ObjectKind : uint32_t
    {
        kObjectGround = 1,        // the ground's object: rolling, landed (skater/ground_object.cpp)
        kObjectTrick = 2,         // grind tricks (skater/trick_objects.cpp)
        kObjectAirTrick = 3,      // air tricks: grabs, flips and both (skater/air_tricks.cpp)
        kObjectManualTrick = 4,   // manuals (skater/manual_trick.cpp)
        kObjectText = 5,          // a text object: a named bonus in the line (skater/text_object.cpp)
        kObjectWallRideTrick = 6, // Wall Ride's and Wall Tap's trick (newWallTrick makes one)
        kObjectVaultTrick = 7,    // Vault's (newVaultTrick)
        kObjectPoleTrick = 8,     // the pole states' (newPoleTrick)
        kObjectCessSlide = 9,     // a cess slide (skater/cess_slide.cpp)
        kObjectStallTrick = 10,   // hand plants' tricks
        kObjectSkitchTrick = 11,  // Skitch's (newSkitch)
    };

    ee::Ptr<GameObject> newGameObject(uint64_t kind, ee::Address state);
    int32_t releaseGameObject(ee::Ptr<GameObject> object);
    uint32_t gameObjectState(ee::Ptr<GameObject> object);
    uint64_t gameObjectKind(ee::Ptr<GameObject> object);
    int32_t sendObjectMessage(ee::Ptr<GameObject> object, uint32_t message, ee::Address argument, ee::Address result);
    int32_t dispatchObjectMessage(ee::Ptr<zge::SearchTree> messages, uint32_t message, ee::Ptr<ObjectMessageNote> note, ee::Address result);
    uint32_t newSlotNode(ee::Ptr<GameObject> object);
    int32_t putObjectInSlot(uint32_t slot, ee::Ptr<GameObject> object);
    ee::Ptr<GameObject> objectInSlot(uint32_t slot);
    ee::Ptr<GameObject> takeFromSlot(uint32_t slot);
    ee::Ptr<GameObject> firstInSlot(uint32_t slot);
}

namespace aggressive
{
    // (objects.cpp)
    int32_t beginEndAndQueueObject(ee::Ptr<aggressive::skater::Player> player, ee::Ptr<GameObject> object);
    int32_t createSlot(ee::Address owner);
    int32_t destroySlot(uint32_t slot);
    int32_t emptySlot(uint32_t slot);
    void forEachInSlot(uint32_t slot, uint32_t visit, ee::Address context);
    void forEachInSlotFromLast(uint32_t slot, uint32_t visit, ee::Address context);
    void freeKeyedValues();
    uint32_t freeObjectClasses();
    uint32_t freeSlotPools();
    uint32_t newSlot(ee::Address owner);
    uint32_t registerObjectClass(uint32_t kind);
    int32_t unregisterObjectClass(uint64_t kind);
    uint32_t restoreSlotObjects(uint32_t slot, uint32_t buffer);
    uint32_t saveSlotObjects(ee::Ptr<zge::List> slot, uint32_t buffer, uint32_t bytes);
    uint32_t setClassHandler(uint64_t kind, uint32_t message, uint32_t handler);
    void setGameObjectState(ee::Ptr<GameObject> object, ee::Address state, ee::Ptr<uint32_t> previous);
    uint32_t setupKeyedValues();
    uint32_t setupObjectClasses();
    void setupSlotPools();
    int32_t slotNodeEmpty(uint32_t node);
    int32_t slotSaveSize(ee::Ptr<zge::List> slot);
}
