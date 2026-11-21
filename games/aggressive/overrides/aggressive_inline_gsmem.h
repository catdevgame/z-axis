#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The GS texture memory audit (ZX_GSMEM_AUDIT=1, developers; off by
    // default, nothing hooked): every texture cache allocation past the
    // CLUTs or the end of GS memory, counted per level, with the peak block
    // (runtime/gs_texture_audit.h). See aggressive_inline_gsmem.cpp.
    void installGsTextureAudit(PS2Machine &runtime);
}
