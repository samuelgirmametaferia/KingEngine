#pragma once

#include "../ecs/scene.h"
#include "../scene/frustum.h"

namespace king::systems
{

struct CameraSystem
{
    // Updates the primary camera view-projection and frustum.
    // Returns true if a primary camera existed.
    static bool UpdatePrimaryCamera(Scene& scene, Frustum& outFrustum, Mat4x4& outViewProj);

    // Updates the aspect ratio for the primary camera.
    static void OnResize(Scene& scene, float aspect);
};

} // namespace king::systems
