// BMX XXX's effects library (library.h). BMX XXX's own code (fx.c's block);
// releaseFxMaterials is word for word DM2's (games/dm2/fx/library.cpp), the
// set-up and shutdown follow Aggressive Inline's (games/aggressive/fx/library.cpp).

#include "library.h"
#include "effects_clock.h"
#include "emitter_objects.h"
#include "spawns.h"
#include "tree_fade.h"
#include "water_wake.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/images.h"
#include "zge/assets/zfile_library.h"
#include "zge/game.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/scene/effects.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/bmx/fx/library.cpp.h"

namespace game::bmx
{
    namespace
    {
        // Set while the effects are set up.
        inline ee::Global<uint32_t, sym::g_effectsReady> g_effectsReady;
        // The effects' clock, float milliseconds.
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;
        // The object the level's effects start on.
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_effectsObject> g_effectsObject;
        // The effects' two tasks.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_effectsFirstTask> g_firstTask;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_effectsSecondTask> g_secondTask;
        // DM2's depth range script (its first task plays it); BMX XXX only
        // clears it at the set-up.
        inline ee::Global<uint32_t, sym::g_depthRangeScript> g_depthRangeScript;

        constexpr uint32_t kFxLibrary = sym::fxLibraryPath; // "assets\fx.zal"
        constexpr uint32_t kFxFile = sym::fxSourceFile;     // "./src/com/action/fx.c"
        constexpr uint32_t kFirstTaskLine = 720, kSecondTaskLine = 722;
        constexpr uint32_t kMaterialNameFormat = sym::fxMaterialNameFormat; // "MatFx%s%02d"

        // The effect classes (the game's tables of their methods).
        inline const ee::Ptr<zge::EffectClass> kSparkClass(sym::sparkEffectClass);
        inline const ee::Ptr<zge::EffectClass> kTracerClass(sym::tracerEffectClass);
        inline const ee::Ptr<zge::EffectClass> kShardClass(sym::shardEffectClass);
        inline const ee::Ptr<zge::EffectClass> kScreenShardClass(sym::screenShardEffectClass);
        inline const ee::Ptr<zge::EffectClass> kSplashClass(sym::splashEffectClass);
        inline const ee::Ptr<zge::EffectClass> kDustClass(sym::dustEffectClass);
        inline const ee::Ptr<zge::EffectClass> kPosterClass(sym::posterEffectClass);
        inline const ee::Ptr<zge::EffectClass> kRippleClass(sym::rippleEffectClass);
        inline const ee::Ptr<zge::EffectClass> kLightningClass(sym::lightningEffectClass);
        inline const ee::Ptr<zge::EffectClass> kSleepClass(sym::sleepEffectClass);

        // The names the classes, affectors and starters are registered under.
        constexpr uint32_t kLightningName = sym::lightningName;         // "lightning"
        constexpr uint32_t kSparkName = sym::sparkName;                 // "spark"
        constexpr uint32_t kTracerName = sym::tracerName;               // "tracer"
        constexpr uint32_t kShardName = sym::shardName;                 // "shard"
        constexpr uint32_t kScreenShardName = sym::screenShardName;     // "screenshard"
        constexpr uint32_t kSplashName = sym::splashName;               // "splash"
        constexpr uint32_t kDustName = sym::dustName;                   // "dust"
        constexpr uint32_t kPosterName = sym::posterName;               // "poster"
        constexpr uint32_t kRippleName = sym::rippleName;               // "ripple"
        constexpr uint32_t kSleepName = sym::sleepName;                 // "sleep"
        constexpr uint32_t kSkidName = sym::skidName;                   // "skid"
        constexpr uint32_t kPockMarkName = sym::pockMarkName;           // "pockmark"
        constexpr uint32_t kGlowName = sym::glowName;                   // "glow"
        constexpr uint32_t kGravityName = sym::gravityName;             // "gravity"
        constexpr uint32_t kScreenGravityName = sym::screenGravityName; // "gravity_screen"
        // The two affectors: gravity, and the screen shards' gravity.
        inline const ee::Ptr<zge::Affector> kGravityAffector(sym::gravityAffector);
        inline const ee::Ptr<zge::Affector> kScreenGravityAffector(sym::screenGravityAffector);

