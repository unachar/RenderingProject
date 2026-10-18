#define SHADER_POSTPROCESS
#define SHADER_3D
#define SHADER_RAYTRACING
#define SKY_COMPUTE
#include "common.hlsl"
// Physical sky from the atmosphere LUT (root parameter 13, space6, clamp sampler s1).
#define SKY_LUT 1
#define SKY_LUT_SCATTER_REGISTER t0, space6
#define SKY_LUT_TRANSMISSION_REGISTER t1, space6
#define SKY_LUT_SAMPLER_REGISTER s1
#include "SkyAtmosphere.hlsli"
#if USE_NRD
#include "../../External/NRD/sdk/Shaders/NRD.hlsli"
#endif

#include "RayTelemetry.hlsli"
Texture2D<float> ReflectionDepth : register(t1);
Texture2D<float4> ReflectionNormal : register(t2);
Texture2D<float4> ReflectionMaterial : register(t3);
Texture2D<float4> FilteredRadiance : register(t4);
ByteAddressBuffer SceneVertices[] : register(t0, space1);
ByteAddressBuffer SceneIndices[] : register(t0, space2);
Texture2D<float4> SceneTextures[] : register(t0, space3);
Texture2D<float4> EmissionTextures[] : register(t0, space4);
StructuredBuffer<uint> EmissiveMeshes : register(t6);
SamplerState MaterialSampler : register(s0);
RWTexture2D<float4> NoisyRadiance : register(u0);
RWTexture2D<float4> ReflectionRadiance : register(u1);

struct ReflectionGeometry
{
    float4x4 World;
    float4x4 NormalToWorld;
    uint4 Layout; // vertex stride, index format (0/16/32), vertex offset, index offset
    float4 Material; // roughness, metallic, texture enabled, reserved
    float4 BaseColor;
    float4 Emission;
    uint4 Surface; // triangle count, emission texture present, reserved
};
StructuredBuffer<ReflectionGeometry> ReflectionMeshes : register(t0);
struct ReflectionLight
{
    float4 Direction, Color, PositionType, Extras;
    uint4 Flags;
};
StructuredBuffer<ReflectionLight> ReflectionLights : register(t5);
cbuffer ReflectionConstants : register(b6)
{
    float4x4 ReflectionInvViewProjection;
    float4x4 ReflectionView;
    float4x4 ReflectionViewProjection;
    float4 ReflectionCamera;
    uint4 ReflectionExtent; // width, height, sequence, light count
    float4 GiParameters; // distance, intensity, checkerboard enabled, emissive mesh count
    float4 CacheParameters; // enabled, base cell size (m), capacity, max history frames
    float4 ReSTIRParameters; // enabled, max temporal M, spatial sample count, spatial radius (px)
    float4x4 AdaptivePreviousViewProjection;
    float4 AdaptiveSampling;
    uint4 ReSTIRState;       // history valid, reservoir width (traced columns), spatial visibility, 反射レイの粗さ上限 × 1000
    float4 ReflectionStability;
};
#include "../../source/rendering/stablesampling.h"

Texture2D<float4> AdaptiveRadiance:register(t0,space10);
Texture2D<float4> AdaptiveGuide:register(t1,space10);
RWTexture2D<float4> AdaptiveGuideOut:register(u0,space10);
// ---------------------------------------------------------------------------------------------
// ReSTIR GI (Ouyang et al. 2021): resampled importance sampling of the secondary bounce.
//   Each traced pixel stores a reservoir (sample point, its normal and outgoing radiance, weight W,
//   confidence M, and the visible point's depth / normal for reuse validation).
//   Temporal pass: merges the reprojected reservoir of the previous frame (history).
//   Spatial pass: merges k neighbours with the Jacobian of the solid angle change and an optional
//   visibility ray, then writes the final irradiance for the denoiser.
//   Reservoirs live at trace resolution (packed checkerboard columns when NRD is active).
// ---------------------------------------------------------------------------------------------
uint ReflectionHash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
RWByteAddressBuffer ReservoirCurrent : register(u0, space8);
RWByteAddressBuffer ReservoirHistory : register(u1, space8);
Texture2D<float4> GiVelocity : register(t0, space8);

struct GiReservoir
{
    float3 Position;      // secondary hit (or far point along the ray for sky samples)
    float3 Normal;        // normal at the secondary hit (-ray for sky)
    float3 Radiance;      // radiance leaving the sample towards the visible point
    float W;              // unbiased contribution weight
    float M;              // confidence
    float VisibleDepth;   // linear view depth of the pixel that generated / owns the reservoir
    float3 VisibleNormal;
};

// Component-wise sign without a vector conditional (not valid in HLSL 2021).
float2 OctSign(float2 v)
{
    return step(0.0f, v) * 2.0f - 1.0f;
}

uint PackOctahedral16(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.000001f);
    float2 e = n.xy;
    if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * OctSign(n.xy);
    uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16);
}

float3 UnpackOctahedral16(uint packed)
{
    float2 e = float2(packed & 0xffffu, packed >> 16) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f) n.xy = (1.0f - abs(n.yx)) * OctSign(n.xy);
    return normalize(n);
}

uint PackOctahedral8(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.000001f);
    float2 e = n.xy;
    if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * OctSign(n.xy);
    uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 255.0f);
    return q.x | (q.y << 8);
}

float3 UnpackOctahedral8(uint packed)
{
    float2 e = float2(packed & 0xffu, (packed >> 8) & 0xffu) / 255.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f) n.xy = (1.0f - abs(n.yx)) * OctSign(n.xy);
    return normalize(n);
}

void StoreReservoir(RWByteAddressBuffer buffer, uint index, GiReservoir r)
{
    uint4 a = uint4(asuint(r.Position), PackOctahedral16(r.Normal));
    uint4 b = uint4(f32tof16(r.Radiance.r) | (f32tof16(r.Radiance.g) << 16),
        f32tof16(r.Radiance.b) | (f32tof16(r.M) << 16), asuint(r.W),
        f32tof16(r.VisibleDepth) | (PackOctahedral8(r.VisibleNormal) << 16));
    buffer.Store4(index * 32u, a);
    buffer.Store4(index * 32u + 16u, b);
}

GiReservoir LoadReservoir(RWByteAddressBuffer buffer, uint index)
{
    uint4 a = buffer.Load4(index * 32u);
    uint4 b = buffer.Load4(index * 32u + 16u);
    GiReservoir r;
    r.Position = asfloat(a.xyz);
    r.Normal = UnpackOctahedral16(a.w);
    r.Radiance = float3(f16tof32(b.x), f16tof32(b.x >> 16), f16tof32(b.y));
    r.M = f16tof32(b.y >> 16);
    r.W = asfloat(b.z);
    r.VisibleDepth = f16tof32(b.w);
    r.VisibleNormal = UnpackOctahedral8(b.w >> 16);
    if (any(isnan(r.Radiance)) || isnan(r.W) || isinf(r.W)) { r.W = 0.0f; r.M = 0.0f; }
    return r;
}

float ReSTIRLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Target function at a visible point: luminance of the unshadowed cosine weighted radiance.
float ReSTIRTarget(GiReservoir r, float3 position, float3 normal)
{
    float3 direction = r.Position - position;
    float distance = length(direction);
    if (distance < 0.0001f) return 0.0f;
    float cosine = saturate(dot(normal, direction / distance));
    return ReSTIRLuminance(r.Radiance) * cosine;
}

float ReSTIRRandom(inout uint state)
{
    state = ReflectionHash(state + 0x9e3779b9u);
    return ((state >> 8) + 0.5f) / 16777216.0f;
}

// Reprojected / neighbour reservoirs are only reused on a similar surface.
bool ReSTIRSimilarSurface(float depthA, float3 normalA, float depthB, float3 normalB)
{
    return abs(depthA - depthB) < max(depthA, 0.01f) * 0.1f && dot(normalA, normalB) > 0.9f;
}

uint2 ReSTIRStorageToPixel(uint2 storage)
{
    uint2 pixel = storage;
    if (GiParameters.z > 0.5f) pixel.x = 2 * storage.x + ((storage.y + ReflectionExtent.z) & 1u);
    return pixel;
}

bool ReSTIRVisiblePoint(uint2 pixel, out float3 position, out float3 normal, out float viewDepth)
{
    position = 0; normal = float3(0, 1, 0); viewDepth = 0;
    if (any(pixel >= ReflectionExtent.xy)) return false;
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    if (depth >= 0.99999f) return false;
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    position = world.xyz / world.w;
    normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    viewDepth = abs(mul(float4(position, 1), ReflectionView).z);
    return true;
}

// ---------------------------------------------------------------------------------------------
// World-space irradiance cache (hash grid, SHaRC-style).
//   Every GI sample writes its irradiance estimate (sky + bounce + emitters, i.e. everything
//   except the analytic lights evaluated at the hit) into the cell of its primary surface.
//   A GI ray that hits a cached cell adds albedo/pi * E_cache to the one-bounce radiance, so
//   indirect light propagates over frames -> multiple bounces for the cost of one lookup.
// ---------------------------------------------------------------------------------------------
RWByteAddressBuffer CacheKeys : register(u0, space7);
RWByteAddressBuffer CacheAccumulation : register(u1, space7); // uint4: rgb fixed point, sample count
struct CacheValue { float4 Signal; float4 Distance; };
RWStructuredBuffer<CacheValue> CacheResolved : register(u2, space7); // rgb irradiance, history frames
RWByteAddressBuffer CacheAges : register(u3, space7);
static const float kCacheScale = 256.0f;        // fixed point scale of the accumulation
static const float kCacheMaxIrradiance = 256.0f;
static const uint kCacheProbes = 8;
static const uint kCacheMaxAge = 240;           // frames without samples before eviction

uint CacheHash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

// Cell size doubles every octave of camera distance beyond 8 m (constant screen footprint).
void CacheCell(float3 position, float3 normal, out uint key, out uint bucket)
{
    float distance = length(position - ReflectionCamera.xyz);
    uint level = (uint)clamp(floor(log2(max(distance, 1.0f) / 8.0f)) + 1.0f, 0.0f, 12.0f);
    float cellSize = CacheParameters.y * exp2((float)level);
    int3 cell = int3(floor(position / cellSize));
    float3 magnitude = abs(normal);
    uint axis = magnitude.x > magnitude.y ? (magnitude.x > magnitude.z ? 0u : 2u) : (magnitude.y > magnitude.z ? 1u : 2u);
    uint orientation = axis * 2u + (normal[axis] < 0 ? 1u : 0u);
    uint hash = CacheHash(asuint(cell.x) + CacheHash(asuint(cell.y) + CacheHash(asuint(cell.z) + CacheHash(level | (orientation << 4)))));
    key = hash | 1u;                                     // 0 marks an empty slot
    bucket = CacheHash(hash ^ 0x9e3779b9u) & ((uint)CacheParameters.z - 1u);
}

bool CacheLookupSignal(float3 position,float3 normal,out float3 irradiance,out float hitDistance)
{
    irradiance=0; hitDistance=0;
    if (CacheParameters.x < 0.5f) return false;
    uint key, bucket;
    CacheCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    [loop] for (uint probe = 0; probe < kCacheProbes; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        if (CacheKeys.Load(slot * 4) == key)
        {
            float4 resolved=CacheResolved[slot].Signal; hitDistance=CacheResolved[slot].Distance.x;
            irradiance = resolved.rgb;
            return resolved.w>0 && CacheAges.Load(slot*4)<=8u && all(isfinite(resolved)) && isfinite(hitDistance);
        }
    }
    return false;
}

bool CacheLookup(float3 position,float3 normal,out float3 irradiance) { float distance; return CacheLookupSignal(position,normal,irradiance,distance); }

void CacheInsert(float3 position,float3 normal,float3 irradiance,float hitDistance)
{
    if (CacheParameters.x < 0.5f || any(isnan(irradiance)) || any(isinf(irradiance))) return;
    uint key, bucket;
    CacheCell(position, normal, key, bucket);
    uint mask = (uint)CacheParameters.z - 1u;
    int found = -1;
    [loop] for (uint probe = 0; probe < kCacheProbes && found < 0; ++probe)
    {
        uint slot = (bucket + probe) & mask;
        if (CacheKeys.Load(slot * 4) == key) found = (int)slot;
    }
    [loop] for (uint claim = 0; claim < kCacheProbes && found < 0; ++claim)
    {
        uint slot = (bucket + claim) & mask;
        uint previous;
        CacheKeys.InterlockedCompareExchange(slot * 4, 0u, key, previous);
        if (previous == 0u || previous == key) found = (int)slot;
    }
    if (found < 0) return; // neighbourhood full: the sample is simply not cached
    uint3 value = (uint3)(min(max(irradiance, 0), kCacheMaxIrradiance) * kCacheScale + 0.5f);
    uint address = (uint)found * 32;
    uint ignored;
    CacheAccumulation.InterlockedAdd(address + 0, value.x, ignored);
    CacheAccumulation.InterlockedAdd(address + 4, value.y, ignored);
    CacheAccumulation.InterlockedAdd(address + 8, value.z, ignored);
    CacheAccumulation.InterlockedAdd(address + 12, 1u, ignored);
    CacheAccumulation.InterlockedMax(address+16,asuint(clamp(hitDistance,0,65504)),ignored);
}


