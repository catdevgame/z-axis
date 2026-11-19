// The rigid bodies' box tests and the response of two boxes that touch
// (zge/collision/dynamics.h). Two boxes: the separating axis test on 15 axes,
// the three of each and their nine cross products; two that overlap get an
// impulse along the axis of least overlap and are pushed apart.

#include "dynamics.h"
#include "zge/assets/chunk_reader.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics.cpp.h"


namespace zge
{
    namespace
    {
        constexpr uint32_t kAxisCount = kSeparatingAxisCount, kFaceAxes = 6, kBoxAxes = 3;
        constexpr uint32_t kNoAxis = 100;

        // The box's corners: each axis scaled by its half extent, then the
        // eight sums.
        inline ee::Global<Vec4, sym::scaledAxisX> g_scaledAxisX;
        inline ee::Global<Vec4, sym::scaledAxisY> g_scaledAxisY;
        inline ee::Global<Vec4, sym::scaledAxisZ> g_scaledAxisZ;
        constexpr uint32_t kCornerCount = 8;
        inline ee::Global<Vec4[kCornerCount], sym::corners> g_corners;
        constexpr float kCornerTolerance = 0.05f;
        constexpr float kNoCornerAlong = 1000000.0f; // past any corner's (ZGE4's two-sided faces)

        // How much of a contact's speed a body keeps, and how much of the
        // speed along the face it loses (updateDynamics sets them each step).
        inline ee::Global<ee::Float, sym::g_contactRestitution> g_contactRestitution;
        inline ee::Global<ee::Float, sym::g_contactDamping> g_contactDamping;

        // orientedBoxesOverlap's boxes as rows.
        inline ee::Global<Matrix, sym::g_boxARows> g_boxARows;
        inline ee::Global<Matrix, sym::g_boxBRows> g_boxBRows;

        enum ContactKind : int32_t
        {
            kSeparating = 1,   // above the speed
            kResting = 2,      // within it
            kClosing = 3,
        };
        constexpr float kRestingSpeed = 0.2f;
        constexpr float kRestitutionFactor = -1.8f;    // -(1 + 0.8)
        constexpr float kShortAxisSquared = 9.9999994e-11f;
        constexpr float kShortAxis = 1e-05f;
        constexpr float kShortNormalSquared = 0.01f;
        constexpr float kContactSpinKept = 0.9f;
        constexpr float kContactPushKept = 0.3f;

        // A held point: how much of its distance from the place each step
        // closes, the speed under which it is left alone, and what it keeps.
        constexpr float kHoldCorrection = 0.3f;
        constexpr float kHoldSlowSquared = 1e-04f;
        constexpr float kHoldSideDamping = -0.1f;
        constexpr float kHoldSpinKept = 0.9f;

        int32_t classifySpeed(ee::Float speed)
        {
            int32_t kind = kSeparating;
            if (!(ee::Float(kRestingSpeed) < speed))
                kind = ee::Float(-kRestingSpeed) < speed ? kResting : kClosing;
            return kind;
        }

        // The separating axes of two boxes by their rows: the three of each,
        // then the nine cross products.
        void fillSeparatingAxes(const Matrix &rowsA, const Matrix &rowsB)
        {
            SeparatingAxis *axes = *g_separatingAxes;
            for (uint32_t i = 0; i < kBoxAxes; ++i)
            {
                axes[i].axis = matrixRow(rowsA, i);
                axes[kBoxAxes + i].axis = matrixRow(rowsB, i);
            }
            for (uint32_t i = 0; i < kBoxAxes; ++i)
                for (uint32_t j = 0; j < kBoxAxes; ++j)
                    axes[kFaceAxes + i * kBoxAxes + j].axis = cross(matrixRow(rowsA, i), matrixRow(rowsB, j));
        }
    }

