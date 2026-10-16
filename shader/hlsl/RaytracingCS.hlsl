RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> Normal : register(t2);
RWTexture2D<float2> Visibility : register(u0);
#if USE_NRD
#include "../../External/NRD/sdk/Shaders/NRD.hlsli"
Texture2D<float4> Velocity : register(t3);
Texture2D<float4> Material : register(t4);
RWTexture2D<float2> NrdMotion : register(u1);
RWTexture2D<float4> NrdNormalRoughness : register(u2);
RWTexture2D<float> NrdViewZ : register(u3);
RWTexture2D<float> NrdPenumbra : register(u4);
Texture2D<float> DenoisedShadow : register(t5);
RWTexture2D<float> NrdAmbient : register(u5);
Texture2D<float> DenoisedAmbient : register(t6);
#endif

cbuffer RaytracingConstants : register(b0)
{
    float4x4 InvViewProjection;
    float4 SunDirection;
    uint4 ExtentSamples;
    float4 Distances;
    uint4 Features;
    float4x4 WorldToView;
    float4 Jitter; // current and previous sample jitter, in pixels
};

float Random(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (value >> 8) / 16777216.0f;
}

float3 Tangent(float3 normal)
{
    float3 axis = abs(normal.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0);
    return normalize(cross(axis, normal));
}

float TraceHitDistance(float3 origin, float3 direction, float distance, bool closest = false,
    uint visibilityMask = 0x01)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = Distances.z * 0.25f;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(Scene, closest ? RAY_FLAG_NONE : RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        visibilityMask, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 65504.0f : query.CommittedRayT();
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy))
    {
        return;
    }
    float depth = Depth.Load(int3(pixel, 0));
    if (depth >= 0.99999f)
    {
        Visibility[pixel] = float2(1, 1);
#if USE_NRD
        NrdMotion[pixel] = 0;
        NrdNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 1, 0), 1, 0);
        NrdViewZ[pixel] = 1000000.0f;
        NrdPenumbra[pixel] = 65504.0f;
        NrdAmbient[pixel] = 1.0f;
#endif
        return;
    }
    float2 uv = (pixel + 0.5f) / ExtentSamples.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(Normal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    float3 origin = position + normal * Distances.z;
    float3 tangent = Tangent(normal);
    float3 bitangent = cross(normal, tangent);
    float shadow = 0;
    float ambient = 0;
    float penumbra = 0;
    float ambientDistance = Distances.y;
    // Only the denoiser consumes hit distances; binary visibility can stop at any hit.
    bool needsHitDistance = false;
#if USE_NRD
    needsHitDistance = Features.z != 0;
#endif
    float3 sunTangent = Tangent(SunDirection.xyz);
    float3 sunBitangent = cross(SunDirection.xyz, sunTangent);
    for (uint sample = 0; sample < ExtentSamples.z; ++sample)
    {
        uint seed = pixel.x + pixel.y * ExtentSamples.x + sample * 9781u + ExtentSamples.w * 104729u;
        float angle = Random(seed + 17u) * 6.28318530718f;
        float radius = sqrt(Random(seed + 31u));
        float2 disk = float2(cos(angle), sin(angle)) * radius;
        if (Features.x != 0 && SunDirection.w > 0.5f)
        {
            float3 direction = normalize(SunDirection.xyz +
                (sunTangent * disk.x + sunBitangent * disk.y) * Distances.w);
            float hitDistance = TraceHitDistance(origin, direction, Distances.x, needsHitDistance && sample == 0);
            shadow += hitDistance >= 65504.0f ? 1.0f : 0.0f;
#if USE_NRD
            // SIGMA takes the penumbra of one stochastic ray, never averaged binary visibility.
            if (sample == 0) penumbra = dot(normal, direction) > 0 ?
                SIGMA_FrontEnd_PackPenumbra(hitDistance, tan(Distances.w)) : 0;
#endif
        }
        else
        {
            shadow += 1;
            penumbra = 65504.0f;
        }
        if (Features.y != 0)
        {
            float3 direction = tangent * disk.x + bitangent * disk.y + normal * sqrt(1.0f - radius * radius);
            float hitDistance = TraceHitDistance(origin, direction, Distances.y, needsHitDistance && sample == 0, 0x02);
            ambient += hitDistance >= 65504.0f ? 1.0f : 0.0f;
            if (sample == 0) ambientDistance = min(hitDistance, Distances.y);
        }
        else
        {
            ambient += 1;
        }
    }
    Visibility[pixel] = float2(shadow, ambient) / ExtentSamples.z;
#if USE_NRD
    NrdMotion[pixel] = -Velocity.Load(int3(pixel, 0)).zw + (Jitter.zw - Jitter.xy) / ExtentSamples.xy;
    NrdNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normal, saturate(Material.Load(int3(pixel, 0)).y), 0);
    NrdViewZ[pixel] = abs(mul(float4(position, 1), WorldToView).z);
    NrdPenumbra[pixel] = penumbra;
    NrdAmbient[pixel] = REBLUR_FrontEnd_GetNormHitDist(ambientDistance, NrdViewZ[pixel], float3(Distances.y, 0.000001f, 1), 1);
#endif
}

#if USE_NRD
[numthreads(8, 8, 1)]
void ComposeAmbient(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy)) return;
    float depth = Depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5f) / ExtentSamples.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), WorldToView).z);
    float2 visibility = Visibility[pixel];
    if (depth >= 0.99999f) visibility.y = 1;
    else if (viewZ < asfloat(Features.w)) visibility.y = saturate(DenoisedAmbient.Load(int3(pixel, 0)));
    Visibility[pixel] = visibility;
}
[numthreads(8, 8, 1)]
void ComposeShadow(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy)) return;
    float2 visibility = Visibility[pixel];
    float depth = Depth.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5f) / ExtentSamples.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), WorldToView).z);
    if (depth >= 0.99999f) visibility.x = 1.0f;
    else if (viewZ < asfloat(Features.w))
        visibility.x = SIGMA_BackEnd_UnpackShadow(DenoisedShadow.Load(int3(pixel, 0)));
    Visibility[pixel] = visibility;
}
#endif
