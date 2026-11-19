// DM2's effects library (library.h). DM2's own code (fx.c); Aggressive
// Inline's loadFxMaterial (game/aggressive/fx/materials.cpp) is the same
// lineage, one texture a call.

#include "library.h"
#include "fx_manager.h"
#include "lens_flare.h"
#include "tree_fade.h"

#include "dm2/camera/camera.h"
#include "dm2/level/level_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/camera/scene_camera.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/images.h"
#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/platform/tasks.h"
#include "zge/scene/camera.h"
#include "zge/scene/effects.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include <cstring>
#include "sym/dm2/fx/library.cpp.h"

namespace zge
{
    uint32_t findZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, uint32_t group, uint32_t asset); // dm2/zfile_library.cpp
}

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kMaterialNameFormat = sym::fxMaterialNameFormat; // "MatFx%s%02d"
        constexpr uint32_t kImageKindTwo = 2;
        constexpr uint32_t kRegisterState = 5, kRegisterBuild = 0x33;

        // The depth range script being played (0: none): pairs of signed
        // bytes, a pair a frame, ended by kScriptEnd.
        inline ee::Global<ee::Ptr<int8_t>, sym::g_depthRangeScript> g_depthRangeScript;
        constexpr uint32_t kBuiltInScript = sym::depthRangeScript;
        constexpr int8_t kScriptEnd = -0x80;
        constexpr ee::Float kScriptUnit = 0.005f;

        // Frames the effects' second task has run.
        inline ee::Global<uint32_t, sym::g_effectsFrames> g_effectsFrames;
    }

    // "MatFx", the name and the texture's number are formatted into the
    // frame and not used (a debug name, cut).
    void loadFxMaterials(ee::Ptr<zge::ZfileLibrary> library, uint32_t firstGroup, uint32_t name, uint32_t count, uint32_t flags,
                         ee::Ptr<uint32_t> materials)
    {
        ee::Local<char[0x40]> debugName;
        for (uint32_t index = 0; index < count; ++index)
        {
            zge::findZfileLibraryAsset(library, firstGroup + index, 0);
            const uint32_t memory = zge::allocateMemory(zge::zfileLibraryAssetBytes(library));
            zge::readZfileLibraryAsset(library, memory);
            zge::formatString(debugName, kMaterialNameFormat, name, index);
            const uint32_t record = zge::newImageRecord();
            if (record)
            {
                const uint32_t fileReader = zge::findImageReader(memory);
                const uint32_t read = zge::readImage(record, memory, (flags & kFxKindTwo) ? kImageKindTwo : fileReader);
                if (read)
                {
                    const ee::Ptr<zge::Texture> texture = zge::newProceduralTexture(ee::Ptr<zge::TextureImage>(record));
                    if (texture)
                    {
                        const ee::Ptr<zge::Material> made = zge::newMaterial();
                        materials[index] = ee::Address(made);
                        if (made)
                        {
                            zge::addMaterialTexture(made, texture);
                            zge::zgui::rebuildMaterialRegisters(made, 0, kRegisterState, flags & kFxStateMask);
                            zge::setMaterialState(made, 0, kRegisterBuild, 1);
                            if (flags & kFxRegister3)
                                zge::zgui::rebuildMaterialRegisters(made, 0, 3, 0);
                            if (flags & kFxRegister7)
                                zge::zgui::rebuildMaterialRegisters(made, 0, 7, 0);
                        }
                        zge::releaseLoadedTexture(texture);
                    }
                }
                zge::freeImageRecord(record);
            }
            zge::freeMemory(memory);
        }
    }
    READABLE_FUNCTION_WAITS(loadFxMaterials, Verified);

    void releaseFxMaterials(ee::Ptr<uint32_t> materials, uint32_t count)
    {
        for (uint32_t index = 0; index < count; ++index)
            zge::releaseMaterial(materials[index]);
    }
    READABLE_FUNCTION_WAITS(releaseFxMaterials, Verified);

    uint32_t depthRangeScriptFrame()
    {
        ee::Local<CameraState> state;
        copyCameraState(state, 0);
        if (!state->node)
            return 1;
        const ee::Ptr<zge::Camera> camera = objectCamera(state->node);
        const ee::Ptr<int8_t> script = *g_depthRangeScript;
        if (!script)
            return 1;
        const int8_t first = script[0];
        if (first == kScriptEnd)
        {
            zge::setSceneDepthRange(camera, 0.0f, 0.0f);
            *g_depthRangeScript = {};
        }
        else
        {
            const int8_t second = script[1];
            *g_depthRangeScript = script + 2;
            zge::setSceneDepthRange(camera, ee::toFloat(first) * kScriptUnit, ee::toFloat(second) * kScriptUnit);
        }
        return 1;
    }
    READABLE_FUNCTION(depthRangeScriptFrame, Verified);

    void startDepthRangeScript()
    {
        *g_depthRangeScript = ee::Ptr<int8_t>(kBuiltInScript);
    }
    READABLE_FUNCTION(startDepthRangeScript, Draft);

    uint32_t countEffectsFrame()
    {
        *g_effectsFrames = *g_effectsFrames + 1;
        return 1;
    }
    READABLE_FUNCTION(countEffectsFrame, Verified);
    namespace
    {
        // The level's lens flare (made at the first placing; 0: none).
        inline ee::Global<ee::Ptr<LensFlare>, sym::g_levelLensFlare> g_levelLensFlare;

        // The flare's textures in the effects library: their groups, names
        // and flags.
        constexpr uint32_t kLensGlowGroup = 0x18, kLensRing1Group = 0x19, kLensRing2Group = 0x1A, kLensRayGroup = 0x1B,
                           kLensStarGroup = 0x1C;
        constexpr uint32_t kLensGlowName = sym::lensGlowName;   // "LensGlow"
        constexpr uint32_t kLensRing1Name = sym::lensRing1Name; // "LensRing1"
        constexpr uint32_t kLensRing2Name = sym::lensRing2Name; // "LensRing2"
        constexpr uint32_t kLensRayName = sym::lensRayName;     // "LensRay"
        constexpr uint32_t kLensStarName = sym::lensStarName;   // "LensStar"
        constexpr uint32_t kFlareTexture = 0xC0000002u, kFlareRingTexture = 0xC0000001u;
        // The glow's two material states set on.
        constexpr uint32_t kGlowStateA = 0x13, kGlowStateB = 0x14;
    }

    // placeLevelLensFlare: where the level has a "LensFlare" point, the
    // level's lens flare (made with its textures from the effects library
    // the first time) is put there in the point's colour, and its elements
    // are laid out.
    void placeLevelLensFlare(ee::Ptr<zge::ZfileLibrary> library)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<uint32_t> colour;
        if (!lensFlarePoint(position, colour))
            return;
        if (!*g_levelLensFlare)
        {
            const ee::Ptr<LensFlare> made = newManagedFx(kManagedLensFlare);
            *g_levelLensFlare = made;
            loadFxMaterials(library, kLensGlowGroup, kLensGlowName, 1, kFlareTexture, &made->glow);
            loadFxMaterials(library, kLensRing1Group, kLensRing1Name, 1, kFlareRingTexture, &made->ring1);
            loadFxMaterials(library, kLensRing2Group, kLensRing2Name, 1, kFlareRingTexture, &made->ring2);
            loadFxMaterials(library, kLensRayGroup, kLensRayName, 1, kFlareRingTexture, &made->ray);
            loadFxMaterials(library, kLensStarGroup, kLensStarName, 1, kFlareTexture, &made->star);
            zge::setMaterialState(ee::Ptr<zge::Material>(made->glow), 0, kGlowStateA, 1);
            zge::setMaterialState(ee::Ptr<zge::Material>(made->glow), 0, kGlowStateB, 1);
        }
        const ee::Ptr<LensFlare> flare = *g_levelLensFlare;
        flare->position = *position;
        std::memcpy(flare->colour, &*colour, sizeof(flare->colour));
        setupLensFlareElements(flare);
    }
    READABLE_FUNCTION_WAITS(placeLevelLensFlare, Verified);
}