    // boxClosestPoint: the box's point on a plane by its normal: the average of
    // the corners within 0.05 of the plane at `distance` from the box's
    // centre along the normal. `cornersKnown`: the last call's still hold.
    void boxClosestPoint(ee::Ptr<RigidBody> body, const Vec4 *normal, Vec4 *out, uint32_t cornersKnown, ee::Float distance)
    {
        const ee::Float centre = dot(body->position, *normal);
        Vec4 *corners = *g_corners;
        if (cornersKnown == 0)
        {
            Vec4 &x = *g_scaledAxisX, &y = *g_scaledAxisY, &z = *g_scaledAxisZ;
            const Vec4 &half = body->halfExtents->half;
            x = matrixRow(body->rotation, 0) * half.x;
            y = matrixRow(body->rotation, 1) * half.y;
            z = matrixRow(body->rotation, 2) * half.z;
            const auto fourCorners = [&](Vec4 *corner)
            {
                corner[0] = x + y + z;
                corner[1] = x + y - z;
                corner[2] = x - y + z;
                corner[3] = x - y - z;
            };
            fourCorners(corners);
            vec3Negate(&x.xyz(), &x.xyz());
            fourCorners(corners + 4);
        }
        const ee::Float wanted = distance - centre;
        *out = kOrigin;
        int32_t found = 0;
        // ZGE4: a body with two-sided faces takes the corners past the plane instead, and the nearest of them all is
        // kept for when none is (as the original tests it: along the normal below the plane; above it, the corner's
        // distance against the normal compared with the last kept one's along it).
        const bool twoSided = game::readable::kIsBmx && body->hasTwoSidedFaces();
        ee::Float nearestAlong = kNoCornerAlong;
        uint32_t nearest = 0;
        for (uint32_t i = 0; i < kCornerCount; ++i)
        {
            const ee::Float along = dot(corners[i], *normal);
            if (twoSided)
            {
                const bool below = wanted < ee::Float(0.0f);
                const ee::Float at = centre + along;
                if (below ? at < distance : distance < at)
                {
                    ++found;
                    *out = *out + corners[i];
                }
                if ((below ? along : -along) < nearestAlong)
                {
                    nearestAlong = along;
                    nearest = i;
                }
            }
            else if (ee::abs(along - wanted) < ee::Float(kCornerTolerance))
            {
                ++found;
                *out = *out + corners[i];
            }
        }
        if (game::readable::kIsBmx && found == 0)
        {
            // ZGE4 does not divide by no corners: the origin, or the nearest corner.
            if (twoSided)
                *out = corners[nearest];
        }
        else
            *out = *out * (ee::Float(1.0f) / ee::toFloat(found));
        *out = *out + body->position;
    }
    READABLE_FUNCTION_VU0(boxClosestPoint, Verified);

    // projectTriangleOnAxis: a triangle's three points along an axis: the middle of
    // their range and its half width.
    void projectTriangleOnAxis(const Vec4 *triangle, const Vec4 *axis, ee::Float *centreOut, ee::Float *extentOut)
    {
        ee::Float highest = dot(*axis, triangle[0]);
        ee::Float lowest = highest;
        const ee::Float second = dot(*axis, triangle[1]);
        if (second < highest)
            lowest = second;
        if (highest < second)
            highest = second;
        const ee::Float third = dot(*axis, triangle[2]);
        if (third < lowest)
            lowest = third;
        if (highest < third)
            highest = third;
        const ee::Float centre = (highest + lowest) * ee::Float(0.5f);
        *centreOut = centre;
        *extentOut = highest - centre;
    }
    READABLE_FUNCTION(projectTriangleOnAxis, Verified);

    // intervalsSeparate: 1 if two intervals, centre and half width each, do not meet.
    int32_t intervalsSeparate(ee::Float centreA, ee::Float radiusA, ee::Float centreB, ee::Float radiusB)
    {
        const bool meet = ee::abs(centreA - centreB) < radiusA + radiusB;
        return meet ? 0 : 1;
    }
    READABLE_FUNCTION(intervalsSeparate, Verified);

    // classifyFaceNormal: which way a face's normal lies along a direction.
    int32_t classifyFaceNormal(const Vec4 *a, const Vec4 *b)
    {
        return classifySpeed(dot(*a, *b));
    }
    READABLE_FUNCTION(classifyFaceNormal, Verified);

