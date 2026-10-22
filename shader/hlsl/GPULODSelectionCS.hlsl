#include "GPUVisibilityCommon.hlsli"
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    if (slot >= Slots)
    {
        return;
    }
    uint4 result = gResult[slot];
    if (result.w != 1)
    {
        return;
    }
    GPUInstanceData instance = gInstances[slot];
    uint selected = 0;
    bool native = NativeHierarchy != 0 && (instance.Flags & 16u) != 0;
    if (EnableLOD != 0 && !native && instance.LODGroupIndex < GroupCount)
    {
        GPULODGroup group = gGroups[instance.LODGroupIndex];
        uint count = min(group.Count, 8u);
        if (count > 0 && group.Offset < LevelCount && count <= LevelCount - group.Offset)
        {
            float4 center = mul(float4(instance.BoundsCenter.xyz, 1), View);
            float radius = length(instance.BoundsExtents.xyz);
            // 近クリップ面と交差する場合は、常に詳細な形状を維持する。
            float4 nearest = mul(float4(center.xy, center.z - radius, 1), Projection);
            bool nearCamera = !all(isfinite(nearest)) || nearest.z >= nearest.w || nearest.w <= 0;
            float size = radius * abs(Projection[1][1]) / max(center.z - radius, 0.00001f);
            if (!isfinite(size))
            {
                float distance = length(center.xyz);
                if (distance < instance.LodDistances.x)
                {
                    selected = 0;
                }
                else if (distance < instance.LodDistances.y)
                {
                    selected = 1;
                }
                else
                {
                    selected = 2;
                }
                selected = min(selected, count - 1);
            }
            else if (!nearCamera)
            {
                size *= exp2(-LODBias);
                selected = count - 1;
                [loop] for (uint level = 0; level < count; ++level)
                {
                    if (size >= gLevels[group.Offset + level].Threshold)
                    {
                        selected = level;
                        break;
                    }
                }
                uint previous = gHistory[slot].x;
                if (previous < count)
                {
                    selected = previous;
                    [unroll] for (uint step = 0; step < 8; ++step)
                    {
                        if (selected + 1 >= count)
                        {
                            break;
                        }
                        GPULODLevel old = gLevels[group.Offset + selected];
                        if (size >= old.Threshold * (1 - old.Hysteresis))
                        {
                            break;
                        }
                        ++selected;
                    }
                    [unroll] for (uint refine = 0; refine < 8; ++refine)
                    {
                        if (selected == 0)
                        {
                            break;
                        }
                        GPULODLevel next = gLevels[group.Offset + selected - 1];
                        if (size < next.Threshold * (1 + next.Hysteresis))
                        {
                            break;
                        }
                        --selected;
                    }
                }
            }
            if (ForceLOD != 0xffffffffu)
            {
                selected = min(ForceLOD, count - 1);
            }
            [unroll] for (uint fallback = 0; fallback < 8; ++fallback)
            {
                if (fallback > selected)
                {
                    break;
                }
                uint candidate = selected - fallback;
                uint mesh = gLevels[group.Offset + candidate].MeshIndex;
                // FXC の HLSL の論理式は短絡評価されるとは限らない。ルート SRV には
                // 記述子の長さがないため、読み取り前に InvalidID を除外する。
                if (mesh >= MeshCount)
                {
                    continue;
                }
                uint4 geometry = gGeometry[mesh];
                if (geometry.w != 0 && geometry.y != 0)
                {
                    result.y = mesh;
                    selected = (uint)candidate;
                    break;
                }
                if (candidate == 0)
                {
                    selected = 0; // 検証済みの元の Mesh を保持する。
                }
            }
        }
    }
    result.z = selected;
    gResult[slot] = result;
    gHistory[slot].x = selected;
    uint unused;
    gStatistics.InterlockedAdd(20, 1, unused);
    gStatistics.InterlockedAdd(24, gGeometry[result.y].y / 3, unused);
    gStatistics.InterlockedAdd(28, gGeometry[result.y].y, unused);
    gStatistics.InterlockedAdd(32 + selected * 4, 1, unused);
}
