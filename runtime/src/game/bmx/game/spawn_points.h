#pragma once
// BMX XXX's spawn locators (bmx.c, spawn_points.cpp): the level's "Spawn"
// scene points a rider is put at. DM2's line (game/dm2/rider/physics/
// respawn.cpp's locatorMatrix).

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::bmx
{
    // The matrix of the Spawn locator named `name` nearest the point: its
    // rotation and position into `matrix` (1); the identity when the level
    // has none (0).
    int32_t locatorMatrix(const ee::Float *point, uint32_t name, zge::Matrix *matrix);
    // The first Spawn locator of the name moved to the matrix (its position
    // and rotation); with none, a new one added there.
    void setSpawnPoint(const zge::Matrix *matrix, ee::Ptr<const char> name);
    // A new Spawn scene point of the name at the matrix (inferred name).
    void addSpawnPoint(const zge::Matrix *matrix, ee::Ptr<const char> name);
}
