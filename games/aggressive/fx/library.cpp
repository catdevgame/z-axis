// The effects library (inlineFx.c): what a level's load sets up for its
// effects and the level's end takes down. assets\fx.zal's textures, the
// effect classes by name and their affectors, the hashes and handles the
// effects find them by, the skater's bones they start from, and the two
// tasks that run them. The tables below are the original's calls in its
// order .
//

#include "library.h"
#include "particles.h"

#include "aggressive/globals.h"
#include "aggressive/script/script.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/text/strings.h"

#include "ee/function_ptr.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/tasks.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/effects.h"
#include "zge/scene/particle_types.h"
#include "aggressive/fx/library.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/fx/library.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        constexpr uint32_t kFxLibrary = sym::fxLibrary;        // "assets\fx.zal"
        constexpr uint32_t kFireFrames = sym::g_fireFrames;       // kFxID_ImgFireA00 .. A09
        constexpr uint32_t kInlineFxSource = sym::inlineFxSource;   // "./src/com/inlineFx.c"
        constexpr uint32_t kDebrisObjectName = sym::debrisObjectName; // "PropDef_Debris"
        constexpr uint32_t kAllTouchedName = sym::allTouchedName;   // "parkEditorAllEditorPiecesTouched"
        constexpr uint32_t kResetTouchedName = sym::resetTouchedName; // "parkEditorResetTouchesPiece"
        constexpr uint32_t kAllTouchedNative = sym::parkEditorAllEditorPiecesTouched;
        constexpr uint32_t kResetTouchedNative = sym::parkEditorResetTouchesPiece;
        constexpr uint32_t kFxTask = sym::idleFxStage, kFxDrawTask = sym::drawFxOverlay;

        // A class's affector methods, the first four words of every class's
        // table (the same four functions in each; zge's EffectClass has the
        // rest, from +0x10). The affectors are the class's list at +0x24.
        struct ClassMethods
        {
            // +0x00 resetEffectClass: the class's affectors cleared through its
            // own +0x0C, nothing else. Only shutdownInlineFx calls it.
            ee::FunctionPtr<void(uint32_t effectClass)> reset;
            // +0x04 addEffectClassProperty: a new link {affector, argument} on the
            // class's list.
            ee::FunctionPtr<void(uint32_t effectClass, uint32_t affector, uint32_t argument)> addAffector;
            // +0x08 addEffectClassPropertyByName: the affector found by its type's
            // name (registerEffectTypeName's), then added as +0x04 does; none
            // found, nothing.
            ee::FunctionPtr<void(uint32_t effectClass, uint32_t affectorName, uint32_t argument)> addNamedAffector;
            // +0x0C clearEffectClassProperties: each link taken off the list and
            // freed.
            ee::FunctionPtr<void(uint32_t effectClass)> clearAffectors;
        };
        static_assert(offsetof(ClassMethods, addNamedAffector) == 0x08 && offsetof(ClassMethods, clearAffectors) == 0x0C);

        inline ee::Global<int32_t, sym::g_inlineFxReady> g_inlineFxReady;
        inline ee::Global<uint32_t, sym::g_debrisObject> g_debrisObject;
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;
        inline ee::Global<int32_t, sym::g_weatherActive> g_weatherActive;
        inline ee::Global<int32_t, sym::g_weatherHeld> g_weatherHeld;
        inline ee::Global<uint32_t, sym::g_clearWeather> g_clearWeather;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_lensFlareTask> g_lensFlareTask;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_fxTaskHandle> g_fxTaskHandle;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_fxDrawTaskHandle> g_fxDrawTaskHandle;
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_effectsObject> g_effectsObject;
        inline ee::Global<uint32_t, sym::g_fxTable> g_fxTable;
        inline ee::Global<zge::Vec4, sym::g_lensFlarePosition> g_lensFlarePosition; // the level's sun
        inline ee::Global<int32_t, sym::g_levelNumber> g_currentLevel;

        // The classes (0x38-byte method tables), their names, and where
        // their initEffectClass and registerEffectClassName calls return.
        constexpr uint32_t kSnowEffect = sym::g_snowEffectClass;
        constexpr uint32_t kRainEffect = sym::g_rainEffectClass;
        constexpr uint32_t kSparkEffect = sym::sparkEffect;
        constexpr uint32_t kShardEffect = sym::g_shardEffectClass;
        constexpr uint32_t kScreenShardEffect = sym::g_screenShardEffectClass;
        constexpr uint32_t kUserSparkEffect = sym::g_userSparkEffectClass;
        constexpr uint32_t kLightningEffect = sym::g_lightningEffectClass;
        constexpr uint32_t kVaporEffect = sym::g_vaporEffectClass;
        constexpr uint32_t kBloodSquirtEffect = sym::g_bloodSquirtEffectClass;
        constexpr uint32_t kBloodCloudEffect = sym::g_bloodCloudEffectClass;
        constexpr uint32_t kBloodPoolEffect = sym::g_bloodPoolEffectClass;
        constexpr uint32_t kSplashRootEffect = sym::g_splashRootEffectClass;
        constexpr uint32_t kSplashRingEffect = sym::g_splashRingEffectClass;
        constexpr uint32_t kSmokeEffect = sym::g_smokeEffectClass;
        constexpr uint32_t kUserSmokeEffect = sym::g_userSmokeEffectClass;
        constexpr uint32_t kPosterEffect = sym::g_posterEffectClass;
        constexpr uint32_t kImpactRingEffect = sym::g_impactRingEffectClass;
        constexpr uint32_t kExplosionEffect = sym::g_explosionEffectClass;
        constexpr uint32_t kDebrisEffect = sym::g_debrisEffectClass;
        constexpr uint32_t kFiretrailEffect = sym::g_fireTrailEffectClass;
        constexpr uint32_t kBubbleEffect = sym::g_bubbleEffectClass;
        constexpr uint32_t kUserBubbleEffect = sym::g_userBubbleEffectClass;
        constexpr uint32_t kFireflyEffect = sym::g_fireflyEffectClass;
        constexpr uint32_t kUserFireflyEffect = sym::g_userFireflyEffectClass;
        constexpr uint32_t kSplashEffect = sym::g_splashEffectClass;
        constexpr uint32_t kRippleEffect = sym::g_rippleEffectClass;

        struct EffectClassRow { uint32_t effectClass, name, initReturnsTo, nameReturnsTo; };
        constexpr EffectClassRow kEffectClasses[] = {
            {kSnowEffect, sym::g_textSnow, points::initInlineFx::afterInitEffectClass, points::initInlineFx::afterRegisterEffectClassName}, // "snow"
            {kRainEffect, sym::g_textRain, points::initInlineFx::afterInitEffectClass2, points::initInlineFx::afterRegisterEffectClassName2}, // "rain"
            {kSparkEffect, sym::g_textSpark2, points::initInlineFx::afterInitEffectClass3, points::initInlineFx::afterRegisterEffectClassName3}, // "spark"
            {kShardEffect, sym::g_textShard, points::initInlineFx::afterInitEffectClass4, points::initInlineFx::afterRegisterEffectClassName4}, // "shard"
            {kScreenShardEffect, sym::g_textScreenshard, points::initInlineFx::afterInitEffectClass5, points::initInlineFx::afterRegisterEffectClassName5}, // "screenshard"
            {kUserSparkEffect, sym::g_textUserspark, points::initInlineFx::afterInitEffectClass6, points::initInlineFx::afterRegisterEffectClassName6}, // "userspark"
            {kLightningEffect, sym::g_textLightning, points::initInlineFx::afterInitEffectClass7, points::initInlineFx::afterRegisterEffectClassName7}, // "lightning"
            {kVaporEffect, sym::g_textVapor, points::initInlineFx::afterInitEffectClass8, points::initInlineFx::afterRegisterEffectClassName8}, // "vapor"
            {kBloodSquirtEffect, sym::g_textBloodsquirt, points::initInlineFx::afterInitEffectClass9, points::initInlineFx::afterRegisterEffectClassName9}, // "bloodsquirt"
            {kBloodCloudEffect, sym::g_textBloodcloud, points::initInlineFx::afterInitEffectClass10, points::initInlineFx::afterRegisterEffectClassName10}, // "bloodcloud"
            {kBloodPoolEffect, sym::g_textBloodpool, points::initInlineFx::afterInitEffectClass11, points::initInlineFx::afterRegisterEffectClassName11}, // "bloodpool"
            {kSplashRootEffect, sym::g_textSplashroot, points::initInlineFx::afterInitEffectClass12, points::initInlineFx::afterRegisterEffectClassName12}, // "splashroot"
            {kSplashRingEffect, sym::g_textSplashring, points::initInlineFx::afterInitEffectClass13, points::initInlineFx::afterRegisterEffectClassName13}, // "splashring"
            {kSmokeEffect, sym::g_textSmoke, points::initInlineFx::afterInitEffectClass14, points::initInlineFx::afterRegisterEffectClassName14}, // "smoke"
            {kUserSmokeEffect, sym::g_textUsersmoke, points::initInlineFx::afterInitEffectClass15, points::initInlineFx::afterRegisterEffectClassName15}, // "usersmoke"
            {kPosterEffect, sym::g_textPoster, points::initInlineFx::afterInitEffectClass16, points::initInlineFx::afterRegisterEffectClassName16}, // "poster"
            {kImpactRingEffect, sym::g_textImpactRing, points::initInlineFx::afterInitEffectClass17, points::initInlineFx::afterRegisterEffectClassName17}, // "impactRing"
            {kExplosionEffect, sym::g_textExplosion, points::initInlineFx::afterInitEffectClass18, points::initInlineFx::afterRegisterEffectClassName18}, // "explosion"
            {kDebrisEffect, sym::g_textDebris, points::initInlineFx::afterInitEffectClass19, points::initInlineFx::afterRegisterEffectClassName19}, // "debris"
            {kFiretrailEffect, sym::g_textFiretrail, points::initInlineFx::afterInitEffectClass20, points::initInlineFx::afterRegisterEffectClassName20}, // "firetrail"
            {kBubbleEffect, sym::g_textBubble, points::initInlineFx::afterInitEffectClass21, points::initInlineFx::afterRegisterEffectClassName21}, // "bubble"
            {kUserBubbleEffect, sym::g_textUserbubble, points::initInlineFx::afterInitEffectClass22, points::initInlineFx::afterRegisterEffectClassName22}, // "userbubble"
            {kFireflyEffect, sym::g_textFirefly, points::initInlineFx::afterInitEffectClass23, points::initInlineFx::afterRegisterEffectClassName23}, // "firefly"
            {kUserFireflyEffect, sym::g_textUserfirefly, points::initInlineFx::afterInitEffectClass24, points::initInlineFx::afterRegisterEffectClassName24}, // "userfirefly"
            {kSplashEffect, sym::g_textSplash, points::initInlineFx::afterInitEffectClass25, points::initInlineFx::afterRegisterEffectClassName25}, // "splash"
            {kRippleEffect, sym::g_textRipple, points::initInlineFx::afterInitEffectClass26, points::initInlineFx::afterRegisterEffectClassName26}, // "ripple"
        };

        // The affector types and their names.
        struct NameRow { uint32_t id, name, returnsTo; };
        constexpr NameRow kAffectorTypes[] = {
            {sym::g_gravityAffectorClass, sym::g_textGravity2, points::initInlineFx::afterRegisterEffectTypeName}, // "gravity"
            {sym::g_gravityScreenAffectorClass, sym::g_textGravityScreen, points::initInlineFx::afterRegisterEffectTypeName2}, // "gravity_screen"
            {sym::g_shardGravityAffectorClass, sym::g_textShardGravity, points::initInlineFx::afterRegisterEffectTypeName3}, // "shard_gravity"
            {sym::g_floorCollisionAffectorClass, sym::g_textFloorcoll, points::initInlineFx::afterRegisterEffectTypeName4}, // "floorcoll"
            {sym::g_smokeFloorCollisionAffectorClass, sym::g_textSmokeFloorColl, points::initInlineFx::afterRegisterEffectTypeName5}, // "smoke_floor_coll"
        };

        // What each name makes: its maker, the name (a class's, or "skid", a
        // poster made as its own).
        constexpr NameRow kNamedClasses[] = {
            {sym::startSnow, sym::g_textSnow, points::initInlineFx::afterRegisterNamedEffectClass}, // "snow"
            {sym::startRain, sym::g_textRain, points::initInlineFx::afterRegisterNamedEffectClass2}, // "rain"
            {sym::startSparks, sym::g_textSpark2, points::initInlineFx::afterRegisterNamedEffectClass3}, // "spark"
            {sym::startShard, sym::g_textShard, points::initInlineFx::afterRegisterNamedEffectClass4}, // "shard"
            {sym::startScreenShards, sym::g_textScreenshard, points::initInlineFx::afterRegisterNamedEffectClass5}, // "screenshard"
            {sym::startUserSparks, sym::g_textUserspark, points::initInlineFx::afterRegisterNamedEffectClass6}, // "userspark"
            {sym::startLightning, sym::g_textLightning, points::initInlineFx::afterRegisterNamedEffectClass7}, // "lightning"
            {sym::startVapor, sym::g_textVapor, points::initInlineFx::afterRegisterNamedEffectClass8}, // "vapor"
            {sym::startBloodSquirt, sym::g_textBloodsquirt, points::initInlineFx::afterRegisterNamedEffectClass9}, // "bloodsquirt"
            {sym::startBloodCloud, sym::g_textBloodcloud, points::initInlineFx::afterRegisterNamedEffectClass10}, // "bloodcloud"
            {sym::startBloodPool, sym::g_textBloodpool, points::initInlineFx::afterRegisterNamedEffectClass11}, // "bloodpool"
            {sym::startSplashRoot, sym::g_textSplashroot, points::initInlineFx::afterRegisterNamedEffectClass12}, // "splashroot"
            {sym::startSplashRing, sym::g_textSplashring, points::initInlineFx::afterRegisterNamedEffectClass13}, // "splashring"
            {sym::startSmoke, sym::g_textSmoke, points::initInlineFx::afterRegisterNamedEffectClass14}, // "smoke"
            {sym::startUserSmoke, sym::g_textUsersmoke, points::initInlineFx::afterRegisterNamedEffectClass15}, // "usersmoke"
            {sym::startPoster, sym::g_textSkid, points::initInlineFx::afterRegisterNamedEffectClass16}, // "skid"
            {sym::startPoster, sym::g_textPoster, points::initInlineFx::afterRegisterNamedEffectClass17}, // "poster"
            {sym::startImpactRings, sym::g_textImpactRing, points::initInlineFx::afterRegisterNamedEffectClass18}, // "impactRing"
            {sym::startExplosion, sym::g_textExplosion, points::initInlineFx::afterRegisterNamedEffectClass19}, // "explosion"
            {sym::startDebris, sym::g_textDebris, points::initInlineFx::afterRegisterNamedEffectClass20}, // "debris"
            {sym::startFiretrail, sym::g_textFiretrail, points::initInlineFx::afterRegisterNamedEffectClass21}, // "firetrail"
            {sym::startBubbles, sym::g_textBubble, points::initInlineFx::afterRegisterNamedEffectClass22}, // "bubble"
            {sym::startUserBubbles, sym::g_textUserbubble, points::initInlineFx::afterRegisterNamedEffectClass23}, // "userbubble"
            {sym::startFireflies, sym::g_textFirefly, points::initInlineFx::afterRegisterNamedEffectClass24}, // "firefly"
            {sym::startUserFireflies, sym::g_textUserfirefly, points::initInlineFx::afterRegisterNamedEffectClass25}, // "userfirefly"
            {sym::startSplash, sym::g_textSplash, points::initInlineFx::afterRegisterNamedEffectClass26}, // "splash"
            {sym::startRipples, sym::g_textRipple, points::initInlineFx::afterRegisterNamedEffectClass27}, // "ripple"
        };

        // The affectors each class has: registered through +0x08 of the
        // spark class (the method every class shares).
        struct AffectorRow { uint32_t effectClass, affector, data, returnsTo; };
        constexpr AffectorRow kAffectors[] = {
            {kSparkEffect, sym::g_textGravity2, 0x00000000u, points::initInlineFx::next298}, // "gravity"
            {kShardEffect, sym::g_textShardGravity, 0x00000000u, points::initInlineFx::next304}, // "shard_gravity"
            {kShardEffect, sym::g_textFloorcoll, sym::g_shardFloorBounce, points::initInlineFx::next311}, // "floorcoll"
            {kScreenShardEffect, sym::g_textGravityScreen, 0x00000000u, points::initInlineFx::next317}, // "gravity_screen"
            {kUserSparkEffect, sym::g_textGravity2, 0x00000000u, points::initInlineFx::next323}, // "gravity"
            {kSmokeEffect, sym::g_textSmokeFloorColl, 0x00000000u, points::initInlineFx::next330}, // "smoke_floor_coll"
            {kSplashEffect, sym::g_textGravity2, 0x00000000u, points::initInlineFx::next336}, // "gravity"
        };

        // The textures: id, name, flags, where kept.
        struct MaterialRow { uint32_t id, name, flags, material, returnsTo; };
        constexpr MaterialRow kMaterials[] = {
            {sym::g_textKFxIDImgSpark00, sym::g_textSpark3, 0x08000002u, g_sparkTexture.address, points::initInlineFx::afterLoadFxMaterial}, // kFxID_ImgSpark00, "Spark"
            {sym::g_textKFxIDImgStreakTurbo, sym::g_textTurboStreak, 0x08000002u, g_turboStreakTexture.address, points::initInlineFx::afterLoadFxMaterial2}, // kFxID_ImgStreakTurbo, "TurboStreak"
            {sym::g_textKFxIDImgStreakTrick, sym::g_textTrickStreak, 0x08000002u, g_trickStreakTexture.address, points::initInlineFx::afterLoadFxMaterial3}, // kFxID_ImgStreakTrick, "TrickStreak"
            {sym::g_textKFxIDImgStreakDrop, sym::g_textDropStreak, 0x08000002u, g_dropStreakTexture.address, points::initInlineFx::afterLoadFxMaterial4}, // kFxID_ImgStreakDrop, "DropStreak"
            {sym::g_textKFxIDImgImpactRing, sym::g_textImpactRing2, 0x08000002u, g_impactRingTexture.address, points::initInlineFx::afterLoadFxMaterial5}, // kFxID_ImgImpactRing, "ImpactRing"
            {sym::g_textKFxIDImgSkid, sym::g_textSkid2, 0x08000003u, g_skidTexture.address, points::initInlineFx::afterLoadFxMaterial6}, // kFxID_ImgSkid, "Skid"
            {sym::g_textKFxIDImgFireTrail, sym::g_textFiretrail2, 0x00000002u, g_firetrailTexture.address, points::initInlineFx::afterLoadFxMaterial7}, // kFxID_ImgFireTrail, "Firetrail"
            {sym::g_textKFxIDImgGlass, sym::g_textGlass, 0x00000004u, g_glassTexture.address, points::initInlineFx::afterLoadFxMaterial8}, // kFxID_ImgGlass, "Glass"
            {sym::g_textKFxIDImgIceFace, sym::g_textIceGlass, 0x08000002u, g_iceGlassTexture.address, points::initInlineFx::afterLoadFxMaterial9}, // kFxID_ImgIceFace, "IceGlass"
            {sym::g_textKFxIDImgBloodSmall, sym::g_textBloodSmall, 0x48000004u, g_bloodSmallTexture.address, points::initInlineFx::afterLoadFxMaterial10}, // kFxID_ImgBloodSmall, "BloodSmall"
            {sym::g_textKFxIDImgBloodMedium, sym::g_textBloodMedium, 0x48000004u, g_bloodMediumTexture.address, points::initInlineFx::afterLoadFxMaterial11}, // kFxID_ImgBloodMedium, "BloodMedium"
            {sym::g_textKFxIDImgBloodLarge, sym::g_textBloodLarge, 0x48000004u, g_bloodLargeTexture.address, points::initInlineFx::afterLoadFxMaterial12}, // kFxID_ImgBloodLarge, "BloodLarge"
            {sym::g_textKFxIDImgBloodCloud, sym::g_textBloodCloud, 0x40000004u, g_bloodCloudTexture.address, points::initInlineFx::afterLoadFxMaterial13}, // kFxID_ImgBloodCloud, "BloodCloud"
            {sym::g_textKFxIDImgBloodPool, sym::g_textBloodPool, 0x40000004u, g_bloodPoolTexture.address, points::initInlineFx::afterLoadFxMaterial14}, // kFxID_ImgBloodPool, "BloodPool"
            {sym::g_textKFxIDImgBubble, sym::g_textBubble2, 0x08000002u, g_bubbleTexture.address, points::initInlineFx::afterLoadFxMaterial15}, // kFxID_ImgBubble, "Bubble"
            {sym::g_textKFxIDImgFireFly, sym::g_textFirefly2, 0x08000002u, g_fireflyTexture.address, points::initInlineFx::afterLoadFxMaterial16}, // kFxID_ImgFireFly, "Firefly"
            {sym::g_textKFxIDImgSmokeBall, sym::g_textSmokeBall, 0x08000002u, g_smokeBallTexture.address, points::initInlineFx::afterLoadFxMaterial17}, // kFxID_ImgSmokeBall, "SmokeBall"
            {sym::g_textKFxIDImgSteamBall, sym::g_textSteamBall, 0x08000002u, g_steamBallTexture.address, points::initInlineFx::afterLoadFxMaterial18}, // kFxID_ImgSteamBall, "SteamBall"
            {sym::g_textKFxIDImgSnowBall, sym::g_textSnowBall, 0x08000002u, g_snowBallTexture.address, points::initInlineFx::afterLoadFxMaterial19}, // kFxID_ImgSnowBall, "SnowBall"
            {sym::g_textKFxIDImgFireBall, sym::g_textFireBall, 0x08000002u, g_fireBallTexture.address, points::initInlineFx::afterLoadFxMaterial20}, // kFxID_ImgFireBall, "FireBall"
            {sym::g_textKFxIDImgExplosion, sym::g_textExplosion2, 0x08000002u, g_explosionTexture.address, points::initInlineFx::afterLoadFxMaterial21}, // kFxID_ImgExplosion, "Explosion"
            {sym::g_textKFxIDImgRipple, sym::g_textRipple2, 0x48000004u, g_rippleTexture.address, points::initInlineFx::afterLoadFxMaterial22}, // kFxID_ImgRipple, "Ripple"
            {sym::g_textKFxIDImgSplash, sym::g_textSplash2, 0x08000002u, g_splashTexture.address, points::initInlineFx::afterLoadFxMaterial23}, // kFxID_ImgSplash, "Splash"
            {sym::g_textKFxIDImgSplashRing, sym::g_textSplashRing, 0x48000004u, g_splashRingTexture.address, points::initInlineFx::afterLoadFxMaterial24}, // kFxID_ImgSplashRing, "SplashRing"
            {sym::g_textKFxIDImgSplashRing2, sym::g_textSplashRing2, 0x48000004u, g_splashRing2Texture.address, points::initInlineFx::afterLoadFxMaterial25}, // kFxID_ImgSplashRing2, "SplashRing2"
            {sym::g_textKFxIDImgFireA00, sym::g_textFire00, 0x08000002u, kFireFrames, points::initInlineFx::afterLoadFxMaterial26}, // kFxID_ImgFireA00, "Fire00"
            {sym::g_textKFxIDImgFireA01, sym::g_textFire01, 0x08000002u, kFireFrames + 4, points::initInlineFx::afterLoadFxMaterial27}, // kFxID_ImgFireA01, "Fire01"
            {sym::g_textKFxIDImgFireA02, sym::g_textFire02, 0x08000002u, kFireFrames + 8, points::initInlineFx::afterLoadFxMaterial28}, // kFxID_ImgFireA02, "Fire02"
            {sym::g_textKFxIDImgFireA03, sym::g_textFire03, 0x08000002u, kFireFrames + 12, points::initInlineFx::afterLoadFxMaterial29}, // kFxID_ImgFireA03, "Fire03"
            {sym::g_textKFxIDImgFireA04, sym::g_textFire04, 0x08000002u, kFireFrames + 16, points::initInlineFx::afterLoadFxMaterial30}, // kFxID_ImgFireA04, "Fire04"
            {sym::g_textKFxIDImgFireA05, sym::g_textFire05, 0x08000002u, kFireFrames + 20, points::initInlineFx::afterLoadFxMaterial31}, // kFxID_ImgFireA05, "Fire05"
            {sym::g_textKFxIDImgFireA06, sym::g_textFire06, 0x08000002u, kFireFrames + 24, points::initInlineFx::afterLoadFxMaterial32}, // kFxID_ImgFireA06, "Fire06"
            {sym::g_textKFxIDImgFireA07, sym::g_textFire07, 0x08000002u, kFireFrames + 28, points::initInlineFx::afterLoadFxMaterial33}, // kFxID_ImgFireA07, "Fire07"
            {sym::g_textKFxIDImgFireA08, sym::g_textFire08, 0x08000002u, kFireFrames + 32, points::initInlineFx::afterLoadFxMaterial34}, // kFxID_ImgFireA08, "Fire08"
            {sym::g_textKFxIDImgFireA09, sym::g_textFire09, 0x08000002u, kFireFrames + 36, points::initInlineFx::afterLoadFxMaterial35}, // kFxID_ImgFireA09, "Fire09"
        };

        // The hashes the effects look their classes up by, and the classes.
        struct KeptRow { uint32_t name, kept, returnsTo; };
        constexpr KeptRow kClassHashes[] = {
            {sym::g_textSnow, g_snowHash.address, points::initInlineFx::afterNameHash}, // "snow"
            {sym::g_textLightning, g_lightningHash.address, points::initInlineFx::afterNameHash2}, // "lightning"
            {sym::g_textRain, g_rainHash.address, points::initInlineFx::afterNameHash3}, // "rain"
            {sym::g_textUserbubble, g_userBubbleHash.address, points::initInlineFx::afterNameHash4}, // "userbubble"
            {sym::g_textUserfirefly, g_userFireflyHash.address, points::initInlineFx::afterNameHash5}, // "userfirefly"
            {sym::g_textUsersmoke, g_userSmokeHash.address, points::initInlineFx::afterNameHash6}, // "usersmoke"
            {sym::g_textUserspark, g_userSparkHash.address, points::initInlineFx::afterNameHash7}, // "userspark"
            {sym::g_textBubble, g_bubbleHash.address, points::initInlineFx::afterNameHash8}, // "bubble"
            {sym::g_textFirefly, g_fireflyHash.address, points::initInlineFx::afterNameHash9}, // "firefly"
            {sym::g_textSmoke, g_smokeHash.address, points::initInlineFx::afterNameHash10}, // "smoke"
            {sym::g_textSpark2, g_sparkHash.address, points::initInlineFx::afterNameHash11}, // "spark"
            {sym::g_textImpactRing, g_impactRingHash.address, points::initInlineFx::afterNameHash12}, // "impactRing"
            {sym::g_textExplosion, g_explosionHash.address, points::initInlineFx::afterNameHash13}, // "explosion"
            {sym::g_textDebris, g_debrisHash.address, points::initInlineFx::afterNameHash14}, // "debris"
            {sym::g_textFiretrail, g_firetrailHash.address, points::initInlineFx::afterNameHash15}, // "firetrail"
            {sym::g_textVapor, g_vaporHash.address, points::initInlineFx::afterNameHash16}, // "vapor"
            {sym::g_textBloodsquirt, g_bloodSquirtHash.address, points::initInlineFx::afterNameHash17}, // "bloodsquirt"
            {sym::g_textBloodcloud, g_bloodCloudHash.address, points::initInlineFx::afterNameHash18}, // "bloodcloud"
            {sym::g_textBloodpool, g_bloodPoolHash.address, points::initInlineFx::afterNameHash19}, // "bloodpool"
            {sym::g_textSplashroot, g_splashRootHash.address, points::initInlineFx::afterNameHash20}, // "splashroot"
            {sym::g_textSplashring, g_splashRingHash.address, points::initInlineFx::afterNameHash21}, // "splashring"
            {sym::g_textShard, g_shardHash.address, points::initInlineFx::afterNameHash22}, // "shard"
            {sym::g_textSkid, g_skidHash.address, points::initInlineFx::afterNameHash23}, // "skid"
            {sym::g_textRipple, g_rippleHash.address, points::initInlineFx::afterNameHash24}, // "ripple"
            {sym::g_textScreenshard, g_screenShardHash.address, points::initInlineFx::afterNameHash25}, // "screenshard"
        };
        constexpr KeptRow kClassHandles[] = {
            {sym::g_textSnow, g_snowClass.address, points::initInlineFx::afterFindEffectClassByName}, // "snow"
            {sym::g_textLightning, g_lightningClass.address, points::initInlineFx::afterFindEffectClassByName2}, // "lightning"
            {sym::g_textRain, g_rainClass.address, points::initInlineFx::afterFindEffectClassByName3}, // "rain"
            {sym::g_textUserbubble, g_userBubbleClass.address, points::initInlineFx::afterFindEffectClassByName4}, // "userbubble"
            {sym::g_textUserfirefly, g_userFireflyClass.address, points::initInlineFx::afterFindEffectClassByName5}, // "userfirefly"
            {sym::g_textUsersmoke, g_userSmokeClass.address, points::initInlineFx::afterFindEffectClassByName6}, // "usersmoke"
            {sym::g_textUserspark, g_userSparkClass.address, points::initInlineFx::afterFindEffectClassByName7}, // "userspark"
            {sym::g_textBubble, g_bubbleClass.address, points::initInlineFx::afterFindEffectClassByName8}, // "bubble"
            {sym::g_textFirefly, g_fireflyClass.address, points::initInlineFx::afterFindEffectClassByName9}, // "firefly"
            {sym::g_textSmoke, g_smokeClass.address, points::initInlineFx::afterFindEffectClassByName10}, // "smoke"
            {sym::g_textSpark2, g_sparkClass.address, points::initInlineFx::afterFindEffectClassByName11}, // "spark"
            {sym::g_textImpactRing, g_impactRingClass.address, points::initInlineFx::afterFindEffectClassByName12}, // "impactRing"
            {sym::g_textExplosion, g_explosionClass.address, points::initInlineFx::afterFindEffectClassByName13}, // "explosion"
            {sym::g_textDebris, g_debrisClass.address, points::initInlineFx::afterFindEffectClassByName14}, // "debris"
            {sym::g_textFiretrail, g_firetrailClass.address, points::initInlineFx::afterFindEffectClassByName15}, // "firetrail"
            {sym::g_textVapor, g_vaporClass.address, points::initInlineFx::afterFindEffectClassByName16}, // "vapor"
            {sym::g_textBloodsquirt, g_bloodSquirtClass.address, points::initInlineFx::afterFindEffectClassByName17}, // "bloodsquirt"
            {sym::g_textBloodcloud, g_bloodCloudClass.address, points::initInlineFx::afterFindEffectClassByName18}, // "bloodcloud"
            {sym::g_textBloodpool, g_bloodPoolClass.address, points::initInlineFx::afterFindEffectClassByName19}, // "bloodpool"
            {sym::g_textSplashroot, g_splashRootClass.address, points::initInlineFx::afterFindEffectClassByName20}, // "splashroot"
            {sym::g_textSplashring, g_splashRingClass.address, points::initInlineFx::afterFindEffectClassByName21}, // "splashring"
            {sym::g_textShard, g_shardClass.address, points::initInlineFx::afterFindEffectClassByName22}, // "shard"
            {sym::g_textPoster, g_posterClass.address, points::initInlineFx::afterFindEffectClassByName23}, // "poster"
            {sym::g_textRipple, g_rippleClass.address, points::initInlineFx::afterFindEffectClassByName24}, // "ripple"
            {sym::g_textScreenshard, g_screenShardClass.address, points::initInlineFx::afterFindEffectClassByName25}, // "screenshard"
        };
        // The bones the effects start from: the hashes of their names.
        constexpr KeptRow kBoneHashes[] = {
            {sym::g_textLHand2, sym::g_pointBones, points::initInlineFx::afterNameHash27}, // "L_Hand"
            {sym::g_textRHand, sym::g_boneHashRightHand, points::initInlineFx::afterNameHash28}, // "R_Hand"
            {sym::g_textLFoot2, sym::g_boneHashLeftFoot, points::initInlineFx::afterNameHash29}, // "L_Foot"
            {sym::g_textRFoot, sym::g_boneHashRightFoot, points::initInlineFx::afterNameHash30}, // "R_Foot"
            {sym::g_textLForeArm, sym::g_lForeArmHash, points::initInlineFx::afterNameHash31}, // "L_ForeArm"
            {sym::g_textRForeArm, sym::g_rForeArmHash, points::initInlineFx::afterNameHash32}, // "R_ForeArm"
            {sym::g_textLThigh, sym::g_lThighHash, points::initInlineFx::afterNameHash33}, // "L_Thigh"
            {sym::g_textRThigh, sym::g_rThighHash, points::initInlineFx::afterNameHash34}, // "R_Thigh"
            {sym::g_textLUpperArm2, sym::g_lUpperArmHash, points::initInlineFx::afterNameHash35}, // "L_UpperArm"
            {sym::g_textRUpperArm2, sym::g_rUpperArmHash, points::initInlineFx::afterNameHash36}, // "R_UpperArm"
            {sym::g_textHips3, sym::g_hipsHash22, points::initInlineFx::afterNameHash37}, // "Hips"
            {sym::g_textHead2, sym::g_headHash, points::initInlineFx::afterNameHash38}, // "Head"
        };

        // What the original keeps in its saved registers as it goes on (the
        // addresses it uses more than once): from the call that returns to
        // `from` on, the register holds `value`.

        constexpr NameRow kClassNamesFreed[] = {
            {0, sym::g_textSnow, points::shutdownInlineFx::afterUnregisterEffectClassName}, // "snow"
            {0, sym::g_textRain, points::shutdownInlineFx::afterUnregisterEffectClassName2}, // "rain"
            {0, sym::g_textSpark2, points::shutdownInlineFx::afterUnregisterEffectClassName3}, // "spark"
            {0, sym::g_textShard, points::shutdownInlineFx::afterUnregisterEffectClassName4}, // "shard"
            {0, sym::g_textScreenshard, points::shutdownInlineFx::afterUnregisterEffectClassName5}, // "screenshard"
            {0, sym::g_textUserspark, points::shutdownInlineFx::afterUnregisterEffectClassName6}, // "userspark"
            {0, sym::g_textLightning, points::shutdownInlineFx::afterUnregisterEffectClassName7}, // "lightning"
            {0, sym::g_textVapor, points::shutdownInlineFx::afterUnregisterEffectClassName8}, // "vapor"
            {0, sym::g_textBloodsquirt, points::shutdownInlineFx::afterUnregisterEffectClassName9}, // "bloodsquirt"
            {0, sym::g_textBloodcloud, points::shutdownInlineFx::afterUnregisterEffectClassName10}, // "bloodcloud"
            {0, sym::g_textBloodpool, points::shutdownInlineFx::afterUnregisterEffectClassName11}, // "bloodpool"
            {0, sym::g_textSplashroot, points::shutdownInlineFx::afterUnregisterEffectClassName12}, // "splashroot"
            {0, sym::g_textSplashring, points::shutdownInlineFx::afterUnregisterEffectClassName13}, // "splashring"
            {0, sym::g_textSmoke, points::shutdownInlineFx::afterUnregisterEffectClassName14}, // "smoke"
            {0, sym::g_textUsersmoke, points::shutdownInlineFx::afterUnregisterEffectClassName15}, // "usersmoke"
            {0, sym::g_textPoster, points::shutdownInlineFx::afterUnregisterEffectClassName16}, // "poster"
            {0, sym::g_textImpactRing, points::shutdownInlineFx::afterUnregisterEffectClassName17}, // "impactRing"
            {0, sym::g_textExplosion, points::shutdownInlineFx::afterUnregisterEffectClassName18}, // "explosion"
            {0, sym::g_textDebris, points::shutdownInlineFx::afterUnregisterEffectClassName19}, // "debris"
            {0, sym::g_textFiretrail, points::shutdownInlineFx::afterUnregisterEffectClassName20}, // "firetrail"
            {0, sym::g_textBubble, points::shutdownInlineFx::afterUnregisterEffectClassName21}, // "bubble"
            {0, sym::g_textUserbubble, points::shutdownInlineFx::afterUnregisterEffectClassName22}, // "userbubble"
            {0, sym::g_textFirefly, points::shutdownInlineFx::afterUnregisterEffectClassName23}, // "firefly"
            {0, sym::g_textUserfirefly, points::shutdownInlineFx::afterUnregisterEffectClassName24}, // "userfirefly"
            {0, sym::g_textSplash, points::shutdownInlineFx::afterUnregisterEffectClassName25}, // "splash"
            {0, sym::g_textRipple, points::shutdownInlineFx::afterUnregisterEffectClassName26}, // "ripple"
        };

        // Each class's affectors cleared at the shut-down, through a class's
        // reset (its own, or as the original does, the spark's or the
        // shard's: the same function in every table).
        struct ClassShutdownRow { uint32_t method, effectClass, returnsTo; };
        constexpr ClassShutdownRow kClassShutdowns[] = {
            {kSparkEffect, kSnowEffect, points::shutdownInlineFx::next69},
            {kSparkEffect, kRainEffect, points::shutdownInlineFx::next73},
            {kSparkEffect, kSparkEffect, points::shutdownInlineFx::next76},
            {kScreenShardEffect, kScreenShardEffect, points::shutdownInlineFx::next80},
            {kShardEffect, kShardEffect, points::shutdownInlineFx::next83},
            {kShardEffect, kUserSparkEffect, points::shutdownInlineFx::next87},
            {kLightningEffect, kLightningEffect, points::shutdownInlineFx::next91},
            {kVaporEffect, kVaporEffect, points::shutdownInlineFx::next95},
            {kBloodSquirtEffect, kBloodSquirtEffect, points::shutdownInlineFx::next99},
            {kBloodCloudEffect, kBloodCloudEffect, points::shutdownInlineFx::next103},
            {kBloodPoolEffect, kBloodPoolEffect, points::shutdownInlineFx::next107},
            {kSplashRootEffect, kSplashRootEffect, points::shutdownInlineFx::next111},
            {kSplashRingEffect, kSplashRingEffect, points::shutdownInlineFx::next115},
            {kSmokeEffect, kSmokeEffect, points::shutdownInlineFx::next119},
            {kUserSmokeEffect, kUserSmokeEffect, points::shutdownInlineFx::next123},
            {kPosterEffect, kPosterEffect, points::shutdownInlineFx::next127},
            {kImpactRingEffect, kImpactRingEffect, points::shutdownInlineFx::next131},
            {kExplosionEffect, kExplosionEffect, points::shutdownInlineFx::next135},
            {kDebrisEffect, kDebrisEffect, points::shutdownInlineFx::next139},
            {kFiretrailEffect, kFiretrailEffect, points::shutdownInlineFx::next143},
            {kBubbleEffect, kBubbleEffect, points::shutdownInlineFx::next147},
            {kUserBubbleEffect, kUserBubbleEffect, points::shutdownInlineFx::next151},
            {kFireflyEffect, kFireflyEffect, points::shutdownInlineFx::next155},
            {kUserFireflyEffect, kUserFireflyEffect, points::shutdownInlineFx::next159},
            {kRippleEffect, kRippleEffect, points::shutdownInlineFx::next163},
            {kSplashEffect, kSplashEffect, points::shutdownInlineFx::next167},
        };

        constexpr KeptRow kMaterialsReleased[] = {
            {0, g_sparkTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial},
            {0, g_turboStreakTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial2},
            {0, g_trickStreakTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial3},
            {0, g_dropStreakTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial4},
            {0, g_impactRingTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial5},
            {0, g_smokeBallTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial6},
            {0, g_steamBallTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial7},
            {0, g_snowBallTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial8},
            {0, g_fireBallTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial9},
            {0, g_skidTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial10},
            {0, g_bloodSmallTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial11},
            {0, g_bloodMediumTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial12},
            {0, g_bloodLargeTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial13},
            {0, g_bloodCloudTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial14},
            {0, g_bloodPoolTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial15},
            {0, g_firetrailTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial16},
            {0, g_glassTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial17},
            {0, g_iceGlassTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial18},
            {0, g_bubbleTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial19},
            {0, g_fireflyTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial20},
            {0, g_explosionTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial21},
            {0, g_rippleTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial22},
            {0, g_splashTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial23},
            {0, g_splashRingTexture.address, points::shutdownInlineFx::afterReleaseFxMaterial24},
            {0, g_splashRing2Texture.address, points::shutdownInlineFx::afterReleaseFxMaterial25},
            {0, kFireFrames, points::shutdownInlineFx::afterReleaseFxMaterial26},
            {0, kFireFrames + 4, points::shutdownInlineFx::afterReleaseFxMaterial27},
            {0, kFireFrames + 8, points::shutdownInlineFx::afterReleaseFxMaterial28},
            {0, kFireFrames + 12, points::shutdownInlineFx::afterReleaseFxMaterial29},
            {0, kFireFrames + 16, points::shutdownInlineFx::afterReleaseFxMaterial30},
            {0, kFireFrames + 20, points::shutdownInlineFx::afterReleaseFxMaterial31},
            {0, kFireFrames + 24, points::shutdownInlineFx::afterReleaseFxMaterial32},
            {0, kFireFrames + 28, points::shutdownInlineFx::afterReleaseFxMaterial33},
            {0, kFireFrames + 32, points::shutdownInlineFx::afterReleaseFxMaterial34},
            {0, kFireFrames + 36, points::shutdownInlineFx::afterReleaseFxMaterial35},
        };

        // The saved registers the original holds, as the tables above say.
    }

    // initInlineFx: the level's effects made ready (loadLevel). Levels with
    // weather start it; levels 1, 4 and 5 (and 8) have the sun's lens flare
    // at a place of their own.
    void initInlineFx()
    {
        *g_inlineFxReady = 1;
        *g_tickTime = ee::Float(0.0f);
        *g_clockMs = ee::Float(0.0f);
        *g_weatherActive = 0;
        *g_weatherHeld = 0;
        *g_clearWeather = 0;
        *g_lensFlareTask = 0;
        const uint32_t library = zge::openAssetLibrary(kFxLibrary);
        script::scriptRegisterNative(ee::Ptr<const char>(kAllTouchedName), kAllTouchedNative);
        script::scriptRegisterNative(ee::Ptr<const char>(kResetTouchedName), kResetTouchedNative);
        zge::initEffectTables();
        *g_particleBudget = 0x200;
        const ee::Ptr<zge::SceneObject> emitter = zge::newSceneEmitter(0);
        *g_effectsObject = emitter;
        zge::setEmitterOwner(emitter, 0);

        for (const EffectClassRow &row : kEffectClasses)
            zge::initEffectClass(ee::Ptr<zge::EffectClass>(row.effectClass));
        for (const EffectClassRow &row : kEffectClasses)
            zge::registerEffectClassName(ee::Ptr<zge::EffectClass>(row.effectClass), row.name);
        for (const NameRow &row : kAffectorTypes)
            zge::registerEffectTypeName(ee::Ptr<zge::Affector>(row.id), row.name);
        for (const NameRow &row : kNamedClasses)
            zge::registerNamedEffectClass(row.id, row.name);
        const ee::Ptr<ClassMethods> sparkClass(kSparkEffect);
        for (const AffectorRow &row : kAffectors)
            sparkClass->addNamedAffector(row.effectClass, row.affector, row.data);
        for (const MaterialRow &row : kMaterials)
            loadFxMaterial(library, row.id, row.name, row.flags, ee::Ptr<uint32_t>(row.material));
        zge::closeAssetLibrary(library);

        for (const KeptRow &row : kClassHashes)
            *ee::Ptr<uint32_t>(row.kept) = zge::nameHash(ee::Ptr<const char>(row.name));
        for (const KeptRow &row : kClassHandles)
            *ee::Ptr<ee::Ptr<zge::EffectClass>>(row.kept) = zge::findEffectClassByName(row.name);
        const uint32_t debrisHash = zge::nameHash(ee::Ptr<const char>(kDebrisObjectName));
        *g_debrisObject = zge::findWorldObjectByHash(debrisHash);
        for (const KeptRow &row : kBoneHashes)
            *ee::Ptr<uint32_t>(row.kept) = zge::nameHash(ee::Ptr<const char>(row.name));
        loadUserEffects();
        setupMovers();
        *g_fxTaskHandle = zge::newTask(0, kFxTask, 0, 0, kInlineFxSource, 1892);
        *g_fxDrawTaskHandle = zge::newTask(1, kFxDrawTask, 0, 1, kInlineFxSource, 1894);
        *g_fxTable = 0;

        if (*g_gameType != GameType::SplitScreen)
        {
            const auto setFlare = [](ee::Float x, ee::Float y, ee::Float z) {
                g_lensFlarePosition->x = x;
                g_lensFlarePosition->y = y;
                g_lensFlarePosition->z = z;
                g_lensFlarePosition->w = 0.0f;
            };
            switch (*g_currentLevel)
            {
            case 0:
                startLevelWeather(0);
                break;
            case 3:
                startLevelWeather(4);
                break;
            case 1:
                setFlare(ee::Float(50.0f), ee::Float(-50.0f), ee::Float(-140.0f));
                break;
            case 4:
                setFlare(ee::Float(-94.0f), ee::Float(-58.0f), ee::Float(-213.0f));
                break;
            case 5:
            case 8:
                setFlare(ee::Float(50.0f), ee::Float(-100.0f), ee::Float(-140.0f));
                break;
            case 2:
            case 6:
            case 7:
            case 9:
                break;
            default:
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(initInlineFx, Verified);

    // shutdownInlineFx: all of it taken down again.
    void shutdownInlineFx()
    {
        const ee::Ptr<zge::Task> drawTask = *g_fxDrawTaskHandle;
        *g_inlineFxReady = 0;
        zge::deleteTask(drawTask);
        zge::deleteTask(*g_fxTaskHandle);
        const ee::Ptr<zge::Task> flareTask = *g_lensFlareTask;
        if (flareTask)
            zge::deleteTask(flareTask);

        for (const NameRow &row : kClassNamesFreed)
            zge::unregisterEffectClassName(row.name);
        for (const ClassShutdownRow &row : kClassShutdowns)
            ee::Ptr<ClassMethods>(row.method)->reset(row.effectClass);
        for (const KeptRow &row : kMaterialsReleased)
            releaseFxMaterial(ee::Ptr<uint32_t>(row.kept));
        *g_debrisObject = 0;
        stopLevelWeather();
        freeUserEffects();
        freeMovers();
        zge::freeNamedEffectClasses();
        *g_effectsObject = {};
    }
    READABLE_FUNCTION_WAITS(shutdownInlineFx, Verified);

    namespace
    {
        constexpr uint32_t kPointerArrayAdd_ = sym::pointerArrayAdd; // pointerArrayAdd ((CursorArray *, item); readable: pointerArrayAdd_, Verified)
        inline ee::Global<int32_t, sym::g_effectClassNames> g_effectClassNames;
        inline ee::Global<int32_t, sym::g_effectTypeNames> g_effectTypeNames;
        inline ee::Global<int32_t, sym::g_namedEffectClasses> g_namedEffectClasses; // g_namedEffectClasses (zge::(anonymous)::g_namedEffectClasses)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(pointerArrayAdd_int32_t, ee::game<int32_t>, kPointerArrayAdd_);
        }
    }

}
