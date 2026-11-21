#pragma once

// Native draws: meshes a game hands the GS whole, from host memory, instead
// of the vertices its VU1 microprograms would compute and kick one by one.
//
// A native draw stands in the GS stream exactly where the VU1 work it
// replaces would have drawn, with the GS state of that point (the frame and Z
// buffers, scissor, texture, blending and tests of the active context, set by
// the game's own GS packets), so it orders and depth-tests against every
// GS-path draw around it. What a native draw adds is the geometry: a
// triangle list kept on the host (and on the GPU, uploaded once) and the
// object-to-screen transform of the draw.
//
// Producers: game overrides that observe the game's renderer and substitute
// its VU1 work in the VIF1 stream (PS2Memory::setVif1Substitute; Aggressive
// Inline: game_overrides/aggressive_inline_native.cpp, meshes decoded by
// runtime/zaxis_mesh.h). Consumers: GS::drawNative, then
// GSRasterBackend::SubmitNative (the GPU renderer draws it with a vertex
// shader); a backend that does not take it gets the same triangles as GS
// primitives, transformed on the CPU as the VU1 program would (the CPU
// renderer, or a draw the GPU renderer cannot do in one go).

#include <cstdint>
#include <memory>
#include <vector>

// One vertex of a native mesh, as the GS needs it.
struct GSNativeVertex
{
    float pos[3] = {};      // object space
    float st[2] = {};       // texture coordinates, 1.0 = the texture's size (the GS's S and T before the divide by w)
    uint8_t rgba[4] = {};   // the GS vertex colour (0x80 = 1.0 for MODULATE)
    float stBounds[4] = {}; // the triangle's S/T range: min s, min t, max s, max t (texel clamping when upscaled)
    int16_t normal[4] = {}; // object space, 32768 = 1.0 (GSNativeDraw::normalOffset); w unused
};
static_assert(sizeof(GSNativeVertex) == 48, "GSNativeVertex layout (vertex buffers, shaders)");

// A skinned vertex's bones (AI's VU1 0x08 / 0x0C programs): up to four, each
// the VU1 address of its matrix in the bone palette (GSNativeSkinning) and a
// weight in sixteenths. A vertex is moved by sum(weight / 16 * [x y z 1] *
// bone), its normal turned by the first bone's rotation only.
struct GSNativeSkinWeights
{
    uint8_t bone[4] = {};   // palette row of the bone's first row (0..124)
    uint8_t weight[4] = {}; // sixteenths
};
static_assert(sizeof(GSNativeSkinWeights) == 8, "GSNativeSkinWeights layout (vertex buffers, shaders)");

// A mesh: a triangle list (three vertices per triangle, not indexed, each
// carrying its triangle's S/T range), split in runs that are drawn with
// different GS state (a game's materials).
struct GSNativeMesh
{
    uint64_t id = 0;                     // unique for the process (GPU caches)
    std::vector<GSNativeVertex> vertices;
    // Skinned meshes: one per vertex (empty: a rigid mesh).
    std::vector<GSNativeSkinWeights> skin;
    float boundsMin[3] = {};             // object-space box of all vertices
    float boundsMax[3] = {};
    // A label for logs (the game's own name for the mesh, an address).
    uint32_t source = 0;

    static uint64_t nextId();
};

// How the GS-path microprogram that a native draw replaces treats what the
// GS cannot take, so a CPU fallback matches it.
enum class GSNativeClip : uint8_t
{
    // Coordinates as computed (the GS gets them; the GPU clips them).
    None = 0,
    // Vertices behind the near plane cut off (a clipping microprogram).
    Near = 1,
    // The rigid clipping programs of ZGE3 and ZGE4 (AI's VU1 0x06, GSNativeDraw::clipTransform the clip volume):
    // a triangle with all three vertices beyond one side of the volume (+x, -x, +y, -y or the near side) is left
    // out; one with a vertex outside it is cut at the near plane (w' = nearW), and every triangle drawn, cut or
    // not, is split in four at its edges' midpoints (position, S/T and colour halfway in object space, the colour
    // truncated). The GPU draws the same surface uncut: this is the form of its triangles on the CPU (the PS2's
    // picture: X and Y clamped to the GS window per vertex bend big triangles' textures by their shape).
    NearSplit = 2,
    // ZGE2's (DM2's VU1 0x06): the same, but a triangle wholly beyond the far side is left out too, only one with
    // a vertex beyond the near side is cut, and a triangle is walked from its last vertex back.
    NearSplitAndFar = 3,
};

