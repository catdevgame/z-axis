#pragma once

// BMX XXX's weapons (./src/com/action/weapon.c): the six weapon types, the
// shooters that carry them (a set of weapons with their ammunition, the one
// in hand, its aim and its part of the screen) and the projectiles they
// fire. BMX XXX's own code: the other two games have none.
//
// The layouts are read from BMX XXX's machine code; a field is named for
// what the weapon code does with it, and bytes no written function touches
// are padding.

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // The weapon types, in the order of their names ("None", "Winchester70",
    // "AK47", "M60", "GrenadeLauncher", "RocketLauncher") and of the table.
    constexpr uint32_t kWeaponTypes = 6;
    constexpr uint32_t kNoWeapon = 0;
    // What a weapon fires ("Bullet", "Grenade", "Rocket"). A bullet has no
    // record of its own: it hits or misses in the frame it is fired.
    enum ProjectileKind : uint8_t
    {
        kProjectileBullet = 0,
        kProjectileGrenade = 1,
        kProjectileRocket = 2,
    };
    constexpr uint32_t kProjectileKinds = 3;

    // What a shooter is doing with the weapon in hand. Each state lasts the
    // weapon type's time for it.
    enum ShooterState : uint8_t
    {
        kShooterReady = 0,
        kShooterFiring = 1,      // the trigger is pulled: the shot leaves when the time is up
        kShooterFired = 2,       // the shot has left: fire again, make ready or reload
        kShooterMakingReady = 3, // a round is ready when the time is up
        kShooterReloading = 4,
        kShooterLoading = 5,     // the rounds move from the reserve when the time is up
        kShooterPuttingAway = 6, // the next weapon comes to hand when the time is up
        kShooterTakingOut = 7,
    };
    constexpr uint32_t kShooterStates = 8;

    // WeaponType::flags
    constexpr uint8_t kWeaponRepeats = 0x02;         // fires again while the fire buttons are held
    constexpr uint8_t kWeaponSights = 0x04;          // may be aimed unzoomed
    constexpr uint8_t kWeaponScope = 0x08;           // three zoom levels, with the scope's picture
    constexpr uint8_t kWeaponZoomsWhileFiring = 0x10; // keeps its zoom through a shot
    constexpr uint8_t kWeaponMuzzleFlash = 0x20;
    constexpr uint8_t kWeaponTracers = 0x40;
    constexpr uint8_t kWeaponNoReloadWhenEmpty = 0x80; // an empty one waits for the reload buttons

    // A row of the weapon type table.
    struct WeaponType
    {
        uint8_t flags;                             // +0x00
        uint8_t projectile;                        // +0x01 ProjectileKind
        uint8_t _02[2];
        ee::Float stateSeconds[kShooterStates];    // +0x04 how long each ShooterState lasts
        uint16_t clip;                             // +0x24 rounds it holds
        uint16_t reserve;                          // +0x26 rounds a shooter starts with beside the loaded ones
        uint8_t _28[4];
        ee::Float recoil;                          // +0x2C how far a shot throws the aim
        ee::Float recoilRecovery;                  // +0x30 the part of the throw taken back each frame
        ee::Float reach;                           // +0x34 a bullet's range; a grenade's or rocket's speed, a frame
        ee::Float force;                           // +0x38 a bullet's velocity as it hits; a grenade's or rocket's life, seconds
        ee::Float blastRadius;                     // +0x3C how far a grenade's or rocket's blast reaches
        uint8_t _40[0x14];
    };
    static_assert(sizeof(WeaponType) == 0x54 && offsetof(WeaponType, clip) == 0x24 && offsetof(WeaponType, recoil) == 0x2C);

    struct Shooter;
    struct Projectile;
    struct ProjectilePath;

    // What a shooter's owner is asked or told about a projectile.
    enum ProjectileEvent : uint8_t
    {
        kProjectileMayFire = 0,  // before the trigger is pulled (the projectile has only its kind and shooter)
        kProjectileFired = 1,
        kProjectileMeetsRider = 2, // the hit test asks whether the rider on its path counts
        kProjectileHitsRider = 3,
        kProjectileHitsObject = 4,
        kProjectileHitsFace = 5,
        kProjectileExpired = 6,
        kProjectileBlastReachesRider = 7, // with the rider and the blast's strength there
        kProjectileMarksFace = 8, // a bullet's mark on a face: answer 1 with the mark's colour
    };
    // The owner's answer: 1 lets the weapon go on.
    struct ProjectileMessage;
    using ProjectileHandler = ee::FunctionPtr<uint32_t(ee::Ptr<Projectile>, ee::Ptr<ProjectileMessage>)>;

    // Shooter::flags
    constexpr uint8_t kShooterIgnoresButtons = 0x01;
    constexpr uint8_t kShooterNoCrosshair = 0x02;
    constexpr uint8_t kShooterInUse = 0x80;

    // A shooter: the weapons someone carries. Four at g_shooters.
    struct Shooter
    {
        uint8_t flags;                             // +0x00
        uint8_t weapon;                            // +0x01 the weapon type in hand
        uint8_t nextWeapon;                        // +0x02 the one coming to hand
        uint8_t state;                             // +0x03 ShooterState
        uint8_t zoom;                              // +0x04 the zoom level asked for
        uint8_t _05;
        uint16_t stateFrames;                      // +0x06 frames left of the state
        struct Ammunition
        {
            uint16_t loaded;
            uint16_t reserve;
        } ammunition[kWeaponTypes];                // +0x08
        uint32_t owned;                            // +0x20 a bit for each weapon type carried
        uint8_t _24[0xC];
        zge::Vec4 position;                        // +0x30 where its sounds and shots come from
        zge::Matrix aim;                           // +0x40 its third row is the way the shots go
        zge::Angle recoilYaw;                      // +0x80 the aim's throw, taken back a part each frame
        zge::Angle recoilPitch;                    // +0x82
        ee::Float fov;                             // +0x84 the view's angle, eased towards the zoom level's
        ee::Ptr<zge::zgui::Node> ui;               // +0x88 its crosshairs' widgets
        ee::Ptr<rider::ButtonRecord> buttons;      // +0x8C
        ProjectileHandler owner;                   // +0x90
        uint32_t ownerData;                        // +0x94
        uint8_t _98[8];
    };
    static_assert(sizeof(Shooter) == 0xA0 && offsetof(Shooter, owned) == 0x20 && offsetof(Shooter, aim) == 0x40);
    static_assert(offsetof(Shooter, recoilYaw) == 0x80 && offsetof(Shooter, ui) == 0x88 && offsetof(Shooter, ownerData) == 0x94);
    constexpr uint32_t kShooters = 4;

    // What newShooter is given.
    struct ShooterOwner
    {
        ee::Ptr<rider::ButtonRecord> buttons;
        ProjectileHandler handler;
        uint32_t data;
        uint8_t flags;                             // kShooterIgnoresButtons, kShooterNoCrosshair
    };
    static_assert(offsetof(ShooterOwner, flags) == 0xC);

    // What resetShooter is given: the weapons carried (the empty hand is
    // always among them) and the shooter's flags.
    struct ShooterWeapons
    {
        uint32_t owned;
        uint8_t flags;
    };
    static_assert(offsetof(ShooterWeapons, flags) == 4);

    // Projectile::flags
    constexpr uint8_t kProjectileInUse = 0x01;
    constexpr uint8_t kProjectileFalls = 0x02;
    constexpr uint8_t kProjectileTrail = 0x04;

    // A grenade or rocket in flight. Sixteen at g_projectiles.
    struct Projectile
    {
        uint8_t flags;                             // +0x00
        uint8_t kind;                              // +0x01 ProjectileKind
        uint16_t life;                             // +0x02 frames left
        uint8_t _04[0xC];
        zge::Matrix matrix;                        // +0x10 where it is (the last row) and how it is turned
        zge::Vec4 velocity;                        // +0x50 a frame
        zge::EulerAngles spin;                     // +0x60 a frame
        uint8_t _66[2];
        ee::Ptr<Shooter> shooter;                  // +0x68
        uint32_t sound;                            // +0x6C what the sound engine answered as it was fired
        ee::Ptr<zge::Node> node;                   // +0x70 its model in the scene (0: none)
        uint8_t _74[0xC];
    };
    static_assert(sizeof(Projectile) == 0x80 && offsetof(Projectile, velocity) == 0x50 && offsetof(Projectile, shooter) == 0x68);
    constexpr uint32_t kProjectiles = 16;

    // ProjectilePath::hits
    constexpr uint32_t kProjectileHitRider = 0x1;
    constexpr uint32_t kProjectileHitObject = 0x2;
    constexpr uint32_t kProjectileHitFace = 0x4;

    // A projectile's way through a frame, and what the hit test found on it.
    struct ProjectilePath
    {
        ee::Ptr<Projectile> projectile;            // +0x00
        uint32_t hits;                             // +0x04
        uint8_t _08[8];
        zge::Vec4 start;                           // +0x10
        zge::Vec4 direction;                       // +0x20 a unit vector
        ee::Float length;                          // +0x30
        ee::Ptr<rider::collision::RayHit> hit;     // +0x34 the level's face hit (the ray cast's answer)
        uint8_t rider;                             // +0x38 the rider hit (0xFF: none)
        uint8_t _39[3];
        uint32_t object;                           // +0x3C the object hit
    };
    static_assert(sizeof(ProjectilePath) == 0x40 && offsetof(ProjectilePath, length) == 0x30 && offsetof(ProjectilePath, object) == 0x3C);

    // What the owner's handler is handed with a projectile: the event, and
    // for a hit what was hit.
    struct ProjectileMessage
    {
        uint8_t event;                             // +0x00 ProjectileEvent
        uint8_t _01[0xF];
        union
        {
            struct
            {
                uint32_t subject;                  // +0x10 the rider's index or the object
                ee::Float strength;                // +0x14 a blast's strength at the rider, 0 to 1
            };
            zge::VertexColour colour;              // +0x10 the owner's answer to kProjectileMarksFace
            zge::Vec4 point;                       // +0x10 where the face was hit
        };
        uint64_t face;                             // +0x20 the face hit
        uint8_t _28[8];
    };
    static_assert(sizeof(ProjectileMessage) == 0x30 && offsetof(ProjectileMessage, face) == 0x20);

    // The crosshairs are laid out on a rectangle of the screen (bmx/camera/action_camera.h: a camera's viewport).
    struct ScreenRectangle;

    // (weapons.cpp)
    uint32_t setUpWeapons();
    void closeWeapons();
    uint32_t weaponTask();
    Shooter *newShooter(const ShooterOwner *owner);
    void resetShooter(Shooter *shooter, const ShooterWeapons *weapons);
    void freeShooter(Shooter *shooter);
    // The next weapon type (the one before when `forward` is 0) the shooter
    // carries with a round left; the empty hand always counts.
    uint32_t nextUsableWeapon(const Shooter *shooter, uint32_t forward);
    uint32_t updateShooter(Shooter *shooter);

    // (projectiles.cpp)
    uint32_t updateProjectiles();
    void fireShot(Shooter *shooter);
    Projectile *newProjectile(Shooter *shooter, uint32_t kind);
    void freeProjectile(Projectile *projectile);
    void expireProjectile(Projectile *projectile);

    // (hits.cpp)
    // What the projectile meets on its path this frame (1: something).
    uint32_t findProjectileHit(ProjectilePath *path);
    // The owner told of what was met, and the projectile's end there.
    void projectileHit(Projectile *projectile, ProjectilePath *path);
    void projectileHitRider(Projectile *projectile, ProjectilePath *path);
    void projectileHitObject(Projectile *projectile, ProjectilePath *path);
    void projectileHitFace(Projectile *projectile, ProjectilePath *path);
    // A grenade's or rocket's blast where it is.
    void explodeProjectile(Projectile *projectile);

    // (crosshairs.cpp)
    void layoutWeaponCrosshairs(const ScreenRectangle *rectangle);
    void layoutCrosshair(ee::Ptr<zge::zgui::Node> crosshair, const ScreenRectangle *rectangle);
    void layoutScope(ee::Ptr<zge::zgui::Node> scope, const ScreenRectangle *rectangle, ee::Float scale);
}
