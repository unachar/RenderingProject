#include "MeshletCullingCommon.hlsli"
groupshared uint Allocation;
groupshared uint Accepted;
[numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint object = group.x + group.y * 65535u;
    if (object >= min(ObjectCount.Load(0), ObjectCapacity))
    {
        return;
    }
    uint commandOffset = object * 24;
    if (lane == 0)
    {
        Accepted = 0;
        FallbackCommands.Store4(commandOffset, OriginalCommands.Load4(commandOffset));
        FallbackCommands.Store2(commandOffset + 16, OriginalCommands.Load2(commandOffset + 16));
        if (NativeSuccess[object] != 0)
        {
            FallbackCommands.Store(commandOffset + 4, 0);
        }
    }
    uint4 selected = Objects[object];
    uint4 range = selected.y < MeshCount ? Ranges[selected.y] : uint4(0, 0, 0, 0);
    if (lane == 0 && NativeSuccess[object] == 0 && range.y != 0 && range.w != 0)
    {
        uint first = Statistics.Load(0), observed;
        [allow_uav_condition] while (true)
        {
            if (range.y > Capacity || first > Capacity - range.y)
            {
                Stat(36);
                break;
            }
            Statistics.InterlockedCompareExchange(0, first, first + range.y, observed);
            if (first == observed)
            {
                Accepted = 1;
                Allocation = first;
                break;
            }
            first = observed;
        }
        // オブジェクト全体の割り当てに成功した。Hi-Z 用の元のコマンドは保持し、
        // メインの不透明フォールバックリストだけがこのオブジェクトを飛ばす。
        if (Accepted != 0)
        {
            FallbackCommands.Store(commandOffset + 4, 0);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (Accepted == 0)
    {
        return;
    }
    GPUInstanceData instance = Instances[selected.x];
    [loop] for (uint clusterIndex = lane; clusterIndex < range.w; clusterIndex += 64)
    {
        ClusterRecord cluster = Clusters[range.z + clusterIndex];
        Stat(8);
        float4 sphere = SphereWorld(cluster.Bounds, instance);
        bool culled = EnableClusters != 0 && ((EnableFrustum != 0 && FrustumCull(sphere)) ||
                                              (EnableCone != 0 && ConeCull(cluster.Bounds, sphere, instance)) ||
                                              OcclusionCull(sphere, instance));
        if (!culled)
        {
            Stat(12);
            Stat(44, cluster.Count);
        }
        else
        {
            Stat(40, cluster.Count);
        }
        [loop] for (uint child = 0; child < cluster.Count; ++child)
        {
            uint meshlet = cluster.Offset + child;
            // カリング済みクラスターは子を調べず、番兵値を初期化する。
            Candidates[Allocation + meshlet - range.x] =
                uint4(selected.x, meshlet, culled ? 2u : 0u, range.z + clusterIndex);
        }
    }
}
