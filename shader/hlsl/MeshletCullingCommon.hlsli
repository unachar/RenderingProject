#include "MeshletCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances : register(t0);
StructuredBuffer<uint4> Objects : register(t1);
StructuredBuffer<uint4> Ranges : register(t2);
StructuredBuffer<ClusterRecord> Clusters : register(t3);
StructuredBuffer<MeshletBounds> Bounds : register(t4);
StructuredBuffer<MeshletRecord> Meshlets : register(t5);
ByteAddressBuffer ObjectCount : register(t6);
ByteAddressBuffer OriginalCommands : register(t7);
ByteAddressBuffer Materials : register(t8);
RWStructuredBuffer<uint4> Candidates : register(u0); // インスタンス、メッシュレット、理由、クラスター。
RWStructuredBuffer<uint2> Visible : register(u1);
RWByteAddressBuffer Statistics : register(u2);
RWByteAddressBuffer Arguments : register(u3);
RWByteAddressBuffer FallbackCommands : register(u4);
Texture2D<float> HiZ : register(t9);
StructuredBuffer<uint> NativeSuccess : register(t10);

cbuffer CullParameters : register(b0)
{
    row_major float4x4 VP;
    row_major float4x4 PreviousVP;
    float4 Camera;
    uint Capacity, ObjectCapacity, MeshCount, MaterialCount;
    uint EnableFrustum, EnableCone, EnableOcclusion, EnableClusters;
    float Bias;
    float2 HiZSize;
    uint HiZMips;
};

void Stat(uint offset, uint value = 1)
{
    uint unused;
    Statistics.InterlockedAdd(offset, value, unused);
}

float4 SphereWorld(MeshletBounds bounds, GPUInstanceData instance)
{
    float3 center = mul(float4(bounds.Center, 1), instance.World).xyz;
    // フロベニウスノルムは、せん断があっても最大特異値の上限になる。
    float scale =
        sqrt(dot(instance.World[0].xyz, instance.World[0].xyz) + dot(instance.World[1].xyz, instance.World[1].xyz) +
             dot(instance.World[2].xyz, instance.World[2].xyz));
    return float4(center, bounds.Radius * scale);
}

bool FrustumCull(float4 sphere)
{
    float4 c0 = float4(VP[0][0], VP[1][0], VP[2][0], VP[3][0]);
    float4 c1 = float4(VP[0][1], VP[1][1], VP[2][1], VP[3][1]);
    float4 c2 = float4(VP[0][2], VP[1][2], VP[2][2], VP[3][2]);
    float4 c3 = float4(VP[0][3], VP[1][3], VP[2][3], VP[3][3]);
    float4 planes[6] = {c3 + c0, c3 - c0, c3 + c1, c3 - c1, c2, c3 - c2};
    [unroll] for (uint i = 0; i < 6; ++i)
    {
        if (dot(float4(sphere.xyz, 1), planes[i]) < -sphere.w * length(planes[i].xyz))
        {
            return true;
        }
    }
    return false;
}

bool ConeCull(MeshletBounds bounds, float4 sphere, GPUInstanceData instance)
{
    if (bounds.ConeCutoff >= 1 || instance.MaterialIndex >= MaterialCount ||
        (Materials.Load(instance.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 40) & 4u) != 0)
    {
        return false;
    }
    float3 a = instance.World[0].xyz;
    float3 b = instance.World[1].xyz;
    float3 c = instance.World[2].xyz;
    float3 lengths = float3(length(a), length(b), length(c));
    if (min(lengths.x, min(lengths.y, lengths.z)) < 1e-6 || dot(cross(a, b), c) <= 0)
    {
        return false;
    }
    a /= lengths.x;
    b /= lengths.y;
    c /= lengths.z;
    if (max(lengths.x, max(lengths.y, lengths.z)) - min(lengths.x, min(lengths.y, lengths.z)) > 1e-4 * lengths.x ||
        abs(dot(a, b)) + abs(dot(a, c)) + abs(dot(b, c)) > 1e-4)
    {
        return false;
    }
    float3 delta = sphere.xyz - Camera.xyz;
    float distance = length(delta);
    if (distance <= sphere.w + 1e-5)
    {
        return false;
    }
    float3 axis = normalize(mul(bounds.ConeAxis, (float3x3)instance.World));
    // 境界球で、全三角形にわたる透視投影の変化を考慮する。
    return dot(axis, delta / distance) > bounds.ConeCutoff + sphere.w / distance + 1e-5;
}

bool OcclusionCull(float4 sphere, GPUInstanceData instance)
{
    if (EnableOcclusion == 0 || (instance.Flags & 8u) != 0)
    {
        return false;
    }
    if (!all(instance.World[0] == instance.PreviousWorld[0]) || !all(instance.World[1] == instance.PreviousWorld[1]) ||
        !all(instance.World[2] == instance.PreviousWorld[2]) || !all(instance.World[3] == instance.PreviousWorld[3]))
    {
        return false;
    }
    float2 minimum = 1;
    float2 maximum = 0;
    float nearest = 1;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        float3 sign = float3((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1);
        float4 clip = mul(float4(sphere.xyz + sign * sphere.w, 1), PreviousVP);
        if (!all(isfinite(clip)) || clip.w <= 1e-5 || clip.z <= 0 || clip.z >= clip.w)
        {
            return false;
        }
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        minimum = min(minimum, uv);
        maximum = max(maximum, uv);
        nearest = min(nearest, ndc.z);
    }
    minimum -= 1 / HiZSize;
    maximum += 1 / HiZSize;
    if (any(minimum < 0) || any(maximum >= 1) || any(maximum < minimum))
    {
        return false;
    }
    uint width, height, mips, mip;
    uint2 first = 0;
    uint2 last = 0;
    [loop] for (mip = 0; mip < HiZMips; ++mip)
    {
        HiZ.GetDimensions(mip, width, height, mips);
        first = min((uint2)floor(minimum * float2(width, height)), uint2(width - 1, height - 1));
        last = min((uint2)floor(maximum * float2(width, height)), uint2(width - 1, height - 1));
        if (all(last - first <= 1))
        {
            break;
        }
    }
    if (mip == HiZMips)
    {
        return false;
    }
    float farthest = 0;
    [loop] for (uint y = first.y; y <= last.y; ++y)
    {
        [loop] for (uint x = first.x; x <= last.x; ++x)
        {
            float depth = HiZ.Load(int3(x, y, mip));
            if (!isfinite(depth))
            {
                return false;
            }
            farthest = max(farthest, depth);
        }
    }
    return nearest > farthest + max(Bias, 0);
}
