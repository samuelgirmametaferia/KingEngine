#pragma once

#include "../math/types.h"
#include "../render/material.h"
#include "../scene/camera.h"
#include "entity.h"

#include <d3d11.h>

#include <string>
#include <vector>

namespace king
{

struct Transform
{
    Float3 position{ 0, 0, 0 };
    Float4 rotation{ 0, 0, 0, 1 }; // quaternion
    Float3 scale{ 1, 1, 1 };

    Entity parent = kInvalidEntity;
};

// For the sample, a mesh is just a list of vertices uploaded to D3D11.
struct VertexPN
{
    float x, y, z;
    float nx, ny, nz;
};

struct Mesh
{
    std::vector<VertexPN> vertices;
    std::vector<uint16_t> indices;
    ID3D11Buffer* vb = nullptr; // owned by renderer; released when destroying scene/mesh
    ID3D11Buffer* ib = nullptr; // owned by renderer; released when destroying scene/mesh

    // Simple bounds for culling (optional)
    Float3 boundsCenter{ 0, 0, 0 };
    float boundsRadius = 1.0f;
};

struct MeshRenderer
{
    Entity mesh = kInvalidEntity;
    PbrMaterial material;

    // Light grouping: renderable receives only lights whose mask overlaps this.
    uint32_t lightMask = 0xFFFFFFFFu;

    // Shadow participation.
    bool castsShadows = true;
    bool receivesShadows = true;
};

struct CameraComponent
{
    Camera camera;
    bool primary = false;
};

enum class LightType : uint8_t
{
    Directional,
    Point,
    Spot,
};

struct Light
{
    LightType type = LightType::Directional;
    Float3 color{ 1, 1, 1 };
    float intensity = 1.0f;

    // Light grouping: light affects only renderables whose mask overlaps this.
    uint32_t groupMask = 0xFFFFFFFFu;

    bool castsShadows = false;

    // For now, store direction explicitly (world space, normalized).
    // Later we can derive from Transform rotation.
    Float3 direction{ 0.3f, -1.0f, 0.2f };

    // Point/spot range (world units). Ignored for directional.
    float range = 10.0f;

    // Spot cone angles (cosines). Ignored unless type == Spot.
    float innerConeCos = 0.90f;
    float outerConeCos = 0.80f;

    // Directional: use rotation of Transform for direction.
    // Point/Spot: use position of Transform.
};

} // namespace king
