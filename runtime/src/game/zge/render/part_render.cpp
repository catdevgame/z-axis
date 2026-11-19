// A part's render state (zge/render/part.h, ZgePart +0x50 .. +0x67) and a
// node's: the part's render type resolved at its first draw, its packet
// buffers shared between clones by a use count at each buffer's end, the
// packets built for a part and for every part, the material chains a
// node's material table sends (each material's GS block, then its strips
// of the part's packet), and a node's own lights.
//

#include "draw.h"
#include "material.h"
#include "packet.h"
#include "part.h"
#include "renderer.h"
#include "vif_codes.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/math/vec.h"
#include "zge/scene/light.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/render/part.h"
#include "zge/debug/profiler.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/part_render.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kHeapFree = sym::heapFree;      // (block)
        constexpr uint32_t kSceVu0CopyMatrix = sym::sceVu0CopyMatrix;
        constexpr uint32_t kVif1PacketAlign = sym::vif1PacketAlign;
        // This file's and the packet module's, called through the table as
        // the original calls them.
        constexpr uint32_t kBufferUseCount = sym::bufferUseCount;
        constexpr uint32_t kRetainPacketBuffer = sym::retainPacketBuffer;
        constexpr uint32_t kReleasePacketBuffer = sym::releasePacketBuffer;
        constexpr uint32_t kResolveRenderType = sym::resolveRenderType;
        constexpr uint32_t kBuildPartPacket = sym::buildPartPacket;   // (part, whole, streams) (ps2/part_packet.cpp; a game override hooks it)
        constexpr uint32_t kBuildMaterialChain = sym::buildMaterialChain;
        constexpr uint32_t kBuildPartPackets = sym::buildPartPackets;
        constexpr uint32_t kFreeNodeLights = sym::freeNodeLights;

        // The render classes' types by TGPH class (ELF data, g_renderClassTypes).
        inline ee::Global<uint32_t[8], sym::g_renderClassTypes> g_renderClassTypes;

        constexpr uint32_t kAllStreams = 0x7FFu;
        constexpr uint32_t typeBits(RenderType type) { return static_cast<uint32_t>(type) << kTypeWordShift; }
    }

    // resolveRenderType: the part's render type, when it has none yet: a part
    // with bones skinned (lit by normals or prelit), else rigid (prelit, or
    // sky for a sky node). Its type word (or the type it had). ZGE2 resolves
    // it at every call and skins every part with bones lit by normals.
    uint32_t resolveRenderType(ee::Ptr<Node> node, ee::Ptr<ZgePart> part)
    {
        const uint32_t typeWord = part->typeWord;
        const uint32_t type = typeWord >> kTypeWordShift;
        if (type != 0 && !game::readable::kIsDm2) // (ZGE2 resolves the type at every call)
            return type;
        uint32_t chosen;
        if (part->boneCount != 0)
        {
            if (game::readable::kIsDm2 || (part->flags & kPartLitByNormals)) // (ZGE2 has no prelit skinned type)
                chosen = typeBits(RenderType::Skinned);
            else
                chosen = typeBits(RenderType::SkinnedPrelit);
        }
        else
        {
            // (ZGE2 keeps the sky flag in the upper word with the tree's flags, and tests no node pointer)
            if (game::readable::kIsDm2 ? (nodeTreeFlags(*node) & kNodeSky) != 0 : node && (node->flags & kNodeSky))
                chosen = typeBits(RenderType::Sky);
            else
                chosen = typeBits(RenderType::RigidPrelit);
        }
        const uint32_t resolved = typeWord & (0x03FF0000u | 0xFFFFu) | chosen;
        part->typeWord = resolved;
        return resolved;
    }
    READABLE_FUNCTION(resolveRenderType, Verified);

    namespace
    {
        ee::Ptr<uint32_t> lastWordOf(ee::Ptr<uint32_t> buffer, uint32_t bytes) { return (buffer.cast<uint8_t>() + bytes).cast<uint32_t>() - 1; }
    }

    // bufferUseCount: a packet buffer's use count (the word at the end of its
    // allocation, bufferFlags giving the size).
    ee::Ptr<uint32_t> bufferUseCount(ee::Ptr<ZgePart> part, ee::Ptr<uint32_t> buffer)
    {
        return lastWordOf(buffer, part->bufferFlags >> kBufferBytesShift);
    }
    READABLE_FUNCTION(bufferUseCount, Verified);

    // markBuilt: a buffer just built: one use (the count's address).
    ee::Ptr<uint32_t> markBuilt(ee::Ptr<ZgePart> part, ee::Ptr<uint32_t> buffer)
    {
        const ee::Ptr<uint32_t> uses = bufferUseCount(part, buffer);
        *uses = 1;
        return uses;
    }
    READABLE_FUNCTION(markBuilt, Verified);

    // retainPacketBuffer: a buffer shared with a clone: one more use.
    ee::Ptr<uint32_t> retainPacketBuffer(ee::Ptr<ZgePart> part, ee::Ptr<uint32_t> buffer)
    {
        const ee::Ptr<uint32_t> uses = bufferUseCount(part, buffer);
        *uses = *uses + 1;
        return uses;
    }
    READABLE_FUNCTION(retainPacketBuffer, Verified);

    // releasePacketBuffer: one use less; whether that was the last.
    int32_t releasePacketBuffer(ee::Ptr<ZgePart> part, ee::Ptr<uint32_t> buffer)
    {
        const ee::Ptr<uint32_t> uses = bufferUseCount(part, buffer);
        const int32_t left = static_cast<int32_t>(*uses) - 1;
        *uses = static_cast<uint32_t>(left);
        return left < 1 ? 1 : 0;
    }
    READABLE_FUNCTION(releasePacketBuffer, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t vif1PacketAlign(uint32_t packet, uint32_t bit, uint32_t pos) { return ee::tailCall<int32_t>(kVif1PacketAlign, packet, bit, pos); }
        }
        // The chain's REF of a material's GS block and its CALL of the strips.
        void addMaterialRef(ee::Ptr<sce::Vif1Packet> packet, ee::Ptr<Material> material, uint32_t qwords, uint32_t unpackCode)
        {
            sce::vif1PacketRef(packet, ee::Ptr<uint64_t>(materialLayer(material, 0).gifTag), qwords, vif::stcycl(4, 4), unpackCode, 0);
        }
        void addStripsCall(ee::Ptr<sce::Vif1Packet> packet, ee::Ptr<uint8_t> strips) { sce::vif1PacketCall(packet, strips, 0); }
    }

    // buildMaterialChain: the material table's chain for the part's buffer to
    // build, made once (32 bytes a material, the first word 0 until built):
    // for each material with strips in the part's packet, a REF of its GS
    // block (UNPACKed to its VU1 address) and a CALL of its strips; then
    // RET, aligned (the original's tail call). The chain's first word when
    // it was built already.
    int32_t buildMaterialChain(ee::Ptr<MaterialTable> table, ee::Ptr<ZgePart> part)
    {
        const uint32_t bufferToBuild = part->bufferFlags & kBufferToBuild;
        uint32_t &chainSlot = table->packetChains[bufferToBuild];
        const uint32_t count = table->count;
        if (chainSlot == 0)
        {
            chainSlot = allocateIn(count << 5 | 0x10u, 0, 1);
            *ee::Ptr<uint32_t>(chainSlot) = 0;
        }
        const ee::Ptr<int32_t> chain(chainSlot);
        if (*chain != 0)
            return *chain;
        ee::Ptr<PacketMaterial> entry = part->materialTables[bufferToBuild];
        const ee::Ptr<sce::Vif1Packet> packet(kChainPacket);
        vif1PacketInit(packet, chainSlot);
        vif1PacketReset(packet);
        for (uint32_t index = 0; index < count; ++index, ++entry)
        {
            if (entry->start)
            {
                const ee::Ptr<Material> material = table->materials[index];
                const uint32_t qwords = layerQwordCount(material, 0);
                addMaterialRef(packet, material, qwords, vif::unpack(vif::kUnpackV4_32, qwords, entry->gsBlockAddress | vif::kUnpackFromTops));
                addStripsCall(packet, entry->start);
            }
        }
        sce::vif1PacketRet(packet, 0);
        return tail::vif1PacketAlign(kChainPacket, 2, 0);
    }

    // buildNodeMaterialChain: ZGE2's buildMaterialChain, which takes the node
    // (its material table and its drawn part, Node::viewPart): the same chain,
    // made once, but with a REF and a CALL for every material of the table,
    // as ZGE2's packets start every one (no test for strips).
    int32_t buildNodeMaterialChain(ee::Ptr<Node> node)
    {
        const ee::Ptr<MaterialTable> table = node->materials;
        const ee::Ptr<ZgePart> part = nodeDrawnPart(*node);
        const uint32_t bufferToBuild = part->bufferFlags & kBufferToBuild;
        uint32_t &chainSlot = table->packetChains[bufferToBuild];
        const uint32_t count = table->count;
        if (chainSlot == 0)
        {
            chainSlot = allocateIn(count << 5 | 0x10u, 0, 1);
            *ee::Ptr<uint32_t>(chainSlot) = 0;
        }
        const ee::Ptr<int32_t> chain(chainSlot);
        if (*chain != 0)
            return *chain;
        ee::Ptr<PacketMaterial> entry = part->materialTables[bufferToBuild];
        const ee::Ptr<sce::Vif1Packet> packet(kChainPacket);
        vif1PacketInit(packet, chainSlot);
        vif1PacketReset(packet);
        for (uint32_t index = 0; index < count; ++index, ++entry)
        {
            const ee::Ptr<Material> material = table->materials[index];
            const uint32_t qwords = layerQwordCount(material, 0);
            addMaterialRef(packet, material, qwords, vif::unpack(vif::kUnpackV4_32, qwords, entry->gsBlockAddress | vif::kUnpackFromTops));
            addStripsCall(packet, entry->start);
        }
        sce::vif1PacketRet(packet, 0);
        return tail::vif1PacketAlign(kChainPacket, 2, 0);
    }
    READABLE_FUNCTION_WAITS_OWN(buildMaterialChain, Verified, dm2, buildNodeMaterialChain);

    // ---- a part's packets --------------------------------------------------

    // newNodeRenderState: a node's render state made (none on this platform).
    uint32_t newNodeRenderState() { return 1; }
    READABLE_FUNCTION(newNodeRenderState, Verified);

    void freeNodeLights(ee::Ptr<Node> node);

    // freeNodeRenderState: a node's render state freed: its own lights.
    void freeNodeRenderState(ee::Ptr<Node> node) { freeNodeLights(node); }
    READABLE_FUNCTION_WAITS(freeNodeRenderState, Verified);

    // clearRenderState: a new part's render state cleared.
    uint32_t clearRenderState(ee::Ptr<ZgePart> part)
    {
        // Its render state: its tables, its packets and the words after them.
        memoryFill(part->materialTables, 0, 24);
        return 1;
    }
    READABLE_FUNCTION(clearRenderState, Verified);

    // cloneRenderState: a clone's render state (clonePart): unless the clone
    // gets streams of its own (shareFlags 0xFC0), the source's packet
    // buffers and tables shared, one more use each, and its type word and
    // buffer flags copied (the original copies them field by field).
    uint32_t cloneRenderState(ee::Ptr<ZgePart> source, ee::Ptr<ZgePart> clone, uint32_t shareFlags)
    {
        if ((shareFlags & 0xFC0u) == 0)
        {
            clone->packets[0] = source->packets[0];
            clone->packets[1] = source->packets[1];
            clone->materialTables[0] = source->materialTables[0];
            clone->materialTables[1] = source->materialTables[1];
            const ee::Ptr<uint32_t> first = clone->packets[0];
            clone->typeWord = source->typeWord;
            clone->bufferFlags = source->bufferFlags;
            if (first)
                retainPacketBuffer(clone, first);
            const ee::Ptr<uint32_t> second = clone->packets[1];
            if (second)
                retainPacketBuffer(clone, second);
        }
        return 1;
    }
    READABLE_FUNCTION(cloneRenderState, Verified);

    // releasePartPackets: a part's packet buffers let go of (releasePart): each
    // freed with its last use.
    uint32_t releasePartPackets(ee::Ptr<ZgePart> part)
    {
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 keeps a buffer's packet and its material table as two
            // allocations, with no use count: both freed.
            for (ee::Ptr<uint32_t> &packet : part->packets)
                if (packet)
                {
                    freeIn(packet, 0);
                    packet = ee::Ptr<uint32_t>();
                }
            for (ee::Ptr<PacketMaterial> &table : part->materialTables)
                if (table)
                {
                    freeIn(table, 0);
                    table = ee::Ptr<PacketMaterial>();
                }
            return 1;
        }
        for (ee::Ptr<uint32_t> &buffer : part->packets)
        {
            if (buffer && releasePacketBuffer(part, buffer) != 0)
            {
                freeIn(buffer, 0);
                buffer = ee::Ptr<uint32_t>();
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(releasePartPackets, Verified);

    uint32_t buildPartPackets(ee::Ptr<ZgePart> part);

    // buildChainPartPackets: a part of a chain's packets built (buildPartChainPackets).
    uint32_t buildChainPartPackets(ee::Ptr<ZgePart> part)
    {
        buildPartPackets(part);
        return 1;
    }
    READABLE_FUNCTION_WAITS(buildChainPartPackets, Verified);

    // setPartRenderClass: the part's render type from its TGPH class (a skinned
    // class without normals is the prelit skinned type).
    uint32_t setPartRenderClass(ee::Ptr<ZgePart> part, uint32_t renderClass)
    {
        uint32_t type = (*g_renderClassTypes)[renderClass];
        if (type == static_cast<uint32_t>(RenderType::Skinned))
            type = (part->flags & kPartLitByNormals) == 0 ? static_cast<uint32_t>(RenderType::SkinnedPrelit) : type;
        const uint32_t typeWord = part->typeWord & 0x03FFFFFFu | type << kTypeWordShift;
        part->typeWord = typeWord;
        return typeWord;
    }
    READABLE_FUNCTION(setPartRenderClass, Verified);

    // renderHookNone (no caller in this build): a render hook that does
    // nothing here.
    uint32_t renderHookNone() { return 0; }
    READABLE_FUNCTION(renderHookNone, Verified);

    // renderIgnoresStream: whether the renderer ignores a stream of the part
    // (partIgnoresStream): every class but terrain ignores normals.
    int32_t renderIgnoresStream(ee::Ptr<ZgePart> part, StreamKind kind)
    {
        if (kind == StreamKind::Normals)
            return (part->typeWord >> kTypeWordShift ^ static_cast<uint32_t>(RenderType::Terrain)) != 0 ? 1 : 0;
        return 0;
    }
    READABLE_FUNCTION(renderIgnoresStream, Verified);

    // clearMaterialChains: a new material table's chains cleared (newMaterialTable).
    uint32_t clearMaterialChains(ee::Ptr<MaterialTable> table)
    {
        for (uint32_t index = 0;;)
        {
            table->packetChains[index] = 0;
            ++index;
            if (!(index < 2))
                return 1;
        }
    }
    READABLE_FUNCTION(clearMaterialChains, Verified);

    // freeMaterialChains: a material table's chains freed.
    uint32_t freeMaterialChains(ee::Ptr<MaterialTable> table)
    {
        for (uint32_t &chain : table->packetChains)
        {
            if (chain != 0)
            {
                freeIn(chain, 0);
                chain = 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeMaterialChains, Verified);

    // buildPartPackets: a new part's packets built: its render type, its whole
    // packet, its first material set's chain; its strip groups let go of
    // unless it keeps them (flags 0x2000).
    uint32_t buildPartPackets(ee::Ptr<ZgePart> part)
    {
        const ee::Ptr<MaterialTable> materials = meshMaterials(part, 0);
        if ((part->flags & kPartNew) != 0 && part->materialSets)
        {
            resolveRenderType(0, part);
            buildPartPacket(part, 1, kAllStreams);
            buildMaterialChain(materials, part);
            if constexpr (!game::readable::kIsBmx) // (ZGE4 keeps the strip groups)
                if ((part->flags & 0x2000u) == 0)
                    releaseStream(part, StreamKind::Groups);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(buildPartPackets, Verified);

    // buildAllPartPackets: every part's packets built (forEachPart, the original's
    // tail call).
    void buildAllPartPackets() { game::tail(zge::forEachPart, kBuildPartPackets, 0); }
    READABLE_FUNCTION_WAITS(buildAllPartPackets, Verified);

    // ---- a node's own lights ------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        void copyMatrix(ee::Address out, ee::Address in) { ee::game(kSceVu0CopyMatrix, out, in); }
        namespace tail
        {
        void sceVu0CopyMatrix(ee::Address out, ee::Address in) { ee::tailCall(kSceVu0CopyMatrix, out, in); }
        }
    }

    // updateNodeLights: a node lit by its own lights gets its light block at
    // the first draw: the renderer's default light matrices copied
    // (sceVu0CopyMatrix twice, the second the original's tail call); ZGE4's
    // first draw bucket has its own pair.
    // (The original returns the block, or 0 for a node without its own
    // lights; its one caller, drawNodeInView, does not read it.)
    void updateNodeLights(ee::Ptr<Node> node)
    {
        if ((node->flags & kNodeOwnLights) == 0 || node->lights)
            return;
        const ee::Ptr<LightBlock> lights(heapAllocate(sizeof(LightBlock)));
        node->lights = lights;
        if constexpr (game::readable::kIsBmx) // (ZGE4: the first draw bucket's nodes have default lights of their own)
        {
            if ((node->flags & kNodeSecondBucket) == 0)
            {
                copyMatrix(lights, &firstBucketNormalLights(*g_renderer));
                tail::sceVu0CopyMatrix(lights->colours, &firstBucketLightColours(*g_renderer));
                return;
            }
        }
        copyMatrix(lights, &g_renderer->normalLightMatrix);
        tail::sceVu0CopyMatrix(lights->colours, &g_renderer->lightColourMatrix);
    }
    READABLE_FUNCTION_WAITS(updateNodeLights, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Ptr<LightBlock> block) { ee::tailCall(kHeapFree, block); }
        }
    }

    // freeNodeLights: a node's own lights freed (the original's tail call).
    void freeNodeLights(ee::Ptr<Node> node)
    {
        if (node->lights)
        {
            tail::heapFree(node->lights);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(freeNodeLights, Verified);

    // setNodeLight (no caller in this build): one of a node's own lights
    // set: its direction (normalised, negated, as the block's column) and
    // its colour (alpha 1).
    void setNodeLight(ee::Ptr<Node> node, ee::Ptr<Vec4> direction, ee::Ptr<Vec3> colour, uint32_t index)
    {
        ee::Local<Vec4> towardLight;
        const ee::Ptr<LightBlock> lights = node->lights;
        if (lights)
        {
            towardLight = unit(*direction);
            const ee::Ptr<ee::Float> column = &lights->directions[0].x + index;
            column[0] = -towardLight->x;
            column[4] = -towardLight->y;
            column[8] = -towardLight->z;
            column[12] = ee::Float(0.0f); // (row 3, stored as the integer 0)
            lights->colours[index].x = colour->x;
            lights->colours[index].y = colour->y;
            lights->colours[index].z = colour->z;
            lights->colours[index].w = ee::Float(1.0f);
        }
    }
    READABLE_FUNCTION(setNodeLight, Draft);

    // setNodeAmbient, nodeAmbient: a node's ambient colour (its light
    // block's last colour row) set or read; the block (0: none).
    ee::Ptr<LightBlock> setNodeAmbient(ee::Ptr<Node> node, ee::Ptr<Vec3> colour)
    {
        const ee::Ptr<LightBlock> lights = node->lights;
        if (lights)
        {
            lights->colours[3].x = colour->x;
            lights->colours[3].y = colour->y;
            lights->colours[3].z = colour->z;
        }
        return lights;
    }
    READABLE_FUNCTION(setNodeAmbient, Verified);

    void nodeAmbient(ee::Ptr<Node> node, ee::Ptr<Vec3> colour)
    {
        const ee::Ptr<LightBlock> lights = node->lights;
        if (lights)
        {
            colour->x = lights->colours[3].x;
            colour->y = lights->colours[3].y;
            colour->z = lights->colours[3].z;
        }
    }
    READABLE_FUNCTION(nodeAmbient, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t startProfileBar(uint32_t appProfileBar) { return zge::startProfileBar(appProfileBar); }
        uint32_t stopProfileBar(uint32_t appProfileBar2) { return zge::stopProfileBar(appProfileBar2); }
    }

    namespace
    {
        namespace routed
        {
        }
    }

    uint32_t releaseOrphanPart(ee::Ptr<ZgePart> zgePart)
    {
        if (static_cast<int16_t>(zgePart->refCount) == 0)
        {
            zge::releasePart(zgePart);
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseOrphanPart, Verified);
}