// Bounded VNDF sampling as used by NVIDIA MathLib v11. Keeps reflection rays above the surface.
// https://github.com/NVIDIA-RTX/MathLib/blob/aa432ad9a80691eb8c3ec4d576bbad5ac100fdc8/ml.hlsli
float3 SampleVisibleGgx(float3 view, float3 normal, float roughness, float2 random)
{
    float3 tangent = normalize(cross(abs(normal.y) < 0.95f ? float3(0,1,0) : float3(1,0,0), normal));
    float3 bitangent = cross(normal, tangent);
    float alpha = roughness * roughness;
    float3 localView = float3(dot(view, tangent), dot(view, bitangent), max(dot(view, normal), 0.0001f));
    float3 stretchedView = normalize(float3(localView.xy * alpha, localView.z));
    float boundScale = 1 + length(localView.xy);
    float boundSquared = boundScale * boundScale;
    float bound = (1 - alpha * alpha) * boundSquared /
        (boundSquared + alpha * alpha * localView.z * localView.z);
    float height = 1 - random.y * (1 + bound * stretchedView.z);
    float radius = sqrt(max(1 - height * height, 0));
    float angle = 6.2831853f * random.x;
    float3 hemisphereNormal = float3(radius * cos(angle), radius * sin(angle), height) + stretchedView;
    float3 localHalf = normalize(float3(hemisphereNormal.xy * alpha, max(hemisphereNormal.z, 0)));
    return normalize(tangent * localHalf.x + bitangent * localHalf.y + normal * localHalf.z);
}

uint LoadReflectionIndex(uint instance, uint index, ReflectionGeometry mesh)
{
    if (mesh.Layout.y == 0) return index;
    uint offset = mesh.Layout.w + index * (mesh.Layout.y / 8);
    uint packed = SceneIndices[NonUniformResourceIndex(instance)].Load(offset & ~3u);
    return mesh.Layout.y == 16 ? (packed >> ((offset & 2) * 8)) & 0xffff : packed;
}

float3 SampleEmission(uint instance, float2 uv)
{
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    float3 emission = mesh.Emission.rgb * mesh.Emission.a;
    if (mesh.Surface.y != 0)
    {
        emission *= EmissionTextures[NonUniformResourceIndex(instance)].SampleLevel(MaterialSampler, uv, 0).rgb;
    }
    return emission;
}

float TraceBounceVisibility(float3 position, float3 normal, float3 direction, float distance)
{
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(distance - SceneRayParameters.y, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_RADIANCE_SHADOW);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1 : 0;
}

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming,
    bool diffuseOnly, uint randomSeed, out float3 surfaceDiffuse, out float3 surfaceNormal)
{
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    float3 weights = float3(1 - barycentrics.x - barycentrics.y, barycentrics);
    float3 normal = 0;
    float2 uv = 0;
    float3 albedo = 0;
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint vertex = LoadReflectionIndex(instance, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + vertex * mesh.Layout.x;
        normal += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset + 12)) * weights[corner];
        uv += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load2(offset + 24)) * weights[corner];
        albedo += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset + 32)) * weights[corner];
    }
    normal = normalize(mul(float4(normal, 0), mesh.NormalToWorld).xyz);
    if (dot(normal, incoming) > 0) normal = -normal;
    if (mesh.Material.z > 0.5f)
        albedo *= SceneTextures[NonUniformResourceIndex(instance)].SampleLevel(MaterialSampler, uv, 0).rgb;
    albedo *= mesh.BaseColor.rgb;
    float metallic = saturate(mesh.Material.y);
    float roughness = clamp(mesh.Material.x, 0.02f, 1);
    float3 f0 = lerp(0.04f, max(albedo, 0), metallic);
    float3 view = -incoming;
    float nDotV = max(dot(normal, view), 0.0001f);
    float3 skyFresnel = f0 + (1 - f0) * pow(1 - saturate(nDotV), 5);
    float3 diffuse = max(albedo, 0) * (1 - metallic) / 3.14159265f;
    surfaceDiffuse = diffuse;
    surfaceNormal = normal;
    float3 radiance = 0;
    if (!diffuseOnly)
    {
        // Diffuse GI bounces skip both atmosphere marches (the result was discarded anyway).
        // Secondary-hit sky terms (diffuse sky light and second-order reflection) use the
        // reduced step atmosphere; the directly reflected sky in main() keeps full quality.
        radiance = diffuse * EvaluatePhysicalSkyFast(normal, 0.01f) * 0.12f +
            skyFresnel * EvaluatePhysicalSkyFast(reflect(incoming, normal), max(0.001f, roughness * 0.02f)) +
            SampleEmission(instance, uv);
    }
    int count = (int)ReflectionExtent.w;
    [branch] if (diffuseOnly)
    {
        // One shadow ray per GI bounce, light chosen proportionally to its unshadowed
        // contribution (single pass weighted reservoir sampling). The former uniform choice
        // wasted most rays on weak / out of range lights, which is pure noise at 0.5 rpp.
        // Unbiased: estimate = f_i * V_i / (w_i / W).
        float random = ((ReflectionHash(randomSeed) >> 8) + 0.5f) / 16777216.0f;
        float totalWeight = 0;
        float selectedWeight = 0;
        float3 selectedDirection = 0;
        float3 selectedRadiance = 0;
        float selectedRange = 0;
        bool selectedShadow = false;
        [loop] for (int i = 0; i < count; ++i)
        {
            ReflectionLight light = ReflectionLights[i];
            float3 direction;
            float attenuation, volume;
            ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation, volume);
            float3 incident = light.Color.rgb * max(light.Color.a, 0) * saturate(dot(normal, direction)) * attenuation;
            float weight = dot(incident, float3(0.2126f, 0.7152f, 0.0722f));
            if (weight <= 0.0000001f) continue;
            totalWeight += weight;
            float probability = weight / totalWeight;
            if (random < probability)
            {
                random /= probability;
                selectedWeight = weight;
                selectedDirection = direction;
                selectedRadiance = incident;
                selectedShadow = light.Flags.x != 0;
                selectedRange = light.PositionType.w < 0.5f ? SceneRayParameters.x :
                    min(length(light.PositionType.xyz - position), SceneRayParameters.x);
            }
            else
            {
                random = (random - probability) / max(1 - probability, 0.000001f);
            }
        }
        if (selectedWeight > 0)
        {
            float shadow = selectedShadow ?
                TraceBounceVisibility(position, normal, selectedDirection, selectedRange) : 1;
            radiance += diffuse * selectedRadiance * (shadow * totalWeight / selectedWeight);
        }
        return radiance;
    }
    float shadowRandom = ((ReflectionHash(randomSeed ^ 0x5bd1e995u) >> 8) + 0.5f) / 16777216.0f;
    float shadowTotalWeight = 0;
    float shadowSelectedWeight = 0;
    float3 shadowSelectedContribution = 0;
    float3 shadowSelectedDirection = 0;
    float shadowSelectedRange = 0;
    [loop] for (int i = 0; i < count; ++i)
    {
        ReflectionLight light = ReflectionLights[i];
        float3 direction;
        float attenuation, volume;
        ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation, volume);
        float cosine = saturate(dot(normal, direction));
        if (cosine * attenuation < 0.00001f) continue;
        float3 halfVector = normalize(view + direction);
        float nDotH = saturate(dot(normal, halfVector));
        float vDotH = saturate(dot(view, halfVector));
        float alphaSquared = max(pow(roughness, 4), 0.000001f);
        float denominator = nDotH * nDotH * (alphaSquared - 1) + 1;
        float distribution = alphaSquared / max(3.14159265f * denominator * denominator, 0.000001f);
        float k = (roughness + 1) * (roughness + 1) / 8;
        float geometry = (nDotV / (nDotV * (1 - k) + k)) * (cosine / (cosine * (1 - k) + k));
        float3 fresnel = f0 + (1 - f0) * pow(1 - vDotH, 5);
        float3 specular = distribution * geometry * fresnel / max(4 * nDotV * cosine, 0.00001f);
        float3 response = (1 - fresnel) * diffuse + specular;
        float3 contribution = response * light.Color.rgb * max(light.Color.a, 0) * cosine * attenuation;
        // Same result, but no shadow ray for lights that cannot change the pixel.
        if (all(contribution < 0.000001f)) continue;
        if (light.Flags.x == 0)
        {
            radiance += contribution; // 影を落とさないライトはそのまま足す（レイ不要）
            continue;
        }
        // 負荷削減：影を落とすライトは「寄与に比例して 1 灯選び、影のレイは 1 本だけ」（重み付きリザーバー、不偏）。
        // 以前はライトごとに影のレイを飛ばしていたので、ライトが多いと反射のコストがライト数に比例していた
        float weight = dot(contribution, float3(0.2126f, 0.7152f, 0.0722f));
        if (weight <= 0.0000001f) continue;
        shadowTotalWeight += weight;
        float probability = weight / shadowTotalWeight;
        if (shadowRandom < probability)
        {
            shadowRandom /= probability;
            shadowSelectedWeight = weight;
            shadowSelectedContribution = contribution;
            shadowSelectedDirection = direction;
            shadowSelectedRange = light.PositionType.w < 0.5f ? SceneRayParameters.x :
                min(length(light.PositionType.xyz - position), SceneRayParameters.x);
        }
        else
        {
            shadowRandom = (shadowRandom - probability) / max(1 - probability, 0.000001f);
        }
    }
    if (shadowSelectedWeight > 0)
    {
        float shadow = TraceSceneShadow(position, normal, shadowSelectedDirection, shadowSelectedRange);
        radiance += shadowSelectedContribution * (shadow * shadowTotalWeight / shadowSelectedWeight);
    }
    return radiance;
}

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming,
    bool diffuseOnly = false, uint randomSeed = 0)
{
    float3 surfaceDiffuse, surfaceNormal;
    return ShadeReflectionHit(instance, primitive, barycentrics, position, incoming, diffuseOnly, randomSeed,
        surfaceDiffuse, surfaceNormal);
}

