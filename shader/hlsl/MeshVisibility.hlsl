#define SHADER_3D
#include "common.hlsl"

struct StaticVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Diffuse;
};

StructuredBuffer<StaticVertex> MeshVertices : register(t20);
ByteAddressBuffer MeshIndices : register(t21);
Texture2D<float> PreviousHiZ : register(t22);
Texture2D<float> CurrentHiZ : register(t23);
SamplerState HiZSampler : register(s2);

cbuffer MeshDispatchConstants : register(b4)
{
    uint MeshIndexCount;
    uint MeshIndexIs16Bit;
    uint OcclusionPhase;
    uint PreviousHiZValid;
    uint CurrentHiZValid;
    uint HiZWidth;
    uint HiZHeight;
    uint HiZMipCount;
    float4 MeshLocalCenter;
    float4 MeshLocalExtents;
};

struct MeshPayload
{
    uint MeshletIndex;
};

groupshared MeshPayload AmplificationPayload;

bool MeshObjectOccluded(Texture2D<float> hierarchy, float2 uvMinimum, float2 uvMaximum, float nearestDepth)
{
    float2 extentPixels = max((uvMaximum - uvMinimum) * float2(HiZWidth, HiZHeight), 1.0f);
    float mip = clamp(floor(log2(max(extentPixels.x, extentPixels.y))), 0.0f, (float)max(HiZMipCount, 1u) - 1.0f);
    float2 center = (uvMinimum + uvMaximum) * 0.5f;
    float depth = hierarchy.SampleLevel(HiZSampler, center, mip);
    depth = max(depth, hierarchy.SampleLevel(HiZSampler, uvMinimum, mip));
    depth = max(depth, hierarchy.SampleLevel(HiZSampler, uvMaximum, mip));
    depth = max(depth, hierarchy.SampleLevel(HiZSampler, float2(uvMinimum.x, uvMaximum.y), mip));
    depth = max(depth, hierarchy.SampleLevel(HiZSampler, float2(uvMaximum.x, uvMinimum.y), mip));
    return depth < nearestDepth - 0.0015f;
}

bool MeshObjectVisible()
{
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    bool crossesNearPlane = false;
    float2 uvMinimum = 1.0f;
    float2 uvMaximum = 0.0f;
    float nearestDepth = 1.0f;
    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        float3 signValue = float3(
            (corner & 1u) ? 1.0f : -1.0f,
            (corner & 2u) ? 1.0f : -1.0f,
            (corner & 4u) ? 1.0f : -1.0f);
        float3 localPosition = MeshLocalCenter.xyz + MeshLocalExtents.xyz * signValue;
        float4 worldPosition = mul(float4(localPosition, 1.0f), World);
        float4 viewPosition = mul(worldPosition, View);
        float4 clipPosition = mul(viewPosition, Projection);
        outsideLeft = outsideLeft && clipPosition.x < -clipPosition.w;
        outsideRight = outsideRight && clipPosition.x > clipPosition.w;
        outsideBottom = outsideBottom && clipPosition.y < -clipPosition.w;
        outsideTop = outsideTop && clipPosition.y > clipPosition.w;
        outsideNear = outsideNear && clipPosition.z < 0.0f;
        outsideFar = outsideFar && clipPosition.z > clipPosition.w;
        crossesNearPlane = crossesNearPlane || clipPosition.w <= 0.0f || clipPosition.z <= 0.0f;
        float3 ndc = clipPosition.xyz / max(abs(clipPosition.w), 1.0e-5f);
        float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        uvMinimum = min(uvMinimum, uv);
        uvMaximum = max(uvMaximum, uv);
        nearestDepth = min(nearestDepth, ndc.z);
    }
    if (outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar) return false;
    if (OcclusionPhase == 0u || crossesNearPlane) return true;
    uvMinimum = saturate(uvMinimum);
    uvMaximum = saturate(uvMaximum);
    nearestDepth = saturate(nearestDepth);
    if (OcclusionPhase == 1u)
    {
        return PreviousHiZValid == 0u || !MeshObjectOccluded(PreviousHiZ, uvMinimum, uvMaximum, nearestDepth);
    }
    if (PreviousHiZValid == 0u) return false;
    if (!MeshObjectOccluded(PreviousHiZ, uvMinimum, uvMaximum, nearestDepth)) return false;
    return CurrentHiZValid == 0u || !MeshObjectOccluded(CurrentHiZ, uvMinimum, uvMaximum, nearestDepth);
}

[numthreads(1, 1, 1)]
void ASMain(uint3 groupId : SV_GroupID)
{
    AmplificationPayload.MeshletIndex = groupId.x;
    DispatchMesh(MeshObjectVisible() ? 1u : 0u, 1u, 1u, AmplificationPayload);
}

uint LoadMeshIndex(uint index)
{
    if (MeshIndexIs16Bit != 0u)
    {
        uint packed = MeshIndices.Load((index >> 1u) * 4u);
        return (index & 1u) != 0u ? packed >> 16u : packed & 0xffffu;
    }
    return MeshIndices.Load(index * 4u);
}

PSInput3D BuildMeshVertex(uint index)
{
    StaticVertex input = MeshVertices[index];
    PSInput3D output;
    float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    float4 viewPosition = mul(worldPosition, View);
    output.Position = mul(viewPosition, Projection);
    output.Normal = normalize(mul(input.Normal, (float3x3)World));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    output.WorldPos = worldPosition.xyz;
    output.ViewPos = viewPosition.xyz;
    output.CameraPos = CameraPos;
    return output;
}

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MSMain(
    in payload MeshPayload payload,
    uint threadIndex : SV_GroupIndex,
    out vertices PSInput3D outputVertices[192],
    out indices uint3 outputTriangles[64])
{
    const uint firstIndex = payload.MeshletIndex * 192u;
    const uint remainingIndices = firstIndex < MeshIndexCount ? MeshIndexCount - firstIndex : 0u;
    const uint primitiveCount = min(remainingIndices / 3u, 64u);
    SetMeshOutputCounts(primitiveCount * 3u, primitiveCount);
    if (threadIndex >= primitiveCount) return;
    const uint outputBase = threadIndex * 3u;
    outputVertices[outputBase + 0u] = BuildMeshVertex(LoadMeshIndex(firstIndex + outputBase + 0u));
    outputVertices[outputBase + 1u] = BuildMeshVertex(LoadMeshIndex(firstIndex + outputBase + 1u));
    outputVertices[outputBase + 2u] = BuildMeshVertex(LoadMeshIndex(firstIndex + outputBase + 2u));
    outputTriangles[threadIndex] = uint3(outputBase, outputBase + 1u, outputBase + 2u);
}
