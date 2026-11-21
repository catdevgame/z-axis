// The follow camera's frame (camera.h): the look-at point above the skater,
// the eye behind it at the camera's distance, kept out of walls by
// collision queries, the view turned toward the wanted direction.
// updateFollowCamera runs it each frame; its steps are updateCameraLookAt (the look-at
// point), turnCameraView (the view's turn and the eye) and keepCameraEyeClear (the
// eye kept clear of the level). Vector work is zge/math/vec.h: on VU0, in
// the original's order.

#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/moves.h"
#include "aggressive/camera.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/collision/collision.h"
#include "zge/assets/tag_table.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/sound/sound.h"
#include "zge/render/fx_manager.h"
#include "zge/scene/animation.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene_database.h"
#include "aggressive/sound/sound_mode.h"
#include "aggressive/npc.h"
#include "aggressive/pads.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/camera.cpp.h"

namespace aggressive
{
    namespace
    {
        using skater::Player;
        using skater::PlayerAxes;
        using skater::State;
        using zge::Vec4;

        constexpr uint32_t kEaseStep = sym::easeStep;       // (camera, seconds, factor) -> factor; keeps camera->easeStep
        constexpr uint32_t kSetEye = sym::setEye;         // (camera, Vec4 *) camera->eye = it
        constexpr uint32_t kSetLookAt = sym::setLookAt;      // (camera, Vec4 *) camera->lookAt = it

        // The eye's sweeps (keepCameraEyeClear): points in world space, the
        // skater's own object (and a vehicle skitched) skipped.
        constexpr uint32_t kEyeSweep = uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed;

        inline ee::Global<ee::Float, sym::g_orientationEase> g_orientationEase;  // 0.05 of the way a frame
        inline ee::Global<ee::Float, sym::g_minHeadToTarget> g_minHeadToTarget;  // 0.1
        inline ee::Global<ee::Float, sym::g_hitPullBack> g_hitPullBack;      // -0.01: back from a hit toward the head
        inline ee::Global<ee::Float, sym::g_parallel2> g_parallel;         // 1e-5
        inline ee::Global<ee::Float, sym::g_qpipeStill> g_qpipeStill;       // 1e-5
        inline ee::Global<ee::Float, sym::g_qpipeStillRate> g_qpipeStillRate;   // 0.2
        inline ee::Global<ee::Float, sym::g_fallingRate> g_fallingRate;      // 0.07
        inline ee::Global<ee::Float, sym::g_qpipeRate> g_qpipeRate;        // 0.04 (over the time)
        inline ee::Global<ee::Float, sym::g_spineStill> g_spineStill;       // 1e-5
        inline ee::Global<ee::Float, sym::g_spineStillRate> g_spineStillRate;   // 0.05
        inline ee::Global<ee::Float, sym::g_turnRate> g_turnRate;         // 0.07
        inline ee::Global<ee::Float, sym::g_downTurnRate> g_downTurnRate;     // 0.07
        inline ee::Global<ee::Float, sym::g_distanceEase> g_distanceEase;     // 0.04
        inline ee::Global<ee::Float, sym::g_sameEye> g_sameEye;          // 1e-5: the eye on the look-at point
        inline ee::Global<ee::Float, sym::g_floor> g_floor;            // 0.9: |normal.up| above it, a floor or ceiling
        inline ee::Global<ee::Float, sym::g_pastEye> g_pastEye;          // 0.3: the sweep goes this far past the eye
        inline ee::Global<ee::Float, sym::g_hitPullBack2> g_hitPullBack2;     // -0.01
        inline ee::Global<ee::Float, sym::g_square> g_square;           // 0.98
        inline ee::Global<ee::Float, sym::g_probeSide> g_probeSide;        // 1.33
        inline ee::Global<ee::Float, sym::g_probeUp> g_probeUp;          // 1.2

        // The engine's up axis (0, -1, 0): y grows downward.
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;

        // The first face a query hit.
        const zge::FaceHit &firstHit(const zge::QueryResults &results) { return *results.nodes[0]->faces; }

        // A camera object's scene, which is its camera.
        ee::Ptr<zge::Camera> cameraOf(ee::Ptr<zge::SceneObject> object) { return zge::sceneOf(object).cast<zge::Camera>(); }

    }

    // updateCameraLookAt: where the camera looks. The camera's orientation eases
    // toward the skater's drawn one (or snaps); the point is the skater's
    // drawn position lowered by the state's character offset along that
    // orientation's up, moved by the eased sideways offset; out of a grind,
    // a point a wall hides from the skater's head is pulled in to the wall.
    void updateCameraLookAt(ee::Ptr<FollowCamera> camera)
    {
        ee::Local<Vec4> target;
        ee::Local<Vec4> lowered;
        ee::Local<Vec4> side;
        ee::Local<Vec4> step;
        ee::Local<zge::Matrix> view;
        ee::Local<zge::Matrix> orientation;
        ee::Local<zge::Quaternion> viewRotation;
        ee::Local<zge::QuerySegment> sight; // from the skater's head to the point
        ee::Local<zge::QueryResults> hits;
        const ee::Ptr<Player> player = camera->player;
        const PlayerAxes &axes = player->axes;
        // (The original copies the skater's position to `target` first; it is
        // overwritten below before any read.)

        if (camera->snap)
            zge::quaternionCopy(&camera->orientation, &player->drawnOrientation);
        else
            zge::quaternionSlerp(&camera->orientation, &player->drawnOrientation, &camera->orientation, *g_orientationEase);

        ee::Float charOffset;
        switch (player->state)
        {
        case State::HorizontalPole: charOffset = cameraTuning(CameraTuning::HPoleCharOffset); break;
        case State::Grind:
        case State::HandGrind:
        case State::DoubleGrind: charOffset = cameraTuning(CameraTuning::GrindCharOffset); break;
        case State::Skitch: charOffset = cameraTuning(CameraTuning::CharOffsetSkitch); break;
        default: charOffset = cameraTuning(CameraTuning::CharOffset); break;
        }
        zge::quaternionToMatrix(orientation, &camera->orientation);
        *lowered = orientation->row(1) * -charOffset;
        *target = player->drawnPosition + *lowered;

        // The sideways offset eases toward the skater's forward axis while on
        // the ground or in a manual, else back to nothing.
        const State state = player->state;
        const bool rolling = state == State::Ground || state == State::Stand || state == State::Manual;
        const ee::Float horizOffset = rolling ? cameraTuning(CameraTuning::HorizOffset) : ee::Float(0.0f);
        Vec4 &sideOffset = camera->sideOffset;
        *side = sideOffset * (ee::Float(1.0f) - cameraTuning(CameraTuning::HorizOffsetDampen));
        if (ee::Float(0.0f) < horizOffset)
        {
            *step = axes.forward * cameraTuning(CameraTuning::HorizOffsetDampen);
            *side = *step + *side;
        }
        // Kept square to the view.
        zge::quaternionCopy(viewRotation, zge::objectOrientation(camera->node));
        zge::quaternionToMatrix(view, viewRotation);
        aggressive::skater::alongPlane(&sideOffset, &view->row(2), side);
        *target = *target + sideOffset;

        const bool grinding = state == State::Grind || state == State::HandGrind || state == State::DoubleGrind;
        if (!grinding)
        {
            // A wall between the skater's head and the point: the point
            // comes to the wall, a little toward the head.
            sight->end = *target;
            *step = axes.up * charOffset;
            sight->start = player->position + *step;
            *step = sight->start - sight->end;
            if (*g_minHeadToTarget < zge::vec3Length(&step->xyz()) &&
                zge::collisionQuery(static_cast<uint32_t>(zge::CollisionKind::Segment), sight, zge::kQueryWorldPoints, 0, 0, 0, hits, 0))
            {
                *step = sight->end - sight->start;
                *step = *step * *g_hitPullBack;
                *target = firstHit(*hits).point + *step;
            }
        }
        setLookAt(camera, target);
    }
    READABLE_FUNCTION(updateCameraLookAt, Verified);

    ee::Float easeStep(ee::Ptr<FollowCamera> camera, ee::Float seconds, ee::Float factor);

