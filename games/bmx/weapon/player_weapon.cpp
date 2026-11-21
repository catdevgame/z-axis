// BMX XXX's weapons (player_weapon.h): a player's weapon: the rider
// standing with a rifle, turned and aimed by the stick, and the player's
// camera looking down the aim (./src/com/action/weapon.c).

#include "player_weapon.h"

#include "bmx/camera/action_camera.h"
#include "bmx/rider/physics/physics.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/scene/animator.h"
#include "zge/text/cstring.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/weapon/player_weapon.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::Rider;
    using rider::inputStick;
    using rider::riderMotionByName;
    using rider::riderOf;

    namespace
    {
        inline ee::Global<PlayerWeapon[kPlayerWeapons], sym::g_playerWeaponRecords> g_playerWeaponRecords;
        // The weapons' motions ("assets\motweap.zal").
        inline ee::Global<int32_t, sym::g_weaponMotions> g_weaponMotions;

        constexpr uint32_t kStanceMotionName = sym::bmx::g_weaponStanceMotionName; // "weapon.stand.rifle.med"

        // The weapons a player's shooter is reset with (every type but the
        // empty hand, which is always there), and its flags: bit 2, which no
        // written function reads.
        constexpr uint32_t kPlayerWeaponTypes = 0x3E;
        constexpr uint8_t kPlayerShooterFlags = 0x04;

        // The rider's flag (the third byte of its flags) set when its motion
        // has ended.
        constexpr uint8_t kRiderMotionEnded = 0x01;
        // The rider's flags (the first byte) cleared as the camera is aimed.
        constexpr uint8_t kRiderViewFlags = 0x0C;

        // How high over the rider's position the shots come from, along the
        // rider's up axis.
        constexpr ee::Float kMuzzleHeight = ee::Float(1.5f);
        // The aim's limit up and down.
        constexpr int32_t kPitchLimit = 0x3C71;
        // The view's angle below which the stick turns the aim slower.
        constexpr ee::Float kFullSpeedFov = ee::Float(30.0f);
        // The stick's turn and tilt rates at full push, degrees a frame, and
        // the part of the way to the wanted rate each goes a frame (with the
        // stick, and let go).
        constexpr ee::Float kTurnRate = ee::Float(-4.5f);
        constexpr ee::Float kTiltRate = ee::Float(2.5f);
        constexpr ee::Float kTurnEasing = std::bit_cast<float>(0x3DAE147Bu);     // 0.085
        constexpr ee::Float kTurnStopEasing = std::bit_cast<float>(0x3F266666u); // 0.65
        constexpr ee::Float kTiltEasing = std::bit_cast<float>(0x3DF5C28Fu);     // 0.12
        constexpr ee::Float kTiltStopEasing = ee::Float(0.75f);
        // A rate under this many angle units is none.
        constexpr ee::Float kLeastRate = ee::Float(4.0f);
        constexpr ee::Float kTurnsAnAngle = ee::Float(1.0f / 65536.0f);
        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);

        // Each level 0 state's frame: it answers 0 to be run again at once.
        using PlayerWeaponStateFrame = ee::FunctionPtr<uint32_t(ee::Ptr<PlayerWeapon>)>;
        inline ee::Global<PlayerWeaponStateFrame[1], sym::g_playerWeaponStates> g_playerWeaponStates;

        constexpr ee::Float kFrameSeconds = std::bit_cast<float>(0x3C888889u); // 1 / 60

        // The rider's axes (right, up, forward and a fourth row) as a matrix.
        ee::Ptr<Matrix> riderAxes(Rider *rider) { return rider::matrix(rider->right); }
        Vec4 &vec4(ee::Float *v) { return *rider::vec4(v); }

        // A rate eased towards the stick's: the push squared, slower when
        // the view is zoomed in. Under kLeastRate it stops.
        void easeRate(int16_t *rate, ee::Float push, ee::Float zoomScale, ee::Float fullRate, ee::Float easing, ee::Float stopEasing)
        {
            int32_t wanted = 0;
            ee::Float ease = stopEasing;
            if (!(push == ee::Float(0.0f)))
            {
                ee::Float squared = push * push;
                if (!(ee::Float(0.0f) < push))
                    squared = -squared;
                wanted = static_cast<int16_t>(ee::toInt(squared * zoomScale * fullRate * kDegreesToAngle));
                ease = easing;
            }
            const ee::Float change = (ee::toFloat(wanted) * kTurnsAnAngle - ee::toFloat(*rate) * kTurnsAnAngle) * ease * kAnglesATurn;
            *rate = static_cast<int16_t>(*rate + ee::toInt(change));
            if (ee::abs(ee::toFloat(*rate)) < kLeastRate)
                *rate = 0;
        }

        // The shooter put at the rider: over its position, along its up axis.
        void placeShooter(Shooter *shooter, Rider *rider, uint64_t carried)
        {
            shooter->position = scaledCarrying(vec4(rider->up), kMuzzleHeight, carried);
            shooter->position = shooter->position + vec4(rider->position);
        }
    }

    // setUpPlayerWeapons: the weapons' motions loaded. Only the first eight
    // bytes of the records are cleared (the first record's flags, owner,
    // states and the first frame count), not both records.
    uint32_t setUpPlayerWeapons()
    {
        *g_weaponMotions = loadAnimationArchive(sym::g_weaponMotionsArchiveName); // "assets\motweap.zal"
        memoryFill(&*g_playerWeaponRecords, 0, 8);
        return 1;
    }
    READABLE_FUNCTION(setUpPlayerWeapons, Verified);

    // closePlayerWeapons: every player weapon in use freed, and the weapons'
    // motions with them.
    void closePlayerWeapons()
    {
        for (uint32_t slot = 0; slot < kPlayerWeapons; ++slot)
        {
            PlayerWeapon *weapon = &(*g_playerWeaponRecords)[slot];
            if (weapon->flags & kPlayerWeaponInUse)
                freePlayerWeapon(playerWeaponHandle(weapon));
        }
        // (The original's tail jump.)
        game::tail(zge::freeAnimationLibrary, static_cast<uint32_t>(*g_weaponMotions));
    }
    READABLE_FUNCTION(closePlayerWeapons, Verified);

    // newPlayerWeapon: a free record for a player and its rider, with a
    // shooter that reads the record's own buttons and hands its projectiles'
    // events to the rider code. Returns its handle, 0 when both records or
    // all the shooters are in use. The shooter's flags are never set here:
    // they are what the stack held.
    uint32_t newPlayerWeapon(const PlayerWeaponOwner *owner)
    {
        ee::Local<ShooterOwner> shooterOwner;
        for (uint32_t slot = 0; slot < kPlayerWeapons; ++slot)
        {
            PlayerWeapon *weapon = &(*g_playerWeaponRecords)[slot];
            if (weapon->flags & kPlayerWeaponInUse)
                continue;
            memoryFill(weapon, 0, sizeof(PlayerWeapon));
            shooterOwner->buttons = &weapon->buttons;
            weapon->flags |= kPlayerWeaponInUse;
            shooterOwner->handler = ProjectileHandler(sym::playerWeaponProjectileEvent);
            weapon->player = owner->player;
            weapon->rider = owner->rider;
            shooterOwner->data = playerWeaponHandle(weapon);
            Shooter *shooter = newShooter(shooterOwner);
            weapon->shooter = shooter;
            if (shooter != nullptr)
                return playerWeaponHandle(weapon);
            weapon->flags &= static_cast<uint8_t>(~kPlayerWeaponInUse);
            return 0;
        }
        return 0;
    }
    READABLE_FUNCTION(newPlayerWeapon, Verified);

    // freePlayerWeapon: its shooter freed, and the record free.
    void freePlayerWeapon(uint32_t handle)
    {
        PlayerWeapon *weapon = playerWeaponOf(handle);
        freeShooter(weapon->shooter);
        weapon->flags &= static_cast<uint8_t>(~kPlayerWeaponInUse);
    }
    READABLE_FUNCTION(freePlayerWeapon, Verified);

    // updatePlayerWeapon: a player's weapon's frame: its state's frame (again
    // while it answers 0), then the rider's motion stepped and the rider's
    // model put where the rider stands: turned the rider's way (the model's
    // up and forward axes are the rider's turned round) and moved by the
    // motion's root. Then the camera is aimed.
    uint32_t updatePlayerWeapon(uint32_t handle)
    {
        ee::Local<Matrix> axes;
        PlayerWeapon *weapon = playerWeaponOf(handle);
        Rider *rider = riderOf(weapon->rider);
        while ((*g_playerWeaponStates)[weapon->states[0]](weapon) == 0)
        {
        }
        ++weapon->stateFrames[0];
        rider::stepRiderMotion(rider, kFrameSeconds);
        *axes = *riderAxes(rider);
        vec3Negate(&axes->row(1).xyz(), &axes->row(1).xyz());
        vec3Negate(&axes->row(2).xyz(), &axes->row(2).xyz());
        setObjectRotationMatrix(rider->messenger, axes);
        vec4(rider->rootOffset) = vec4(rider->motionRoot);
        rider->rootOffset[2] = -rider->rootOffset[2];
        rider->rootOffset[1] = -rider->rootOffset[1];
        vec4(rider->rootOffset) = transformed(*riderAxes(rider), vec4(rider->rootOffset));
        setObjectPosition(rider->messenger, rider::vec4(rider->position));
        aimPlayerCamera(weapon);
        return 1;
    }
    READABLE_FUNCTION(updatePlayerWeapon, Verified);

    // playerWeaponProjectileEvent: the handler of a player weapon's
    // shooter: the event goes to the rider code with the weapon's player.
    uint32_t playerWeaponProjectileEvent(Projectile *projectile, ProjectileMessage *message)
    {
        const PlayerWeapon *weapon = ee::Ptr<PlayerWeapon>(projectile->shooter->ownerData);
        return rider::riderHitByProjectile(weapon->player, projectile, message);
    }
    READABLE_FUNCTION(playerWeaponProjectileEvent, Verified);

    // resetWeapon: a player's weapon as a turn starts: armed or not, no
    // buttons, standing, aiming level, its shooter with every weapon full,
    // at the rider and facing the rider's way, and the camera aimed.
    void resetWeapon(uint32_t handle, uint32_t armed)
    {
        ee::Local<ShooterWeapons> weapons;
        PlayerWeapon *weapon = playerWeaponOf(handle);
        Rider *rider = riderOf(weapon->rider);
        if (armed)
            weapon->flags |= kPlayerWeaponArmed;
        else
            weapon->flags &= static_cast<uint8_t>(~kPlayerWeaponArmed);
        weapon->buttons.held = 0;
        weapon->buttons.changed = 0;
        for (uint32_t stick = 0; stick < 2; ++stick)
        {
            weapon->buttons.previous[stick].x = ee::Float(0.0f);
            weapon->buttons.previous[stick].y = ee::Float(0.0f);
            weapon->buttons.sticks[stick].x = ee::Float(0.0f);
            weapon->buttons.sticks[stick].y = ee::Float(0.0f);
        }
        setPlayerWeaponState(weapon, 0, kPlayerWeaponStanding);
        weapon->pitch = 0;
        weapons->owned = kPlayerWeaponTypes;
        weapons->flags = kPlayerShooterFlags;
        resetShooter(weapon->shooter, weapons);
        weapon->shooter->aim = *riderAxes(rider);
        placeShooter(weapon->shooter, rider, rider::physics::upperHalf(vec4(rider->right)));
        aimPlayerCamera(weapon);
    }
    READABLE_FUNCTION(resetWeapon, Verified);

    // setPlayerWeaponState: a level's state set, its frame count and the
    // levels below it started again. Returns the state.
    uint32_t setPlayerWeaponState(PlayerWeapon *weapon, uint32_t level, uint32_t state)
    {
        weapon->states[level] = static_cast<uint8_t>(state);
        weapon->stateFrames[level] = 0;
        for (uint32_t below = level + 1; below < kPlayerWeaponStateLevels; ++below)
        {
            weapon->states[below] = 0;
            weapon->stateFrames[below] = 0;
        }
        return weapon->states[level];
    }
    READABLE_FUNCTION(setPlayerWeaponState, Verified);

    uint32_t playerWeaponState(const PlayerWeapon *weapon, uint32_t level)
    {
        return weapon->states[level];
    }
    READABLE_FUNCTION(playerWeaponState, Verified);

    // playerWeaponStanding: the standing state's frame. The stick turns the
    // rider and tilts the aim (slower when zoomed in under 30 degrees); the
    // shooter is put at the rider with the rider's axes tilted by the aim's
    // angle and has its frame; then the stance's.
    uint32_t playerWeaponStanding(PlayerWeapon *weapon)
    {
        ee::Local<Matrix> turn;
        Rider *rider = riderOf(weapon->rider);
        const ee::Float part = weapon->shooter->fov / kFullSpeedFov;
        const ee::Float zoomScale = part <= ee::Float(1.0f) ? part : ee::Float(1.0f);

        easeRate(&rider->yawRate, inputStick(&weapon->buttons, 0)->x, zoomScale, kTurnRate, kTurnEasing, kTurnStopEasing);
        matrixIdentity(turn);
        matrixRotateY(turn, turn, rider->yawRate);
        matrixMultiply(riderAxes(rider), riderAxes(rider), turn);

        easeRate(&rider->pitch, inputStick(&weapon->buttons, 0)->y, zoomScale, kTiltRate, kTiltEasing, kTiltStopEasing);
        int32_t pitch = weapon->pitch + rider->pitch;
        if (pitch > kPitchLimit)
            pitch = kPitchLimit;
        if (pitch < -kPitchLimit)
            pitch = -kPitchLimit;
        weapon->pitch = static_cast<Angle>(pitch);

        weapon->shooter->aim = *riderAxes(rider);
        matrixIdentity(turn);
        matrixRotateX(turn, turn, weapon->pitch);
        matrixMultiply(&weapon->shooter->aim, &weapon->shooter->aim, turn);
        placeShooter(weapon->shooter, rider, zge::scaleLeftover(zge::LeftBy::lastCall));
        updateShooter(weapon->shooter);

        uint32_t done;
        do
        {
            done = playerWeaponStance(weapon);
        } while (done == 0 && playerWeaponState(weapon, 0) == kPlayerWeaponStanding);
        ++weapon->stateFrames[1];
        return done;
    }
    READABLE_FUNCTION(playerWeaponStanding, Verified);

    // playerWeaponStance: the standing state's own states: the rider's
    // stance motion started, then started again each time it has ended.
    uint32_t playerWeaponStance(PlayerWeapon *weapon)
    {
        ee::Local<char[0x40]> name;
        Rider *rider = riderOf(weapon->rider);
        const uint32_t state = playerWeaponState(weapon, 1);
        if (state == kStandingBegins)
        {
            formatString(name, kStanceMotionName);
            riderMotionByName(name.address(), &rider->motionName, &rider->motion);
            rider->motionFlags |= rider::kMotionRestart;
            setPlayerWeaponState(weapon, 1, kStandingHolds);
        }
        else if (state == kStandingHolds)
        {
            if (rider->flags[2] & kRiderMotionEnded)
            {
                riderMotionByName(sym::g_weaponStanceMotionName, &rider->motionName, &rider->motion);
                rider->motionFlags |= rider::kMotionRestart;
            }
        }
        ++weapon->stateFrames[2];
        return 1;
    }
    READABLE_FUNCTION(playerWeaponStance, Verified);

    // aimPlayerCamera: the player's camera put at the shooter, looking the
    // aim's way with the shooter's view angle.
    void aimPlayerCamera(PlayerWeapon *weapon)
    {
        ee::Local<CameraView> view;
        Rider *rider = riderOf(weapon->rider);
        const Shooter *shooter = weapon->shooter;
        view->fov = static_cast<Angle>(ee::toInt(shooter->fov * kDegreesToAngle));
        view->eye = shooter->position;
        view->lookAt = weapon->shooter->position + weapon->shooter->aim.row(2);
        view->axes = weapon->shooter->aim;
        setCameraView(playerActionCamera(weapon->player), view);
        rider->flags[0] &= static_cast<uint8_t>(~kRiderViewFlags);
    }
    READABLE_FUNCTION(aimPlayerCamera, Verified);

    // playerWeaponOf, playerWeaponHandle: a weapon's handle is its record.
    PlayerWeapon *playerWeaponOf(uint32_t handle)
    {
        return ee::Ptr<PlayerWeapon>(handle);
    }
    READABLE_FUNCTION(playerWeaponOf, Verified);

    uint32_t playerWeaponHandle(PlayerWeapon *weapon)
    {
        return ee::Address(weapon);
    }
    READABLE_FUNCTION(playerWeaponHandle, Verified);

    // weaponInput: the buttons a player weapon's shooter reads (an input
    // replay of a player who watches plays into them).
    rider::ButtonRecord *weaponInput(uint32_t handle)
    {
        return &playerWeaponOf(handle)->buttons;
    }
    READABLE_FUNCTION(weaponInput, Verified);
}
