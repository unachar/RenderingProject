#define SHADER_POSTPROCESS
#define SHADER_3D
#define SHADER_RAYTRACING
#define SKY_COMPUTE
#include "common.hlsl"
#include "SkyAtmosphere.hlsli"
#if USE_NRD
#include "../../External/NRD/sdk/Shaders/NRD.hlsli"
#endif

Texture2D<float> ReflectionDepth : register(t1);
Texture2D<float4> ReflectionNormal : register(t2);
Texture2D<float4> ReflectionMaterial : register(t3);
Texture2D<float4> FilteredRadiance : register(t4);
ByteAddressBuffer SceneVertices[] : register(t0, space1);
ByteAddressBuffer SceneIndices[] : register(t0, space2);
Texture2D<float4> SceneTextures[] : register(t0, space3);
SamplerState MaterialSampler : register(s0);
RWTexture2D<float4> NoisyRadiance : register(u0);
RWTexture2D<float4> ReflectionRadiance : register(u1);

struct ReflectionGeometry
{
    float4x4 World;
    float4x4 NormalToWorld;
    uint4 Layout; // vertex stride, index format (0/16/32), vertex offset, index offset
    float4 Material; // roughness, metallic, texture enabled, reserved
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
    uint4 ReflectionExtent; // width, height, sequence, unused
};

uint ReflectionHash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352d;
    value ^= value >> 15; value *= 0x846ca68b;
    return value ^ (value >> 16);
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

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming)
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
    float metallic = saturate(mesh.Material.y);
    float roughness = clamp(mesh.Material.x, 0.02f, 1);
    float3 f0 = lerp(0.04f, max(albedo, 0), metallic);
    float3 view = -incoming;
    float nDotV = max(dot(normal, view), 0.0001f);
    float3 skyFresnel = f0 + (1 - f0) * pow(1 - saturate(nDotV), 5);
    float3 diffuse = max(albedo, 0) * (1 - metallic) / 3.14159265f;
    float3 radiance = diffuse * EvaluatePhysicalSky(normal, 0.01f) * 0.12f +
        skyFresnel * EvaluatePhysicalSky(reflect(incoming, normal), max(0.001f, roughness * 0.02f));
    int count = (int)ReflectionExtent.w;
    [loop] for (int i = 0; i < count; ++i)
    {
        ReflectionLight light = ReflectionLights[i];
        float3 direction;
        float attenuation, volume;
        ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation, volume);
        float cosine = saturate(dot(normal, direction));
        if (cosine * attenuation < 0.00001f) continue;
        float range = light.PositionType.w < 0.5f ? SceneRayParameters.x :
            min(length(light.PositionType.xyz - position), SceneRayParameters.x);
        float shadow = light.Flags.x != 0 ? TraceSceneShadow(position, normal, direction, range) : 1;
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
        radiance += ((1 - fresnel) * diffuse + specular) * light.Color.rgb * max(light.Color.a, 0) * cosine * attenuation * shadow;
    }
    return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (any(pixel >= ReflectionExtent.xy)) return;
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    if (depth >= 0.99999f)
    {
        NoisyRadiance[pixel] = 0;
        ReflectionRadiance[pixel] = 0;
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 view = normalize(ReflectionCamera.xyz - position);
    float roughness = clamp(ReflectionMaterial.Load(int3(pixel, 0)).y, 0.02f, 1);
    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 0xffff) + 0.5f, (seed >> 16) + 0.5f) / 65536;
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
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
        while (query.Proceed()) { }
        hit = query.CommittedStatus() != COMMITTED_NOTHING;
        hitDistance = hit ? query.CommittedRayT() : 65504;
        radiance = hit ? ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(), ray.Origin + direction * hitDistance, direction) :
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
    NoisyRadiance[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normalizedDistance, true);
#else
    NoisyRadiance[pixel] = float4(radiance, hitDistance);
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
