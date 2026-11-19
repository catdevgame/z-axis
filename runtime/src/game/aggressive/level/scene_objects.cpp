// The level's scene objects as the game side asks after them (readable
// round gamehand): an object's node, whether it was drawn, the level's box
// sweep, a car's wheels turned by how far it went. (The level objects of
// the scripts, not objects.cpp's game objects.)

#include "aggressive/level/scene_objects.h"
#include "zge/math/vector.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/text/strings.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "sym/aggressive/level/scene_objects.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kMatrixRotateX = sym::matrixRotateX;  // (out, in, angle) the matrix turned about x (engine)

        // A car's two axles, by name, hashed once.
        inline ee::Global<ee::Ptr<const char>[2], sym::g_axleNames> g_axleNames; // "FrontAxle", "RearAxle"
        inline ee::Global<uint32_t[2], sym::g_axleHashes> g_axleHashes;
        inline ee::Global<uint32_t, sym::g_axlesHashed> g_axlesHashed;
        // 65536 / 360: degrees to the engine's 16-bit angle.
        inline ee::Global<ee::Float, sym::g_angleUnitsPerDegree> g_angleUnitsPerDegree;
    }

    // objectNode: the object's scene's root node, 0 without a scene.
    uint32_t objectNode(ee::Address object)
    {
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(ee::Ptr<zge::SceneObject>(object)); // the callers' object word
        if (!scene)
            return 0;
        return ee::Address(zge::sceneRootNode(scene)); // the callers take the node as a word
    }
    READABLE_FUNCTION(objectNode, Verified);

    // objectDrawn: 1 if the object's geometry was drawn in the last frame
    // (on screen): the NPCs animate only then.
    int32_t objectDrawn(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<zge::Node> node = zge::objectLevelNode(object);
        return zge::nodeNotDrawn(node) == 0;
    }
    READABLE_FUNCTION(objectDrawn, Verified);

    // turnCarWheels : a traffic car's axles turned about x by the distance
    // it went (its follower's `speed`, signed, times the frame's
    // `seconds`; updateTrafficCar, traffic_cars.cpp) over the wheel's
    // `circumference` turns.
    void turnCarWheels(ee::Ptr<zge::SceneObject> car, ee::Float circumference, ee::Float speed, ee::Float seconds)
    {
        ee::Local<zge::Matrix> rotation;
        ee::Local<zge::Quaternion> turned;
        if (!*g_axlesHashed)
        {
            for (uint32_t i = 0; i < 2; ++i)
                (*g_axleHashes)[i] = zge::nameHash((*g_axleNames)[i]);
            *g_axlesHashed = 1;
        }
        if (speed == ee::Float(0.0f))
            return;
        const int16_t angle = static_cast<int16_t>(
            ee::toInt(speed / circumference * ee::Float(360.0f) * seconds * *g_angleUnitsPerDegree));
        for (uint32_t i = 0; i < 2; ++i)
        {
            const ee::Ptr<zge::AnimationChannel> bone = zge::findAnimatorBone(car, (*g_axleHashes)[i]);
            if (bone)
            {
                zge::quaternionToMatrix(rotation, zge::boneRotation(bone));
                zge::matrixRotateX(rotation, rotation, static_cast<int32_t>(angle));
                zge::matrixToQuaternion(turned, rotation);
                zge::setBoneRotation(bone, turned);
            }
        }
    }
    READABLE_FUNCTION(turnCarWheels, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_levelFrameCount> g_levelFrameCount; // g_levelFrameCount; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<ee::Float, sym::g_runningScriptTimers> g_runningScriptTimers;
        inline ee::Global<ee::Float, sym::g_inverseFrameTime> g_inverseFrameTime;
    }

    namespace
    {
        ee::Float vec3Length(ee::Ptr<const zge::Vec3> vector) { return zge::vec3Length(vector); }
    }

    // A loop over the 16 timers with no body left.
    int32_t scriptTimerLoopStub()
    {
        int32_t result = 0;
        for (;;)
        {
            result = result + 1 & 0xFFu;
            if (static_cast<uint32_t>(result) < 0x10u)
                continue;
            return result;
        }
        return result;
    }
    READABLE_FUNCTION(scriptTimerLoopStub, Draft);

    // ---- the scripts' timers, ticked once a second, and the area of a
    // triangle (were the sweep's triangleArea .. levelFrame3)

    // triangleArea: half the length of the cross product of a triangle's
    // two edges from its first corner.
    ee::Float triangleArea(ee::Ptr<zge::Vec4> corners)
    {
        ee::Local<zge::Vec4> toSecond, toThird, normal;
        *toSecond = corners[1] - corners[0];
        *toThird = corners[2] - corners[0];
        *normal = zge::cross(*toSecond, *toThird);
        return zge::vec3Length(&normal->xyz()) / ee::Float(2.0f);
    }
    READABLE_FUNCTION(triangleArea, Verified);

    // A script timer: the seconds left, whether it runs, and the object that
    // hears "script_timer_done" when it runs out (0: none).
    struct ScriptTimer
    {
        uint32_t seconds; // +0x00
        uint32_t running; // +0x04
        uint32_t target;  // +0x08
    };
    static_assert(sizeof(ScriptTimer) == 12);
    constexpr uint32_t kScriptTimers = 16;

    namespace
    {
        inline ee::Global<ScriptTimer[kScriptTimers], sym::g_scriptTimers> g_scriptTimers;
        constexpr uint32_t kTimerDone = sym::g_textScriptTimerDone; // "script_timer_done"
    }

    // clearScriptTimers: every timer stopped and cleared.
    void clearScriptTimers()
    {
        for (ScriptTimer &timer : *g_scriptTimers)
        {
            timer.seconds = 0;
            timer.running = 0;
            timer.target = 0;
        }
    }
    READABLE_FUNCTION(clearScriptTimers, Draft);

    // startScriptTimer / stopScriptTimer: a timer (by its number, a byte)
    // set going for `seconds`, telling `target`; or stopped. The count of
    // running timers (a float) goes with it.
    void startScriptTimer(uint32_t number, uint32_t seconds, uint32_t target)
    {
        ScriptTimer &timer = (*g_scriptTimers)[number & 0xFF];
        const ee::Float running = *g_runningScriptTimers + ee::Float(1.0f);
        timer.seconds = seconds;
        timer.running = 1;
        timer.target = target;
        *g_runningScriptTimers = running;
    }
    READABLE_FUNCTION(startScriptTimer, Draft);

    void stopScriptTimer(uint32_t number)
    {
        ScriptTimer &timer = (*g_scriptTimers)[number & 0xFF];
        const ee::Float running = *g_runningScriptTimers - ee::Float(1.0f);
        timer.seconds = 0;
        timer.running = 0;
        timer.target = 0;
        *g_runningScriptTimers = running;
    }
    READABLE_FUNCTION(stopScriptTimer, Draft);

    // scriptTimerTicks: `seconds` more for a running timer.
    void scriptTimerTicks(uint32_t number, uint32_t seconds)
    {
        ScriptTimer &timer = (*g_scriptTimers)[number & 0xFF];
        if (timer.running)
            timer.seconds += seconds;
    }
    READABLE_FUNCTION(scriptTimerTicks, Draft);

    // updateScriptTimers: (once a second) each running timer one second
    // down; one at 0 stops, and its object hears "script_timer_done".
    void updateScriptTimers()
    {
        ee::Local<zge::EngineMessage> done;
        for (ScriptTimer &timer : *g_scriptTimers)
        {
            if (timer.running == 1 && timer.seconds != 0)
                timer.seconds -= 1;
            if (timer.running != 1 || timer.seconds != 0)
                continue;
            const uint32_t target = timer.target;
            const ee::Float running = *g_runningScriptTimers - ee::Float(1.0f);
            timer.running = 0;
            timer.seconds = 0;
            timer.target = 0;
            *g_runningScriptTimers = running;
            if (target)
            {
                // The message's list links are left as the stack had them.
                done->target = target;
                done->sender = 0;
                done->type = zge::nameHash(ee::Ptr<const char>(kTimerDone));
                done->flags = zge::kMessageNow;
                done->data = 0;
                zge::sendEngineMessage(done);
            }
        }
    }
    READABLE_FUNCTION_WAITS(updateScriptTimers, Draft);

    // levelFrame3: (each frame, seconds the frame's time) the timers ticked
    // once every so many frames: as many as make a second at the first
    // frame's time.
    void levelFrame3(ee::Float seconds)
    {
        ee::Float framesASecond = *g_inverseFrameTime;
        if (framesASecond == ee::Float(0.0f))
        {
            framesASecond = ee::Float(1.0f) / seconds;
            *g_inverseFrameTime = framesASecond;
        }
        if (*g_runningScriptTimers == ee::Float(0.0f))
            return;
        ee::Float frames = *g_levelFrameCount;
        if (framesASecond < frames)
        {
            updateScriptTimers();
            frames = 0.0f;
            *g_levelFrameCount = frames;
        }
        *g_levelFrameCount = frames + ee::Float(1.0f);
    }
    READABLE_FUNCTION_WAITS(levelFrame3, Verified);
}
