// A dynamics definition's chunks (zge/collision/dynamics.h): ZDYN holds DDEF,
// DBDY and DCON, read into a record of the definitions' pool. A body's bones
// and joints then go in a tree of solver nodes, and an object's dynamics is
// a deep copy of its definition.

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/scene/follower.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics_load.cpp.h"

namespace zge
{
    ee::Float toDynamicsUnits(ee::Float length);

    namespace
    {
        constexpr uint32_t kBodyBlockBytes = 0xEC0;         // ConstraintSystem::bodyBlocks, per body
        constexpr uint32_t kConstraintBytes = 0x30;         // of a kind 0 link
        constexpr uint32_t kTurnFirstLinkUnder = 3;         // bodies placed: from there the first link stays as it is
        constexpr float kDynamicsUnitsPerFileUnit = 0.01f;
        constexpr float kLinkLengthScale = 10.0f;           // DynamicsLink::fileLength
        // A box's inertia: (a^2 + b^2) * mass / 12, scaled: ZGE2 and ZGE3 by 20, ZGE4 by 4.
        constexpr float kBoxInertiaDivisor = 12.0f, kBoxInertiaScale = game::readable::perGame(20.0f, 20.0f, 4.0f);

        enum LinkKind : uint32_t
        {
            kLinkBall = 0,
            kLinkConstraint = 1,
            kLinkHinge = 3,
            kLinkBodyPair = 4,
        };

        // Zeroed bytes off the heap.
        template <typename T>
        ee::Ptr<T> newZeroed(uint32_t bytes = sizeof(T))
        {
            const uint32_t block = heapAllocate(bytes);
            return ee::Ptr<T>(memoryFill(block, 0, bytes));
        }

        // A new copy of a record.
        template <typename T>
        ee::Ptr<T> copyRecord(ee::Ptr<T> source, uint32_t bytes = sizeof(T))
        {
            const ee::Ptr<T> record = newZeroed<T>(bytes);
            memoryCopy(record, source, bytes);
            return record;
        }

        // The chunk readers' two words. (Their code is one code, so a call
        // names which by its address: docs/READABLE.md, "Writing a readable function".)
        void readU32(ee::Ptr<ChunkReader> reader, uint32_t *out) { readChunkU32(reader, out); }
        void readFloat(ee::Ptr<ChunkReader> reader, ee::Float *out) { readChunkWord(reader, out); }

        // A length of the file read through the stack word `word`, in the
        // dynamics' units.
        ee::Float readLength(ee::Ptr<ChunkReader> reader, const ee::Local<ee::Float> &word)
        {
            readFloat(reader, &*word);
            return toDynamicsUnits(*word);
        }

        // Three lengths: x, y and z of a vector.
        void readVector(ee::Ptr<ChunkReader> reader, const ee::Local<ee::Float> &word, Vec4 &out)
        {
            out.x = readLength(reader, word);
            out.y = readLength(reader, word);
            out.z = readLength(reader, word);
        }

        // The body of the definition so far with this name hash, 0 if none.
        ee::Ptr<RigidBody> findBody(uint32_t nameHash)
        {
            ee::Ptr<RigidBody> body = (*g_currentDynamics)->bodies;
            while (body && body->nameHash != nameHash)
                body = body->next;
            return body;
        }

        // ZGE2's joint readers end by putting the joint's two bodies on each other's ignore lists (RigidBody::ignored;
        // the sweep's boxes have the lists in ZGE3). As the original, a body not found (null) is written at all the same.
        void ignoreEachOther(ee::Ptr<BodyConstraint> constraint)
        {
            if constexpr (game::readable::kIsDm2)
            {
                constraint->bodyA->addIgnored(constraint->bodyB);
                constraint->bodyB->addIgnored(constraint->bodyA);
            }
        }
    }

    // linkBodyChildren: a node under another. The child's siblings, if it has
    // any, come with it, ahead of the parent's children.
    int32_t linkBodyChildren(ee::Ptr<SolverNode> parent, ee::Ptr<SolverNode> child)
    {
        parent->childCount = parent->childCount + 1;
        if (child->nextSibling)
        {
            ee::Ptr<SolverNode> last = child->nextSibling;
            for (;;)
            {
                if (!last->nextSibling)
                    break;
                last = last->nextSibling;
            }
            last->nextSibling = parent->firstChild;
        }
        else
            child->nextSibling = parent->firstChild;
        parent->firstChild = child;
        child->parent = parent;
        return 0;
    }
    READABLE_FUNCTION(linkBodyChildren, Verified);

