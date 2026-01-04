#pragma once

#include "entity.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace king
{

// Simple sparse-set style container: stable dense storage + entity->index map.
// Not the fastest possible ECS, but a solid, clear foundation.
template <typename T>
class SparseSet
{
public:
    bool Has(Entity e) const
    {
        return mIndex.find(e) != mIndex.end();
    }

    T* TryGet(Entity e)
    {
        auto it = mIndex.find(e);
        if (it == mIndex.end()) return nullptr;
        return &mData[it->second];
    }

    const T* TryGet(Entity e) const
    {
        auto it = mIndex.find(e);
        if (it == mIndex.end()) return nullptr;
        return &mData[it->second];
    }

    template <typename... Args>
    T& Emplace(Entity e, Args&&... args)
    {
        Remove(e);
        const size_t idx = mData.size();
        mEntities.push_back(e);
        mData.emplace_back(std::forward<Args>(args)...);
        mIndex[e] = idx;
        return mData.back();
    }

    void Remove(Entity e)
    {
        auto it = mIndex.find(e);
        if (it == mIndex.end()) return;

        size_t idx = it->second;
        size_t last = mData.size() - 1;

        if (idx != last)
        {
            mData[idx] = std::move(mData[last]);
            mEntities[idx] = mEntities[last];
            mIndex[mEntities[idx]] = idx;
        }

        mData.pop_back();
        mEntities.pop_back();
        mIndex.erase(it);
    }

    void Clear()
    {
        mData.clear();
        mEntities.clear();
        mIndex.clear();
    }

    size_t Size() const { return mData.size(); }
    const std::vector<Entity>& Entities() const { return mEntities; }
    std::vector<T>& Data() { return mData; }
    const std::vector<T>& Data() const { return mData; }

private:
    std::vector<Entity> mEntities;
    std::vector<T> mData;
    std::unordered_map<Entity, size_t> mIndex;
};

} // namespace king
