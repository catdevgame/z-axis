// The skater carried by what it rides: carryRider, run by the state updates
// (grind, stall, wall ride, poles, the ground's move). The object under the
// skater (by its state: the rail ground, the wall ridden, or the ground
// object) is followed through Player::carrier: on the frame it changes the
// object's matrices are kept; from the next, if the object moves (isMover),
// the skater's orientation is turned by the object's turn since the last frame
// and the point it rides (the wall tap's point, the skitch's hold, else its
// position) carried with the object: the move goes to Player::carriedMove,
// which the moves add to the position. The rail's ends or the wall's corners
// are worked out again in the world. Then the carried objects' frame (the
// engine's carried objects, zge/scene/carried_objects.h: Animal Rescue's
// parrots on the skater): each follows its carrier, and the parrots call.

#include "moves.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "aggressive/skater/skater.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"

#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/node.h"
#include "zge/scene/message.h"
#include "zge/math/quaternion.h"
#include "zge/text/cstring.h"
#include "aggressive/sound/events.h"
#include "zge/scene/carried_objects.h"

#include <cstddef>
#include "sym/aggressive/skater/carried.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        using zge::Matrix;

        // nodeMatrix's second argument.
        constexpr uint32_t kWorldMatrix = 1, kInverseWorldMatrix = 2;

        const Vec4 kNoMove{0.0f, 0.0f, 0.0f, 1.0f};

        ee::Ptr<Matrix> objectMatrix(ee::Ptr<zge::Node> object, uint32_t which)
        {
            return zge::nodeMatrix(ee::Ptr<zge::Node>(object), which);
        }

        // The object in one of the skater's fields, its frame worked out first.
        ee::Ptr<zge::Node> updatedObject(ee::Ptr<zge::Node> object)
        {
            if (object)
                zge::updateMovedNode(zge::rootNode(object));
            return object;
        }

        bool ridesRail(State state)
        {
            return state == State::Grind || state == State::HandGrind || state == State::Stall || state == State::Skitch;
        }
        bool ridesWall(State state)
        {
            return state == State::WallRide || state == State::WallTap;
        }

        void keepMatrices(RiddenObject &carrier)
        {
            carrier.inverse = *objectMatrix(carrier.object, kInverseWorldMatrix);
            carrier.world = *objectMatrix(carrier.object, kWorldMatrix);
        }
    }

    // carryRider: see the top of the file.
    void carryRider(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Matrix> turn;
        RiddenObject &carrier = player->carrier;
        carrier.frameTime = frameTime;

        // The object ridden; on a change, kept for the next frame.
        const State state = player->state;
        const bool onGround = !ridesRail(state) && !ridesWall(state);
        const ee::Ptr<zge::Node> object = ridesRail(state)   ? updatedObject(player->grindRail)
                                          : ridesWall(state) ? updatedObject(player->wallContact.object)
                                                             : updatedObject(ee::Ptr<zge::Node>(player->object));
        bool carrying;
        if (object != carrier.object)
        {
            carrier.object = object;
            carrier.kept = object ? 0 : 1;
            if (!object && onGround)
                carrier.moving = 0;
            carrying = object != 0;
        }
        else
        {
            if (!object && onGround)
            {
                carrier.moving = 0;
                carrier.kept = 1;
            }
            if (!carrier.kept)
            {
                // The frame after a change: does the object move?
                objectMatrix(carrier.object, kWorldMatrix); // looked up and not used
                carrier.moving = static_cast<uint8_t>(isMover(carrier.object));
                carrier.kept = 1;
            }
            carrying = carrier.moving != 0;
        }

        if (!carrying)
        {
            // Off a moving object: nothing carried any more.
            if (carrier.moving)
            {
                carrier.object = {};
                carrier.moving = 0;
                carrier.move = kNoMove;
            }
            return;
        }

        if (!carrier.kept)
        {
            // A new object: its matrices kept, the point to carry taken.
            keepMatrices(carrier);
            if (player->state == State::WallTap)
                carrier.carriedPoint = zge::transformed(carrier.inverse, player->wallContact.point);
            else if (player->state == State::Skitch)
                carrier.carriedPoint = zge::transformed(carrier.inverse, player->skitchHold);
            else
                carrier.carriedPoint = player->position;
            carrier.move = kNoMove;
            carrier.moving = 0;
            return;
        }

        // Carried: the skater turned with the object, the point moved with it.
        zge::matrixMultiply(turn, &carrier.inverse, objectMatrix(carrier.object, kWorldMatrix));
        zge::matrixMultiply(&player->orientation, turn, &player->orientation);
        if (player->state == State::WallTap || player->state == State::Skitch)
        {
            // The point in the object's frame: where it was and is in the world.
            const Vec4 before = zge::transformed(carrier.world, carrier.carriedPoint);
            keepMatrices(carrier);
            const Vec4 after = zge::transformed(carrier.world, carrier.carriedPoint);
            carrier.move = after - before;
            if (player->state == State::WallTap)
                player->wallContact.point = after;
            else if (player->state == State::Skitch)
                player->skitchHold = after;
        }
        else
        {
            // Last frame's position, in the object's frame then, put where the
            // object is now.
            const Vec4 onObject = zge::transformed(carrier.inverse, carrier.carriedPoint);
            keepMatrices(carrier);
            const Vec4 after = zge::transformed(carrier.world, onObject);
            carrier.move = after - carrier.carriedPoint;
            carrier.carriedPoint = player->position;
        }

        // What the skater rides, in the world again.
        if (ridesRail(player->state))
            railToWorld(&player->rail);
        else if (ridesWall(player->state))
            wallToWorld(&player->wallContact);
        carrier.moving = 1;
    }
    READABLE_FUNCTION_VU0(carryRider, Verified); // grind.cpp and wall.cpp read the VU0 it leaves

    namespace
    {
        inline ee::Global<int32_t, sym::g_animalCallTicks> g_animalCallTicks; // its name is a low-confidence guess (initCarriedAnimals clears it)
    }

    namespace
    {
        // soundEvent's speech: stream `a` on the speech channel.
        constexpr uint32_t kPlaySpeech = 79;
        constexpr uint32_t kCallSpeechLevel0 = 182;
        constexpr uint32_t kCallSpeechLevel1 = 183, kCallSpeechLevel1InAir = 184;
        // How far g_animalCallTicks runs before the call in each level.
        constexpr uint32_t kCallTicksLevel0 = 781, kCallTicksLevel1 = 361;

        namespace tail
        {
        void soundEvent(uint32_t event, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { game::tail(aggressive::soundEvent, event, a, b, c, d); }
        }
    }

    // animalCallSound: a carried animal's call, outside split screen and only
    // in levels 0 and 1: once g_animalCallTicks has reached the level's
    // count, it starts again and the call is played (speech 182 in level 0;
    // in level 1, 184 while the first player is in the air (a jump, quarter
    // pipe or spine air), else 183).
    void animalCallSound()
    {
        const ee::Ptr<skater::Player> player = levelPlayer(0);
        if (*g_gameType == GameType::SplitScreen)
            return;
        const int32_t level = g_gameState->level;
        if (level == 0 && static_cast<uint32_t>(*g_animalCallTicks) >= kCallTicksLevel0)
        {
            *g_animalCallTicks = 0;
            tail::soundEvent(kPlaySpeech, kCallSpeechLevel0, 0, 0, 0);
            return;
        }
        if (level != 1 || static_cast<uint32_t>(*g_animalCallTicks) < kCallTicksLevel1)
            return;
        *g_animalCallTicks = 0;
        const uint8_t sinceAir = static_cast<uint8_t>(static_cast<uint8_t>(player->state) - static_cast<uint8_t>(skater::State::Air));
        const bool inAir = sinceAir <= static_cast<uint8_t>(skater::State::AirSpine) - static_cast<uint8_t>(skater::State::Air);
        tail::soundEvent(kPlaySpeech, inAir ? kCallSpeechLevel1InAir : kCallSpeechLevel1, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(animalCallSound, Verified);

    // updateCarriedAnimal: the animal follows its carrier: on the carrier's
    // place and turn when it sits on no bone; on a bone, at the bone's
    // point in the world, carried by the carrier's turned root offset, and
    // turned as the bone. Each call also counts towards the animals' call
    // (animalCallSound).
    void updateCarriedAnimal(ee::Ptr<zge::CarriedAnimal> carried)
    {
        using zge::Vec4;
        animalCallSound();
        const ee::Ptr<zge::SceneObject> carrierObject(carried->carrier->carrier); // the carrier record's word
        if (carried->bone == 0)
        {
            zge::setObjectPosition(ee::Ptr<zge::SceneObject>(carried->animal), zge::objectPosition(carrierObject));
            zge::setObjectOrientation(ee::Ptr<zge::SceneObject>(carried->animal), zge::objectOrientation(carrierObject));
            return;
        }

        // In the original's frame order: the origin, the animal's point,
        // the root's offset and point, the offset turned, the move.
        ee::Local<Vec4> origin, point, rootOffset, rootPoint, turnedOffset, move;
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Quaternion> orientation;
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(carrierObject);
        *origin = zge::kZero;
        zge::updatePose(carrierObject);
        const ee::Ptr<zge::SceneBone> root(zge::rootBone(scene));
        zge::bonePoint(scene, root, origin, rootOffset, zge::kBoneInParent);
        zge::bonePoint(scene, root, origin, rootPoint, zge::kBoneInWorld);
        zge::quaternionToMatrix(turn, zge::objectOrientation(carrierObject));
        *turnedOffset = zge::rotated(*turn, *rootOffset);
        const Vec4 &position = *zge::objectPosition(carrierObject);
        *point = *turnedOffset + position;
        *move = *point - *rootPoint;
        zge::bonePoint(scene, carried->bone, origin, point, zge::kBoneInWorld);
        *point = *point + *move;
        zge::setObjectPosition(ee::Ptr<zge::SceneObject>(carried->animal), point);
        zge::boneMatrix(scene, carried->bone, turn, zge::kBoneInWorld);
        zge::matrixToQuaternion(orientation, turn);
        zge::setObjectOrientation(ee::Ptr<zge::SceneObject>(carried->animal), orientation);
    }
    READABLE_FUNCTION_WAITS(updateCarriedAnimal, Verified);

    // updateCarriedAnimals: (a carrier's record, from the tree's walk) each
    // animal it carries put in place; an animal its update lets go is
    // stepped over. Returns 1 (go on).
    uint32_t updateCarriedAnimals(ee::Ptr<zge::AnimalCarrier> carrier)
    {
        ee::Ptr<zge::CarriedAnimal> previous;
        const auto after = [&] { return previous ? previous->next : carrier->animals.head; };
        for (ee::Ptr<zge::CarriedAnimal> animal = carrier->animals.head; animal; animal = after())
        {
            updateCarriedAnimal(animal);
            if (after() == animal)
                previous = animal;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateCarriedAnimals, Verified);
}
