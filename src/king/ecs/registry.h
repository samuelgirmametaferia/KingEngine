#pragma once

#include "components.h"
#include "sparse_set.h"

#include "../thread_config.h"

#include <cstdint>
#include <vector>

namespace king
{

class Registry
{
public:
    Registry()
        : mWorkerThreadsCached(king::GetThreadConfig().ecsWorkerThreads)
    {
    }

    Entity CreateEntity()
    {
        Entity e = ++mNext;
        mAlive.push_back(e);
        return e;
    }

    void DestroyEntity(Entity e)
    {
        // Remove components
        transforms.Remove(e);
        meshes.Remove(e);
        renderers.Remove(e);
        cameras.Remove(e);
        lights.Remove(e);

        // Remove from alive list (linear; fine for now)
        for (size_t i = 0; i < mAlive.size(); ++i)
        {
            if (mAlive[i] == e)
            {
                mAlive[i] = mAlive.back();
                mAlive.pop_back();
                break;
            }
        }
    }

    const std::vector<Entity>& Alive() const { return mAlive; }

    uint32_t WorkerThreads() const { return mWorkerThreadsCached; }

    void SetWorkerThreads(uint32_t threads) { mWorkerThreadsCached = threads; }

    SparseSet<Transform> transforms;
    SparseSet<Mesh> meshes;
    SparseSet<MeshRenderer> renderers;
    SparseSet<CameraComponent> cameras;
    SparseSet<Light> lights;

private:
    Entity mNext = 0;
    std::vector<Entity> mAlive;
    uint32_t mWorkerThreadsCached = 0;
};

} // namespace king
