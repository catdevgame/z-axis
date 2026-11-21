// A rigid body's box against the faces of the objects it can touch
// (zge/collision/dynamics.h): the separating axis test on 13 axes, the face's
// normal, the box's three axes and the nine crosses of those with the
// triangle's edges.

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics_faces.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_faceContactCount> g_faceContactCount;

        // The axes of the table: the box's three, the face's normal, then the
        // nine crosses of a box axis with an edge.
        constexpr uint32_t kBoxAxes = 3, kFaceNormalAxis = 3, kFirstEdgeAxis = 4, kEdgeAxisEnd = 13, kTriangleEdges = 3;
        constexpr uint32_t kTriangleVertices = 3;
        constexpr float kBoxTolerance = 0.0f;       // added to the box's radius on each axis
        constexpr float kShortCrossSquared = 1e-03f; // an edge along a box axis gives no axis
        constexpr float kShortNormalSquared = 0.01f;
        constexpr float kPushScale = 1.0f;

        // A triangle on an axis. (triangleExtentAlong is the same code, so the
        // call names which by its address.)
        void projectTriangle(const Vec4 *triangle, Vec4 &axis, ee::Float &centreOut, ee::Float &extentOut)
        {
            projectTriangleOnAxis(triangle, &axis, &centreOut, &extentOut);
        }

        // Whether the box in the axis table touches a triangle with this
        // normal: no axis separates them. (The original has this twice, for
        // the two places a face comes from.)
        bool boxTouchesFace(ee::Ptr<RigidBody> body, const Vec4 *triangle, const Vec4 &normal)
        {
            SeparatingAxis *axes = *g_separatingAxes;
            SeparatingAxis &plane = axes[kFaceNormalAxis];
            plane.axis = normal;
            projectBoxOnAxis(body, &plane.axis, &plane.centreA, &plane.radiusA);
            const ee::Float faceDistance = dot(normal, triangle[0]);
            plane.radiusB = ee::Float(0.0f);
            plane.radiusA = plane.radiusA + ee::Float(kBoxTolerance);
            plane.centreB = faceDistance;
            if (intervalsSeparate(plane.centreA, plane.radiusA, plane.centreB, plane.radiusB) != 0)
                return false;

            // The box's axes.
            for (uint32_t i = 0; i < kBoxAxes; ++i)
            {
                SeparatingAxis &entry = axes[i];
                projectTriangle(triangle, entry.axis, entry.centreB, entry.radiusB);
                if (intervalsSeparate(entry.centreA, entry.radiusA, entry.centreB, entry.radiusB) != 0)
                    return false;
            }

            // The crosses of the box's axes with the triangle's edges.
            Vec4 edges[kTriangleEdges];
            edges[0] = triangle[0] - triangle[1];
            edges[1] = triangle[0] - triangle[2];
            edges[2] = triangle[1] - triangle[2];
            for (uint32_t i = kFirstEdgeAxis; i < kEdgeAxisEnd; ++i)
            {
                const uint32_t pair = i - kFirstEdgeAxis;
                SeparatingAxis &entry = axes[i];
                entry.axis = cross(axes[pair % kBoxAxes].axis, edges[pair / kBoxAxes]);
                projectBoxOnAxis(body, &entry.axis, &entry.centreA, &entry.radiusA);
                entry.radiusA = entry.radiusA + ee::Float(kBoxTolerance);
                projectTriangle(triangle, entry.axis, entry.centreB, entry.radiusB);
                if (ee::Float(kShortCrossSquared) < dot(entry.axis, entry.axis) &&
                    intervalsSeparate(entry.centreA, entry.radiusA, entry.centreB, entry.radiusB) != 0)
                    return false;
            }
            return true;
        }
    }

    // findBodyFaceContacts: the box of a body against every contact object's faces.
    // Each face the box truly overlaps, and that the dynamics' callback takes,
    // adds to the point (the box's point on its plane), the normal and the
    // push (along the normal, from the box to the face); each comes out the
    // average. Returns a ContactKind from the contact point's velocity along
    // the normal, 0 if nothing touches.
    int32_t findBodyFaceContacts(ee::Ptr<RigidBody> body, Vec4 *pointOut, Vec4 *normalOut, Vec4 *pushOut)
    {
        ee::Local<Vec4> corner;
        ee::Local<Vec4> velocity;
        ee::Local<Vec4[kTriangleVertices]> triangle;
        ee::Local<Vec4> normal;
        ee::Local<FaceContact> contact;
        ee::Local<ee::Ptr<uint16_t>> vertexIndices;

        uint32_t contacts = 0, worldFacesUsed = 0, face = 0;
        ee::Ptr<HitNode> faceObject; // the object of the face in hand
        *g_contactObject = ee::Ptr<HitNode>();
        *g_faceContactCount = 0;
        *pointOut = kOrigin;
        *normalOut = kOrigin;
        *pushOut = kOrigin;

        // The box's axes and its size along them.
        SeparatingAxis *axes = *g_separatingAxes;
        for (uint32_t i = 0; i < kBoxAxes; ++i)
            axes[i].axis = matrixRow(body->rotation, i);
        for (uint32_t i = 0; i < kBoxAxes; ++i)
        {
            projectBoxOnAxis(body, &axes[i].axis, &axes[i].centreA, &axes[i].radiusA);
            axes[i].radiusA = axes[i].radiusA + ee::Float(kBoxTolerance);
        }

        // What the callback is shown of the face in hand.
        const auto showFace = [&]
        {
            contact->face = face;
            for (uint32_t i = 0; i < kTriangleVertices; ++i)
                contact->vertices[i] = triangle[i];
            contact->normal = normal;
        };

        // A face that touches: the callback's say, then what it adds.
        const auto addFace = [&](uint32_t objectIndex)
        {
            const ContactCallback callback = (*g_currentDynamics)->contactCallback;
            if (callback)
            {
                contact->object = contactObject(objectIndex);
                showFace();
                const int32_t taken = callback(*g_currentDynamics, body, contact);
                *g_contactObject = contactObject(objectIndex);
                if (taken == 0)
                    return;
            }
            ++contacts;
            *normalOut = *normalOut + normal;
            SeparatingAxis &plane = axes[kFaceNormalAxis];
            vec3Negate(&plane.axis.xyz(), &plane.axis.xyz());
            plane.centreA = -plane.centreA;
            const ee::Float distance = plane.centreA + plane.radiusA;
            corner = kOrigin;
            boxClosestPoint(body, &plane.axis, corner, *g_faceContactCount, distance);
            *pointOut = *pointOut + corner;
            ++*g_faceContactCount;
            corner = triangle[0] - corner;
            const ee::Float depth = dot(normal, corner);
            corner = normal * depth;
            *pushOut = *pushOut + corner;
        };

        if (*g_collideWithFaces != 0)
        {
            for (int32_t objectIndex = 0; objectIndex < g_contactObjects->count; ++objectIndex)
            {
                const ee::Ptr<HitNode> object = contactObject(objectIndex);
                const ee::Ptr<ZgePart> part = object->part;
                const ee::Ptr<Node> node = object->node;
                for (uint32_t faceIndex = 0; faceIndex < contactObject(objectIndex)->faceCount; ++faceIndex)
                {
                    face = object->faces[faceIndex].face;
                    faceObject = object;
                    if (worldFacesUsed < kWorldFaceCount)
                    {
                        // Moved to the world already.
                        const WorldFace &worldFace = (*g_worldFaces)[worldFacesUsed];
                        for (uint32_t i = 0; i < kTriangleVertices; ++i)
                            triangle[i] = worldFace.vertices[i];
                        normal = worldFace.normal;
                        ++worldFacesUsed;
                    }
                    else
                    {
                        faceVertices(part, face, vertexIndices);
                        for (uint32_t i = 0; i < kTriangleVertices; ++i)
                        {
                            vertexPosition(part, (*vertexIndices)[i], &triangle[i]);
                            if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
                                transformPoint(&triangle[i], nodeMatrix(node, 1u), &triangle[i]);
                            else
                                triangle[i] = transformed(*nodeMatrix(node, 1u), triangle[i]);
                        }
                        faceNormal(part, face, normal);
                        normal = rotated(*nodeMatrix(node, 1u), normal);
                    }
                    if (game::readable::kIsBmx && body->hasTwoSidedFaces())
                    {
                        // ZGE4: the face turned to the body (the way to it kept on the stack).
                        ee::Local<Vec4> toBody;
                        *toBody = body->position - triangle[0];
                        if (dot(toBody, normal) < ee::Float(0.0f))
                            vec3Negate(&normal->xyz(), &normal->xyz());
                    }
                    if (boxTouchesFace(body, &triangle[0], normal))
                        addFace(objectIndex);
                }
            }
        }

        // The search is over: the callback is told, if it took any, with the
        // last face looked at: ZGE2 and ZGE3 with no object, ZGE4 with that
        // face's.
        const ContactCallback callback = (*g_currentDynamics)->contactCallback;
        if (callback && *g_contactObject)
        {
            contact->object = game::readable::kIsBmx ? faceObject : ee::Ptr<HitNode>();
            showFace();
            callback(*g_currentDynamics, body, contact);
        }
        if (*g_faceContactCount == 0)
            return 0;

        // The averages.
        *pointOut = *pointOut * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(*g_faceContactCount)));
        *pushOut = *pushOut * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(*g_faceContactCount)));
        *normalOut = *normalOut * (ee::Float(1.0f) / ee::toFloat(static_cast<int32_t>(contacts)));
        if (dot(*normalOut, *normalOut) < ee::Float(kShortNormalSquared))
            normalOut->y = ee::Float(-1.0f);
        else if (game::readable::kIsBmx)
            *normalOut = unit(*normalOut); // ZGE4 makes a normal long enough length 1

        // The push is what of it lies along the normal, and is dropped if
        // that is backwards.
        const ee::Float along = dot(*normalOut, *pushOut);
        *pushOut = *normalOut * along;
        pointVelocity(body, pointOut, velocity);
        // (The original works out the pushed point and drops it. Its w stays
        // in VU0 for the next vector.)
        const Vec4 scaledPush = *pushOut * kPushScale;
        const Vec4 pushedPoint = *pointOut + scaledPush;
        static_cast<void>(pushedPoint);
        if (dot(*pushOut, *normalOut) < ee::Float(0.0f))
            *pushOut = kOrigin;
        return classifyFaceNormal(velocity, normalOut);
    }
    READABLE_FUNCTION(findBodyFaceContacts, Verified);
}