bool AdaptiveReflection(uint2 pixel,float depth,float3 position,float3 normal,float roughness,out float4 signal)
{
    signal=0; if(AdaptiveSampling.x<.5||AdaptiveSampling.y<.5||roughness<AdaptiveSampling.z) return false;
    uint period=roughness>.55?4u:2u; if(((pixel.x+pixel.y*2u+ReflectionExtent.z)%period)==0) return false;
    float2 motion=GiVelocity.Load(int3(pixel,0)).zw;
    if(!all(isfinite(motion))||length(motion*ReflectionExtent.xy)>.5) return false;
    float tolerance=max(2e-5,abs(1-depth)*.005);
    [unroll] for(uint i=0;i<4;++i)
    {
        int2 offset=i==0?int2(1,0):i==1?int2(-1,0):i==2?int2(0,1):int2(0,-1);
        int2 p=clamp(int2(pixel)+offset,0,int2(ReflectionExtent.xy)-1);
        if(abs(ReflectionDepth.Load(int3(p,0))-depth)>tolerance) return false;
        if(dot(normalize(ReflectionNormal.Load(int3(p,0)).xyz*2-1),normal)<.995) return false;
    }
    float4 previous=mul(float4(position,1),AdaptivePreviousViewProjection); if(previous.w<=0) return false;
    float3 ndc=previous.xyz/previous.w; float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<=0)||any(uv>=1)||ndc.z<0||ndc.z>1) return false;
    uint2 old=min(uint2(uv*ReflectionExtent.xy),ReflectionExtent.xy-1); float4 guide=AdaptiveGuide.Load(int3(old,0));
    if(abs(guide.w-ndc.z)>tolerance||dot(guide.xyz,normal)<.995) return false;
    signal=AdaptiveRadiance.Load(int3(old,0)); return all(isfinite(signal))&&all(signal.rgb>=0);
}
[numthreads(8,8,1)]
void CaptureAdaptiveGuide(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=ReflectionExtent.xy)) return;
    AdaptiveGuideOut[id.xy]=float4(normalize(ReflectionNormal.Load(int3(id.xy,0)).xyz*2-1),ReflectionDepth.Load(int3(id.xy,0)));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 負荷削減：NRD がある場合は半分のピクセル（チェッカーボード）だけトレースし、NRD が残りを復元する
    //（GI の TraceDiffuse と同じ詰め方：トレースしたピクセルを (x >> 1, y) に格納。NRD の CheckerboardMode::BLACK）
    uint2 storage = id.xy;
    uint2 pixel = id.xy;
    const bool checkerboard = (ReSTIRState.w & 0x10000u) != 0u;
    if (checkerboard)
    {
        pixel.x = 2 * pixel.x + ((pixel.y + ReflectionExtent.z) & 1u);
        if (storage.y >= ReflectionExtent.y || storage.x >= (ReflectionExtent.x + 1) / 2) return;
        if (pixel.x >= ReflectionExtent.x)
        {
            NoisyRadiance[storage] = 0;
            return;
        }
    }
    else if (any(pixel >= ReflectionExtent.xy)) return;
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    if (depth >= 0.99999f)
    {
        NoisyRadiance[storage] = 0;
        ReflectionRadiance[pixel] = 0;
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 view = normalize(ReflectionCamera.xyz - position);
    float roughness = clamp(ReflectionMaterial.Load(int3(pixel, 0)).y, 0.02f, 1);
    float4 adaptiveSignal;
    if(AdaptiveReflection(pixel,depth,position,normal,roughness,adaptiveSignal))
    {
        ReflectionRadiance[pixel]=adaptiveSignal; float distance=abs(adaptiveSignal.w)>0?abs(adaptiveSignal.w):65504;
#if USE_NRD
        float viewZ=abs(mul(float4(position,1),ReflectionView).z);
        float normalized=REBLUR_FrontEnd_GetNormHitDist(distance,viewZ,float3(3,.1,20),roughness);
        NoisyRadiance[storage]=REBLUR_FrontEnd_PackRadianceAndNormHitDist(adaptiveSignal.rgb,normalized,true);
#else
        NoisyRadiance[storage]=float4(adaptiveSignal.rgb,distance);
#endif
        return;
    }

    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 0xffff) + 0.5f, (seed >> 16) + 0.5f) / 65536;
    // 負荷削減：粗い面は反射レイを飛ばさず空（環境）で近似する（ローブが広く、レイ 1 本ではほぼノイズにしかならない）
    const float roughnessCutoff = (ReSTIRState.w & 0xffffu) / 1000.0f;
    if (roughnessCutoff < 0.999f && roughness > roughnessCutoff)
    {
        float3 skyRadiance = EvaluatePhysicalSkyFast(reflect(-view, normal), max(0.001f, roughness * 0.02f));
        ReflectionRadiance[pixel] = float4(skyRadiance, 0);
#if USE_NRD
        float viewZCut = abs(mul(float4(position, 1), ReflectionView).z);
        NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(skyRadiance,
            REBLUR_FrontEnd_GetNormHitDist(65504, viewZCut, float3(3, 0.1f, 20), roughness), true);
#else
        NoisyRadiance[storage] = float4(skyRadiance, 65504);
#endif
        return;
    }
    float alpha = roughness * roughness;
    float3 halfVector = SampleVisibleGgx(view, normal, roughness, random);
    float3 direction = reflect(-view, halfVector);
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = SceneRayParameters.x;
    bool hit = false;
    float hitDistance = 65504;
    float3 radiance = 0;
    float nDotL = dot(normal, direction);
    if (nDotL > 0)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        CountTelemetryRay(RAY_TELEMETRY_REFLECTION);
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
        while (query.Proceed()) { }
        hit = query.CommittedStatus() != COMMITTED_NOTHING;
        hitDistance = hit ? query.CommittedRayT() : 65504;
        radiance = hit ? ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(), ray.Origin + direction * hitDistance, direction, false, seed * 7919u + 17u) :
            EvaluatePhysicalSky(direction, max(0.001f, roughness * 0.02f));
        // Smith masking and the bounded-lobe probability after dividing by the sampling density.
        float masking = 2 * nDotL / (nDotL + sqrt(alpha * alpha + (1 - alpha * alpha) * nDotL * nDotL));
        float nDotV = max(dot(normal, view), 0.0001f);
        float projectedView = sqrt(max(1 - nDotV * nDotV, 0));
        float boundSquared = (1 + projectedView) * (1 + projectedView);
        float bound = (1 - alpha * alpha) * boundSquared / (boundSquared + alpha * alpha * nDotV * nDotV);
        float stretchedLength = sqrt(alpha * alpha * (1 - nDotV * nDotV) + nDotV * nDotV);
        radiance *= masking * (bound * nDotV + stretchedLength) / (nDotV + stretchedLength);
    }
    radiance = min(max(radiance, 0), 1000);
    float4 hitClip = mul(float4(ray.Origin + direction * min(hitDistance, ray.TMax), 1), ReflectionViewProjection);
    bool offscreen = hitClip.w <= 0 || any(abs(hitClip.xy) > hitClip.w) || hitClip.z < 0 || hitClip.z > hitClip.w;
    // Signed distance supports capture validation; RGB is the shading signal.
    ReflectionRadiance[pixel] = float4(radiance, hit ? (offscreen ? -hitDistance : hitDistance) : 0);