// The per-draw data of a skinned mesh, as AI's skinned programs (VU1 0x08 /
// 0x0C) get it from the node's packet (FUN_0010C250).
struct GSNativeSkinning
{
    static constexpr int kPaletteRows = 128;
    // VU1 memory 0..127: bone k's matrix at rows 4k..4k+3 (xyz: the
    // rotation's rows, then the translation; w ignored).
    float palette[kPaletteRows][4] = {};
    // Colours from three directional lights and an ambient (VU1 0x08):
    // c = min(255, 128 * (ambient + sum_i max(0, n . dir_i) * colour_i)),
    // truncated, alpha 0x80; n the normal turned by the first bone. Else
    // (0x0C) the vertex colours, alpha 0x80.
    bool lit = false;
    float lightDir[3][4] = {};    // row k: component k of lights 0, 1, 2 (x, y, z)
    float lightColour[4][4] = {}; // lights 0, 1, 2, the ambient (r, g, b; 1.0 = 0x80)
    // The cap of 128 * (...) above: 255 as VU1 (up to twice the texture),
    // 128 as the Xbox (its vertex colours saturate at 1.0).
    float maxColour = 255.0f;
    // Lit, the alpha lit too (DM2's VU1 0x118): 128 * (ambient + sum_i
    // max(0, n . dir_i) * colour_i) of the colours' w, not capped,
    // truncated, its low byte; else 0x80.
    bool alphaFromLights = false;
};

// A skinned vertex as the programs compute it: its position (object space
// before GSNativeDraw::transform) and, lit, its colour.
void gsNativeSkinVertex(const GSNativeSkinning &skin, const GSNativeVertex &v, const GSNativeSkinWeights &w,
                        float pos[3], uint8_t rgba[4]);

// A rigid mesh lit from its vertex normals (GSNativeVertex::normal, object
// space, 32768 = 1.0) by three directional lights and an ambient instead of
// drawn with its vertex colours (BMX XXX's rigid programs, VU1 0x04 / 0x06,
// given a node's own lights: routines 0xC80 / 0x1F50): c = min(255, scale *
// (ambient + sum_i max(0, n . dir_i) * colour_i)), truncated (FTOI0; the GS
// keeps the low byte), and a constant alpha.
struct GSNativeLights
{
    float lightDir[3][4] = {};    // row k: component k of lights 0, 1, 2 in object space (x, y, z)
    float lightColour[4][4] = {}; // lights 0, 1, 2, the ambient (r, g, b)
    // The lit colour's scale: BMX XXX's programs 255 (1.0 lights a vertex
    // 0xFF, twice the texture).
    float scale = 255.0f;
    // The vertex alpha: BMX XXX's programs leave the lighting's w the float
    // 2.08e11 (0x5241E0C5), whose low byte the GS takes.
    uint8_t alpha = 0xC5;
};
// A lit rigid vertex's colour.
void gsNativeLightVertex(const GSNativeLights &lights, const int16_t normal[3], uint8_t rgba[4]);

// The VU's CLIPw.xyz flags of a clip-space position c (GSNativeDraw::dropFlags),
// against |max(c.w, 0)| as AI's programs give it (MAXx.w first).
inline uint8_t gsNativeClipFlags(const float c[4])
{
    const float w = c[3] > 0.0f ? c[3] : 0.0f;
    uint8_t f = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (c[i] > w)
            f |= static_cast<uint8_t>(1u << (2 * i));
        if (c[i] < -w)
            f |= static_cast<uint8_t>(2u << (2 * i));
    }
    return f;
}

// What frame interpolation (gs/gs_interpolate.h) needs of a native
// draw: whose it is, so the same node's draw in the next frame is found, and
// its two matrices, so it can be drawn between the two frames' places. The
// draw's transform is world x (the camera's world to GS window matrix, with
// the program's own changes to its columns: Z times 16, the sky's Z), and
// its clipTransform world x the camera's clip matrix.
struct GSNativeMotion
{
    // The node (its game address) and the view drawn (split screen): a node
    // has one world matrix and one camera per view in a frame, whatever part
    // (LOD) or material run of it is drawn. 0: no identity (drawn where this
    // frame has it).
    uint64_t key = 0;
    float world[4][4] = {};  // object -> world (affine)
    float camera[4][4] = {}; // the view matrix: world -> camera (rigid)
};
inline uint64_t gsNativeMotionKey(uint32_t node, uint32_t view)
{
    return node == 0u ? 0u : (static_cast<uint64_t>(node) << 8) | (view & 0xFFu);
}