    // dynamicsContactObject: what findBodyFaceContacts last touched.
    ee::Ptr<HitNode> dynamicsContactObject() { return *g_contactObject; }
    READABLE_FUNCTION(dynamicsContactObject, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(sym::memset, m, c, n); }
    }

    // resolveBodyContacts: a body against the world's faces this step. With a face
    // that it closes on or rests on: an impulse at the contact point, then
    // the speed along the face damped and the spin too, and what the faces
    // push it out by kept. A body already touching something does not look.
    void resolveBodyContacts(ee::Ptr<RigidBody> body)
    {
        ee::Local<Vec4> impulse;
        ee::Local<Vec4> turn;
        ee::Local<Vec4> point;
        ee::Local<Vec4> velocityAtPoint;
        ee::Local<Vec4> arm;
        ee::Local<Vec4> contact;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> push;
        const ee::Float keep = -(*g_contactRestitution + ee::Float(1.0f));
        constexpr uint32_t kBytes = sizeof(Vec4);
        memset(impulse, 0, kBytes);
        memset(turn, 0, kBytes);
        memset(contact, 0, kBytes);
        memset(normal, 0, kBytes);
        memset(push, 0, kBytes);
        int32_t kind = 0;
        if (body->shape == 0)
            kind = findBodyFaceContacts(body, contact, normal, push);
        body->contactCount = 0;
        if (kind != kResting && kind != kClosing)
            return;

        normal = unit(normal);
        point = *contact;
        arm = point - body->position;
        pointVelocity(body, point, velocityAtPoint);
        vec3Length(&arm->xyz()); // (the original measures it and drops the result)
        const ee::Float wanted = keep * dot(velocityAtPoint, normal);
        const ee::Float inverseMass = ee::Float(1.0f) / body->mass;
        const ee::Float turning = turningShare(arm, normal, *body);
        impulse = normal * (wanted / (inverseMass + turning));
        turn = rotated(body->worldInverseInertia, cross(arm, impulse));
        body->spin = body->spin + turn;
        impulse = impulse * inverseMass;
        body->velocity = body->velocity + impulse;

        // The speed along the face damped. ZGE2 keeps the damping as the negative number it multiplies by (-0.1, an
        // engine constant); ZGE3 keeps the dynamics' 0.1 and negates it here.
        Vec4 alongNormal = normal * dot(body->velocity, normal);
        alongNormal = body->velocity - alongNormal;
        const ee::Float dampingScale = game::readable::kIsDm2 ? *g_contactDamping : -*g_contactDamping;
        const Vec4 damping = alongNormal * dampingScale;
        body->velocity = body->velocity + damping;
        body->spin = body->spin * kContactSpinKept;
        push = push * kContactPushKept;
        body->contactPush = *push;
        body->contactCount = 1;
    }

    namespace
    {
        // ZGE4's contact response: how much spin a body keeps after a contact (ZGE3 0.9).
        constexpr float kContactSpinKeptZge4 = 0.95f;
    }

    // resolveBodyContactsFromDynamics: ZGE4's resolveBodyContacts, the body's dynamics with it. Only a face the body
    // closes on gets the impulse, and the normal is used as found (not normalised); the speed into the face is taken
    // off, what is left scaled by one less the dynamics' damping, and the impulse scaled by the dynamics' restitution.
    // A body resting on a face keeps its push (scaled) and some spin, with no impulse; one touching nothing has its
    // push cleared.
    void resolveBodyContactsFromDynamics(ee::Ptr<Dynamics> dynamics, ee::Ptr<RigidBody> body)
    {
        ee::Local<Vec4> impulse;
        ee::Local<Vec4> turn;
        ee::Local<Vec4> point;
        ee::Local<Vec4> velocityAtPoint;
        ee::Local<Vec4> arm;
        ee::Local<Vec4> contact;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> push;
        const ee::Float keep = -(*g_contactRestitution + ee::Float(1.0f));
        constexpr uint32_t kBytes = sizeof(Vec4);
        memset(impulse, 0, kBytes);
        memset(turn, 0, kBytes);
        memset(contact, 0, kBytes);
        memset(normal, 0, kBytes);
        memset(push, 0, kBytes);
        int32_t kind = 0;
        if (body->shape == 0)
            kind = findBodyFaceContacts(body, contact, normal, push);
        body->contactCount = 0;
        if (kind == kClosing)
        {
            point = *contact;
            arm = point - body->position;
            pointVelocity(body, point, velocityAtPoint);
            vec3Length(&arm->xyz()); // (the original measures it and drops the result)
            const ee::Float wanted = keep * dot(velocityAtPoint, normal);
            const ee::Float inverseMass = ee::Float(1.0f) / body->mass;
            const ee::Float turning = turningShare(arm, normal, *body);
            impulse = normal * (wanted / (inverseMass + turning));
            turn = rotated(body->worldInverseInertia, cross(arm, impulse));
            body->spin = body->spin + turn;
            const Vec4 intoFace = normal * dot(body->velocity, normal);
            body->velocity = body->velocity - intoFace;
            body->velocity = body->velocity * (ee::Float(1.0f) - dynamics->contactDamping);
            impulse = impulse * (inverseMass * dynamics->restitution);
            body->velocity = body->velocity + impulse;
            body->spin = body->spin * kContactSpinKeptZge4;
            push = push * kContactPushKept;
            body->contactPush = *push;
            body->contactCount = 1;
        }
        else if (kind == kResting)
        {
            push = push * kContactPushKept;
            body->spin = body->spin * kContactSpinKeptZge4;
            body->contactPush = *push;
        }
        else
            body->contactPush = kOrigin;
    }
    READABLE_FUNCTION_WAITS_OWN(resolveBodyContacts, Verified, bmx, resolveBodyContactsFromDynamics);

    // resolveBodyContact: a body's point held at a place: an impulse at the point
    // that stops it, the speed that would close 0.3 of the distance each
    // step added first, then some of its sideways speed and spin taken off.
    void resolveBodyContact(ee::Ptr<RigidBody> body, ee::Ptr<BodyContact> contact)
    {
        // Every working vector on the game's stack at the original's place:
        // what they leave there is what resolveBodyContacts and
        // transformContactFaces find after resolveBodyContactList.
        ee::Local<Vec4> heldPoint;
        ee::Local<Vec4> direction;
        ee::Local<Vec4> working;    // the closing speed, then the turning share's vector, then the sideways speed
        ee::Local<Vec4> point;
        ee::Local<Vec4> arm;
        ee::Local<Vec4> velocityAtPoint;
        ee::Local<Vec4> impulse;
        ee::Local<Vec4> turn;
        const ee::Ptr<BodyContactDetail> detail = contact->detail;
        heldPoint = rotated(body->rotation, detail->anchor);
        heldPoint = *heldPoint + body->position;
        point = *heldPoint;
        arm = *point - body->position;
        pointVelocity(body, point, velocityAtPoint);
        working = *point - detail->target;
        working = *working * (ee::Float(kHoldCorrection) / *g_dynamicsStepSeconds);
        velocityAtPoint = *velocityAtPoint + *working;
        direction = *velocityAtPoint;
        if (dot(*direction, *direction) < ee::Float(kHoldSlowSquared))
            return;

        direction = unit(*direction);
        const ee::Float speed = -dot(*velocityAtPoint, *direction);
        const ee::Float inverseMass = ee::Float(1.0f) / body->mass;
        working = cross(rotated(body->worldInverseInertia, cross(*arm, *direction)), *arm);
        const ee::Float turning = dot(*working, *direction);
        impulse = *direction * (speed / (inverseMass + turning));
        turn = rotated(body->worldInverseInertia, cross(*arm, *impulse));
        body->spin = body->spin + *turn;
        impulse = *impulse * inverseMass;
        body->velocity = body->velocity + *impulse;
        const ee::Float along = dot(body->velocity, *direction);
        working = *direction * along;
        working = body->velocity - *working;
        working = *working * kHoldSideDamping;
        body->velocity = body->velocity + *working;
        body->spin = body->spin * kHoldSpinKept;
    }
    READABLE_FUNCTION(resolveBodyContact, Verified);

    // resolveSlackHoldContact: ZGE4's hold with slack (kBodyContactSlackHold). A point further from its target than
    // the detail's slack is pulled as a plain hold is (resolveBodyContact), toward the place the slack reaches from
    // the target on the point's side; within the slack it is left alone. For that call the contact is a hold whose
    // detail has that place for its target (a detail on the stack), then it is the slack hold again.
    inline void resolveSlackHoldContact(ee::Ptr<RigidBody> body, ee::Ptr<BodyContact> contact)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<Vec4> fromTarget; // the point less its target, then the slack's reach that way
        ee::Local<Vec4> point;
        ee::Local<BodyContactDetail> held;
        const ee::Ptr<BodyContactDetail> detail = contact->detail;
        point = rotated(body->rotation, detail->anchor);
        point = *point + body->position;
        fromTarget = *point - detail->target;
        if (!(detail->slack < vec3Length(&fromTarget->xyz())))
            return;
        fromTarget = unit(*fromTarget);
        fromTarget = scaledAfterMeasure(*fromTarget, detail->slack);
        held->target = *fromTarget + detail->target;
        held->anchor = detail->anchor;
        held->body = detail->body;
        contact->detail = held;
        contact->kind = kBodyContactHold;
        resolveBodyContact(body, contact);
        contact->detail = detail;
        contact->kind = kBodyContactSlackHold;
    }
    READABLE_FUNCTION_ONLY(bmx, resolveSlackHoldContact);

    // resolveBodyContactList: each of the body's contacts that holds a point and is on (ZGE4: and each slack hold).
    void resolveBodyContactList(ee::Ptr<RigidBody> body)
    {
        for (ee::Ptr<BodyContact> contact = body->contacts; contact; contact = contact->next)
        {
            if (!(ee::Float(0.0f) < contact->strength))
                continue;
            if (contact->kind == kBodyContactHold)
                resolveBodyContact(body, contact);
            else if constexpr (game::readable::kIsBmx)
            {
                if (contact->kind == kBodyContactSlackHold)
                    resolveSlackHoldContact(body, contact);
            }
        }
    }
    READABLE_FUNCTION(resolveBodyContactList, Verified);

    namespace
    {
        // A separating axis's squared length: ZGE2 through its lengthSquared, ZGE3 inline.
        ee::Float axisSquared(const Vec4 &axis)
        {
            if constexpr (game::readable::kIsDm2)
                return lengthSquared(&axis.xyz());
            else
                return squaredLength(axis);
        }
    }

    // boxesOverlap: 1 if no axis of the 15 separates the boxes. Their
    // intervals stay in the table for boxesContact.
    uint32_t boxesOverlap(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second)
    {
        fillSeparatingAxes(first->rotation, second->rotation);
        for (uint32_t index = 0; index < kAxisCount; ++index)
        {
            SeparatingAxis &entry = (*g_separatingAxes)[index];
            projectBoxOnAxis(first, &entry.axis, &entry.centreA, &entry.radiusA);
            projectBoxOnAxis(second, &entry.axis, &entry.centreB, &entry.radiusB);
            if (ee::Float(kShortAxisSquared) < axisSquared(entry.axis) &&
                intervalsSeparate(entry.centreA, entry.radiusA, entry.centreB, entry.radiusB) != 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(boxesOverlap, Verified);

    // projectOrientedBoxOnAxis: a box on an axis: the centre's place along it, and how
    // far the box reaches, the sum of its half extents along its own axes
    // (the rows of `rows`).
    void projectOrientedBoxOnAxis(ee::Ptr<OrientedBox> box, const Matrix *rows, const Vec4 *axis, ee::Float *centreOut, ee::Float *radiusOut)
    {
        *centreOut = dot(*axis, box->centre);
        const ee::Float alongX = ee::abs(box->halfExtents.x * dot(matrixRow(*rows, 0), *axis));
        const ee::Float alongY = ee::abs(box->halfExtents.y * dot(matrixRow(*rows, 1), *axis));
        const ee::Float alongZ = ee::abs(box->halfExtents.z * dot(matrixRow(*rows, 2), *axis));
        *radiusOut = alongX + alongY + alongZ;
    }
    READABLE_FUNCTION_VU0(projectOrientedBoxOnAxis, Verified);

    // orientedBoxesOverlap: boxesOverlap for boxes that aren't bodies: 1 if no axis
    // of the 15 separates them, their rows made from their orientations. The
    // intervals stay in the table.
    uint32_t orientedBoxesOverlap(ee::Ptr<OrientedBox> first, ee::Ptr<OrientedBox> second)
    {
        Matrix &rowsA = *g_boxARows, &rowsB = *g_boxBRows;
        quaternionToMatrix(&rowsA, &first->orientation);
        quaternionToMatrix(&rowsB, &second->orientation);
        fillSeparatingAxes(rowsA, rowsB);
        for (uint32_t index = 0; index < kAxisCount; ++index)
        {
            SeparatingAxis &entry = (*g_separatingAxes)[index];
            projectOrientedBoxOnAxis(first, &rowsA, &entry.axis, &entry.centreA, &entry.radiusA);
            projectOrientedBoxOnAxis(second, &rowsB, &entry.axis, &entry.centreB, &entry.radiusB);
            if (ee::Float(kShortAxisSquared) < squaredLength(entry.axis) &&
                intervalsSeparate(entry.centreA, entry.radiusA, entry.centreB, entry.radiusB) != 0)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(orientedBoxesOverlap, Verified);

    namespace
    {
        // How far a box reaches along a line: centre and half width.
        struct BoxSpan
        {
            ee::Float centre, radius;
        };
    }

    // boxesContact: where two boxes that overlap touch: the axis of least
    // overlap the normal, turned from the second to the first, the point
    // midway between the boxes along the line of their centres.
    void boxesContact(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second, Vec4 *pointOut, Vec4 *normalOut, ee::Float *depthOut)
    {
        ee::Local<Vec4> between;
        ee::Local<BoxSpan> spanA;
        ee::Local<BoxSpan> spanB;
        SeparatingAxis *axes = *g_separatingAxes;
        for (uint32_t i = 0; i < kFaceAxes; ++i)
            axes[i].length = ee::Float(1.0f);

        // The cross products made units; those too short to be axes get no length.
        for (uint32_t i = kFaceAxes; i < kAxisCount; ++i)
        {
            SeparatingAxis &entry = axes[i];
            const ee::Float length = vec3Length(&entry.axis.xyz());
            entry.length = length;
            if (ee::Float(kShortAxis) < length)
            {
                entry.axis = entry.axis * (ee::Float(1.0f) / length);
                entry.radiusA = entry.radiusA / entry.length;
                entry.centreB = entry.centreB / entry.length;
                entry.radiusB = entry.radiusB / entry.length;
                entry.centreA = entry.centreA / entry.length;
            }
            else
                entry.length = ee::Float(0.0f);
        }

        // The least overlap, each axis turned so that the first box is on
        // its positive side.
        uint32_t best = kNoAxis;
        ee::Float depth(0.0f);
        for (uint32_t index = 0; index < kAxisCount; ++index)
        {
            SeparatingAxis &entry = axes[index];
            if (entry.centreB < entry.centreA)
            {
                entry.centreA = -entry.centreA;
                entry.centreB = -entry.centreB;
                vec3Negate(&entry.axis.xyz(), &entry.axis.xyz());
            }
            if (!(ee::Float(0.0f) < entry.length))
                continue;
            const ee::Float overlap = entry.centreA + entry.radiusA - (entry.centreB - entry.radiusB);
            if (best == kNoAxis || overlap < depth)
            {
                best = index;
                depth = overlap;
            }
        }

        Vec4 normal = axes[best].axis;
        between = first->position - second->position;
        if (dot(between, normal) < ee::Float(0.0f))
            normal = normal * ee::Float(-1.0f);
        const ee::Float tooShort(kShortNormalSquared);
        if (dot(normal, normal) < tooShort)
            normal.y = ee::Float(-1.0f);
        *normalOut = normal;
        *depthOut = depth;
        if (dot(between, between) < tooShort)
            between->y = ee::Float(-1.0f);
        between = unit(between);

        projectBoxOnAxis(first, between, &spanA->centre, &spanA->radius);
        projectBoxOnAxis(second, between, &spanB->centre, &spanB->radius);
        Vec4 midpoint = first->position + second->position;
        between = between * (spanB->radius - spanA->radius);
        midpoint = midpoint + between;
        *pointOut = midpoint * ee::Float(0.5f);
    }
    READABLE_FUNCTION(boxesContact, Verified);

    // orientedBoxContact: boxesContact for boxes that aren't bodies (a
    // skater, a car or an NPC against a body), after orientedBoxesOverlap
    // filled the axis table and the boxes' rows: the axis of least overlap
    // the normal, turned from the second box to the first, the point midway
    // between the boxes along the line of their centres.
    void orientedBoxContact(ee::Ptr<OrientedBox> first, ee::Ptr<OrientedBox> second, Vec4 *pointOut, Vec4 *normalOut,
                            ee::Float *depthOut)
    {
        ee::Local<Vec4> between;
        ee::Local<BoxSpan> spanA;
        ee::Local<BoxSpan> spanB;
        SeparatingAxis *axes = *g_separatingAxes;
        for (uint32_t i = 0; i < kFaceAxes; ++i)
            axes[i].length = ee::Float(1.0f);

        // The cross products made units; those too short to be axes get no length.
        for (uint32_t i = kFaceAxes; i < kAxisCount; ++i)
        {
            SeparatingAxis &entry = axes[i];
            const ee::Float length = vec3Length(&entry.axis.xyz());
            entry.length = length;
            if (ee::Float(kShortAxis) < length)
            {
                entry.axis = entry.axis * (ee::Float(1.0f) / length);
                entry.radiusA = entry.radiusA / entry.length;
                entry.centreB = entry.centreB / entry.length;
                entry.radiusB = entry.radiusB / entry.length;
                entry.centreA = entry.centreA / entry.length;
            }
            else
                entry.length = ee::Float(0.0f);
        }

        // The least overlap, each axis turned so that the first box is on
        // its positive side.
        uint32_t best = kNoAxis;
        ee::Float depth(0.0f);
        for (uint32_t index = 0; index < kAxisCount; ++index)
        {
            SeparatingAxis &entry = axes[index];
            if (entry.centreB < entry.centreA)
            {
                entry.centreA = -entry.centreA;
                entry.centreB = -entry.centreB;
                vec3Negate(&entry.axis.xyz(), &entry.axis.xyz());
            }
            if (!(ee::Float(0.0f) < entry.length))
                continue;
            const ee::Float overlap = entry.centreA + entry.radiusA - (entry.centreB - entry.radiusB);
            if (best == kNoAxis || overlap < depth)
            {
                best = index;
                depth = overlap;
            }
        }

        Vec4 normal = axes[best].axis;
        between = first->centre - second->centre;
        if (dot(between, normal) < ee::Float(0.0f))
            normal = normal * ee::Float(-1.0f);
        const ee::Float tooShort(kShortNormalSquared);
        if (dot(normal, normal) < tooShort)
            normal.y = ee::Float(-1.0f);
        *normalOut = normal;
        *depthOut = depth;
        if (dot(between, between) < tooShort)
            between->y = ee::Float(-1.0f);
        between = unit(between);

        projectOrientedBoxOnAxis(first, &*g_boxARows, between, &spanA->centre, &spanA->radius);
        projectOrientedBoxOnAxis(second, &*g_boxBRows, between, &spanB->centre, &spanB->radius);
        Vec4 midpoint = first->centre + second->centre;
        between = between * (spanB->radius - spanA->radius);
        midpoint = midpoint + between;
        *pointOut = midpoint * ee::Float(0.5f);
    }
    READABLE_FUNCTION(orientedBoxContact, Verified);

    // classifyContactVelocity: how fast two bodies at a point move along a normal.
    int32_t classifyContactVelocity(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second, const Vec4 *point, const Vec4 *normal)
    {
        ee::Local<Vec4> velocityA;
        ee::Local<Vec4> velocityB;
        pointVelocity(first, point, velocityA);
        pointVelocity(second, point, velocityB);
        const Vec4 relative = *velocityA - *velocityB;
        return classifySpeed(dot(relative, *normal));
    }
    READABLE_FUNCTION(classifyContactVelocity, Verified);

    // pairStillTouching: 1 if two boxes still overlap, never with a body on
    // the first one's ignore list (DM2's bodies keep one).
    uint32_t pairStillTouching(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second)
    {
        if constexpr (game::readable::kIsDm2)
        {
            if (first->ignores(second))
                return 0;
        }
        if (first->shape == 0)
        {
            if (second->shape != 0)
                return 0;
            if (static_cast<int32_t>(boxesOverlap(first, second)) != 0)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(pairStillTouching, Verified);

    // pairTouched: two boxes have just touched: if they are closing, an
    // impulse along the normal that keeps 0.8 of their speed, and both pushed
    // out by the depth, the first along the normal.
    void pairTouched(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second)
    {
        ee::Local<Vec4> velocityA;
        ee::Local<Vec4> velocityB;
        ee::Local<Vec4> point;
        ee::Local<Vec4> normal;
        ee::Local<ee::Float> depth;
        boxesContact(first, second, point, normal, depth);
        if (classifyContactVelocity(first, second, point, normal) != kClosing)
            return;
        pointVelocity(first, point, velocityA);
        pointVelocity(second, point, velocityB);

        const Vec4 armA = point - first->position;
        const Vec4 armB = point - second->position;
        const Vec4 relative = *velocityA - *velocityB;
        const ee::Float closing = dot(normal, relative);
        const ee::Float inverseMassA = ee::Float(1.0f) / first->mass;
        const ee::Float inverseMassB = ee::Float(1.0f) / second->mass;
        const ee::Float turningA = turningShare(armA, normal, *first);
        const ee::Float turningB = turningShare(armB, normal, *second);
        const ee::Float strength = closing * ee::Float(kRestitutionFactor) / (inverseMassA + inverseMassB + turningA + turningB);
        Vec4 impulse = normal * strength;
        applyImpulse(*first, impulse, armA);
        impulse = impulse * ee::Float(-1.0f);
        applyImpulse(*second, impulse, armB);

        normal = normal * *depth;
        first->position = first->position + normal;
        if constexpr (!game::readable::kIsBmx) // (ZGE4's bodies keep no pushed position)
            first->pushedPosition = first->pushedPosition + normal;
        second->position = second->position - normal;
        if constexpr (!game::readable::kIsBmx)
            second->pushedPosition = second->pushedPosition - normal;
    }
    READABLE_FUNCTION(pairTouched, Verified);

    // collideBodyPair: pairTouched's older form, with the test in front: two
    // box bodies that overlap get an impulse along the contact normal that
    // keeps 0.26 of their speed, and are pushed out by the depth, the first
    // along the normal. (Nothing in the game calls it.)
    // Kept: any verdict of classifyContactVelocity lets the impulse through
    // (it is never 0), so boxes already parting get it too.
    void collideBodyPair(ee::Ptr<RigidBody> first, ee::Ptr<RigidBody> second)
    {
        constexpr float kOldRestitutionFactor = -1.26f; // -(1 + 0.26)
        ee::Local<Vec4> point;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> velocityA;
        ee::Local<Vec4> velocityB;
        ee::Local<ee::Float> depth;
        if constexpr (game::readable::kIsDm2) // (ZGE2's bodies keep an ignore list, as pairStillTouching asks)
        {
            if (first->ignores(second))
                return;
        }
        if (first->shape != 0 || second->shape != 0 || boxesOverlap(first, second) == 0)
            return;
        boxesContact(first, second, point, normal, depth);
        if (classifyContactVelocity(first, second, point, normal) == 0)
            return;
        pointVelocity(first, point, velocityA);
        pointVelocity(second, point, velocityB);

        const Vec4 armA = point - first->position;
        const Vec4 armB = point - second->position;
        const Vec4 relative = *velocityA - *velocityB;
        const ee::Float closing = dot(normal, relative);
        const ee::Float inverseMassA = ee::Float(1.0f) / first->mass;
        const ee::Float inverseMassB = ee::Float(1.0f) / second->mass;
        const ee::Float turningA = turningShare(armA, normal, *first);
        const ee::Float turningB = turningShare(armB, normal, *second);
        const ee::Float strength = closing * ee::Float(kOldRestitutionFactor) / (inverseMassA + inverseMassB + turningA + turningB);
        Vec4 impulse = normal * strength;
        applyImpulse(*first, impulse, armA);
        impulse = impulse * ee::Float(-1.0f);
        applyImpulse(*second, impulse, armB);

        normal = normal * *depth;
        first->position = first->position + normal;
        first->pushedPosition = first->pushedPosition + normal;
        second->position = second->position - normal;
        second->pushedPosition = second->pushedPosition - normal;
    }
    READABLE_FUNCTION(collideBodyPair, Draft);

    namespace
    {
        // findBodyFaceContacts' count of the faces touched so far.
        inline ee::Global<uint32_t, sym::g_faceContactCount> g_faceContactCount;

    }

    // boxPointOnFacePlane: findBodyFaceContacts' step for a face that touches,
    // on its own: the box's point on the face's plane (boxClosestPoint), the
    // table's face-normal axis and its centre turned round first. out is
    // cleared to the origin before anything else. (Nothing in the game calls
    // it.)
    void boxPointOnFacePlane(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> out)
    {
        constexpr uint32_t kFaceNormalAxis = 3;
        aligned(out) = kOrigin;
        SeparatingAxis &plane = (*g_separatingAxes)[kFaceNormalAxis];
        vec3Negate(&plane.axis.xyz(), &plane.axis.xyz());
        const ee::Float centre = -plane.centreA;
        const uint32_t cornersKnown = *g_faceContactCount;
        const ee::Float distance = centre + plane.radiusA;
        plane.centreA = centre;
        game::tail(boxClosestPoint, body, &plane.axis, out, cornersKnown, distance);
    }
    READABLE_FUNCTION(boxPointOnFacePlane, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float vec3Length(ee::Ptr<const Vec3> entry) { return zge::vec3Length(entry); }
    }


    // readZfsdChunk: a ZFSD chunk names the dynamics file to read in its
    // place (includeChunkFile); its verdict.
    int32_t readZfsdChunk(ee::Ptr<ChunkReader> reader, uint32_t context)
    {
        return includeChunkFile(reader, context);
    }
    READABLE_FUNCTION_WAITS(readZfsdChunk, Verified);

    // boxHalfExtents: a box body's half extents copied out (the aligned
    // quadword); 0 for a body of another shape, else out.
    ee::Ptr<Vec4> boxHalfExtents(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> out)
    {
        if (body->shape != 0)
            return ee::Ptr<Vec4>();
        aligned(out) = aligned(body->halfExtents).half;
        return out;
    }
    READABLE_FUNCTION(boxHalfExtents, Draft);

    // setBoxHalfExtents: a box body's half extents set (the aligned
    // quadword); a body of another shape is left alone.
    void setBoxHalfExtents(ee::Ptr<RigidBody> body, ee::Ptr<Vec4> half)
    {
        if (body->shape == 0)
            aligned(body->halfExtents).half = aligned(half);
    }
    READABLE_FUNCTION(setBoxHalfExtents, Draft);

    // setBodyTwoSidedFaces (ZGE4 only): whether faces touch the body from either side (RigidBody::twoSidedFaces).
    inline void setBodyTwoSidedFaces(ee::Ptr<RigidBody> body, uint32_t twoSided)
    {
        body->setTwoSidedFaces(twoSided);
    }
    READABLE_FUNCTION_ONLY(bmx, setBodyTwoSidedFaces);

    // bodyClosingOnFaces: 1 when a box body touches the world's faces and
    // moves into them (findBodyFaceContacts' kClosing), else 0.
    int32_t bodyClosingOnFaces(ee::Ptr<RigidBody> body)
    {
        int32_t kind = 0;
        if (body->shape == 0)
        {
            ee::Local<Vec4> point;
            ee::Local<Vec4> normal;
            ee::Local<Vec4> push;
            kind = findBodyFaceContacts(body, point, normal, push);
        }
        return kind == kClosing ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(bodyClosingOnFaces, Draft);
}
