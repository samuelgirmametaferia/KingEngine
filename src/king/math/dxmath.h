#pragma once

#include "types.h"

#include <DirectXMath.h>

namespace king::dx
{

inline DirectX::XMVECTOR Load(const Float3& v) { return DirectX::XMVectorSet(v.x, v.y, v.z, 0.0f); }
inline DirectX::XMVECTOR LoadPoint(const Float3& v) { return DirectX::XMVectorSet(v.x, v.y, v.z, 1.0f); }
inline DirectX::XMVECTOR Load(const Float4& v) { return DirectX::XMVectorSet(v.x, v.y, v.z, v.w); }

inline Float3 StoreFloat3(DirectX::FXMVECTOR v)
{
    Float3 out{};
    DirectX::XMStoreFloat3((DirectX::XMFLOAT3*)&out, v);
    return out;
}

inline Float4 StoreFloat4(DirectX::FXMVECTOR v)
{
    Float4 out{};
    DirectX::XMStoreFloat4((DirectX::XMFLOAT4*)&out, v);
    return out;
}

inline Mat4x4 StoreMat4x4(DirectX::FXMMATRIX m)
{
    Mat4x4 out{};
    DirectX::XMFLOAT4X4 tmp{};
    DirectX::XMStoreFloat4x4(&tmp, m);
    // DirectXMath stores row-major in XMFLOAT4X4.
    const float* p = &tmp._11;
    for (int i = 0; i < 16; ++i) out.m[i] = p[i];
    return out;
}

inline DirectX::XMMATRIX LoadMat4x4(const Mat4x4& m)
{
    DirectX::XMFLOAT4X4 tmp{};
    float* p = &tmp._11;
    for (int i = 0; i < 16; ++i) p[i] = m.m[i];
    return DirectX::XMLoadFloat4x4(&tmp);
}

} // namespace king::dx