    // buildBodyHierarchy: the solver's nodes, one per body and link, and the tree
    // through them: from the first kind 0 link, each link whose ball joint
    // has one body placed goes under that body with the other body under it.
    // When no link is left to place while under three bodies are in, the
    // first link is turned over once: its body and its parent change places.
    // Bodies no link reaches never get placed, and the loop never ends; the
    // game's files have none.
    void buildBodyHierarchy(ee::Ptr<Dynamics> dynamics)
    {
        ConstraintSystem &system = dynamics->system;
        uint32_t bodyCount = 0;
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
            ++bodyCount;
        uint32_t linkCount = 0;
        for (ee::Ptr<DynamicsLink> link = dynamics->links; link; link = link->next)
            ++linkCount;
        const uint32_t nodeCount = bodyCount + linkCount;
        system.nodes = newZeroed<SolverNode>(nodeCount * sizeof(SolverNode));
        if constexpr (!game::readable::kIsBmx) // (ZGE4's solver has no body blocks)
            system.bodyBlocks = newZeroed<uint8_t>(bodyCount * kBodyBlockBytes);

        const ee::Ptr<SolverNode> nodes = system.nodes;
        ee::Ptr<SolverNode> node = nodes;
        uint32_t index = 0;
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
        {
            node->index = index++;
            node->body = body;
            ++node;
        }
        index = 0;
        for (ee::Ptr<DynamicsLink> link = dynamics->links; link; link = link->next)
        {
            node->index = index++;
            node->link = link;
            node->isLink = 1;
            ++node;
        }

        // The node of a body: bodyCount if none has it.
        const auto indexOfBody = [&](ee::Ptr<RigidBody> body)
        {
            for (uint32_t i = 0; i < bodyCount; ++i)
                if (nodes[i].body == body)
                    return i;
            return bodyCount;
        };

        uint32_t placed = 0;
        bool canTurnFirstLink = true;
        while (bodyCount != 0)
        {
            ee::Ptr<RigidBody> inTree, outside;
            uint32_t linkIndex = bodyCount;

            // The ends of a ball joint against the bodies in the tree.
            const auto findPlacedEnd = [&](ee::Ptr<BodyConstraint> constraint)
            {
                for (uint32_t i = 0; i < bodyCount; ++i)
                {
                    const SolverNode &candidate = nodes[i];
                    if (!candidate.firstChild && !candidate.parent)
                        continue;
                    if (constraint->bodyA == candidate.body)
                    {
                        outside = constraint->bodyB;
                        inTree = constraint->bodyA;
                        return;
                    }
                    if (constraint->bodyB == candidate.body)
                    {
                        inTree = constraint->bodyB;
                        outside = constraint->bodyA;
                        return;
                    }
                }
            };

            for (; linkIndex < nodeCount; ++linkIndex)
            {
                const SolverNode &linkNode = nodes[linkIndex];
                if (linkNode.childCount != 0)
                    continue;
                if (linkNode.link->kind == kLinkBall)
                {
                    const ee::Ptr<BodyConstraint> constraint = linkNode.link->constraint;
                    if (placed == 0)
                    {
                        outside = constraint->bodyB;
                        inTree = constraint->bodyA;
                    }
                    else
                        findPlacedEnd(constraint);
                }
                if (inTree)
                    break;
            }
            if (placed >= kTurnFirstLinkUnder)
                canTurnFirstLink = false;

            if (inTree)
            {
                const uint32_t bodyIndex = indexOfBody(inTree);
                if (placed == 0)
                    placed = 1;
                linkBodyChildren(nodes + bodyIndex, nodes + linkIndex);
            }
            else if (canTurnFirstLink)
            {
                canTurnFirstLink = false;
                linkIndex = bodyCount;
                const ee::Ptr<SolverNode> firstLink = nodes + bodyCount;
                const ee::Ptr<SolverNode> body = firstLink->firstChild, parent = firstLink->parent;
                firstLink->parent = body;
                firstLink->firstChild = parent;
                body->parent = ee::Ptr<SolverNode>();
                parent->firstChild = ee::Ptr<SolverNode>();
                body->firstChild = firstLink;
                parent->childCount = 0;
                parent->parent = firstLink;
                body->childCount = 1;
            }
            if (outside)
            {
                const uint32_t bodyIndex = indexOfBody(outside);
                ++placed;
                linkBodyChildren(nodes + linkIndex, nodes + bodyIndex);
            }
            if (placed >= bodyCount)
                break;
        }
        const uint32_t orderBytes = nodeCount * sizeof(ee::Ptr<SolverNode>);
        system.bodyCount = bodyCount;
        system.linkCount = linkCount;
        system.order = ee::Ptr<ee::Ptr<SolverNode>>(heapAllocate(orderBytes));
        system.reversedOrder = ee::Ptr<ee::Ptr<SolverNode>>(heapAllocate(orderBytes));

        // The root: the first body with no parent.
        for (uint32_t i = 0; i < bodyCount; ++i)
        {
            if (!nodes[i].parent)
            {
                system.root = nodes + i;
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(buildBodyHierarchy, Verified);

    // toDynamicsUnits: the files' lengths to the dynamics' units.
    ee::Float toDynamicsUnits(ee::Float length) { return length * ee::Float(kDynamicsUnitsPerFileUnit); }
    READABLE_FUNCTION(toDynamicsUnits, Verified);

    // readBodyDefinition: a box body's half extents, and how long they are.
    ee::Ptr<BodyExtents> readBodyDefinition(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<ee::Float> word;
        const ee::Ptr<BodyExtents> extents(heapAllocate(sizeof(BodyExtents)));
        readVector(reader, word, extents->half);
        extents->length = vec3Length(&extents->half.xyz());
        return extents;
    }
    READABLE_FUNCTION_WAITS(readBodyDefinition, Verified);

    // readConstraint: a ball joint: for each end, the body by its name hash
    // (left null when no body of the definition so far has it) and the
    // anchor in the body's frame, with its length.
    ee::Ptr<BodyConstraint> readConstraint(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<uint32_t> nameHash;
        ee::Local<ee::Float> word;
        const ee::Ptr<BodyConstraint> constraint(heapAllocate(kConstraintBytes));
        memoryFill(constraint, 0, kConstraintBytes);
        readU32(reader, &*nameHash);
        readVector(reader, word, constraint->anchorA);
        constraint->anchorLengthA = vec3Length(&constraint->anchorA.xyz());
        constraint->bodyA = findBody(*nameHash);

        readU32(reader, &*nameHash);
        readVector(reader, word, constraint->anchorB);
        constraint->anchorLengthB = vec3Length(&constraint->anchorB.xyz());
        constraint->bodyB = findBody(*nameHash);
        ignoreEachOther(constraint);
        return constraint;
    }
    READABLE_FUNCTION_WAITS(readConstraint, Verified);

    // readDdefChunk: DDEF: the definition's name hash.
    uint32_t readDdefChunk(ee::Ptr<ChunkReader> reader)
    {
        readU32(reader, &(*g_currentDynamics)->nameHash);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readDdefChunk, Verified);

    // readDbdyChunk: DBDY: a body, at the front of the definition's list: its
    // shape, name hash, mass and box size, its spheres, and for a box its
    // half extents and the inertia of that box, as a diagonal.
    uint32_t readDbdyChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<ee::Float> word;
        const ee::Ptr<RigidBody> body(heapAllocate(kRigidBodyBytes));
        memoryFill(body, 0, kRigidBodyBytes);
        const ee::Ptr<Dynamics> definition = *g_currentDynamics;
        body->next = definition->bodies;
        definition->bodies = body;
        readU32(reader, &body->shape);
        readU32(reader, &body->nameHash);
        readFloat(reader, &body->mass);
        readVector(reader, word, body->boxSize);
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 keeps no spheres: the count stays on the stack and every body's records (four numbers each) are
            // read past, whatever its shape (the original tests the shape only in the skipped branch's delay slot).
            ee::Local<uint32_t> sphereCount;
            readU32(reader, sphereCount);
            if (*sphereCount != 0)
            {
                for (uint32_t index = 0; index < *sphereCount; ++index)
                {
                    readFloat(reader, word);
                    readFloat(reader, word);
                    readFloat(reader, word);
                    readFloat(reader, word);
                }
            }
        }
        else
        {
            readU32(reader, &body->sphereCount);
            if (body->sphereCount == 0)
                body->spheres = ee::Ptr<BodySphere>();
            else
            {
                body->spheres = ee::Ptr<BodySphere>(heapAllocate(body->sphereCount * sizeof(BodySphere)));
                for (uint32_t index = 0; index < body->sphereCount; ++index)
                {
                    BodySphere &sphere = body->spheres[index];
                    sphere.centre.x = readLength(reader, word);
                    sphere.centre.y = readLength(reader, word);
                    sphere.centre.z = readLength(reader, word);
                    sphere.radius = readLength(reader, word);
                    sphere.aroundOrigin = dot(sphere.centre, sphere.centre) < sphere.radius * sphere.radius ? 1 : 0;
                }
            }
        }
        if (body->shape == 0)
        {
            const ee::Ptr<BodyExtents> extents = readBodyDefinition(reader);
            body->halfExtents = extents;
            matrixIdentity(&body->inertia);
            matrixIdentity(&body->inverseInertia);
            const ee::Float mass = body->mass;
            const ee::Float x = extents->half.x, y = extents->half.y, z = extents->half.z;
            const auto inertiaAbout = [&](ee::Float a, ee::Float b) { return (a * a + b * b) * mass / ee::Float(kBoxInertiaDivisor) * ee::Float(kBoxInertiaScale); };
            const ee::Float aboutX = inertiaAbout(y, z), aboutY = inertiaAbout(x, z), aboutZ = inertiaAbout(y, x);
            body->inertia.m[0][0] = aboutX;
            body->inertia.m[1][1] = aboutY;
            body->inverseInertia.m[0][0] = ee::Float(1.0f) / aboutX;
            body->inverseInertia.m[1][1] = ee::Float(1.0f) / aboutY;
            body->inertia.m[2][2] = aboutZ;
            body->inverseInertia.m[2][2] = ee::Float(1.0f) / aboutZ;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readDbdyChunk, Verified);

    // readDynamicsConstraint: DCON kind 1: readConstraint's two bodies and
    // anchors in a 0x40-byte record, with an axis after them (three lengths);
    // the second body is looked up after the axis is read.
    ee::Ptr<BodyConstraint> readDynamicsConstraint(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<uint32_t> nameHash;
        ee::Local<ee::Float> word;
        const ee::Ptr<BodyConstraint> constraint(heapAllocate(sizeof(BodyConstraint)));
        memoryFill(constraint, 0, sizeof(BodyConstraint));
        readU32(reader, &*nameHash);
        readVector(reader, word, constraint->anchorA);
        constraint->anchorLengthA = vec3Length(&constraint->anchorA.xyz());
        constraint->bodyA = findBody(*nameHash);

        readU32(reader, &*nameHash);
        readVector(reader, word, constraint->anchorB);
        constraint->anchorLengthB = vec3Length(&constraint->anchorB.xyz());
        readVector(reader, word, constraint->axis);
        constraint->bodyB = findBody(*nameHash);
        ignoreEachOther(constraint);
        return constraint;
    }
    READABLE_FUNCTION_WAITS(readDynamicsConstraint, Draft);

    // readDynamicsHinge: DCON kind 3: a body by its name hash, made 1000
    // times as heavy and marked (RigidBody::linkKind3Mark), and a word of the
    // file, in a 0x30-byte record.
    // Kept bug: when no body has the name, the mark and the mass are written
    // through a null body, at those offsets from address 0.
    ee::Ptr<BodyHinge> readDynamicsHinge(ee::Ptr<ChunkReader> reader)
    {
        constexpr float kHeavier = 1e+03f;
        ee::Local<uint32_t> nameHash;
        const ee::Ptr<BodyHinge> hinge(heapAllocate(sizeof(BodyHinge)));
        memoryFill(hinge, 0, sizeof(BodyHinge));
        readU32(reader, &*nameHash);
        readU32(reader, &hinge->fileWord);
        const ee::Ptr<RigidBody> body = findBody(*nameHash);
        if (body)
            hinge->body = body;
        const ee::Float mass = body->mass;
        body->linkKind3Mark = 1;
        body->mass = mass * ee::Float(kHeavier);
        return hinge;
    }
    READABLE_FUNCTION_WAITS(readDynamicsHinge, Draft);

    // readDynamicsBodyPair: DCON kind 4: two bodies by their name hashes (0
    // for a name no body has).
    ee::Ptr<BodyPair> readDynamicsBodyPair(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<uint32_t> nameHash;
        const ee::Ptr<BodyPair> pair(heapAllocate(sizeof(BodyPair)));
        memoryFill(pair, 0, sizeof(BodyPair));
        readU32(reader, &*nameHash);
        const ee::Ptr<RigidBody> first = findBody(*nameHash);
        readU32(reader, &*nameHash);
        const ee::Ptr<RigidBody> second = findBody(*nameHash);
        pair->first = first;
        pair->second = second;
        return pair;
    }
    READABLE_FUNCTION_WAITS(readDynamicsBodyPair, Draft);

    namespace
    {
        // A link's record, whatever its kind: the solver reads the two bodies
        // every kind starts with (DynamicsLink::constraint).
        ee::Ptr<BodyConstraint> linkRecord(ee::Address record) { return ee::Ptr<BodyConstraint>(record); }
    }

    // readDconChunk: DCON: a link, at the front of the definition's list, its
    // kind's record after it. A kind it doesn't know gets none.
    uint32_t readDconChunk(ee::Ptr<ChunkReader> reader)
    {
        ee::Local<ee::Float> word;
        const ee::Ptr<DynamicsLink> link(heapAllocate(sizeof(DynamicsLink)));
        memoryFill(link, 0, sizeof(DynamicsLink));
        const ee::Ptr<Dynamics> definition = *g_currentDynamics;
        link->next = definition->links;
        definition->links = link;
        readU32(reader, &link->kind);
        readU32(reader, &link->rigidBody);
        link->fileLength = readLength(reader, word) * ee::Float(kLinkLengthScale);

        switch (link->kind)
        {
        case kLinkBall:
            link->constraint = readConstraint(reader);
            break;
        case kLinkConstraint:
            link->constraint = readDynamicsConstraint(reader);
            break;
        case kLinkHinge:
            link->constraint = linkRecord(readDynamicsHinge(reader));
            break;
        case kLinkBodyPair:
            link->constraint = linkRecord(readDynamicsBodyPair(reader));
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readDconChunk, Verified);

    // readZdynChunk: ZDYN: a definition, given to whoever asked.
    uint32_t readZdynChunk(ee::Ptr<ChunkReader> reader, ee::Ptr<ee::Ptr<Dynamics>> out)
    {
        *out = loadDynamicsDefinition(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZdynChunk, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Address block) { ee::tailCall<int32_t>(sym::heapFree, block); }
        inline void poolFree(uint32_t pool, ee::Address record) { ee::tailCall<int32_t>(sym::poolFree, pool, record); }
        }
    }

    // freeDynamicsBodies: what a dynamics owns: its bodies and their records, its
    // links and theirs, and the solver's tables if it has them.
    void freeDynamicsBodies(ee::Ptr<Dynamics> dynamics)
    {
        for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = dynamics->bodies)
        {
            dynamics->bodies = body->next;
            if (body->halfExtents)
                heapFree(body->halfExtents);
            if constexpr (!game::readable::kIsBmx) // (ZGE4's bodies keep no spheres)
                if (body->spheres)
                    heapFree(body->spheres);
            heapFree(body);
        }
        for (ee::Ptr<DynamicsLink> link = dynamics->links; link; link = dynamics->links)
        {
            dynamics->links = link->next;
            if (link->constraint)
                heapFree(link->constraint);
            heapFree(link);
        }
        if (dynamics->system.nodes)
        {
            heapFree(dynamics->system.order);
            heapFree(dynamics->system.reversedOrder);
            if constexpr (game::readable::kIsBmx) // (ZGE4's solver has no body blocks: buildBodyHierarchy)
                tail::heapFree(dynamics->system.nodes);
            else
            {
                heapFree(dynamics->system.nodes);
                tail::heapFree(dynamics->system.bodyBlocks);
            }
        }
    }
    READABLE_FUNCTION_WAITS(freeDynamicsBodies, Verified);

    // freeDynamicsRecord: a dynamics and all it owns.
    void freeDynamicsRecord(ee::Ptr<Dynamics> dynamics)
    {
        freeDynamicsBodies(dynamics);
        tail::heapFree(dynamics);
    }
    READABLE_FUNCTION_WAITS(freeDynamicsRecord, Verified);

    // freeDynamicsCopy (ZGE4 only): a dynamics copyDynamicsDefinition took from the definitions' pool, and all it
    // owns: freeDynamicsRecord's, the record back to the pool.
    inline void freeDynamicsCopy(ee::Ptr<Dynamics> dynamics)
    {
        freeDynamicsBodies(dynamics);
        tail::poolFree(*g_dynamicsDefinitionPool, ee::Address(dynamics));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeDynamicsCopy);

    // setDynamicsRestitution: how much of a contact's speed it keeps.
    void setDynamicsRestitution(ee::Ptr<Dynamics> dynamics, ee::Float restitution) { dynamics->restitution = restitution; }
    READABLE_FUNCTION(setDynamicsRestitution, Verified);

    // setDynamicsContactHandler: what is asked about each face a body touches.
    void setDynamicsContactHandler(ee::Ptr<Dynamics> dynamics, ContactCallback handler) { dynamics->contactCallback = handler; }
    READABLE_FUNCTION(setDynamicsContactHandler, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t firstLink(ee::Ptr<PoolList> list) { return poolFirst(list); }
        uint32_t nextLink(uint32_t link) { return poolNext(ee::Ptr<PoolLink>(link)); }
        ee::Ptr<Dynamics> recordOfLink(uint32_t link) { return poolRecordAs<Dynamics>(link); }
    }

    namespace
    {
        // The node's name hash: ZGE4 reads it (the hash sits at the node's front there); ZGE3 asks the scene for it.
        uint32_t nameHashOfNode(ee::Ptr<Node> node)
        {
            if constexpr (game::readable::kIsBmx)
                return node->nameHash;
            else
                return nodeNameHash(node);
        }

        // A deep copy of a definition (see cloneDynamicsDefinition); with
        // `rename`, it and its first body take the node's name hash. With `pooled` (ZGE4's copyDynamicsDefinition),
        // the record comes from the definitions' pool, made here when this is the first; else from the heap.
        ee::Ptr<Dynamics> copyDefinition(ee::Ptr<Dynamics> definition, bool rename, ee::Ptr<Node> node, bool pooled = false)
        {
            ee::Ptr<Dynamics> dynamics;
            if (pooled)
            {
                if (*g_dynamicsDefinitionPool == 0)
                    *g_dynamicsDefinitionPool = createPool(kDynamicsBytes, 0u, sym::freeDynamicsDefinitionRecord);
                dynamics = ee::Ptr<Dynamics>(poolAlloc(ee::Ptr<Pool>(*g_dynamicsDefinitionPool)));
                memoryCopy(dynamics, definition, kDynamicsBytes);
            }
            else
                dynamics = copyRecord(definition);
            if (rename)
                dynamics->nameHash = nameHashOfNode(node);

            ee::Ptr<RigidBody> lastBody;
            for (ee::Ptr<RigidBody> source = definition->bodies; source; source = source->next)
            {
                const ee::Ptr<RigidBody> body = copyRecord(source, kRigidBodyBytes);
                if (!lastBody)
                {
                    dynamics->bodies = body;
                    if (rename)
                        dynamics->bodies->nameHash = nameHashOfNode(node);
                }
                else
                    lastBody->next = body;
                lastBody = body;
                if (source->halfExtents)
                {
                    if (source->shape != 0)
                        return ee::Ptr<Dynamics>();
                    body->halfExtents = copyRecord(source->halfExtents);
                }
            }

            ee::Ptr<DynamicsLink> lastLink;
            for (ee::Ptr<DynamicsLink> sourceLink = definition->links; sourceLink; sourceLink = sourceLink->next)
            {
                const ee::Ptr<DynamicsLink> link = copyRecord(sourceLink);
                if (!lastLink)
                    dynamics->links = link;
                else
                    lastLink->next = link;
                lastLink = link;
                if (sourceLink->constraint && sourceLink->kind == kLinkBall)
                {
                    const ee::Ptr<BodyConstraint> constraint = copyRecord(sourceLink->constraint, kConstraintBytes);
                    // Its ends onto this dynamics' bodies, by name hash.
                    for (ee::Ptr<RigidBody> body = dynamics->bodies; body; body = body->next)
                    {
                        if (body->nameHash == constraint->bodyA->nameHash)
                            constraint->bodyA = body;
                        if (body->nameHash == constraint->bodyB->nameHash)
                            constraint->bodyB = body;
                    }
                    link->constraint = constraint;
                }
            }
            if (definition->system.nodes)
                buildBodyHierarchy(dynamics);
            return dynamics;
        }
    }

    // cloneDynamicsDefinition: an object's own dynamics: a deep copy of the definition
    // of that name, by its node's name hash, 0 if there is none. Its first
    // body takes the node's name hash too, so a joint on that body stays on
    // the definition's body. A body not a box with half extents ends the copy
    // where it stands and returns 0 (the game's are boxes); a link not kind 0
    // shares the definition's record.
    ee::Ptr<Dynamics> cloneDynamicsDefinition(ee::Ptr<Node> node, uint32_t definitionName)
    {
        ee::Ptr<Dynamics> definition;
        for (uint32_t link = firstLink(ee::Ptr<PoolList>(*g_dynamicsDefinitionPool));; link = nextLink(link))
        {
            if (link == 0)
                return ee::Ptr<Dynamics>();
            definition = recordOfLink(link);
            if (definition->nameHash == definitionName)
                break;
        }

        return copyDefinition(definition, true, node);
    }
    READABLE_FUNCTION_WAITS(cloneDynamicsDefinition, Verified);

    // copyDynamicsDefinition: cloneDynamicsDefinition's copy of a definition
    // handed in, with the names as they are. Its first argument is not read.
    // (Nothing in the game calls it.)
    ee::Ptr<Dynamics> copyDynamicsDefinition(uint32_t /*node, not read*/, ee::Ptr<Dynamics> definition)
    {
        // (ZGE4 takes this copy from the definitions' pool; its clone, like ZGE3's, from the heap.)
        return copyDefinition(definition, false, ee::Ptr<Node>(), game::readable::kIsBmx);
    }
    READABLE_FUNCTION_WAITS(copyDynamicsDefinition, Draft);

    // copyDynamicsFirstBody (ZGE4 only): a copy of a definition from the definitions' pool with its first body
    // alone (its box's half extents its own; a body not a box with half extents: 0), no links and an empty
    // system: BMX XXX's bail dynamics, "dynamics.rider" (newRider). Its first argument is not read. (Named from its
    // use; no build names it.)
    inline ee::Ptr<Dynamics> copyDynamicsFirstBody(uint32_t /*object, not read*/, ee::Ptr<Dynamics> definition)
    {
        const ee::Ptr<Dynamics> dynamics(poolAlloc(ee::Ptr<Pool>(*g_dynamicsDefinitionPool)));
        memoryCopy(dynamics, definition, kDynamicsBytes);
        if (const ee::Ptr<RigidBody> source = definition->bodies)
        {
            const ee::Ptr<RigidBody> body = copyRecord(source, kRigidBodyBytes);
            dynamics->bodies = body;
            if (source->halfExtents)
            {
                if (source->shape != 0)
                    return ee::Ptr<Dynamics>();
                body->halfExtents = copyRecord(source->halfExtents);
            }
            body->next = ee::Ptr<RigidBody>();
        }
        dynamics->links = ee::Ptr<DynamicsLink>();
        static_assert(sizeof(ConstraintSystem) == 0x1C);
        memoryFill(&dynamics->system, 0, sizeof(ConstraintSystem));
        return dynamics;
    }
    READABLE_FUNCTION_ONLY(bmx, copyDynamicsFirstBody);

    namespace
    {
        inline ee::Global<int32_t, sym::g_collideWithFaces> g_collideWithFacesWord; // g_collideWithFacesWord (g_collideWithFacesWord)
    }

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float vec3Length(ee::Ptr<const Vec3> anchorA) { return zge::vec3Length(anchorA); }
    }