// A draw of part of a mesh.
struct GSNativeDraw
{
    std::shared_ptr<const GSNativeMesh> mesh;
    uint32_t first = 0; // first vertex (a multiple of 3)
    uint32_t count = 0; // vertices (a multiple of 3)
    // Object space to GS window space, row vectors: [x y z 1] * transform =
    // (x', y', z', w'), then X = x'/w', Y = y'/w' (GS window pixels, the
    // context's XYOFFSET not yet subtracted), Z = z'/w' (the GS depth value,
    // clamped at 0 as the VU1 program does), and the GS's STQ = (s/w', t/w',
    // 1/w'). w' is the view-space depth.
    float transform[4][4] = {};
    // w' below which a vertex is cut off (Near: the clipper's near plane).
    float nearW = 0.0f;
    GSNativeClip clip = GSNativeClip::None;
    // Colours interpolated in perspective (as 3D surfaces should be) rather
    // than linearly on screen (as the GS does). AI's clipping program
    // splits what crosses the near plane at the midpoints of the clipped
    // edges, which gives such surfaces nearly perspective colours there.
    bool perspectiveColour = false;
    // Diagnostics: drawn as GS primitives even by a backend that takes
    // native draws (GS::drawNative's own path).
    bool asPrimitives = false;
    // Per-draw vertex changes (AI's terrain program, VU1 0x20: its grass
    // shells and scrolling UVs):
    // - every vertex moved by normal * normalOffset (object space) first;
    float normalOffset = 0.0f;
    // - the vertex alpha replaced by this (-1: the vertices' own);
    int alpha = -1;
    // - S, T replaced by i0 * uv[0] + i1 * uv[1] + i2 * uv[2] + uv[3] when
    //   uvTransform, where (i0, i1, i2) is (s, t, 1) (the program's routine
    //   0x581, a UV matrix) or, with uvFromNormal, the normal (x, y, z) /
    //   32768 (routine 0x592, sphere-mapped UVs).
    bool uvTransform = false;
    bool uvFromNormal = false;
    float uv[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    // Triangles the microprogram leaves out whole (AI's sky and skinned
    // programs: no clipping, a triangle with any vertex outside a volume is
    // not drawn): VU CLIP flags of c = [x y z 1] * clipTransform against
    // |max(c.w, 0)|, bit 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z; a triangle is
    // dropped if any of its vertices has a flag of dropFlags (0: none).
    uint8_t dropFlags = 0;
    float clipTransform[4][4] = {};
    // X and Y clamped to the GS window, 0..4095 (AI's sky program).
    bool clampXY = false;
    // STQ = (s, t, 1): textures mapped affinely on screen (AI's skinned
    // programs give Q = 1.0, the ROW register's z).
    bool affineUV = false;
    // A skinned mesh's bones and lights (the mesh has GSNativeMesh::skin).
    std::shared_ptr<const GSNativeSkinning> skin;
    // A rigid mesh's lights: its colours lit from its normals (null: its
    // vertex colours).
    std::shared_ptr<const GSNativeLights> lights;
    // The FOG register where it is drawn (set by GS::drawNative): the VU1
    // programs write XYZ2, so every vertex has that fog.
    uint8_t fog = 0;
    // Frame interpolation's identity and matrices (key 0: none).
    GSNativeMotion motion;
};

// A game's depth fog drawn as planes (AI: FUN_001110C0, eight flat
// full-viewport planes of the fog colour at evenly spaced view distances,
// each blended over what lies behind it; GS::noteFogPlanes). A backend
// that knows where the planes are can draw the fog per pixel from the
// depth buffer instead (smooth, without the bands), and knows that the 3D
// picture of that view is finished there (anti-aliasing before the HUD).
struct GSFogPlanes
{
    int count = 8;        // planes, at nearest + k * step (k = 0 .. count - 1)
    float nearest = 0.0f; // view distance of the nearest plane
    float step = 0.0f;
    // The view's depth mapping as the projection gives it: GS Z = zA / w +
    // zB at view distance w (a backend may fit it to the planes' Z instead).
    float zA = 0.0f;
    float zB = 0.0f;
};
