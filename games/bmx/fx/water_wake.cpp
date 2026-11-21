// BMX XXX's water wake (water_wake.h). BMX XXX's own code (fx.c's block);
// DM2 has the same pair.

#include "water_wake.h"
#include "library.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/scene/effects.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"

#include <cstdint>
#include "sym/bmx/fx/water_wake.cpp.h"

namespace game::bmx
{
    namespace
    {
        inline ee::Global<WaterWake, sym::g_waterWake> g_waterWake;
        // The wake's effect class (the game's table of its methods).
        inline const ee::Ptr<zge::EffectClass> kWakeClass(sym::waterWakeEffectClass);

        constexpr uint32_t kFxLibrary = sym::fxLibraryPath;         // "assets\\fx.zal"
        constexpr uint32_t kWakeAsset = sym::waterWakeAssetName;    // "Water.Wake.00"
        constexpr uint32_t kWakeMaterial = sym::waterWakeMaterialName; // "WaterWakes"
        constexpr uint32_t kWakeName = sym::waterWakeName;          // "WaterWake"
        constexpr uint32_t kBlendOne = 1;

        namespace calls
        {
        // The original's tail jump to the library's close.
        void closeZfileLibraryTail(ee::Ptr<zge::ZfileLibrary> library) { ee::tailCall<void>(sym::closeZfileLibrary, library, 0u); }
        }
    }

    // initWaterWake: nothing when it is set up already. The library is
    // opened again for the one texture.
    void initWaterWake()
    {
        if (g_waterWake->ready)
            return;
        g_waterWake->ready = 1;
        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kFxLibrary);
        loadFxMaterials(library, ee::pointerAt<char>(kWakeAsset), kWakeMaterial, 1, kFxKindThree | kBlendOne,
                        ee::Ptr<uint32_t>(&g_waterWake->material), 0, 0);
        g_waterWake->emitter = zge::newSceneEmitter(0);
        zge::registerEffectClassName(kWakeClass, kWakeName);
        calls::closeZfileLibraryTail(library);
    }
    READABLE_FUNCTION_WAITS(initWaterWake, Verified);

    // freeWaterWake: the class's name unregistered, the material let go, and
    // the wake no longer set up.
    void freeWaterWake()
    {
        zge::unregisterEffectClassName(kWakeName);
        releaseFxMaterials(ee::Ptr<uint32_t>(&g_waterWake->material), 1);
        g_waterWake->ready = 0;
    }
    READABLE_FUNCTION_WAITS(freeWaterWake, Verified);
}
