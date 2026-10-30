#include "pch.h"
#include "reflectionprobes.h"
#include "componentmanager.h"
#include "texturemanager.h"

#include <cmath>
#include <unordered_map>

namespace ReflectionProbes
{
    namespace
    {
        std::vector<Probe> g_Probes;
    }

    void Clear()
    {
        g_Probes.clear();
    }

    void Add(const Probe& probe)
    {
        g_Probes.push_back(probe);
    }

    uint32_t Count()
    {
        return static_cast<uint32_t>(g_Probes.size());
    }

    const std::vector<Probe>& All()
    {
        return g_Probes;
    }

    void SyncFromScene()
    {
        Clear();
        static std::unordered_map<std::string, int> textureCache;
        ComponentManager::ForEach<ReflectionProbeComponent, TransformComponent>(
            [&](EntityID entity, ReflectionProbeComponent& probeComponent, TransformComponent& transform) {
                (void)entity;
                if (!probeComponent.IsActive || probeComponent.EnvironmentTexturePath.empty())
                {
                    return;
                }
                auto found = textureCache.find(probeComponent.EnvironmentTexturePath);
                if (found == textureCache.end())
                {
                    found = textureCache
                                .emplace(probeComponent.EnvironmentTexturePath,
                                         TextureManager::LoadTexture(probeComponent.EnvironmentTexturePath.c_str()))
                                .first;
                }
                Probe probe{};
                probe.Center[0] = transform.Position.x;
                probe.Center[1] = transform.Position.y;
                probe.Center[2] = transform.Position.z;
                probe.HalfExtents[0] = (std::max)(probeComponent.InfluenceSize.x, 0.1f) * 0.5f;
                probe.HalfExtents[1] = (std::max)(probeComponent.InfluenceSize.y, 0.1f) * 0.5f;
                probe.HalfExtents[2] = (std::max)(probeComponent.InfluenceSize.z, 0.1f) * 0.5f;
                probe.EnvironmentSrvIndex = found->second;
                probe.Priority = probeComponent.Priority;
                Add(probe);
            });
    }

    int SelectEnvironmentSrvIndex(const float cameraPosition[3])
    {
        int bestSrv = -1;
        int bestPriority = 0;
        float bestDistanceSq = 0.0f;
        for (const Probe& probe : g_Probes)
        {
            if (probe.EnvironmentSrvIndex < 0)
            {
                continue;
            }
            float distanceSq = 0.0f;
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = cameraPosition[axis] - probe.Center[axis];
                if (std::fabs(delta) > probe.HalfExtents[axis])
                {
                    inside = false;
                    break;
                }
                distanceSq += delta * delta;
            }
            if (!inside)
            {
                continue;
            }
            if (bestSrv < 0 || probe.Priority > bestPriority ||
                (probe.Priority == bestPriority && distanceSq < bestDistanceSq))
            {
                bestSrv = probe.EnvironmentSrvIndex;
                bestPriority = probe.Priority;
                bestDistanceSq = distanceSq;
            }
        }
        return bestSrv;
    }
} // namespace ReflectionProbes