    // readDbncChunk: the DBNC chunk: the dynamics' restitution, one word.
    uint32_t readDbncChunk(ee::Ptr<ChunkReader> chunkReader)
    {
        zge::readChunkWord(chunkReader, &(*g_currentDynamics)->restitution);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readDbncChunk, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        Vec3 * vec3Negate(ee::Ptr<Vec3> vec3, ee::Ptr<const Vec3> vec32) { return zge::vec3Negate(vec3, vec32); }
    }

    // The contact query's results when debug drawing is on (g_collideWithFacesWord), else 0.
    uint32_t dynamicsDebugRecord()
    {
        if (*g_collideWithFacesWord == 0)
            return 0;
        return sym::g_contactObjects;
    }
    READABLE_FUNCTION(dynamicsDebugRecord, Draft);

    // The held points of a body (BodyContact; nothing makes one in AI; DM2's
    // newRider holds the bail body's hands; ZGE4's game makes them).

    // newBodyContactRecord: a held point added to the front of the body's
    // list: on, holding `other`, its detail's target `target` and its
    // anchor the negated sum of the body's box size and `offset`.
    ee::Ptr<BodyContact> newBodyContactRecord(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> offset, ee::Ptr<Vec4> target,
                                              ee::Ptr<RigidBody> other)
    {
        const ee::Ptr<BodyContact> contact = newZeroed<BodyContact>();
        const ee::Ptr<BodyContactDetail> detail = newZeroed<BodyContactDetail>();
        contact->kind = kBodyContactHold;
        contact->strength = ee::Float(1.0f);
        contact->detail = detail;
        contact->other = other;
        contact->next = body->contacts;
        body->contacts = contact;
        detail->body = body;
        detail->target = aligned(target);
        detail->anchor = body->boxSize + *offset;
        vec3Negate(&detail->anchor.xyz(), &detail->anchor.xyz());
        return contact;
    }

