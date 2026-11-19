#pragma once

// The host's service to the class draws' host form (zge/host/class_draws.cpp): native draws of the meshes the class
// renderers draw, made from their draw records (zge/render/draw.h) instead of the VU1 packets the strict encoder
// writes, each at its place in the GS stream. One boot's state (ps2x::session).
//
// Render path (settings.ini [graphics] render_path, PS2X_RENDER_PATH; the game's override says which at boot,
// attach): `records` draws natively from the records; `native` has the override observe the packets and substitute
// their VU1 work (aggressive_inline_native.cpp, dave_mirra_native.cpp); `strict` sends every mesh through VIF1, VU1
// and the GS. Unset (`auto`, the app's native) is each game's native form: Aggressive Inline's the override on the GPU
// renderer, DM2's and BMX XXX's the records (DM2's with its override for what they do not reach; BMX XXX has no
// override). The Native Meshes row switches records off and on again while the game runs.
//
// A draw from its record leaves out the VU1 work of the draw (the part's packet called, the matrices, bones and
// lights unpacked, the microprogram run): the view's mesh packet gets the draw's texture request and VU1 state as
// ever (what the game reads back: the texture cache's GS addresses set by the VIF1 handler at its marker, the request
// queue, the draw list's owner word), then the draw's anchor, a VIF NOP (PS2Memory::addVif1NativeAnchor). When VIF1
// reaches the anchor the draw's GS state is read (the material's layer as the texture handler has just left it) and
// its triangles go to the GS there (GS::drawNative): on the GPU renderer from vertex buffers, on the CPU renderer as
// GS primitives. Game RAM differs from the other paths only in the mesh packet's bytes.
//
// PS2X_TRACE_NATIVE=1: the counts below every 20,000 draws; 2: also each part left without a mesh or rebuilt from
// changed streams, once, and why.

#include "runtime/gs/gs_native.h"
#include "runtime/ps2_zaxis_mesh.h"

#include <cstdint>
#include <functional>

class PS2Runtime;

namespace game::host::native_draws
{
    // The boot's runtime, whether its class renderers draw from their records (render_path = records), what its
    // rigid packet builder sends and its clipping program draws (ps2x::zaxis::RigidDrawForm) and what its skinned
    // packet builder sends (ps2x::zaxis::SkinnedDrawForm: the skin weights, the material table's order).
    // followInterpolation: a game whose own render path leaves meshes on VU1 that frame interpolation
    // (runtime/gs/gs_interpolate.h) cannot move (DM2's riders, every mesh of BMX XXX): its records drawn whenever
    // interpolation is on, as with render_path = records, unless the Native Meshes row is off (setMeshesOn).
    void attach(PS2Runtime &runtime, bool records, const ps2x::zaxis::RigidDrawForm &rigidDraw,
                const ps2x::zaxis::SkinnedDrawForm &skinnedDraw, bool followInterpolation = false);
    // The game's rigid and skinned packet forms (attach).
    const ps2x::zaxis::RigidDrawForm &rigidDraw();
    const ps2x::zaxis::SkinnedDrawForm &skinnedDraw();
    // Records switched off (the Native Meshes row: strict) and on again.
    void setRecords(bool on);
    // The Native Meshes row on or off (off: no records for interpolation either).
    void setMeshesOn(bool on);
    // Records drawn now: render_path = records, or frame interpolation on (attach's followInterpolation).
    bool recordsWanted();
    // Whether the class renderers draw from their records now: wanted, attached, in the game (not ps2xFnCheck's
    // replay of one function).
    bool fromRecords();

    // ---- a draw's place in the stream --------------------------------------------------------------------

    // The GS side of a draw, run on the GS's thread in stream order.
    using Work = std::function<void()>;
    // Called (EE thread) where VIF1 reaches the draw, after any interrupt handler before it: the draw's GS state read
    // then, its work returned (empty: nothing drawn).
    using Begin = std::function<Work()>;
    // The VIF code to append to the packet where the draw stands (a NOP that names `begin`).
    uint32_t anchorCode(Begin begin);

    // ---- the GS side (in a Work) ---------------------------------------------------------------------------

    // A GIF packet to the GS as VU1 would kick it (a material layer's GS block).
    void gifPacket(const uint8_t *bytes, uint32_t size);
    // A GS register written (A+D).
    void setRegister(uint8_t reg, uint64_t value);
    void draw(const GSNativeDraw &draw);
    // Character Lighting: Xbox (gs_quality), where the renderer draws native draws itself (the CPU renderer's
    // primitives keep VU1's lights), for a game whose Xbox release has the lights (SkinnedDrawForm::xboxLights).
    bool xboxCharacterLighting();

    // ---- counts ---------------------------------------------------------------------------------------------

    enum class Count : uint32_t
    {
        Rigid,          // drawn from the record: rigid prelit
        Sky,
        Terrain,
        Skinned,
        // left to the strict encoder (its VU1 work as ever), by why:
        NoMesh,         // no mesh from the GEO data for the part (no file origin, or not modelled)
        StreamsChanged, // the part's packet was rebuilt from changed streams: its file mesh is not what VU1 draws
        Effect,         // a material routine the host form does not draw
        Lights,         // a lit rigid draw (ZGE4's rigid lights)
        Kind,           // a skinned part whose streams do not match its render type
        kCount
    };
    void count(Count what);
    // A part left to the strict encoder for want of a mesh, and why: PS2X_TRACE_NATIVE=2 prints the first of each.
    void noteNoMesh(uint32_t partAddress, uint32_t renderType, const char *why);
    // A part whose packet was rebuilt from changed streams (the part's rebuild bits): PS2X_TRACE_NATIVE=2 prints it
    // once.
    void noteStreamsChanged(uint32_t partAddress, uint32_t renderType, uint32_t streams);
    // Every draw the host form was given (drawn or left to the strict encoder): an override that wraps a class
    // renderer sees whether the readable one handled the draw.
    uint64_t draws();
    // Native work that found nothing to draw at its anchor.
    void countDeclined();
}
