#pragma once
#include "gpuscenetypes.h"
#include <unordered_map>
#include <vector>

// ECS mutation notifications. No D3D or component dependencies in this header.
namespace GpuSceneChanges
{
struct PendingChanges
{
    unordered_map<uint32_t, GpuSceneDirtyFlags> Changed;
    vector<uint32_t> Destroyed;
    bool ResetScene = false;
};
inline PendingChanges &Pending()
{
    static PendingChanges changes;
    return changes;
}
inline void Notify(uint32_t entity, GpuSceneDirtyFlags flags = GpuSceneDirtyFlags::All)
{
    if (entity != UINT32_MAX)
    {
        Pending().Changed[entity] = Pending().Changed[entity] | flags;
    }
}
inline void Destroy(uint32_t entity)
{
    Pending().Destroyed.push_back(entity);
    Pending().Changed.erase(entity);
}
inline void Reset()
{
    Pending() = {};
    Pending().ResetScene = true;
}
} // namespace GpuSceneChanges
