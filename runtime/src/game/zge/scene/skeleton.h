#pragma once

// Skinned models (types 8 and 0xC): bones,
// poses and per-vertex weights as the packet builders and drawSkinned read
// them.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>

namespace zge
{
    // One bone of a part's skeleton (Part::bones stream).
    struct Bone
    {
#if PS2X_GAME_BMX // (ZGE4's record is 0x20 bytes, its offset first: BMX loadBoneChunk, posePart, copyStream)
        Vec4 offset;        // BMX +0x00
        uint32_t bytes;     // BMX +0x10 the record's size (BoneRecord::bytes)
        uint16_t poseIndex; // BMX +0x14
        uint8_t _16[0x0A];
#else
        uint8_t _00[0x10];
        Vec4 offset;        // +0x10 the bind position: VU1 gets translation - offset * rotation
        uint16_t poseIndex; // +0x20 which of the node's poses drives it
        uint8_t _22[0x0E];
#endif
    };
    static_assert(offsetof(Bone, offset) == game::readable::perGame(0x10u, 0x10u, 0x00u) &&
                  offsetof(Bone, poseIndex) == game::readable::perGame(0x20u, 0x20u, 0x14u));

    // A node's pose of one bone (Node::bonePoses + index * 0xC0): three
    // matrices in a node's order (Node::local, world, inverseWorld). The
    // first two are the bone's BoneMatrices (zge/scene/scene.h).
    struct BonePose
    {
        Matrix local;       // +0x00 relative to the parent bone
        Matrix matrix;      // +0x40 in the model's space: rotation (rows 0-2) and translation (row 3)
        // +0x80 set to the identity with the others (resetBonePose); nothing
        // found in AI, DM2 or BMX XXX reads it or writes it after that (a
        // node keeps its world matrix's inverse here).
        Matrix unread;
    };
    static_assert(sizeof(BonePose) == 0xC0 && offsetof(BonePose, matrix) == 0x40);

    // Per vertex (Part stream +0x20): up to four bones (0: none) with
    // weights in sixteenths; bone 0's weight is 16 minus the others' (its
    // weight byte goes to the shadow alone, in 255ths: posePart).
    struct SkinWeights
    {
        uint8_t bone0, shadowWeight, bone1, weight1, bone2, weight2, bone3, weight3;
    };
    static_assert(sizeof(SkinWeights) == 8);
}
