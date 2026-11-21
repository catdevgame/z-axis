// The primitive API's packet builders, its strict encoder (block 7, from projectedDepth on; BMX
// XXX has them word for word, DM2 an older form): the effects, particles,
// fog planes, zgui and the text hand in batches of triangle strips, which
// go into one of two VIF1 packets for VU1's primitive microprogram, the
// world one (g_worldPrimitives: vertices transformed on VU1, a matrix applied
// first on VU0 if given) and the 2D one (g_screenPrimitives: screen positions).
// zge/render/primitives.cpp has the entry that picks
// the packet. The lines of either packet are built here too: line strips of
// two vertices, or of up to 16 with every second one starting a new line (ADC).
//
// Each strip is one UNPACK: a GIF tag (PRE: a triangle strip with the
// packet's PRIM bits, regs ST or UV, RGBAQ, XYZ2) and three quadwords a
// vertex, staged in a fixed block of RAM and copied in. The world packet
// splits strips at 60 vertices (VU1's buffer), repeating the two before
// the cut; after each strip an MSCAL (the first since the program was
// uploaded) or an MSCNT runs the microprogram. The 2D packet runs it once
// after the whole batch.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/mirror.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/render/display.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/part.h"
#include "zge/render/renderer.h"
#include "zge/render/vif_codes.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/render/primitives.h"
#include "zge/render/material.h"
#include "zge/scene/camera.h"
#include "zge/container/cursor_array.h"
#include "zge/text/cstring.h"
#include "zge/container/pointer_array.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/ps2/primitive_packets.cpp.h"

namespace zge
{
    // A packet and the state the builders keep with it (0x50 bytes).
    struct PrimitivePacket
    {
        sce::Vif1Packet vif;                 // +0x00 (+0x04: the buffer's start)
        uint8_t _18[0x08];
        uint32_t buffer;                     // +0x20 the DMA buffer being filled (uncached), 0 before the first
        uint32_t bufferBytes;                // +0x24
        ee::Ptr<PointerArray> buffers[2];    // +0x28 the DMA buffers, a set per g_bufferSet
        // +0x30 the bound material's two-sided flag (kMaterialFlagTwoSided, bindTexture); none of
        // the builders reads it.
        uint32_t twoSided;
        uint32_t colourShift;                // +0x34 a vertex's RGBA bytes shifted right by it (1: textured, 0x80 is 1.0)
        uint32_t texture;                    // +0x38 the texture bound
        uint32_t _3c;
        uint64_t primBits;                   // +0x40 PRIM's bits above the primitive type (shading, texture, blend ...)
        uint32_t startProgram;               // +0x48 the next run is an MSCAL (the program's start), not an MSCNT
        uint32_t _4c;
    };
    static_assert(sizeof(PrimitivePacket) == 0x50);
    static_assert(offsetof(PrimitivePacket, buffer) == 0x20 && offsetof(PrimitivePacket, buffers) == 0x28 &&
                  offsetof(PrimitivePacket, twoSided) == 0x30 && offsetof(PrimitivePacket, colourShift) == 0x34 && offsetof(PrimitivePacket, texture) == 0x38 &&
                  offsetof(PrimitivePacket, primBits) == 0x40 && offsetof(PrimitivePacket, startProgram) == 0x48);

    namespace
    {
        // A position's x and y as the two words a packet takes them in.
        uint64_t xyWords(ee::Float x, ee::Float y) { return x.bits() | static_cast<uint64_t>(y.bits()) << 32; }
        uint64_t xyWords(const Vec4 &v) { return xyWords(v.x, v.y); }
        uint64_t xyWords(const PrimitiveVertex &v) { return xyWords(v.x, v.y); }
        // A vertex's u and v, the same way.
        uint64_t texelWords(const PrimitiveVertex &v) { return v.u.bits() | static_cast<uint64_t>(v.v.bits()) << 32; }
    }

    // Where a batch's loop has got to, as drawWorldPrimitives leaves it in its
    // frame. On a batch of one that is { the strip past it, 0, -1 }: when
    // drawSparks follows drawParticles, this is the vector its first streak
    // measures, and the -1 makes it past any length, so that streak has none.
    struct StripLoopPlace
    {
        ee::Ptr<PrimitiveStrip> nextStrip; // +0x00
        int32_t verticesAfter;             // +0x04 this piece's strip, past this piece
        int32_t stripsLeft;                // +0x08
    };
    static_assert(sizeof(StripLoopPlace) == 0x0C);

    // What drawScreenPrimitives leaves in its frame through a batch: the
    // depth-from-vertex bit of its flags, and where its strip loop has got to
    // (on the last strip, { 0 or 4, -1, the strip past the batch }).
    struct ScreenStripLoopPlace
    {
        uint32_t depthFromVertex;          // +0x00 the flags' kPrimitiveDepthFromVertex bit, as it is
        int32_t stripsLeft;                // +0x04 past this strip
        ee::Ptr<PrimitiveStrip> nextStrip; // +0x08
    };
    static_assert(sizeof(ScreenStripLoopPlace) == 0x0C);

    namespace
    {
        inline ee::Global<PrimitivePacket, sym::g_worldPrimitives> g_worldPrimitives;
        inline ee::Global<PrimitivePacket, sym::g_screenPrimitives> g_screenPrimitives;
        // The matrix of the last batch (kPrimitiveSameMatrix), 0 for none.
        inline ee::Global<ee::Ptr<Matrix>, sym::g_lastPrimitiveMatrix> g_lastPrimitiveMatrix;
        // Which of the packets' buffer sets this frame fills [inferred: one per frame in flight].
        inline ee::Global<uint32_t, sym::g_bufferSet> g_bufferSet;
        // The primitive renderer's words: the default materials, this frame's list of bound materials, the frame.
        inline ee::Global<PrimitiveWords, sym::g_primitiveRendererReady> g_primitiveWords;
        // A view-relative batch's matrix: the view's times the batch's.
        constexpr ee::Ptr<Matrix> kComposedMatrix(sym::composedMatrix);
        // The view (world to camera) matrix; the 2D depth's projection.
        inline ee::Global<ee::Ptr<Matrix>, sym::g_viewMatrix2> g_viewMatrix;
        inline ee::Global<ee::Ptr<Matrix>, sym::g_screenProjection> g_screenProjection;
        // The default textures, two a slot: PrimitiveWords::defaultMaterials (ZGE2's a word further on).
        constexpr uint32_t kDefaultTextures = sym::g_primitiveRendererReady;
        constexpr uint32_t kFirstDefaultTexture = offsetof(PrimitiveWords, defaultMaterials) / 4;
        // The 2D packet's depth: a batch's own while it is built, else this.
        inline ee::Global<ee::Float, sym::g_defaultScreenDepth> g_defaultScreenDepth;

        // The staged vertex, three quadwords each packet.
        struct WorldVertexQwords
        {
            int32_t s, t;      // +0x00 u and v times 4096, truncated (written as float words)
            ee::Float q;       // +0x08 1 (initPrimitiveRenderer): the GIF's Q (ST packed is S, T, Q)
            uint32_t flags;    // +0x0C as a part's strip entries (part.h): kStripNoTriangle (the strip's first two), kStripFlipped (every other vertex)
            int32_t rgba[4];   // +0x10
            uint64_t xy;       // +0x20 bit 0 of x: set from the third vertex on
            ee::Float z;       // +0x28
            ee::Float w;       // +0x2C 1 (initPrimitiveRenderer): a point
        };
        struct ScreenVertexQwords
        {
            uint64_t uv;       // +0x00 the vertex's eight bytes
            ee::Float q;       // +0x08 1, as the world vertex's
            uint32_t flags;    // +0x0C 0, where the world vertex has its flags
            int32_t rgba[4];   // +0x10
            uint64_t xy;       // +0x20
            ee::Float depth;   // +0x28 the batch's (g_screenDepth)
            ee::Float w;       // +0x2C 1
        };
        // A line's two vertices in the world packet, where a strip's one goes.
        struct WorldLineQwords
        {
            uint64_t uv;       // +0x00 the vertex's eight bytes
            ee::Float q;       // +0x08 1, as the world vertex's
            uint32_t flags;    // +0x0C kStripNoTriangle: the first (no line ends there)
            int32_t rgba[4];   // +0x10
            uint64_t xy;       // +0x20 bit 0 of x: the second
            ee::Float z;       // +0x28
            ee::Float w;       // +0x2C 1
        };
        static_assert(sizeof(WorldLineQwords) == 0x30);
        inline ee::Global<WorldLineQwords[2], sym::g_worldLine> g_worldLine;
        inline ee::Global<WorldVertexQwords, sym::g_worldLine> g_worldVertex;
        inline ee::Global<ScreenVertexQwords, sym::g_screenVertex> g_screenVertex;
        static_assert(sizeof(WorldVertexQwords) == 0x30 && sizeof(ScreenVertexQwords) == 0x30);

        constexpr uint32_t kPrimitiveStateHeld = sym::g_primitiveRendererReady;  // the primitive state, held while a strip's vertices go out
        constexpr uint32_t kCloseUnpack = sym::vif1PacketCloseUnpack;   // sceVif1PkCloseUpkCode, by address: a game override hooks it
        constexpr uint32_t kVif1PacketCnt = sym::vif1PacketCnt; // sceVif1PkCnt (the original's tail call)
        constexpr uint32_t kAddCallCode = sym::vif1PacketAddCallCode;   // vif1PacketAddCallCode (its result is the builder's)
        constexpr uint32_t kRunProgram = vif::mscal(2);
        constexpr uint32_t kContinueProgram = vif::mscnt();

