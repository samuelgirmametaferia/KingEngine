#pragma once

#include <cstdint>

namespace king
{

struct Float2
{
    float x = 0, y = 0;
};

struct Float3
{
    float x = 0, y = 0, z = 0;
};

struct Float4
{
    float x = 0, y = 0, z = 0, w = 0;
};

// Row-major 4x4 matrix (friendly for serialization/bindings).
// When sending to HLSL (column-major by default), transpose as needed.
struct Mat4x4
{
    float m[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
};

} // namespace king
