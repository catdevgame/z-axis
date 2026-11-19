// The effects' materials (inlineFx.c; gamehand15, were the sweep's drafts
// of loadFxMaterial and releaseFxMaterial): a texture of the effects library
// (assets\fx.zal) read by its id into a material of its own, as the
// library's load (initInlineFx), the snow, rain, lightning and lens flare
// make their textures, and a material let go.
//

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/images.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/aggressive/fx/materials.cpp.h"

namespace zge
{
}

namespace aggressive::fx
{
    namespace
    {
        constexpr uint32_t kMaterialName = sym::materialName;        // "MatFx%s"

        // The flags: the image's kind and the material's state.
        enum FxMaterialFlags : uint32_t
        {
            kStateMask = 0x07FFFFFFu,  // register 5's value
            kRegister4 = 0x08000000u,  // register 4 set (0)
            kKindTwo = 0x10000000u,    // the image read as kind 2
            kRegister7 = 0x20000000u,  // register 7 set (0)
            kKindThree = 0x40000000u,  // the reader 3 (else the file's own)
            kRegister3 = 0x80000000u,  // register 3 set (0)
        };
        constexpr uint32_t kImageReaderThree = 3, kImageKindTwo = 2;
        constexpr uint32_t kRegisterState = 5, kRegisterBuild = 0x36;
    }

    // loadFxMaterial (library, texture id, material name, flags, &material):
    // the library's texture of that id read whole into memory, made a
    // texture and a new material with it (*material; the material keeps
    // the texture, its own reference let go). "MatFx" and the name are
    // formatted into the frame and not used (a debug name, cut). Returns
    // freeMemory's.
    int32_t loadFxMaterial(uint32_t library, uint32_t textureId, uint32_t name, uint32_t flags, ee::Ptr<uint32_t> material)
    {
        ee::Local<char[0xB0]> debugName; // "MatFx" and the name (not used)

        // The texture file, read whole (its length rounded up to its
        // alignment; an asset always has one).
        const uint32_t id = zge::nameHash(ee::Ptr<const char>(textureId));
        zge::findAsset(library, id);
        const uint32_t length = zge::currentAssetLength(library);
        const uint32_t alignment = static_cast<uint32_t>(zge::currentAssetAlignment(library));
        const uint32_t blocks = alignment ? (length - 1 + alignment) / alignment : 0;
        const uint32_t memory = zge::allocateMemory(blocks * alignment);
        zge::readAsset(library, memory, 0u);
        zge::formatString(debugName, kMaterialName, name);

        const uint32_t record = zge::newImageRecord();
        if (record)
        {
            const uint32_t fileReader = zge::findImageReader(memory);
            const uint32_t reader = (flags & kKindThree) ? kImageReaderThree : fileReader;
            const uint32_t read = zge::readImage(record, memory, (flags & kKindTwo) ? kImageKindTwo : reader);
            if (read)
            {
                const ee::Ptr<zge::Texture> texture = zge::newProceduralTexture(ee::Ptr<zge::TextureImage>(record));
                if (texture)
                {
                    const ee::Ptr<zge::Material> made = zge::newMaterial();
                    *material = ee::Address(made);
                    if (made)
                    {
                        zge::addMaterialTexture(made, texture);
                        zge::zgui::rebuildMaterialRegisters(made, 0, kRegisterState, flags & kStateMask);
                        zge::setMaterialState(made, 0, kRegisterBuild, 1);
                        if (flags & kRegister3)
                            zge::zgui::rebuildMaterialRegisters(made, 0, 3, 0);
                        if (flags & kRegister4)
                            zge::zgui::rebuildMaterialRegisters(made, 0, 4, 0);
                        if (flags & kRegister7)
                            zge::zgui::rebuildMaterialRegisters(made, 0, 7, 0);
                    }
                    zge::releaseLoadedTexture(texture);
                }
            }
            zge::freeImageRecord(record);
        }
        return zge::freeMemory(memory);
    }
    READABLE_FUNCTION_WAITS(loadFxMaterial, Verified);

    // releaseFxMaterial (&material): the material let go (releaseMaterial's tail
    // call; returns its).
    int32_t releaseFxMaterial(ee::Ptr<uint32_t> material)
    {
        return game::tail(zge::releaseMaterial, *material);
    }
    READABLE_FUNCTION_WAITS(releaseFxMaterial, Verified);
}
