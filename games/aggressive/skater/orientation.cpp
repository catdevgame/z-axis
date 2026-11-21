// An orientation turned about an axis (was the sweep's rotateOrientation):
// the skater's turns (Ground's steering, the spine and pole turns, the
// grind balance's lean) rotate its three rows by a quaternion.
//
// The skater's orientation keeps its up row pointing the game's way (y
// grows downward, so up is (0, -1, 0) on flat ground): the rows as they are
// are a reflection, which a quaternion cannot hold. The turn negates up,
// rotates the proper rotation made of the rows and negates up back.
//

#include "moves.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vector.h"
#include "sym/aggressive/skater/orientation.cpp.h"

namespace aggressive::skater
{
    // rotateOrientation: turns `orientation` (rows right, up, forward) by
    // `degrees` about `axis`.
    void rotateOrientation(ee::Ptr<zge::Matrix> orientation, ee::Ptr<const zge::Vec3> axis, ee::Float degrees)
    {
        ee::Local<zge::Quaternion> turn;
        ee::Local<zge::Quaternion> turned;
        ee::Local<zge::Matrix> proper; // the orientation as a proper rotation: up flipped
        ee::Local<zge::Quaternion> current;
        ee::Local<Vec4> up;
        ee::Local<Vec4> forward;
        ee::Local<Vec4> right;
        zge::Matrix &rows = *orientation;

        zge::matrixIdentity(proper);
        *forward = row(rows, 2);
        *up = row(rows, 1);
        *right = row(rows, 0);
        vec3Negate(up, up);
        row(*proper, 2) = *forward;
        row(*proper, 1) = *up;
        row(*proper, 0) = *right;
        zge::matrixToQuaternion(current, proper);

        const ee::Float angle = degrees * zge::kDegreesToAngle;
        zge::quaternionFromAxisAngle(turn, axis, static_cast<zge::Angle>(ee::toInt(angle)));
        zge::quaternionMultiply(turned, turn, current);
        zge::quaternionToMatrix(orientation, turned);

        // Up back to the game's way.
        *up = row(rows, 1);
        vec3Negate(up, up);
        row(rows, 1) = *up;
    }
    READABLE_FUNCTION(rotateOrientation, Verified);
}
