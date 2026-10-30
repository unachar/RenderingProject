// Stage 2 H: offline reference path tracer (see
// docs/rendering-technology-stage2h-design.md). Progressive,
// seed-deterministic unidirectional path tracing with next-event
// estimation to analytic lights and Russian roulette.
//
// V1 SCOPE: the tool renders fixture scenes it builds itself
// (Cornell / metal / glass / emissive validation scenes), so scene
// data arrives as one combined position/index buffer plus
// per-instance transforms and per-instance analytic PBR materials.
// Evaluating the engine's packed scene materials is the follow-on
// step. The seed derivation mirrors
// ReferenceCapture::DeriveSeed (source/rendering/referencecapture.cpp)
// verbatim so CPU and GPU agree on reproducibility.

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<float4> InstanceTransforms0 : register(t1); // rows 0..2 + row 3 in the next buffer
StructuredBuffer<float4> InstanceTransforms1 : register(t2);
StructuredBuffer<uint4> InstanceInfo : register(t3); // x: geometry, y: material, zw: unused
StructuredBuffer<float4> MaterialData : register(t4); // 3 entries per material: albedo+metallic, emission+roughness, unused
StructuredBuffer<uint4> GeometryInfo : register(t5); // x: index start, y: vertex start, z: index count
StructuredBuffer<float3> Positions : register(t6);
StructuredBuffer<uint> Indices : register(t7);
StructuredBuffer<float4> Lights : register(t8); // 2 entries per light: (pos.xyz, type), (dirOrColor.xyz, intensity)
RWTexture2D<float4> Accumulation : register(u0);

cbuffer ReferenceConstants : register(b0)
{
    float4x4 InvViewProjection;
    float3 CameraPosition;
    uint SampleOffset;
    uint2 Resolution;
    uint SamplesInDispatch;
    uint MaxDepth;
    uint JobSeed;
    uint LightCount;
    float3 SkyRadiance;
    uint ReferencePadding;
};

// ---- RNG: splitmix32 stream seeded by ReferenceCapture::DeriveSeed
uint Mix32(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

struct Rng
{
    uint State;
    float Next()
    {
        State = Mix32(State + 0x9e3779b9u);
        return float(State >> 8) * (1.0f / 16777216.0f);
    }
    float2 Next2()
    {
        return float2(Next(), Next());
    }
};

static const float REF_PI = 3.14159265359f;

float3x3 InstanceObjectToWorld(uint instance)
{
    const float4 r0 = InstanceTransforms0[instance * 2 + 0];
    const float4 r1 = InstanceTransforms0[instance * 2 + 1];
    const float4 r2 = InstanceTransforms1[instance * 2 + 0];
    return float3x3(r0.xyz, r1.xyz, r2.xyz);
}

float3 InstanceTranslation(uint instance)
{
    return InstanceTransforms1[instance * 2 + 1].xyz;
}

struct HitSurface
{
    bool Hit;
    float3 Position;
    float3 Normal;
    uint Material;
    float Distance;
};

HitSurface TraceScene(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 1e-4f;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xff, ray);
    query.Proceed();
    HitSurface surface;
    surface.Hit = false;
    surface.Distance = tMax;
    surface.Material = 0;
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return surface;
    }
    const uint instance = query.CommittedInstanceID();
    const uint geometryIndex = InstanceInfo[instance].x;
    const uint4 geometry = GeometryInfo[geometryIndex];
    const uint primitive = query.CommittedPrimitiveIndex();
    const uint i0 = Indices[geometry.x + primitive * 3 + 0];
    const uint i1 = Indices[geometry.x + primitive * 3 + 1];
    const uint i2 = Indices[geometry.x + primitive * 3 + 2];
    const float3x3 objectToWorld = InstanceObjectToWorld(instance);
    const float3 translation = InstanceTranslation(instance);
    const float3 p0 = mul(Positions[geometry.y + i0], objectToWorld) + translation;
    const float3 p1 = mul(Positions[geometry.y + i1], objectToWorld) + translation;
    const float3 p2 = mul(Positions[geometry.y + i2], objectToWorld) + translation;
    float3 normal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(normal, direction) > 0.0f)
    {
        normal = -normal;
    }
    surface.Hit = true;
    surface.Distance = query.CommittedRayT();
    surface.Position = origin + direction * surface.Distance;
    surface.Normal = normal;
    surface.Material = InstanceInfo[instance].y;
    return surface;
}

bool Occluded(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 1e-3f;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 SampleCosineHemisphere(float2 xi, float3 normal)
{
    const float r = sqrt(xi.x);
    const float phi = 2.0f * REF_PI * xi.y;
    const float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - xi.x)));
    const float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

