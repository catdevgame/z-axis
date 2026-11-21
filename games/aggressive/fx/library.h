#pragma once

// What the effects library's load (initInlineFx) keeps for
// the effects: the classes' name hashes and class handles by the names they
// are registered with (fx/particles.h: snow, rain, spark, ..., ripple), and
// the textures it loads from assets\fx.zal by their ids (kFxID_ImgBubble
// "Bubble" and the rest; each class draws with its own).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include "sym/aggressive/fx/library.h.h"

namespace zge { struct EffectClass; } // (zge/scene/effects.h)
namespace aggressive::skater { struct Player; }

namespace aggressive::fx
{
    // The classes' name hashes (nameHash of the class name).
    inline ee::Global<uint32_t, sym::g_snowHash> g_snowHash;
    inline ee::Global<uint32_t, sym::g_lightningHash> g_lightningHash;
    inline ee::Global<uint32_t, sym::g_rainHash> g_rainHash;
    inline ee::Global<uint32_t, sym::g_userBubbleHash> g_userBubbleHash;
    inline ee::Global<uint32_t, sym::g_userFireflyHash> g_userFireflyHash;
    inline ee::Global<uint32_t, sym::g_userSmokeHash> g_userSmokeHash;
    inline ee::Global<uint32_t, sym::g_userSparkHash> g_userSparkHash;
    inline ee::Global<uint32_t, sym::g_bubbleHash> g_bubbleHash;
    inline ee::Global<uint32_t, sym::g_fireflyHash> g_fireflyHash;
    inline ee::Global<uint32_t, sym::g_smokeHash> g_smokeHash;
    inline ee::Global<uint32_t, sym::g_sparkHash> g_sparkHash;
    inline ee::Global<uint32_t, sym::g_impactRingHash> g_impactRingHash;
    inline ee::Global<uint32_t, sym::g_explosionHash> g_explosionHash;
    inline ee::Global<uint32_t, sym::g_debrisHash> g_debrisHash;
    inline ee::Global<uint32_t, sym::g_firetrailHash> g_firetrailHash;
    inline ee::Global<uint32_t, sym::g_vaporHash> g_vaporHash;
    inline ee::Global<uint32_t, sym::g_bloodSquirtHash> g_bloodSquirtHash;
    inline ee::Global<uint32_t, sym::g_bloodCloudHash> g_bloodCloudHash;
    inline ee::Global<uint32_t, sym::g_bloodPoolHash> g_bloodPoolHash;
    inline ee::Global<uint32_t, sym::g_splashRootHash> g_splashRootHash;
    inline ee::Global<uint32_t, sym::g_splashRingHash> g_splashRingHash;
    inline ee::Global<uint32_t, sym::g_shardHash> g_shardHash;
    inline ee::Global<uint32_t, sym::g_skidHash> g_skidHash;
    inline ee::Global<uint32_t, sym::g_rippleHash> g_rippleHash;
    inline ee::Global<uint32_t, sym::g_screenShardHash> g_screenShardHash;

    // The classes themselves (their tables, by name: findEffectClassByName), as the
    // effects' starts hand them to startEffect.
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_snowClass> g_snowClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_lightningClass> g_lightningClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_rainClass> g_rainClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_userBubbleClass> g_userBubbleClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_userFireflyClass> g_userFireflyClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_userSmokeClass> g_userSmokeClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_userSparkClass> g_userSparkClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_bubbleClass> g_bubbleClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_fireflyClass> g_fireflyClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_smokeClass> g_smokeClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_sparkClass> g_sparkClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_impactRingClass> g_impactRingClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_explosionClass> g_explosionClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_debrisClass> g_debrisClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_firetrailClass> g_firetrailClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_vaporClass> g_vaporClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_bloodSquirtClass> g_bloodSquirtClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_bloodCloudClass> g_bloodCloudClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_bloodPoolClass> g_bloodPoolClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_splashRootClass> g_splashRootClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_splashRingClass> g_splashRingClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_shardClass> g_shardClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_posterClass> g_posterClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_rippleClass> g_rippleClass;
    inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_screenShardClass> g_screenShardClass;