namespace game::dm2
{
    namespace
    {
        // Set while the effects are set up.
        inline ee::Global<uint32_t, sym::g_inlineFxReady> g_inlineFxReady;
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_effectsClockMs;
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_effectsObject> g_effectsObject;
        // The effects' two tasks.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_depthRangeTask> g_depthRangeTask;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_effectsFrameTask> g_effectsFrameTask;

        constexpr uint32_t kFxLibraryPath = sym::fxLibraryPath; // "assets\fx.zal"
        constexpr uint32_t kFxSourceFile = sym::fxSourceFile;   // "./src/com/fx.c"
        constexpr ee::Ptr<const char> kFxProgressName{sym::fxProgressName}; // "FX"
        constexpr uint32_t kFxProgressSteps = 7;

        // The effect classes and the names they are registered under, in the
        // set-up's order.
        struct EffectClassRow
        {
            uint32_t effectClass;
            uint32_t name;
        };
        constexpr EffectClassRow kEffectClasses[] = {
            {sym::lightningEffect, sym::lightningName},     // "lightning"
            {sym::sparkEffect, sym::sparkName},             // "spark"
            {sym::shardEffect, sym::shardName},             // "shard"
            {sym::screenShardEffect, sym::screenShardName}, // "screenshard"
            {sym::splashEffect, sym::splashName},           // "splash"
            {sym::dustEffect, sym::dustName},               // "dust"
            {sym::posterEffect, sym::posterName},           // "poster"
            {sym::rippleEffect, sym::rippleName},           // "ripple"
            {sym::sleepEffect, sym::sleepName},             // "sleep"
        };

