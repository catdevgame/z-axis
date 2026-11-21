// Light objects.



#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/light_objects.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_lightObjectsReady> g_lightObjectsReady;
    }

    // G_lightObjectsReady = 1 (between the camera and light object functions); initSceneObjects.
    uint32_t initLightObjects()
    {
        *g_lightObjectsReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initLightObjects, Verified);

    // G_lightObjectsReady cleared; shutDownSceneObjects.
    uint32_t shutdownLightObjects()
    {
        if (*g_lightObjectsReady == 0)
            return 1;
        *g_lightObjectsReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownLightObjects, Verified);

    uint32_t lightObjectStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(lightObjectStubZero, Draft);

    // Empty; a light object method.
    void lightObjectNoopA()
    {
    }
    READABLE_FUNCTION(lightObjectNoopA, Draft);

    void lightObjectNoopB()
    {
    }
    READABLE_FUNCTION(lightObjectNoopB, Draft);

    void lightObjectNoopC()
    {
    }
    READABLE_FUNCTION(lightObjectNoopC, Draft);

    void lightObjectNoopD()
    {
    }
    READABLE_FUNCTION(lightObjectNoopD, Draft);

    void lightObjectNoopE()
    {
    }
    READABLE_FUNCTION(lightObjectNoopE, Draft);
}
