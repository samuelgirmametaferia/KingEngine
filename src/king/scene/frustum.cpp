#include "frustum.h"

#include <cmath>

namespace king
{

static Plane NormalizePlane(float a, float b, float c, float d)
{
    const float len = std::sqrt(a * a + b * b + c * c);
    Plane p{};
    if (len > 0.0f)
    {
        p.n.x = a / len;
        p.n.y = b / len;
        p.n.z = c / len;
        p.d = d / len;
    }
    return p;
}

Frustum Frustum::FromViewProjection(const Mat4x4& m)
{
    // m is row-major.
    // Let M be the 4x4, rows r0..r3.
    // Planes (row-major extraction):
    // left  = r3 + r0
    // right = r3 - r0
    // bottom= r3 + r1
    // top   = r3 - r1
    // near  = r3 + r2
    // far   = r3 - r2

    const float* a = m.m;

    auto r0 = [&](int c) { return a[c + 0]; };
    auto r1 = [&](int c) { return a[c + 4]; };
    auto r2 = [&](int c) { return a[c + 8]; };
    auto r3 = [&](int c) { return a[c + 12]; };

    Frustum f{};

    // left
    f.planes[0] = NormalizePlane(r3(0) + r0(0), r3(1) + r0(1), r3(2) + r0(2), r3(3) + r0(3));
    // right
    f.planes[1] = NormalizePlane(r3(0) - r0(0), r3(1) - r0(1), r3(2) - r0(2), r3(3) - r0(3));
    // bottom
    f.planes[2] = NormalizePlane(r3(0) + r1(0), r3(1) + r1(1), r3(2) + r1(2), r3(3) + r1(3));
    // top
    f.planes[3] = NormalizePlane(r3(0) - r1(0), r3(1) - r1(1), r3(2) - r1(2), r3(3) - r1(3));
    // near
    f.planes[4] = NormalizePlane(r3(0) + r2(0), r3(1) + r2(1), r3(2) + r2(2), r3(3) + r2(3));
    // far
    f.planes[5] = NormalizePlane(r3(0) - r2(0), r3(1) - r2(1), r3(2) - r2(2), r3(3) - r2(3));

    return f;
}

static float PlaneDistance(const Plane& p, const Float3& x)
{
    return p.n.x * x.x + p.n.y * x.y + p.n.z * x.z + p.d;
}

bool Frustum::Intersects(const Sphere& s) const
{
    for (const auto& p : planes)
    {
        if (PlaneDistance(p, s.center) < -s.radius)
            return false;
    }
    return true;
}

bool Frustum::Intersects(const AABB& b) const
{
    for (const auto& p : planes)
    {
        // Pick the most positive vertex (support point) w.r.t plane normal.
        Float3 v{};
        v.x = (p.n.x >= 0.0f) ? b.max.x : b.min.x;
        v.y = (p.n.y >= 0.0f) ? b.max.y : b.min.y;
        v.z = (p.n.z >= 0.0f) ? b.max.z : b.min.z;

        if (PlaneDistance(p, v) < 0.0f)
            return false;
    }
    return true;
}

} // namespace king
