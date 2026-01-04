#pragma once

#include "../ecs/scene.h"

namespace king::systems
{

struct LightingSystem
{
    // Ensures a directional light exists ("sun"). Creates one if missing.
    static void EnsureDefaultSun(Scene& scene);

    // Selects the first directional light in the scene.
    // Returns true if one was found.
    static bool GetPrimaryDirectionalLight(const Scene& scene, Light& outLight);
};

} // namespace king::systems