#if USE_NRD
    float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3, 0.1f, 20), roughness);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(radiance, hitDistance);
#endif
}

#if USE_NRD
[numthreads(8, 8, 1)]
void Compose(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= ReflectionExtent.xy)) return;
    float depth = ReflectionDepth.Load(int3(id.xy, 0));
    float2 uv = (id.xy + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float viewZ = abs(mul(float4(world.xyz / world.w, 1), ReflectionView).z);
    if (depth < 0.99999f && viewZ < ReflectionCamera.w)
        ReflectionRadiance[id.xy] = float4(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(FilteredRadiance.Load(int3(id.xy, 0))).rgb,
            ReflectionRadiance[id.xy].w);
}
#endif

// One triangle sampled uniformly on one emissive mesh. Area and inverse-square falloff
// turn authored radiance into incident irradiance; the visibility ray prevents wall leaks.
float3 SampleEmissiveIrradiance(float3 position, float3 normal, uint seed, uint2 pixel, bool checkerboard)
{
    uint emitterCount = uint(GiParameters.w);
    if (emitterCount == 0)
    {
        return 0;
    }
    uint sequence = PixelSampleIndex(ReflectionExtent.z, checkerboard);
    SamplePair selection = SampleSequence(pixel.x, pixel.y, sequence, 1u);
    uint emitterIndex = ReflectionStability.x > .5 ? min(uint(selection.x * emitterCount), emitterCount - 1) : ReflectionHash(seed) % emitterCount;
    uint instance = EmissiveMeshes[emitterIndex];
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    uint triangleCount = mesh.Surface.x;
    if (triangleCount == 0)
    {
        return 0;
    }
    uint primitive = ReflectionStability.x > .5 ? min(uint(selection.y * triangleCount), triangleCount - 1) : ReflectionHash(seed + 1) % triangleCount;
    uint random = ReflectionHash(seed + 2);
    float2 sample = float2((random & 65535) + 0.5f, (random >> 16) + 0.5f) / 65536;
    if (ReflectionStability.x > .5)
    {
        SamplePair sequenceSample = SampleSequence(pixel.x, pixel.y, sequence, 2u);
        sample = float2(sequenceSample.x, sequenceSample.y);
    }
    float root = sqrt(sample.x);
    float3 weights = float3(1 - root, root * (1 - sample.y), root * sample.y);
    float3 vertices[3];
    float2 uv = 0;
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        uint index = LoadReflectionIndex(instance, primitive * 3 + corner, mesh);
        uint offset = mesh.Layout.z + index * mesh.Layout.x;
        vertices[corner] = mul(float4(asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load3(offset)), 1), mesh.World).xyz;
        uv += asfloat(SceneVertices[NonUniformResourceIndex(instance)].Load2(offset + 24)) * weights[corner];
    }
    float3 areaVector = cross(vertices[1] - vertices[0], vertices[2] - vertices[0]);
    float twiceArea = length(areaVector);
    if (twiceArea < 0.000001f)
    {
        return 0;
    }
    float3 lightPosition = vertices[0] * weights.x + vertices[1] * weights.y + vertices[2] * weights.z;
    float3 offset = lightPosition - position;
    float distanceSquared = dot(offset, offset);
    float distance = sqrt(distanceSquared);
    if (distance < 2 * SceneRayParameters.y || distance > GiParameters.x)
    {
        return 0;
    }
    float3 direction = offset / distance;
    float cosine = saturate(dot(normal, direction));
    // Emissive materials radiate from both sides, matching the existing two-sided ray scene.
    float emitterCosine = abs(dot(areaVector / twiceArea, -direction));
    if (cosine * emitterCosine <= 0)
    {
        return 0;
    }
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    float3 biasedOffset = lightPosition - ray.Origin;
    ray.Direction = normalize(biasedOffset);
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(length(biasedOffset) - SceneRayParameters.y, ray.TMin);
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    CountTelemetryRay(RAY_TELEMETRY_RADIANCE_SHADOW);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        return 0;
    }
    // Solid angle estimate of the selected emitter mesh (area pdf of the triangle choice).
    // A mesh never covers more than the hemisphere (2 pi sr); bounding the estimate removes the
    // 1/d^2 singularity of samples taken right next to the receiver, which is what produces
    // isolated, frame-to-frame fireflies around emissive objects. Unbiased everywhere else.
    float solidAngle = emitterCosine * 0.5f * twiceArea * triangleCount / max(distanceSquared, 0.0001f);
    solidAngle = min(solidAngle, 6.2831853f);
    return SampleEmission(instance, uv) * (cosine * solidAngle * emitterCount);
}

