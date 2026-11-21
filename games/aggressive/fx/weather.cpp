// The weather ("snow" and "rain"; inlineFx.c, were the sweep's drafts of
// loadSnowTexture .. drawRain): flakes and drops made around the camera, within
// its field of view either way of where it looks, at a random distance; the
// flakes from a little above player one, the drops from high up. Their
// textures, starts, spawns, moves and draws.
//

#include "aggressive/camera.h"
#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/level/level_object.h"
#include "aggressive/skater/skater.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/tasks.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"

#include <cstring>
#include "sym/aggressive/fx/weather.cpp.h"

namespace zge
{
}

namespace aggressive::fx
{
    uint32_t loadSnowTexture();
    uint32_t loadRainTexture();

    namespace
    {
        constexpr uint32_t kCameraObject = sym::cameraObject;      // (camera) -> its object (+0x0C)
        constexpr uint32_t kAllocateSized = sym::allocateSized;     // (bytes) -> a block of the smallest size-class pool that fits
        // (library, texture id name, material name, flags, &texture): an
        // effects texture loaded from the library (user_effects.cpp's).
        constexpr uint32_t kLoadMatFxTextures = sym::loadFxMaterial;
        constexpr uint32_t kFxLibrary = sym::fxLibrary;         // "assets\fx.zal"
        constexpr uint32_t kSnowTextureId = sym::snowTextureId;     // "kFxID_ImgSnow00"
        constexpr uint32_t kSnowMaterial = sym::snowMaterial;      // "Snow"
        constexpr uint32_t kRainTextureId = sym::rainTextureId;     // "kFxID_ImgRain00"
        constexpr uint32_t kRainMaterial = sym::rainMaterial;      // "Rain"
        constexpr uint32_t kSnowTextureFlags = 0x08000002u, kRainTextureFlags = 0x08000004u;
        inline ee::Global<uint8_t[4], sym::snowColour> g_snowColour; // RGBA
        inline ee::Global<uint8_t[4], sym::rainColour> g_rainColour;

        // Set: the weather is not moved; with g_clearWeather also set, all
        // of it freed at the next move (and the request cleared) [inferred].
        inline ee::Global<uint32_t, sym::g_weatherHeld> g_weatherHeld;
        inline ee::Global<uint32_t, sym::g_clearWeather> g_clearWeather;
        // The wind every puff, drop and flake drifts with (units a second).
        inline ee::Global<zge::Vec4, sym::g_smokeWind> g_smokeWind;
        // Degrees to 16-bit angle units (.sdata copies of 182.04).
        inline ee::Global<ee::Float, sym::g_snowDegreesToAngle> g_snowDegreesToAngle;
        inline ee::Global<ee::Float, sym::g_rainDegreesToAngle> g_rainDegreesToAngle;
        // A flake's wobble on x a tick (the table by a random
        // number's low three bits: 0 right, 1 left, 2 and 3 twice right, 4
        // and 5 twice left, 6 and 7 none).
        inline ee::Global<ee::Float, sym::g_flakeWobbleRight> g_flakeWobbleRight;
        inline ee::Global<ee::Float, sym::g_flakeWobbleLeft> g_flakeWobbleLeft;
        inline ee::Global<ee::Float, sym::g_flakeWobbleFarRight> g_flakeWobbleFarRight;
        inline ee::Global<ee::Float, sym::g_flakeWobbleFarLeft> g_flakeWobbleFarLeft;
        inline constexpr ee::Float kRandomStep = 0.0078125f; // 1/128: a random byte's step in [-1, 1)
        inline constexpr ee::Float kFlakeRise = 12.0f;       // above player one (y down)
        inline constexpr ee::Float kFlakeRiseSpread = 4.0f;  // and 0 .. 7 less this
        inline constexpr ee::Float kDropRiseSpread = 8.0f;   // a drop's height: 0 .. 15 less this
        inline constexpr ee::Float kDropHeight = -60.0f;     // and this

