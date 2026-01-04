#pragma once

#include "../math/types.h"

#include <cstdint>

namespace king
{

// Plane: ax + by + cz + d = 0
struct Plane
{
    Float3 n{};
    float d = 0.0f;
};

struct AABB
{
    Float3 min{};
    Float3 max{};
};

struct Sphere
{
    Float3 center{};
    float radius = 0.0f;
};

// View frustum extracted from a view-projection matrix.
class Frustum
{
public:
    // Planes order: left, right, bottom, top, near, far
    Plane planes[6]{};

    static Frustum FromViewProjection(const Mat4x4& viewProjection);

    bool Intersects(const Sphere& s) const;
    bool Intersects(const AABB& b) const;
};

} // namespace king
