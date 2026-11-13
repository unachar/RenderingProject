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
    float4 Jitter; // 現在と前のサンプルのジッター（ピクセル単位）
    float4 Stability; // Stable sampling, shadow history, AO history, reserved.
};

#include "../../source/rendering/stablesampling.h"
#include "Stbn.hlsli"

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

#include "RayTelemetry.hlsli"

float TraceHitDistance(float3 origin, float3 direction, float distance, bool closest = false,
                       uint visibilityMask = 0x01, uint telemetryChannel = RAY_TELEMETRY_SHADOW)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = Distances.z * 0.25f;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    CountTelemetryRay(telemetryChannel);
    query.TraceRayInline(Scene, closest ? RAY_FLAG_NONE : RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         visibilityMask & 0x03u, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 65504.0f : query.CommittedRayT();
}

[numthreads(8, 8, 1)] void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy))
    {
        return;
    }
    float depth = Depth.Load(int3(pixel, 0));
    if (depth <= 0.0f)
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
    bool withinDenoiseRange = false;
#if USE_NRD
    const float viewZ = abs(mul(float4(position, 1), WorldToView).z);
    withinDenoiseRange = Features.z != 0 && viewZ < asfloat(Features.w);
#endif
    float4 normalSample = Normal.Load(int3(pixel, 0));
    float3 normal = normalize(normalSample.xyz * 2.0f - 1.0f);
    // Homogeneous subtraction gives the camera ray for perspective and orthographic views.
    float4 nearPoint = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1), InvViewProjection);
    float4 farPoint = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1), InvViewProjection);
    float3 viewDirection = normalize(nearPoint.xyz * farPoint.w - farPoint.xyz * nearPoint.w);
    float3 origin = position + viewDirection * max(-normalSample.a, 0.0f) + normal * Distances.z;
    float3 tangent = Tangent(normal);
    float3 bitangent = cross(normal, tangent);
    const bool sunActive = Features.x != 0 && SunDirection.w > 0.5f;
    const bool sunBackFacing = sunActive && dot(normal, SunDirection.xyz) < -0.6f;
    const uint shadowSamples = sunActive && !sunBackFacing
        ? ((withinDenoiseRange && Stability.y > 0.5f) || Distances.w == 0.0f ? 1u : ExtentSamples.z) : 0u;
    const uint ambientSamples = Features.y != 0
        ? (withinDenoiseRange && Stability.z > 0.5f ? 1u : ExtentSamples.z) : 0u;
    float shadow = 0;
    float ambient = 0;
    float penumbra = sunActive ? 0.0f : 65504.0f;
    float ambientDistance = Distances.y;
    // ヒット距離を使うのはデノイザーだけ。二値の可視性なら、任意のヒットで停止できる。
    const bool needsHitDistance = withinDenoiseRange;
    float3 sunTangent = Tangent(SunDirection.xyz);
    float3 sunBitangent = cross(SunDirection.xyz, sunTangent);
    // 最適化：N・L < -0.6 の面は、ライティング側のラップ（最大 0.55：肌の散乱 / ターミネーター）を
    // 含めても太陽光の寄与が 0 になるため、可視性の値は結果に影響しない。トレースを省き 0 とする。
    // NRD の半影入力も従来どおり 0（N・L <= 0 の扱い）。
    for (uint sample = 0; sample < max(shadowSamples, ambientSamples); ++sample)
    {
        float2 disk = 0.0f;
        float radius = 0.0f;
        [branch] if (Stability.x <= 0.5f && (sample < ambientSamples || Distances.w > 0.0f))
        {
            uint seed = pixel.x + pixel.y * ExtentSamples.x + sample * 9781u + ExtentSamples.w * 104729u;
            float angle = Random(seed + 17u) * 6.28318530718f;
            radius = sqrt(Random(seed + 31u));
            disk = float2(cos(angle), sin(angle)) * radius;
        }
        if (sample < shadowSamples)
        {
            float3 direction = SunDirection.xyz;
            [branch] if (Distances.w > 0.0f)
            {
                float2 shadowDisk = disk;
                if (Stability.x > 0.5f)
                {
                    // 最初のレイの位相は、フォールバックのサンプル数から独立させる。
                    SamplePair sequenceSample = SampleSequence(pixel.x, pixel.y, (ExtentSamples.w & 7u) + sample * 8u, 3u);
                    float shadowAngle = sequenceSample.y * 6.28318530718f;
                    shadowDisk = float2(cos(shadowAngle), sin(shadowAngle)) * sqrt(sequenceSample.x);
                }
                direction = normalize(direction + (sunTangent * shadowDisk.x + sunBitangent * shadowDisk.y) * Distances.w);
            }
            float hitDistance = TraceHitDistance(origin, direction, Distances.x,
                                                 needsHitDistance && sample == 0 && Distances.w > 0.0f);
            shadow += hitDistance >= 65504.0f ? 1.0f : 0.0f;
#if USE_NRD
            // SIGMA は単一の確率的レイの半影を入力し、平均した二値の可視性は使わない。
            if (sample == 0)
            {
                penumbra = dot(normal, direction) > 0 ? SIGMA_FrontEnd_PackPenumbra(hitDistance, tan(Distances.w)) : 0;
            }
#endif
        }
        if (sample < ambientSamples)
        {
            float2 ambientDisk = disk;
            float ambientRadius = radius;
            if (Stability.x > .5)
            {
                SamplePair fallback = SampleSequence(pixel.x, pixel.y, ExtentSamples.w, 4u + sample);
                SamplePair sequenceSample = StbnSequence(pixel, ExtentSamples.w, 4u, fallback, sample);
                float ambientAngle = sequenceSample.y * 6.28318530718f;
                ambientRadius = sqrt(sequenceSample.x);
                ambientDisk = float2(cos(ambientAngle), sin(ambientAngle)) * ambientRadius;
            }
            float3 direction = tangent * ambientDisk.x + bitangent * ambientDisk.y +
                               normal * sqrt(1.0f - ambientRadius * ambientRadius);
            float hitDistance = TraceHitDistance(origin, direction, Distances.y, needsHitDistance && sample == 0, 0x02,
                                                 RAY_TELEMETRY_AO);
            ambient += hitDistance >= 65504.0f ? 1.0f : 0.0f;
            if (sample == 0)
            {
                ambientDistance = min(hitDistance, Distances.y);
            }
        }
    }
    Visibility[pixel] = float2(sunActive ? shadow / max(shadowSamples, 1u) : 1.0f,
                               ambientSamples > 0 ? ambient / max(ambientSamples, 1u) : 1.0f);
