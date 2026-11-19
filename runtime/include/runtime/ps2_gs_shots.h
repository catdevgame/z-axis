#pragma once

// Pictures at given vsyncs, the same in any two runs of the same input (developers: comparing what two render paths
// draw, docs/DEVTOOLS.md). PS2X_GS_SHOT_VSYNC="T,T,...:PREFIX": at each vsync tick T the display the EE has set is
// noted, and once the GS has drawn everything sent before that vsync (a GS sideband, in stream order) its picture is
// written to PREFIX-vT.png. The CPU renderer's pictures (the GPU renderer keeps them on the GPU: none written).

class PS2Runtime;

namespace ps2x::gs_shots
{
    // Installs the vsync hook if PS2X_GS_SHOT_VSYNC is set (once a boot).
    void install(PS2Runtime &runtime);
}