    // turnCameraView: turns the view toward `direction` (unit; its up from the
    // skater, or the world's up in a spine transfer when they are not
    // parallel) at the state's rate, and moves the eye behind the look-at
    // point at the camera's distance, eased by Camera / QPipe Dampen. A
    // snap sets both at once. Returns the node position call's value.
    ee::Ptr<zge::SceneObject> turnCameraView(ee::Ptr<FollowCamera> camera, ee::Ptr<const Vec4> direction, ee::Float seconds, ee::Float distance)
    {
        ee::Local<Vec4> offset;
        ee::Local<Vec4> up;
        ee::Local<Vec4> forward;
        ee::Local<Vec4> right;
        ee::Local<Vec4> eye;
        ee::Local<Vec4> back;
        ee::Local<zge::Matrix> wanted;
        ee::Local<zge::Matrix> current;
        ee::Local<zge::Quaternion> eased;
        ee::Local<zge::Quaternion> wantedRotation;
        ee::Local<zge::Quaternion> rotation;
        ee::Local<ee::Ptr<zge::Quaternion>> rotationKept; // the original keeps a pointer to `rotation` here
        *rotationKept = rotation;

        zge::quaternionCopy(rotation, zge::objectOrientation(camera->node));
        zge::quaternionToMatrix(current, rotation);
        for (int row = 0; row < 3; ++row)
            current->row(row) = zge::unit(current->row(row));
        zge::matrixIdentity(wanted);

        // The wanted axes: forward, up square to it, right.
        *forward = zge::unit(*direction);
        const ee::Ptr<Player> player = camera->player;
        if (player->state == State::AirSpine)
        {
            // In a spine transfer up is the world's, unless the view is
            // along it.
            const bool parallel = ee::abs(zge::dot(*g_worldUp, *forward)) < *g_parallel;
            aggressive::skater::alongPlane(up, forward, parallel ? &player->axes.up : &*g_worldUp);
            *up = zge::unit(*up);
            zge::vec3Negate(&up->xyz(), &up->xyz());
        }
        else
        {
            aggressive::skater::alongPlane(up, forward, &player->axes.up);
            zge::vec3Negate(&up->xyz(), &up->xyz());
        }
        *up = zge::unit(*up);
        *right = zge::cross(*up, *forward);
        *right = zge::unit(*right);
        *up = zge::unit(*up);
        *forward = zge::unit(*forward);
        wanted->row(2) = *forward;
        wanted->row(1) = *up;
        wanted->row(0) = *right;
        zge::matrixClearTranslation(wanted);
        // (The original takes each new row's dot product with the old one and
        // does not use them.)
        for (int row = 0; row < 3; ++row)
            (void)zge::dot(wanted->row(row), current->row(row));
        zge::matrixToQuaternion(wantedRotation, wanted);

        // The turn's rate a frame (at 60 a second) by the state. In a quarter
        // pipe or a spine transfer the turn is spread over the time the jump
        // is expected to last.
        ee::Float rate;
        switch (player->state)
        {
        case State::AirQuarterPipe:
        {
            const ee::Float airTime = player->expectedAirTime;
            if (ee::abs(airTime) < *g_qpipeStill)
                rate = *g_qpipeStillRate;
            else if (airTime < ee::Float(0.0f))
                rate = *g_fallingRate;
            else
                rate = *g_qpipeRate / airTime;
            break;
        }
        case State::AirSpine:
        {
            const ee::Float airTime = player->expectedAirTime;
            if (ee::abs(airTime) < *g_spineStill)
                rate = *g_spineStillRate;
            else if (airTime < ee::Float(0.0f))
                rate = *g_turnRate;
            else
                rate = cameraTuning(CameraTuning::SpineDampen) / airTime;
            break;
        }
        case State::Skitch:
        case State::Grind:
        case State::HandGrind:
        case State::WallTap:
            rate = cameraTuning(CameraTuning::GrindSlerp);
            break;
        default:
            rate = *g_turnRate;
            break;
        }
        if (player->flags & skater::kDown)
            rate = *g_downTurnRate;

        if (camera->snap)
            zge::setObjectOrientation(camera->node, wantedRotation);
        else
        {
            zge::quaternionSlerp(eased, rotation, wantedRotation, seconds * ee::Float(60.0f) * rate);
            zge::setObjectOrientation(camera->node, eased);
            zge::quaternionToMatrix(wanted, eased);
        }

        // The eye: back from the look-at point along the view at the
        // camera's distance, which grows toward `distance` slowly and
        // shrinks to it at once.
        *eye = camera->lookAt;
        const ee::Float now = camera->distance;
        if (now < distance)
            camera->distance = now + (distance - now) * *g_distanceEase;
        else
            camera->distance = distance;
        *back = wanted->row(2) * -camera->distance;
        *eye = *back + *eye;
        *offset = *eye - camera->eye;
        if (!camera->snap)
        {
            const ee::Float factor = easeStep(camera, seconds, cameraTuning(CameraTuning::QPipeDampen));
            *offset = *offset * factor;
            *eye = *offset + camera->eye;
        }
        else
            camera->snap = 0;
        setEye(camera, eye);
        return zge::setObjectPosition(camera->node, &camera->eye);
    }
    READABLE_FUNCTION(turnCameraView, Verified);