    // newBodyContactRecordWithSlack: ZGE4's newBodyContactRecord: the held point as above, with the slack the hold
    // allows the point before it pulls (BodyContactDetail::slack; ZGE4's game makes these).
    ee::Ptr<BodyContact> newBodyContactRecordWithSlack(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> offset, ee::Ptr<Vec4> target,
                                                       ee::Ptr<RigidBody> other, ee::Float slack)
    {
        const ee::Ptr<BodyContact> contact = newZeroed<BodyContact>();
        const ee::Ptr<BodyContactDetail> detail = newZeroed<BodyContactDetail>();
        contact->kind = kBodyContactHold;
        contact->strength = ee::Float(1.0f);
        contact->detail = detail;
        contact->other = other;
        contact->next = body->contacts;
        body->contacts = contact;
        detail->body = body;
        detail->target = aligned(target);
        detail->slack = slack;
        detail->anchor = body->boxSize + *offset;
        vec3Negate(&detail->anchor.xyz(), &detail->anchor.xyz());
        return contact;
    }
    READABLE_FUNCTION_WAITS_OWN(newBodyContactRecord, Draft, bmx, newBodyContactRecordWithSlack);

    // findBodyContact: the body's held point that holds `other`, or 0.
    ee::Ptr<BodyContact> findBodyContact(ee::Ptr<RigidBody> body, ee::Ptr<RigidBody> other)
    {
        ee::Ptr<BodyContact> contact = body->contacts;
        while (contact && !same(contact->other, other))
            contact = contact->next;
        return contact;
    }
    READABLE_FUNCTION(findBodyContact, Draft);