float DistributionGGX(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (REF_PI * denom * denom);
}

float3 SampleGGX(float2 xi, float3 normal, float roughness)
{
    const float a = roughness * roughness;
    const float phi = 2.0f * REF_PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    const float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    const float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * H.x + bitangent * H.y + normal * H.z);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    const float m = 1.0f - cosTheta;
    const float m5 = m * m * m * m * m;
    return F0 + (1.0f - F0) * m5;
}

float GeometrySmith1(float NdotX, float roughness)
{
    const float a = roughness * roughness;
    return 2.0f * NdotX / (NdotX + sqrt(a * a + (1.0f - a * a) * NdotX * NdotX));
}

// Direct lighting from the analytic light list (NEE, no MIS in v1).
float3 DirectLight(float3 position, float3 normal, float3 V, float3 albedo, float roughness, float metallic,
                   inout Rng rng)
{
    float3 result = float3(0.0f, 0.0f, 0.0f);
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    for (uint i = 0; i < LightCount; ++i)
    {
        const float4 lightA = Lights[i * 2 + 0];
        const float4 lightB = Lights[i * 2 + 1];
        if (lightA.w > 2.5f)
        {
            continue; // continuation slot of a quad light
        }
        if (lightA.w > 1.5f)
        {
            // Quad area light (type 2): solid-angle NEE. Slots:
            // A = (corner, 2), B = (edgeU, 0), C = (edgeV, 0),
            // D = (emission, intensity).
            const float4 lightC = Lights[i * 2 + 2];
            const float4 lightD = Lights[i * 2 + 3];
            ++i;
            const float3 edgeU = lightB.xyz;
            const float3 edgeV = lightC.xyz;
            const float3 normalRaw = cross(edgeU, edgeV);
            const float area = length(normalRaw);
            if (area <= 1e-8f)
            {
                continue;
            }
            const float3 lightNormal = normalRaw / area;
            const float2 quadSample = rng.Next2();
            const float3 lightPoint = lightA.xyz + edgeU * quadSample.x + edgeV * quadSample.y;
            const float3 toLightQ = lightPoint - position;
            const float distance2 = dot(toLightQ, toLightQ);
            const float distanceQ = sqrt(distance2);
            const float3 Lq = toLightQ / max(distanceQ, 1e-5f);
            const float cosLight = dot(lightNormal, -Lq);
            const float NdotLq = dot(normal, Lq);
            if (cosLight <= 0.0f || NdotLq <= 0.0f)
            {
                continue;
            }
            if (Occluded(position + normal * 1e-3f, Lq, distanceQ * 0.999f))
            {
                continue;
            }
            const float pdf = distance2 / (area * cosLight);
            const float3 Hq = normalize(Lq + V);
            const float NdotVq = max(dot(normal, V), 1e-4f);
            const float Dq = DistributionGGX(max(dot(normal, Hq), 0.0f), roughness);
            const float3 Fq = FresnelSchlick(max(dot(Hq, V), 0.0f), F0);
            const float Gq = GeometrySmith1(NdotVq, roughness) * GeometrySmith1(NdotLq, roughness);
            const float3 specularQ = Dq * Fq * Gq / max(4.0f * NdotVq * NdotLq, 1e-4f);
            const float3 diffuseQ = albedo * (1.0f - Fq) * (1.0f - metallic) / REF_PI;
            result += (diffuseQ + specularQ) * (lightD.xyz * lightD.w) * (NdotLq / pdf);
            continue;
        }
        float3 L;
        float lightDistance;
        float3 radiance;
        if (lightA.w < 0.5f)
        {
            // Directional: A.xyz = direction TO the light.
            L = normalize(lightA.xyz);
            lightDistance = 1e30f;
            radiance = lightB.xyz * lightB.w;
        }
        else
        {
            // Point: A.xyz = position.
            const float3 toLight = lightA.xyz - position;
            lightDistance = length(toLight);
            L = toLight / max(lightDistance, 1e-5f);
            radiance = lightB.xyz * lightB.w / max(lightDistance * lightDistance, 1e-4f);
        }
        const float NdotL = dot(normal, L);
        if (NdotL <= 0.0f)
        {
            continue;
        }
        if (Occluded(position + normal * 1e-3f, L, lightDistance * 0.999f))
        {
            continue;
        }
        const float3 H = normalize(L + V);
        const float NdotV = max(dot(normal, V), 1e-4f);
        const float NdotH = max(dot(normal, H), 0.0f);
        const float HdotV = max(dot(H, V), 0.0f);
        const float D = DistributionGGX(NdotH, roughness);
        const float3 F = FresnelSchlick(HdotV, F0);
        const float G = GeometrySmith1(NdotV, roughness) * GeometrySmith1(NdotL, roughness);
        const float3 specular = D * F * G / max(4.0f * NdotV * NdotL, 1e-4f);
        const float3 diffuse = albedo * (1.0f - F) * (1.0f - metallic) / REF_PI;
        result += (diffuse + specular) * radiance * NdotL;
    }
    return result;
}