    // keepCameraEyeClear: keeps the wanted eye out of the level. From the look-at
    // point a segment is swept to just past the eye; a wall in the way
    // (not a floor, unless the skater is fast or skitching, and not with
    // the eye already within 2 of the point) swings the eye about the
    // skater's right or up axis by 20 degrees a turn, away from the wall's
    // side, up to 8 times, else puts it at the wall. Then five short sweeps
    // from the eye (back, and to the four corners of a box about the view)
    // push it off walls close by. `length` gets the eye's distance from the
    // look-at point; returns `length`. Leaves VU0 as the original.
    ee::Ptr<ee::Float> keepCameraEyeClear(ee::Ptr<FollowCamera> camera, ee::Ptr<Vec4> wantedEye, ee::Ptr<ee::Float> length)
    {
        ee::Local<Vec4> hit;
        ee::Local<Vec4> normal;
        ee::Local<uint32_t[4]> skipped; // the objects the sweeps skip, a 0 ending the list
        ee::Local<zge::QuerySegment> sweep;
        ee::Local<zge::QueryResults> results;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> toEye;
        ee::Local<Vec4> fromEye;
        ee::Local<Vec4> up;
        ee::Local<Vec4> side;
        Vec4 &eye = *wantedEye;
        const Vec4 &lookAt = camera->lookAt;
        const ee::Ptr<Player> player = camera->player;
        const auto sweepHits = [&] {
            return zge::collisionQuery(static_cast<uint32_t>(zge::CollisionKind::Segment), sweep, kEyeSweep, 0, 0, skipped, results, 0);
        };

        skipped[0] = ee::Address(zge::sceneObject(zge::sceneOf(player->node))); // the skip list's word
        if (player->state == State::Skitch)
        {
            skipped[2] = 0;
            skipped[1] = ee::Address(player->grindRail); // the vehicle skitched: the skip list's word
        }
        else
            skipped[1] = 0;
        const ee::Float same = *g_sameEye;
        bool atLookAt = ee::abs(eye.x - lookAt.x) < same;
        if (atLookAt)
        {
            atLookAt = ee::abs(eye.y - lookAt.y) < same;
            if (atLookAt)
                atLookAt = ee::abs(eye.z - lookAt.z) < same;
        }
        if (!atLookAt)
        {
            *toEye = eye - lookAt;
            *direction = zge::unit(*toEye);

            // Swing the eye round walls between it and the look-at point.
            ee::Float lastSide = 2.0f;         // 2: no wall's side yet
            const ee::Float noSide = 2.0f;
            const ee::Float minDistance = 2.0f;
            ee::Float turns = 0.0f;
            ee::Float degrees = 0.0f;          // 1 once an axis is chosen
            const Vec4 *axis = nullptr;
            uint32_t pass = 0;
            bool done = false;
            for (;;)
            {
                pass = (pass + 1) & 0xFFu;
                *toEye = *direction * *g_pastEye;
                sweep->end = *toEye + eye;
                sweep->start = lookAt;
                *scratch = lookAt - eye;
                if (zge::vec3Length(&scratch->xyz()) < minDistance)
                    done = true;
                if (!sweepHits())
                    done = true;
                else
                {
                    *hit = firstHit(*results).point;
                    *scratch = *direction * *g_hitPullBack2;
                    *hit = *hit + *scratch;
                    aggressive::skater::hitNormal(results, normal);
                    *toEye = *hit - sweep->end;
                    *scratch = lookAt - *hit;
                    const bool close = zge::vec3Length(&scratch->xyz()) < minDistance;
                    const auto isFloor = [&] { return *g_floor < ee::abs(zge::dot(*normal, *g_worldUp)); };
                    if (!close && !(ee::Float(1.0f) < player->speed) && player->state != State::Skitch && !isFloor())
                    {
                        // A wall in the way: the eye goes to it.
                        eye = *hit;
                        done = true;
                    }
                    else
                    {
                        *fromEye = lookAt - eye;
                        if (lastSide == noSide)
                        {
                            // The first wall picks the axis: the skater's
                            // right for a floor, a quarter pipe's lip or face
                            // (physics kinds 1 and 2) or a skitch, else up.
                            const zge::HitNode &node = *results->nodes[0];
                            const int32_t kind = aggressive::skater::surfacePhysics(node.node, node.faces->face);
                            bool right = true;
                            if (!isFloor())
                            {
                                if (kind == 1 || kind == 2)
                                    ;
                                else
                                    right = player->state == State::Skitch;
                            }
                            axis = right ? &player->axes.right : &player->axes.up;
                            degrees = 1.0f;
                        }
                        *scratch = zge::unit(*fromEye);
                        const ee::Float square = *g_square;
                        const auto across = [&](const Vec4 &v) { return ee::abs(zge::dot(v, *axis)) < square; };
                        if (player->state == State::Stand || !across(*scratch) || !across(*normal) || pass >= 9)
                        {
                            eye = *hit;
                            done = true;
                        }
                        else
                        {
                            // Which side of the wall the eye is on; a turn more
                            // each time it changes (and the first time).
                            *side = zge::cross(*axis, *fromEye);
                            *side = zge::unit(*side);
                            const ee::Float wallSide = zge::dot(*normal, *side);
                            bool turn = ee::Float(0.0f) < wallSide && lastSide < ee::Float(0.0f);
                            if (!turn)
                            {
                                turn = wallSide < ee::Float(0.0f) && ee::Float(0.0f) < lastSide;
                                if (!turn)
                                    turn = lastSide == noSide;
                            }
                            if (turn)
                            {
                                turns = turns + ee::Float(1.0f);
                                lastSide = wallSide;
                            }
                            const ee::Float step = lastSide < ee::Float(0.0f) ? degrees * ee::Float(20.0f)
                                                                              : degrees * ee::Float(-20.0f);
                            aggressive::skater::rotateAbout(fromEye, axis, turns * step);
                            eye = lookAt - *fromEye;
                            *toEye = eye - lookAt;
                            *direction = zge::unit(*toEye);
                        }
                    }
                }
                if (done)
                    break;
            }

            // Five short sweeps from the eye, each 1.5 further back: along
            // the view's up, and to the four corners of a box about the view
            // (up 1.2, 2.5 in split screen; side 1.33).
            *side = zge::cross(player ? player->axes.up : *g_worldUp, *direction);
            *up = zge::cross(*direction, *side);
            *side = *side * *g_probeSide;
            *fromEye = *direction * ee::Float(1.5f);
            const ee::Float probeUp = *g_gameType == GameType::SplitScreen ? ee::Float(2.5f) : *g_probeUp;
            *up = *up * probeUp;
            for (uint32_t probe = 0; probe < 5; ++probe)
            {
                Vec4 &end = sweep->end;
                sweep->start = eye;
                switch (probe)
                {
                case 0: end = eye - *up; break;
                case 1: end = eye - *up, end = end - *side; break;
                case 2: end = eye + *side, end = end - *up; break;
                case 3: end = eye + *up, end = end + *side; break;
                case 4: end = eye - *side, end = end + *up; break;
                }
                end = end - *fromEye;
                if (sweepHits())
                {
                    *hit = firstHit(*results).point;
                    aggressive::skater::hitNormal(results, normal);
                    // Out of the wall from whichever end is in front of it.
                    *toEye = end - sweep->start;
                    if (ee::Float(0.0f) < zge::dot(*toEye, *normal))
                        *toEye = *hit - sweep->start;
                    else
                        *toEye = *hit - end;
                    aggressive::skater::alongPlane(scratch, direction, toEye);
                    eye = eye + *scratch;
                }
            }
        }
        *toEye = eye - lookAt;
        *length = zge::vec3Length(&toEye->xyz());
        return length;
    }
    READABLE_FUNCTION_VU0(keepCameraEyeClear, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_downVertical> g_downVertical;   // 0.01: | |v.down| - 1 | below it, moving straight up or down
        inline ee::Global<ee::Float, sym::g_vertical> g_vertical;       // 0.01
        inline ee::Global<ee::Float, sym::g_stillStickX> g_stillStickX;    // 1e-5
        inline ee::Global<ee::Float, sym::g_stillStickY> g_stillStickY;    // 1e-5
        inline ee::Global<ee::Float, sym::g_poleDistance> g_poleDistance;   // 1.6 (times Distance)
        inline ee::Global<ee::Float, sym::g_spineSlow> g_spineSlow;      // 1e-5
        inline ee::Global<ee::Float, sym::g_spineLean> g_spineLean;      // 0.7
        inline ee::Global<ee::Float, sym::g_spineLeanStill> g_spineLeanStill; // 0.7
        inline ee::Global<ee::Float, sym::g_spineDistance> g_spineDistance;  // 1.6 (times Distance)
        inline ee::Global<ee::Float, sym::g_qpipeDistance> g_qpipeDistance;  // 1.6 (times Distance)

        // The right stick's axes (padAxis).
        constexpr int32_t kRightStickX = 2, kRightStickY = 3;

        void noteSpeed(ee::Ptr<Player> player)
        {
            player->previousSpeed = player->speed;
            player->speed = zge::vec3Length(&player->velocity.xyz());
        }

        // The way the skater travels, flat on its up axis, into `direction`:
        // its forward axis when slow (or `useForward`), else its velocity's,
        // unless that is straight along the world's up (within `tolerance`).
        // `scratch` is the caller's work vector. (`tolerance` by reference:
        // the original reads it only when the velocity decides; clang-cl's
        // build read it at the call, a word the recordings do not cover.)
        void travelDirection(ee::Ptr<Player> player, bool useForward, const ee::Float &tolerance, const Vec4 &worldUp,
                             Vec4 &scratch, Vec4 &direction)
        {
            const PlayerAxes &axes = player->axes;
            if (useForward || player->speed < ee::Float(5.0f))
                direction = axes.forward;
            else
            {
                scratch = zge::unit(player->velocity);
                const bool straight = ee::abs(ee::abs(zge::dot(worldUp, scratch)) - ee::Float(1.0f)) < tolerance;
                if (straight)
                    direction = axes.forward;
                else
                    direction = zge::unit(player->velocity);
            }
            aggressive::skater::alongPlane(&scratch, &axes.up, &direction);
            direction = zge::unit(scratch);
        }

        // The stick turns the eye about the look-at point: right stick x
        // about the skater's up axis (Horz Lookaround degrees), y about its
        // right axis (Vert Lookaround); Invert ... Look 1 keeps the stick's
        // sign.
        void lookAround(ee::Ptr<FollowCamera> camera, Vec4 &eye, const Vec4 &lookAt, Vec4 &offset)
        {
            ee::Float x = padAxis(kRightStickX, camera->player->controls.port);
            const bool stillX = ee::abs(x) < *g_stillStickX;
            if (!stillX)
            {
                if (!(cameraTuning(CameraTuning::InvertHorizLook) == ee::Float(1.0f)))
                    x = -x;
                offset = eye - lookAt;
                aggressive::skater::rotateAbout(&offset, &camera->player->axes.up, cameraTuning(CameraTuning::HorzLookaround) * x);
                eye = offset + lookAt;
            }
            ee::Float y = padAxis(kRightStickY, camera->player->controls.port);
            const bool stillY = ee::abs(y) < *g_stillStickY;
            if (!stillY)
            {
                if (!(cameraTuning(CameraTuning::InvertVertLook) == ee::Float(1.0f)))
                    y = -y;
                offset = eye - lookAt;
                aggressive::skater::rotateAbout(&offset, &camera->player->axes.right, cameraTuning(CameraTuning::VertLookaround) * y);
                eye = offset + lookAt;
            }
        }
    }

