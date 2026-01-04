#include "camera_system.h"

namespace king::systems
{

bool CameraSystem::UpdatePrimaryCamera(Scene& scene, Frustum& outFrustum, Mat4x4& outViewProj)
{
    for (auto e : scene.reg.cameras.Entities())
    {
        auto* cc = scene.reg.cameras.TryGet(e);
        auto* t = scene.reg.transforms.TryGet(e);
        if (!cc || !t || !cc->primary)
            continue;

        // Keep camera position in sync with transform (minimal sample behavior).
        cc->camera.SetPosition(t->position);

        outViewProj = cc->camera.ViewProjectionMatrix();
        outFrustum = Frustum::FromViewProjection(outViewProj);
        return true;
    }

    outViewProj = {};
    outFrustum = {};
    return false;
}

void CameraSystem::OnResize(Scene& scene, float aspect)
{
    for (auto e : scene.reg.cameras.Entities())
    {
        auto* cc = scene.reg.cameras.TryGet(e);
        if (!cc || !cc->primary)
            continue;

        PerspectiveParams p;
        p.aspect = aspect;
        cc->camera.SetPerspective(p);
        break;
    }
}

} // namespace king::systems
