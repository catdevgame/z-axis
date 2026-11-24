// Aggressive Inline (SLUS-20327): Practice's restart point
// (engine/overrides/zge_practice.h has the keys, the clock and the lines).
//
// What a point holds. The game keeps no save state, so a point is what the
// game's own respawn can put back: player one's place and heading, its
// velocity and the speed it pushes to, and the juice in its carton. The
// level's own state (score, goals done, pickups, a Timed Run's clock,
// challenge scripts) goes on as played: a return is a retry from the point,
// as the game's own challenge retry is a retry from where it was given.
//
// Where. Only rolling or standing on the ground (state Ground or Stand, not
// down) and with no scripted camera running: a skater in the air, on a
// rail, a pole or a wall holds the object and the move it is in, which a
// respawn does not put back. Not in split screen (two players).
//
// The return. The game's respawn (placeSkaterAtLevelStart, through
// aggressive_inline_restart.cpp respawnAt: the locator lookup answers with
// the point's place) ends the trick and its line, resets the physics, puts
// the skater standing at the point turned as it was and cuts the camera
// there. Then the point's velocity and push speed are written: the Stand
// state's update (standUpdate) takes the skater rolling again (Ground) at
// that speed as it does after any stop, and the juice is put back through
// the carton's own setter (setJuiceMeter). In place of one level update.

#include "aggressive_inline_practice.h"
#include "sym/game_overrides/aggressive_inline_practice.cpp.h"