        // Where a strip's UNPACKs go: VU1 8 (its GIF tag) and 9, TOPS-relative.
        constexpr uint32_t kStripHeader = vif::kUnpackFromTops | 8u;
        constexpr uint32_t kStripVertices = vif::kUnpackFromTops | 9u;
        constexpr int32_t kMaxStrip = 60;                // vertices an UNPACK holds

        // The strip's GIF tag: NLOOP vertices, EOP, PRE with PRIM (a triangle
        // strip, 4, with the packet's bits), three registers: ST (2) or UV,
        // RGBAQ (1), XYZ2 (5).
        ee::Qword gifTag(uint32_t vertices, uint32_t primitive, uint64_t primBits)
        {
            const gs::GifTag tag{.loops = vertices, .endOfPacket = 1, .setsPrim = 1, .prim = primBits | primitive, .registers = 3};
            return ee::Qword{tag.bits(), gs::registerList({gs::kSt, gs::kRgbaq, gs::kXyz2})};
        }
        constexpr uint32_t kPrimLineStrip = gs::kLineStrip, kPrimTriangleStrip = gs::kTriangleStrip, kPrimTriangleFan = gs::kTriangleFan;
        ee::Qword stripGifTag(uint32_t vertices, uint64_t primBits) { return gifTag(vertices, kPrimTriangleStrip, primBits); }

        // World lines clipped (clipLine): the view's six planes, and where
        // the clipped copy of a line's vertices goes (world space).
        inline ee::Global<uint32_t, sym::g_viewPlanes> g_viewPlanes;
        inline ee::Global<PrimitiveVertex[2], sym::g_clippedLine> g_clippedLine;
        constexpr uint32_t kViewPlanesLast = 0x60, kViewPlaneBytes = 0x10;
        constexpr int32_t kViewPlanes = 6;

        // The textures' slots setPrimitiveTexture takes.
        constexpr uint32_t kLineSlotWorld = 4, kLineSlotScreen = 6;
        constexpr uint32_t kClipLine = sym::clipLine;
        constexpr uint32_t kRunLineProgram = vif::mscal(0x14), kContinueLineProgram = vif::mscal(0x16);
        // The list of lines' buffer check wants this much room.
        constexpr uint32_t kBufferMargin = 0x200, kListBufferMargin = 0x400;
        constexpr uint32_t kMaxListVertices = 16;

        void copyUvAndColour(PrimitiveVertex &to, const PrimitiveVertex &from)
        {
            to.u = from.u;
            to.v = from.v;
            std::memcpy(to.rgba, from.rgba, sizeof(to.rgba));
        }
        // `end` moved to t of the way from `from` to where it is.
        void moveAlongLine(Vec4 &end, const Vec4 &from, ee::Float t)
        {
            end.x = from.x + (end.x - from.x) * t;
            end.y = from.y + (end.y - from.y) * t;
            end.z = from.z + (end.z - from.z) * t;
        }
        ee::Float &coordinate(uint32_t vertex, uint32_t axis) { return *ee::at<ee::Float>(vertex + axis * 4); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        // These two by address, not as game calls: their tail calls then return through
        // the scheduler once when the binding unwinds, as the original's (a readable
        // callee called directly has its tail call made as a call, which returns twice).
        int32_t runScreenProgram(ee::Ptr<PrimitivePacket> packet, uint32_t code) { return ee::game<int32_t>(kAddCallCode, &packet->vif, code); }
        void runVertexProgram(ee::Ptr<PrimitivePacket> packet, uint32_t code) { sce::vif1PacketAddCallCode(&packet->vif, code); }
        void closeVertexUnpack(ee::Ptr<PrimitivePacket> packet) { ee::game(kCloseUnpack, &packet->vif); }
        // A vertex record's quadwords appended to the packet.
        template <typename Vertex>
        void addVertex(ee::Ptr<PrimitivePacket> packet, const Vertex &vertex)
        {
            static_assert(sizeof(Vertex) % sizeof(ee::Qword) == 0);
            sce::vif1PacketAddQwords(&packet->vif, reinterpret_cast<const uint8_t *>(&vertex), sizeof(Vertex) / sizeof(ee::Qword));
        }
        // The bytes written to the packet's buffer so far.
        uint32_t bytesUsed(const PrimitivePacket &packet) { return static_cast<uint32_t>(bytesFrom(packet.vif.bufferStart, packet.vif.current)); }
    }

    // The DMA tag setPrimitiveTexture has bindTexture open after the material: "cnt" (tag ID 1, bits 28-30),
    // the strips' data following it.
    constexpr uint32_t kCntDmaTag = 1u << 28;
    void bindTexture(ee::Ptr<PrimitivePacket> packet, uint32_t texture, uint32_t dmaTag);

    // setPrimitiveTexture: the batch's texture bound (0: the slot's default; the
    // colours then not halved), unless it is bound already.
    void setPrimitiveTexture(ee::Ptr<PrimitivePacket> packet, uint32_t texture, uint32_t slot, uint32_t flags)
    {
        if (texture == 0)
        {
            const uint32_t index = (flags & kPrimitiveSecondSlot) ? slot + 1 : slot;
            texture = ee::Ptr<uint32_t>(kDefaultTextures)[index + kFirstDefaultTexture];
            packet->colourShift = 0;
        }
        else
            packet->colourShift = (flags & kPrimitiveTextured) ? 0 : 1;
        if (texture != packet->texture)
        {
            game::tail(bindTexture, packet, texture, kCntDmaTag);
            return;
        }
    }
    READABLE_FUNCTION(setPrimitiveTexture, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void vif1PacketCnt(ee::Ptr<sce::Vif1Packet> packet, uint32_t flags) { ee::tailCall(kVif1PacketCnt, packet, flags); }
        }
    }

    // nextPrimitiveBuffer: the packet goes on in its next DMA buffer (this frame's
    // set's next, allocated when the set has no more), chained from the one
    // before.
    void nextPrimitiveBuffer(ee::Ptr<PrimitivePacket> packet)
    {
        const ee::Ptr<PointerArray> buffers = packet->buffers[*g_bufferSet];
        uint32_t buffer = 0;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 walks the set with arrayFirst and arrayNext (ZGE3 writes
            // the walk in).
            buffer = packet->buffer == 0 ? arrayFirst(buffers) : arrayNext(buffers);
        }
        else if (packet->buffer == 0)
        {
            // The frame's first: the set from its start.
            if (buffers->count != 0)
            {
                buffers->cursor = 0;
                buffer = buffers->items[0];
            }
            else
                buffers->cursor = -1;
        }
        else
        {
            const int32_t next = buffers->cursor + 1;
            if (buffers->cursor >= 0)
            {
                buffers->cursor = next;
                if (next < static_cast<int32_t>(buffers->count))
                    buffer = buffers->items[next];
            }
            if (!buffer)
            {
                // The set's end (or no walk): a new buffer.
                buffers->cursor = -1;
            }
        }
        if (!buffer)
        {
            buffer = allocateIn(packet->bufferBytes, 0, 1);
            pointerArrayPushBack(buffers, buffer);
        }
        const ee::Ptr<sce::Vif1Packet> vif = &packet->vif;
        if (packet->buffer != 0)
        {
            sce::vif1PacketNext(vif, buffer, 0);
            sce::vif1PacketAlign(vif, 2, 0);
            vif1PacketCloseDmaTag(vif);
        }
        packet->buffer = ee::uncachedMirror(buffer);
        vif1PacketInit(vif, packet->buffer);
        vif1PacketReset(vif);
        tail::vif1PacketCnt(vif, 0);
    }
    READABLE_FUNCTION_WAITS(nextPrimitiveBuffer, Verified);

    namespace
    {
        // The material put on this frame's list of the primitives' materials (whose textures the VIF1 handler
        // uploads), once a frame: a material keeps the frame it was listed in.
        void listMaterialOnce(PrimitiveWords &words, Material &material)
        {
            uint32_t &listedFrame = materialListedFrame(material);
            if (listedFrame == words.frame)
                return;
            *words.boundEnd = ee::Ptr<Material>(&material);
            words.boundEnd = words.boundEnd + 1;
            listedFrame = words.frame;
        }

        // A DMA tag opened at the packet's write position: its first doubleword `tag` (vif1PacketCloseDmaTag
        // fills in its quadword count), its second two VIF NOPs.
        void openDmaTag(sce::Vif1Packet &vif, uint32_t tag)
        {
            const ee::Ptr<uint64_t> words = vif.current.cast<uint64_t>();
            vif.dmaTag = vif.current;
            words[0] = tag;
            words[1] = 0;
            vif.current = (words + 2).cast<uint32_t>();
        }

        // PRIM's bits above the primitive type, as a GIF tag holds them (its PRIM field, bits 47-57).
        constexpr uint32_t kGifTagPrimShift = 47;
        constexpr uint64_t kPrimAttributeBits = 0x7F8;
        // The room a packet's buffer needs for the material's tags; less and it goes on in the next buffer.
        constexpr uint32_t kBindMargin = 0x100;
        // The VIF codes of the material's "ref": STCYCL CL 4, WL 4, then an UNPACK V4-32 of its GIF tag and
        // seven registers (8 quadwords) to VU1 0, TOPS-relative.
        constexpr uint32_t kMaterialCycle = vif::stcycl(4, 4);
        constexpr uint32_t kMaterialUnpack = vif::unpack(vif::kUnpackV4_32, 8, vif::kUnpackFromTops | 0u);
    }

