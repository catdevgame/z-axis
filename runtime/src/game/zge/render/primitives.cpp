// The engine's primitive API: lines, quads and
// strips the effects, particles, fog planes, zgui and the text draw through
// two packets, the world-space one (g_worldPrimitives) and the 2D screen-space one
// (g_screenPrimitives).

#include "zge/platform/platform.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include "sym/zge/render/primitives.cpp.h"

namespace zge
{
    namespace
    {
        // The primitive renderer's words (ps2/primitive_packets.cpp has them too).
        inline ee::Global<PrimitiveWords, sym::g_primitiveRendererReady> g_primitiveWords;
    }
}

namespace zge
{
    // drawPrimitives: primitives by their flags' kind, to the 2D or the world
    // builder with the same arguments (the flags, the vertices, their count,
    // a matrix or none; the builders' own), both by a tail call.
    int32_t drawPrimitives(uint32_t flags, ee::Address strip, int32_t strips, ee::Address matrix)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        if constexpr (game::readable::kIsDm2) // (ZGE2 calls the builder and returns; ZGE3 and ZGE4 jump to it)
        {
            if (flags & kPrimitiveScreen)
                return drawScreenPrimitives(flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
            return drawWorldPrimitives(flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
        }
        if (flags & kPrimitiveScreen)
            return game::tail(drawScreenPrimitives, flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
        return game::tail(drawWorldPrimitives, flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
    }
    READABLE_FUNCTION(drawPrimitives, Verified);

    void primitiveRendererNoopA()
    {
    }
    READABLE_FUNCTION(primitiveRendererNoopA, Draft);

    uint32_t primitiveRendererReady()
    {
        return 1;
    }
    READABLE_FUNCTION_OWN(primitiveRendererReady, Verified, dm2, setPrimitivesOn);

    // setPrimitivesOn (ZGE2's primitiveRendererReady; the profiler calls it round its bars): whether the primitive
    // renderer draws (PrimitiveWords::primitivesOn), returning what it was.
    uint32_t setPrimitivesOn(uint32_t on)
    {
        const uint32_t was = primitivesOnWord(*g_primitiveWords);
        primitivesOnWord(*g_primitiveWords) = on;
        return was;
    }

    // drawLines: a batch of lines, on the screen (flags 0x02000000) or in the
    // world (drawScreenLines, drawWorldLines). Its callers reach it by
    // address. Was game/aggressive's (level/park_anchors.cpp); the blocks say
    // it is the engine's (layer2), BMX XXX (PS2) has it word for word.
    int32_t drawLines(uint32_t primitiveFlags, uint32_t vertices, uint32_t vertexCount, uint32_t vertexMatrix)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        if constexpr (game::readable::kIsDm2) // (ZGE2 calls the builder and returns; ZGE3 and ZGE4 jump to it)
        {
            if ((primitiveFlags & 0x02000000u) != 0)
                return zge::drawScreenLines(primitiveFlags, ee::Ptr<zge::PrimitiveLine>(vertices), vertexCount, ee::Ptr<zge::Matrix>(vertexMatrix));
            return zge::drawWorldLines(primitiveFlags, ee::Ptr<zge::PrimitiveLine>(vertices), vertexCount, ee::Ptr<zge::Matrix>(vertexMatrix));
        }
        if ((primitiveFlags & 0x02000000u) != 0)
            return game::tail(zge::drawScreenLines, primitiveFlags, ee::Ptr<zge::PrimitiveLine>(vertices), vertexCount, ee::Ptr<zge::Matrix>(vertexMatrix));
        return game::tail(zge::drawWorldLines, primitiveFlags, ee::Ptr<zge::PrimitiveLine>(vertices), vertexCount, ee::Ptr<zge::Matrix>(vertexMatrix));
    }
    READABLE_FUNCTION_WAITS(drawLines, Verified);

    int32_t drawTexturedPrimitives(uint32_t flags, ee::Address lists, int32_t count, ee::Address matrix)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        const int32_t result = flags & 0x02000000u;
        if (result != 0)
            return game::tail(drawScreenLineLists, flags, ee::Ptr<PrimitiveLineList>(lists), count, ee::Ptr<Matrix>(matrix));
        return result;
    }
    READABLE_FUNCTION_WAITS(drawTexturedPrimitives, Verified);

    void primitiveRendererNoopB()
    {
    }
    READABLE_FUNCTION(primitiveRendererNoopB, Draft);

    // drawFans: drawPrimitives for strips that are triangle fans: to the 2D
    // or the world builder (drawScreenFans, drawWorldFans), by a tail call.
    // (Nothing in the game calls it.)
    int32_t drawFans(uint32_t flags, ee::Address strip, int32_t strips, ee::Address matrix)
    {
        if (primitiveRendererOff())
            return kPrimitiveNotReadyResult;
        if (flags & kPrimitiveScreen)
            return game::tail(drawScreenFans, flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
        return game::tail(drawWorldFans, flags, ee::Ptr<PrimitiveStrip>(strip), strips, ee::Ptr<Matrix>(matrix));
    }
    READABLE_FUNCTION_WAITS(drawFans, Draft);
}