    // setContactPoint: a held point's target. ZGE4 accepts no contact (nothing to set).
    void setContactPoint(ee::Ptr<BodyContact> contact, ee::Ptr<Vec4> target)
    {
        if constexpr (game::readable::kIsBmx)
            if (!contact)
                return;
        contact->detail->target = *target;
    }
    READABLE_FUNCTION(setContactPoint, Draft);

    // enableContact, disableContact: a held point on (strength 1) or off. ZGE4 accepts no contact (nothing to set).
    void enableContact(ee::Ptr<BodyContact> contact)
    {
        if constexpr (game::readable::kIsBmx)
            if (!contact)
                return;
        contact->strength = ee::Float(1.0f);
    }
    READABLE_FUNCTION(enableContact, Draft);

    // disableContact: see enableContact.
    void disableContact(ee::Ptr<BodyContact> contact)
    {
        if constexpr (game::readable::kIsBmx)
            if (!contact)
                return;
        contact->strength = ee::Float(0.0f);
    }
    READABLE_FUNCTION(disableContact, Draft);

    namespace
    {
        int32_t heapFreeTail(ee::Ptr<BodyContact> block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
    }

    // removeBodyContact: a held point's detail freed and the point taken off
    // the body's list. As the original: a point not first on the list frees
    // the point before it, not itself (bug kept), and one not on the list
    // walks on past its end. ZGE4 takes no point as nothing to do (the
    // result is then the register as the caller left it: no caller reads
    // it) and frees the point itself.
    int32_t removeBodyContact(ee::Ptr<RigidBody> body, ee::Ptr<BodyContact> contact)
    {
        if constexpr (game::readable::kIsBmx)
        {
            if (!contact)
                return 0;
        }
        zge::heapFree(contact->detail);
        ee::Ptr<BodyContact> before = body->contacts;
        if (same(before, contact))
        {
            body->contacts = contact->next;
            return heapFreeTail(contact);
        }
        while (!same(before->next, contact))
            before = before->next;
        before->next = contact->next;
        if constexpr (game::readable::kIsBmx)
            return heapFreeTail(contact);
        return heapFreeTail(before);
    }
    READABLE_FUNCTION_WAITS(removeBodyContact, Draft);

    // newDynamicsForce: a force along `direction` (copied whole), or 0
    // when the heap has no room.
    ee::Ptr<DynamicsForce> newDynamicsForce(ee::Ptr<Vec4> direction)
    {
        const ee::Ptr<DynamicsForce> force(heapAllocate(sizeof(DynamicsForce)));
        if (!force)
            return force;
        force->direction = aligned(direction);
        force->length = vec3Length(&direction->xyz());
        return force;
    }
    READABLE_FUNCTION_WAITS(newDynamicsForce, Draft);

    // setForceStrength: a force's strength. (The second argument is unused.)
    void setForceStrength(ee::Ptr<DynamicsForce> force, uint32_t, ee::Float strength)
    {
        force->direction.w = strength;
    }
    READABLE_FUNCTION(setForceStrength, Draft);
}