    // updateFollowCamera: the follow camera's frame (`seconds` the frame's time).
    // The look-at point first (updateCameraLookAt); then the wanted eye by
    // the skater's state: behind and above the look-at point along the
    // way the skater travels (down, rolling, grinding, in the air ...), to
    // the side on a wall ride, below and behind in a quarter pipe or a
    // spine transfer, circling a vertical pole; kept clear of the level
    // (keepCameraEyeClear) and turned to (turnCameraView). Wall Tap leaves
    // the camera as it is.
    void updateFollowCamera(ee::Ptr<FollowCamera> camera, ee::Float seconds)
    {
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> raise;
        ee::Local<Vec4> back;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> eye;
        ee::Local<ee::Float> distance; // keepCameraEyeClear's: the eye's distance from the look-at point
        const Vec4 &lookAt = camera->lookAt;
        if (!camera->player)
            return;
        updateCameraLookAt(camera);
        const ee::Ptr<Player> player = camera->player;
        const PlayerAxes &axes = player->axes;
        // `raise` starts as the world's up, its w as the stack has it.
        raise->x = 0.0f;
        raise->y = -1.0f;
        raise->z = 0.0f;
        // From the eye to the look-at point, and the view turned along it at
        // the distance it ended up.
        const auto turnToLookAt = [&] {
            *direction = lookAt - *eye;
            *direction = zge::unit(*direction);
            turnCameraView(camera, direction, seconds, *distance);
        };

        if (player->flags & skater::kDown)
        {
            // Down: behind and above, then the eye kept clear once more
            // from where it ended up.
            noteSpeed(player);
            travelDirection(player, false, *g_downVertical, *raise, *scratch, *direction);
            *back = *direction * -cameraTuning(CameraTuning::Distance);
            *raise = axes.up * cameraTuning(CameraTuning::Height);
            *eye = *raise + lookAt;
            *eye = *back + *eye;
            keepCameraEyeClear(camera, eye, distance);
            turnToLookAt();
            *eye = camera->eye;
            keepCameraEyeClear(camera, eye, distance);
            return;
        }

        switch (player->state)
        {
        case State::WallTap:
            return;

        case State::VerticalPole:
        case State::VerticalPoleA:
        {
            // Circling the pole: level, toward the skater from the eye
            // (the original's 0.25 tilt up is overwritten), turned by VPole
            // Rotate degrees a second while the pole's time is short.
            *direction = player->drawnPosition - camera->eye;
            aggressive::skater::alongPlane(direction, &*g_worldUp, direction);
            *back = zge::unit(*direction);
            *raise = *g_worldUp * ee::Float(0.25f);
            *back = *back + *raise;
            *back = zge::unit(*direction);
            if (player->stateValue < cameraTuning(CameraTuning::VPoleRotateTime))
            {
                const ee::Float way = player->side == 0 ? -seconds : seconds;
                aggressive::skater::rotateAbout(back, &*g_worldUp, cameraTuning(CameraTuning::VPoleRotate) * way);
            }
            turnCameraView(camera, back, seconds, cameraTuning(CameraTuning::Distance) * *g_poleDistance);
            return;
        }

        case State::WallRide:
        {
            // To the side away from the wall, forward.
            noteSpeed(player);
            if (player->side == 0)
                *direction = axes.right;
            else
                zge::vec3Negate(&direction->xyz(), &axes.right.xyz());
            *direction = *direction + axes.forward;
            *direction = zge::unit(*direction);
            aggressive::skater::alongPlane(scratch, &axes.up, direction);
            *direction = zge::unit(*scratch);
            *back = *direction * -cameraTuning(CameraTuning::Distance);
            *raise = axes.up * cameraTuning(CameraTuning::Height);
            *eye = *raise + lookAt;
            *eye = *back + *eye;
            keepCameraEyeClear(camera, eye, distance);
            turnToLookAt();
            return;
        }

        case State::AirSpine:
        {
            // Below and behind: from straight down (y up the screen, 0 1 0)
            // leaning back along the travel (or the skater's forward axis
            // when still), at Spine Distance times Distance from the skater.
            const Vec4 &drawn = player->drawnPosition;
            back->x = 0.0f;
            back->y = 1.0f;
            back->z = 0.0f;
            const bool still = ee::abs(player->speed) < *g_spineSlow;
            if (!still)
            {
                *scratch = zge::unit(player->velocity);
                *scratch = *scratch * *g_spineLean;
            }
            else
                *scratch = axes.forward * *g_spineLeanStill;
            *back = *back + *scratch;
            *back = zge::unit(*back);
            *scratch = *back * (cameraTuning(CameraTuning::Distance) * *g_spineDistance);
            *eye = drawn - *scratch;
            keepCameraEyeClear(camera, eye, distance);
            *direction = drawn - *eye;
            *back = zge::unit(*direction);
            turnCameraView(camera, back, seconds, *distance);
            return;
        }

        case State::AirQuarterPipe:
        {
            // Below and behind: from straight down leaning out along the
            // skater's up axis (QPipe Offset) and along the lip by the
            // travel over the pipe's height, at QPipe Distance times
            // Distance from the skater; the view turned at Distance.
            const Vec4 &drawn = player->drawnPosition;
            back->x = 0.0f;
            back->y = 1.0f;
            back->z = 0.0f;
            *scratch = axes.up * cameraTuning(CameraTuning::QPipeOffset);
            *back = *back + *scratch;
            const Vec4 &lip = player->railDirection;
            const ee::Float along = zge::dot(lip, player->velocity);
            *scratch = lip * (along / (player->pipeHeight + ee::Float(10.0f)));
            *back = *back + *scratch;
            *back = zge::unit(*back);
            *scratch = *back * (cameraTuning(CameraTuning::Distance) * *g_qpipeDistance);
            *eye = drawn - *scratch;
            keepCameraEyeClear(camera, eye, distance);
            *direction = drawn - *eye;
            *back = zge::unit(*direction);
            turnCameraView(camera, back, seconds, cameraTuning(CameraTuning::Distance));
            camera->quarterPipeMark = 0.25f;
            return;
        }

        default:
            // The table's other states (and those past it, Cess Slide on,
            // straight on).
            break;
        }

        // Following: behind the look-at point along the travel, raised.
        noteSpeed(player);
        travelDirection(player, player->state == State::HorizontalPole, *g_vertical, *raise, *scratch, *direction);
        const State state = player->state;
        const auto behind = [&](ee::Float length) { *back = *direction * -length; };
        const auto above = [&](ee::Float height) { *raise = axes.up * height; };
        switch (state)
        {
        case State::Grind:
        case State::HandGrind:
        case State::DoubleGrind:
            behind(cameraTuning(CameraTuning::GrindDistance));
            above(cameraTuning(CameraTuning::GrindHeight));
            break;
        case State::Skitch:
            behind(cameraTuning(CameraTuning::Distance));
            *raise = zge::kZero;
            break;
        case State::HorizontalPole:
            behind(cameraTuning(CameraTuning::DistanceHPole));
            above(cameraTuning(CameraTuning::Height));
            break;
        case State::Ground:
        case State::Stand:
        case State::Manual:
            above(cameraTuning(CameraTuning::Height));
            behind(cameraTuning(CameraTuning::Distance));
            break;
        default:
            behind(cameraTuning(CameraTuning::Distance));
            above(cameraTuning(CameraTuning::Height));
            break;
        }
        *eye = *raise + lookAt;
        *eye = *back + *eye;
        if (player->state == State::Air)
        {
            // Rising: higher by Falling Speed times the climb, on Falling Base.
            const ee::Float climb = player->velocity.y;
            ee::Float extra;
            if (ee::Float(0.0f) < climb)
                extra = climb * cameraTuning(CameraTuning::FallingSpeed);
            else
                extra = ee::Float(0.0f);
            *raise = axes.up * ((cameraTuning(CameraTuning::FallingBase) + extra) * cameraTuning(CameraTuning::Height));
            *eye = *raise + *eye;
        }
        else if (player->state == State::HorizontalPole)
        {
            *raise = axes.up * (cameraTuning(CameraTuning::Height) * ee::Float(5.0f));
            *eye = *raise + *eye;
        }
        lookAround(camera, *eye, lookAt, *direction);
        if (player->state != State::HorizontalPole)
            keepCameraEyeClear(camera, eye, distance);
        else
            *distance = cameraTuning(CameraTuning::DistanceHPole);
        turnToLookAt();
    }
    READABLE_FUNCTION(updateFollowCamera, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_lookMoved> g_lookMoved; // eye and look-at closer than this on every axis: kept
        inline ee::Global<ee::Float, sym::g_fovToAngle> g_fovToAngle; // the field of view (FollowCamera::fov) to 16-bit angle units
        constexpr uint32_t kCameraFov = sym::cameraFov;        // () -> the field of view (by address: game overrides hook it)
        constexpr uint32_t kSetCameraFov = sym::setCameraFov;     // (camera, degrees) (by address: hooked too)
        constexpr uint32_t kMemset = sym::memset;           // (dst, byte, bytes) (libc)
        constexpr uint32_t kSoundCameraEvent = 51;
        constexpr uint32_t kSetSceneClearColour = sym::setSceneClearColour;  // (node, colour *)
        constexpr uint32_t kSetSceneFogColour = sym::setSceneFogColour;    // (node, colour *)

        constexpr int32_t kPlayPhaseFlyThru = 6;
        inline ee::Global<uint32_t, sym::g_flyThruActive> g_flyThruActive;

        // The levels (GameState::level) split screen has its own field of
        // view and far clip for (Camera / MP ...).
        constexpr int32_t kLevelCivic = 0, kLevelBoardwalk = 1, kLevelMovie = 5, kLevelMuseum = 6;
        // A level's own settings: the scene point "Globals"'s tags. Their
        // group, and the fields by offset.
        constexpr uint32_t kGlobalsFloats = 8, kGlobalsColours = 0x20;
        constexpr uint32_t kGlobalsFogAlpha = 12, kGlobalsFogStart = 0x10, kGlobalsFogEnd = 20, kGlobalsFarClip = 24;
        constexpr uint32_t kGlobalsFogColour = 8, kGlobalsClearColour = 4;

        // The motion blur effect's request (startFullScreenEffect's,
        // setEffectValue's; zge/render/screen_effects.cpp).
        struct MotionBlurRequest
        {
            uint8_t _00[4];
            uint8_t strength;   // +0x04
            uint8_t _05[3];
            uint32_t colour;    // +0x08
            ee::Float scale[2]; // +0x0C
            uint8_t _14[8];
            ee::Ptr<zge::Camera> camera; // +0x1C
        };
        static_assert(sizeof(MotionBlurRequest) == 0x20 && offsetof(MotionBlurRequest, camera) == 0x1C);
        // The blur's colour: white, alpha 0 (red, green, blue, alpha bytes).
        constexpr uint32_t kMotionBlurColour = 0xFFu | 0xFFu << 8 | 0xFFu << 16;
        inline ee::Global<MotionBlurRequest, sym::g_motionBlurDefaults> g_motionBlurDefaults;
        inline ee::Global<char, sym::g_globalsName> g_globalsName;      // "Globals"
        inline ee::Global<char, sym::g_motionBlurName> g_motionBlurName;   // "FSMotionBlur"
        // The clear and fog colours a level without "Globals" has.
        inline ee::Global<uint32_t, sym::g_defaultFogColour> g_defaultFogColour;

        // A camera's screen rectangle (zge Camera::screen).
        struct ScreenRectangle
        {
            ee::Float x, y, width, height;
        };

        // What cameraMoved hands the sound engine: the scene objects sounds
        // are heard from. The game sets only the first: the other three are
        // what the stack holds, copied as they are.
        struct SoundListeners
        {
            int32_t count;
            ee::Ptr<zge::SceneObject> objects[4];
        };
        static_assert(sizeof(SoundListeners) == 0x14);

        // The eye and the look-at point closer than this on every axis: no
        // direction to look in (.lit4 per function).
        inline ee::Global<ee::Float, sym::g_behindEyeOnLookAt> g_behindEyeOnLookAt;   // 1e-5
        inline ee::Global<ee::Float, sym::g_trackingEyeOnLookAt> g_trackingEyeOnLookAt; // 1e-5
        // Seconds into a camera animation the camera times itself (one the
        // game's animators do not run).
        inline ee::Global<ee::Float, sym::g_cameraAnimationTime> g_cameraAnimationTime;

        constexpr uint32_t kCameraBone = 1; // the camera animation's channel

        // Whether the camera's eye and look-at point are one place, within
        // `near` on every axis.
        bool eyeOnLookAt(ee::Ptr<FollowCamera> camera, ee::Float near)
        {
            bool same = ee::abs(camera->eye.x - camera->lookAt.x) < near;
            if (same)
            {
                same = ee::abs(camera->eye.y - camera->lookAt.y) < near;
                if (same)
                    same = ee::abs(camera->eye.z - camera->lookAt.z) < near;
            }
            return same;
        }

        void lookFromEyeToLookAt(ee::Ptr<FollowCamera> camera)
        {
            zge::lookFromAt(camera->node, &camera->eye, &camera->lookAt, 0);
        }

        // The R1 camera's and the chase camera's look-at point: the skater's
        // drawn position, Char Offset up.
        Vec4 skaterLookAtPoint(ee::Ptr<Player> player)
        {
            Vec4 point = player->drawnPosition;
            point.y = point.y - cameraTuning(CameraTuning::CharOffset);
            return point;
        }

        // How much of the way a chase camera's point goes a frame, by the
        // skater's speed: at least 0.1, `perSpeed` of the speed, at most 0.5.
        ee::Float chaseEase(ee::Float speed, ee::Float perSpeed)
        {
            if (speed < ee::Float(10.0f))
                return ee::Float(0.1f);
            ee::Float ease;
            if (speed < ee::Float(50.0f))
                ease = speed * perSpeed;
            else
                ease = ee::Float(0.5f);
            return ease;
        }
    }

