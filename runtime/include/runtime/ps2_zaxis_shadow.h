#pragma once

// Aggressive Inline's shadow volumes on the host: the packet the game builds
// for one (src/game/zge/render/shadow.h, buildShadowVolumePacket
// FUN_0010DBC8) read back from guest memory, and what VU1's program 0x1C
// (0x1FD0 .. 0x2268 of the resident image) kicks to the GS for it, so a game
// override can have a volume of its own silhouette drawn in its place
// (PS2Memory::setVif1Substitute) exactly as VU1 would draw it.
//
// The packet (a RET chain; VIF1 with the tag's upper half as VIFcodes):
//   UNPACK V4-32 at TOPS: a GIF packet (the volume's GS state: FRAME_2,
//   ZBUF_2, COLCLAMP 0, ALPHA_2), MSCAL 0x24 (XGKICK it), FLUSHA, ITOP 0;
//   UNPACK at 1: the light (the part's space), the RGBAQ tag and colour 0,
//   the tag and colour 1, the triangle tag (PRIM 0x244, three XYZ2), the
//   model -> camera matrix (4 rows), the camera -> screen matrix (4 rows);
//   UNPACKs from 0x20: the silhouette, two points per edge;
//   UNPACK at 0: the near clip, the point count, the push (1.25), 0;
//   MSCAL 0x1C, FLUSHA; then the GS state put back (UNPACK at TOPS, MSCAL
//   0x24).
//
// VU1 0x1C, per edge (A, B): A' = A + scale (A - L), B' likewise; L, A', B'
// to the camera (rows: w = 1), z at least the near clip, to the screen,
// divided by w, x y z at least 0, x y at most 4095, FTOI4 (w the float 1.0);
// the triangle (L, A', B') kicked with the triangle tag in the first pass if
// its screen winding (A' - B') x (L - B') has z < 0, else in the second; the
// first pass after the RGBAQ tag and colour 0, the second after colour 1.

#include "runtime/ps2_zaxis_mesh.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace zx::zaxis
{
    struct ShadowVolumePacket
    {
        std::vector<uint8_t> before, after; // the GS state blocks (the MSCAL 0x24 kicks), GIF packets
        uint32_t itop = 0;
        float nearClip = 0.0f, scale = 0.0f;
        uint32_t pointCount = 0;
        float light[4] = {};
        uint8_t colourTag[16] = {}, colour[2][16] = {}, triangleTag[16] = {};
        float view[4][4] = {};   // the part's space -> camera
        float screen[4][4] = {}; // camera -> GS screen (x, y, z, w)
        std::vector<std::array<float, 4>> points; // the game's silhouette
    };
    // `chain`: the address the frame packet's CALL goes to (what
    // buildShadowVolumePacket returned). False (with why) for anything else
    // than the packet above.
    bool readShadowVolumePacket(const GuestMemory &memory, uint32_t chain, ShadowVolumePacket &out,
                                std::string *why = nullptr);
    // The GIF packets VU1 0x1C kicks for the packet's parameters and these
    // points (edge pairs, the part's space), appended to `gif`. Returns the
    // triangles.
    size_t shadowVolumeGif(const ShadowVolumePacket &packet, const std::vector<std::array<float, 3>> &points,
                           std::vector<uint8_t> &gif);
    // The byte length of the GIF packet at `data` (tags up to EOP); 0 if it
    // does not fit in `size`.
    size_t gifPacketBytes(const uint8_t *data, size_t size);
}
