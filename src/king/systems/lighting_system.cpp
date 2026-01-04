#include "lighting_system.h"

namespace king::systems
{

void LightingSystem::EnsureDefaultSun(Scene& scene)
{
    Light tmp{};
    if (GetPrimaryDirectionalLight(scene, tmp))
        return;

    Entity e = scene.reg.CreateEntity();
    scene.reg.transforms.Emplace(e);
    auto& l = scene.reg.lights.Emplace(e);
    l.type = LightType::Directional;
    l.color = { 1, 1, 1 };
    l.intensity = 2.0f;
    l.direction = { 0.35f, -1.0f, 0.25f };
    l.castsShadows = true;
}

bool LightingSystem::GetPrimaryDirectionalLight(const Scene& scene, Light& outLight)
{
    for (auto e : scene.reg.lights.Entities())
    {
        auto* l = scene.reg.lights.TryGet(e);
        if (l && l->type == LightType::Directional)
        {
            outLight = *l;
            return true;
        }
    }

    outLight = {};
    return false;
}

} // namespace king::systems