float3 TracePath(float3 origin, float3 direction, inout Rng rng)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 rayOrigin = origin;
    float3 rayDirection = direction;
    for (uint depth = 0; depth < MaxDepth; ++depth)
    {
        const HitSurface surface = TraceScene(rayOrigin, rayDirection, 1e30f);
        if (!surface.Hit)
        {
            radiance += throughput * SkyRadiance;
            break;
        }
        const float4 mat0 = MaterialData[surface.Material * 3 + 0];
        const float4 mat1 = MaterialData[surface.Material * 3 + 1];
        const float4 mat2 = MaterialData[surface.Material * 3 + 2];
        const float3 albedo = mat0.rgb;
        const float metallic = mat0.w;
        const float3 emission = mat1.rgb;
        const float roughness = clamp(mat1.w, 0.03f, 1.0f);
        // NEE-registered light sources (mat2.x) contribute through
        // DirectLight; counting their emission on BSDF hits too
        // would double-count. Camera rays still see them. (MIS is
        // the recorded follow-up.)
        if (mat2.x < 0.5f || depth == 0u)
        {
            radiance += throughput * emission;
        }

        const float3 V = -rayDirection;
        radiance += throughput * DirectLight(surface.Position, surface.Normal, V, albedo, roughness, metallic, rng);

        // Sample the BSDF: GGX lobe with probability specularWeight,
        // cosine diffuse otherwise; each branch divides by its
        // selection probability (standard unbiased estimators:
        // specular weight = F * G * HdotV / (NdotV * NdotH)).
        const float specularWeight = lerp(0.04f, 1.0f, metallic);
        float3 nextDirection;
        if (rng.Next() < specularWeight)
        {
            const float3 H = SampleGGX(rng.Next2(), surface.Normal, roughness);
            nextDirection = reflect(rayDirection, H);
            const float NdotL = dot(nextDirection, surface.Normal);
            if (NdotL <= 0.0f)
            {
                break;
            }
            const float NdotV = max(dot(surface.Normal, V), 1e-4f);
            const float NdotH = max(dot(surface.Normal, H), 1e-4f);
            const float HdotV = max(dot(H, V), 0.0f);
            const float3 F = FresnelSchlick(HdotV, lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic));
            const float G = GeometrySmith1(NdotV, roughness) * GeometrySmith1(NdotL, roughness);
            throughput *= F * G * HdotV / (NdotV * NdotH) / max(specularWeight, 1e-4f);
        }
        else
        {
            nextDirection = SampleCosineHemisphere(rng.Next2(), surface.Normal);
            throughput *= albedo * (1.0f - metallic) / max(1.0f - specularWeight, 1e-4f);
        }
        rayOrigin = surface.Position + surface.Normal * 1e-3f;
        rayDirection = nextDirection;

        // Russian roulette after a few bounces.
        if (depth > 2)
        {
            const float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05f, 0.95f);
            if (rng.Next() > survival)
            {
                break;
            }
            throughput /= survival;
        }
    }
    return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Resolution.x || dispatchThreadId.y >= Resolution.y)
    {
        return;
    }
    const uint2 pixel = dispatchThreadId.xy;
    const uint pixelIndex = pixel.y * Resolution.x + pixel.x;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint s = 0; s < SamplesInDispatch; ++s)
    {
        const uint sampleIndex = SampleOffset + s;
        Rng rng;
        rng.State = Mix32(Mix32(JobSeed + 0x9e3779b9u ^ pixelIndex) ^ sampleIndex);
        const float2 jitter = rng.Next2();
        const float2 ndc = (float2(pixel) + jitter) / float2(Resolution) * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
        const float4 farPoint = mul(float4(ndc, 0.0f, 1.0f), InvViewProjection);
        const float3 direction = normalize(farPoint.xyz / farPoint.w - CameraPosition);
        sum += TracePath(CameraPosition, direction, rng);
    }
    const float4 previous = Accumulation[pixel];
    Accumulation[pixel] = float4(previous.rgb + sum, previous.a);
}