        // The effects' textures: the library asset each set starts at, the
        // name its materials are given, and where they are kept.
        constexpr uint32_t kPockMarkAsset = sym::pockMarkAssetName, kPockMarkMaterial = sym::pockMarkMaterialName; // "PockMark.00", "PockMark"
        constexpr uint32_t kSparkAsset = sym::sparkAssetName, kSparkMaterial = sym::sparkMaterialName;             // "Spark.00", "Spark"
        constexpr uint32_t kSkidAsset = sym::skidAssetName, kSkidMaterial = sym::skidMaterialName;                 // "Skid.00", "Skid"
        constexpr uint32_t kSleepAsset = sym::sleepAssetName, kSleepMaterial = sym::sleepMaterialName;             // "Sleep.00", "Sleep"
        constexpr uint32_t kPockMarks = 3, kSleeps = 2;
        inline const ee::Ptr<uint32_t> kPockMarkMaterials(sym::g_pockMarkMaterials);
        inline const ee::Ptr<uint32_t> kSparkMaterials(sym::g_sparkMaterial);
        inline const ee::Ptr<uint32_t> kSkidMaterials(sym::g_skidMaterial);
        inline const ee::Ptr<uint32_t> kSleepMaterials(sym::g_sleepMaterials);
        // The materials' blend modes.
        constexpr uint32_t kBlendTwo = 2, kBlendThree = 3;
        // The loads' steps of seven (not read by the load).
        constexpr uint32_t kLoadSteps = 7;

        // readImage's kinds the flags ask for.
        constexpr uint32_t kImageKindTwo = 2, kImageKindThree = 3;

        // Scaffolding (address-call): an engine function with no body in
        // BMX XXX's instance.
        namespace calls
        {
        // Every emitter the effects' object has is taken off it and deleted
        // by its class (a name from its code: the engine has no row for it).
        void deleteEffectEmitters(ee::Ptr<zge::SceneObject> object) { ee::game(sym::deleteEffectEmitters, object); }
        }
    }

