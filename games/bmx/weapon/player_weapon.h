#pragma once

// BMX XXX's weapons (./src/com/action/weapon.c): a player's weapon: the
// player's rider standing with a rifle, turning and aiming by the stick,
// with a shooter (weapon.h) of its own and the player's camera looking down
// the aim.

#include "weapon.h"

#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // PlayerWeapon::flags
    constexpr uint8_t kPlayerWeaponInUse = 0x01;
    constexpr uint8_t kPlayerWeaponArmed = 0x02; // resetWeapon's second argument

    // The weapon's states are three levels deep: a level's state has its own
    // states on the level below. Setting one starts the levels below it at 0.
    constexpr uint32_t kPlayerWeaponStateLevels = 3;
    constexpr uint32_t kPlayerWeaponStanding = 0;    // level 0: the only state
    constexpr uint32_t kStandingBegins = 0;          // level 1, under standing: the stance's motion starts,
    constexpr uint32_t kStandingHolds = 1;           //          then is started again each time it ends

    // A player's weapon. Two at g_playerWeaponRecords; the game code keeps
    // each as a handle (playerWeaponHandle, playerWeaponOf).
    struct PlayerWeapon
    {
        uint8_t flags;                               // +0x00
        uint8_t player;                              // +0x01 whose it is (playerCamera's index)
        uint8_t rider;                               // +0x02 riderOf's index
        uint8_t states[kPlayerWeaponStateLevels];    // +0x03
        // +0x06 frame counts: the first counts the weapon's frames; the
        // second and third are counted by the level 0 and level 1 states'
        // frames (one level off the states they stand beside), and all of a
        // level and below are cleared with its state.
        uint16_t stateFrames[kPlayerWeaponStateLevels];
        zge::Angle pitch;                            // +0x0C the aim's angle up or down, kept within about 85 degrees
        uint8_t _0e[2];
        rider::ButtonRecord buttons;                 // +0x10 the buttons its shooter reads
        uint8_t _38[0x20];
        ee::Ptr<Shooter> shooter;                    // +0x58
    };
    static_assert(sizeof(PlayerWeapon) == 0x5C && offsetof(PlayerWeapon, pitch) == 0x0C && offsetof(PlayerWeapon, buttons) == 0x10);
    constexpr uint32_t kPlayerWeapons = 2;

    // What newPlayerWeapon is given.
    struct PlayerWeaponOwner
    {
        uint8_t player;
        uint8_t rider;
    };

    // (player_weapon.cpp)
    uint32_t setUpPlayerWeapons();
    uint32_t newPlayerWeapon(const PlayerWeaponOwner *owner);
    void freePlayerWeapon(uint32_t handle);
    uint32_t updatePlayerWeapon(uint32_t handle);
    uint32_t playerWeaponProjectileEvent(Projectile *projectile, ProjectileMessage *message);
    void resetWeapon(uint32_t handle, uint32_t armed);
    uint32_t setPlayerWeaponState(PlayerWeapon *weapon, uint32_t level, uint32_t state);
    uint32_t playerWeaponState(const PlayerWeapon *weapon, uint32_t level);
    uint32_t playerWeaponStanding(PlayerWeapon *weapon);
    uint32_t playerWeaponStance(PlayerWeapon *weapon);
    void aimPlayerCamera(PlayerWeapon *weapon);
    PlayerWeapon *playerWeaponOf(uint32_t handle);
    uint32_t playerWeaponHandle(PlayerWeapon *weapon);
    void closePlayerWeapons();
    rider::ButtonRecord *weaponInput(uint32_t handle);
}