    // The textures (by their fx.zal ids).
    inline ee::Global<uint32_t, sym::g_splashRingTexture> g_splashRingTexture;  // kFxID_ImgSplashRing
    inline ee::Global<uint32_t, sym::g_splashRing2Texture> g_splashRing2Texture; // kFxID_ImgSplashRing2
    inline ee::Global<uint32_t, sym::g_splashTexture> g_splashTexture;      // kFxID_ImgSplash
    inline ee::Global<uint32_t, sym::g_rippleTexture> g_rippleTexture;      // kFxID_ImgRipple
    inline ee::Global<uint32_t, sym::g_bubbleTexture> g_bubbleTexture;      // kFxID_ImgBubble
    inline ee::Global<uint32_t, sym::g_fireflyTexture> g_fireflyTexture;     // kFxID_ImgFireFly
    inline ee::Global<uint32_t, sym::g_glassTexture> g_glassTexture;       // kFxID_ImgGlass
    inline ee::Global<uint32_t, sym::g_iceGlassTexture> g_iceGlassTexture;    // kFxID_ImgIceFace
    inline ee::Global<uint32_t, sym::g_firetrailTexture> g_firetrailTexture;   // kFxID_ImgFireTrail
    inline ee::Global<uint32_t, sym::g_sparkTexture> g_sparkTexture;       // kFxID_ImgSpark00
    inline ee::Global<uint32_t, sym::g_snowTexture> g_snowTexture;        // "Snow" (kFxID_ImgSnow00)
    inline ee::Global<uint32_t, sym::g_rainTexture> g_rainTexture;        // "Rain" (kFxID_ImgRain00)
    inline ee::Global<uint32_t, sym::g_turboStreakTexture> g_turboStreakTexture; // kFxID_ImgStreakTurbo
    inline ee::Global<uint32_t, sym::g_trickStreakTexture> g_trickStreakTexture; // kFxID_ImgStreakTrick
    inline ee::Global<uint32_t, sym::g_dropStreakTexture> g_dropStreakTexture;  // kFxID_ImgStreakDrop
    inline ee::Global<uint32_t, sym::g_bloodSmallTexture> g_bloodSmallTexture;  // kFxID_ImgBloodSmall
    inline ee::Global<uint32_t, sym::g_bloodMediumTexture> g_bloodMediumTexture; // kFxID_ImgBloodMedium
    inline ee::Global<uint32_t, sym::g_bloodLargeTexture> g_bloodLargeTexture;  // kFxID_ImgBloodLarge
    inline ee::Global<uint32_t, sym::g_bloodPoolTexture> g_bloodPoolTexture;   // kFxID_ImgBloodPool
    inline ee::Global<uint32_t, sym::g_bloodCloudTexture> g_bloodCloudTexture;  // kFxID_ImgBloodCloud
    inline ee::Global<uint32_t, sym::g_impactRingTexture> g_impactRingTexture;  // kFxID_ImgImpactRing
    inline ee::Global<uint32_t, sym::g_smokeBallTexture> g_smokeBallTexture;   // kFxID_ImgSmokeBall
    inline ee::Global<uint32_t, sym::g_steamBallTexture> g_steamBallTexture;   // kFxID_ImgSteamBall
    inline ee::Global<uint32_t, sym::g_snowBallTexture> g_snowBallTexture;    // kFxID_ImgSnowBall
    inline ee::Global<uint32_t, sym::g_fireBallTexture> g_fireBallTexture;    // kFxID_ImgFireBall
    inline ee::Global<uint32_t, sym::g_explosionTexture> g_explosionTexture;   // kFxID_ImgExplosion
    inline ee::Global<uint32_t, sym::g_skidTexture> g_skidTexture;        // kFxID_ImgSkid
    inline ee::Global<uint32_t, sym::g_boltTexture> g_boltTexture;        // the storm's lightning
    inline ee::Global<uint32_t, sym::g_forkTexture> g_forkTexture;

    // One of the lens flare's rings and glows, drawn on the line from the sun
    // through the middle of the screen (fx/lens_flare.cpp).
    struct LensFlareElement
    {
        ee::Float distance; // +0x00 where on the line: 1 at the sun, 0 the middle, -1 as far the other side
        ee::Float scale;    // +0x04 its size, times the flare's elementScaleX / Y
        uint8_t colour[4];  // +0x08 RGBA; the alpha fades as the camera turns from the sun
        uint32_t texture;   // +0x0C the flare's glow or second ring
    };
    static_assert(sizeof(LensFlareElement) == 0x10);

    constexpr int kMaxLensFlareElements = 16;

