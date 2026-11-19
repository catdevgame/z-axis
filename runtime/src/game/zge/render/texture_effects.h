#pragma once

// The texture animations of a level (AI's inlineTextureFX.c, BMX XXX's ZGE4
// ZgeMods/UVanim/ztexturefx.c: the engine's, by the rule that a function is
// the engine's when any game's engine library has it; layer2): a flipbook runs
// a material through pictures, a scroller moves its offset.
// texture_effects.cpp makes them from the level's locators and runs them each
// frame.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/texture_effects.h.h"

namespace zge
{
    inline ee::Global<uint32_t, sym::g_scrollers> g_scrollers;  // texture scrollers, the first
    inline ee::Global<uint32_t, sym::g_flipbooks> g_flipbooks;  // texture flipbooks, the first
    inline ee::Global<uint32_t, sym::g_scroller> g_scroller;   // the one being updated, or the last made
    inline ee::Global<uint32_t, sym::g_flipbook> g_flipbook;
    inline ee::Global<uint32_t, sym::g_textureUser> g_textureUser; // the user list's entry being updated
    inline ee::Global<uint32_t, sym::g_textureNode> g_textureNode; // the last of a user's scene nodes

    // A mesh (or material list) a texture animation applies to.
    struct TextureUser
    {
        uint32_t model;         // +0x00 the world model's class
        uint32_t mesh;          // +0x04 (scrollers)
        uint32_t nodes;         // +0x08 a list {object, next}: the scene objects of the class
        uint32_t materials;     // +0x0C (flipbooks) a list {material, next}
        uint32_t next;          // +0x10
    };
    static_assert(sizeof(TextureUser) == 20);

    // The items of a user's lists: its materials (flipbooks) and its scene
    // nodes (the ones that face the camera).
    struct MaterialLink
    {
        uint32_t material;      // +0x00
        uint32_t next;          // +0x04
    };
    struct NodeLink
    {
        uint32_t node;          // +0x00
        uint32_t next;          // +0x04
    };
    static_assert(sizeof(MaterialLink) == 8 && sizeof(NodeLink) == 8);

    // Which way an animation's nodes are turned each frame
    // (faceTexturesToCamera): by the camera's turn alone (its cached matrix
    // 2), or about the vertical only, to face along its view (matrix 3).
    // Anything else leaves them alone.
    enum TextureFacing : uint8_t
    {
        kFaceCamera = 1,
        kFaceCameraUpright = 2,
    };

    struct Flipbook
    {
        uint32_t texture;       // +0x00 the name hash of the material it animates
        uint8_t last;           // +0x04 the last frame
        uint8_t frame;          // +0x05
        uint8_t _06[2];
        ee::Float frameTime;    // +0x08
        ee::Float timer;        // +0x0C
        ee::Float hold;         // +0x10 at the end frames, 0 none
        uint32_t mode;          // +0x14 1 loops, 2 goes back and forth, else stops at the end
        uint32_t rising;        // +0x18 the frames go up (else down)
        uint32_t picture[17];   // +0x1C by frame
        uint32_t users;         // +0x60
        uint32_t next;          // +0x64
    };
    static_assert(offsetof(Flipbook, picture) == 0x1C && offsetof(Flipbook, users) == 0x60 && sizeof(Flipbook) == 104);
    constexpr uint32_t kFlipbookLastPictureByte = 0x5C; // a tag's byte, in the last row of picture
    // A flipbook's facing (TextureFacing): the byte the tag sets at the
    // lowest of its last picture word's.
    inline uint8_t flipbookFacing(const Flipbook &f) { return static_cast<uint8_t>(f.picture[16] & 0xFFu); }

    struct Scroller
    {
        uint8_t kind;           // +0x00 6: scrolls as a node moves
        uint8_t _01[3];
        uint32_t stepped;       // +0x04 a step each period (else a rate a second)
        uint32_t texture;       // +0x08 the name hash of the material it scrolls
        ee::Float du, dv;       // +0x0C
        ee::Float u, v;         // +0x14 the offset
        ee::Float bounceU, bounceV; // +0x1C the offset going back and forth
        ee::Float period;       // +0x24
        ee::Float timer;        // +0x28
        ee::Float wrapU, wrapV; // +0x2C
        uint8_t axisU, axisV;   // +0x34 a node's axis (0 x, 1 y, 2 z) for each
        uint8_t _36[2];
        uint32_t mode;          // +0x38 at a wrap: 0 stop, 1 again after the period, 2 back
        uint8_t facing;         // +0x3C the nodes' facing (TextureFacing), from the tag; faceTexturesToCamera reads it
        uint8_t _3d[3];
        zge::Vec4 lastAt;       // +0x40 the node's position last frame
        uint32_t node;          // +0x50
        uint32_t users;         // +0x54
        uint32_t next;          // +0x58
        uint32_t _5c;
    };
    static_assert(offsetof(Scroller, period) == 0x24 && offsetof(Scroller, axisU) == 0x34);
    static_assert(offsetof(Scroller, lastAt) == 0x40 && offsetof(Scroller, next) == 0x58 && sizeof(Scroller) == 0x60);
    // A scroller's facing (TextureFacing), from its tag.
    inline uint8_t scrollerFacing(const Scroller &s) { return s.facing; }

    // (texture_effects.cpp) The level's texture effects made from its
    // locators, with the task that turns them to the camera each frame; run
    // `seconds` on; freed.
    int32_t initTextureFx();
    void animateTextures(ee::Float seconds);
    int32_t freeTextureEffects();
}
