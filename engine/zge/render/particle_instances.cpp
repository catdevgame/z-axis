// Spark, billboard and poly particle types.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/render/billboards.h"
#include "zge/scene/particle_types.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/particle_instances.cpp.h"

namespace zge
{

    namespace
    {
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<int32_t, sym::g_sparkPool> g_sparkPool;
        constexpr uint32_t kReflectVector = sym::reflectVector; // reflectVector
        constexpr uint32_t kPlaneDistance = sym::planeDistance; // planeDistance ((plane*, point*) -> signed distance (f0); readable: planeDistance_, Verified)
        inline ee::Global<int32_t, sym::g_billboardPool> g_billboardPool;
        constexpr uint32_t kRandom = sym::randomNumber; // randomNumber (() -> a random number)
        inline ee::Global<int32_t, sym::g_polyParticlePool> g_polyParticlePool;
        inline ee::Global<uint64_t, sym::g_splashSparks> g_spawnSplashTemplateInitializer0; // g_spawnSplashTemplateInitializer0; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_effectsObject> g_effectsObject; // g_effectsObject (aggressive::fx::(anonymous)::g_effectsObject, aggressive::fx::(anonymous)::g_effectsObject)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(randomint32_t, ee::game<int32_t>, kRandom);
        }
    }

    // The spark type's empty method.
    void sparkTypeNoop()
    {
    }
    READABLE_FUNCTION(sparkTypeNoop, Draft);

    // (.., emitter): heapFree.
    int32_t freeSparkEmitter(uint32_t /*param_1*/, uint32_t block)
    {
        return game::tail(zge::heapFree, block);
    }
    READABLE_FUNCTION_WAITS(freeSparkEmitter, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t planeDistance(uint32_t plane, uint32_t point) { return ee::game<int32_t>(kPlaneDistance, plane, point); }
        int32_t reflectVector(uint32_t out, uint32_t vector, uint32_t normal) { return ee::game<int32_t>(kReflectVector, out, vector, normal); }
    }

    // The billboard type's empty method.
    void billboardTypeNoop()
    {
    }
    READABLE_FUNCTION(billboardTypeNoop, Draft);

    // (.., effect): heapFree (ZGE4 first hands the effect's billboard queue
    // record to deferBillboardEffectRelease).
    int32_t freeBillboardEmitter(uint32_t /*param_1*/, uint32_t block)
    {
        if constexpr (game::readable::kIsBmx)
            deferBillboardEffectRelease(ee::Ptr<ParticleEffect>(block)->billboardEffect);
        return game::tail(zge::heapFree, block);
    }
    READABLE_FUNCTION_WAITS(freeBillboardEmitter, Draft);

    // The poly type's empty method.
    void polyTypeNoop()
    {
    }
    READABLE_FUNCTION(polyTypeNoop, Draft);

    // (.., emitter): heapFree.
    int32_t freePolyEmitter(uint32_t /*param_1*/, uint32_t block)
    {
        return game::tail(zge::heapFree, block);
    }
    READABLE_FUNCTION_WAITS(freePolyEmitter, Draft);

}