    // easeStep: the eye's easing step this frame: the factor scaled to
    // 60 frames a second, the mean with the last frame's. Returns `factor`.
    ee::Float easeStep(ee::Ptr<FollowCamera> camera, ee::Float seconds, ee::Float factor)
    {
        const ee::Float last = camera->easeStep;
        ee::Float step = factor * (seconds * ee::Float(60.0f));
        if (!(last == ee::Float(0.0f)))
            step = (step + last) / ee::Float(2.0f);
        camera->easeStep = step;
        return factor;
    }
    READABLE_FUNCTION(easeStep, Verified);

    ee::Float cameraFov();

    void setCameraFov(ee::Ptr<FollowCamera> camera, ee::Float degrees);

    // updateChaseCamera: a camera chasing the skater along `direction`, no live
    // code calls it: the look-at point and the eye behind it ease toward
    // the wanted ones by the skater's speed, the eye kept clear of the
    // level, and while FollowCamera::eyeHold runs down the eye stays.
    void updateChaseCamera(ee::Ptr<FollowCamera> camera, ee::Ptr<const Vec4> direction, ee::Float seconds)
    {
        ee::Local<Vec4> wanted;
        ee::Local<Vec4> along;
        ee::Local<Vec4> up;
        ee::Local<Vec4> flatDirection;
        ee::Local<ee::Float> clearDistance;
        const Vec4 &lookAt = camera->lookAt;
        const Vec4 &eye = camera->eye;

        // `direction` without its part along the world's up (y grows down).
        up->x = ee::Float(0.0f);
        up->y = ee::Float(-1.0f);
        up->z = ee::Float(0.0f);
        *along = *up * zge::dot(*up, *direction);
        *flatDirection = *direction - *along;
        *flatDirection = zge::unit(*flatDirection);

        *wanted = skaterLookAtPoint(camera->player);
        const ee::Float lookAtEase = chaseEase(camera->player->speed, ee::Float(0.01f));
        *along = *wanted - lookAt;
        *along = *along * lookAtEase;
        *wanted = *along + lookAt;
        setLookAt(camera, wanted);

        // The eye behind the skater, Height above the look-at point's level.
        const Vec4 &drawnPosition = camera->player->drawnPosition;
        *wanted = drawnPosition;
        *wanted = *flatDirection * -cameraTuning(CameraTuning::Distance);
        *wanted = *wanted + drawnPosition;
        const ee::Float eyeY = drawnPosition.y - cameraTuning(CameraTuning::CharOffset);
        wanted->y = eyeY;
        wanted->y = eyeY - cameraTuning(CameraTuning::Height);
        keepCameraEyeClear(camera, wanted, clearDistance);

        ee::Float eyeEase = ee::Float(0.0f);
        const ee::Float hold = camera->eyeHold;
        if (eyeEase < hold)
            camera->eyeHold = hold - seconds;
        else
            eyeEase = chaseEase(camera->player->speed, ee::Float(0.008333334f));
        *along = *wanted - eye;
        *along = *along * eyeEase;
        *wanted = *along + eye;
        setEye(camera, wanted);
        const ee::Float fov = cameraFov();
        setCameraFov(camera, fov);
        if (!eyeOnLookAt(camera, ee::Float(1e-05f)))
            lookFromEyeToLookAt(camera);
    }
    READABLE_FUNCTION_WAITS(updateChaseCamera, Verified);

    // updateBehindCamera: CameraMode::Behind: the eye straight behind the skater
    // by Camera / Distance, Height above the look-at point. On a pole the
    // camera stays as it is. (The original normalizes the level position
    // first and overwrites it.)
    void updateBehindCamera(ee::Ptr<FollowCamera> camera)
    {
        ee::Local<Vec4> point;
        const ee::Ptr<Player> player = camera->player;
        if (!player)
            return;
        const State state = player->state;
        if (state != State::VerticalPole && state != State::HorizontalPole && state != State::VerticalPoleA)
        {
            *point = skaterLookAtPoint(player);
            setLookAt(camera, point);

            const Player &skater = *camera->player;
            *point = skater.drawnPosition;
            point->y = ee::Float(0.0f);
            *point = zge::unit(*point);
            *point = skater.axes.forward * -cameraTuning(CameraTuning::Distance);
            *point = *point + skater.drawnPosition;
            point->y = skater.drawnPosition.y - cameraTuning(CameraTuning::CharOffset);
            const ee::Float fov = cameraFov();
            setCameraFov(camera, fov);
            point->y = point->y - cameraTuning(CameraTuning::Height);
            setEye(camera, point);
        }
        if (eyeOnLookAt(camera, *g_behindEyeOnLookAt))
            return;
        lookFromEyeToLookAt(camera);
    }
    READABLE_FUNCTION_WAITS(updateBehindCamera, Verified);

    // updateTrackingCamera: CameraMode::Tracking: the eye stays where it was, the
    // view turns to the skater.
    void updateTrackingCamera(ee::Ptr<FollowCamera> camera)
    {
        ee::Local<Vec4> lookAt;
        if (camera->player)
        {
            *lookAt = skaterLookAtPoint(camera->player);
            setLookAt(camera, lookAt);
            if (eyeOnLookAt(camera, *g_trackingEyeOnLookAt))
                return;
            lookFromEyeToLookAt(camera);
        }
    }
    READABLE_FUNCTION_WAITS(updateTrackingCamera, Verified);