    // bindTexture: the batch's texture (a material) bound to the packet: the packet keeps it, its two-sided flag
    // and the PRIM bits of its GIF tag, and the next run starts the VU1 program over; the material goes on this
    // frame's upload list. The open DMA tag closes (the packet moves on to its next buffer when this one is
    // nearly full), a "ref" tag sends the material's GS block (its first layer's GIF tag, registers and
    // routines) to VU1, and a new tag `dmaTag` opens for the strips that follow.
    void bindTexture(ee::Ptr<PrimitivePacket> packet, uint32_t texture, uint32_t dmaTag)
    {
        const ee::Ptr<Material> material(texture);
        packet->texture = texture;
        packet->twoSided = static_cast<uint32_t>(material->flags >> kMaterialFlagTwoSided) & 1u;
        listMaterialOnce(*g_primitiveWords, *material);
        packet->startProgram = 1;
        const MaterialLayer &layer = materialLayer(material, 0);
        packet->primBits = (layer.gifTag[0] >> kGifTagPrimShift) & kPrimAttributeBits;
        vif1PacketCloseDmaTag(&packet->vif);
        if (packet->bufferBytes - kBindMargin < bytesUsed(*packet))
            nextPrimitiveBuffer(packet);
        sce::vif1PacketRef(&packet->vif, &layer.gifTag, layerQwordCount(material, 0), kMaterialCycle, kMaterialUnpack, 0);
        if (packet->vif.dmaTag)
            vif1PacketCloseDmaTag(&packet->vif);
        openDmaTag(packet->vif, dmaTag);
    }
    READABLE_FUNCTION_WAITS(bindTexture, Verified);

    // projectedDepth: the screen depth of a vertex raised by zOffset: (x, y,
    // z + zOffset, 1) by the projection (VU0, all four lanes), divided by
    // its w; the result's z.
    ee::Float projectedDepth(ee::Ptr<Matrix> projection, ee::Ptr<PrimitiveVertex> vertex, ee::Float zOffset)
    {
        // Both stay in the frame: see drawScreenPrimitives.
        ee::Local<Vec4> depth;
        ee::Local<Vec4> point;
        point->x = vertex->x;
        point->y = vertex->y;
        point->z = vertex->z + zOffset;
        point->w = 1.0f;
        depth = projected(point, *projection);
        return depth->z;
    }
    READABLE_FUNCTION(projectedDepth, Verified);

