// The constraint solver's set-up for a dynamics object (zge/collision/dynamics.h)
// and the dynamics' file chunks.

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/assets/chunk_reader.h"
#include "zge/debug/profiler.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/dynamics_solver.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kSolverVectorRows = 6;

        // The dynamics file's chunks and what reads each.
        constexpr uint32_t kChunkZdyn = 0x4E59445Au, kReadZdyn = sym::readZdynChunk;
        constexpr uint32_t kChunkDdef = 0x46454444u, kReadDdef = sym::readDdefChunk;
        constexpr uint32_t kChunkDbdy = 0x59444244u, kReadDbdy = sym::readDbdyChunk;
        constexpr uint32_t kChunkDcon = 0x4E4F4344u, kReadDcon = sym::readDconChunk;
        constexpr uint32_t kChunkDbnc = 0x434E4244u, kReadDbnc = sym::readDbncChunk;
        constexpr uint32_t kProfileColour = sym::profileColour, kProfileLabel = sym::g_textDynamicsTime; // "Dynamics Time"

        inline ee::Global<uint32_t, sym::g_dynamicsChunksRegistered> g_dynamicsChunksRegistered;
        inline ee::Global<uint32_t, sym::g_dynamicsProfileBar> g_dynamicsProfileBar;

        constexpr float kFixedMass = 1e+03f;

        // How hard a joint pulls its anchors together (updateDynamics sets them).
        inline ee::Global<ee::Float, sym::g_jointStiffness> g_jointStiffness;
        inline ee::Global<ee::Float, sym::g_jointDamping> g_jointDamping;

        // The joint's matrix takes a point's motion to the body's six: the
        // identity, then the arm's cross product.
        constexpr uint32_t kJointRows = 3, kJointColumns = 6;
    }

    // forEachConnectedBody: the node and those below and after it put in the solver's
    // two orders, the tree walked depth first.
    void forEachConnectedBody(ee::Ptr<Dynamics> dynamics, ee::Ptr<SolverNode> node, ee::Ptr<uint32_t> placed)
    {
        for (; node; node = node->nextSibling)
        {
            ConstraintSystem &system = dynamics->system;
            system.order[*placed] = node;
            system.reversedOrder[system.bodyCount + system.linkCount - (*placed + 1)] = node;
            *placed = *placed + 1;
            forEachConnectedBody(dynamics, node->firstChild, placed);
        }
    }
    READABLE_FUNCTION(forEachConnectedBody, Verified);

    // addConstraintRows: a node's 6 by 6 mass matrix: a body's mass and inertia,
    // or a fixed point's large mass.
    void addConstraintRows(ee::Ptr<SolverNode> node)
    {
        const uint32_t kind = node->kind;
        if (kind != kSolverNodeBody && kind != kSolverNodeFixed)
            return;
        node->mass.columns = 6;
        node->mass.rows = 6;
        clearSolverMatrix(&node->mass);
        if (kind == kSolverNodeBody)
        {
            const ee::Ptr<RigidBody> body = node->body;
            for (uint32_t i = 0; i < 3; ++i)
            {
                node->mass.data[i][i] = body->mass;
                for (uint32_t j = 0; j < 3; ++j)
                    node->mass.data[3 + i][3 + j] = body->worldInertia.m[i][j];
            }
        }
        else
        {
            for (uint32_t i = 0; i < 3; ++i)
            {
                node->mass.data[i][i] = ee::Float(kFixedMass);
                for (uint32_t j = 0; j < 3; ++j)
                    node->mass.data[3 + i][3 + j] = ee::Float(i == j ? kFixedMass : 0.0f);
            }
        }
    }
    READABLE_FUNCTION(addConstraintRows, Verified);

    // addContactRows: a ball joint's matrix on the node below it, and the force
    // that holds it: the anchor's acceleration from the body's solved force,
    // plus the stiffness times where it is and the damping times its speed.
    // A body under a link hands the force, negated, to the link; a link
    // adds it to its own. A link of another kind writes out whatever is on
    // the stack, as the original does.
    void addContactRows(ee::Ptr<SolverNode> node)
    {
        ee::Local<SolverNodeMatrix> matrix;
        ee::Local<Vec4> anchorVelocity;
        ee::Local<Vec4> anchor;
        const bool nodeIsLink = node->isLink != 0;
        const ee::Ptr<DynamicsLink> link = nodeIsLink ? node->link : node->parent->link;
        if (link->kind == 0)
        {
            const ee::Ptr<BodyConstraint> constraint = link->constraint;
            const ee::Ptr<RigidBody> body = nodeIsLink ? node->parent->body : node->body;
            matrix->columns = kJointColumns;
            matrix->rows = kJointRows;
            clearSolverMatrix(matrix);
            for (uint32_t i = 0; i < kJointRows; ++i)
                matrix->data[i][i] = ee::Float(1.0f);
            const Vec4 &anchorInBody = body == constraint->bodyA ? constraint->anchorA : constraint->anchorB;
            const Vec4 arm = rotated(body->rotation, anchorInBody);
            const ee::Float x = arm.x, y = arm.y, z = arm.z;
            const ee::Float negativeZero = -ee::Float(0.0f);
            matrix->data[0][3] = negativeZero;
            matrix->data[0][4] = z;
            matrix->data[0][5] = -y;
            matrix->data[1][3] = -z;
            matrix->data[1][4] = negativeZero;
            matrix->data[1][5] = x;
            matrix->data[2][3] = y;
            matrix->data[2][4] = -x;
            matrix->data[2][5] = negativeZero;

            if (!nodeIsLink)
            {
                // A body: its matrix turned round.
                for (uint32_t row = 0; row < matrix->rows; ++row)
                    for (uint32_t column = 0; column < matrix->columns; ++column)
                        matrix->data[row][column] = -matrix->data[row][column];
                node->force.columns = 1;
                node->force.rows = 6;
                clearSolverMatrix(&node->force);
            }

            // The anchor's acceleration, and where it is.
            const Vec4 acceleration = body->solvedForce * (ee::Float(1.0f) / body->mass);
            const Vec4 angularAcceleration = rotated(body->worldInverseInertia, body->solvedTorque);
            const Vec4 turning = cross(angularAcceleration, arm);
            Vec4 whirling = cross(body->spin, arm);
            whirling = cross(body->spin, whirling);
            const ee::Float accelerationX = acceleration.x + turning.x + whirling.x;
            const ee::Float accelerationY = acceleration.y + turning.y + whirling.y;
            const ee::Float accelerationZ = acceleration.z + turning.z + whirling.z;
            anchor = body->position + arm;
            pointVelocity(body, anchor, anchorVelocity);
            anchor = anchor * *g_jointStiffness;
            anchorVelocity = anchorVelocity * *g_jointDamping;
            const ee::Float forceX = accelerationX + anchor->x + anchorVelocity->x;
            const ee::Float forceY = accelerationY + anchor->y + anchorVelocity->y;
            const ee::Float forceZ = accelerationZ + anchor->z + anchorVelocity->z;

            if (!nodeIsLink)
            {
                const ee::Ptr<SolverNode> parent = node->parent;
                parent->force.data[0][0] = -forceX;
                parent->force.data[1][0] = -forceY;
                parent->force.data[2][0] = -forceZ;
                parent->force.columns = 1;
                parent->force.rows = kJointRows;
            }
            else
            {
                node->force.data[0][0] = node->force.data[0][0] + forceX;
                node->force.data[1][0] = node->force.data[1][0] + forceY;
                node->force.data[2][0] = node->force.data[2][0] + forceZ;
            }
        }
        if (nodeIsLink)
            copySolverMatrix(&node->linkMatrix, matrix);
        else
            transposeSolverMatrix(&node->linkMatrix, matrix);
        transposeSolverMatrix(&node->linkTransposed, matrix);
    }
    READABLE_FUNCTION(addContactRows, Verified);

    // buildConstraintSystem: every node's mass and link matrices, then from the
    // leaves up each node's articulated mass: its own less what its children
    // pass through their links, and its inverse.
    void buildConstraintSystem(ee::Ptr<ConstraintSystem> system)
    {
        ee::Local<SolverNodeMatrix> transposed;
        ee::Local<SolverNodeMatrix> product;
        const int32_t count = static_cast<int32_t>(system->linkCount + system->bodyCount);
        for (int32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<SolverNode> node = system->reversedOrder[index];
            if (node->isLink != 0)
            {
                node->mass.rows = 0;
                node->mass.columns = 0;
            }
            else
                addConstraintRows(node);
            if (node->parent)
                addContactRows(node);
        }
        for (int32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<SolverNode> node = system->reversedOrder[index];
            for (ee::Ptr<SolverNode> child = node->firstChild; child; child = child->nextSibling)
            {
                if (child->mass.rows == 0)
                    continue;
                transposeSolverMatrix(transposed, &child->linkMatrix);
                multiplySolverMatrices(product, &child->mass, &child->linkMatrix);
                multiplySolverMatrices(product, transposed, product);
                if (node->mass.rows == 0)
                {
                    node->mass.rows = product->rows;
                    node->mass.columns = product->columns;
                    clearSolverMatrix(&node->mass);
                }
                subtractSolverMatrices(&node->mass, &node->mass, product);
            }
            invertSolverMatrix3or6(&node->inverted, &node->mass);
            if (node->parent)
                multiplySolverMatrices(&node->linkMatrix, &node->inverted, &node->linkMatrix);
        }
    }
    READABLE_FUNCTION(buildConstraintSystem, Verified);

    // solveConstraintSystem: the forces through the tree: gathered from the leaves up
    // (each child's force passed through its link), then each node's
    // acceleration from the root down.
    void solveConstraintSystem(ee::Ptr<ConstraintSystem> system)
    {
        ee::Local<SolverNodeMatrix> transposed;
        ee::Local<SolverNodeMatrix> product;
        const int32_t count = static_cast<int32_t>(system->linkCount + system->bodyCount);
        for (int32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<SolverNode> node = system->reversedOrder[index];
            for (ee::Ptr<SolverNode> child = node->firstChild; child; child = child->nextSibling)
            {
                transposeSolverMatrix(transposed, &child->linkMatrix);
                multiplySolverMatrices(product, transposed, &child->force);
                if (node->force.rows == 0)
                {
                    node->force.rows = product->rows;
                    node->force.columns = product->columns;
                    clearSolverMatrix(&node->force);
                }
                subtractSolverMatrices(&node->force, &node->force, product);
            }
        }
        for (int32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<SolverNode> node = system->order[index];
            const ee::Ptr<SolverNode> parent = node->parent;
            multiplySolverMatrices(&node->force, &node->inverted, &node->force);
            if (parent)
            {
                multiplySolverMatrices(product, &node->linkMatrix, &parent->force);
                subtractSolverMatrices(&node->force, &node->force, product);
            }
            clearSolverMatrix(&node->solved);
        }
    }
    READABLE_FUNCTION(solveConstraintSystem, Verified);

    // initSolverMatrix: a node's six by one vector, cleared.
    void initSolverMatrix(ee::Ptr<SolverNode> node)
    {
        node->solved.rows = kSolverVectorRows;
        node->solved.columns = 1;
        game::tail(clearSolverMatrix, &node->solved);
    }
    READABLE_FUNCTION(initSolverMatrix, Verified);

    // solveDynamicsIsland: a step of a dynamics object's tree: its solver orders,
    // every node's matrices cleared, the system built and solved; each link's
    // result carried to the bodies on either side of it, and each body's
    // result added to what it is given next step.
    void solveDynamicsIsland(ee::Ptr<Dynamics> dynamics)
    {
        ee::Local<Vec4> linear;
        ee::Local<Vec4> angular;
        ee::Local<uint32_t> placed;
        // The ragdoll's step can come with no dynamics (cov-ragdoll): the
        // recompiled original then reads low RAM and goes on, and so does
        // this (a real PS2 in user mode may fault there).
        ConstraintSystem &system = dynamics[0].system;
        *placed = 0;
        forEachConnectedBody(dynamics, system.root, placed);
        for (uint32_t i = 0; i < system.bodyCount + system.linkCount; ++i)
        {
            const ee::Ptr<SolverNode> node = system.reversedOrder[i];
            clearSolverMatrix(&node->mass);
            clearSolverMatrix(&node->inverted);
            clearSolverMatrix(&node->force);
            clearSolverMatrix(&node->linkMatrix);
            if (node->isLink == 0)
            {
                node->solved.rows = kSolverVectorRows;
                node->solved.columns = 1;
                initSolverMatrix(node);
            }
        }
        buildConstraintSystem(&system);
        solveConstraintSystem(&system);

        // The links, after the bodies in the node array.
        for (uint32_t i = system.bodyCount; i < system.bodyCount + system.linkCount; ++i)
        {
            const ee::Ptr<SolverNode> node = system.nodes + i;
            if (node->solved.rows == 0)
            {
                node->solved.rows = kSolverVectorRows;
                node->solved.columns = 1;
            }
            multiplySolverMatrices(&node->solved, &node->linkTransposed, &node->force);
            addSolverMatrices(&node->parent->solved, &node->parent->solved, &node->solved);
            multiplySolverMatrices(&node->solved, &node->firstChild->linkTransposed, &node->force);
            addSolverMatrices(&node->firstChild->solved, &node->firstChild->solved, &node->solved);
        }

        // (Only x, y and z are set: w stays what the stack held, and nothing
        // reads the w of a force.)
        for (uint32_t i = 0; i < system.bodyCount; ++i)
        {
            const SolverNode &node = system.nodes[i];
            linear->x = node.solved.data[0][0];
            linear->y = node.solved.data[1][0];
            linear->z = node.solved.data[2][0];
            angular->x = node.solved.data[3][0];
            angular->y = node.solved.data[4][0];
            angular->z = node.solved.data[5][0];
            node.body->solvedForce = node.body->solvedForce + linear;
            node.body->solvedTorque = node.body->solvedTorque + angular;
        }
    }
    READABLE_FUNCTION(solveDynamicsIsland, Verified);

    // freeDynamicsDefinitionRecord: a dynamics definition's record freed (the pool's
    // destructor). 1, 0 with none.
    int32_t freeDynamicsDefinitionRecord(ee::Ptr<Dynamics> definition)
    {
        if (!definition)
            return 0;
        freeDynamicsBodies(definition);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeDynamicsDefinitionRecord, Verified);

    // registerDynamicsChunkHandlers: the dynamics file's readers, the profile bar and the
    // definitions' pool (ZGE4 leaves the pool for the first definition to make: loadDynamicsDefinition). Always 1.
    uint32_t registerDynamicsChunkHandlers()
    {
        *g_dynamicsChunksRegistered = 1;
        static constexpr struct { uint32_t tag, reader; } kChunks[] = {
            {kChunkZdyn, kReadZdyn}, {kChunkDdef, kReadDdef}, {kChunkDbdy, kReadDbdy}, {kChunkDcon, kReadDcon}, {kChunkDbnc, kReadDbnc},
        };
        for (const auto &chunk : kChunks)
            addChunkHandler(chunk.tag, chunk.reader);
        *g_dynamicsProfileBar = newProfileBar(0, 0, kProfileColour, kProfileLabel);
        *g_dynamicsDefinitionPool = 0;
        if constexpr (!game::readable::kIsBmx)
            *g_dynamicsDefinitionPool = createPool(kDynamicsBytes, 0, sym::freeDynamicsDefinitionRecord);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerDynamicsChunkHandlers, Verified);
}