        // What the weather starts from (the named effect's parameters; with
        // none, the defaults at weatherDefaults: falling at 5 give or take 3, size
        // 1, from 2 to 50 away, living 5 seconds give or take 2).
        struct WeatherParameters
        {
            zge::Vec4 velocity;          // +0x00
            zge::Vec4 velocitySpread;    // +0x10
            ee::Float size;              // +0x20
            ee::Float farthest;          // +0x24 from the camera
            ee::Float nearest;           // +0x28
            ee::Float lifetime;          // +0x2C milliseconds
            ee::Float lifetimeSpread;    // +0x30
            uint8_t count;               // +0x34 a start's
        };
        static_assert(offsetof(WeatherParameters, size) == 0x20 && offsetof(WeatherParameters, count) == 0x34);
        constexpr uint32_t kWeatherDefaults = sym::weatherDefaults;

        // A point `distance` from the eye, in a random direction within the
        // camera's field of view either way of `heading` (both in the level's
        // plane); its height is the caller's.
        void aroundCamera(zge::Vec4 &point, ee::Float distance, const zge::Vec4 &eye, int32_t heading, const ee::Float &degreesToAngle);

        // The weather's walk when the script asks for it cleared: every
        // particle freed, once a tick. Returns how many.
        int32_t clearWeather(ee::Ptr<Emitter> emitter, uint32_t clock)
        {
            int32_t counted = 0;
            if (emitter && emitter->particles && emitter->lastMoved < clock)
            {
                for (uint32_t particle = first(*emitter->particles); particle; ++counted)
                    particle = freeCurrent(emitter, particle);
                emitter->lastMoved = clock;
            }
            *g_clearWeather = 0;
            return counted;
        }

        ee::Float randomUnit(ee::Float step)
        {
            const uint32_t r = zge::randomNumber();
            return ee::toFloat(static_cast<int32_t>((r & 0xFFu) - 0x80)) * step;
        }
        // A random number modulo n (DIVU: a zero divisor leaves the dividend,
        // as the recompiled code has it; the original then raises BREAK 7,
        // which no game run reaches).
        uint32_t randomModulo(uint32_t r, uint32_t n)
        {
            return n ? r % n : r;
        }

        // An effects texture loaded from assets\fx.zal (the library opened
        // and closed around it).
        uint32_t loadFxTexture(uint32_t textureId, uint32_t material, uint32_t flags, ee::Ptr<uint32_t> texture)
        {
            const uint32_t library = zge::openAssetLibrary(kFxLibrary);
            loadFxMaterial(library, textureId, material, flags, texture);
            return game::tail(zge::closeAssetLibrary, library);
        }
    }

    // loadSnowTexture: the snow's texture loaded ("Snow").
    uint32_t loadSnowTexture()
    {
        return loadFxTexture(kSnowTextureId, kSnowMaterial, kSnowTextureFlags, g_snowTexture.get());
    }
    READABLE_FUNCTION_WAITS(loadSnowTexture, Verified);

    // startSnow: snow, the parameters' count of flakes.
    uint32_t startSnow(ee::Ptr<zge::SceneObject> object, ee::Ptr<WeatherParameters> snow)
    {
        return game::tail(zge::startEffect, object, *g_snowClass, snow->count, 0, snow);
    }
    READABLE_FUNCTION_WAITS(startSnow, Verified);

    namespace
    {
        void aroundCamera(zge::Vec4 &point, ee::Float distance, const zge::Vec4 &eye, int32_t heading, const ee::Float &degreesToAngle)
        {
            const int32_t fov = ee::toInt(g_followCamera->fov);
            const uint32_t r = zge::randomNumber();
            const uint32_t degrees = randomModulo(r, static_cast<uint32_t>(fov << 1));
            point.z = distance;
            point.x = 0.0f;
            point.y = 0.0f;
            point.w = 0.0f;
            const int32_t turn = -(heading + ee::toInt(ee::toFloat(static_cast<int32_t>(degrees - fov)) * degreesToAngle));
            zge::vec3RotateY(asVec3(point), asVec3(point), static_cast<zge::Angle>(turn));
            point = point + eye;
        }