    // initEffects: the level's effects set up. The clock starts at 0; from
    // the effects' library come the emitter objects' sources (by name) and,
    // last, the textures. Every class but the tracer's is initialised (kept
    // as it is); all ten are registered by name, with the two affectors and
    // the twelve named starters ("skid" and "pockmark" start posters). The
    // spark class's method gives gravity to the spark, tracer, splash and
    // shard classes and the screen's gravity to the screen shards. Then the
    // water wake, the trees' draw check and the two tasks.
    void initEffects()
    {
        *g_effectsReady = 1;
        *g_clockMs = 0.0f;
        const ee::Ptr<zge::ZfileLibrary> library = zge::openZfileLibrary(kFxLibrary);
        zge::initEffectTables();
        *g_effectsObject = zge::newSceneEmitter(0);
        for (uint32_t kind = 0; kind < kEmitterObjectKinds; ++kind)
        {
            EmitterObjectKindRecord &record = (*g_emitterObjectKinds)[kind];
            record.source = ee::Ptr<zge::SceneObject>(zge::findSceneObject(zge::nameHash(ee::pointerAt<char>(record.name)), 0, 0));
        }
        zge::memoryFill(&*g_emitterObjects, 0, sizeof(EmitterObjectSlot) * kEmitterObjectSlots);
        *g_nextEmitterObject = 0;

        zge::initEffectClass(kLightningClass);
        zge::initEffectClass(kSparkClass);
        zge::initEffectClass(kShardClass);
        zge::initEffectClass(kScreenShardClass);
        zge::initEffectClass(kSplashClass);
        zge::initEffectClass(kDustClass);
        zge::initEffectClass(kPosterClass);
        zge::initEffectClass(kRippleClass);
        zge::initEffectClass(kSleepClass);

        zge::registerEffectClassName(kLightningClass, kLightningName);
        zge::registerEffectClassName(kSparkClass, kSparkName);
        zge::registerEffectClassName(kTracerClass, kTracerName);
        zge::registerEffectClassName(kShardClass, kShardName);
        zge::registerEffectClassName(kScreenShardClass, kScreenShardName);
        zge::registerEffectClassName(kSplashClass, kSplashName);
        zge::registerEffectClassName(kDustClass, kDustName);
        zge::registerEffectClassName(kPosterClass, kPosterName);
        zge::registerEffectClassName(kRippleClass, kRippleName);
        zge::registerEffectClassName(kSleepClass, kSleepName);

        zge::registerEffectTypeName(kGravityAffector, kGravityName);
        zge::registerEffectTypeName(kScreenGravityAffector, kScreenGravityName);

        zge::registerNamedEffectClass(sym::startSparkEffect, kSparkName);
        zge::registerNamedEffectClass(sym::startTracerEffect, kTracerName);
        zge::registerNamedEffectClass(sym::startShardEffect, kShardName);
        zge::registerNamedEffectClass(sym::startScreenShardEffect, kScreenShardName);
        zge::registerNamedEffectClass(sym::startSplashEffect, kSplashName);
        zge::registerNamedEffectClass(sym::startLightningEffect, kLightningName);
        zge::registerNamedEffectClass(sym::startDustEffect, kDustName);
        zge::registerNamedEffectClass(sym::startPosterEffect, kPosterName);
        zge::registerNamedEffectClass(sym::startPosterEffect, kSkidName);
        zge::registerNamedEffectClass(sym::startPosterEffect, kPockMarkName);
        zge::registerNamedEffectClass(sym::startRippleEffect, kRippleName);
        zge::registerNamedEffectClass(sym::startSleepEffect, kSleepName);

        kSparkClass->addAffectorByName(kSparkClass, kGravityName, 0);
        kSparkClass->addAffectorByName(kTracerClass, kGravityName, 0);
        kSparkClass->addAffectorByName(kSplashClass, kGravityName, 0);
        kSparkClass->addAffectorByName(kShardClass, kGravityName, 0);
        kSparkClass->addAffectorByName(kScreenShardClass, kScreenGravityName, 0);

        loadFxMaterials(library, ee::pointerAt<char>(kPockMarkAsset), kPockMarkMaterial, kPockMarks, kFxKindThree | kBlendTwo,
                        kPockMarkMaterials, 1, kLoadSteps);
        loadFxMaterials(library, ee::pointerAt<char>(kSparkAsset), kSparkMaterial, 1, kBlendTwo, kSparkMaterials, 2, kLoadSteps);
        loadFxMaterials(library, ee::pointerAt<char>(kSkidAsset), kSkidMaterial, 1, kBlendThree, kSkidMaterials, 4, kLoadSteps);
        loadFxMaterials(library, ee::pointerAt<char>(kSleepAsset), kSleepMaterial, kSleeps, kFxKindThree | kFxTwoSidedOff | kBlendTwo,
                        kSleepMaterials, 5, kLoadSteps);
        zge::closeZfileLibrary(library);
        initWaterWake();
        setupTreeFade();
        *g_firstTask = zge::newTask(0, sym::emptyEffectsTask, 0u, 0, kFxFile, kFirstTaskLine);
        *g_secondTask = zge::newTask(1, sym::countEffectsFrame, 0u, 1, kFxFile, kSecondTaskLine);
        *g_depthRangeScript = 0;
    }
    READABLE_FUNCTION_WAITS(initEffects, Verified);

    // clearEffects: every effect gone: the effects object's emitters deleted
    // and every emitter object of the pool freed.
    void clearEffects()
    {
        calls::deleteEffectEmitters(*g_effectsObject);
        for (uint32_t slot = 0; slot < kEmitterObjectSlots; ++slot)
            freeEmitterObject(&(*g_emitterObjects)[slot]);
    }
    READABLE_FUNCTION_WAITS(clearEffects, Verified);