    // updateAnimatedCamera: the camera modes Animation and AnimationAimed: a camera
    // animation's bone channel places the camera (and, in Animation, turns
    // it). An animation the game's animators do not run is timed here. At
    // its end the field of view goes back to Camera / FOV and the camera
    // to following the skater.
    void updateAnimatedCamera(ee::Ptr<FollowCamera> camera, ee::Float seconds)
    {
        ee::Local<int32_t> done;
        const auto backToFollowing = [&] {
            setFollowCameraMode(camera, static_cast<uint32_t>(CameraMode::Follow));
        };

        *done = 1;
        const int32_t animated = zge::animationOver(camera->node, done);
        bool finished = *done != 0;
        bool checkEnd = true;
        if (animated == 0)
        {
            // Timed here.
            const ee::Float time = *g_cameraAnimationTime + seconds;
            *g_cameraAnimationTime = time;
            zge::updateAnimation(camera->node, 0, time);
            *done = zge::animationDone(camera->node, *g_cameraAnimationTime);
            finished = *done != 0;
            checkEnd = finished;
            if (finished)
                *g_cameraAnimationTime = ee::Float(0.0f);
        }
        if (checkEnd && finished)
        {
            setCameraFov(camera, cameraTuning(CameraTuning::Fov));
            backToFollowing();
            return;
        }
        const ee::Ptr<zge::Animator> animator = zge::sceneObjectAnimator(camera->node);
        ee::Ptr<zge::AnimationChannel> channel;
        if (animator)
            channel = zge::findBoneChannel(animator, kCameraBone);
        if (!animator || !channel)
        {
            backToFollowing();
            return;
        }
        const ee::Ptr<zge::Quaternion> rotation = zge::boneRotation(channel);
        if (rotation)
            zge::setObjectOrientation(camera->node, rotation);
        const ee::Ptr<Vec4> translation = zge::boneTranslation(channel);
        if (translation)
        {
            zge::setObjectPosition(camera->node, translation);
            setEye(camera, translation);
        }
        if (camera->mode == static_cast<uint8_t>(CameraMode::AnimationAimed))
        {
            updateCameraLookAt(camera);
            lookFromEyeToLookAt(camera);
        }
    }
    READABLE_FUNCTION_WAITS(updateAnimatedCamera, Verified);

    // updateR1Camera: CameraMode::R1: the eye to the skater's side (R1
    // Distance, R1 Height), kept clear of the level. When something is in
    // the way the other side is tried and the one with more room taken;
    // then the view turns to the look-at point from the last eye tried. (So
    // if the first side wins, the view is set up from the second: kept.
    // The eye chosen is kept in `chosenEye` and never used.)
    void updateR1Camera(ee::Ptr<FollowCamera> camera, ee::Float seconds)
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> toLookAt;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> chosenEye;
        ee::Local<Vec4> firstEye;
        ee::Local<ee::Float> firstClear;
        ee::Local<ee::Float> secondClear;
        if (!camera->player)
            return;
        const Vec4 &lookAt = camera->lookAt;

        // The eye on one side, and toLookAt from it to R1 Char Offset up
        // from the look-at point. The camera's own side when `ownSide`.
        const auto eyeOnSide = [&](bool ownSide) {
            const PlayerAxes &axes = camera->player->axes;
            *eye = camera->lookAt;
            const bool onRight = camera->side == 1;
            const ee::Float distance = cameraTuning(CameraTuning::R1Distance);
            const ee::Float across = onRight == ownSide ? distance : -distance;
            *toLookAt = axes.right * across;
            *eye = *eye + *toLookAt;
            *toLookAt = axes.up * cameraTuning(CameraTuning::R1Height);
            *eye = *eye + *toLookAt;
            *toLookAt = axes.up * cameraTuning(CameraTuning::R1CharOffset);
            *toLookAt = lookAt + *toLookAt;
            *toLookAt = *toLookAt - *eye;
        };

        eyeOnSide(true);
        const ee::Float wanted = zge::vec3Length(&toLookAt->xyz());
        keepCameraEyeClear(camera, eye, firstClear);

