// Objects carried by objects (carried_objects.h; BMX XXX's engine has them,
// AI's game carried its own copy for Animal Rescue's parrots): a carrier has
// a record in g_animalCarrierTree with the list of what it carries; each
// carried object has a CarriedAnimal in that list and an AnimalAttachment in
// g_carriedAnimalTree, so either side finds the other. Both sides hear of an
// object picked up or let go ("ParrotReleased", "ParrotAttached"). The
// records come from the game's pools (zge/game.h).

#include "carried_objects.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/game.h"
#include "zge/memory/alignment.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/carried_objects.cpp.h"

namespace zge
{
    namespace
    {
        // The two trees of carried objects: by object (g_carriedAnimalTree) and by carrier (g_animalCarrierTree).
        inline ee::Global<SearchTree, sym::g_carriedAnimalTree> g_carriedAnimalTree;
        inline ee::Global<SearchTree, sym::g_animalCarrierTree> g_animalCarrierTree;
    }

    namespace
    {
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage; // sendEngineMessage (by address: a game override hooks it)
        inline ee::Global<int32_t, sym::g_parrotAttachedHash> g_parrotAttachedHash;
        inline ee::Global<int32_t, sym::g_parrotReleasedHash> g_parrotReleasedHash;
        constexpr uint32_t kTextParrotAttached = sym::g_textParrotAttached;
        constexpr uint32_t kTextParrotReleased = sym::g_textParrotReleased;
        inline ee::Global<int32_t, sym::g_carriedAnimalPool> g_carriedAnimalPool;
        inline ee::Global<int32_t, sym::g_animalCarrierPool> g_animalCarrierPool; // its name is a low-confidence guess
        inline ee::Global<int32_t, sym::g_animalAttachmentPool> g_animalAttachmentPool; // its name is a low-confidence guess
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(sendEngineMessage, ee::game, kSendEngineMessage);
            GAME_TABLE_ROUTE(sendEngineMessageint32_t, ee::game<int32_t>, kSendEngineMessage);
        }
    }

    uint32_t initCarriedAnimals()
    {
        initSearchTree(g_carriedAnimalTree.get(), sym::compareCarriedAnimals);
        initSearchTree(g_animalCarrierTree.get(), sym::lessWord);
        const int32_t poolResult = newRecordPool(kCarriedAnimalBytes, 8, 0);
        *g_carriedAnimalPool = poolResult;
        const int32_t pool_2 = newRecordPool(12, 8, 0);
        *g_animalAttachmentPool = pool_2;
        const int32_t pool_3 = newRecordPool(24, 2, 0);
        *g_animalCarrierPool = pool_3;
        const int32_t nameHashResult = nameHash(ee::Ptr<const char>(static_cast<uint32_t>(kTextParrotAttached)));
        *g_parrotAttachedHash = nameHashResult;
        const int32_t nameHash_2 = nameHash(ee::Ptr<const char>(static_cast<uint32_t>(kTextParrotReleased)));
        *g_parrotReleasedHash = nameHash_2;
        clearAnimalCallCount();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCarriedAnimals, Verified);

    // freeCarriedAnimals: the three pools freed and forgotten. ZGE4 frees the attachments' before the carriers'.
    uint32_t freeCarriedAnimals()
    {
        freeRecordPool(*g_carriedAnimalPool);
        if constexpr (game::readable::kIsBmx)
        {
            freeRecordPool(*g_animalAttachmentPool);
            freeRecordPool(*g_animalCarrierPool);
        }
        else
        {
            freeRecordPool(*g_animalCarrierPool);
            freeRecordPool(*g_animalAttachmentPool);
        }
        *g_carriedAnimalPool = 0;
        *g_animalCarrierPool = 0;
        *g_animalAttachmentPool = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeCarriedAnimals, Verified);

    // ---- the animals carried (Animal Rescue's parrots; were the sweep's
    // compareCarriedAnimals .. animalCarrier). A carrier (the skater) has a
    // record in g_animalCarrierTree with the list of what it carries; each
    // animal has a CarriedAnimal in that list and an AnimalAttachment in
    // g_carriedAnimalTree, so either side finds the other.

    struct AnimalAttachment
    {
        SearchTreeNode node;        // +0x00
        ee::Ptr<CarriedAnimal> carried;  // +0x08 (the tree's key, by the animal's object)
    };
    static_assert(sizeof(AnimalAttachment) == 12);

    namespace
    {
        // A key for the attachments' tree: what compareCarriedAnimals reads
        // through it (a carried animal's object).
        // The frame's carried-animal record takes the pool's record size, rounded to a quadword (BMX XXX's is
        // 40 bytes: its lookup pointer is at +0x30).
        inline constexpr uint32_t kKeyRecordRoom = roundUp16(kCarriedAnimalBytes);
        struct AttachmentKey
        {
            CarriedAnimal animal;          // +0x00 only its object is set
            uint8_t _14[kKeyRecordRoom - sizeof(CarriedAnimal)];
            ee::Ptr<CarriedAnimal> lookup; // +0x20 (BMX +0x30) the key: the record above
        };
        static_assert(offsetof(AttachmentKey, lookup) == game::readable::perGame(0x20u, 0x20u, 0x30u));
        // The step the carried objects' frame was last given (its frame keeps it for the calls).
        inline ee::Global<ee::Float, sym::bmx::g_carriedObjectsStep> g_carriedObjectsStep;

        // The message both sides hear: its two objects, the receiver's
        // first.
        struct ParrotNews
        {
            uint32_t carrier; // +0x00
            uint32_t animal;  // +0x04
        };

        // The carrier's record of `carrier`, 0 when it carries nothing.
        ee::Ptr<AnimalCarrier> findCarrier(uint32_t carrier)
        {
            ee::Local<uint32_t> key;
            *key = carrier;
            return searchTreeEntry<AnimalCarrier>(searchTreeFind(g_animalCarrierTree.get(), key));
        }

        // News of a carried animal told to the animal (from its carrier)
        // and to the carrier (from the animal), each only when a level
        // object.
        void tellBothSides(ee::Ptr<CarriedAnimal> carried, uint32_t news)
        {
            ee::Local<ParrotNews> both;
            ee::Local<EngineMessage> message;
            memoryFill(both, 0, sizeof(ParrotNews));
            memoryFill(message, 0, sizeof(EngineMessage));
            both->animal = carried->animal;
            message->type = news;
            both->carrier = carried->carrier->carrier;
            message->flags = kMessageNow;
            message->data = both.address();
            if (isLevelObject(ee::Ptr<SceneObject>(both->animal)))
            {
                message->target = both->animal;
                message->sender = both->carrier;
                routed::sendEngineMessage(message);
            }
            if (isLevelObject(ee::Ptr<SceneObject>(both->carrier)))
            {
                message->target = both->carrier;
                message->sender = both->animal;
                routed::sendEngineMessage(message);
            }
        }
    }

    // compareCarriedAnimals: the attachments' order: by the animals' objects.
    uint32_t compareCarriedAnimals(ee::Ptr<ee::Ptr<CarriedAnimal>> a, ee::Ptr<ee::Ptr<CarriedAnimal>> b)
    {
        return (*a)->animal < (*b)->animal ? 1u : 0u;
    }
    READABLE_FUNCTION(compareCarriedAnimals, Verified);

    // sendParrotAttached / sendParrotReleased: "ParrotAttached" or
    // "ParrotReleased" told to both sides. Note: detachAnimal sends
    // "ParrotAttached" and attachCarriedAnimal "ParrotReleased"; the game
    // ships them that way round.
    void sendParrotAttached(ee::Ptr<CarriedAnimal> carried)
    {
        tellBothSides(carried, *g_parrotAttachedHash);
    }
    READABLE_FUNCTION_WAITS(sendParrotAttached, Verified);
    void sendParrotReleased(ee::Ptr<CarriedAnimal> carried)
    {
        tellBothSides(carried, *g_parrotReleasedHash);
    }
    READABLE_FUNCTION_WAITS(sendParrotReleased, Verified);

    // attachCarriedAnimal: `animal` carried by `carrier`, on the carrier's
    // bone named by boneName's hash (none: on the carrier itself). The
    // carrier gets a record with its first animal; both sides hear of it.
    void attachCarriedAnimal(ee::Address carrier, ee::Address animal, ee::Ptr<uint32_t> boneName)
    {
        ee::Ptr<SceneBone> bone;
        if (boneName)
            bone = findSceneModelBone(sceneOf(ee::Ptr<SceneObject>(carrier)), *boneName);
        ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        const bool firstAnimal = !record;
        if (firstAnimal)
        {
            record = ee::Ptr<AnimalCarrier>(takeRecord(*g_animalCarrierPool));
            memoryFill(record, 0, sizeof(AnimalCarrier));
            record->carrier = carrier;
            initList(&record->animals);
        }
        const ee::Ptr<CarriedAnimal> carried(takeRecord(*g_carriedAnimalPool));
        memoryFill(carried, 0, sizeof(CarriedAnimal));
        const ee::Ptr<AnimalAttachment> attachment(takeRecord(*g_animalAttachmentPool));
        memoryFill(attachment, 0, sizeof(AnimalAttachment));
        carried->animal = animal;
        carried->bone = bone;
        carried->carrier = record;
        attachment->carried = carried;
        if (firstAnimal)
            searchTreeInsert(g_animalCarrierTree.get(), &record->node);
        listPush(&record->animals, carried);
        searchTreeInsert(g_carriedAnimalTree.get(), &attachment->node);
        sendParrotReleased(carried);
    }
    // (registered with ZGE4's own body, attachCarriedObject, below)

    // carriedObjectsStep (ZGE4 only): the step the carried objects' frame was last given.
    inline ee::Float carriedObjectsStep() { return *g_carriedObjectsStep; }
    READABLE_FUNCTION_ONLY(bmx, carriedObjectsStep);

    namespace
    {
        // ZGE4's calls kept on a carried object's record (CarriedAnimal's BMX XXX words: no other version has them).
        inline void keepCarriedCalls(ee::Ptr<CarriedAnimal> carried, CarriedCall setUp, CarriedCall firstUpdate, CarriedCall secondUpdate,
                                     CarriedCall letGo)
        {
            carried->keepCalls(setUp, firstUpdate, secondUpdate, letGo);
        }
    }

    // attachCarriedObject: ZGE4's attachCarriedAnimal: `carried` carried by `carrier` (let go first when something
    // carries it already), its calls kept in place of the carry messages, and set up at once (with the carried
    // objects' step). The carrier gets a record with its first object. 1.
    uint32_t attachCarriedObject(ee::Address carrier, ee::Address carried, CarriedCall setUp, CarriedCall firstUpdate,
                                 CarriedCall secondUpdate, CarriedCall letGo)
    {
        if (animalCarrier(carried) != 0)
            detachAnimal(carried);
        ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        const bool firstObject = !record;
        if (firstObject)
        {
            record = ee::Ptr<AnimalCarrier>(takeRecord(*g_animalCarrierPool));
            memoryFill(record, 0, sizeof(AnimalCarrier));
            record->carrier = carrier;
            initList(&record->animals);
        }
        const ee::Ptr<CarriedAnimal> object(takeRecord(*g_carriedAnimalPool));
        memoryFill(object, 0, sizeof(CarriedAnimal));
        const ee::Ptr<AnimalAttachment> attachment(takeRecord(*g_animalAttachmentPool));
        memoryFill(attachment, 0, sizeof(AnimalAttachment));
        object->animal = carried;
        object->carrier = record;
        attachment->carried = object;
        if (firstObject)
            searchTreeInsert(g_animalCarrierTree.get(), &record->node);
        listPush(&record->animals, object);
        searchTreeInsert(g_carriedAnimalTree.get(), &attachment->node);
        keepCarriedCalls(object, setUp, firstUpdate, secondUpdate, letGo);
        if (setUp)
            setUp(object->animal, carriedObjectsStep());
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(attachCarriedAnimal, Verified, bmx, attachCarriedObject);

    // detachAnimal: the animal let go by whatever carries it (both sides
    // hear of it; ZGE4: its own let-go call, when it has one); its carrier's
    // record goes with its last animal. 1 when it was carried, else 0.
    uint32_t detachAnimal(ee::Address animal)
    {
        ee::Local<AttachmentKey> key;
        ee::Ptr<AnimalAttachment> attachment;
        if (animal)
        {
            key->animal.animal = animal;
            key->lookup = &key->animal;
            attachment = searchTreeEntry<AnimalAttachment>(searchTreeRemove(g_carriedAnimalTree.get(), &key->lookup));
        }
        if (!attachment)
            return 0;
        if constexpr (game::readable::kIsBmx)
        {
            if (attachment->carried->hasLetGo())
                attachment->carried->callLetGo(carriedObjectsStep());
        }
        else
        {
            sendParrotAttached(attachment->carried);
        }
        const ee::Ptr<CarriedAnimal> carried = attachment->carried;
        listRemove(&carried->carrier->animals, carried);
        const ee::Ptr<AnimalCarrier> carrier = attachment->carried->carrier;
        if (carrier->animals.count <= 0)
        {
            searchTreeRemove(g_animalCarrierTree.get(), &carrier->carrier);
            giveRecord(*g_animalCarrierPool, attachment->carried->carrier);
            attachment->carried->carrier = ee::Ptr<AnimalCarrier>();
        }
        giveRecord(*g_carriedAnimalPool, attachment->carried);
        attachment->carried = ee::Ptr<CarriedAnimal>();
        giveRecord(*g_animalAttachmentPool, attachment);
        return 1;
    }
    READABLE_FUNCTION_WAITS(detachAnimal, Verified);

    // detachAllAnimals: every animal the carrier carries let go, first to
    // last (as many as it carried at the start).
    void detachAllAnimals(uint32_t carrier)
    {
        const ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        if (!record)
            return;
        const int32_t count = record->animals.count;
        for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i)
            detachAnimal(record->animals.head->animal);
    }
    READABLE_FUNCTION_WAITS(detachAllAnimals, Draft);

    // carriedAnimalCount: how many animals the carrier carries.
    int32_t carriedAnimalCount(uint32_t carrier)
    {
        const ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        return record ? record->animals.count : 0;
    }
    READABLE_FUNCTION_WAITS(carriedAnimalCount, Draft);

    // listCarriedAnimals: the objects of the animals the carrier carries
    // into out, first to last. A carrier with no record is read at
    // address 0 as the original reads it.
    void listCarriedAnimals(uint32_t carrier, ee::Ptr<uint32_t> out)
    {
        const ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        for (ee::Ptr<CarriedAnimal> node = record->animals.head; node; node = node->next)
        {
            *out = node->animal;
            out += 1;
        }
    }
    READABLE_FUNCTION_WAITS(listCarriedAnimals, Draft);

    // animalCarrier: the object carrying the animal, or 0.
    uint32_t animalCarrier(ee::Address animal)
    {
        ee::Local<AttachmentKey> key;
        key->animal.animal = animal;
        key->lookup = &key->animal;
        const ee::Ptr<AnimalAttachment> attachment = searchTreeEntry<AnimalAttachment>(searchTreeFind(g_carriedAnimalTree.get(), &key->lookup));
        const ee::Ptr<CarriedAnimal> carried = attachment ? attachment->carried : ee::Ptr<CarriedAnimal>();
        return carried ? carried->carrier->carrier : 0;
    }
    READABLE_FUNCTION_WAITS(animalCarrier, Draft);

    // ---- ZGE4's own: carried objects' tags and their messages with a word.

    namespace
    {
        // ZGE4 keeps a tag word where AI's record keeps the bone (its
        // attach takes no bone).
        uint32_t &carriedTag(ee::Ptr<CarriedAnimal> carried)
        {
            return *ee::Ptr<ee::Ptr<SceneBone>>(&carried->bone).cast<uint32_t>();
        }

        // The carried record of an object, or 0.
        ee::Ptr<CarriedAnimal> carriedRecordOf(uint32_t object, ee::Local<AttachmentKey> &key)
        {
            key->animal.animal = object;
            key->lookup = &key->animal;
            const ee::Ptr<AnimalAttachment> attachment =
                searchTreeEntry<AnimalAttachment>(searchTreeFind(g_carriedAnimalTree.get(), &key->lookup));
            return attachment ? attachment->carried : ee::Ptr<CarriedAnimal>();
        }

        // A carry message with a word, sent now to the object (from no one)
        // when it is a level object.
        void sendCarryMessage(uint32_t object, uint32_t type, uint32_t word, ee::Local<uint32_t> &data,
                              ee::Local<EngineMessage> &message)
        {
            memoryFill(data, 0, sizeof(uint32_t));
            memoryFill(message, 0, sizeof(EngineMessage));
            *data = word;
            message->type = type;
            message->data = data.address();
            message->flags = kMessageNow;
            if (isLevelObject(ee::Ptr<SceneObject>(object)))
            {
                message->target = object;
                message->sender = 0;
                routed::sendEngineMessage(message);
            }
        }
    }

    // sendParrotAttachedWith (ZGE4 only): "ParrotAttached" with a word told
    // to a carried object's object (the record's: an object carried by
    // nothing is read at 0, as the original reads it).
    inline void sendParrotAttachedWith(uint32_t object, uint32_t word)
    {
        // (The message is built over the key's place, as the original's.)
        ee::Local<AttachmentKey> key;
        const ee::Ptr<CarriedAnimal> carried = carriedRecordOf(object, key);
        ee::Local<uint32_t> data;
        ee::Local<EngineMessage> message;
        sendCarryMessage(carried->animal, *g_parrotAttachedHash, word, data, message);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, sendParrotAttachedWith);

    // sendParrotReleasedWith (ZGE4 only): "ParrotReleased" with a word told
    // to an object.
    inline void sendParrotReleasedWith(uint32_t object, uint32_t word)
    {
        ee::Local<uint32_t> data;
        ee::Local<EngineMessage> message;
        sendCarryMessage(object, *g_parrotReleasedHash, word, data, message);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, sendParrotReleasedWith);

    // setCarriedAnimalTag (ZGE4 only): a carried object's tag word set
    // (nothing for an object carried by nothing).
    inline void setCarriedAnimalTag(uint32_t object, uint32_t tag)
    {
        ee::Local<AttachmentKey> key;
        const ee::Ptr<CarriedAnimal> carried = carriedRecordOf(object, key);
        if (carried)
            carriedTag(carried) = tag;
    }
    READABLE_FUNCTION_ONLY(bmx, setCarriedAnimalTag);

    // carriedAnimalCountWithTag (ZGE4 only): how many of a carrier's objects
    // have the tag.
    inline int32_t carriedAnimalCountWithTag(uint32_t carrier, uint32_t tag)
    {
        const ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        int32_t count = 0;
        if (!record)
            return count;
        for (ee::Ptr<CarriedAnimal> node = record->animals.head; node; node = node->next)
            if (carriedTag(node) == tag)
                ++count;
        return count;
    }
    READABLE_FUNCTION_ONLY(bmx, carriedAnimalCountWithTag);

    // listCarriedAnimalsWithTag (ZGE4 only): the objects of a carrier's
    // carried objects with the tag into out, first to last.
    inline void listCarriedAnimalsWithTag(uint32_t carrier, uint32_t tag, ee::Ptr<uint32_t> out)
    {
        const ee::Ptr<AnimalCarrier> record = findCarrier(carrier);
        if (!record)
            return;
        for (ee::Ptr<CarriedAnimal> node = record->animals.head; node; node = node->next)
        {
            if (carriedTag(node) == tag)
            {
                *out = node->animal;
                out += 1;
            }
        }
    }
    READABLE_FUNCTION_ONLY(bmx, listCarriedAnimalsWithTag);
}