    // shutdownEffects: the effects taken down: every effect cleared, the two
    // tasks deleted, the ten class names (and "glow", which the set-up never
    // registers) unregistered, the classes reset, the textures let go, the
    // water wake freed and the registry emptied. Kept as it is: the sleep
    // class is reset twice, the first time through the ripple class's
    // method (every class has the same one).
    void shutdownEffects()
    {
        clearEffects();
        const ee::Ptr<zge::Task> secondTask = *g_secondTask;
        *g_effectsReady = 0;
        zge::deleteTask(secondTask);
        zge::deleteTask(*g_firstTask);
        zge::unregisterEffectClassName(kLightningName);
        zge::unregisterEffectClassName(kSparkName);
        zge::unregisterEffectClassName(kTracerName);
        zge::unregisterEffectClassName(kShardName);
        zge::unregisterEffectClassName(kScreenShardName);
        zge::unregisterEffectClassName(kSplashName);
        zge::unregisterEffectClassName(kDustName);
        zge::unregisterEffectClassName(kPosterName);
        zge::unregisterEffectClassName(kRippleName);
        zge::unregisterEffectClassName(kSleepName);
        zge::unregisterEffectClassName(kGlowName);
        kSparkClass->reset(kSparkClass);
        kTracerClass->reset(kTracerClass);
        kDustClass->reset(kDustClass);
        kPosterClass->reset(kPosterClass);
        kLightningClass->reset(kLightningClass);
        kRippleClass->reset(kSleepClass);
        kRippleClass->reset(kRippleClass);
        kSplashClass->reset(kSplashClass);
        kScreenShardClass->reset(kScreenShardClass);
        kShardClass->reset(kShardClass);
        kSleepClass->reset(kSleepClass);
        releaseFxMaterials(kPockMarkMaterials, kPockMarks);
        releaseFxMaterials(kSparkMaterials, 1);
        releaseFxMaterials(kSkidMaterials, 1);
        releaseFxMaterials(kSleepMaterials, kSleeps);
        freeWaterWake();
        zge::freeNamedEffectClasses();
        *g_effectsObject = {};
    }
    READABLE_FUNCTION_WAITS(shutdownEffects, Verified);

    // emptyEffectsTask: the effects' first task (DM2's plays its depth range
    // script here; BMX XXX's does nothing). Returns 1.
    uint32_t emptyEffectsTask()
    {
        return 1;
    }
    READABLE_FUNCTION(emptyEffectsTask, Verified);

    // loadFxMaterials: see library.h. DM2's (games/dm2/fx/library.cpp) with
    // the asset found once, by name, and read on from there. "MatFx", the
    // name and the texture's number are formatted into the frame and not
    // used (a debug name, cut). Each material gets the flags' blend mode, an
    // alpha test above 1 and the two-sided state set 1, then 0 with
    // kFxTwoSidedOff.
    void loadFxMaterials(ee::Ptr<zge::ZfileLibrary> library, const char *assetName, uint32_t name, uint32_t count, uint32_t flags,
                         ee::Ptr<uint32_t> materials, uint32_t /*step*/, uint32_t /*steps*/)
    {
        ee::Local<char[0x40]> debugName;
        if (!zge::findAsset(ee::Address(library), zge::nameHash(assetName)))
            return;
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint32_t memory = zge::allocateMemory(zge::zfileLibraryAssetBytes(library));
            zge::readZfileLibraryAsset(library, memory);
            zge::formatString(debugName, kMaterialNameFormat, name, index);
            const uint32_t record = zge::newImageRecord();
            if (record)
            {
                const uint32_t fileReader = zge::findImageReader(memory);
                const uint32_t asFlagged = (flags & kFxKindThree) ? kImageKindThree : fileReader;
                const uint32_t read = zge::readImage(record, memory, (flags & kFxKindTwo) ? kImageKindTwo : asFlagged);
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
                            // (rebuildMaterialRegisters: the engine's jump to setMaterialState, as the original calls it)
                            zge::zgui::rebuildMaterialRegisters(made, 0, zge::kStateBlendMode, flags & kFxBlendModeMask);
                            zge::setMaterialState(made, 0, zge::kStateAlphaReference, 1);
                            if (flags & kFxNoDepthTest)
                                zge::zgui::rebuildMaterialRegisters(made, 0, zge::kStateDepthGreater, 0);
                            zge::zgui::rebuildMaterialRegisters(made, 0, zge::kStateTwoSided, 1);
                            if (flags & kFxTwoSidedOff)
                                zge::zgui::rebuildMaterialRegisters(made, 0, zge::kStateTwoSided, 0);
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

    // releaseFxMaterials: each of the `count` materials released.
    void releaseFxMaterials(ee::Ptr<uint32_t> materials, uint32_t count)
    {
        for (uint32_t index = 0; index < count; ++index)
            zge::releaseMaterial(materials[index]);
    }
    READABLE_FUNCTION_WAITS(releaseFxMaterials, Verified);
}