    namespace
    {
        // A batch of strips (or fans) into the 2D packet, each as a GS
        // primitive of the kind given. The batch's depth is the default, or
        // projected from its first vertex.
        int32_t drawScreenBatch(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix, uint32_t primitive)
        {
            // These stay in the frame as the original leaves them, though nothing reads
            // them back after the batch: the last strip's GIF tag, the moved vertex, the
            // loop's place, and the screen vertex record's address rounded to the
            // nearest 64 KB, which the original keeps there across each vertex's
            // addVertex (its upper doubleword 0). Later frames reuse that
            // stack: a save message's two unset words (formatLocalizedText) copy what
            // these stores leave into the queue on the heap. No reader of that kind of
            // message looks at them, so no game behaviour depends on the values; they
            // stay so that RAM stays comparable with the original's. (drawScreenFans'
            // original keeps the same places; its block's upper doubleword is what the
            // last call left in a register's upper half, 0 here.) ZGE2's keeps the tag, the moved vertex and the
            // depth bit with the strips left only (its register saves start at +0x30) and moves a vertex with its
            // transformPoint call.
            ee::Local<ee::Qword> stripTag;          // frame +0x00
            ee::Local<Vec4> moved;                  // +0x10
            ee::Local<ScreenStripLoopPlace> place;  // +0x20
            ee::Local<ee::Qword> screenVertexBlock; // +0x30
            constexpr uint32_t kBlock = 64u * 1024u;
            constexpr uint32_t kScreenVertexBlock = (sym::g_screenVertex + kBlock / 2) & ~(kBlock - 1);
            const ee::Ptr<PrimitivePacket> packet = g_screenPrimitives.get();
            if (flags & kPrimitiveSameMatrix)
                matrix = *g_lastPrimitiveMatrix;
            else
                *g_lastPrimitiveMatrix = matrix;
            place->depthFromVertex = flags & kPrimitiveDepthFromVertex;
            const bool depthFromVertex = place->depthFromVertex != 0;
            // By address (a guest call, not game::call): its tail call then
            // returns through the scheduler once when the binding unwinds, as
            // the original's (a readable callee called directly has its tail
            // call made as a call, which returns twice).
            setPrimitiveTexture(packet, strip->texture, depthFromVertex ? 0u : 2u, flags);
            for (int32_t left = strips - 1;;)
            {
                if (left == -1)
                    break;
                if (static_cast<uint32_t>(packet->bufferBytes - 0x200u) < bytesUsed(*packet))
                {
                    nextPrimitiveBuffer(packet); // by address: its tail call (as setPrimitiveTexture's)
                }
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                *stripTag = gifTag(static_cast<uint32_t>(strip->count), primitive, packet->primBits);
                sce::vif1PacketAddQword(&packet->vif, *stripTag);
                if (depthFromVertex)
                {
                    const ee::Ptr<PrimitiveVertex> first = strip->vertices[0];
                    const ee::Float raise = matrix ? matrix->translation().z : ee::Float(0.0f);
                    g_screenVertex->depth = projectedDepth(*g_screenProjection, first, raise);
                }
                ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices = strip->vertices;
                int32_t count = strip->count;
                const int32_t nextLeft = left - 1;
                const ee::Ptr<PrimitiveStrip> nextStrip = strip + 1;
                place->stripsLeft = nextLeft;
                if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps the next strip in a register)
                    place->nextStrip = nextStrip;
                int32_t beforeTriangles = 2;
                for (;;)
                {
                    const ee::Ptr<PrimitiveVertex> v = *vertices;
                    const uint32_t shift = packet->colourShift & 31u;
                    ++vertices;
                    ScreenVertexQwords &q = *g_screenVertex;
                    q.uv = texelWords(*v);
                    for (int i = 0; i < 4; ++i)
                        q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> shift;
                    uint64_t xy;
                    const bool noTriangle = beforeTriangles-- > 0;
                    if (matrix)
                    {
                        if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
                            transformPoint(moved, matrix, v.cast<const Vec4>());
                        else
                            moved = transformed(*matrix, quadwordOf(v));
                        xy = xyWords(*moved);
                    }
                    else
                        xy = xyWords(*v);
                    q.xy = noTriangle ? xy & ~1ull : xy | 1u;
                    if constexpr (!game::readable::kIsDm2) // (ZGE2's frame has its register saves there)
                        *screenVertexBlock = ee::Qword{kScreenVertexBlock, 0};
                    addVertex(packet, *g_screenVertex);
                    if (--count == 0)
                        break;
                }
                strip = nextStrip;
                left = nextLeft;
                g_screenVertex->depth = *g_defaultScreenDepth;
                sce::vif1PacketCloseUnpack(&packet->vif);
            }
            if (packet->startProgram)
            {
                const int32_t result = runScreenProgram(packet, kRunProgram);
                packet->startProgram = 0;
                return result;
            }
            const int32_t result = runScreenProgram(packet, kContinueProgram);
            return result;
        }
    }

    // drawScreenPrimitives: a batch of strips into the 2D packet.
    int32_t drawScreenPrimitives(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix)
    {
        return drawScreenBatch(flags, strip, strips, matrix, kPrimTriangleStrip);
    }
    READABLE_FUNCTION_WAITS(drawScreenPrimitives, Verified);

    // drawScreenFans: drawScreenPrimitives with each strip's vertices a
    // triangle fan. (Nothing in the game calls it.)
    int32_t drawScreenFans(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix)
    {
        return drawScreenBatch(flags, strip, strips, matrix, kPrimTriangleFan);
    }
    READABLE_FUNCTION_WAITS(drawScreenFans, Draft);

    // drawWorldPrimitives: a batch of strips into the world packet, its vertices
    // by the batch's matrix (VU0) if it has one. Always -1.
    int32_t drawWorldPrimitives(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix)
    {
        ee::Local<Vec4> moved; // stays in the frame: see drawScreenPrimitives
        ee::Local<StripLoopPlace> place; // the same: drawSparks' first streak measures it
        ee::Local<ee::Qword> stripTag;   // the same (after drawPosters)
        const ee::Ptr<PrimitivePacket> packet = g_worldPrimitives.get();
        setPrimitiveTexture(packet, strip->texture, 0u, flags); // by address: see drawScreenPrimitives
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
        {
            if (flags & kPrimitiveViewRelative)
            {
                if (matrix)
                {
                    matrixMultiply(kComposedMatrix, *g_viewMatrix, ee::Ptr<Matrix>(matrix));
                    matrix = kComposedMatrix;
                }
                else
                    matrix = *g_viewMatrix;
            }
            *g_lastPrimitiveMatrix = matrix;
        }
        int32_t left = strips - 1;
        if (left == -1)
            return -1;
        for (;;)
        {
            int32_t remaining = strip->count;
            ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices = strip->vertices;
            const int32_t nextLeft = left - 1;
            const ee::Ptr<PrimitiveStrip> nextStrip = strip + 1;
            place->nextStrip = nextStrip;
            place->stripsLeft = nextLeft;
            while (remaining != 0)
            {
                // A piece of at most 60 vertices.
                int32_t count = static_cast<uint32_t>(remaining) < static_cast<uint32_t>(kMaxStrip + 1) ? remaining : kMaxStrip;
                const ee::Ptr<uint32_t> start = packet->vif.bufferStart;
                if (static_cast<uint32_t>(packet->bufferBytes - ((count << 6) + kBufferMargin)) < bytesUsed(*packet))
                {
                    nextPrimitiveBuffer(packet);
                }
                const int32_t after = remaining - count;
                place->verticesAfter = after;
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                *stripTag = stripGifTag(static_cast<uint32_t>(count), packet->primBits);
                sce::vif1PacketAddQword(&packet->vif, *stripTag);
                sce::vif1PacketCloseUnpack(&packet->vif);
                sce::vif1PacketOpenUnpack(&packet->vif, kStripVertices, vif::kUnpackV4_32, 4, 4);
                int32_t beforeTriangles = 2;
                // kStripNoTriangle for the first two vertices: a bit each, shifted out.
                uint32_t firstTwo = kStripNoTriangle | kStripNoTriangle >> 1;
                uint32_t alternate = kStripFlipped;
                for (;;)
                {
                    const ee::Ptr<PrimitiveVertex> v = *vertices;
                    const uint32_t shift = packet->colourShift & 31u;
                    const uint32_t vertexFlags = (firstTwo & kStripNoTriangle) | alternate;
                    ++vertices;
                    firstTwo <<= 1;
                    alternate ^= kStripFlipped;
                    WorldVertexQwords &q = *g_worldVertex;
                    q.s = fixedUv(v->u);
                    q.t = fixedUv(v->v);
                    q.flags = vertexFlags;
                    for (int i = 0; i < 4; ++i)
                        q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> shift;
                    uint64_t xy;
                    ee::Float z;
                    const bool noTriangle = beforeTriangles-- > 0;
                    if (matrix)
                    {
                        moved = transformed(*matrix, quadwordOf(v));
                        xy = xyWords(*moved);
                        z = moved->z;
                    }
                    else
                    {
                        xy = xyWords(*v);
                        z = v->z;
                    }
                    q.xy = noTriangle ? xy & ~1ull : xy | 1u;
                    q.z = z;
                    addVertex(packet, *g_worldVertex);
                    if (--count == 0)
                        break;
                }
                sce::vif1PacketCloseUnpack(&packet->vif);
                if (packet->startProgram)
                {
                    runVertexProgram(packet, kRunProgram);
                    packet->startProgram = 0;
                }
                else
                    runVertexProgram(packet, kContinueProgram);
                if (after == 0)
                    break;
                // The next piece starts again from the last two vertices.
                remaining = after + 2;
                vertices -= 2;
            }
            strip = nextStrip;
            left = nextLeft;
            if (left == -1)
                return -1;
        }
    }

    // drawWorldStripsByPointTransform: ZGE2's drawWorldPrimitives: the same strips
    // in pieces, but a vertex is moved by the batch's matrix through ZGE2's
    // point transform (transformPoint), its flags are 0x8000 for the strip's
    // first two alone (no every-other-vertex bit), and it hands back nothing.
    void drawWorldStripsByPointTransform(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix)
    {
        ee::Local<Vec4> moved; // stays in the frame: see drawScreenPrimitives
        ee::Local<StripLoopPlace> place;
        ee::Local<ee::Qword> stripTag;
        const ee::Ptr<PrimitivePacket> packet = g_worldPrimitives.get();
        setPrimitiveTexture(packet, strip->texture, 0u, flags);
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
        {
            if (flags & kPrimitiveViewRelative)
            {
                if (matrix)
                {
                    matrixMultiply(kComposedMatrix, *g_viewMatrix, ee::Ptr<Matrix>(matrix));
                    matrix = kComposedMatrix;
                }
                else
                    matrix = *g_viewMatrix;
            }
            *g_lastPrimitiveMatrix = matrix;
        }
        for (int32_t left = strips - 1; left != -1;)
        {
            int32_t remaining = strip->count;
            ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices = strip->vertices;
            left = left - 1;
            strip = strip + 1;
            place->stripsLeft = left;
            place->nextStrip = strip;
            while (remaining != 0)
            {
                // A piece of at most 60 vertices.
                int32_t count = static_cast<uint32_t>(remaining) < static_cast<uint32_t>(kMaxStrip + 1) ? remaining : kMaxStrip;
                if (static_cast<uint32_t>(packet->bufferBytes - ((count << 6) + kBufferMargin)) < bytesUsed(*packet))
                    nextPrimitiveBuffer(packet);
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                const int32_t after = remaining - count;
                *stripTag = stripGifTag(static_cast<uint32_t>(count), packet->primBits);
                place->verticesAfter = after;
                sce::vif1PacketAddQword(&packet->vif, *stripTag);
                sce::vif1PacketCloseUnpack(&packet->vif);
                sce::vif1PacketOpenUnpack(&packet->vif, kStripVertices, vif::kUnpackV4_32, 4, 4);
                int32_t beforeTriangles = 2;
                do
                {
                    const ee::Ptr<PrimitiveVertex> v = *vertices;
                    ++vertices;
                    WorldVertexQwords &q = *g_worldVertex;
                    q.s = fixedUv(v->u);
                    q.t = fixedUv(v->v);
                    q.flags = beforeTriangles > 0 ? kStripNoTriangle : 0u; // no triangle yet
                    const uint32_t shift = packet->colourShift & 31u;
                    for (int i = 0; i < 4; ++i)
                        q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> shift;
                    const bool noTriangle = beforeTriangles-- > 0;
                    if (matrix)
                    {
                        transformPoint(moved, matrix, v.cast<const Vec4>());
                        q.xy = noTriangle ? xyWords(*moved) & ~1ull : xyWords(*moved) | 1u;
                        q.z = moved->z;
                    }
                    else
                    {
                        q.xy = noTriangle ? xyWords(*v) & ~1ull : xyWords(*v) | 1u;
                        q.z = v->z;
                    }
                    addVertex(packet, *g_worldVertex);
                } while (--count != 0);
                sce::vif1PacketCloseUnpack(&packet->vif);
                if (packet->startProgram)
                {
                    runVertexProgram(packet, kRunProgram);
                    packet->startProgram = 0;
                }
                else
                    runVertexProgram(packet, kContinueProgram);
                if (after == 0)
                    break;
                // The next piece starts again from the last two vertices.
                remaining = after + 2;
                vertices -= 2;
            }
        }
    }
    READABLE_FUNCTION_WAITS_OWN(drawWorldPrimitives, Verified, dm2, drawWorldStripsByPointTransform);

    // drawWorldFans: a batch whose strips are triangle fans, drawn one
    // triangle at a time (the first vertex and each next two) as three-vertex
    // strips through drawWorldPrimitives, all with the first strip's texture.
    // Returns the last one's result. A count of 0 (strips, or a strip's
    // vertices less 2) is not checked: it runs as if it were 2^32.
    // (Nothing in the game calls it.)
    int32_t drawWorldFans(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix)
    {
        ee::Local<PrimitiveStrip> triangle;
        ee::Local<ee::Ptr<PrimitiveVertex>[3]> corners;
        triangle->texture = strip->texture;
        triangle->vertices = corners;
        triangle->count = 3;
        int32_t result;
        do
        {
            const ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices = strip->vertices;
            int32_t triangles = strip->count - 2;
            ++strip;
            corners[0] = vertices[0];
            for (int32_t next = 1;; ++next)
            {
                corners[1] = vertices[next];
                corners[2] = vertices[next + 1];
                result = drawWorldPrimitives(flags, triangle, 1, matrix);
                if (--triangles == 0)
                    break;
            }
        } while (--strips != 0);
        return result;
    }
    READABLE_FUNCTION_WAITS(drawWorldFans, Draft);

    // clipLine: the line between two vertices, each by a pointer, against
    // the view's planes, in a copy the pointers are set to (by the matrix if
    // there is one). A plane with both ends behind it ends it: 0, nothing to
    // draw. Each plane cuts the end behind it to where the line meets it.
    uint32_t clipLine(ee::Ptr<ee::Ptr<PrimitiveVertex>> first, ee::Ptr<ee::Ptr<PrimitiveVertex>> second, ee::Ptr<Matrix> matrix)
    {
        const ee::Ptr<PrimitiveVertex> clipped(sym::g_clippedLine);
        Vec4 &firstEnd = quadwordOf(clipped), &secondEnd = quadwordOf(clipped + 1);
        if (matrix)
        {
            firstEnd = transformed(*matrix, quadwordOf(*first));
            secondEnd = transformed(*matrix, quadwordOf(*second));
        }
        else
        {
            firstEnd = quadwordOf(*first);
            secondEnd = quadwordOf(*second);
        }
        copyUvAndColour(clipped[0], **first);
        *first = clipped;
        copyUvAndColour(clipped[1], **second);
        *second = clipped + 1;

        for (int32_t plane = kViewPlanes - 1; plane != -1; --plane)
        {
            const ee::Ptr<Vec4> normal(*g_viewPlanes + kViewPlanesLast - (kViewPlanes - 1 - plane) * kViewPlaneBytes);
            const ee::Float firstDistance = dot(firstEnd, *normal);
            const ee::Float distance = normal->w;
            const bool firstBefore = distance <= firstDistance;
            const ee::Float secondDistance = dot(secondEnd, *normal);
            const bool secondBefore = distance <= secondDistance;
            if (firstBefore != secondBefore)
            {
                if (firstBefore)
                    moveAlongLine(secondEnd, firstEnd, (distance - firstDistance) / (secondDistance - firstDistance));
                else
                    moveAlongLine(firstEnd, secondEnd, (distance - secondDistance) / (firstDistance - secondDistance));
            }
            if (!firstBefore && !secondBefore)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(clipLine, Verified);

    // drawWorldLines: a batch of lines into the world packet, by the batch's
    // matrix (VU0) if it has one, clipped first if the flags say so. Each
    // line is its own program run. Always -1.
    int32_t drawWorldLines(uint32_t flags, ee::Ptr<PrimitiveLine> line, int32_t lines, ee::Ptr<Matrix> matrix)
    {
        ee::Local<ee::Qword> tag;
        ee::Local<Vec4> moved;
        // On the stack: clipLine changes them.
        ee::Local<ee::Ptr<PrimitiveVertex>> firstEnd;
        ee::Local<ee::Ptr<PrimitiveVertex>> secondEnd;
        const ee::Ptr<PrimitivePacket> packet = g_worldPrimitives.get();
        setPrimitiveTexture(packet, line->texture, kLineSlotWorld, flags);
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
        {
            if (flags & kPrimitiveViewRelative)
            {
                if (matrix)
                {
                    const ee::Ptr<Matrix> given = matrix;
                    matrix = kComposedMatrix;
                    matrixMultiply(kComposedMatrix, *g_viewMatrix, given);
                }
                else
                    matrix = *g_viewMatrix;
            }
            *g_lastPrimitiveMatrix = matrix;
        }
        const uint32_t shift = packet->colourShift;
        const bool clipLines = (flags & kPrimitiveClipLines) != 0;
        for (int32_t left = lines - 1; left != -1; --left, ++line)
        {
            firstEnd = line->first;
            secondEnd = line->second;
            if (packet->bufferBytes - kBufferMargin < bytesUsed(*packet))
                nextPrimitiveBuffer(packet);
            bool drawn = true;
            if (clipLines)
                drawn = static_cast<int32_t>(clipLine(firstEnd, secondEnd, matrix)) != 0;
            if (drawn)
            {
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                tag = gifTag(2, kPrimLineStrip, packet->primBits);
                sce::vif1PacketAddQword(&packet->vif, *tag);
                WorldLineQwords *const q = *g_worldLine;
                const ee::Ptr<PrimitiveVertex> ends[2] = {*firstEnd, *secondEnd};
                if (!clipLines && matrix)
                {
                    moved = transformed(*matrix, quadwordOf(ends[0]));
                    q[0].xy = xyWords(*moved) & ~1ull;
                    q[0].z = moved->z;
                    moved = transformed(*matrix, quadwordOf(ends[1]));
                    q[1].xy = xyWords(*moved) | 1u;
                    q[1].z = moved->z;
                }
                else
                {
                    q[0].xy = xyWords(*ends[0]) & ~1ull;
                    q[0].z = ends[0]->z;
                    q[1].xy = xyWords(*ends[1]) | 1u;
                    q[1].z = ends[1]->z;
                }
                for (int end = 0; end < 2; ++end)
                {
                    q[end].uv = texelWords(*ends[end]);
                    q[end].flags = end == 0 ? kStripNoTriangle : 0u;
                    for (int i = 0; i < 4; ++i)
                        q[end].rgba[i] = static_cast<int32_t>(ends[end]->rgba[i]) >> (shift & 31u);
                }
                addVertex(packet, *g_worldLine);
                sce::vif1PacketCloseUnpack(&packet->vif);
                if (packet->startProgram)
                {
                    runVertexProgram(packet, kRunLineProgram);
                    packet->startProgram = 0;
                }
                else
                    runVertexProgram(packet, kContinueLineProgram);
            }
        }
        return -1;
    }

    namespace
    {
        // ZGE2's line programs (MSCALs).
        constexpr uint32_t kZge2RunLineProgram = vif::mscal(0x10), kZge2ContinueLineProgram = vif::mscal(0x12);
    }

    // drawWorldLinesUnclipped: ZGE2's drawWorldLines: no clipping; both ends
    // through the one line record (its flags left as they were), an end moved
    // by the batch's matrix through ZGE2's point transform (each into its own
    // frame slot) but its depth the unmoved vertex's; ZGE2's line programs;
    // it hands back nothing.
    void drawWorldLinesUnclipped(uint32_t flags, ee::Ptr<PrimitiveLine> line, int32_t lines, ee::Ptr<Matrix> matrix)
    {
        ee::Local<ee::Qword> tag;
        ee::Local<Vec4> moved[2] = {};
        const ee::Ptr<PrimitivePacket> packet = g_worldPrimitives.get();
        setPrimitiveTexture(packet, line->texture, kLineSlotWorld, flags);
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
        {
            if (flags & kPrimitiveViewRelative)
            {
                if (matrix)
                {
                    const ee::Ptr<Matrix> given = matrix;
                    matrix = kComposedMatrix;
                    matrixMultiply(kComposedMatrix, *g_viewMatrix, given);
                }
                else
                    matrix = *g_viewMatrix;
            }
            *g_lastPrimitiveMatrix = matrix;
        }
        const uint32_t shift = packet->colourShift;
        for (int32_t left = lines - 1; left != -1; --left, ++line)
        {
            if (packet->bufferBytes - kBufferMargin < bytesUsed(*packet))
                nextPrimitiveBuffer(packet);
            sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
            tag = gifTag(2, kPrimLineStrip, packet->primBits);
            sce::vif1PacketAddQword(&packet->vif, *tag);
            WorldLineQwords &q = (*g_worldLine)[0];
            const ee::Ptr<PrimitiveVertex> ends[2] = {line->first, line->second};
            for (int end = 0; end < 2; ++end)
            {
                const ee::Ptr<PrimitiveVertex> v = ends[end];
                q.uv = texelWords(*v);
                for (int i = 0; i < 4; ++i)
                    q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> (shift & 31u);
                uint64_t xy = xyWords(*v);
                if (matrix)
                {
                    transformPoint(moved[end], matrix, v.cast<const Vec4>());
                    xy = xyWords(*moved[end]);
                }
                q.xy = end == 0 ? xy & ~1ull : xy | 1u; // bit 0: the line's second end
                q.z = v->z;
                addVertex(packet, q);
            }
            sce::vif1PacketCloseUnpack(&packet->vif);
            if (packet->startProgram)
            {
                runVertexProgram(packet, kZge2RunLineProgram);
                packet->startProgram = 0;
            }
            else
                runVertexProgram(packet, kZge2ContinueLineProgram);
        }
    }
    READABLE_FUNCTION_WAITS_OWN(drawWorldLines, Verified, dm2, drawWorldLinesUnclipped);

    namespace
    {
        // A vertex's position as the point ZGE2's point transform reads: the
        // quadword it starts with (x, y, z and the word after).
        ee::Ptr<const Vec4> vertexPoint(ee::Ptr<PrimitiveVertex> vertex)
        {
            return vertex.cast<const Vec4>();
        }
    }

    // drawScreenLines: a batch of lines into the 2D packet, at the default depth
    // or one projected from each line's first vertex. The program runs once
    // after the batch.
    int32_t drawScreenLines(uint32_t flags, ee::Ptr<PrimitiveLine> line, int32_t lines, ee::Ptr<Matrix> matrix)
    {
        ee::Local<ee::Qword> tag;
        ee::Local<Vec4> moved;
        ee::Local<int32_t> nextLeft;
        ee::Local<int32_t> depthFromVertex;
        ee::Local<ee::Ptr<PrimitiveLine>> nextLine;
        ee::Local<ee::Qword> qword;
        const ee::Ptr<PrimitivePacket> packet = g_screenPrimitives.get();
        // (The original's saved registers, as its callers and its last line
        // left them.)
        int32_t beforeSecond = 0;
        int32_t verticesLeft = 0;
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
            *g_lastPrimitiveMatrix = matrix;
        setPrimitiveTexture(packet, line->texture, kLineSlotScreen, flags);
        int32_t left = lines - 1;
        beforeSecond = static_cast<int32_t>(flags & kPrimitiveDepthFromVertex);
        if (left != -1)
        {
            *depthFromVertex = beforeSecond;
            for (;;)
            {
                if (packet->bufferBytes - kBufferMargin < bytesUsed(*packet))
                    nextPrimitiveBuffer(packet);
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                *tag = gifTag(2, kPrimLineStrip, packet->primBits);
                sce::vif1PacketAddQword(&packet->vif, *tag);
                if (*depthFromVertex != 0)
                {
                    const ee::Float raise = matrix ? matrix->translation().z : ee::Float(0.0f);
                    g_screenVertex->depth = projectedDepth(*g_screenProjection, line->first, raise);
                }
                *nextLeft = left - 1;
                verticesLeft = 2;
                beforeSecond = 1;
                *nextLine = line + 1;
                ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices = &line->first;
                for (;;)
                {
                    const ee::Ptr<PrimitiveVertex> v = *vertices;
                    const uint32_t shift = packet->colourShift & 31u;
                    ++vertices;
                    ScreenVertexQwords &q = *g_screenVertex;
                    q.uv = texelWords(*v);
                    for (int i = 0; i < 4; ++i)
                        q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> shift;
                    const bool firstEnd = beforeSecond-- > 0;
                    uint64_t xy;
                    if (matrix)
                    {
                        if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
                            transformPoint(moved, matrix, vertexPoint(v));
                        else
                            moved = transformed(*matrix, quadwordOf(v));
                        xy = xyWords(*moved);
                    }
                    else
                        xy = xyWords(*v);
                    q.xy = firstEnd ? xy & ~1ull : xy | 1u;
                    --verticesLeft;
                    addVertex(packet, *g_screenVertex);
                    if (verticesLeft != 0)
                        continue;
                    break;
                }
                line = *nextLine;
                closeVertexUnpack(packet);
                left = *nextLeft;
                g_screenVertex->depth = *g_defaultScreenDepth;
                if (left != -1)
                    continue;
                break;
            }
        }
        int32_t result;
        if (packet->startProgram)
        {
            result = runScreenProgram(packet, kRunProgram);
            packet->startProgram = 0;
        }
        else
            result = runScreenProgram(packet, kContinueProgram);
        return result;
    }
    READABLE_FUNCTION_WAITS(drawScreenLines, Verified);

    // drawScreenLineLists: lists of lines into the 2D packet, at most 16 vertices an
    // UNPACK, each piece its own run. Always -1.
    int32_t drawScreenLineLists(uint32_t flags, ee::Ptr<PrimitiveLineList> list, int32_t lists, ee::Ptr<Matrix> matrix)
    {
        ee::Local<ee::Qword> tag;
        ee::Local<Vec4> moved;
        ee::Local<int32_t> remainingVertices;
        ee::Local<int32_t> nextLeft;
        ee::Local<ee::Ptr<PrimitiveLineList>> nextList;
        const ee::Ptr<PrimitivePacket> packet = g_screenPrimitives.get();
        int32_t pieceLeft = 0; // vertices of this piece still to write
        if (flags & kPrimitiveSameMatrix)
            matrix = *g_lastPrimitiveMatrix;
        else
            *g_lastPrimitiveMatrix = matrix;
        setPrimitiveTexture(packet, list->texture, kLineSlotScreen, flags);
        int32_t left = lists - 1;
        if (left == -1)
            return -1;
        for (;;)
        {
            *nextLeft = left - 1;
            int32_t vertices = list->lines << 1;
            ee::Ptr<PrimitiveVertex> vertex = list->vertices;
            *nextList = list + 1;
            uint32_t oddVertex = 0;
            for (;;)
            {
                const uint32_t piece = static_cast<uint32_t>(vertices) <= kMaxListVertices ? static_cast<uint32_t>(vertices) : kMaxListVertices;
                if (packet->bufferBytes - kListBufferMargin < bytesUsed(*packet))
                    nextPrimitiveBuffer(packet);
                pieceLeft = static_cast<int32_t>(piece);
                sce::vif1PacketOpenUnpack(&packet->vif, kStripHeader, vif::kUnpackV4_32, 4, 4);
                *remainingVertices = vertices - static_cast<int32_t>(piece);
                oddVertex = 0;
                *tag = gifTag(piece, kPrimLineStrip, packet->primBits);
                sce::vif1PacketAddQword(&packet->vif, *tag);
                for (;;)
                {
                    const ee::Ptr<PrimitiveVertex> v = vertex;
                    const uint32_t shift = packet->colourShift & 31u;
                    ScreenVertexQwords &q = *g_screenVertex;
                    q.uv = texelWords(*v);
                    for (int i = 0; i < 4; ++i)
                        q.rgba[i] = static_cast<int32_t>(v->rgba[i]) >> shift;
                    oddVertex ^= 1;
                    uint64_t xy;
                    if (matrix)
                    {
                        if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
                            transformPoint(moved, matrix, vertexPoint(v));
                        else
                            moved = transformed(*matrix, quadwordOf(v));
                        xy = xyWords(*moved);
                    }
                    else
                        xy = xyWords(*v);
                    q.xy = oddVertex != 0 ? xy & ~1ull : xy | 1u;
                    ++vertex;
                    addVertex(packet, *g_screenVertex);
                    if (--pieceLeft == 0)
                        break;
                }
                sce::vif1PacketCloseUnpack(&packet->vif);
                if (packet->startProgram)
                {
                    runVertexProgram(packet, kRunProgram);
                    packet->startProgram = 0;
                }
                else
                    runVertexProgram(packet, kContinueProgram);
                vertices = *remainingVertices;
                if (vertices == 0)
                    break;
            }
            left = *nextLeft;
            list = *nextList;
            if (left != -1)
                continue;
            break;
        }
        return -1;
    }
    READABLE_FUNCTION_WAITS(drawScreenLineLists, Verified);

    // drawPrimitiveStrips: triangles (debug triangles, the profiler's
    // ticks, shards) drawn one by one as three-vertex strips, into the
    // packet the flags pick (drawPrimitives). Returns the last one's result.
    // A count of 0 is not checked: it draws as if it were 2^32.
    int32_t drawPrimitiveStrips(uint32_t flags, ee::Address triangleRecords, int32_t count, ee::Address matrix)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        ee::Ptr<PrimitiveTriangle> triangles(triangleRecords);
        ee::Local<PrimitiveStrip> strip;
        int32_t result;
        do
        {
            strip->texture = triangles->texture;
            strip->vertices = &triangles->vertices[0];
            strip->count = 3;
            ++triangles;
            result = drawPrimitives(flags, strip, 1, matrix);
        } while (--count != 0);
        return result;
    }
    READABLE_FUNCTION_WAITS(drawPrimitiveStrips, Verified);

    namespace
    {
        struct alignas(16) QuadStrip
        {
            PrimitiveStrip strip;
            uint32_t padding;
            ee::Ptr<PrimitiveVertex> vertices[4];
        };
        static_assert(offsetof(QuadStrip, vertices) == 0x10);
    }

    // drawPrimitiveQuads: quads (a texture and four vertices each, 0x14 bytes)
    // drawn one by one as four-vertex strips (vertices 0, 1, 3, 2), into the
    // packet the flags pick. Returns the last builder's result.
    int32_t drawPrimitiveQuads(uint32_t flags, ee::Address quadRecords, int32_t count, ee::Address matrixAddress)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        const ee::Ptr<Matrix> matrix(matrixAddress);
        ee::Ptr<uint32_t> quads(quadRecords); // five words a quad: its texture, then its corners
        ee::Local<QuadStrip> quad; // the strip and its vertex list
        const bool screen = (flags & kPrimitiveScreen) != 0;
        int32_t result = 0;
        do
        {
            quad->strip.texture = quads[0];
            quad->vertices[0] = ee::Ptr<PrimitiveVertex>(quads[1]);
            quad->vertices[1] = ee::Ptr<PrimitiveVertex>(quads[2]);
            quad->vertices[2] = ee::Ptr<PrimitiveVertex>(quads[4]);
            quad->vertices[3] = ee::Ptr<PrimitiveVertex>(quads[3]);
            quad->strip.vertices = &quad->vertices[0];
            quad->strip.count = 4;
            quads += 5;
            --count;
            const ee::Ptr<PrimitiveStrip> strip = &quad->strip;
            result = screen ? drawScreenPrimitives(flags, strip, 1, matrix) : drawWorldPrimitives(flags, strip, 1, matrix);
        } while (count != 0);
        return result;
    }
    READABLE_FUNCTION_WAITS(drawPrimitiveQuads, Verified);

    namespace
    {
        constexpr uint32_t kPacketBufferBytes = 0x8000;

        void addVifCode(PrimitivePacket &packet, uint32_t code)
        {
            sce::vif1PacketAddCode(&packet.vif, code);
        }
    }

    // resetPrimitiveBuffers: a new frame for the primitive API: this buffer
    // set's material list starts empty, the frame count goes up (a material
    // is listed again once), both packets start over in their first buffer,
    // and the world packet opens with the block that has the VIF1 handler
    // upload the primitives' textures: kMarkPrimitiveTextures, a NOP, FLUSHA with the
    // interrupt bit (the handler runs there), kMarkNoTextures, FLUSHA. The original
    // returns 1; no caller reads it.
    void resetPrimitiveBuffers()
    {
        if constexpr (game::readable::kIsDm2) // ZGE2 forgets each packet's texture and program start first
        {
            for (PrimitivePacket *packet : {&*g_worldPrimitives, &*g_screenPrimitives})
            {
                packet->texture = ~0u; // none (setPrimitiveTexture binds the next one)
                packet->startProgram = 0;
            }
        }
        PrimitiveWords &words = *g_primitiveWords;
        words.boundEnd = words.boundMaterials[*g_bufferSet];
        ++words.frame;
        for (PrimitivePacket *packet : {&*g_worldPrimitives, &*g_screenPrimitives})
        {
            packet->buffer = 0;
            packet->bufferBytes = kPacketBufferBytes;
            nextPrimitiveBuffer(packet);
        }
        PrimitivePacket &world = *g_worldPrimitives;
        addVifCode(world, vif::mark(kMarkPrimitiveTextures));
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no NOP before the interrupt)
            addVifCode(world, vif::nop());
        addVifCode(world, vif::flushA() | vif::kInterrupt);
        addVifCode(world, vif::mark(kMarkNoTextures));
        addVifCode(world, vif::flushA());
    }

    // listPrimitiveMaterial (ZGE4 only): the material put on this frame's list of the primitives' materials (whose
    // textures the VIF1 handler uploads), once a frame (listMaterialOnce). ZGE3's
    // bindTexture does the same inline; ZGE4 keeps it inline there too and has this one for its billboards
    // (drawQueuedBillboards). The original returns what it last held; no caller reads it.
    inline void listPrimitiveMaterial(ee::Ptr<Material> material)
    {
        listMaterialOnce(*g_primitiveWords, *material);
    }
    READABLE_FUNCTION_ONLY(bmx, listPrimitiveMaterial);

    // resetPrimitiveBuffersIntoPacket: ZGE4's resetPrimitiveBuffers: the same new frame, but the block that has the
    // VIF1 handler upload the primitives' textures goes into the packet it is given (a CNT tag opened there first;
    // 0: no packet, no block), not into the world packet (ZGE4's beginFramePackets hands it the frame packet;
    // initPrimitiveRenderer hands 0). The original returns 1; no caller reads it.
    void resetPrimitiveBuffersIntoPacket(sce::Vif1PacketPtr packet)
    {
        PrimitiveWords &words = *g_primitiveWords;
        words.boundEnd = words.boundMaterials[*g_bufferSet];
        ++words.frame;
        for (PrimitivePacket *primitives : {&*g_worldPrimitives, &*g_screenPrimitives})
        {
            primitives->buffer = 0;
            primitives->bufferBytes = kPacketBufferBytes;
            nextPrimitiveBuffer(primitives);
        }
        if (!packet)
            return;
        sce::vif1PacketCnt(packet, 0);
        sce::vif1PacketAddCode(packet, vif::mark(kMarkPrimitiveTextures));
        sce::vif1PacketAddCode(packet, vif::nop());
        sce::vif1PacketAddCode(packet, vif::flushA() | vif::kInterrupt);
        sce::vif1PacketAddCode(packet, vif::mark(kMarkNoTextures));
        sce::vif1PacketAddCode(packet, vif::flushA());
    }
    READABLE_FUNCTION_WAITS_OWN(resetPrimitiveBuffers, Verified, bmx, resetPrimitiveBuffersIntoPacket);

    // primitiveTextures: the materials the last frame's primitives bound
    // (the other buffer set's list): their count and the list.
    void primitiveTextures(ee::Ptr<int32_t> countOut, ee::Ptr<ee::Ptr<ee::Ptr<Material>>> listOut)
    {
        const uint32_t lastSet = *g_bufferSet ^ 1u;
        PrimitiveWords &words = *g_primitiveWords;
        *countOut = words.boundCount[lastSet];
        *listOut = words.boundMaterials[lastSet];
    }
    READABLE_FUNCTION(primitiveTextures, Verified);

    namespace
    {
        // What the 2D microprogram is handed each frame: x, y and depth
        // scales (y halved [inferred: field rendering's half-height
        // frames]), and the screen's corner in pixels (the display's is in
        // GS coordinates, 12.4 fixed point).
        struct ScreenViewport
        {
            Vec4 scale;
            Vec4 corner;
        };
        static_assert(sizeof(ScreenViewport) == 0x20);
        constexpr uint32_t kViewportToVu1 = vif::kUnpackFromTops | 0u; // VU1 address 0, TOPS-relative
        constexpr uint32_t kViewportQwords = 2;
        // The 2D program's routine at 0x10 that takes the viewport
        // [inferred], then the program from 0.
        constexpr uint32_t kLoadScreenViewport = vif::mscal(0x10), kRunScreenProgram = vif::mscal(0);

        void addCallCode(PrimitivePacket &packet, uint32_t code)
        {
            sce::vif1PacketAddCallCode(&packet.vif, code);
        }
    }

    // addViewportPacket: the frame's 2D packet starts with the scissor set
    // to the whole draw buffer and the screen's scales and corner sent to
    // VU1.
    void addViewportPacket()
    {
        PrimitivePacket &screen = *g_screenPrimitives;
        setViewportScissor(&screen.vif, 0);
        sce::vif1PacketCnt(&screen.vif, 0);
        ee::Local<ScreenViewport> viewport;
        viewport->scale = Vec4{ee::Float(1.0f), ee::Float(0.5f), ee::Float(1.0f), ee::Float(0.0f)};
        viewport->corner = Vec4{g_display->cornerX * ee::Float(kPixelsPerSubpixel), g_display->cornerY * ee::Float(kPixelsPerSubpixel),
                                ee::Float(0.0f), ee::Float(0.0f)};
        sce::vif1PacketOpenUnpack(&screen.vif, kViewportToVu1, vif::kUnpackV4_32, 4, 4);
        sce::vif1PacketAddQwords(&screen.vif, viewport, kViewportQwords);
        closeVertexUnpack(&screen);
        addCallCode(screen, kLoadScreenViewport);
        addCallCode(screen, kRunScreenProgram);
    }

    namespace
    {
        // ZGE2's view set-up of the world primitives (VU1's 2D and 3D programs at its own addresses): the STROW
        // row, BASE 0 and OFFSET 0x200, the screen and clip matrices and the frustum's first plane.
        constexpr uint32_t kZge2LoadScreenViewport = vif::mscal(0xC), kZge2LoadWorldView = vif::mscal(0xE);
        constexpr uint32_t kZge2WorldBufferOffset = 0x200; // the double buffer's second half
        inline ee::Global<uint32_t[4], sym::strowWords> g_strowWords;
        inline ee::Global<ee::Ptr<CameraFrustum>, sym::g_viewPlanes> g_viewFrustum;
        // The view's world -> GS screen and world -> clip matrices (zge/scene/node.h's g_screenMatrix, g_clipMatrix).
        inline ee::Global<ee::Ptr<Matrix>, sym::g_screenMatrix> g_viewScreenMatrix;
        inline ee::Global<ee::Ptr<Matrix>, sym::g_clipMatrix> g_viewClipMatrix;
    }

    // addViewportAndWorldView: ZGE2's addViewportPacket: the screen
    // primitives' packet as ZGE3's but with no scissor (the screen program's
    // viewport routine at 0xC), then the world primitives' packet started
    // the way ZGE3's camera starts it (beginFramePacket, with no scissor):
    // the STROW row, the double buffer's BASE and OFFSET, the world -> GS
    // screen and world -> clip matrices and the frustum's first plane, and
    // the world program's view routine (0xE). 1.
    int32_t addViewportAndWorldView()
    {
        PrimitivePacket &screen = *g_screenPrimitives;
        sce::vif1PacketCnt(&screen.vif, 0);
        ee::Local<ScreenViewport> viewport;
        viewport->scale = Vec4{ee::Float(1.0f), ee::Float(0.5f), ee::Float(1.0f), ee::Float(0.0f)};
        viewport->corner = Vec4{g_display->cornerX * ee::Float(kPixelsPerSubpixel), g_display->cornerY * ee::Float(kPixelsPerSubpixel),
                                ee::Float(0.0f), ee::Float(0.0f)};
        sce::vif1PacketOpenUnpack(&screen.vif, kViewportToVu1, vif::kUnpackV4_32, 4, 4);
        sce::vif1PacketAddQwords(&screen.vif, viewport, kViewportQwords);
        closeVertexUnpack(&screen);
        addCallCode(screen, kZge2LoadScreenViewport);
        addCallCode(screen, kRunScreenProgram);

        sce::Vif1Packet *world = &g_worldPrimitives->vif;
        sce::vif1PacketCnt(world, 0u);
        sce::vif1PacketAddCode(world, vif::flushA());
        sce::vif1PacketAddCode(world, vif::strow());
        for (uint32_t word = 0; word < 4; ++word)
            sce::vif1PacketAddWord(world, (*g_strowWords)[word]);
        sce::vif1PacketAddCode(world, vif::base(0));
        sce::vif1PacketAddCode(world, vif::offset(kZge2WorldBufferOffset));
        sce::vif1PacketOpenUnpack(world, kViewportToVu1, vif::kUnpackV4_32, 4u, 4u);
        sce::vif1PacketAddQwords(world, reinterpret_cast<const uint8_t *>((*g_viewScreenMatrix).get()), 4);
        sce::vif1PacketAddQwords(world, reinterpret_cast<const uint8_t *>((*g_viewClipMatrix).get()), 4);
        sce::vif1PacketAddQword(world, std::bit_cast<ee::Qword>((*g_viewFrustum)->planes[0]));
        sce::vif1PacketCloseUnpack(world);
        sce::vif1PacketAddCallCode(world, kZge2LoadWorldView);
        sce::vif1PacketAddCallCode(world, vif::mscal(0)); // the program from 0, after the routine
        return 1;
    }
    READABLE_FUNCTION_OWN(addViewportPacket, Verified, dm2, addViewportAndWorldView);

    // closeScreenPrimitivePackets: the frame's two packets ended, each chained to the
    // next buffer if there is one, then the count of materials the frame's
    // primitives listed. The count is signed (sra): an end below its list
    // gives a negative one (diffcall3: was the drafter's unsigned shift).
    int32_t closeScreenPrimitivePackets()
    {
        int32_t address = 0;
        ee::Ptr<PrimitivePacket> packet = g_screenPrimitives.get();
        for (int32_t i = 1; i >= 0; --i, packet = packet - 1)
        {
            const ee::Ptr<sce::Vif1Packet> vif = &packet->vif;
            if (address != 0)
                sce::vif1PacketNext(vif, address, 0);
            else
                sce::vif1PacketEnd(vif, 0);
            sce::vif1PacketAlign(vif, 2, 0);
            vif1PacketCloseDmaTag(vif);
            if constexpr (game::readable::kIsDm2)
            {
                // The next set's first buffer: ZGE2 asks arrayFirst (ZGE3
                // writes it in).
                address = static_cast<int32_t>(arrayFirst(packet->buffers[*g_bufferSet]));
                continue;
            }
            PointerArray &buffers = *packet->buffers[*g_bufferSet];
            if (buffers.count != 0)
            {
                buffers.cursor = 0;
                address = static_cast<int32_t>(buffers.items[0]);
            }
            else
            {
                buffers.cursor = -1;
                address = 0;
            }
        }
        const uint32_t bufferSet = *g_bufferSet;
        PrimitiveWords &words = *g_primitiveWords;
        words.boundCount[bufferSet] = static_cast<int32_t>(&*words.boundEnd - words.boundMaterials[bufferSet]);
        return address;
    }
    READABLE_FUNCTION(closeScreenPrimitivePackets, Verified);

    namespace
    {
        // The default materials, one for each kind of primitive (the index's
        // bits): 4 lines (else triangle strips), 2 no depth test (else
        // "greater or equal"), 1 alpha blended. setPrimitiveTexture's slot and
        // kPrimitiveSecondSlot pick one.
        constexpr int32_t kPrimitiveMaterials = 7;
        constexpr uint32_t kMaterialLines = 4, kMaterialNoDepthTest = 2, kMaterialBlended = 1;
        // The GS values set: PRIM's line and triangle strip, ZTST's always and
        // greater-or-equal, ALPHA's (Cs - Cd) * As + Cd.
        constexpr uint32_t kPrimLine = gs::kLine;

    }

    // createPrimitiveMaterials: the seven default materials, no texture.
    void createPrimitiveMaterials()
    {
        const ee::Ptr<ee::Ptr<Material>> materials(g_primitiveWords->defaultMaterials);
        for (int32_t kind = 0; kind < kPrimitiveMaterials; ++kind)
        {
            const ee::Ptr<Material> material = newMaterial();
            addMaterialTexture(material, 0);
            materials[kind] = material;
            setMaterialState(material, 0, kStatePrimType, (kind & kMaterialLines) ? kPrimLine : kPrimTriangleStrip);
            setMaterialState(material, 0, kStateZtst, (kind & kMaterialNoDepthTest) ? gs::kDepthAlways : gs::kDepthGequal);
            if (kind & kMaterialBlended)
            {
                setMaterialState(material, 0, kStateAbe, 1);
                setMaterialState(material, 0, kStateAlphaA, gs::kBlendSource);
                setMaterialState(material, 0, kStateAlphaB, gs::kBlendFrame);
                setMaterialState(material, 0, kStateAlphaC, gs::kBlendSourceAlpha);
                setMaterialState(material, 0, kStateAlphaD, gs::kBlendFrame);
            }
            else
                setMaterialState(material, 0, kStateAbe, 0);
        }
    }
    READABLE_FUNCTION_WAITS(createPrimitiveMaterials, Verified);

    // freePrimitiveMaterials: the seven default materials freed.
    void freePrimitiveMaterials()
    {
        const ee::Ptr<ee::Ptr<Material>> materials(g_primitiveWords->defaultMaterials);
        for (int32_t kind = 0; kind < kPrimitiveMaterials; ++kind)
            freeMaterial(materials[kind]);
    }
    READABLE_FUNCTION_WAITS(freePrimitiveMaterials, Verified);

    namespace
    {
        // Each packet keeps two sets of DMA buffers (a set per frame in
        // flight), each a list of 0x8000-byte buffers that grows as the
        // packet needs more. The lists start with this many (the world
        // packet's, the 2D packet's).
        inline ee::Global<int32_t[2], sym::g_primitiveBufferCounts> g_primitiveBufferCounts;
        constexpr int32_t kBufferSets = 2;
        constexpr uint32_t kBufferListCapacity = 4;
        // The renderer's state, from its flag to the staged line: the words, then 0x180 bytes of packets and the staged line.
        // (ZGE2's ends with its one staged world vertex, 0x390 bytes from the flag.)
        constexpr uint32_t kPrimitiveStateBytes = game::readable::kIsDm2 ? 0x390u : sizeof(PrimitiveWords) + 0x180;
        static_assert(kPrimitiveStateBytes == game::readable::perGame(0x3B0u, 0x390u, 0x5B0u));

        // The words initPrimitiveRenderer sets: ZGE2's primitives-on word too
        // (ZGE3 calls primitiveRendererReady instead).
        template <typename Words>
        void markPrimitiveRendererReady(Words &words)
        {
            words.ready = 1;
            if constexpr (requires { words.primitivesOn; })
                words.primitivesOn = 1;
            else
                primitiveRendererReady();
        }

    }

    // initPrimitiveRenderer: the renderer's state cleared and marked ready,
    // each packet's two buffer lists made and filled with their first
    // buffers, the default materials made, the staged vertices' constant
    // words set (Q and w 1, no flags, the 2D depth the display's near
    // depth), and the first frame begun. Returns 1 (startEngine goes on).
    uint32_t initPrimitiveRenderer()
    {
        memoryFill(g_primitiveWords.get(), 0, kPrimitiveStateBytes);
        markPrimitiveRendererReady(*g_primitiveWords);

        PrimitivePacket *const packets[] = {&*g_worldPrimitives, &*g_screenPrimitives};
        for (PrimitivePacket *packet : packets)
            for (int32_t set = 0; set < kBufferSets; ++set)
                packet->buffers[set] = newCursorArray(kBufferListCapacity);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 allocates each buffer when a frame first needs it)
            for (int32_t which = 0; which < 2; ++which)
                for (int32_t set = 0; set < kBufferSets; ++set)
                    for (int32_t i = 0; i < (*g_primitiveBufferCounts)[which]; ++i)
                        pointerArrayPushBack(packets[which]->buffers[set], allocateIn(kPacketBufferBytes, kMemoryPackets, 1));

        createPrimitiveMaterials();
        ScreenVertexQwords &screen = *g_screenVertex;
        WorldLineQwords *const line = *g_worldLine;
        screen.flags = 0;
        screen.q = ee::Float(1.0f);
        screen.depth = *g_defaultScreenDepth;
        screen.w = ee::Float(1.0f);
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 stages one world vertex there, no line.
            WorldVertexQwords &vertex = *g_worldVertex;
            vertex.q = ee::Float(1.0f);
            vertex.flags = 0;
            vertex.w = ee::Float(1.0f);
        }
        else
            for (int32_t end = 0; end < 2; ++end)
            {
                line[end].q = ee::Float(1.0f);
                line[end].flags = 0;
                line[end].w = ee::Float(1.0f);
            }
        if constexpr (game::readable::kIsBmx)
            resetPrimitiveBuffersIntoPacket(sce::Vif1PacketPtr()); // ZGE4's word: no packet to open the block in
        else
            resetPrimitiveBuffers();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initPrimitiveRenderer, Verified);

    // shutdownPrimitiveRenderer: if the renderer is ready, the default
    // materials, every buffer in each packet's two lists (walked until the
    // end or a 0 item) and the lists freed, and the renderer marked not
    // ready. Returns 1 either way (shutdownEngine goes on).
    uint32_t shutdownPrimitiveRenderer()
    {
        if (g_primitiveWords->ready == 0)
            return 1;
        freePrimitiveMaterials();
        for (PrimitivePacket *packet : {&*g_worldPrimitives, &*g_screenPrimitives})
            for (int32_t set = 0; set < kBufferSets; ++set)
            {
                // ZGE2 walks the buffers with arrayFirst and arrayNext; ZGE3 inlines the walk.
                uint32_t buffer;
                if constexpr (game::readable::kIsDm2)
                    buffer = arrayFirst(packet->buffers[set]);
                else
                    buffer = walkFirst(*packet->buffers[set]);
                while (buffer != 0)
                {
                    freeIn(buffer, kMemoryPackets);
                    if constexpr (game::readable::kIsDm2)
                        buffer = arrayNext(packet->buffers[set]);
                    else
                        walkNext(*packet->buffers[set], buffer);
                }
                freePointerArray(packet->buffers[set]);
            }
        g_primitiveWords->ready = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownPrimitiveRenderer, Verified);

    namespace
    {
        constexpr uint32_t kScreenPrimitives = sym::g_screenPrimitives;
        inline ee::Global<int32_t, sym::g_screenPrimitivesStart> g_screenPrimitivesStart;
        inline ee::Global<ee::Float, sym::g_primitiveDepth> g_primitiveDepth; // g_primitiveDepth; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_bufferSet> g_bufferSetWord; // g_bufferSetWord ((anonymous)::g_bufferSetWord)
        inline ee::Global<int32_t, sym::g_primitiveRendererReady> g_primitiveRendererReady;
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void vif1PacketCloseUnpack(uint32_t screenPrimitives) { ee::game(kCloseUnpack, screenPrimitives); }
        int32_t vif1PacketAddCallCode(uint32_t screenPrimitives, uint32_t x14000002u) { return ee::game<int32_t>(kAddCallCode, screenPrimitives, x14000002u); }
    }

    namespace
    {
        namespace routed
        {
            GAME_ROUTE(vif1PacketAddCallCode, sce::vif1PacketAddCallCode);
        }
    }

    int32_t flipBufferSet()
    {
        const int32_t bufferSet = *g_bufferSetWord ^ 0x1u;
        *g_bufferSetWord = bufferSet;
        return bufferSet;
    }
    READABLE_FUNCTION(flipBufferSet, Verified);

}