[numthreads(8, 8, 1)]
void TraceDiffuse(uint3 id : SV_DispatchThreadID)
{
    // NRD checkerboard input is PACKED: the traced pixel of each horizontal pair is stored at
    // (x >> 1, y), i.e. in the left half of the texture. Writing it at its full resolution
    // position made NRD sample a horizontally stretched copy of the screen (plus stale texels
    // from older frames), which showed up as objects "printed" into the denoised noise.
    uint2 storage = id.xy;
    uint2 pixel = id.xy;
    bool checkerboard = GiParameters.z > 0.5f;
    if (checkerboard)
    {
        // Same parity as NRD: has data when ((x ^ y ^ frameIndex) & 1) == 0 (CheckerboardMode::BLACK).
        pixel.x = 2 * pixel.x + ((pixel.y + ReflectionExtent.z) & 1u);
        if (storage.y >= ReflectionExtent.y || storage.x >= (ReflectionExtent.x + 1) / 2)
        {
            return;
        }
        if (pixel.x >= ReflectionExtent.x)
        {
            // Odd width: keep the last packed texel defined, NRD never reads it as data.
            NoisyRadiance[storage] = 0;
            return;
        }
    }
    else if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    NoisyRadiance[storage] = 0;
    if (depth >= 0.99999f)
    {
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    uint period=max(1u,(uint)AdaptiveSampling.w);
    if(period>1 && ((ReflectionHash(pixel.x+pixel.y*ReflectionExtent.x)+ReflectionExtent.z)%period)!=0)
    {
        float2 motion=GiVelocity.Load(int3(pixel,0)).zw; float3 cached; float cachedDistance;
        bool stable=all(isfinite(motion)) && length(motion*ReflectionExtent.xy)<.25;
        [unroll] for(uint i=0;i<4;++i)
        {
            int2 offset=i==0?int2(1,0):i==1?int2(-1,0):i==2?int2(0,1):int2(0,-1);
            int2 p=clamp(int2(pixel)+offset,0,int2(ReflectionExtent.xy)-1);
            stable=stable && abs(ReflectionDepth.Load(int3(p,0))-depth)<max(2e-5,abs(1-depth)*.005) && dot(normalize(ReflectionNormal.Load(int3(p,0)).xyz*2-1),normal)>.995;
        }
        if(stable && CacheLookupSignal(position,normal,cached,cachedDistance))
        {
#if USE_NRD
            float viewZ=abs(mul(float4(position,1),ReflectionView).z);
            float normalized=REBLUR_FrontEnd_GetNormHitDist(cachedDistance,viewZ,float3(3,.1,20),1);
            NoisyRadiance[storage]=REBLUR_FrontEnd_PackRadianceAndNormHitDist(cached,normalized,true);
#else
            NoisyRadiance[storage]=float4(cached,cachedDistance);
#endif
            return;
        }
    }
    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 65535) + 0.5f, (seed >> 16) + 0.5f) / 65536;
    if (ReflectionStability.x > .5)
    {
        SamplePair sequenceSample = SampleSequence(pixel.x, pixel.y, PixelSampleIndex(ReflectionExtent.z, checkerboard), 0u);
        random = float2(sequenceSample.x, sequenceSample.y);
    }
    float radius = sqrt(random.x);
    float angle = 6.2831853f * random.y;
    float3 tangent = normalize(cross(abs(normal.y) < 0.95f ? float3(0,1,0) : float3(1,0,0), normal));
    float3 direction = tangent * (radius * cos(angle)) + cross(normal, tangent) * (radius * sin(angle)) + normal * sqrt(1 - random.x);
    RayDesc ray;
    ray.Origin = position + normal * SceneRayParameters.y;
    ray.Direction = direction;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = GiParameters.x;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    CountTelemetryRay(RAY_TELEMETRY_GI);
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    float hitDistance = ray.TMax;
    float3 incoming = 0;
    float3 secondaryNormal = -direction;
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        hitDistance = query.CommittedRayT();
        // Emission is estimated separately by area sampling to avoid double counting.
        float3 hitPosition = ray.Origin + direction * hitDistance;
        float3 hitDiffuse, hitNormal;
        incoming = ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(), hitPosition, direction, true, seed, hitDiffuse, hitNormal);
        secondaryNormal = hitNormal;
        // Further bounces: indirect irradiance cached at the hit surface (radiance = albedo/pi * E).
        float3 cachedIrradiance;
        if (CacheLookup(hitPosition, hitNormal, cachedIrradiance))
        {
            incoming += hitDiffuse * cachedIrradiance;
        }
    }
    else
    {
        // Sun / moon discs are excluded: direct light already covers them (see SkyAtmosphere.hlsli).
        incoming = EvaluatePhysicalSkyFast(direction, 0.01f, false);
    }
    float3 emissiveIrradiance = SampleEmissiveIrradiance(position, normal, seed + 17, pixel, checkerboard);
    float3 irradiance = incoming * 3.14159265f + emissiveIrradiance;
    irradiance = min(max(irradiance, 0), 65504);
    CacheInsert(position, normal, irradiance,hitDistance);
    if (ReSTIRParameters.x > 0.5f)
    {
        // Initial candidate: source pdf = cos / pi, target = lum(L) * cos  ->  w = lum(L) * pi.
        GiReservoir reservoir;
        bool hit = query.CommittedStatus() != COMMITTED_NOTHING;
        reservoir.Position = hit ? ray.Origin + direction * hitDistance : position + direction * 10000.0f;
        reservoir.Normal = secondaryNormal; // needed for the Jacobian of spatial reuse
        reservoir.Radiance = min(max(incoming, 0.0f), 65504.0f);
        float target = ReSTIRLuminance(reservoir.Radiance) * saturate(dot(normal, direction));
        reservoir.M = 1.0f;
        reservoir.W = target > 0.0f ? 3.14159265f / max(saturate(dot(normal, direction)), 0.0001f) : 0.0f;
        reservoir.VisibleDepth = abs(mul(float4(position, 1), ReflectionView).z);
        reservoir.VisibleNormal = normal;
        StoreReservoir(ReservoirCurrent, storage.y * ReSTIRState.y + storage.x, reservoir);
        // Emitters are sampled separately and are not resampled: keep them for the spatial pass.
        NoisyRadiance[storage] = float4(min(emissiveIrradiance, 65504.0f), hitDistance);
        return;
    }