    // The level's lens flare (newLensFlare): its five textures (from the
    // effects library), where the sun is, its colour and its elements.
    struct LensFlare
    {
        uint32_t glow;       // +0x00 "LensGlow"
        uint32_t ring1;      // +0x04 "LensRing1"
        uint32_t ray;        // +0x08 "LensRay"
        uint32_t unusedSlot; // +0x0C made 0 with the textures; read nowhere
        uint32_t _10;
        uint32_t star;       // +0x14 "LensStar"
        uint32_t ring2;      // +0x18 "LensRing2"
        uint32_t _1c;
        zge::Vec4 position;  // +0x20 the sun
        uint8_t colour[4];   // +0x30 RGBA, from the level's weather
        uint32_t elementCount;                            // +0x34 9
        LensFlareElement elements[kMaxLensFlareElements]; // +0x38
        uint32_t elementScaleX; // +0x138 an element's half width is its scale times half this (256)
        uint32_t elementScaleY; // +0x13C and its half height
        uint8_t _140[0x10];
    };
    static_assert(sizeof(LensFlare) == 0x150);
    static_assert(offsetof(LensFlare, star) == 0x14 && offsetof(LensFlare, position) == 0x20 && offsetof(LensFlare, colour) == 0x30);
    static_assert(offsetof(LensFlare, elements) == 0x38 && offsetof(LensFlare, elementScaleX) == 0x138);
    // The level's lens flare (made by its first placeLensFlare), 0: none.
    inline ee::Global<ee::Ptr<LensFlare>, sym::g_levelLensFlare> g_levelLensFlare;

    int32_t loadFxMaterial(uint32_t library, uint32_t textureId, uint32_t name, uint32_t flags, ee::Ptr<uint32_t> material); // (materials.cpp)
    int32_t releaseFxMaterial(ee::Ptr<uint32_t> material);
    void loadUserEffects();                                                      // (user_effects.cpp)
    void freeUserEffects();
    void stopLevelWeather();                                                     // (weather.cpp)
}

namespace aggressive::fx
{
    // (lens_flare.cpp)
    void drawLensFlare(ee::Ptr<LensFlare> flare);
    void placeLensFlare(ee::Ptr<zge::Vec4> position, uint32_t rgba);
}

namespace aggressive::fx
{
    // (library.cpp)
    void initInlineFx();
    void shutdownInlineFx();
}

namespace aggressive::fx
{
    // (lightning.cpp)
    uint32_t loadLightningTextures();
}

namespace aggressive
{
    // (movers.cpp)
    void freeMovers();
    void moveMovers(ee::Float seconds);
    int32_t setupMovers();
}

namespace aggressive::fx
{
    // (screen_flash.cpp)
    void drawScreenFlash();
    void startScreenFlash(uint32_t kind, uint32_t rgba, ee::Float seconds);
    void updateScreenFlash(ee::Float seconds);
}

namespace aggressive::fx
{
    // (skater_effects.cpp)
    void clearTouchedBreakables();
    void clearTrailsAfterRespawn();
    int32_t countBreakables();
    void makeSkaterReflection(ee::Ptr<skater::Player> player);
    void reflectSkater(ee::Ptr<skater::Player> player);
    void releaseSkaterReflection(ee::Ptr<skater::Player> player);
    void skaterEffects(ee::Ptr<skater::Player> player);
}

namespace aggressive::fx
{
    // (spawn_effects.cpp)
    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec3> normal, ee::Float fromWidth, ee::Float toWidth);
    void spawnSplashDrops(ee::Ptr<zge::Vec4> origin, ee::Ptr<zge::Vec4> position, ee::Float radius);
    void spawnWallTapRipple(ee::Ptr<skater::Player> player, ee::Ptr<zge::Vec4> at, ee::Ptr<zge::Vec4> velocity);
}

namespace aggressive::fx
{
    // (spawns.cpp)
    void bleed(ee::Ptr<skater::Player> player, uint32_t pointIndex);
    void debris(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> normal, uint32_t kindArgument);
    int32_t destroySkidMarks();
    void fireTrail(ee::Ptr<zge::Vec4> head, ee::Ptr<zge::Vec4> tail);
    void grindSparks(ee::Ptr<skater::Player> source, ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec4> direction, uint32_t on);
    void impactRing(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Quaternion> orientation, ee::Ptr<zge::Vec4> velocity);
    void spawnExplosion(ee::Address position, uint32_t withDebris, ee::Float floorHeight);
    void spawnSplashRing(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Float offset);
    void splash(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec4> velocity);
    void splashWith(ee::Address position, ee::Address velocity, ee::Float radius);
    void startLevelUpFlash(ee::Ptr<skater::Player> player);
    void startPisserCreatePuff(uint32_t toolPosition);
    void startPisserDeletePuff(uint32_t toolPosition);
    void updateLevelUpFlash(ee::Ptr<skater::Player> player);
    void updatePisserPuff(ee::Float seconds);
}

namespace aggressive::fx
{
    // (user_effects.cpp)
    void setUserEffectState(uint32_t name, uint32_t enabled);
    void updateUserEffects(ee::Float dt);
}

namespace aggressive::fx
{
    // (weather.cpp)
    void resumeWeatherEffects();
    void startLevelWeather(uint32_t weather);
    void suspendWeatherEffects();
    void updateLevelWeather(ee::Float seconds);
}

namespace aggressive::fx
{
    // (spawns.cpp)
    void animationEventHook(ee::Ptr<skater::Player> player, uint32_t hook);
}
