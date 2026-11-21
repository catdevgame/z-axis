// DM2's rider's and bike's bodies meeting the world (physics.c): newRider
// gives the rider model's dynamics (the bail's bodies) and the bike model's
// their contact callbacks. A hard enough touch makes its sound and, on a
// dusty surface of the level, dust; then the touch goes on to the level's
// own contact handling (dynamicsObjectContact), as every object's does.

#include "physics.h"

#include "../collision/collision.h"
#include "dm2/game/players.h"
#include "dm2/level/dynamics_objects.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "zge/collision/dynamics.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/rider/physics/body_contacts.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // A touch counts from a closing speed of 4 (the rider's bodies) or 5
        // (the bike's) on. The rider's hits come in four strengths: up to 9,
        // up to 17, up to 30, and harder.
        constexpr ee::Float kRiderTouchLeast = 4.0f;
        constexpr ee::Float kBikeTouchLeast = 5.0f;
        constexpr ee::Float kRiderHitSteps[3] = {9.0f, 17.0f, 30.0f};

        // The rider's body parts that sound (by their bones' names' hashes):
        // the sound's code is the part's: 0 a foot, 1 the hips, 2 a forearm,
        // 3 the head. Any other body makes none.
        inline ee::Global<int32_t, sym::g_hipsBoneHash> g_hipsBone;
        inline ee::Global<int32_t, sym::g_headBoneHash> g_headBone;
        inline ee::Global<int32_t, sym::g_leftLowArmBoneHash> g_leftLowArmBone;
        inline ee::Global<int32_t, sym::g_rightLowArmBoneHash> g_rightLowArmBone;
        inline ee::Global<int32_t, sym::g_leftFootBoneHash> g_leftFootBone;
        inline ee::Global<int32_t, sym::g_rightFootBoneHash> g_rightFootBone;
        constexpr int32_t kPartFoot = 0, kPartHips = 1, kPartForearm = 2, kPartHead = 3, kPartSilent = -1;

        // The sounds (the game's sound events; inferred from their use): a
        // body part's hit (its part and strength), a second one for a hit
        // past the first strength (at most once in 45 frames: the rider's
        // soundWait), and the bike's (a volume up to 127, at the touched
        // face's first corner).
        constexpr int32_t kSoundBodyHit = 23;
        constexpr int32_t kSoundHardHit = 24;
        constexpr int32_t kSoundBikeHit = 22;
        constexpr uint8_t kHardHitWait = 45;
        constexpr int32_t kSoundVolumeMost = 127;

        // Dust's motion is the body's velocity with the touch's push, over
        // a step (1 / 60); a rider's hit adds 0.15 of its speed to the
        // rider's rideDistance; the bike's volume is 0.19685 of its speed.
        inline ee::Global<ee::Float, sym::g_riderDustScale> g_riderDustScale;
        inline ee::Global<ee::Float, sym::g_riderHitDistance> g_riderHitDistance;
        inline ee::Global<ee::Float, sym::g_bikeDustScale> g_bikeDustScale;
        inline ee::Global<ee::Float, sym::g_bikeHitVolume> g_bikeHitVolume;

        // The surface sounds (the level face's; collision::faceSound) that
        // raise dust here: 5, 8 and 10 .. 12.
        bool raisesDust(uint32_t sound)
        {
            return sound == 5 || sound == 8 || (sound >= 10 && sound <= 12);
        }

        // Whether the touch is of a level face (not a scene object's) that
        // raises dust.
        bool dustyFace(const zge::FaceContact &contact)
        {
            if (!contact.object || zge::nodeSceneObject(contact.object->node, {}))
                return false;
            const collision::FaceRef face = collision::makeFace(ee::Address(contact.object->node), static_cast<int32_t>(contact.face));
            if (face == 0)
                return false;
            return raisesDust(collision::faceSound(face));
        }

        // The touch's push: how fast the body closes on the face.
        ee::Float closingSpeed(const zge::RigidBody &body, const zge::FaceContact &contact)
        {
            return -zge::dot(body.velocity, contact.normal);
        }

        // Dust at the body, moving as it does with the push taken off (put
        // together in `motion`, the handler's; its w is the frame's, as the
        // original's).
        void bodyDust(zge::RigidBody &body, const zge::FaceContact &contact, ee::Float speed, ee::Float scale,
                      ee::Ptr<zge::Vec4> motion)
        {
            motion->x = body.velocity.x + speed * contact.normal.x;
            motion->y = body.velocity.y + speed * contact.normal.y;
            motion->z = body.velocity.z + speed * contact.normal.z;
            *motion = *motion * scale;
            dust(&body.position.x, &motion->x);
        }
    }

    // riderDynamicsContact: a body of a player's rider (the rider whose
    // model's dynamics these are; none, and the touch is let be) touching
    // something harder than kRiderTouchLeast: the part's hit sounds, by
    // strength, a hard hit's second sound when the rider's wait allows, dust
    // on a dusty face, and the speed counted into the rider's rideDistance.
    uint32_t riderDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> motion;
        ee::Ptr<Rider> rider;
        for (uint32_t index = 0;; ++index)
        {
            const uint8_t players = *g_multiplayerPlayers;
            if (players == 0 ? index != 0 : index >= players)
                break;
            if (riderOf(index & 0xFF)->riderDynamics == ee::Address(dynamics))
            {
                rider = riderOf(index & 0xFF);
                break;
            }
        }
        if (!rider)
            return 1;

        const ee::Float speed = closingSpeed(*body, *contact);
        if (!(kRiderTouchLeast < speed))
            return dynamicsObjectContact(dynamics, body, contact);
        int32_t strength = 0;
        if (!(speed < kRiderHitSteps[0]))
            strength = !(speed < kRiderHitSteps[1]) ? (speed < kRiderHitSteps[2] ? 2 : 3) : 1;
        const bool dusty = dustyFace(*contact);

        const int32_t bone = static_cast<int32_t>(body->nameHash);
        int32_t part = kPartSilent;
        if (bone == *g_hipsBone)
            part = kPartHips;
        else if (bone == *g_headBone)
            part = kPartHead;
        else if (bone == *g_leftLowArmBone || bone == *g_rightLowArmBone)
            part = kPartForearm;
        else if (bone == *g_leftFootBone || bone == *g_rightFootBone)
            part = kPartFoot;
        if (part != kPartSilent)
        {
            soundEvent(kSoundBodyHit, &body->position.x, part, strength);
            if (rider->soundWait == 0 && strength != 0)
            {
                soundEvent(kSoundHardHit, &body->position.x, part, strength);
                rider->soundWait = kHardHitWait;
            }
        }
        if (dusty)
            bodyDust(*body, *contact, speed, *g_riderDustScale, motion);
        rider->rideDistance += ee::toInt(speed * *g_riderHitDistance);
        return dynamicsObjectContact(dynamics, body, contact);
    }
    READABLE_FUNCTION_WAITS(riderDynamicsContact, Verified);

    // bikeDynamicsContact: the bike's frame (its dynamics' first body)
    // touching something harder than kBikeTouchLeast: dust on a dusty face
    // and the bike's hit, as loud as the touch is hard.
    uint32_t bikeDynamicsContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact)
    {
        ee::Local<zge::Vec4> motion;
        if (dynamics->bodies != body)
            return dynamicsObjectContact(dynamics, body, contact);
        const ee::Float speed = closingSpeed(*body, *contact);
        if (!(kBikeTouchLeast < speed))
            return dynamicsObjectContact(dynamics, body, contact);
        if (dustyFace(*contact))
            bodyDust(*body, *contact, speed, *g_bikeDustScale, motion);
        const int32_t loud = ee::toInt(speed * *g_bikeHitVolume);
        soundEvent(kSoundBikeHit, &contact->vertices[0].x, 0, loud < kSoundVolumeMost ? loud : kSoundVolumeMost);
        return dynamicsObjectContact(dynamics, body, contact);
    }
    READABLE_FUNCTION_WAITS(bikeDynamicsContact, Verified);
}
