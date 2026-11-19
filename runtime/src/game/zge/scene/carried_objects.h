#pragma once

// Objects carried by objects (carried_objects.cpp; AI's Animal Rescue
// parrots on the skater): a carrier has a record in a tree with the list of
// what it carries, each carried object an entry in that list and an
// attachment in a second tree by its object, so either side finds the other.

#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/carried_objects.h.h"

namespace zge
{
    struct SceneBone;

    struct CarriedAnimal;
    // A List of CarriedAnimals (each starts with its list links).
    struct CarriedAnimalList
    {
        ee::Ptr<CarriedAnimal> head;  // +0x00
        ee::Ptr<CarriedAnimal> tail;  // +0x04
        int32_t count;                // +0x08
    };

    // A carrier's record (in the carriers' tree by its object).
    struct AnimalCarrier
    {
        SearchTreeNode node;          // +0x00
        uint32_t carrier;             // +0x08 the carrier's object (the tree's key)
        CarriedAnimalList animals;    // +0x0C
    };
    static_assert(sizeof(AnimalCarrier) == 24 && offsetof(AnimalCarrier, animals) == 0x0C);

    // ZGE4's calls on a carried object (the carrier's attach hands them over), each with the object and the
    // carried objects' step.
    using CarriedCall = ee::FunctionPtr<void(uint32_t object, ee::Float step)>;

    // An object carried, in its carrier's list.
    struct CarriedAnimal
    {
        ee::Ptr<CarriedAnimal> next;     // +0x00 in its carrier's list
        ee::Ptr<CarriedAnimal> previous; // +0x04
        uint32_t animal;                 // +0x08 the carried object
        ee::Ptr<AnimalCarrier> carrier;  // +0x0C
        ee::Ptr<SceneBone> bone;         // +0x10 the carrier's bone it sits on; 0: on the carrier itself (ZGE4's attach
                                         //       takes none)
#if PS2X_GAME_BMX
        // ZGE4 sends no carry messages: the carried object is set up as it is attached, updated twice a frame and
        // let go (detachAnimal) by these.
        uint8_t _14[4];                  //       (0: nothing sets it)
        CarriedCall setUp;               // +0x18
        CarriedCall firstUpdate;         // +0x1C
        CarriedCall secondUpdate;        // +0x20
        CarriedCall letGo;               // +0x24
#endif

        // ZGE4's calls kept on the record as it is attached, and its let-go call (when it has one) made as it is
        // let go. ZGE2's and ZGE3's records keep none: both do nothing there.
#if PS2X_GAME_BMX
        void keepCalls(CarriedCall setUpCall, CarriedCall firstUpdateCall, CarriedCall secondUpdateCall, CarriedCall letGoCall)
        {
            firstUpdate = firstUpdateCall;
            letGo = letGoCall;
            setUp = setUpCall;
            secondUpdate = secondUpdateCall;
        }
        bool hasLetGo() const { return static_cast<bool>(letGo); }
        void callLetGo(ee::Float step) const { letGo(animal, step); }
#else
        void keepCalls(CarriedCall, CarriedCall, CarriedCall, CarriedCall) {}
        bool hasLetGo() const { return false; }
        void callLetGo(ee::Float) const {}
#endif
    };
    static_assert(sizeof(CarriedAnimal) == game::readable::perGame(20u, 20u, 40u) && offsetof(CarriedAnimal, bone) == 0x10);
    // The carried animals' pool's record: BMX XXX's is 40 bytes (its
    // attachCarriedAnimal keeps four more words, which only its own code
    // reads). (DM2's engine has no carried objects: AI's size.)
    inline constexpr uint32_t kCarriedAnimalBytes = game::readable::perGame(20u, 20u, 40u);

    // The parrots' call count AI's game keeps (aggressive/skater/carried.cpp
    // counts it up), cleared when the carried objects start (its name is a
    // low-confidence guess). BMX XXX's engine keeps no such count.
#if !PS2X_GAME_BMX
    inline void clearAnimalCallCount() { *ee::Global<int32_t, sym::g_animalCallTicks>{} = 0; }
#else
    inline void clearAnimalCallCount() {}
#endif

    // (carried_objects.cpp)
    void attachCarriedAnimal(ee::Address carrier, ee::Address animal, ee::Ptr<uint32_t> boneName);
    uint32_t animalCarrier(ee::Address animal);
    int32_t carriedAnimalCount(uint32_t carrier);
    uint32_t detachAnimal(ee::Address animal);
    void detachAllAnimals(uint32_t carrier);
    uint32_t freeCarriedAnimals();
    uint32_t initCarriedAnimals();
    void listCarriedAnimals(uint32_t carrier, ee::Ptr<uint32_t> out);
    void sendParrotAttached(ee::Ptr<CarriedAnimal> carried);
    void sendParrotReleased(ee::Ptr<CarriedAnimal> carried);
    // ZGE4 only: the carry messages with a word, and the carried objects'
    // tags.
    void sendParrotAttachedWith(uint32_t object, uint32_t word);
    void sendParrotReleasedWith(uint32_t object, uint32_t word);
    void setCarriedAnimalTag(uint32_t object, uint32_t tag);
    int32_t carriedAnimalCountWithTag(uint32_t carrier, uint32_t tag);
    void listCarriedAnimalsWithTag(uint32_t carrier, uint32_t tag, ee::Ptr<uint32_t> out);
    // ZGE4's attachCarriedAnimal (BMX XXX's own body, attachCarriedAnimal's _OWN): `carried` carried by `carrier`
    // (let go first when something carries it already), its calls kept in place of the carry messages and set up
    // at once with the carried objects' step. 1.
    uint32_t attachCarriedObject(ee::Address carrier, ee::Address carried, CarriedCall setUp, CarriedCall firstUpdate,
                                 CarriedCall secondUpdate, CarriedCall letGo);
}