        // nearest .. farthest (whole units) at random.
        ee::Float randomDistance(const WeatherParameters &weather)
        {
            const uint32_t r = zge::randomNumber();
            const ee::Float nearest = weather.nearest;
            const uint32_t span = static_cast<uint32_t>(ee::toInt(weather.farthest - nearest));
            return nearest + ee::toFloat(static_cast<int32_t>(randomModulo(r, span)));
        }
    }

    // spawnSnow ("snow" +0x18): `count` flakes, each at a random distance
    // (nearest .. farthest) from the camera in a random direction within its
    // field of view either way of where it looks, a little above player
    // one, with the velocity (and its spread), size and lifetime (and its
    // spread).
    void spawnSnow(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<WeatherParameters> given)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> lookAt;
        ee::Local<zge::Vec4> point;
        ee::Local<int32_t> born;
        ee::Local<int32_t> heading;
        const uint32_t wanted = count & 0xFFu;
        *born = effectsClock();
        std::memcpy(&colour[0], *g_snowColour, 4);
        const ee::Ptr<WeatherParameters> snow = given ? given : ee::Ptr<WeatherParameters>(kWeatherDefaults);
        *eye = g_followCamera->eye;
        *lookAt = g_followCamera->lookAt;
        *heading = static_cast<short>(zge::angleAtan2(lookAt->x - eye->x, lookAt->z - eye->z));
        const skater::Player &playerOne = parts::currentLevel()->players[0];
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Particle> flake = newParticle(*emitter);
            if (!flake)
                break;
            flake->flags = kParticleMoves;
            flake->born = static_cast<uint32_t>(*born);
            flake->texture = *g_snowTexture;
            flake->size = snow->size;
            std::memcpy(flake->colour, &colour[0], 4);
            flake->lifetime = static_cast<uint32_t>(ee::toInt(snow->lifetime + snow->lifetimeSpread * randomUnit(kRandomStep)));
            const ee::Float distance = randomDistance(*snow);
            aroundCamera(point, distance, eye, *heading, *g_snowDegreesToAngle);
            const uint32_t r = zge::randomNumber();
            point->y = playerOne.drawnPosition.y - kFlakeRise + (ee::toFloat(static_cast<int32_t>(r & 7u)) - kFlakeRiseSpread);
            flake->tail = *point;
            flake->position = flake->tail;
            flake->velocity.x = snow->velocity.x + snow->velocitySpread.x * randomUnit(kRandomStep);
            flake->velocity.y = snow->velocity.y + snow->velocitySpread.y * randomUnit(kRandomStep);
            flake->velocity.z = snow->velocity.z + snow->velocitySpread.z * randomUnit(kRandomStep);
            zge::pointerArrayAdd(emitter->particles, flake);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSnow, Verified);

    // moveSnow ("snow" +0x1C): while the weather is held, nothing (or,
    // asked to, every flake freed); else once a tick flakes whose life is
    // over are freed, the others fall by their velocity and the wind over
    // the tick and wobble on x at random. Returns how many it freed or
    // looked at.
    int32_t moveSnow(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t counted = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        const uint32_t clearing = *g_clearWeather;
        if (*g_weatherHeld)
            return clearing ? clearWeather(emitter, clock) : 0;
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++counted)
        {
            Particle &flake = *ee::Ptr<Particle>(particle);
            if (clock - flake.born >= flake.lifetime)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            if (flake.flags & kParticleMoves)
            {
                flake.tail = flake.position;
                step = flake.velocity * *g_tickTime;
                flake.position = flake.position + *step;
                step = *g_smokeWind * *g_tickTime;
                flake.position = flake.position + *step;
                switch (zge::randomNumber() & 7u)
                {
                case 0:
                    flake.position.x = flake.position.x + *g_flakeWobbleRight;
                    break;
                case 1:
                    flake.position.x = flake.position.x - *g_flakeWobbleLeft;
                    break;
                case 2:
                case 3:
                    flake.position.x = flake.position.x + *g_flakeWobbleFarRight;
                    break;
                case 4:
                case 5:
                    flake.position.x = flake.position.x - *g_flakeWobbleFarLeft;
                    break;
                default: // 6, 7: none
                    break;
                }
            }
            particle = next(*emitter->particles);
        }
        emitter->lastMoved = clock;
        return counted;
    }
    READABLE_FUNCTION_WAITS(moveSnow, Verified);

    // drawSnow ("snow" +0x20): each flake a billboard of its size,
    // texture and colour. Returns how many it drew.
    int32_t drawSnow(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        listCorners(billboard->quad);
        int32_t drawn = 0;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const Particle &flake = *ee::Ptr<Particle>(particle);
            std::memcpy(billboard->colour, flake.colour, 4);
            billboard->quad.record.texture = flake.texture;
            placeBillboard(billboard, currentCameraMatrix(), flake.position);
            ++drawn;
            placeCorners(billboard, flake.size);
            drawBillboard(billboard);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSnow, Verified);

    // loadRainTexture: the rain's texture loaded ("Rain").
    uint32_t loadRainTexture()
    {
        return loadFxTexture(kRainTextureId, kRainMaterial, kRainTextureFlags, g_rainTexture.get());
    }
    READABLE_FUNCTION_WAITS(loadRainTexture, Verified);

    // startRain: rain, the parameters' count of drops.
    uint32_t startRain(ee::Ptr<zge::SceneObject> object, ee::Ptr<WeatherParameters> rain)
    {
        return game::tail(zge::startEffect, object, *g_rainClass, rain->count, 0, rain);
    }
    READABLE_FUNCTION_WAITS(startRain, Verified);

    // spawnRain ("rain" +0x18): `count` drops, placed as the snow's
    // flakes but high above (y -68 .. -53).
    void spawnRain(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<WeatherParameters> given)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> lookAt;
        ee::Local<zge::Vec4> point;
        ee::Local<int32_t> born;
        const uint32_t wanted = count & 0xFFu;
        *born = effectsClock();
        std::memcpy(&colour[0], *g_rainColour, 4);
        const ee::Ptr<WeatherParameters> rain = given ? given : ee::Ptr<WeatherParameters>(kWeatherDefaults);
        *eye = g_followCamera->eye;
        *lookAt = g_followCamera->lookAt;
        const int32_t heading = static_cast<short>(zge::angleAtan2(lookAt->x - eye->x, lookAt->z - eye->z));
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Particle> drop = newParticle(*emitter);
            if (!drop)
                break;
            drop->flags = kParticleMoves;
            drop->born = static_cast<uint32_t>(*born);
            drop->texture = *g_rainTexture;
            drop->size = rain->size;
            std::memcpy(drop->colour, &colour[0], 4);
            drop->lifetime = static_cast<uint32_t>(ee::toInt(rain->lifetime + rain->lifetimeSpread * randomUnit(kRandomStep)));
            const ee::Float distance = randomDistance(*rain);
            aroundCamera(point, distance, eye, heading, *g_rainDegreesToAngle);
            const uint32_t r = zge::randomNumber();
            point->y = ee::toFloat(static_cast<int32_t>(r & 0xFu)) - kDropRiseSpread + kDropHeight;
            drop->tail = *point;
            drop->position = drop->tail;
            drop->velocity.x = rain->velocity.x + rain->velocitySpread.x * randomUnit(kRandomStep);
            drop->velocity.y = rain->velocity.y + rain->velocitySpread.y * randomUnit(kRandomStep);
            drop->velocity.z = rain->velocity.z + rain->velocitySpread.z * randomUnit(kRandomStep);
            zge::pointerArrayAdd(emitter->particles, drop);
        }
    }
    READABLE_FUNCTION_WAITS(spawnRain, Verified);

    // moveRain ("rain" +0x1C): while the weather is held, nothing (or,
    // asked to, every drop freed); else once a tick drops whose life is
    // over or which have fallen below 0 are freed, the others fall by
    // their velocity and the wind over the tick. Returns how many it freed
    // or looked at.
    int32_t moveRain(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t counted = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        const uint32_t clearing = *g_clearWeather;
        if (*g_weatherHeld)
            return clearing ? clearWeather(emitter, clock) : 0;
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++counted)
        {
            Particle &drop = *ee::Ptr<Particle>(particle);
            if (!(clock - drop.born < drop.lifetime) || ee::Float(0.0f) < drop.position.y)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            if (drop.flags & kParticleMoves)
            {
                drop.tail = drop.position;
                step = drop.velocity * *g_tickTime;
                drop.position = drop.position + *step;
                step = *g_smokeWind * *g_tickTime;
                drop.position = drop.position + *step;
            }
            particle = next(*emitter->particles);
        }
        emitter->lastMoved = clock;
        return counted;
    }
    READABLE_FUNCTION_WAITS(moveRain, Verified);

    // drawRain ("rain" +0x20): each drop a streak facing the camera
    // (half its size wide, its size high), its first two corners (v 0)
    // clear. Returns how many it drew.
    int32_t drawRain(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        listCorners(billboard->quad);
        int32_t drawn = 0;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const Particle &drop = *ee::Ptr<Particle>(particle);
            std::memcpy(billboard->colour, drop.colour, 4);
            const ee::Float halfWidth = drop.size * ee::Float(0.5f);
            billboard->quad.record.texture = drop.texture;
            placeBillboard(billboard, *zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(*g_currentCamera), 3), drop.position);
            ++drawn;
            for (uint32_t i = 0; i < 4; ++i)
            {
                const bool left = ((i + 1) & 2u) != 0;
                const bool bottom = (i & 2u) != 0;
                QuadVertex &corner = billboard->quad.corners[i];
                std::memcpy(corner.colour, billboard->colour, 4);
                if (!bottom)
                    corner.colour[3] = 0;
                corner.u = left ? 0.0f : 1.0f;
                corner.v = bottom ? 1.0f : 0.0f;
                billboard->corner.x = left ? -halfWidth : halfWidth;
                billboard->corner.y = bottom ? drop.size : -drop.size;
                billboard->corner.z = 0.0f;
                corner.position = zge::transformed(billboard->camera, billboard->corner);
            }
            drawBillboard(billboard);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawRain, Verified);

    // ---- the level's weather (inlineFx.c) -----------------------------------
    namespace
    {
        // A level's weather (its Globals' or the script's): what its textures
        // and spawns are.
        enum LevelWeather : uint32_t
        {
            kWeatherSnow = 0,
            kWeatherSnowLeft = 1,  // its texture, nothing spawned
            kWeatherLightRain = 2,
            kWeatherRain = 3,
            kWeatherStorm = 4,     // rain, lightning and thunder flashes
            kWeatherSun = 5,       // a white lens flare
            kWeatherDusk = 6,      // an orange one
            kWeatherNone = 7,
            kWeatherKinds = 8,
        };
        inline ee::Global<uint32_t, sym::g_weatherActive> g_weatherActive;
        inline ee::Global<uint32_t, sym::g_levelWeather> g_levelWeather;
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_lensFlareTask> g_lensFlareTask;
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject;
        // The storm: seconds to the next burst of lightning and to the next
        // bolt, the bolts so far and in this burst.
        inline ee::Global<ee::Float, sym::g_stormPause> g_stormPause;
        inline ee::Global<ee::Float, sym::g_nextBolt> g_nextBolt;
        inline ee::Global<uint8_t, sym::g_stormBolts> g_stormBolts;
        inline ee::Global<uint8_t, sym::g_stormBurst> g_stormBurst;
        inline ee::Global<uint32_t, sym::g_sunFlareColour> g_sunFlareColour;  // 0xFFFFFFFF
        inline ee::Global<uint32_t, sym::g_duskFlareColour> g_duskFlareColour; // 0xFF648CAF
        inline ee::Global<uint32_t, sym::g_boltFlashColour> g_boltFlashColour; // 0x80FFFFFF
        inline ee::Global<ee::Float, sym::g_boltFlashSeconds> g_boltFlashSeconds; // 0.25

        inline ee::Global<zge::Vec4, sym::g_lensFlarePosition> g_lensFlarePosition; // the level's sun
        constexpr uint32_t kSnowEmission = sym::snowEmission;      // the emissions' parameters (.data)
        constexpr uint32_t kLightRainEmission = sym::lightRainEmission;
        constexpr uint32_t kRainEmission = sym::rainEmission;
        constexpr uint32_t kDrawLevelLensFlare = sym::drawLevelLensFlare;
        constexpr uint32_t kInlineFxSource = sym::inlineFxSource;    // "./src/com/inlineFx.c"
    }

    // startLevelWeather: a level's weather starts: its textures, or for the sun
    // the lens flare and a task to draw it. Once only: none while one runs.
    void startLevelWeather(uint32_t weather)
    {
        if (*g_weatherActive == 0)
        {
            *g_levelWeather = weather;
            *g_weatherActive = 1;
            const auto startFlare = [&](uint32_t colour, int32_t sourceLine) {
                placeLensFlare(g_lensFlarePosition.get(), colour);
                *g_lensFlareTask = zge::newTask(1, kDrawLevelLensFlare, 0, 1, kInlineFxSource, sourceLine);
            };
            switch (weather)
            {
            case kWeatherSnow:
            case kWeatherSnowLeft:
                loadSnowTexture();
                return;
            case kWeatherStorm:
                loadLightningTextures();
                [[fallthrough]];
            case kWeatherLightRain:
            case kWeatherRain:
                loadRainTexture();
                return;
            case kWeatherDusk:
                startFlare(*g_duskFlareColour, 2765);
                break;
            case kWeatherSun:
                startFlare(*g_sunFlareColour, 2772);
                break;
            case kWeatherNone:
                break;
            default:
                *g_weatherActive = 0;
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(startLevelWeather, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int releaseFxMaterial(ee::Ptr<uint32_t> material) { return game::tail(aggressive::fx::releaseFxMaterial, material); }
        }
    }

    // stopLevelWeather: the level's weather over: the wind gone, and the
    // textures it loaded released.
    void stopLevelWeather()
    {
        *g_smokeWind = zge::kZero;
        const uint32_t weather = *g_levelWeather;
        if (*g_weatherActive == 0)
            return;
        *g_weatherActive = 0;
        *g_weatherHeld = 0;
        *g_clearWeather = 0;
        const auto release = [](ee::Ptr<uint32_t> material) { releaseFxMaterial(material); };
        const auto releaseLast = [](ee::Ptr<uint32_t> material) { tail::releaseFxMaterial(material); };
        const auto flare = [] { return *g_levelLensFlare; };
        switch (weather)
        {
        case kWeatherSnow:
        case kWeatherSnowLeft:
            releaseLast(g_snowTexture.get());
            return;
        case kWeatherStorm:
            release(g_boltTexture.get());
            release(g_forkTexture.get());
            [[fallthrough]];
        case kWeatherLightRain:
        case kWeatherRain:
            releaseLast(g_rainTexture.get());
            return;
        case kWeatherSun:
        case kWeatherDusk:
        {
            release(&flare()->glow);
            release(&flare()->ring1);
            release(&flare()->ring2);
            release(&flare()->ray);
            releaseLast(&flare()->star);
            return;
        }
        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(stopLevelWeather, Verified);

    // updateLevelWeather: the weather's frame: its emission on the effects' object;
    // the storm's bolts besides (a burst of two or three, a flash with each,
    // then 15 to 25 seconds' pause).
    void updateLevelWeather(ee::Float seconds)
    {
        if (*g_weatherActive == 0)
            return;
        if (*g_weatherHeld != 0 || *g_levelWeather >= kWeatherKinds)
            return;
        const auto emit = [&](uint32_t hash, uint32_t parameters) {
            const uint32_t object = *g_effectsObject;
            if (object == 0)
                return;
            zge::spawnNamedEffect(hash, object, parameters);
        };
        switch (*g_levelWeather)
        {
        case kWeatherSnow:
            emit(*g_snowHash, kSnowEmission);
            return;
        case kWeatherLightRain:
        {
            const uint32_t object = *g_effectsObject;
            if (object != 0)
                zge::spawnNamedEffect(*g_rainHash, object, kLightRainEmission);
            return;
        }
        case kWeatherStorm:
            if (*g_stormPause <= seconds)
            {
                if (*g_nextBolt <= seconds)
                {
                    const int32_t gap = static_cast<int32_t>(zge::randomNumber());
                    const uint32_t object = *g_effectsObject;
                    *g_nextBolt = ee::toFloat(gap & 3) * ee::Float(0.15f) + ee::Float(0.3f);
                    if (object != 0)
                        zge::spawnNamedEffect(*g_lightningHash, object, 0);
                    startScreenFlash(1, *g_boltFlashColour, *g_boltFlashSeconds);
                    *g_stormBolts = static_cast<uint8_t>(*g_stormBolts + 1);
                    if (*g_stormBolts == *g_stormBurst)
                    {
                        *g_stormBolts = 0;
                        const int32_t pause = static_cast<int32_t>(zge::randomNumber());
                        const ee::Float wait = ee::toFloat(pause & 7) * ee::Float(1.5f) + ee::Float(15.0f);
                        *g_stormPause = wait;
                        *g_stormBurst = static_cast<uint8_t>((ee::toInt(wait) & 1) + 2);
                    }
                }
                else
                {
                    const ee::Float left = *g_nextBolt - seconds;
                    *g_nextBolt = left;
                }
            }
            else
            {
                const ee::Float left = *g_stormPause - seconds;
                *g_stormPause = left;
            }
            [[fallthrough]];
        case kWeatherRain:
            emit(*g_rainHash, kRainEmission);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(updateLevelWeather, Verified);

    // drawLevelLensFlare: the lens flare task's frame. Returns 1 (a task goes on).
    uint32_t drawLevelLensFlare()
    {
        drawLensFlare(*g_levelLensFlare);
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawLevelLensFlare, Verified);

    // suspendWeatherEffects: the script holds the weather: nothing spawns, and what
    // is out is cleared.
    void suspendWeatherEffects()
    {
        *g_clearWeather = 1;
        *g_weatherHeld = 1;
    }
    READABLE_FUNCTION(suspendWeatherEffects, Verified);

    // resumeWeatherEffects: the weather goes on.
    void resumeWeatherEffects()
    {
        *g_weatherHeld = 0;
    }
    READABLE_FUNCTION(resumeWeatherEffects, Verified);

    // setSmokeWind: the wind the smoke drifts with. Nothing calls it.
    void setSmokeWind(ee::Ptr<const zge::Vec4> wind)
    {
        *g_smokeWind = *wind;
    }
    READABLE_FUNCTION(setSmokeWind, Verified);

    // clearSmokeWind: no wind. Nothing calls it.
    void clearSmokeWind()
    {
        *g_smokeWind = zge::kZero;
    }
    READABLE_FUNCTION(clearSmokeWind, Verified);
}
