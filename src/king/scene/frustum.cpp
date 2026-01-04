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
    // m is row-major, but our math/shaders use ROW-VECTOR convention:
    //   clip = mul(worldPos, viewProj)
    // In that convention, frustum planes are extracted from the COLUMNS of the
    // view-projection matrix (equivalent to extracting from ROWS of transpose).
    //
    // Also: D3D depth range is z in [0, 1], so the near/far inequalities are:
    //   0 <= z <= w
    // With row-vectors, z = dot(pos, col2), w = dot(pos, col3)
    //   near plane: col2
    //   far  plane: col3 - col2

    const float* a = m.m;

    auto c0 = [&](int r) { return a[0 + r * 4]; };
    auto c1 = [&](int r) { return a[1 + r * 4]; };
    auto c2 = [&](int r) { return a[2 + r * 4]; };
    auto c3 = [&](int r) { return a[3 + r * 4]; };

    Frustum f{};

    // left   = c3 + c0
    f.planes[0] = NormalizePlane(c3(0) + c0(0), c3(1) + c0(1), c3(2) + c0(2), c3(3) + c0(3));
    // right  = c3 - c0
    f.planes[1] = NormalizePlane(c3(0) - c0(0), c3(1) - c0(1), c3(2) - c0(2), c3(3) - c0(3));
    // bottom = c3 + c1
    f.planes[2] = NormalizePlane(c3(0) + c1(0), c3(1) + c1(1), c3(2) + c1(2), c3(3) + c1(3));
    // top    = c3 - c1
    f.planes[3] = NormalizePlane(c3(0) - c1(0), c3(1) - c1(1), c3(2) - c1(2), c3(3) - c1(3));
    // near   = c2  (D3D z>=0)
    f.planes[4] = NormalizePlane(c2(0), c2(1), c2(2), c2(3));
    // far    = c3 - c2 (D3D z<=w)
    f.planes[5] = NormalizePlane(c3(0) - c2(0), c3(1) - c2(1), c3(2) - c2(2), c3(3) - c2(3));

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