#include "aggressive_inline_restart.h"
#include "overrides/zge_practice.h"
#include "machine.h"
#include "runtime/session.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace aggressive_inline::practice
{
    namespace
    {
        namespace zp = zge_host::practice;

        constexpr uint32_t kGameType = sym::g_gameTypeByte; // byte: 1 Career, 2 Freeskate, 3 Timed Run, 4 split screen
        constexpr uint8_t kTypeSplitScreen = 4u;
        constexpr uint32_t kLevelIndex = sym::g_levelNumber; // byte
        constexpr uint32_t kLevelFrame = sym::g_levelFrame;  // main-loop frames since the level started
        constexpr uint32_t kTopState = sym::g_topState;     // the current top-level state record
        constexpr uint32_t kTopStateLevel = sym::topStates; // state 0 (the level)
        constexpr uint32_t kStateLevelObject = 0x10u;       // a state record's level object (TopState::level)
        constexpr uint32_t kScriptedCamera = sym::g_scriptedCamera; // 1 while a scripted camera runs
        constexpr uint32_t kJuiceAmount = sym::juiceAmount;     // (carton, float *out)
        constexpr uint32_t kSetJuiceMeter = sym::setJuiceMeter; // (carton, juice in f12)

        // The level object's player one (skater/skater.h Player: the level
        // +0x10), and the Player fields a point keeps.
        constexpr uint32_t kLevelPlayerOne = 0x10u;
        constexpr uint32_t kPlayerState = 0x074u;       // Player::state (byte)
        constexpr uint32_t kPlayerFlags = 0x080u;       // Player::flags
        constexpr uint32_t kPlayerPosition = 0x090u;    // Player::position
        constexpr uint32_t kPlayerVelocity = 0x0A0u;    // Player::velocity
        constexpr uint32_t kPlayerOrientation = 0x0B0u; // Player::orientation (rows right, up, forward)
        constexpr uint32_t kPlayerPushSpeed = 0x1E4u;   // Player::pushSpeed
        constexpr uint32_t kPlayerSpeed = 0x1E8u;       // Player::speed
        constexpr uint32_t kPlayerJuiceMeter = 0x8BCu;  // Player::juiceMeter (the HUD's carton)
        constexpr uint8_t kStateGround = 0u;            // skater::State::Ground
        constexpr uint8_t kStateStand = 1u;             // skater::State::Stand
        constexpr uint32_t kDown = 0x2u;                // skater.h kDown: bailed

        struct Point
        {
            uint8_t level = 0;
            float orientation[3][4] = {};
            float position[4] = {};
            float velocity[4] = {};
            float pushSpeed = 0.0f;
            float juice = -1.0f; // -1: no carton
        };

        struct State
        {
            Point point;
            bool trace = false;        // ZX_TRACE_PRACTICE=1: the skater at the VBlanks after a return
            bool traceRead = false;
            uint32_t traceFrames = 0;
            uint32_t levelObject = 0; // the level the point is in
            uint32_t lastFrame = 0;   // the level's frame at the last VBlank (a level loaded again starts over)
        };
        State &state()
        {
            State &s = zx::session::state<State>();
            if (!s.traceRead)
            {
                const char *trace = std::getenv("ZX_TRACE_PRACTICE");
                s.trace = trace && trace[0] == '1';
                s.traceRead = true;
            }
            return s;
        }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        void readFloats(const uint8_t *rdram, uint32_t address, float *out, size_t count)
        {
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(out, p, count * sizeof(float));
        }

        void writeFloats(uint8_t *rdram, uint32_t address, const float *values, size_t count)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, values, count * sizeof(float));
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            readFloats(rdram, address, &value, 1);
            return value;
        }

        uint32_t levelObject(const uint8_t *rdram)
        {
            const uint32_t top = readWord(rdram, kTopState);
            return top == kTopStateLevel ? readWord(rdram, top + kStateLevelObject) : 0u;
        }

        float length3(const float v[4]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // Memory for a guest call's out value, below the caller's stack.
        uint32_t scratchBelow(const R5900Context *ctx) { return (getRegU32(ctx, 29) - 0x200u) & ~0xFu; }

        // The carton's juice, -1 when there is none (juiceAmount).
        float juiceOf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t meter)
        {
            if (!meter)
                return -1.0f;
            const uint32_t out = scratchBelow(ctx);
            uint32_t found = 0;
            if (!runtime->callGuestFunction(rdram, ctx, kJuiceAmount, {meter, out}, out - 0x100u, &found) || !found)
                return -1.0f;
            return readFloat(rdram, out);
        }

        void setJuice(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t meter, float juice)
        {
            if (!meter || juice < 0.0f)
                return;
            const float f12 = ctx->f[12];
            ctx->f[12] = juice;
            runtime->callGuestFunction(rdram, ctx, kSetJuiceMeter, {meter}, scratchBelow(ctx) - 0x100u);
            ctx->f[12] = f12;
        }

        void logPlace(const char *what, const float position[4], float speed, float juice)
        {
            std::cout << "[practice] " << what << ": position (" << position[0] << ", " << position[1] << ", "
                      << position[2] << ") speed " << speed << " juice " << juice << std::endl;
        }

        void setPoint(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            zp::Practice &practice = zp::current();
            const uint32_t level = levelObject(rdram);
            if (!level)
                return;
            if (readByte(rdram, kGameType) == kTypeSplitScreen)
            {
                practice.pointRefused("No restart point in split screen");
                return;
            }
            const uint32_t player = level + kLevelPlayerOne;
            const uint8_t skaterState = readByte(rdram, player + kPlayerState);
            if ((skaterState != kStateGround && skaterState != kStateStand) || (readWord(rdram, player + kPlayerFlags) & kDown) ||
                readWord(rdram, kScriptedCamera))
            {
                practice.pointRefused("A restart point needs the skater on the ground");
                return;
            }
            Point point;
            point.level = readByte(rdram, kLevelIndex);
            readFloats(rdram, player + kPlayerOrientation, &point.orientation[0][0], 12);
            readFloats(rdram, player + kPlayerPosition, point.position, 4);
            readFloats(rdram, player + kPlayerVelocity, point.velocity, 4);
            point.pushSpeed = readFloat(rdram, player + kPlayerPushSpeed);
            point.juice = juiceOf(rdram, ctx, runtime, readWord(rdram, player + kPlayerJuiceMeter));
            state().point = point;
            state().levelObject = level;
            logPlace("point", point.position, length3(point.velocity), point.juice);
            practice.pointSet();
        }

        bool returnToPoint(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            zp::Practice &practice = zp::current();
            const uint32_t level = levelObject(rdram);
            const Point &point = state().point;
            if (!level || level != state().levelObject || readByte(rdram, kLevelIndex) != point.level)
            {
                practice.leave();
                return false;
            }
            if (!restart::respawnAt(rdram, ctx, runtime, point.orientation, point.position))
            {
                practice.returnFailed("The skater could not be put back");
                return false;
            }
            const uint32_t player = level + kLevelPlayerOne;
            writeFloats(rdram, player + kPlayerVelocity, point.velocity, 4);
            writeFloats(rdram, player + kPlayerPushSpeed, &point.pushSpeed, 1);
            setJuice(rdram, ctx, runtime, readWord(rdram, player + kPlayerJuiceMeter), point.juice);

            float position[4] = {};
            float velocity[4] = {};
            readFloats(rdram, player + kPlayerPosition, position, 4);
            readFloats(rdram, player + kPlayerVelocity, velocity, 4);
            logPlace("back", position, length3(velocity),
                     juiceOf(rdram, ctx, runtime, readWord(rdram, player + kPlayerJuiceMeter)));
            practice.returned();
            state().traceFrames = 3;
            return true;
        }
    }

    bool onVsync(uint8_t *rdram, bool pointPressed, bool restartPressed)
    {
        zp::Practice &practice = zp::current();
        const uint32_t level = levelObject(rdram);
        const uint32_t frame = readWord(rdram, kLevelFrame);
        // Out of the level, or the level loaded again (its frames start over).
        if (practice.hasPoint() && (!level || level != state().levelObject || frame < state().lastFrame))
            practice.leave();
        state().lastFrame = frame;
        if (!level)
            return false;
        if (state().traceFrames > 0 && state().trace)
        {
            --state().traceFrames;
            const uint32_t player = level + kLevelPlayerOne;
            float position[4] = {};
            readFloats(rdram, player + kPlayerPosition, position, 4);
            std::cout << "[practice] after the return, frame " << frame << ": state "
                      << static_cast<int>(readByte(rdram, player + kPlayerState)) << " speed "
                      << readFloat(rdram, player + kPlayerSpeed) << " position (" << position[0] << ", " << position[1]
                      << ", " << position[2] << ")" << std::endl;
        }
        return practice.onVsync(pointPressed, restartPressed);
    }

    bool runPending(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, float frameSeconds)
    {
        zp::Practice &practice = zp::current();
        switch (practice.take())
        {
        case zp::Request::SetPoint:
            setPoint(rdram, ctx, runtime);
            break;
        case zp::Request::Return:
            if (returnToPoint(rdram, ctx, runtime))
            {
                // In place of this update.
                ctx->pc = getRegU32(ctx, 31);
                return true;
            }
            break;
        case zp::Request::None:
            break;
        }
        practice.played(frameSeconds);
        return false;
    }
}
