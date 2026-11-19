// A part's VIF packet, built on its first draw and rebuilt when streams
// change (buildPartPacket), with a scratch buffer that fits the part.
#include "zge/render/packet.h"

#include "zge/render/draw.h"
#include "zge/render/ps2/packet_size.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/math/vec.h"
#include "sce/packet_calls.h"
#include "zge/native/host.h"
#include "zge/platform/platform.h"
#include "zge/render/part.h"
#include "zge/text/cstring.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include "sym/zge/render/ps2/part_packet.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kFlushCache = sym::flushCache;  // (0): FlushCache, the packet is read by DMA
        // The class builders: (part, packet, material table, streams) -> bytes written.
        constexpr uint32_t kBuildRigid = sym::buildRigid;   // types 4, 0x18 (ZGE2: 4, 0x14)
        constexpr uint32_t kBuildTerrain = sym::buildTerrain; // type 0x20
        constexpr uint32_t kBuildSkinned = sym::buildSkinned; // types 8, 0xC

        constexpr uint32_t kAllStreams = 0x7FFu;
        constexpr uint16_t kRebuildBits = 0x07FFu; // flags kept: & 0xF800

        // A class builder: (part, packet, material table, streams to
        // rebuild) -> the packet's bytes.
        using PacketBuilder = ee::FunctionPtr<uint32_t(ee::Ptr<ZgePart>, ee::Ptr<uint8_t>, ee::Ptr<PacketMaterial>, uint32_t)>;

        // The class builder of a render type (buildPartPacket's jump table at
        // types 4 to 32), none for the others.
        PacketBuilder builderOf(uint32_t renderType)
        {
            switch (renderType)
            {
            case static_cast<uint32_t>(RenderType::RigidPrelit):
            case static_cast<uint32_t>(RenderType::Sky):
                return kBuildRigid;
            case 0x08:
            case 0x0C:
                return kBuildSkinned;
            case 0x20:
                return kBuildTerrain;
            default:
                return 0u;
            }
        }

        // The size model (packet_size.h) against what the builders wrote,
        // on every build in the game: a difference is logged (the first 16).
        // PS2X_TRACE_PACKETS=1 also logs each first build, and now and then
        // how many calls took each path.
        struct PacketStats
        {
            uint64_t builds = 0, first = 0, rebuilt = 0, differ = 0, larger = 0;
            uint32_t largest = 0;
            // Every call, by path.
            uint64_t calls = 0, builtAlready = 0, inPlace = 0, newBuffer = 0, noBuilder = 0;
        };
        PacketStats g_stats;

        bool tracePackets()
        {
            static const bool trace = []
            {
                const char *v = std::getenv("PS2X_TRACE_PACKETS");
                return v && *v && *v != '0';
            }();
            return trace;
        }

        enum class PacketPath
        {
            First,
            BuiltAlready,
            InPlace,
            NewBuffer,
        };

        void notePath(PacketPath path, bool hasBuilder)
        {
            if (!native::hostExtrasOn() || !tracePackets())
                return;
            PacketStats &s = g_stats;
            ++s.calls;
            s.builtAlready += path == PacketPath::BuiltAlready;
            s.inPlace += path == PacketPath::InPlace;
            s.newBuffer += path == PacketPath::NewBuffer;
            s.noBuilder += path != PacketPath::BuiltAlready && !hasBuilder;
            if ((s.calls & (s.calls - 1)) == 0 || s.calls % 4096u == 0 || path != PacketPath::First)
                std::cout << "[packet] " << s.calls << " calls: " << s.calls - s.builtAlready - s.inPlace - s.newBuffer
                          << " first builds, " << s.builtAlready << " built already, " << s.inPlace
                          << " rebuilt in place, " << s.newBuffer << " rebuilt in a new buffer; " << s.noBuilder
                          << " of a type with no builder" << std::endl;
        }

        void notePacket(const ZgePart &part, bool whole, uint32_t bytes, uint32_t expected)
        {
            const bool trace = tracePackets();
            PacketStats &s = g_stats;
            ++s.builds;
            ++(whole ? s.first : s.rebuilt);
            if (bytes > kPacketScratchBytes && whole)
                ++s.larger;
            if (bytes > s.largest)
                s.largest = bytes;
            const uint32_t type = part.typeWord >> kTypeWordShift;
            if (expected != bytes && s.differ++ < 16u)
                std::cout << "[packet] part id 0x" << std::hex << part.id << " type 0x" << type << std::dec << ": "
                          << bytes << " bytes written, " << expected << " expected" << std::endl;
            if (!trace)
                return;
            if (whole)
                std::cout << "[packet] part id 0x" << std::hex << part.id << " type 0x" << type << std::dec << ": "
                          << bytes << " bytes, " << part.groupCount << " groups, scratch "
                          << (bytes > kPacketScratchBytes ? "sized to the part" : "160 KB") << std::endl;
            if (s.builds % 64u == 0u)
                std::cout << "[packet] " << s.builds << " packets written: " << s.first << " first, " << s.rebuilt
                          << " rebuilt; " << s.larger << " larger than 160 KB, the largest " << s.largest
                          << " bytes; " << s.differ << " not the size expected" << std::endl;
        }
    }

    uint32_t packetBytes(const ZgePart &part, uint32_t startAlignment)
    {
        PacketSize size(packetBytesPerEntry(part.typeWord >> kTypeWordShift), startAlignment);
        if (!builderOf(part.typeWord >> kTypeWordShift))
            return 0u;
        const uint8_t *group = streamRecords<uint8_t>(part.groups).get(); // the stream's data
        for (uint32_t g = 0; g < part.groupCount; ++g)
        {
            uint16_t count;
            std::memcpy(&count, group + 2, sizeof(count));
            size.group(group[0] & 3u, group[1], count);
            group += StripGroup::strideFor(count);
        }
        return size.bytes();
    }

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
    }

    namespace
    {
        // The material table's place in a packet buffer: right after the
        // packet's bytes. A buffer as the part's packet field holds it.
        ee::Ptr<PacketMaterial> tableAfter(ee::Ptr<uint8_t> packet, uint32_t bytes) { return (packet + bytes).cast<PacketMaterial>(); }
        ee::Ptr<uint32_t> asPacketField(ee::Ptr<uint8_t> packet) { return packet.cast<uint32_t>(); }
        ee::Ptr<uint8_t> packetOf(ee::Ptr<uint32_t> field) { return field.cast<uint8_t>(); }
        // Where a packet starts within its quadword.
        uint32_t startAlignment(ee::Ptr<uint8_t> packet) { return static_cast<uint32_t>(bytesFrom(ee::Ptr<uint8_t>(&aligned(packet)), packet)); }
    }

    int32_t buildPartPacket(ee::Ptr<ZgePart> partAddress, int32_t whole, uint32_t streams)
    {
        ZgePart &part = *partAddress;
        const uint32_t buffer = part.bufferFlags & kBufferToBuild;
        ee::Ptr<PacketMaterial> &tableField = part.materialTables[buffer];
        ee::Ptr<uint32_t> &packetField = part.packets[buffer];

        // The material table, made here and placed after the packet.
        const uint32_t tableBytes = part.materialCount * static_cast<uint32_t>(sizeof(PacketMaterial));
        const ee::Ptr<PacketMaterial> table(allocateMemory(tableBytes));
        memoryFill(table, 0, tableBytes);

        ee::Ptr<uint8_t> packet;
        if (whole)
        {
            if (packetField)
            {
                // Built already.
                notePath(PacketPath::BuiltAlready, false);
                freeMemory(table);
                part.flags &= ~kRebuildBits;
                return 1;
            }
            // The original's 160 KB, or the part's packet if that is more
            // (the class builders write with no bound). Measured from the
            // groups in RAM, which the original never reads itself: not
            // under ps2xFnCheck, where only the original's reads are there.
            uint32_t scratchBytes = kPacketScratchBytes;
            if (native::hostExtrasOn())
            {
                const uint32_t needed = packetBytes(part, kPacketWorstStartAlignment);
                if (needed > scratchBytes)
                    scratchBytes = needed;
            }
            notePath(PacketPath::First, static_cast<bool>(builderOf(part.typeWord >> kTypeWordShift)));
            packet = ee::Ptr<uint8_t>(allocateMemory(scratchBytes));
        }
        else
        {
            packet = packetOf(packetField);
            notePath(packet ? PacketPath::InPlace : PacketPath::NewBuffer, static_cast<bool>(builderOf(part.typeWord >> kTypeWordShift)));
            if (!packet)
            {
                packet = ee::Ptr<uint8_t>(allocateIn(part.bufferFlags >> kBufferBytesShift, 0, 1));
                streams = kAllStreams;
            }
        }

        // The packet, by the render type's builder (a type without one
        // writes nothing).
        uint32_t bytes = 0;
        if (!whole || !packetField)
        {
            if (const PacketBuilder build = builderOf(part.typeWord >> kTypeWordShift))
            {
                bytes = build(partAddress, packet, table, streams);
                if (native::hostExtrasOn() && part.groups)
                    notePacket(part, whole != 0, bytes, packetBytes(part, startAlignment(packet)));
            }
        }

        if (whole)
        {
            const uint32_t allocationBytes = bytes + 4u + tableBytes; // the packet, a word, the table
            if (!packetField)
            {
                // An allocation of the packet's size; the table's starts
                // moved with the packet.
                const ee::Ptr<uint8_t> scratch = packet;
                packet = ee::Ptr<uint8_t>(allocateIn(allocationBytes, 0, 1));
                memoryCopy(packet, scratch, bytes);
                const uint32_t materials = part.materialCount;
                for (uint32_t i = materials; i-- > 0;)
                {
                    if (table[i].start)
                        table[i].start = packet + bytesFrom(scratch, table[i].start);
                }
                freeMemory(scratch);

                const ee::Ptr<PacketMaterial> tableCopy = tableAfter(packet, bytes);
                memoryCopy(tableCopy, table, tableBytes);
                tableField = tableCopy;
                part.bufferFlags = (part.bufferFlags & (kBufferToBuild | kBufferDrawn)) |
                                   (allocationBytes << kBufferBytesShift);
                packetField = asPacketField(packet);
                markBuilt(partAddress, asPacketField(packet));
            }
        }
        else
        {
            if (!packetField)
            {
                const ee::Ptr<PacketMaterial> tableCopy = tableAfter(packet, bytes);
                memoryCopy(tableCopy, table, tableBytes);
                tableField = tableCopy;
                packetField = asPacketField(packet);
                markBuilt(partAddress, asPacketField(packet));
            }
        }

        freeMemory(table);
        part.flags &= ~kRebuildBits;
        flushCache(0);
        return 1;
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t flushCache(uint32_t mode) { return ee::tailCall<int32_t>(kFlushCache, mode); }
        }
    }

    // buildPartPacketAndTable: ZGE2's buildPartPacket, which is handed the
    // node too (and reads nothing of it). The buffer's material table is an
    // allocation of its own (made again for a whole build, else when
    // missing), not copied after the packet; a whole build is always made
    // again, in the original's 128 KB scratch, then copied into an
    // allocation of the packet's size with the table's starts moved with
    // it; a rebuild is written in the buffer's packet (made, with every
    // stream, when missing). ZGE2's builders: buildSkinned for skinned parts,
    // buildRigid for rigid and sky ones. Then the rebuild bits cleared and
    // the data cache written back (the original's tail call).
    int32_t buildPartPacketAndTable(ee::Ptr<Node>, ee::Ptr<ZgePart> partAddress, int32_t whole, uint32_t streams)
    {
        constexpr uint32_t kScratchBytes = 0x20000u;
        ZgePart &part = *partAddress;
        const uint32_t buffer = part.bufferFlags & kBufferToBuild;
        ee::Ptr<PacketMaterial> &tableField = part.materialTables[buffer];
        ee::Ptr<uint32_t> &packetField = part.packets[buffer];
        const uint32_t tableBytes = part.materialCount * static_cast<uint32_t>(sizeof(PacketMaterial));
        ee::Ptr<uint8_t> packet;
        if (whole)
        {
            tableField = ee::Ptr<PacketMaterial>(allocateIn(tableBytes, 0, 1));
            packet = ee::Ptr<uint8_t>(allocateMemory(kScratchBytes));
        }
        else
        {
            if (!tableField)
                tableField = ee::Ptr<PacketMaterial>(allocateIn(tableBytes, 0, 1));
            if (!packetField)
            {
                packetField = asPacketField(ee::Ptr<uint8_t>(allocateIn(part.bufferFlags >> kBufferBytesShift, 0, 1)));
                streams = kAllStreams;
            }
            packet = packetOf(packetField);
        }

        const ee::Ptr<PacketMaterial> table = tableField;
        const RenderType type = renderTypeOf(part);
        PacketBuilder build = 0u;
        if (type == RenderType::Skinned)
            build = kBuildSkinned;
        else if (type == RenderType::RigidPrelit || type == RenderType::Sky)
            build = kBuildRigid;
        const uint32_t bytes = build ? build(partAddress, packet, table, streams) : 0u;

        if (whole)
        {
            const ee::Ptr<uint8_t> scratch = packet;
            packet = ee::Ptr<uint8_t>(allocateIn(bytes, 0, 1));
            memoryCopy(packet, scratch, bytes);
            for (uint32_t i = part.materialCount; i-- > 0;)
                table[i].start = packet + bytesFrom(scratch, table[i].start);
            freeMemory(scratch);
            part.bufferFlags = (part.bufferFlags & (kBufferToBuild | kBufferDrawn)) | (bytes << kBufferBytesShift);
            packetField = asPacketField(packet);
        }
        part.flags &= ~kRebuildBits;
        return tail::flushCache(0); // the original's tail call
    }
    READABLE_FUNCTION_OWN(buildPartPacket, Verified, dm2, buildPartPacketAndTable);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void vif1PacketAddDword(ee::Ptr<sce::Vif1Packet> packet, uint64_t value) { ee::tailCall(sym::vif1PacketAddDword, packet, value); }
        }
    }

    // addGsRegister (kAddGsRegister): a GS register's A+D pair in an open
    // GIF tag's data: the value, then the register's number (zero-extended).
    void addGsRegister(sce::Vif1Packet *packet, uint32_t gsRegister, uint64_t value)
    {
        sce::vif1PacketAddDword(packet, value);
        tail::vif1PacketAddDword(packet, static_cast<uint64_t>(gsRegister));
    }
    READABLE_FUNCTION(addGsRegister, Verified);

    // addGsRegister2: the same code again (the motion blur's copy, drawBlur).
    void addGsRegister2(sce::Vif1Packet *packet, uint32_t gsRegister, uint64_t value)
    {
        sce::vif1PacketAddDword(packet, value);
        tail::vif1PacketAddDword(packet, static_cast<uint64_t>(gsRegister));
    }
    READABLE_FUNCTION(addGsRegister2, Verified);
}
