#pragma once

// Xbox models for the PS2 game, made at build time (zx-native-assets
// --xbox-models, run by the packaging step next to tools/modding/xbox_pack.py; the
// running game only loads the result).
//
// The Xbox release's skaters are more detailed than the PS2's (up to twice
// the vertices) and share the PS2's skeletons and texture pictures (larger).
// A model file (.ZIT: FMDL > MODL > TXRC textures, one TGEO, TGIN) is made
// from the PS2 file with its TGEO taken from the Xbox file:
//   - the Xbox TGEO converted to the PS2's form (runtime/zaxis_xbox.h
//     convertGeometry: strips cut into the PS2 renderer's 60-entry groups,
//     materials' render state as the PS2 exporter wrote it);
//   - its materials' texture ids changed to the PS2 library's, by the pairs
//     tools/modding/xbox_pack.py makes (texture_library.h pairTextures: the same
//     names, sizes dropped), so the PS2 textures (and the pack's Xbox
//     pictures for them) stay as they are;
//     a pair the pack refuses the Xbox picture of (a colour key drawn
//     opaque, pictures that differ) still pairs by name, with the PS2
//     picture;
//   - its strips made again from the triangles (strips.h: the converted
//     D3D strips keep every degenerate joint as an entry that draws
//     nothing, about four entries a triangle), checked to draw the same
//     triangles in the same winding;
//   - the PS2 file's textures and TGIN kept.
// A model is left out (and why reported) if its object or part counts, or
// its bones, differ, a material names a texture with no PS2 pair, or a
// material's VIF packet would be more than one DMA tag sends. (The PS2's
// packet builder buildPartPacket builds a first packet in a fixed 160 KB
// buffer, which the larger Xbox skaters overrun; the readable one,
// zge/render/packet.h, sizes it to the part.)

#include <cstdint>
#include <string>
#include <vector>

namespace zge::native
{
    struct XboxModelReport
    {
        std::string why;          // empty: made
        uint32_t ps2Vertices = 0, xboxVertices = 0;
        uint32_t materials = 0, remapped = 0;
        uint32_t largestPacket = 0;  // bytes of the largest part's VIF packet (zge/render/ps2/packet_size.h)
        uint32_t largestEntries = 0; // strip entries of the part with the most
        // The same with the strips as converted, before strips.h made them
        // again, and the triangles they draw (the same before and after).
        uint32_t packetBefore = 0, entriesBefore = 0, triangles = 0;
        uint32_t ps2Packet = 0; // the PS2 model's largest packet
        // Textures paired by name whose Xbox picture the pack leaves out
        // (and why): the model draws them with the PS2 picture.
        std::vector<std::string> ps2Pictures;
    };

    // `relative`: the file below the assets folder (e.g. "skaters/agrub/ag_zit.zit").
    bool makeXboxModel(const std::string &relative, const std::vector<uint8_t> &ps2, const std::vector<uint8_t> &xbox,
                       std::vector<uint8_t> &out, XboxModelReport &report);
}