        // The two affector types.
        constexpr uint32_t kGravityName = sym::gravityName;             // "gravity"
        constexpr uint32_t kScreenGravityName = sym::screenGravityName; // "gravity_screen"
        constexpr uint32_t kGravityAffector = sym::gravityAffector;
        constexpr uint32_t kScreenGravityAffector = sym::screenGravityAffector;

        // The named effects' starters: the poster's is also "skid" and "pockmark".
        struct StarterRow
        {
            uint32_t starter;
            uint32_t name;
        };
        constexpr StarterRow kStarters[] = {
            {sym::startSparkEffect, sym::sparkName},
            {sym::startShardEffect, sym::shardName},
            {sym::startScreenShardEffect, sym::screenShardName},
            {sym::startSplashEffect, sym::splashName},
            {sym::startLightningEffect, sym::lightningName},
            {sym::startDustEffect, sym::dustName},
            {sym::startPosterEffect, sym::posterName},
            {sym::startPosterEffect, sym::skidName},     // "skid"
            {sym::startPosterEffect, sym::pockmarkName}, // "pockmark"
            {sym::startRippleEffect, sym::rippleName},
            {sym::startSleepEffect, sym::sleepName},
        };

        // Which classes fall, and how: asked through the spark class's own method.
        constexpr EffectClassRow kFallingClasses[] = {
            {sym::sparkEffect, kGravityName},
            {sym::splashEffect, kGravityName},
            {sym::shardEffect, kGravityName},
            {sym::screenShardEffect, kScreenGravityName},
        };

        // The effects' textures: where in the library, the debug name, how
        // many, loadFxMaterials' flags, and where the materials are kept.
        struct MaterialRow
        {
            uint32_t firstGroup;
            uint32_t name;
            uint32_t count;
            uint32_t flags;
            uint32_t materials;
        };
        constexpr MaterialRow kMaterials[] = {
            {1, sym::pockMarkMaterialName, 2, kFxKindTwo | 1, sym::g_pockMarkMaterials},             // "PockMark"
            {5, sym::sparkMaterialName, 1, kFxRegister3 | 2, sym::g_sparkMaterial},                  // "Spark"
            {6, sym::dustMaterialName, 0x10, kFxRegister3 | 2, sym::g_dustMaterials},                // "Dust"
            {0x16, sym::skidMaterialName, 1, 3, sym::g_skidMaterial},                                // "Skid"
            {3, sym::sleepMaterialName, 2, kFxKindTwo | kFxRegister7 | 2, sym::g_sleepMaterial},     // "Sleep"
        };

        // What the shutdown takes out of the class names: the nine, and
        // "glow", which the set-up never puts in.
        constexpr uint32_t kGlowName = sym::glowName; // "glow"

        // The water wake: its own class and texture, set up once.
        inline ee::Global<uint32_t, sym::g_waterWakeReady> g_waterWakeReady;
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_waterWakeObject> g_waterWakeObject;
        constexpr uint32_t kWaterWakeEffect = sym::waterWakeEffect;
        constexpr uint32_t kWaterWakeName = sym::waterWakeName;                 // "WaterWake"
        constexpr uint32_t kWaterWakeMaterialName = sym::waterWakeMaterialName; // "WaterWakes"
        constexpr uint32_t kWaterWakeGroup = 0x17;

        void fxProgress(uint32_t step)
        {
            zge::chunkProgressHook()(kFxProgressName, step, kFxProgressSteps);
        }
    }

