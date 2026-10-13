#include "GPUVisibilityCommon.hlsli"

// Full AABB coverage, expanded by a pixel. Any uncertain projection is visible.
bool ProjectBounds(GPUInstanceData instance, out float2 minimum, out float2 maximum, out float nearest)
{
    minimum = 1; maximum = 0; nearest = 1;
    [unroll] for (uint corner = 0; corner < 8; ++corner)
    {
        float3 signs = float3((corner & 1) ? 1 : -1, (corner & 2) ? 1 : -1, (corner & 4) ? 1 : -1);
        float3 world = instance.BoundsCenter.xyz + signs * instance.BoundsExtents.xyz;
        float4 clip = mul(float4(world, 1), PreviousViewProjection);
        if (!all(isfinite(clip)) || clip.w <= 0.00001f || clip.z <= 0 || clip.z >= clip.w) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        minimum = min(minimum, uv); maximum = max(maximum, uv); nearest = min(nearest, ndc.z);
    }
    minimum -= 1.0f / HiZSize; maximum += 1.0f / HiZSize;
    return all(minimum >= 0) && all(maximum < 1) && all(maximum >= minimum);
}
bool Occluded(float2 minimum, float2 maximum, float nearest)
{
    uint width, height, mipCount;
    uint mip = 0;
    uint2 first, last;
    // Load every texel touched by the rectangle; no holes between corner samples.
    [loop] for (; mip < HiZMips; ++mip)
    {
        gHiZ.GetDimensions(mip, width, height, mipCount);
        first = min((uint2)floor(minimum * float2(width, height)), uint2(width - 1, height - 1));
        last = min((uint2)floor(maximum * float2(width, height)), uint2(width - 1, height - 1));
        if (all(last - first <= 1)) break;
    }
    if (mip >= HiZMips) return false;
    float farthest = 0;
    [loop] for (uint y = first.y; y <= last.y; ++y)
        [loop] for (uint x = first.x; x <= last.x; ++x)
        {
            float depth = gHiZ.Load(int3(x, y, mip));
            if (!isfinite(depth)) return false;
            farthest = max(farthest, depth);
        }
    return nearest > farthest + max(DepthBias, 0);
}
[numthreads(64, 1, 1)]
void Reset(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 16) gStatistics.Store(id.x * 4, 0);
}
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= Slots) return;
    GPUInstanceData instance = gInstances[slot];
    uint4 history = gHistory[slot];
    if (ResetHistory != 0 || history.y != instance.Entity || history.z != instance.MeshIndex || history.w != instance.LODGroupIndex)
        gHistory[slot] = uint4(0xffffffffu, instance.Entity, instance.MeshIndex, instance.LODGroupIndex);
    uint4 result = uint4(slot, instance.MeshIndex, 0, 0);
    if ((instance.Flags & 5u) != 5u || instance.MeshIndex >= MeshCount)
    { gResult[slot] = result; return; }
    if (gGeometry[instance.MeshIndex].w == 0)
    { gResult[slot] = result; return; }
    uint unused;
    gStatistics.InterlockedAdd(0, 1, unused);
    if (gFrustum[slot] == 0)
    {
        result.w = 2;
        gStatistics.InterlockedAdd(4, 1, unused);
    }
    else
    {
        result.w = 1;
        gStatistics.InterlockedAdd(8, 1, unused);
        // Dynamic geometry and moving transforms never use stale depth.
        bool stationary = all(instance.World[0] == instance.PreviousWorld[0]) && all(instance.World[1] == instance.PreviousWorld[1]) &&
            all(instance.World[2] == instance.PreviousWorld[2]) && all(instance.World[3] == instance.PreviousWorld[3]);
        float2 minimum, maximum; float nearest;
        if (EnableOcclusion != 0 && stationary && (instance.Flags & 8u) == 0 && ProjectBounds(instance, minimum, maximum, nearest))
        {
            gStatistics.InterlockedAdd(12, 1, unused);
            if (Occluded(minimum, maximum, nearest))
            { result.w = 3; gStatistics.InterlockedAdd(16, 1, unused); }
        }
    }
    gResult[slot] = result;
}
