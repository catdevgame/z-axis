// BMX XXX's "PROXIMITY" objective type: the rider touching a place of the
// level: a face or an edge with a tag (TYPE Face, Edge; NAME the tag's
// word) or a level object (TYPE Entity; NAME its name). BMX XXX's own code
// (its game block); the names are the script's keywords and the type's
// callbacks' places in its registration.

#include "objective_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/proximity_type.cpp.h"

namespace game::bmx::objectives
{
    using rider::Rider;
    namespace collision = rider::collision;

    namespace
    {
        // The block's keys: NAME, TYPE (and two more words of the table).
        constexpr uint32_t kProximityKeys = 4;
        // TYPE's words: "Face", "Edge", "Entity".
        enum ProximityType : uint32_t
        {
            kProximityFace = 0,
            kProximityEdge = 1,
            kProximityEntity = 2,
            kProximityTypes = 3,
        };
        constexpr uint32_t kMaxTargets = 32;

        // One place of a PROXIMITY objective, by its type: a face's or an
        // edge's reference, or a level object in the low word.
        union ProximityTarget
        {
            uint64_t reference;               // a collision::FaceRef or EdgeRef
            ee::Ptr<zge::SceneObject> entity; // (the low word)
        };
        static_assert(sizeof(ProximityTarget) == 8);

        // A PROXIMITY objective's record (0x118 bytes, from the type's pool).
        struct ProximityObjective
        {
            uint32_t type;                             // +0x00 TYPE (ProximityType)
            uint8_t _04[8];
            uint32_t nameHash;                         // +0x0C NAME: a face's or edge's tag word, or the object's name hash
            ProximityTarget targets[kMaxTargets + 1];  // +0x10 the places of that name, ended by a 0
        };
        static_assert(offsetof(ProximityObjective, nameHash) == 0x0C && offsetof(ProximityObjective, targets) == 0x10 && sizeof(ProximityObjective) == 0x118);

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_proximityObjectives> g_proximityObjectives;

        // The highlight's colour for a place to reach.
        constexpr uint32_t kTargetHighlight = highlightColour(0x40, 0xD0, 0x60);
        // The bounds start inside out and end this much wider than the places.
        constexpr ee::Float kFarAway = 99999.9f;
        constexpr ee::Float kFarBelow = -99999.9f;
        constexpr ee::Float kBoundsMargin = 20.0f;

        // The rider's contact kinds this type tells apart
        // (Rider::contactKind): on an edge, on a node that may be a level
        // object's, on a level object. Scaffolding (unnamed-field): the
        // rider lane names only the last.
        constexpr uint8_t kContactEdge = 5;
        constexpr uint8_t kContactObjectNode = 6;

        uint32_t contactWord(const Rider &rider)
        {
            return *reinterpret_cast<const uint32_t *>(rider.contact);
        }
    }

    void readProximityParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readProximityBlock(ee::Ptr<const char> beginKeyword);
    void freeProximity(Objective objective);
    void proximityEvent(Objective objective, ee::Ptr<ObjectiveEvent> event);
    void highlightProximity(Objective objective, ee::Ptr<Rider> rider);
    void updateProximityBounds(Objective objective);
    uint32_t proximityFrame(Objective objective, ee::Ptr<ObjectiveEvent> event);
    ee::Address proximityOf(Objective objective);