#if USE_NRD
    float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3, 0.1f, 20), 1);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(irradiance, hitDistance);
#endif
}

[numthreads(8, 8, 1)]
void FilterDiffuse(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float centerDepth = ReflectionDepth.Load(int3(pixel, 0));
    if (centerDepth >= 0.99999f)
    {
        ReflectionRadiance[pixel] = 0;
        return;
    }
    float3 centerNormal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 sum = 0;
    float weightSum = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
    {
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            int2 samplePixel = clamp(int2(pixel) + int2(x, y), 0, int2(ReflectionExtent.xy) - 1);
            int2 storage = samplePixel;
            if (GiParameters.z > 0.5f)
            {
                if (((samplePixel.x + samplePixel.y + ReflectionExtent.z) & 1u) != 0)
                {
                    continue;
                }
                storage.x >>= 1; // packed checkerboard layout, see TraceDiffuse
            }
            float depth = ReflectionDepth.Load(int3(samplePixel, 0));
            float3 normal = normalize(ReflectionNormal.Load(int3(samplePixel, 0)).xyz * 2 - 1);
            float weight = exp2(-abs(depth - centerDepth) * 10000 - float(x*x + y*y) * 0.25f) * pow(saturate(dot(normal, centerNormal)), 32);
            float4 noisy = FilteredRadiance.Load(int3(storage, 0));
#if USE_NRD
            float3 sampleIrradiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisy).rgb;
#else
            float3 sampleIrradiance = noisy.rgb;
#endif
            sum += sampleIrradiance * weight;
            weightSum += weight;
        }
    }
    ReflectionRadiance[pixel] = float4(sum / max(weightSum, 0.00001f) * GiParameters.y, 1);
}

#if USE_NRD
[numthreads(8, 8, 1)]
void ComposeDiffuse(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(id.xy, 0));
    float3 irradiance = 0;
    if (depth < 0.99999f)
    {
        float3 normal = ReflectionNormal.Load(int3(id.xy, 0)).xyz * 2 - 1;
        float depthTolerance = max(0.00002f, (1 - depth) * 0.02f);
        float weightSum = 0;
        // NRD now receives a correctly packed checkerboard, so only a light 3x3 edge-aware
        // smoothing remains (was 5x5 as a workaround). Geometry weights keep neighbouring walls
        // and silhouettes separate, so nothing bleeds across objects.
        float3 centerIrradiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(
            FilteredRadiance.Load(int3(id.xy, 0))).rgb;
        float centerLuma = dot(centerIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                int2 pixel = clamp(int2(id.xy) + int2(x, y), 0, int2(ReflectionExtent.xy) - 1);
                float sampleDepth = ReflectionDepth.Load(int3(pixel, 0));
                if (sampleDepth >= 0.99999f)
                {
                    continue;
                }
                float3 sampleNormal = ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1;
                float geometryWeight = saturate(1 - abs(sampleDepth - depth) / depthTolerance) *
                    pow(saturate(dot(normal, sampleNormal)), 32);
                float spatialWeight = exp2(-float(x * x + y * y) * 0.35f);
                float3 sampleIrradiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(
                    FilteredRadiance.Load(int3(pixel, 0))).rgb;
                float sampleLuma = dot(sampleIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
                float lumaWeight = rcp(1.0f + abs(sampleLuma - centerLuma) /
                    max(0.25f + centerLuma * 0.35f, 0.0001f));
                float weight = geometryWeight * spatialWeight * lumaWeight;
                irradiance += sampleIrradiance * weight;
                weightSum += weight;
            }
        }
        irradiance /= max(weightSum, 0.00001f);
    }
    ReflectionRadiance[id.xy] = float4(irradiance * GiParameters.y, 1);
}
#endif


// ---------------------------------------------------------------------------------------------
// Radiance cache maintenance (one thread per cell).
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void ResolveRadianceCache(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= (uint)CacheParameters.z) return;
    uint key = CacheKeys.Load(slot * 4);
    if (key == 0u) return;
    uint4 accumulated = CacheAccumulation.Load4(slot * 32);
    if (accumulated.w > 0u)
    {
        float3 average = float3(accumulated.xyz) / (kCacheScale * accumulated.w);
        float4 resolved=CacheResolved[slot].Signal;
        float history = min(resolved.w + 1.0f, max(CacheParameters.w, 1.0f));
        resolved.rgb = lerp(resolved.rgb, average, 1.0f / history);
        resolved.w = history;
        CacheResolved[slot].Signal=resolved;
        CacheResolved[slot].Distance=float4(asfloat(CacheAccumulation.Load(slot*32+16)),0,0,0);
        CacheAccumulation.Store4(slot*32,uint4(0,0,0,0));
    CacheAccumulation.Store4(slot*32+16,uint4(0,0,0,0));
        CacheAges.Store(slot * 4, 0u);
        return;
    }
    uint age = CacheAges.Load(slot * 4) + 1u;
    if (age > kCacheMaxAge)
    {
        CacheKeys.Store(slot * 4, 0u);
        CacheResolved[slot]=(CacheValue)0;
        age = 0u;
    }
    CacheAges.Store(slot * 4, age);
}

[numthreads(64, 1, 1)]
void ClearRadianceCache(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= (uint)CacheParameters.z) return;
    CacheKeys.Store(slot * 4, 0u);
    CacheAccumulation.Store4(slot*32,uint4(0,0,0,0));
    CacheAccumulation.Store4(slot*32+16,uint4(0,0,0,0));
    CacheResolved[slot]=(CacheValue)0;
    CacheAges.Store(slot * 4, 0u);
}


