#pragma once

// Part clones (clonePart): a model's instance gets its own chain of parts
// made from the model's.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/render/part.h"

namespace zge
{
    // clonePart's copyFlags: a stream whose bit is set is copied
    // (copyStream), else shared with the source (shareStream,
    // shareStream). Bits 0x800 >> k for kinds 0-5 (positions .. colours),
    // 0x20 the bones with the skin weights, 0x10 shadow edges, 0x8 >> (k - 9)
    // for groups .. face edges; the material sets are always copied.
    constexpr uint32_t kCopyBones = 0x020u;
    // ORed in: the material sets (stream kind 13). ZGE2's bit is 0x4000.
    constexpr uint32_t kAlwaysCopied = game::readable::perGame(0x2000u, 0x4000u);

    // A copy of source and of the parts chained after it (+0x30), each with
    // the source's counts and disc flags, its streams copied or shared by
    // copyFlags, its render state from cloneRenderState. ZGE2 copies the one
    // part, its counts and streams only.
    ee::Ptr<ZgePart> clonePart(ee::Ptr<ZgePart> source, uint32_t copyFlags);
}
