#pragma once

// The arithmetic of the tooling's Python converters, which the map
// converter reproduces so its files are theirs byte for byte: a sum of
// floats is compensated (CPython's sum(), Neumaier's algorithm, since
// Python 3.12) and a distance is CPython's math.dist (a scaled sum of
// squares with a correction, within one unit in the last place). Every other
// operation is IEEE double arithmetic in the same order, built without FMA
// contraction (runtime/CMakeLists.txt).

#include "collection/levels/geometry.h"

#include <initializer_list>
#include <vector>

namespace zx::app::levels
{
    double sumOf(std::initializer_list<double> values);
    double sumOf(const std::vector<double> &values);
    // The Euclidean distance between two points.
    double distanceOf(const Vec3 &p, const Vec3 &q);
}