// ---------------------------------------------------------------------------------------------
// ReSTIR GI passes (dispatched over the trace resolution).
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void ReSTIRTemporal(uint3 id : SV_DispatchThreadID)
{
    uint2 storage = id.xy;
    if (storage.x >= ReSTIRState.y || storage.y >= ReflectionExtent.y) return;
    uint2 pixel = ReSTIRStorageToPixel(storage);
    float3 position, normal;
    float viewDepth;
    uint index = storage.y * ReSTIRState.y + storage.x;
    if (!ReSTIRVisiblePoint(pixel, position, normal, viewDepth)) return;
    GiReservoir current = LoadReservoir(ReservoirCurrent, index);
    if (ReSTIRState.x == 0u) return;
    // Reproject with the GBuffer motion (current uv - previous uv).
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float2 previousUv = uv - GiVelocity.Load(int3(pixel, 0)).zw;
    if (any(previousUv < 0.0f) || any(previousUv >= 1.0f)) return;
    int2 previousPixel = int2(previousUv * ReflectionExtent.xy);
    uint2 previousStorage = uint2(GiParameters.z > 0.5f ? previousPixel.x >> 1 : previousPixel.x, previousPixel.y);
    previousStorage.x = min(previousStorage.x, ReSTIRState.y - 1);
    GiReservoir history = LoadReservoir(ReservoirHistory, previousStorage.y * ReSTIRState.y + previousStorage.x);
    if (history.M <= 0.0f || !ReSTIRSimilarSurface(viewDepth, normal, history.VisibleDepth, history.VisibleNormal)) return;
    history.M = min(history.M, ReSTIRParameters.y);
    uint state = ReflectionHash(index * 7919u + ReflectionExtent.z * 104729u);
    // Combine: w_i = p_hat_q(y_i) * W_i * M_i
    float targetCurrent = ReSTIRTarget(current, position, normal);
    float targetHistory = ReSTIRTarget(history, position, normal);
    float weightCurrent = targetCurrent * current.W * current.M;
    float weightHistory = targetHistory * history.W * history.M;
    float weightSum = weightCurrent + weightHistory;
    GiReservoir result = current;
    float selectedTarget = targetCurrent;
    if (weightSum > 0.0f && ReSTIRRandom(state) * weightSum < weightHistory)
    {
        result = history;
        selectedTarget = targetHistory;
    }
    result.M = current.M + history.M;
    result.W = selectedTarget > 0.0f ? weightSum / (result.M * selectedTarget) : 0.0f;
    result.VisibleDepth = viewDepth;
    result.VisibleNormal = normal;
    StoreReservoir(ReservoirCurrent, index, result);
}

// Jacobian of reusing the sample of a neighbour (visible point xr) at xq (ReSTIR GI, eq. 11).
float ReSTIRJacobian(GiReservoir sample, float3 neighbourPosition, float3 position)
{
    float3 toNeighbour = neighbourPosition - sample.Position;
    float3 toCenter = position - sample.Position;
    float distanceNeighbour = dot(toNeighbour, toNeighbour);
    float distanceCenter = dot(toCenter, toCenter);
    if (distanceCenter < 1e-8f || distanceNeighbour < 1e-8f) return 0.0f;
    float cosineNeighbour = abs(dot(sample.Normal, toNeighbour * rsqrt(distanceNeighbour)));
    float cosineCenter = abs(dot(sample.Normal, toCenter * rsqrt(distanceCenter)));
    float jacobian = (cosineCenter / max(cosineNeighbour, 1e-4f)) * (distanceNeighbour / distanceCenter);
    return (isnan(jacobian) || isinf(jacobian)) ? 0.0f : clamp(jacobian, 0.0f, 10.0f);
}

[numthreads(8, 8, 1)]
void ReSTIRSpatial(uint3 id : SV_DispatchThreadID)
{
    uint2 storage = id.xy;
    if (storage.x >= ReSTIRState.y || storage.y >= ReflectionExtent.y) return;
    uint2 pixel = ReSTIRStorageToPixel(storage);
    float3 position, normal;
    float viewDepth;
    if (!ReSTIRVisiblePoint(pixel, position, normal, viewDepth)) return;
    uint index = storage.y * ReSTIRState.y + storage.x;
    GiReservoir center = LoadReservoir(ReservoirCurrent, index);
    uint state = ReflectionHash(index * 31337u + ReflectionExtent.z * 7919u + 17u);
    float centerTarget = ReSTIRTarget(center, position, normal);
    GiReservoir result = center;
    float selectedTarget = centerTarget;
    float weightSum = centerTarget * center.W * center.M;
    float totalM = center.M;
    uint samples = (uint)ReSTIRParameters.z;
    float radius = ReSTIRParameters.w;
    float columnScale = GiParameters.z > 0.5f ? 0.5f : 1.0f;
    [loop] for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        float angle = ReSTIRRandom(state) * 6.2831853f;
        float distance = sqrt(ReSTIRRandom(state)) * radius;
        int2 offset = int2(round(float2(cos(angle) * distance * columnScale, sin(angle) * distance)));
        if (all(offset == 0)) continue;
        int2 neighbourStorage = int2(storage) + offset;
        if (any(neighbourStorage < 0) || neighbourStorage.x >= (int)ReSTIRState.y || neighbourStorage.y >= (int)ReflectionExtent.y) continue;
        uint neighbourIndex = neighbourStorage.y * ReSTIRState.y + neighbourStorage.x;
        GiReservoir neighbour = LoadReservoir(ReservoirCurrent, neighbourIndex);
        if (neighbour.M <= 0.0f || !ReSTIRSimilarSurface(viewDepth, normal, neighbour.VisibleDepth, neighbour.VisibleNormal)) continue;
        float3 neighbourPosition, neighbourNormal;
        float neighbourDepth;
        if (!ReSTIRVisiblePoint(ReSTIRStorageToPixel(uint2(neighbourStorage)), neighbourPosition, neighbourNormal, neighbourDepth)) continue;
        float jacobian = ReSTIRJacobian(neighbour, neighbourPosition, position);
        if (jacobian <= 0.0f) continue;
        float target = ReSTIRTarget(neighbour, position, normal) / jacobian;
        float weight = target * neighbour.W * neighbour.M;
        weightSum += weight;
        totalM += neighbour.M;
        if (weight > 0.0f && ReSTIRRandom(state) * weightSum < weight)
        {
            result = neighbour;
            selectedTarget = target;
        }
    }
    float W = selectedTarget > 0.0f ? weightSum / (totalM * selectedTarget) : 0.0f;
    float3 direction = result.Position - position;
    float sampleDistance = length(direction);
    direction /= max(sampleDistance, 0.0001f);
    // Visibility of the reused sample from this pixel (removes light leaking through walls).
    if (ReSTIRState.z != 0u && W > 0.0f && sampleDistance < 9000.0f)
    {
        RayDesc ray;
        ray.Origin = position + normal * SceneRayParameters.y;
        ray.Direction = direction;
        ray.TMin = SceneRayParameters.y * 0.25f;
        ray.TMax = max(sampleDistance - SceneRayParameters.y * 2.0f, ray.TMin + 0.001f);
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
        CountTelemetryRay(RAY_TELEMETRY_RESTIR_VISIBILITY);
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
        while (query.Proceed()) { }
        if (query.CommittedStatus() != COMMITTED_NOTHING) W = 0.0f;
    }
    float3 irradiance = result.Radiance * saturate(dot(normal, direction)) * W;
    float4 emissive = NoisyRadiance[storage]; // (emitter irradiance, primary hit distance) from TraceDiffuse
    irradiance = min(max(irradiance + emissive.rgb, 0.0f), 65504.0f);
    float hitDistance = min(sampleDistance, GiParameters.x);
#if USE_NRD
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewDepth, float3(3, 0.1f, 20), 1);
    NoisyRadiance[storage] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance, normalizedDistance, true);
#else
    NoisyRadiance[storage] = float4(irradiance, hitDistance);
#endif
}