        ee::Float distance;
        if (*firstClear < wanted)
        {
            *firstEye = *eye;
            eyeOnSide(false);
            const ee::Float wantedThere = zge::vec3Length(&toLookAt->xyz());
            keepCameraEyeClear(camera, eye, secondClear);
            const ee::Float there = *secondClear;
            const ee::Float here = *firstClear;
            if (there < wantedThere && !(here < there))
            {
                *chosenEye = *firstEye;
                distance = here;
            }
            else
            {
                *chosenEye = *eye;
                distance = there;
                camera->side = static_cast<int8_t>(-camera->side);
            }
        }
        else
        {
            *chosenEye = *eye;
            distance = *firstClear;
        }
        *direction = lookAt - *eye;
        *direction = zge::unit(*direction);
        turnCameraView(camera, direction, seconds, distance);
        setCameraFov(camera, cameraTuning(CameraTuning::R1Fov));
    }
    READABLE_FUNCTION_WAITS(updateR1Camera, Verified);

    // initFollowCamera: a camera's set-up: its scene object, the eye at the
    // origin looking along x, Camera / Distance, the animator its animations
    // run on, and the level's settings. Returns 1.
    uint32_t initFollowCamera(ee::Ptr<FollowCamera> camera)
    {
        camera->node = zge::newSceneCamera(1);
        camera->eye = zge::kZero;
        camera->lookAt = zge::kZero;
        camera->lookAt.x = camera->lookAt.x + ee::Float(1.0f);
        camera->fov = cameraFov();
        camera->snap = 1;
        camera->player = ee::Ptr<Player>(0u);
        camera->side = 1;
        camera->distance = cameraTuning(CameraTuning::Distance);
        camera->easeStep = ee::Float(0.0f);
        camera->eyeHold = ee::Float(0.0f);
        camera->sideOffset = zge::kZero;
        camera->orientation.x = ee::Float(0.0f);
        camera->orientation.y = ee::Float(0.0f);
        camera->orientation.z = ee::Float(0.0f);
        camera->orientation.w = ee::Float(0.0f);
        zge::createObjectAnimator(camera->node);
        applyLevelCameraSettings(camera);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initFollowCamera, Verified);

    // setupFollowCameras: the game type's cameras: one, or in split screen
    // two, side by side, each on half the first one's viewport. Returns 1.
    uint32_t setupFollowCameras(uint32_t gameType)
    {
        ee::Local<ScreenRectangle> viewport;
        FollowCamera &first = (*g_followCameras)[0];
        FollowCamera &second = (*g_followCameras)[1];
        if (static_cast<GameType>(gameType & 0xFFu) != GameType::SplitScreen)
        {
            initFollowCamera(&first);
            return 1;
        }
        initFollowCamera(&first);
        initFollowCamera(&second);
        zge::sceneViewport(cameraOf(first.node), viewport);
        const ee::Ptr<zge::SceneObject> firstNode = first.node;
        viewport->width = viewport->width * ee::Float(0.5f);
        zge::swapCameraViewport(cameraOf(firstNode), viewport);
        const ee::Ptr<zge::SceneObject> secondNode = second.node;
        viewport->x = viewport->x + viewport->width;
        zge::swapCameraViewport(cameraOf(secondNode), viewport);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupFollowCameras, Verified);

    // releaseFollowCameras: both cameras let go: the motion blur stopped, the
    // animator and the scene object released.
    void releaseFollowCameras()
    {
        const auto release = [&](FollowCamera &camera) {
            if (camera.motionBlur != 0)
            {
                zge::stopFullScreenEffect(camera.motionBlur);
                camera.motionBlur = ee::Ptr<zge::RunningEffect>();
            }
            zge::freeObjectAnimator(camera.node);
            zge::releaseSceneCamera(camera.node);
            camera.node = ee::Ptr<zge::SceneObject>();
        };
        FollowCamera &first = (*g_followCameras)[0];
        FollowCamera &second = (*g_followCameras)[1];
        if (first.node != 0)
            release(first);
        if (second.node != 0)
            release(second);
    }
    READABLE_FUNCTION_WAITS(releaseFollowCameras, Verified);

    // followCameras: the follow cameras' array.
    ee::Ptr<FollowCamera> followCameras()
    {
        return &(*g_followCameras)[0];
    }
    READABLE_FUNCTION(followCameras, Verified);

    // setFollowCameraTarget: the skater a camera follows (0: none), and it cuts there.
    void setFollowCameraTarget(ee::Ptr<FollowCamera> camera, ee::Ptr<skater::Player> target)
    {
        camera->player = target;
        camera->snap = 1;
    }
    READABLE_FUNCTION(setFollowCameraTarget, Verified);

    // setEye: the camera's eye.
    void setEye(ee::Ptr<FollowCamera> camera, ee::Ptr<const zge::Vec4> eye)
    {
        camera->eye = *eye;
    }
    READABLE_FUNCTION(setEye, Verified);

    // setLookAt: the camera's look-at point.
    void setLookAt(ee::Ptr<FollowCamera> camera, ee::Ptr<const zge::Vec4> lookAt)
    {
        camera->lookAt = *lookAt;
    }
    READABLE_FUNCTION(setLookAt, Verified);

    // setCameraBlur: the running motion blur's strength and scales (the
    // grinds change them; 0 record, nothing). (The request starts from the
    // defaults and Camera / Normal Blur, then gets these.)
    void setCameraBlur(ee::Ptr<FollowCamera> camera, uint32_t strength, ee::Float scaleX, ee::Float scaleY)
    {
        ee::Local<MotionBlurRequest> request;
        *request = *g_motionBlurDefaults;
        request->strength = static_cast<uint8_t>(ee::toInt(cameraTuning(CameraTuning::NormalBlur)));
        if (camera->motionBlur != 0)
        {
            request->strength = static_cast<uint8_t>(strength);
            request->scale[0] = scaleX;
            request->scale[1] = scaleY;
            request->colour = kMotionBlurColour;
            request->camera = cameraOf(camera->node);
            zge::setEffectValue(camera->motionBlur, request);
        }
    }
    READABLE_FUNCTION_WAITS(setCameraBlur, Verified);

    // setFollowCameraMode: a camera's mode; returns the old one. Leaving a camera
    // animation stops it and leaves the fly-through (to play from the
    // level's opening one, else back to the pause menu, the NPCs shown
    // again). The new mode: an animation enters the fly-through game mode, R1
    // takes its field of view, the rest the game's.
    uint32_t setFollowCameraMode(ee::Ptr<FollowCamera> camera, uint32_t newMode)
    {
        ee::Local<int32_t> done;
        const uint8_t mode = static_cast<uint8_t>(newMode);
        const uint8_t previous = camera->mode;
        *done = 1;
        const bool leavingAnimation = previous >= static_cast<uint8_t>(CameraMode::Animation) &&
                                      previous < static_cast<uint8_t>(CameraMode::Editor);
        if (leavingAnimation)
        {
            if (zge::animationOver(camera->node, done) != 0)
            {
                if (*done == 0)
                    zge::stopAnimation(camera->node);
            }
            else
                zge::stopTracks(camera->node);
            *g_cameraAnimationTime = ee::Float(0.0f);
            if ((currentLevel()->flags & kLevelStopReasonBits) == kStopOpeningFlyThru)
                enterGameMode(kModePlaying);
            else
            {
                enterGameMode(kModePaused);
                if (playPhase() == kPlayPhaseFlyThru)
                    setPlayPhase(0);
                showNpcsAfterFlyThru();
            }
        }
        camera->mode = mode;

        const CameraMode entered = static_cast<CameraMode>(mode);
        if (entered == CameraMode::R1)
            setCameraFov(camera, cameraTuning(CameraTuning::R1Fov));
        else
        {
            if (entered == CameraMode::AnimationAimed)
                *g_flyThruActive = 1;
            if (entered == CameraMode::Animation || entered == CameraMode::AnimationAimed)
                enterGameMode(kModeFlyThru);
            else
            {
                const ee::Float fov = cameraFov();
                setCameraFov(camera, fov);
            }
        }
        return previous;
    }
    READABLE_FUNCTION_WAITS(setFollowCameraMode, Verified);

    // orbitFreeCamera: moves the first camera by `angle` (the free camera and
    // the motion viewer).
    void orbitFreeCamera(uint32_t move, uint32_t angleArgument)
    {
        ee::Local<Vec4> offset;
        ee::Local<Vec4> unusedUp;
        FollowCamera &camera = *g_followCamera;
        const zge::Angle angle = static_cast<zge::Angle>(angleArgument);
        const FreeCameraMove how = static_cast<FreeCameraMove>(move);
        // An up vector no code uses, left in the frame.
        unusedUp->y = ee::Float(-1.0f);
        unusedUp->x = ee::Float(0.0f);
        unusedUp->z = ee::Float(0.0f);
        *offset = camera.lookAt - camera.eye;

        // From a vector's length and pitch and heading: the same vector
        // again.
        const auto fromAngles = [&](ee::Float length, int32_t pitch, int32_t heading) {
            const ee::Float sinPitch = zge::angleSin(static_cast<zge::Angle>(pitch));
            const ee::Float x = length * sinPitch * zge::angleCos(static_cast<zge::Angle>(heading));
            offset->x = x;
            const ee::Float sinPitchAgain = zge::angleSin(static_cast<zge::Angle>(pitch));
            const ee::Float z = length * sinPitchAgain * zge::angleSin(static_cast<zge::Angle>(heading));
            offset->z = z;
            offset->y = length * zge::angleCos(static_cast<zge::Angle>(pitch));
        };

        if (how == FreeCameraMove::Turn)
        {
            zge::vec3RotateY(&offset->xyz(), &offset->xyz(), angle);
            camera.lookAt = camera.eye + *offset;
        }
        else if (how == FreeCameraMove::Tilt)
        {
            const ee::Float length = zge::vec3Length(&offset->xyz());
            const zge::Angle heading = static_cast<short>(zge::angleAtan2(offset->z, offset->x));
            zge::Angle pitch = static_cast<zge::Angle>(static_cast<short>(zge::angleAcos(offset->y / length)) + angle);
            if (ee::toFloat(static_cast<int32_t>(pitch)) * zge::kAngleToDegrees < ee::Float(1.0f))
                pitch = 182; // a degree
            fromAngles(length, pitch, heading);
            camera.lookAt = camera.eye + *offset;
        }
        else if (how == FreeCameraMove::Orbit)
        {
            *offset = camera.eye - camera.lookAt;
            const ee::Float length = zge::vec3Length(&offset->xyz());
            const zge::Angle heading = static_cast<short>(zge::angleAtan2(offset->z, offset->x));
            const zge::Angle pitch = static_cast<short>(zge::angleAcos(offset->y / length));
            const zge::Angle turned = static_cast<zge::Angle>(heading + angle);
            fromAngles(length, pitch, turned);
            camera.eye = camera.lookAt + *offset;
        }
    }
    READABLE_FUNCTION(orbitFreeCamera, Verified);

    // playCameraAnimation: plays a camera animation (`fov` when given goes first):
    // `aimed` the eye's only, the view on the look-at point; otherwise the
    // animation has the whole camera, through the game's animators.
    uint32_t playCameraAnimation(ee::Ptr<FollowCamera> camera, ee::Address name, ee::Ptr<const ee::Float> fov, uint32_t aimed)
    {
        ee::Local<ee::Ptr<zge::Motion>> motion;
        *motion = {};
        if (fov)
            setCameraFov(camera, *fov);
        if (aimed != 0)
        {
            setFollowCameraMode(camera, static_cast<uint32_t>(CameraMode::AnimationAimed));
            zge::findAnimation(name, motion);
            zge::startAnimation(camera->node, *motion, ee::Float(0.0f));
            return 1;
        }
        setFollowCameraMode(camera, static_cast<uint32_t>(CameraMode::Animation));
        return zge::playAnimationNamed(camera->node, name, 4, ee::Float(0.0f), ee::Float(-1.0f));
    }
    READABLE_FUNCTION_WAITS(playCameraAnimation, Verified);

    void setSceneClearColour(ee::Ptr<zge::SceneObject> node, ee::Address colour);

    void setSceneFogColour(ee::Ptr<zge::SceneObject> node, ee::Address colour);

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // applyLevelCameraSettings: the level's picture settings: from the scene point
    // "Globals" the clear and fog colours, the fog and the far clip (none:
    // fog 170 to 250, far clip 1000); in split screen, on the levels that
    // have them, Camera / MP ... Far Clip, a thinner fog and fog at 65 to 90.
    // The far clip goes into Camera / Far Clip as well. On a level, the
    // camera's motion blur starts, at Camera / Normal Blur. Returns 1.
    uint32_t applyLevelCameraSettings(ee::Ptr<FollowCamera> camera)
    {
        ee::Local<uint8_t[4]> clearColour; // red, green, blue, alpha
        ee::Local<uint8_t[4]> fogColour;
        ee::Local<MotionBlurRequest> request;
        memset(clearColour, 0, 4);
        ee::Float fogStart = ee::Float(170.0f);
        ee::Float fogEnd = ee::Float(250.0f);
        ee::Float farClip;
        clearColour[3] = 0x80;
        std::memcpy(&fogColour[0], g_defaultFogColour.get(), 4);
        const uint32_t globalsHash = zge::nameHash(&*g_globalsName);
        const ee::Ptr<zge::ScenePoint> globals = zge::findScenePoint(globalsHash, 0, 0);

        if (globals)
        {
            const ee::Ptr<zge::TagRecord> record = globals->tags;
            fogStart = zge::tagFloat(record, kGlobalsFloats, kGlobalsFogStart);
            fogEnd = zge::tagFloat(record, kGlobalsFloats, kGlobalsFogEnd);
            farClip = zge::tagFloat(record, kGlobalsFloats, kGlobalsFarClip);
            zge::copyTagWord(record, kGlobalsColours, kGlobalsFogColour, fogColour);
            zge::copyTagWord(record, kGlobalsColours, kGlobalsClearColour, clearColour);
            fogColour[3] = static_cast<uint8_t>(ee::toInt(zge::tagFloat(record, kGlobalsFloats, kGlobalsFogAlpha)));
            if (*g_gameType == GameType::SplitScreen)
            {
                const int32_t level = g_gameState->level;
                uint8_t &fogAlpha = fogColour[3];
                if (level == kLevelBoardwalk)
                {
                    fogAlpha = 0x30;
                    farClip = cameraTuning(CameraTuning::MpBoardwalkFarClip);
                }
                else if (level == kLevelCivic)
                {
                    fogAlpha = 0x40;
                    farClip = cameraTuning(CameraTuning::MpCivicFarClip);
                }
                else if (level == kLevelMovie)
                {
                    fogAlpha = 0x40;
                    farClip = cameraTuning(CameraTuning::MpMovieFarClip);
                }
                else if (level == kLevelMuseum)
                {
                    fogAlpha = 0x40;
                    farClip = cameraTuning(CameraTuning::MpMuseumFarClip);
                }
                setSceneClearColour(camera->node, clearColour);
                setSceneFarClip(camera->node, farClip);
                setSceneFogColour(camera->node, fogColour);
                zge::setCameraFogStart(cameraOf(camera->node), ee::Float(65.0f));
                zge::setCameraFogEnd(cameraOf(camera->node), ee::Float(90.0f));
            }
            else
            {
                setSceneClearColour(camera->node, clearColour);
                setSceneFarClip(camera->node, farClip);
                setSceneFogColour(camera->node, fogColour);
                zge::setCameraFogStart(cameraOf(camera->node), fogStart);
                zge::setCameraFogEnd(cameraOf(camera->node), fogEnd);
            }
        }
        else
        {
            farClip = ee::Float(1000.0f);
            setSceneClearColour(camera->node, clearColour);
            setSceneFogColour(camera->node, fogColour);
            zge::setCameraFogStart(cameraOf(camera->node), fogStart);
            zge::setCameraFogEnd(cameraOf(camera->node), fogEnd);
            setSceneFarClip(camera->node, farClip);
        }
        (*g_tuningCategories)[kCameraTuningCategory].entries[static_cast<uint32_t>(CameraTuning::FarClip)].value = farClip;
        if (camera->motionBlur == 0)
        {
            if (g_gameState->topState->id != kTopStateLevel)
                return 1;
            *request = *g_motionBlurDefaults;
            request->strength = static_cast<uint8_t>(ee::toInt(cameraTuning(CameraTuning::NormalBlur)));
            request->colour = kMotionBlurColour;
            request->camera = cameraOf(camera->node);
            const uint32_t effectHash = zge::nameHash(&*g_motionBlurName);
            camera->motionBlur = zge::startFullScreenEffect(effectHash, request);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(applyLevelCameraSettings, Verified);

    // cutCamera: the next update cuts to the camera's place, at its
    // Camera / Distance. Nothing calls it.
    void cutCamera(ee::Ptr<FollowCamera> camera)
    {
        camera->snap = 1;
        camera->distance = cameraTuning(CameraTuning::Distance);
    }
    READABLE_FUNCTION(cutCamera, Verified);

    // cameraAnimationEvent: an animation event on the camera (animations.cpp's
    // handleAnimationEvent): sound event 51 (its sound bank's 107).
    void cameraAnimationEvent()
    {
        game::tail(soundEvent, kSoundCameraEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(cameraAnimationEvent, Verified);

    namespace
    {
        namespace tail
        {
        void setCameraClearColour(ee::Ptr<zge::Camera> camera, ee::Address colour) { ee::tailCall(sym::setCameraClearColour, camera, colour); }
        }
    }

    // setSceneClearColour: a node's scene's clear colour.
    void setSceneClearColour(ee::Ptr<zge::SceneObject> node, ee::Address colour)
    {
        tail::setCameraClearColour(cameraOf(node), colour);
    }
    READABLE_FUNCTION(setSceneClearColour, Verified);

    namespace
    {
        namespace tail
        {
        void setCameraFogColour(ee::Ptr<zge::Camera> camera, ee::Address colour) { ee::tailCall(sym::setCameraFogColour, camera, colour); }
        }
    }

    // setSceneFogColour: a node's scene's fog colour.
    void setSceneFogColour(ee::Ptr<zge::SceneObject> node, ee::Address colour)
    {
        tail::setCameraFogColour(cameraOf(node), colour);
    }
    READABLE_FUNCTION(setSceneFogColour, Verified);

    // updateCamera: a camera's frame (camera, seconds), by its CameraMode:
    // Free has the view from the eye to the look-at point, set again unless
    // both are in one place within g_lookMoved; Animation and AnimationAimed
    // do all of it themselves; Editor nothing. Then the scene's far clip (Camera / Far Clip), the field of
    // view and cameraMoved. 0 without a node, else 1.
    uint32_t updateCamera(ee::Ptr<FollowCamera> camera, ee::Float seconds)
    {
        ee::Local<SoundListeners> listeners;
        if (!camera->node)
            return 0;
        const uint8_t mode = camera->mode;
        switch (static_cast<CameraMode>(mode))
        {
        case CameraMode::Follow:
            updateFollowCamera(camera, seconds);
            break;
        case CameraMode::R1:
            updateR1Camera(camera, seconds);
            break;
        case CameraMode::Tracking:
            updateTrackingCamera(camera);
            break;
        case CameraMode::Behind:
            updateBehindCamera(camera);
            break;
        case CameraMode::Free:
            if (!eyeOnLookAt(camera, *g_lookMoved))
                zge::lookFromAt(camera->node, &camera->eye, &camera->lookAt, 0);
            break;
        case CameraMode::Animation:
        case CameraMode::AnimationAimed:
            updateAnimatedCamera(camera, seconds);
            return 1;
        case CameraMode::Editor:
            return 1;
        default: // Plain2, Plain6 (the table's) and past the table (a branch)
            break;
        }
        const ee::Ptr<zge::Camera> scene = cameraOf(camera->node);
        zge::setCameraFarPlane(scene, cameraTuning(CameraTuning::FarClip));
        const ee::Float fov = camera->fov * *g_fovToAngle;
        zge::setFov(camera->node, static_cast<int32_t>(static_cast<int16_t>(ee::toInt(fov))));
        // The sounds are heard from this camera.
        listeners->count = 1;
        listeners->objects[0] = camera->node;
        zge::cameraMoved(listeners);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateCamera, Verified);

    // cameraFov: the field of view. Split screen has one for each of its
    // levels, and no other level has one (0).
    ee::Float cameraFov()
    {
        if (*g_gameType == GameType::SplitScreen)
        {
            switch (g_gameState->level)
            {
            case kLevelCivic:
                return cameraTuning(CameraTuning::MpCivicFov);
            case kLevelBoardwalk:
                return cameraTuning(CameraTuning::MpBoardwalkFov);
            case kLevelMovie:
                return cameraTuning(CameraTuning::MpMovieFov);
            case kLevelMuseum:
                return cameraTuning(CameraTuning::MpMuseumFov);
            default:
                return ee::Float(0.0f);
            }
        }
        return cameraTuning(CameraTuning::Fov);
    }
    READABLE_FUNCTION(cameraFov, Verified);

    void setCameraFov(ee::Ptr<FollowCamera> camera, ee::Float degrees)
    {
        camera->fov = degrees;
    }
    READABLE_FUNCTION(setCameraFov, Verified);

    // restorePlayCameras: leaving the pause menu: the follow cameras back to the
    // Camera tuning's Type (the photo mode's override hooks it to leave its
    // own camera first).
    uint32_t restorePlayCameras()
    {
        const auto setMode = [](FollowCamera &camera) {
            setFollowCameraMode(&camera, ee::toInt(cameraTuning(CameraTuning::Type)) & 0xFFu);
        };
        setMode((*g_followCameras)[0]);
        if (*g_gameType == GameType::SplitScreen)
            setMode((*g_followCameras)[1]);
        return 1;
    }
    READABLE_FUNCTION_WAITS(restorePlayCameras, Verified);
}