#if USE_NRD
    NrdMotion[pixel] = -Velocity.Load(int3(pixel, 0)).zw + (Jitter.zw - Jitter.xy) / ExtentSamples.xy;
    NrdNormalRoughness[pixel] =
        NRD_FrontEnd_PackNormalAndRoughness(normal, saturate(Material.Load(int3(pixel, 0)).y), 0);
    NrdViewZ[pixel] = viewZ;
    NrdPenumbra[pixel] = penumbra;
    NrdAmbient[pixel] =
        REBLUR_FrontEnd_GetNormHitDist(ambientDistance, viewZ, float3(Distances.y, 0.000001f, 1), 1);
#endif
}

#if USE_NRD
    [numthreads(8, 8, 1)] void ComposeAmbient(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy))
    {
        return;
    }
    const float depth = Depth.Load(int3(pixel, 0));
    if (depth <= 0.0f)
    {
        return;
    }
    float2 uv = (pixel + 0.5f) / ExtentSamples.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), WorldToView).z);
    if (viewZ < asfloat(Features.w))
    {
        float2 visibility = Visibility[pixel];
        visibility.y = saturate(DenoisedAmbient.Load(int3(pixel, 0)));
        Visibility[pixel] = visibility;
    }
}
[numthreads(8, 8, 1)] void ComposeShadow(uint3 dispatchId : SV_DispatchThreadID) {
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= ExtentSamples.xy))
    {
        return;
    }
    const float depth = Depth.Load(int3(pixel, 0));
    if (depth <= 0.0f)
    {
        return;
    }
    float2 uv = (pixel + 0.5f) / ExtentSamples.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), WorldToView).z);
    if (viewZ < asfloat(Features.w))
    {
        float2 visibility = Visibility[pixel];
        visibility.x = SIGMA_BackEnd_UnpackShadow(DenoisedShadow.Load(int3(pixel, 0)));
        Visibility[pixel] = visibility;
    }
}
#endif