    // registerProximityType: the "PROXIMITY" type and its callbacks.
    void registerProximityType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::proximityTypeName);
        setup->hooks.readParameters = sym::readProximityParameters;
        setup->hooks.freeData = sym::freeProximity;
        setup->hooks.onEvent = sym::proximityEvent;
        setup->hooks.drawForRider = sym::highlightProximity;
        setup->hooks.moveBounds = sym::updateProximityBounds;
        setup->hooks.inProgress = sym::proximityFrame;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerProximityType, Verified);

    // readProximityParameters (the type's readParameters): the block
    // PROXIMITY_BEGIN .. PROXIMITY_END is made readable inside the
    // objective's block, with the objective as its owner.
    void readProximityParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kProximityKeys]> keyNames;
        ee::Local<uint32_t[kProximityKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::proximityKeyNames);
        for (uint32_t i = 0; i < kProximityKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kProximityKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::proximityBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::proximityEndKeyword);
        setup->keyCount = kProximityKeys;
        setup->onEnd = sym::readProximityBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::proximityBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readProximityParameters, Verified);

    // readProximityBlock (the block's handler): the objective's record made
    // from the block's values: its TYPE (a face's unless the word is Edge
    // or Entity), and the places NAME names: every face or edge of the
    // level with that tag word, or every level object of that name, 32 at
    // most. Always 1.
    uint32_t readProximityBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> text;
        ee::Local<uint32_t[kProximityTypes]> typeWords;
        const ee::Ptr<uint32_t> words(sym::proximityTypeWords);
        for (uint32_t i = 0; i < kProximityTypes; ++i)
            typeWords[i] = words[i];
        const Objective owner(zge::blockClassOwner(beginKeyword));
        zge::objectiveInfo(owner);
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<ProximityObjective> proximity(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_proximityObjectives), sizeof(ProximityObjective)));
        zge::memoryFill(proximity, 0, sizeof(ProximityObjective));

        zge::formatString(text, sym::bmx::proximityKeyType);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, text);
        if (value)
        {
            zge::scriptValueText(value, text, sizeof(char[0x20]));
            if (zge::stringCompareNoCase(text, typeWords[kProximityFace]) == 0)
                proximity->type = kProximityFace;
            else if (zge::stringCompareNoCase(text, typeWords[kProximityEdge]) == 0)
                proximity->type = kProximityEdge;
            else if (zge::stringCompareNoCase(text, typeWords[kProximityEntity]) == 0)
                proximity->type = kProximityEntity;
        }

        zge::formatString(text, sym::bmx::proximityKeyName);
        value = zge::findScriptValue(values, text);
        if (value)
        {
            zge::scriptValueText(value, text, sizeof(char[0x20]));
            uint32_t count = 0;
            proximity->nameHash = zge::nameHash(text);
            const ee::Ptr<uint32_t> name(&proximity->nameHash);
            if (proximity->type == kProximityFace || proximity->type == kProximityEdge)
            {
                const ee::Ptr<uint64_t> found = proximity->type == kProximityFace ? rider::collision::facesTagged(name, 1) : rider::collision::edgesTagged(name, 1);
                if (found)
                {
                    while (found[count] != 0)
                    {
                        proximity->targets[count].reference = found[count];
                        ++count;
                        if (found[count] != 0 && count >= kMaxTargets)
                            break;
                    }
                }
                proximity->targets[count].reference = 0;
            }
            else if (proximity->type == kProximityEntity)
            {
                const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
                if (found)
                {
                    while (found[count])
                    {
                        proximity->targets[count].entity = found[count];
                        ++count;
                        if (found[count] && count >= kMaxTargets)
                            break;
                    }
                }
                proximity->targets[count].entity = ee::Ptr<zge::SceneObject>();
            }
        }
        zge::setObjectiveData(owner, ee::Address(proximity));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::proximityBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readProximityBlock, Verified);

    // freeProximity (the type's free): the record back to the pool.
    void freeProximity(Objective objective)
    {
        const ee::Address proximity = zge::objectiveData(objective);
        if (proximity)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_proximityObjectives), proximity);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeProximity, Verified);

    // proximityEvent (the type's event), on the frame's event (the engine
    // sends it while the rider is outside the objective's bounds or the
    // type's frame answers 0): the objective is done when what the player's
    // rider is in contact with is one of the named places: for a face, the
    // contact's face has the tag word (every contact kind but the edge's
    // and the two objects'); for an edge, the rider is on an edge with the
    // tag word; for an entity, the contact is the object of that name, or a
    // node of it.
    void proximityEvent(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        const ee::Ptr<ProximityObjective> proximity(proximityOf(objective));
        if (event->kind != kFrameEvent)
            return;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        if (proximity->type == kProximityEdge)
        {
            if (rider->contactKind != kContactEdge)
                return;
            if (collision::edgeTagWord(contactReference(*rider)) != proximity->nameHash)
                return;
        }
        else if (proximity->type == kProximityFace)
        {
            const uint8_t kind = rider->contactKind;
            if (kind >= rider::kContactKinds || kind == kContactEdge || kind == kContactObjectNode || kind == rider::kContactObject)
                return;
            if (collision::faceTagWord(contactReference(*rider)) != proximity->nameHash)
                return;
        }
        else if (proximity->type == kProximityEntity)
        {
            if (rider->contactKind == rider::kContactObject)
            {
                if (zge::objectNameHash(ee::Ptr<zge::SceneObject>(contactWord(*rider))) != proximity->nameHash)
                    return;
            }
            else if (rider->contactKind == kContactObjectNode)
            {
                const ee::Ptr<zge::Node> node(contactWord(*rider));
                if (!node)
                    return;
                const ee::Ptr<zge::SceneObject> object = zge::nodeSceneObject(node, ee::Ptr<ee::Ptr<zge::SceneBone>>());
                if (!object)
                    return;
                if (zge::objectNameHash(object) != proximity->nameHash)
                    return;
            }
            else
                return;
        }
        else
            return;
        calls::setObjectiveStateTail(objective, kObjectiveDone);
    }
    READABLE_FUNCTION_WAITS(proximityEvent, Verified);

    // highlightProximity (the type's highlight): every place of the
    // objective highlighted for the rider.
    void highlightProximity(Objective objective, ee::Ptr<Rider> rider)
    {
        const ee::Ptr<ProximityObjective> proximity(proximityOf(objective));
        const ee::Address from(&rider->position);
        if (proximity->type == kProximityEdge)
        {
            for (uint32_t i = 0; proximity->targets[i].reference != 0; ++i)
                highlightEdge(proximity->targets[i].reference, from, kTargetHighlight);
        }
        else if (proximity->type == kProximityFace)
        {
            for (uint32_t i = 0; proximity->targets[i].reference != 0; ++i)
                highlightFace(proximity->targets[i].reference, from, kTargetHighlight);
        }
        else if (proximity->type == kProximityEntity)
        {
            for (uint32_t i = 0; proximity->targets[i].entity; ++i)
                highlightObject(proximity->targets[i].entity, from, kTargetHighlight);
        }
    }
    READABLE_FUNCTION_WAITS(highlightProximity, Draft);

    // updateProximityBounds (the type's updateBounds): the objective's
    // bounds made the box round its places (the faces' centres, the edges'
    // two ends, the objects' nodes' boxes), 20 units wider each way.
    void updateProximityBounds(Objective objective)
    {
        ee::Local<ee::Float[4]> point;
        const ee::Ptr<ProximityObjective> proximity(proximityOf(objective));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        bounds->low.z = kFarAway;
        bounds->low.x = kFarAway;
        bounds->low.y = kFarAway;
        bounds->high.x = kFarBelow;
        bounds->high.z = kFarBelow;
        bounds->high.y = kFarBelow;
        if (proximity->type == kProximityEdge)
        {
            for (uint32_t i = 0; proximity->targets[i].reference != 0; ++i)
            {
                for (uint32_t end = 0; end < 2; ++end)
                {
                    collision::edgePoint(proximity->targets[i].reference, end, point);
                    growBounds(bounds, point);
                }
            }
        }
        else if (proximity->type == kProximityFace)
        {
            for (uint32_t i = 0; proximity->targets[i].reference != 0; ++i)
            {
                collision::faceCentre(proximity->targets[i].reference, point);
                growBounds(bounds, point);
            }
        }
        else if (proximity->type == kProximityEntity)
        {
            for (uint32_t i = 0; proximity->targets[i].entity; ++i)
            {
                const ee::Ptr<zge::Node> node = zge::objectLevelNode(proximity->targets[i].entity);
                zge::boxUnion(bounds, bounds, &node->box());
            }
        }
        const ee::Float minX = bounds->low.x - kBoundsMargin;
        const ee::Float minY = bounds->low.y - kBoundsMargin;
        const ee::Float minZ = bounds->low.z - kBoundsMargin;
        const ee::Float maxX = bounds->high.x + kBoundsMargin;
        const ee::Float maxY = bounds->high.y + kBoundsMargin;
        const ee::Float maxZ = bounds->high.z + kBoundsMargin;
        bounds->low.x = minX;
        bounds->low.y = minY;
        bounds->low.z = minZ;
        bounds->high.x = maxX;
        bounds->high.z = maxZ;
        bounds->high.y = maxY;
    }
    READABLE_FUNCTION_WAITS(updateProximityBounds, Verified);

    // proximityFrame (the type's frame): whether the player's rider is
    // inside the objective's bounds.
    uint32_t proximityFrame(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        ee::Local<zge::Vec4> at;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        *at = *rider::vec4(rider->position);
        if (!(bounds->low.x <= at->x) || !(bounds->low.y <= at->y) || !(bounds->low.z <= at->z))
            return 0;
        if (!(at->x <= bounds->high.x) || !(at->y <= bounds->high.y) || !(at->z <= bounds->high.z))
            return 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(proximityFrame, Verified);

    // proximityOf: the objective's record.
    ee::Address proximityOf(Objective objective)
    {
        return zge::objectiveData(objective);
    }
    READABLE_FUNCTION_WAITS(proximityOf, Verified);
}
