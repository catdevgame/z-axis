// BMX XXX's weapons (weapon.h): the weapons' models and widgets loaded for a
// level, and the shooters: who carries which weapons, the one in hand, its
// ammunition, zoom and recoil, and the buttons that fire, reload and change
// it (./src/com/action/weapon.c).

#include "weapon.h"

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/zfile_library.h"
#include "zge/collision/collision.h"
#include "zge/game.h"
#include "zge/math/matrix.h"
#include "zge/platform/tasks.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/weapon/weapons.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using zge::zgui::Node;

    namespace
    {
        inline ee::Global<WeaponType[kWeaponTypes], sym::g_weaponTypes> g_weaponTypes;
        inline ee::Global<ee::Ptr<char>[kWeaponTypes], sym::g_weaponNames> g_weaponNames;
        inline ee::Global<ee::Ptr<char>[kProjectileKinds], sym::g_projectileNames> g_projectileNames;
        // The models: each weapon type's ("weapon.<name>") and each
        // projectile kind's ("weapon.proj.<name>"), 0 where the level's
        // models have none.
        inline ee::Global<ee::Ptr<Scene>[kWeaponTypes], sym::g_weaponModels> g_weaponModels;
        inline ee::Global<ee::Ptr<Scene>[kProjectileKinds], sym::g_projectileModels> g_projectileModels;
        // The weapons' widgets ("UI_Weapon") and the weapons' task.
        inline ee::Global<ee::Ptr<Node>, sym::g_weaponUi> g_weaponUi;
        inline ee::Global<ee::Ptr<Task>, sym::g_weaponTask> g_weaponTask;
        inline ee::Global<Shooter[kShooters], sym::g_shooters> g_shooters;
        inline ee::Global<Projectile[kProjectiles], sym::g_projectiles> g_projectiles;
        // How many projectiles are in flight, and the slot the next one tries first.
        inline ee::Global<uint32_t, sym::g_projectileCount> g_projectileCount;
        inline ee::Global<uint32_t, sym::g_nextProjectileSlot> g_nextProjectileSlot;
        // The view's angle at each zoom level: 60, 30, 20 and 10 degrees.
        inline ee::Global<ee::Float[4], sym::g_zoomFovs> g_zoomFovs;

        constexpr uint32_t kWeaponModelName = sym::bmx::g_weaponModelFormat;         // "weapon.%s"
        constexpr uint32_t kProjectileModelName = sym::bmx::g_projectileModelFormat; // "weapon.proj.%s"
        constexpr uint32_t kCrosshairName = sym::bmx::g_weaponCrosshairFormat;       // "Weapon_%s.Crosshair"

        // The shooter's buttons.
        constexpr uint32_t kFireButtons = 0x30C0;
        constexpr uint32_t kReloadButtons = 0x30;
        constexpr uint32_t kPreviousWeaponButton = 0x100;
        constexpr uint32_t kNextWeaponButton = 0x200;
        constexpr uint32_t kZoomOutButton = 0x400;
        constexpr uint32_t kZoomInButton = 0x800;

        // The sound events a shooter sends from its position. All but the
        // zoom's and the empty click's carry the weapon type less one.
        constexpr uint32_t kSoundTriggerPulled = 0x26;
        constexpr uint32_t kSoundShot = 0x27;
        constexpr uint32_t kSoundMakingReady = 0x28;
        constexpr uint32_t kSoundReload = 0x29;
        constexpr uint32_t kSoundLoading = 0x2A;
        constexpr uint32_t kSoundPutAway = 0x2B;
        constexpr uint32_t kSoundTakenOut = 0x2C;
        constexpr uint32_t kSoundEmpty = 0x2D;
        constexpr uint32_t kSoundZoom = 0x2E;

        constexpr ee::Float kFramesASecond = ee::Float(60.0f);
        // An angle as a part of a turn, and back.
        constexpr ee::Float kTurnsAnAngle = ee::Float(1.0f / 65536.0f);
        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);
        // The part of the way to the zoom level's angle the view goes each frame.
        constexpr ee::Float kZoomEasing = std::bit_cast<float>(0x3EB33333u); // 0.35

        constexpr uint32_t kWeaponTaskSourceLine = 0x185;

        // A model of the level's library, hidden and out of the collision
        // until something shows a copy of it. 0 when the library has none.
        void loadHiddenModel(ee::Ptr<ZfileLibrary> library, ee::Ptr<Scene> *slot, const char *name)
        {
            const uint32_t hash = nameHash(name);
            if (findAsset(ee::Address(library), hash) == 0)
                return;
            *slot = ee::Ptr<Scene>(loadLibraryModelEntry(ee::Address(library), hash));
            if (*slot != 0)
            {
                const ee::Ptr<SceneBone> root = rootBone(*slot);
                const ee::Ptr<zge::Node> node = boneNode(*slot, root);
                setNodeCollides(node, 0, 1);
                setNodeShown(node, 0, 1);
            }
        }

        // A shooter's projectiles in flight freed.
        void freeProjectilesOf(Shooter *shooter)
        {
            uint32_t left = *g_projectileCount;
            for (uint32_t slot = 0; left != 0 && slot < kProjectiles; ++slot)
            {
                Projectile &projectile = (*g_projectiles)[slot];
                if ((projectile.flags & kProjectileInUse) == 0)
                    continue;
                if (projectile.shooter == shooter)
                    freeProjectile(&projectile);
                --left;
            }
        }

        // The sound a shooter sends for the weapon in hand.
        void weaponSound(Shooter *shooter, uint32_t event)
        {
            soundEvent(event, &shooter->position, shooter->weapon - 1u, 0, 0);
        }

        // A state begun: it lasts the weapon type's time for it.
        void beginState(Shooter *shooter, uint32_t state)
        {
            shooter->state = static_cast<uint8_t>(state);
            const WeaponType &type = (*g_weaponTypes)[shooter->weapon];
            shooter->stateFrames = static_cast<uint16_t>(ee::toInt((&type.stateSeconds[0])[state] * kFramesASecond));
        }

        // How far the weapon may be zoomed out and in.
        uint32_t leastZoom(const WeaponType &type) { return (type.flags & kWeaponSights) ? 0 : 1; }
        uint32_t mostZoom(const WeaponType &type) { return (type.flags & kWeaponScope) ? 3 : 1; }

        // One step of the shooter's weapon handling, taken when the state's
        // time is up. Returns whether a state began (it may last no time:
        // the next step is taken at once).
        bool shooterStep(Shooter *shooter, uint32_t held, uint32_t changed)
        {
            ee::Local<Projectile> asked;
            ee::Local<ProjectileMessage> message;
            const uint32_t pressed = held & changed;
            const uint32_t weapon = shooter->weapon;
            const WeaponType &type = (*g_weaponTypes)[weapon];
            Shooter::Ammunition &rounds = shooter->ammunition[weapon];
            switch (shooter->state)
            {
            case kShooterReady:
            {
                if (weapon != kNoWeapon && (held & kFireButtons) != 0)
                {
                    if (rounds.loaded != 0)
                    {
                        // The owner may forbid the shot.
                        if (shooter->owner)
                        {
                            memoryFill(asked, 0, sizeof(Projectile));
                            asked->shooter = shooter;
                            message->event = kProjectileMayFire;
                            asked->kind = type.projectile;
                            if (shooter->owner(asked, message) != 1)
                                return false;
                        }
                        shooter->state = kShooterFiring;
                        weaponSound(shooter, kSoundTriggerPulled);
                        beginState(shooter, shooter->state);
                        return true;
                    }
                    if ((changed & kFireButtons) == 0)
                        return false;
                    if (rounds.reserve == 0)
                    {
                        soundEvent(kSoundEmpty, &shooter->position, 0, 0, 0);
                        return false;
                    }
                    weaponSound(shooter, kSoundReload);
                    beginState(shooter, kShooterReloading);
                    return true;
                }
                if ((pressed & kReloadButtons) != 0 && rounds.loaded < type.clip && rounds.reserve != 0)
                {
                    weaponSound(shooter, kSoundReload);
                    beginState(shooter, kShooterReloading);
                    return true;
                }
                uint32_t next = weapon;
                if ((pressed & kNextWeaponButton) != 0)
                    next = nextUsableWeapon(shooter, 1);
                if (next == shooter->weapon)
                {
                    if ((pressed & kPreviousWeaponButton) == 0)
                        return false;
                    next = nextUsableWeapon(shooter, 0);
                    if (next == shooter->weapon)
                        return false;
                }
                weaponSound(shooter, kSoundPutAway);
                shooter->nextWeapon = static_cast<uint8_t>(next);
                beginState(shooter, kShooterPuttingAway);
                return true;
            }
            case kShooterFiring:
                fireShot(shooter);
                weaponSound(shooter, kSoundShot);
                beginState(shooter, kShooterFired);
                return true;
            case kShooterFired:
                if ((type.flags & kWeaponRepeats) != 0 && rounds.loaded != 0)
                {
                    if ((held & kFireButtons) != 0)
                    {
                        // The next shot, with no time between.
                        shooter->stateFrames = 0;
                        shooter->state = kShooterFiring;
                    }
                    else
                    {
                        beginState(shooter, kShooterReady);
                    }
                    return true;
                }
                if ((type.flags & kWeaponRepeats) == 0 && rounds.loaded != 0)
                {
                    weaponSound(shooter, kSoundMakingReady);
                    beginState(shooter, kShooterMakingReady);
                    return true;
                }
                // Empty: reloaded at once when there is a reserve.
                if (rounds.reserve != 0 && (type.flags & kWeaponNoReloadWhenEmpty) == 0)
                {
                    weaponSound(shooter, kSoundReload);
                    beginState(shooter, kShooterReloading);
                }
                else
                {
                    beginState(shooter, kShooterReady);
                }
                return true;
            case kShooterMakingReady:
                beginState(shooter, kShooterReady);
                return true;
            case kShooterReloading:
                weaponSound(shooter, kSoundLoading);
                beginState(shooter, kShooterLoading);
                return true;
            case kShooterLoading:
            {
                // As many rounds as the clip has room for, or as are left.
                const uint32_t room = static_cast<uint32_t>(type.clip) - rounds.loaded;
                const uint32_t moved = room < rounds.reserve ? room : rounds.reserve;
                rounds.loaded = static_cast<uint16_t>(rounds.loaded + moved);
                rounds.reserve = static_cast<uint16_t>(rounds.reserve - moved);
                weaponSound(shooter, kSoundMakingReady);
                beginState(shooter, kShooterMakingReady);
                return true;
            }
            case kShooterPuttingAway:
                // The next weapon comes to hand, a scoped one zoomed in.
                shooter->weapon = shooter->nextWeapon;
                shooter->zoom = ((*g_weaponTypes)[shooter->weapon].flags & kWeaponScope) ? 1 : 0;
                weaponSound(shooter, kSoundTakenOut);
                beginState(shooter, kShooterTakingOut);
                return true;
            case kShooterTakingOut:
                if (rounds.loaded == 0)
                {
                    beginState(shooter, kShooterReady);
                }
                else
                {
                    weaponSound(shooter, kSoundMakingReady);
                    beginState(shooter, kShooterMakingReady);
                }
                return true;
            default:
                beginState(shooter, shooter->state);
                return true;
            }
        }
    }

    // setUpWeapons: the weapons made ready for a level: the weapon types'
    // and the projectiles' models from the level's model library (hidden),
    // the weapons' widgets with the crosshairs laid out over the whole
    // screen, no projectile in flight, and the weapons' task.
    uint32_t setUpWeapons()
    {
        ee::Local<ScreenRectangle> screen;
        ee::Local<char[0x40]> name;
        ee::Local<uint32_t> visible;
        const ee::Ptr<ZfileLibrary> library = openZfileLibrary(sym::g_weaponModelLibraryName); // "assets\models.zal"
        if (library != 0)
        {
            for (uint32_t type = 0; type < kWeaponTypes; ++type)
            {
                (*g_weaponModels)[type] = ee::Ptr<Scene>();
                formatString(name, kWeaponModelName, (*g_weaponNames)[type]);
                loadHiddenModel(library, &(*g_weaponModels)[type], name);
            }
            for (uint32_t kind = 0; kind < kProjectileKinds; ++kind)
            {
                (*g_projectileModels)[kind] = ee::Ptr<Scene>();
                formatString(name, kProjectileModelName, (*g_projectileNames)[kind]);
                loadHiddenModel(library, &(*g_projectileModels)[kind], name);
            }
            closeZfileLibrary(library);
        }
        zge::zgui::loadZguiScript(sym::g_weaponScriptName, zgui::g_zgui->root); // "weapon.zsc"
        const ee::Ptr<Node> ui = zge::zgui::findNode(zgui::g_zgui->root, sym::g_weaponUiName, 1); // "UI_Weapon"
        *visible = 1;
        *g_weaponUi = ui;
        zge::zgui::setVariable(ui, sym::g_weaponVisible, visible); // "Visible"
        screen->width = ee::Float(512.0f);
        screen->height = ee::Float(448.0f);
        screen->x = ee::Float(0.0f);
        screen->y = ee::Float(0.0f);
        layoutWeaponCrosshairs(screen);
        memoryFill(&*g_projectiles, 0, sizeof(Projectile) * kProjectiles);
        *g_nextProjectileSlot = 0;
        *g_projectileCount = 0;
        *g_weaponTask = newTask(1, sym::weaponTask, 0, 1, sym::g_weaponSourceFile, kWeaponTaskSourceLine); // "./src/com/action/weapon.c"
        return 1;
    }
    READABLE_FUNCTION(setUpWeapons, Verified);

    // closeWeapons: the task, every projectile in flight, every shooter,
    // the widgets and the models freed.
    void closeWeapons()
    {
        deleteTask(*g_weaponTask);
        for (uint32_t slot = 0; slot < kProjectiles; ++slot)
        {
            if ((*g_projectiles)[slot].flags & kProjectileInUse)
                freeProjectile(&(*g_projectiles)[slot]);
        }
        for (uint32_t slot = 0; slot < kShooters; ++slot)
        {
            if ((*g_shooters)[slot].flags & kShooterInUse)
                freeShooter(&(*g_shooters)[slot]);
        }
        zge::zgui::destroyNode(*g_weaponUi);
        for (uint32_t type = 0; type < kWeaponTypes; ++type)
        {
            if ((*g_weaponModels)[type] != 0)
            {
                releaseSceneAssetsScene((*g_weaponModels)[type]);
                (*g_weaponModels)[type] = ee::Ptr<Scene>();
            }
        }
        for (uint32_t kind = 0; kind < kProjectileKinds; ++kind)
        {
            if ((*g_projectileModels)[kind] != 0)
            {
                releaseSceneAssetsScene((*g_projectileModels)[kind]);
                (*g_projectileModels)[kind] = ee::Ptr<Scene>();
            }
        }
    }
    READABLE_FUNCTION(closeWeapons, Verified);

    // weaponTask: the weapons' task does nothing (the game's frame calls
    // updateProjectiles and the owners call updateShooter).
    uint32_t weaponTask()
    {
        return 1;
    }
    READABLE_FUNCTION(weaponTask, Verified);

    // newShooter: a free shooter given to an owner, with the empty hand
    // only. 0 when all four are in use.
    Shooter *newShooter(const ShooterOwner *owner)
    {
        ee::Local<ShooterWeapons> weapons;
        for (uint32_t slot = 0; slot < kShooters; ++slot)
        {
            Shooter *shooter = &(*g_shooters)[slot];
            if (shooter->flags & kShooterInUse)
                continue;
            memoryFill(shooter, 0, sizeof(Shooter));
            shooter->buttons = owner->buttons;
            shooter->ui = *g_weaponUi;
            weapons->owned = 0;
            shooter->owner = owner->handler;
            shooter->ownerData = owner->data;
            weapons->flags = owner->flags;
            resetShooter(shooter, weapons);
            return shooter;
        }
        return nullptr;
    }
    READABLE_FUNCTION(newShooter, Verified);

    // resetShooter: the shooter given a set of weapons, each with a full
    // clip and its type's reserve: nothing in hand, the first usable weapon
    // coming to hand, no zoom, every crosshair and the scope hidden, and its
    // projectiles in flight freed. The first usable weapon is picked before
    // the ammunition is filled, by what the shooter had left.
    void resetShooter(Shooter *shooter, const ShooterWeapons *weapons)
    {
        ee::Local<char[0x40]> name;
        ee::Local<uint32_t> visible;
        shooter->flags = weapons->flags | kShooterInUse;
        shooter->weapon = kNoWeapon;
        shooter->owned = weapons->owned | 1u;
        shooter->nextWeapon = static_cast<uint8_t>(nextUsableWeapon(shooter, 1));
        shooter->state = kShooterPuttingAway;
        shooter->fov = (*g_zoomFovs)[0];
        shooter->stateFrames = 0;
        shooter->zoom = 0;
        *visible = 0;
        for (uint32_t type = 0; type < kWeaponTypes; ++type)
        {
            shooter->ammunition[type].loaded = (*g_weaponTypes)[type].clip;
            shooter->ammunition[type].reserve = (*g_weaponTypes)[type].reserve;
            if (shooter->flags & kShooterNoCrosshair)
                continue;
            formatString(name, kCrosshairName, (*g_weaponNames)[type]);
            const ee::Ptr<Node> crosshair = zge::zgui::findNode(shooter->ui, name.address(), 1);
            if (crosshair != 0)
                zge::zgui::setVariable(crosshair, sym::g_weaponVisible, visible);
        }
        if ((shooter->flags & kShooterNoCrosshair) == 0)
            zge::zgui::setVariable(zge::zgui::findNode(shooter->ui, sym::g_weaponScopeName, 1), sym::g_weaponVisible, visible); // "Scope"
        freeProjectilesOf(shooter);
    }
    READABLE_FUNCTION(resetShooter, Verified);

    // freeShooter: its projectiles in flight freed, and the shooter free.
    void freeShooter(Shooter *shooter)
    {
        freeProjectilesOf(shooter);
        shooter->flags &= static_cast<uint8_t>(~kShooterInUse);
    }
    READABLE_FUNCTION(freeShooter, Verified);

    // nextUsableWeapon: from the weapon in hand, the next type (or the one
    // before) the shooter carries that has a round loaded or in reserve; the
    // empty hand always counts. The weapon in hand when there is no other.
    uint32_t nextUsableWeapon(const Shooter *shooter, uint32_t forward)
    {
        const int32_t step = forward ? 1 : -1;
        const int32_t inHand = shooter->weapon;
        int32_t type = inHand + step;
        if (type < 0)
            type = kWeaponTypes - 1;
        if (type >= static_cast<int32_t>(kWeaponTypes))
            type = 0;
        while (type != inHand)
        {
            if ((static_cast<int32_t>(shooter->owned) >> type) & 1)
            {
                if (shooter->ammunition[type].loaded != 0 || shooter->ammunition[type].reserve != 0 || type == kNoWeapon)
                    break;
            }
            type += step;
            if (type < 0)
                type = kWeaponTypes - 1;
            if (type >= static_cast<int32_t>(kWeaponTypes))
                type = 0;
        }
        return static_cast<uint8_t>(type);
    }
    READABLE_FUNCTION(nextUsableWeapon, Verified);

    // updateShooter: a shooter's frame. The recoil is taken back a part and
    // turned into the aim; the zoom buttons move the zoom level (only while
    // the weapon is ready, and a weapon keeps its zoom through a shot only
    // if its type says so) and the view's angle eases to the level's; the
    // crosshair or the scope is shown (neither while the game's camera is
    // away, the weapon is changing, or the shooter has no crosshair); then
    // the weapon is handled when its state's time is up: fire, reload and
    // change weapon by the buttons.
    uint32_t updateShooter(Shooter *shooter)
    {
        ee::Local<char[0x40]> name;
        ee::Local<Matrix> turned;
        ee::Local<Matrix> turn;
        ee::Local<uint32_t> visible;

        uint32_t held = shooter->buttons->held;
        uint32_t changed = shooter->buttons->changed;
        if (shooter->flags & kShooterIgnoresButtons)
        {
            held = 0;
            changed = 0;
        }

        const ee::Float recovery = (*g_weaponTypes)[shooter->weapon].recoilRecovery;
        shooter->recoilPitch = static_cast<Angle>(shooter->recoilPitch - ee::toInt(ee::toFloat(shooter->recoilPitch) * kTurnsAnAngle * recovery * kAnglesATurn));
        shooter->recoilYaw = static_cast<Angle>(shooter->recoilYaw - ee::toInt(ee::toFloat(shooter->recoilYaw) * kTurnsAnAngle * recovery * kAnglesATurn));
        matrixIdentity(turn);
        matrixRotateX(turn, turn, shooter->recoilPitch);
        matrixMultiply(turned, &shooter->aim, turn);
        matrixIdentity(turn);
        matrixRotateY(turn, turn, shooter->recoilYaw);
        matrixMultiply(&shooter->aim, turned, turn);

        uint32_t zoom = 0;
        const WeaponType &type = (*g_weaponTypes)[shooter->weapon];
        if (type.flags & (kWeaponSights | kWeaponScope))
        {
            uint32_t least = 0;
            uint32_t most = 0;
            switch (shooter->state)
            {
            case kShooterFiring:
            case kShooterFired:
            case kShooterMakingReady:
                if (type.flags & kWeaponZoomsWhileFiring)
                {
                    most = mostZoom(type);
                    least = leastZoom(type);
                }
                break;
            case kShooterReloading:
            case kShooterLoading:
            case kShooterPuttingAway:
            case kShooterTakingOut:
                break;
            default:
            {
                const uint32_t pressed = held & changed;
                most = mostZoom(type);
                least = leastZoom(type);
                if ((pressed & kZoomInButton) != 0 && shooter->zoom < most)
                {
                    ++shooter->zoom;
                    soundEvent(kSoundZoom, &shooter->position, 0, 0, 0);
                }
                else if ((pressed & kZoomOutButton) != 0 && least < shooter->zoom)
                {
                    --shooter->zoom;
                    soundEvent(kSoundZoom, &shooter->position, 0, 0, 0);
                }
                break;
            }
            }
            zoom = shooter->zoom < most ? shooter->zoom : most;
            if (zoom < least)
                zoom = least;
        }
        shooter->fov = shooter->fov + ((*g_zoomFovs)[zoom] - shooter->fov) * kZoomEasing;

        if ((shooter->flags & kShooterNoCrosshair) == 0)
        {
            *visible = 1;
            zge::zgui::setVariable(shooter->ui, sym::g_weaponVisible, visible); // "Visible"
            // The scope's picture while zoomed in.
            uint32_t scoped = 0;
            if (zoom != 0)
                scoped = cameraState() == 0 ? 1 : 0;
            *visible = (shooter->flags & kShooterNoCrosshair) ? 0 : scoped;
            zge::zgui::setVariable(zge::zgui::findNode(shooter->ui, sym::g_weaponScopeName, 1), sym::g_weaponVisible, visible); // "Scope"
            // The crosshair of the weapon in hand; a scoped weapon's only
            // while zoomed in.
            if (shooter->state == kShooterPuttingAway || shooter->state == kShooterTakingOut)
            {
                *visible = 0;
            }
            else
            {
                *visible = 1;
                if ((*g_weaponTypes)[shooter->weapon].flags & kWeaponScope)
                    *visible = zoom != 0 ? 1 : 0;
            }
            const uint32_t shown = cameraState() != 0 ? 0 : (*visible & 1);
            *visible = (shooter->flags & kShooterNoCrosshair) ? 0 : shown;
            formatString(name, kCrosshairName, (*g_weaponNames)[shooter->weapon]);
            const ee::Ptr<Node> crosshair = zge::zgui::findNode(shooter->ui, name.address(), 1);
            if (crosshair != 0)
                zge::zgui::setVariable(crosshair, sym::g_weaponVisible, visible);
        }

        while (shooter->stateFrames == 0)
        {
            if (!shooterStep(shooter, held, changed))
                break;
        }
        if (shooter->stateFrames != 0)
            --shooter->stateFrames;
        return 1;
    }
    READABLE_FUNCTION(updateShooter, Verified);
}
