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
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1 : 0;
}

float3 ShadeReflectionHit(uint instance, uint primitive, float2 barycentrics, float3 position, float3 incoming,
    bool diffuseOnly = false, uint randomSeed = 0)
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
    float3 radiance = diffuse * EvaluatePhysicalSky(normal, 0.01f) * 0.12f +
        skyFresnel * EvaluatePhysicalSky(reflect(incoming, normal), max(0.001f, roughness * 0.02f));
    if (diffuseOnly)
    {
        radiance = 0;
    }
    else
    {
        radiance += SampleEmission(instance, uv);
    }
    int count = (int)ReflectionExtent.w;
    int sampledLight = int(ReflectionHash(randomSeed) % max(uint(count), 1u));
    [loop] for (int iteration = 0; iteration < (diffuseOnly ? min(count, 1) : count); ++iteration)
    {
        int i = diffuseOnly ? sampledLight : iteration;
        ReflectionLight light = ReflectionLights[i];
        float3 direction;
        float attenuation, volume;
        ResolveSingleLightCommon(position, light.Direction, light.PositionType, light.Extras, direction, attenuation, volume);
        float cosine = saturate(dot(normal, direction));
        if (cosine * attenuation < 0.00001f) continue;
        float range = light.PositionType.w < 0.5f ? SceneRayParameters.x :
            min(length(light.PositionType.xyz - position), SceneRayParameters.x);
        float shadow = 1;
        if (light.Flags.x != 0)
        {
            shadow = diffuseOnly ? TraceBounceVisibility(position, normal, direction, range) :
                TraceSceneShadow(position, normal, direction, range);
        }
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
        if (diffuseOnly)
        {
            response = diffuse * count;
        }
        radiance += response * light.Color.rgb * max(light.Color.a, 0) * cosine * attenuation * shadow;
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

// One triangle sampled uniformly on one emissive mesh. Area and inverse-square falloff
// turn authored radiance into incident irradiance; the visibility ray prevents wall leaks.
float3 SampleEmissiveIrradiance(float3 position, float3 normal, uint seed)
{
    uint emitterCount = uint(GiParameters.w);
    if (emitterCount == 0)
    {
        return 0;
    }
    uint instance = EmissiveMeshes[ReflectionHash(seed) % emitterCount];
    ReflectionGeometry mesh = ReflectionMeshes[instance];
    uint triangleCount = mesh.Surface.x;
    if (triangleCount == 0)
    {
        return 0;
    }
    uint primitive = ReflectionHash(seed + 1) % triangleCount;
    uint random = ReflectionHash(seed + 2);
    float2 sample = float2((random & 65535) + 0.5f, (random >> 16) + 0.5f) / 65536;
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
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        return 0;
    }
    float areaPdfInverse = 0.5f * twiceArea * triangleCount * emitterCount;
    return SampleEmission(instance, uv) * (cosine * emitterCosine * areaPdfInverse / max(distanceSquared, 0.0001f));
}

[numthreads(8, 8, 1)]
void TraceDiffuse(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    bool checkerboard = GiParameters.z > 0.5f;
    if (checkerboard)
    {
        pixel.x = 2 * pixel.x + ((pixel.y + ReflectionExtent.z) & 1u);
    }
    if (any(pixel >= ReflectionExtent.xy))
    {
        return;
    }
    float depth = ReflectionDepth.Load(int3(pixel, 0));
    NoisyRadiance[id.xy] = 0;
    if (depth >= 0.99999f)
    {
        return;
    }
    float2 uv = (pixel + 0.5f) / ReflectionExtent.xy;
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), ReflectionInvViewProjection);
    float3 position = world.xyz / world.w;
    float3 normal = normalize(ReflectionNormal.Load(int3(pixel, 0)).xyz * 2 - 1);
    uint seed = ReflectionHash(pixel.x + pixel.y * ReflectionExtent.x + ReflectionExtent.z * 104729u);
    float2 random = float2((seed & 65535) + 0.5f, (seed >> 16) + 0.5f) / 65536;
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
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    float hitDistance = ray.TMax;
    float3 incoming = 0;
    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        hitDistance = query.CommittedRayT();
        // Emission is estimated separately by area sampling to avoid double counting.
        incoming = ShadeReflectionHit(query.CommittedInstanceID(), query.CommittedPrimitiveIndex(),
            query.CommittedTriangleBarycentrics(), ray.Origin + direction * hitDistance, direction, true, seed);
    }
    else
    {
        incoming = EvaluatePhysicalSky(direction, 0.01f);
    }
    float3 irradiance = incoming * 3.14159265f + SampleEmissiveIrradiance(position, normal, seed + 17);
    irradiance = min(max(irradiance, 0), 65504);
#if USE_NRD
    float viewZ = abs(mul(float4(position, 1), ReflectionView).z);
    float normalizedDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3, 0.1f, 20), 1);
    NoisyRadiance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance, normalizedDistance, true);
#else
    NoisyRadiance[id.xy] = float4(irradiance, hitDistance);
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
            if (GiParameters.z > 0.5f && ((samplePixel.x + samplePixel.y + ReflectionExtent.z) & 1u) != 0)
            {
                continue;
            }
            float depth = ReflectionDepth.Load(int3(samplePixel, 0));
            float3 normal = normalize(ReflectionNormal.Load(int3(samplePixel, 0)).xyz * 2 - 1);
            float weight = exp2(-abs(depth - centerDepth) * 10000 - float(x*x + y*y) * 0.25f) * pow(saturate(dot(normal, centerNormal)), 32);
            int2 signalPixel = samplePixel;
            if (GiParameters.z > 0.5f)
            {
                signalPixel.x /= 2;
            }
            float4 noisy = FilteredRadiance.Load(int3(signalPixel, 0));
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
        // Remove residual checkerboard grain in the existing compose dispatch.
        // Geometry weights keep neighboring walls and silhouettes separate.
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
                float spatialWeight = float((2 - abs(x)) * (2 - abs(y)));
                float weight = geometryWeight * spatialWeight;
                float3 sampleIrradiance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(
                    FilteredRadiance.Load(int3(pixel, 0))).rgb;
                irradiance += sampleIrradiance * weight;
                weightSum += weight;
            }
        }
        irradiance /= max(weightSum, 0.00001f);
    }
    ReflectionRadiance[id.xy] = float4(irradiance * GiParameters.y, 1);
}
#endif