    void initWaterWake()
    {
        if (*g_waterWakeReady)
            return;
        *g_waterWakeReady = 1;
        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kFxLibraryPath);
        loadFxMaterials(library, kWaterWakeGroup, kWaterWakeMaterialName, 1, kFxKindTwo | 1, ee::Ptr<uint32_t>(sym::g_waterWakeMaterial));
        *g_waterWakeObject = zge::newSceneEmitter(0);
        zge::registerEffectClassName(ee::Ptr<zge::EffectClass>(kWaterWakeEffect), kWaterWakeName);
        game::tail(zge::closeZfileLibrary, library);
    }
    READABLE_FUNCTION_WAITS(initWaterWake, Verified);

    void freeWaterWake()
    {
        zge::unregisterEffectClassName(kWaterWakeName);
        releaseFxMaterials(ee::Ptr<uint32_t>(sym::g_waterWakeMaterial), 1);
        *g_waterWakeReady = 0;
    }
    READABLE_FUNCTION_WAITS(freeWaterWake, Verified);

    void initInlineFx()
    {
        *g_inlineFxReady = 1;
        *g_effectsClockMs = 0.0f;
        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kFxLibraryPath);
        zge::initEffectTables();
        initFxManager();
        *g_effectsObject = zge::newSceneEmitter(0);

        for (const EffectClassRow &row : kEffectClasses)
            zge::initEffectClass(ee::Ptr<zge::EffectClass>(row.effectClass));
        for (const EffectClassRow &row : kEffectClasses)
            zge::registerEffectClassName(ee::Ptr<zge::EffectClass>(row.effectClass), row.name);
        zge::registerEffectTypeName(ee::Ptr<zge::Affector>(kGravityAffector), kGravityName);
        zge::registerEffectTypeName(ee::Ptr<zge::Affector>(kScreenGravityAffector), kScreenGravityName);
        for (const StarterRow &row : kStarters)
            zge::registerNamedEffectClass(row.starter, row.name);
        const ee::Ptr<zge::EffectClass> sparkClass(sym::sparkEffect);
        for (const EffectClassRow &row : kFallingClasses)
            sparkClass->addAffectorByName(ee::Ptr<zge::EffectClass>(row.effectClass), row.name, 0);

        uint32_t step = 0;
        for (const MaterialRow &row : kMaterials)
        {
            fxProgress(step++);
            loadFxMaterials(library, row.firstGroup, row.name, row.count, row.flags, ee::Ptr<uint32_t>(row.materials));
        }
        fxProgress(5);
        placeLevelLensFlare(library);
        fxProgress(6);
        zge::closeZfileLibrary(library);
        initWaterWake();
        setupTreeFade();
        fxProgress(7);

        *g_depthRangeTask = zge::newTask(0, sym::depthRangeScriptFrame, 0, 0, kFxSourceFile, 624);
        *g_effectsFrameTask = zge::newTask(1, sym::countEffectsFrame, 0, 1, kFxSourceFile, 626);
        *g_depthRangeScript = {};
    }
    READABLE_FUNCTION_WAITS(initInlineFx, Verified);

    // Kept from the original: the sleep class is emptied twice, the first
    // time through the ripple class's method.
    void shutdownInlineFx()
    {
        *g_inlineFxReady = 0;
        zge::deleteTask(*g_effectsFrameTask);
        zge::deleteTask(*g_depthRangeTask);
        for (const EffectClassRow &row : kEffectClasses)
            zge::unregisterEffectClassName(row.name);
        zge::unregisterEffectClassName(kGlowName);

        const auto reset = [](uint32_t methodsOf, uint32_t effectClass) {
            ee::Ptr<zge::EffectClass>(methodsOf)->reset(ee::Ptr<zge::EffectClass>(effectClass));
        };
        reset(sym::sparkEffect, sym::sparkEffect);
        reset(sym::dustEffect, sym::dustEffect);
        reset(sym::posterEffect, sym::posterEffect);
        reset(sym::lightningEffect, sym::lightningEffect);
        reset(sym::rippleEffect, sym::sleepEffect);
        reset(sym::rippleEffect, sym::rippleEffect);
        reset(sym::splashEffect, sym::splashEffect);
        reset(sym::screenShardEffect, sym::screenShardEffect);
        reset(sym::shardEffect, sym::shardEffect);
        reset(sym::sleepEffect, sym::sleepEffect);

        for (const MaterialRow &row : kMaterials)
            releaseFxMaterials(ee::Ptr<uint32_t>(row.materials), row.count);
        freeWaterWake();
        zge::freeNamedEffectClasses();
        shutdownFxManager();
        *g_effectsObject = {};
    }
    READABLE_FUNCTION_WAITS(shutdownInlineFx, Verified);
}
