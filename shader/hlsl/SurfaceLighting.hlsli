#ifndef SURFACE_LIGHTING_INCLUDED
#define SURFACE_LIGHTING_INCLUDED

struct SurfaceData
{
    float3 BaseColor;
    float3 NormalWS;
    float Metallic;
    float Roughness;
    float F0;
    float AO;
    float3 Emissive;
    float Opacity;
    float Transmission;
    float IOR;
    float Thickness;
    float3 AbsorptionColor;
};

SurfaceData BuildLightingSurface(float4 baseColor, float3 normalWS, float3 material, float4 emissionAO)
{
    SurfaceData surface = (SurfaceData)0;
    surface.BaseColor = baseColor.rgb;
    surface.NormalWS = normalWS;
    surface.Metallic = material.r;
    surface.Roughness = material.g;
    surface.F0 = material.b;
    surface.AO = emissionAO.a;
    surface.Emissive = emissionAO.rgb;
    surface.Opacity = saturate(baseColor.a);
    surface.IOR = 1.5f;
    return surface;
}

float IsMaterialClass(float value, float target)
{
    return abs(value - target) < 0.5f;
}

float ContactShadowPixelNoise(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

float2 DirectionalReceiverDepthGradient(
    float3 receiverWorldDx,
    float3 receiverWorldDy,
    float4x4 lightViewProjection)
{
    float3 lightDx = mul(
        float4(receiverWorldDx, 0.0f), lightViewProjection).xyz;
    float3 lightDy = mul(
        float4(receiverWorldDy, 0.0f), lightViewProjection).xyz;
    float2 uvDx = float2(lightDx.x * 0.5f, -lightDx.y * 0.5f);
    float2 uvDy = float2(lightDy.x * 0.5f, -lightDy.y * 0.5f);
    float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) <= 0.0000000001f)
    {
        return float2(0.0f, 0.0f);
    }

    float inverseDeterminant = rcp(determinant);
    float2 gradient = float2(
        (lightDx.z * uvDy.y - lightDy.z * uvDx.y) * inverseDeterminant,
        (uvDx.x * lightDy.z - uvDy.x * lightDx.z) * inverseDeterminant);

    return clamp(gradient, float2(-4.0f, -4.0f), float2(4.0f, 4.0f));
}

float DirectionalShadowComparisonBias(float4 shadowParams, float nDotL)
{
    return max(shadowParams.z, 0.0f) +
        max(shadowParams.w, 0.0f) * (1.0f - saturate(nDotL));
}

Texture2DArray<float4> ShadowTransmissionTexture : register(t35);
Texture2DArray<float> ShadowTransmissionDepth : register(t36);

float3 SampleShadowTransmission(float2 uv, float lightLayer, float receiverDepth)
{
    if (ShadowFilterParams.y < 0.5f || any(uv < 0) || any(uv >= 1)) return 1.0f;
    uint width, height, slices;
    ShadowTransmissionDepth.GetDimensions(width, height, slices);
    int2 pixel = int2(uv * float2(width, height));
    float3 transmission = 1.0f;
    [loop]
    for (int peel = 0; peel < 4; ++peel)
    {
        int slice = (int)lightLayer * 4 + peel;
        float depth = ShadowTransmissionDepth.Load(int4(pixel, slice, 0));
        if (depth < receiverDepth)
            transmission *= ShadowTransmissionTexture.Load(int4(pixel, slice, 0)).rgb;
    }
    return transmission;
}

float3 SampleConventionalShadowPcf9(
    float2 shadowUv,
    float shadowLayer,
    float texelSize,
    float currentDepth,
    float2 receiverDepthGradient,
    int filterRadius)
{
    const float2 kernel[9] =
    {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f), float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f), float2(0.0f, -1.0f),
        float2(0.7071068f, 0.7071068f), float2(-0.7071068f, 0.7071068f),
        float2(0.7071068f, -0.7071068f), float2(-0.7071068f, -0.7071068f)
    };
    filterRadius = clamp(filterRadius, 0, 3);
    int tapCount = filterRadius <= 0 ? 1 : 9;
    float3 visibility = 0.0f;
    [loop]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 offset = kernel[tap] * (float)max(filterRadius, 1) * texelSize;
        visibility += ShadowMapTexture.SampleCmpLevelZero(
            ShadowSampler,
            float3(shadowUv + offset, shadowLayer),
            currentDepth + dot(receiverDepthGradient, offset)) *
            SampleShadowTransmission(shadowUv + offset, shadowLayer, currentDepth + dot(receiverDepthGradient, offset));
    }
    return visibility / (float)tapCount;
}

bool ProjectVirtualShadowLevel(
    int level,
    float3 worldPos,
    out float3 lightNdc,
    out float2 virtualUv,
    out float2 physicalUv,
    out float levelInterior)
{
    float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
    if (clip.w <= 0.000001f)
    {
        lightNdc = 0.0f;
        virtualUv = 0.0f;
        physicalUv = 0.0f;
        levelInterior = 0.0f;
        return false;
    }

    lightNdc = clip.xyz / clip.w;
    virtualUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    bool resident;
    int2 localPage;
    physicalUv = VirtualShadowMapUvCommon(level, virtualUv, resident, localPage);
    if (!resident || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        levelInterior = 0.0f;
        return false;
    }

    levelInterior = VirtualShadowLevelInteriorCommon(level, virtualUv, worldPos);
    return true;
}

bool SampleVirtualShadowLevel(
    int level,
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float3 lightDir,
    int filterRadius,
    out float3 visibility,
    out float levelInterior)
{
    float3 lightNdc;
    float2 virtualUv;
    float2 centerPhysicalUv;
    if (!ProjectVirtualShadowLevel(
            level, worldPos, lightNdc, virtualUv, centerPhysicalUv, levelInterior))
    {
        visibility = 1.0f;
        return false;
    }

    float4 params = VirtualShadowParams[level];

    float nDotL = saturate(dot(normal, lightDir));
    float baseBias = DirectionalShadowComparisonBias(params, nDotL);
    float currentDepth = lightNdc.z - baseBias;
    float2 receiverDepthGradient = DirectionalReceiverDepthGradient(
        worldPosDx,
        worldPosDy,
        VirtualShadowViewProjections[level]);

    const float2 poisson[9] =
    {
        float2(0.0f, 0.0f),
        float2(0.286f, 0.088f),
        float2(-0.218f, 0.273f),
        float2(-0.153f, -0.394f),
        float2(0.424f, -0.309f),
        float2(0.523f, 0.334f),
        float2(-0.444f, 0.474f),
        float2(-0.666f, -0.126f),
        float2(0.137f, -0.712f)
    };

    filterRadius = clamp(filterRadius, 0, 3);
    int tapCount = filterRadius <= 0 ? 1 : 9;

    float pageGrid = max(VirtualShadowPageOrigins[level].z, 1.0f);
    float pageTexels = rcp(max(params.y * pageGrid, 0.000001f));
    float2 texelInPage = frac(virtualUv * pageGrid) * pageTexels;
    float edgeTexels = min(
        min(texelInPage.x, texelInPage.y),
        min(pageTexels - texelInPage.x, pageTexels - texelInPage.y));
    bool kernelInOnePage = edgeTexels > (float)filterRadius + 1.0f;

    float3 visibilitySum = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 offset = poisson[tap] * (float)max(filterRadius, 1) *
            ShadowWorldFilterScaleCommon(level);
        float2 tapVirtualUv = virtualUv + offset * params.y;
        float2 physicalUv;
        if (kernelInOnePage)
        {
            physicalUv = centerPhysicalUv + offset * params.y;
        }
        else
        {
            bool tapResident;
            int2 tapPage;
            physicalUv = VirtualShadowMapUvCommon(
                level,
                tapVirtualUv,
                tapResident,
                tapPage);
            if (!tapResident) continue;
        }

        visibilitySum += ShadowMapTexture.SampleCmpLevelZero(
            ShadowSampler,
            float3(physicalUv, max(params.x, 0.0f)),
            currentDepth + dot(
                receiverDepthGradient,
                tapVirtualUv - virtualUv)) * SampleShadowTransmission(physicalUv, max(params.x, 0.0f),
                    currentDepth + dot(receiverDepthGradient, tapVirtualUv - virtualUv));
        sampleCount += 1.0f;
    }

    if (sampleCount <= 0.0f)
    {
        visibility = 1.0f;
        return false;
    }
    visibility = visibilitySum / sampleCount;
    return true;
}

float3 SampleSurfaceShadowMap(
    int lightIndex,
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float3 lightDir,
    int filterRadius)
{
#ifdef SHADER_RAYTRACING
    if (SceneRayFlags.x > 0.5f)
        return SceneShadowForLightCommon(lightIndex, worldPos, normal, lightDir);
#endif
    float shadowLayer = LightShadowData[lightIndex].x;
    [branch]
    if (shadowLayer < -0.5f)
    {
        return 1.0f;
    }

    const bool directionalMultiLevel = LightShadowData[lightIndex].w < 0.0f;
    const bool virtualShadow =
        VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f &&
        directionalMultiLevel;
    if (virtualShadow)
    {
        int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
        float3 fineVisibility = 1.0f;
        float fineInterior = 1.0f;
        bool hasFine = false;

        [loop]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= levelCount) break;
            float3 levelVisibility;
            float levelInterior;
            if (!SampleVirtualShadowLevel(
                    level,
                    worldPos,
                    worldPosDx,
                    worldPosDy,
                    normal,
                    lightDir,
                    filterRadius,
                    levelVisibility,
                    levelInterior))
            {
                continue;
            }

            if (!hasFine)
            {
                fineVisibility = levelVisibility;
                fineInterior = levelInterior;
                hasFine = true;
                if (fineInterior >= 0.999f)
                {
                    return fineVisibility;
                }
                continue;
            }
            return lerp(levelVisibility, fineVisibility, fineInterior);
        }

        return hasFine ? lerp(1.0f, fineVisibility, fineInterior) : 1.0f;
    }

    if (VirtualShadowGlobal.x > 1.5f && directionalMultiLevel)
    {
        int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
        float3 fineVisibility = 1.0f;
        float fineInterior = 1.0f;
        bool hasFine = false;

        [loop]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= levelCount) break;
            float4 lightClip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
            if (lightClip.w <= 0.000001f) continue;

            float3 lightNdc = lightClip.xyz / lightClip.w;
            float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
            if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
                lightNdc.z < 0.0f || lightNdc.z > 1.0f)
            {
                continue;
            }

            float4 params = VirtualShadowParams[level];
            float baseBias = DirectionalShadowComparisonBias(
                params, dot(normal, lightDir));
            float currentDepth = lightNdc.z - baseBias;
            float2 receiverDepthGradient = DirectionalReceiverDepthGradient(
                worldPosDx,
                worldPosDy,
                VirtualShadowViewProjections[level]);
            float3 levelVisibility = SampleConventionalShadowPcf9(
                shadowUv,
                params.x,
                params.y * ShadowWorldFilterScaleCommon(level),
                currentDepth,
                receiverDepthGradient,
                filterRadius);
            float levelInterior = CascadedShadowLevelInteriorCommon(
                level, shadowUv, worldPos);

            if (!hasFine)
            {
                fineVisibility = levelVisibility;
                fineInterior = levelInterior;
                hasFine = true;
                if (fineInterior >= 0.999f)
                {
                    return fineVisibility;
                }
                continue;
            }
            return lerp(levelVisibility, fineVisibility, fineInterior);
        }

        return hasFine ? lerp(1.0f, fineVisibility, fineInterior) : 1.0f;
    }

    float4 lightClip = mul(float4(worldPos, 1.0f), LightViewProjections[lightIndex]);
    float3 lightNdc = lightClip.xyz / max(lightClip.w, 0.000001f);
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    bool inBounds =
        lightClip.w > 0.0f &&
        all(shadowUv >= 0.0f) && all(shadowUv <= 1.0f) &&
        lightNdc.z >= 0.0f && lightNdc.z <= 1.0f;
    if (!inBounds) return 1.0f;

    float texelSize = LightShadowData[lightIndex].y;
    float currentDepth = lightNdc.z -
        max(LightShadowData[lightIndex].z, 0.0f) -
        abs(LightShadowData[lightIndex].w) *
            (1.0f - saturate(dot(normal, lightDir)));
    return SampleConventionalShadowPcf9(
        shadowUv,
        shadowLayer,
        texelSize,
        currentDepth,
        float2(0.0f, 0.0f),
        filterRadius);
}

float LoadContactDepth(float2 uv)
{
    uint width;
    uint height;
    DepthTexture.GetDimensions(width, height);
    int2 pixel = clamp(
        int2(uv * float2(width, height)),
        int2(0, 0),
        int2((int)width - 1, (int)height - 1));
    return DepthTexture.Load(int3(pixel, 0));
}

float3 ReconstructContactWorldPosition(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(float4(ndc, depth, 1.0f), PPInvViewProjection);
    return world.xyz / max(abs(world.w), 0.000001f);
}

float SampleContactShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    if (ShadowRuntimeGlobal.x < 0.5f) return 1.0f;

    float cameraDistance = length(worldPos - PPCameraPos.xyz);
    if (cameraDistance >= 30.0f) return 1.0f;

    const int configuredSteps = clamp((int)round(ShadowRuntimeGlobal.z), 4, 24);
    const float distanceQuality = 1.0f - smoothstep(8.0f, 24.0f, cameraDistance);
    const int stepCount = clamp(
        (int)round(lerp(4.0f, (float)configuredSteps, distanceQuality)),
        4,
        configuredSteps);
    const float rayLength = max(ShadowRuntimeGlobal.y, 0.02f);
    const float stepLength = rayLength / (float)stepCount;
    const float3 origin = worldPos + normal * max(0.018f, stepLength * 0.55f);

    uint depthWidth;
    uint depthHeight;
    DepthTexture.GetDimensions(depthWidth, depthHeight);
    float3 receiverProjection = WorldToScreenUV(worldPos);
    float2 receiverPixel = floor(saturate(receiverProjection.xy) *
        float2((float)depthWidth, (float)depthHeight));
    const float jitter = ContactShadowPixelNoise(receiverPixel);

    float occlusion = 0.0f;
    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {

        float t = ((float)stepIndex + 0.15f + jitter * 0.70f) / (float)stepCount;
        float3 rayPosition = origin + lightDir * rayLength * t;
        float3 projected = WorldToScreenUV(rayPosition);
        if (projected.x <= 0.001f || projected.x >= 0.999f ||
            projected.y <= 0.001f || projected.y >= 0.999f ||
            projected.z <= 0.0f || projected.z >= 1.0f)
        {
            break;
        }

        float sceneDepth = LoadContactDepth(projected.xy);
        if (sceneDepth >= 0.9999f) continue;
        float3 sceneWorld = ReconstructContactWorldPosition(projected.xy, sceneDepth);
        float3 cameraToRay = rayPosition - PPCameraPos.xyz;
        float rayDistance = length(cameraToRay);
        float3 viewDirection = cameraToRay / max(rayDistance, 0.000001f);
        float sceneDistance = dot(sceneWorld - PPCameraPos.xyz, viewDirection);
        float depthDelta = rayDistance - sceneDistance;
        float thickness = 0.012f + stepLength * 0.75f + rayDistance * 0.0015f;
        float hit =
            smoothstep(0.0015f, thickness * 0.30f, depthDelta) *
            (1.0f - smoothstep(thickness * 0.76f, thickness, depthDelta));
        float weightedHit = hit * (1.0f - t * 0.65f);

        occlusion = 1.0f - (1.0f - occlusion) * (1.0f - weightedHit);
    }

    float distanceFade = 1.0f - smoothstep(18.0f, 30.0f, cameraDistance);
    return lerp(1.0f, 0.42f, saturate(occlusion * distanceFade));
}

float DistanceToBoxSdf(float3 samplePoint, float3 center, float3 extents)
{
    float3 q = abs(samplePoint - center) - extents;
    return length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f);
}

float SampleDistanceFieldShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    const int objectCount = min((int)round(DistanceFieldGlobal.x), 16);
    if (objectCount <= 0) return 1.0f;
    const int stepCount = clamp((int)round(DistanceFieldGlobal.z), 4, 24);
    const float maxDistance = max(DistanceFieldGlobal.y, 0.1f);
    float travel = 0.12f;
    const float3 origin = worldPos + normal * 0.04f;
    [loop]
    for (int stepIndex = 0; stepIndex < stepCount && travel < maxDistance; ++stepIndex)
    {
        const float3 samplePosition = origin + lightDir * travel;
        float closestDistance = maxDistance;
        [loop]
        for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex)
        {
            const float3 center = DistanceFieldData0[objectIndex].xyz;
            const float3 extents = DistanceFieldData1[objectIndex].xyz;
            const bool receiverObject = all(abs(worldPos - center) <= extents + 0.02f);
            if (!receiverObject)
            {
                closestDistance = min(closestDistance, DistanceToBoxSdf(samplePosition, center, extents));
            }
        }
        if (closestDistance < 0.025f) return 0.45f;
        travel += max(closestDistance, 0.05f);
    }
    return 1.0f;
}

// Radiance already includes this light's intensity, attenuation and visibility.
// =============================================================================================
// Render quality extensions
//   ・DFG LUT / multiple scattering GGX energy compensation (Fdez-Aguera 2019, Kulla-Conty)
//   ・Extended material models: clear coat, sheen (cloth), skin, hair, anisotropic GGX
//   ・Rect area lights with LTC (Heitz et al. 2016)
// =============================================================================================
#ifdef SURFACE_PBR_LUTS
Texture2D<float4> LtcMatrixLut : register(t42);
Texture2D<float2> LtcAmplitudeLut : register(t43);
#ifdef SKY_LUT
#define PbrLutSampler SkyLutSampler // same clamp sampler (s4)
#else
SamplerState PbrLutSampler : register(s4);
#endif
#endif

static const uint kShadingStandard = 0u;
static const uint kShadingClearCoat = 1u;
static const uint kShadingSheen = 2u;
static const uint kShadingSkin = 3u;
static const uint kShadingHair = 4u;
static const uint kShadingAnisotropic = 5u;

struct ShadingModelData
{
    uint Model;
    float Strength;
};
// Set by the deferred lighting pass per pixel (decoded from the shadow GBuffer alpha).
static ShadingModelData g_ShadingModel = { 0u, 0.0f };

ShadingModelData DecodeShadingModel(float encoded)
{
    uint value = (uint)round(saturate(encoded) * 255.0f);
    ShadingModelData data;
    data.Model = value >> 5u;
    data.Strength = (value & 31u) / 31.0f;
    return data;
}

bool QualityEnergyCompensation()
{
    return QualityFlags.x > 0.5f;
}

float2 PbrLutUv(float NdotV, float roughness)
{
    return (float2(sqrt(saturate(1.0f - NdotV)), saturate(roughness)) * 63.0f + 0.5f) / 64.0f;
}

// Split sum DFG: returns (A, B) with  Fss*Ess = F0 * A + B  and  Ess = A + B.
float2 SampleDfg(float NdotV, float roughness)
{
#ifdef SURFACE_PBR_LUTS
    float2 amplitude = LtcAmplitudeLut.SampleLevel(PbrLutSampler, PbrLutUv(NdotV, roughness), 0);
    return float2(amplitude.x - amplitude.y, amplitude.y);
#else
    // Karis 2014 analytic fit (forward / no LUT).
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
#endif
}

// Multiplier restoring the energy lost by single scattering microfacet models.
float3 SpecularEnergyCompensation(float3 F0, float NdotV, float roughness)
{
    if (!QualityEnergyCompensation()) return 1.0f;
    float2 dfg = SampleDfg(NdotV, roughness);
    float Ess = max(dfg.x + dfg.y, 0.05f);
    return 1.0f + F0 * (1.0f / Ess - 1.0f);
}

float3 SurfaceTangent(float3 N)
{
    // Strands / brushed direction follow the world up axis projected on the surface.
    float3 up = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    return SafeNormalizeCommon(up - N * dot(N, up), float3(1.0f, 0.0f, 0.0f));
}

float DistributionCharlie(float NdotH, float roughness)
{
    float alpha = max(roughness * roughness, 0.0001f);
    float invAlpha = 1.0f / alpha;
    float sin2 = max(1.0f - NdotH * NdotH, 0.0078125f);
    return (2.0f + invAlpha) * pow(sin2, invAlpha * 0.5f) / (2.0f * 3.14159265f);
}

float KajiyaKaySpecular(float3 T, float3 H, float exponent)
{
    float TdotH = dot(T, H);
    float sinTH = sqrt(max(1.0f - TdotH * TdotH, 0.0f));
    return smoothstep(-1.0f, 0.0f, TdotH) * pow(sinTH, exponent);
}

float3 EvaluatePbrDirectLight(
    float3 N, float3 V, float3 L, float3 radiance,
    float3 albedo, float3 materialData, float shadowSoftness)
{
    const float PI = 3.14159265359f;
    float3 H = SafeNormalizeCommon(L + V, N);

    float rawNdotL = dot(N, L);
    float NdotLScalar = saturate(rawNdotL);
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float metallic = saturate(materialData.r);
    float roughness = clamp(materialData.g, 0.04f, 1.0f);
    float specularF0 = saturate(materialData.b);

    float3 F0 = lerp(float3(specularF0, specularF0, specularF0), albedo, metallic);

    float a = roughness * roughness;
    float a2 = a * a;

    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    float D = a2 / max(PI * d * d, 0.00001f);

    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;

    float G_L = NdotLScalar / max(NdotLScalar * (1.0f - k) + k, 0.00001f);
    float G_V = NdotV / max(NdotV * (1.0f - k) + k, 0.00001f);
    float G = G_L * G_V;

    float oneMinusVdotH = 1.0f - VdotH;
    float oneMinusVdotH2 = oneMinusVdotH * oneMinusVdotH;
    float oneMinusVdotH5 = oneMinusVdotH2 * oneMinusVdotH2 * oneMinusVdotH;

    float3 F = F0 + (1.0f - F0) * oneMinusVdotH5;

    const uint model = g_ShadingModel.Model;
    const float strength = g_ShadingModel.Strength;
    if (model == kShadingAnisotropic && strength > 0.0f)
    {
        // Anisotropic GGX (Burley 2012) along the surface tangent.
        float3 T = SurfaceTangent(N);
        float3 B = cross(N, T);
        float aspect = sqrt(1.0f - 0.9f * strength);
        float at = max(a / aspect, 0.002f);
        float ab = max(a * aspect, 0.002f);
        float TdotH = dot(T, H);
        float BdotH = dot(B, H);
        float denominator = TdotH * TdotH / (at * at) + BdotH * BdotH / (ab * ab) + NdotH * NdotH;
        D = 1.0f / max(PI * at * ab * denominator * denominator, 0.00001f);
    }
    if (model == kShadingSkin && strength > 0.0f)
    {
        // Two lobe skin specular (Jimenez / d'Eon): sharp oil layer + broad epidermis.
        float roughA = max(roughness * 0.75f, 0.04f);
        float roughB = min(roughness * 1.35f, 1.0f);
        float aA = roughA * roughA, aB = roughB * roughB;
        float dA = NdotH * NdotH * (aA * aA - 1.0f) + 1.0f;
        float dB = NdotH * NdotH * (aB * aB - 1.0f) + 1.0f;
        float twoLobe = 0.85f * aA * aA / max(PI * dA * dA, 0.00001f) + 0.15f * aB * aB / max(PI * dB * dB, 0.00001f);
        D = lerp(D, twoLobe, strength);
    }

    float3 specularBRDF = D * G * F;
    specularBRDF /= max(4.0f * NdotLScalar * NdotV, 0.00001f);
    specularBRDF *= SpecularEnergyCompensation(F0, NdotV, roughness);

    float3 kS = F;
    float3 kD = 1.0f - kS;
    kD *= 1.0f - metallic;

    float3 diffuseBRDF = kD * albedo / PI;

    float terminatorWidth = lerp(
        0.08f,
        0.45f,
        saturate(shadowSoftness * 2.0f));
    float wrappedNdotL = saturate(
        (rawNdotL + terminatorWidth) / (1.0f + terminatorWidth));

    wrappedNdotL = wrappedNdotL * wrappedNdotL * (3.0f - 2.0f * wrappedNdotL);

    float3 diffuseTerm = diffuseBRDF * wrappedNdotL;
    float3 specularTerm = specularBRDF * NdotLScalar;
    float3 extra = 0.0f;

    if (model == kShadingSkin && strength > 0.0f)
    {
        // Pre-integrated style scattering: per channel wrap (red scatters farthest) keeps the
        // terminator soft and reddish; normalised so total energy is preserved.
        float3 wrap = float3(0.55f, 0.22f, 0.16f) * strength;
        float3 scattered = saturate((rawNdotL + wrap) / (1.0f + wrap)) / (1.0f + wrap);
        diffuseTerm = diffuseBRDF * lerp(wrappedNdotL.xxx, scattered, strength);
        // Thin regions (ears, fingers): light transmitted from behind.
        float3 transmitDirection = SafeNormalizeCommon(L + N * 0.35f, L);
        float transmission = pow(saturate(dot(V, -transmitDirection)), 4.0f) * saturate(-rawNdotL + 0.3f);
        extra += albedo * float3(1.0f, 0.35f, 0.22f) * transmission * strength * 0.35f;
    }
    else if (model == kShadingHair && strength > 0.0f)
    {
        // Kajiya-Kay with two shifted lobes (Scheuermann 2004): white primary, tinted secondary.
        float3 T = SurfaceTangent(N);
        float exponent = clamp(2.0f / max(a * a, 0.0004f), 4.0f, 512.0f);
        float primary = KajiyaKaySpecular(SafeNormalizeCommon(T + N * -0.08f, T), H, exponent);
        float secondary = KajiyaKaySpecular(SafeNormalizeCommon(T + N * 0.12f, T), H, exponent * 0.25f);
        float3 hairSpecular = (F * primary + albedo * secondary * 0.6f) * NdotLScalar * 0.25f;
        specularTerm = lerp(specularTerm, hairSpecular, strength);
        // Strands scatter light around the terminator.
        diffuseTerm = lerp(diffuseTerm, albedo / PI * saturate(rawNdotL * 0.75f + 0.25f) * (1.0f - metallic), strength);
    }
    else if (model == kShadingSheen && strength > 0.0f)
    {
        // Cloth sheen: Charlie distribution with the Neubelt visibility term.
        float sheenVisibility = 1.0f / max(4.0f * (NdotLScalar + NdotV - NdotLScalar * NdotV), 0.00001f);
        float3 sheenColor = lerp(float3(1.0f, 1.0f, 1.0f), albedo, 0.5f) * strength;
        extra += sheenColor * DistributionCharlie(NdotH, roughness) * sheenVisibility * NdotLScalar;
        diffuseTerm *= 1.0f - 0.25f * strength;
    }
    else if (model == kShadingClearCoat && strength > 0.0f)
    {
        // Clear coat: polyurethane layer, IOR 1.5, fixed low roughness, Kelemen visibility.
        const float coatAlpha = 0.06f * 0.06f;
        float dc = NdotH * NdotH * (coatAlpha * coatAlpha - 1.0f) + 1.0f;
        float coatD = coatAlpha * coatAlpha / max(PI * dc * dc, 0.00001f);
        float coatV = 0.25f / max(VdotH * VdotH, 0.0001f);
        float coatF = (0.04f + 0.96f * oneMinusVdotH5) * strength;
        diffuseTerm *= 1.0f - coatF;
        specularTerm *= 1.0f - coatF;
        extra += (coatD * coatV * coatF * NdotLScalar).xxx;
    }

    return (diffuseTerm + specularTerm + extra) * radiance;
}

// ---------------------------------------------------------------------------------------------
// Rect area light, LTC (Heitz 2016). Horizon clipping + Hill's edge integral.
// ---------------------------------------------------------------------------------------------
void RectLightBasis(float3 emitNormal, out float3 right, out float3 up)
{
    float3 axis = abs(emitNormal.y) > 0.99f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    right = normalize(cross(axis, emitNormal));
    up = cross(emitNormal, right);
}

float3 LtcIntegrateEdgeVector(float3 v1, float3 v2)
{
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985f + (0.4965155f + 0.0145206f * y) * y;
    float b = 3.4175940f + (4.1616724f + y) * y;
    float v = a / b;
    float thetaOverSin = x > 0.0f ? v : 0.5f * rsqrt(max(1.0f - x * x, 1e-7f)) - v;
    return cross(v1, v2) * thetaOverSin;
}

void LtcClipQuadToHorizon(inout float3 L[5], out int n)
{
    int config = 0;
    if (L[0].z > 0.0f) config += 1;
    if (L[1].z > 0.0f) config += 2;
    if (L[2].z > 0.0f) config += 4;
    if (L[3].z > 0.0f) config += 8;
    n = 0;
    if (config == 1) { n = 3; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if (config == 2) { n = 3; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if (config == 3) { n = 4; L[2] = -L[2].z * L[1] + L[1].z * L[2]; L[3] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if (config == 4) { n = 3; L[0] = -L[3].z * L[2] + L[2].z * L[3]; L[1] = -L[1].z * L[2] + L[2].z * L[1]; }
    else if (config == 6) { n = 4; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if (config == 7) { n = 5; L[4] = -L[3].z * L[0] + L[0].z * L[3]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if (config == 8) { n = 3; L[0] = -L[0].z * L[3] + L[3].z * L[0]; L[1] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = L[3]; }
    else if (config == 9) { n = 4; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[2].z * L[3] + L[3].z * L[2]; }
    else if (config == 11) { n = 5; L[4] = L[3]; L[3] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if (config == 12) { n = 4; L[1] = -L[1].z * L[2] + L[2].z * L[1]; L[0] = -L[0].z * L[3] + L[3].z * L[0]; }
    else if (config == 13) { n = 5; L[4] = L[3]; L[3] = L[2]; L[2] = -L[1].z * L[2] + L[2].z * L[1]; L[1] = -L[1].z * L[0] + L[0].z * L[1]; }
    else if (config == 14) { n = 5; L[4] = -L[0].z * L[3] + L[3].z * L[0]; L[0] = -L[0].z * L[1] + L[1].z * L[0]; }
    else if (config == 15) { n = 4; }
    if (n == 3) L[3] = L[0];
    if (n == 4) L[4] = L[0];
}

// Cosine weighted integral (form factor) of the quad transformed by minv.
float LtcEvaluateQuad(float3 N, float3 V, float3 P, float3x3 minv, float3 corners[4])
{
    float3 T1 = SafeNormalizeCommon(V - N * dot(V, N), SurfaceTangent(N));
    float3 T2 = cross(N, T1);
    float3x3 transform = mul(minv, float3x3(T1, T2, N));
    float3 L[5];
    L[0] = mul(transform, corners[0] - P);
    L[1] = mul(transform, corners[1] - P);
    L[2] = mul(transform, corners[2] - P);
    L[3] = mul(transform, corners[3] - P);
    L[4] = L[3];
    int n;
    LtcClipQuadToHorizon(L, n);
    if (n == 0) return 0.0f;
    L[0] = normalize(L[0]);
    L[1] = normalize(L[1]);
    L[2] = normalize(L[2]);
    L[3] = normalize(L[3]);
    L[4] = normalize(L[4]);
    float sum = LtcIntegrateEdgeVector(L[0], L[1]).z + LtcIntegrateEdgeVector(L[1], L[2]).z +
        LtcIntegrateEdgeVector(L[2], L[3]).z;
    if (n >= 4) sum += LtcIntegrateEdgeVector(L[3], L[4]).z;
    if (n == 5) sum += LtcIntegrateEdgeVector(L[4], L[0]).z;
    return abs(sum) / (2.0f * 3.14159265f);
}

// Returns outgoing radiance for emitter radiance 1 (diffuse + specular, standard GGX).
float3 EvaluateRectLightLtcTinted(float3 N, float3 V, float3 P, float3 center, float3 emitNormal,
    float width, float height, float3 albedo, float3 materialData, float3 diffuseTint, float3 specularTint)
{
    if (dot(P - center, emitNormal) <= 0.0f) return 0.0f; // one sided emitter
    float3 right, up;
    RectLightBasis(emitNormal, right, up);
    right *= width * 0.5f;
    up *= height * 0.5f;
    float3 corners[4] = { center + right + up, center + right - up, center - right - up, center - right + up };
    float metallic = saturate(materialData.r);
    float roughness = clamp(materialData.g, 0.04f, 1.0f);
    float specularF0 = saturate(materialData.b);
    float3 F0 = lerp(specularF0.xxx, albedo, metallic);
    float NdotV = saturate(dot(N, V));
    float diffuse = LtcEvaluateQuad(N, V, P, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), corners);
    float3 specular = 0.0f;
#ifdef SURFACE_PBR_LUTS
    float2 uv = PbrLutUv(NdotV, roughness);
    float4 t1 = LtcMatrixLut.SampleLevel(PbrLutSampler, uv, 0);
    float2 t2 = LtcAmplitudeLut.SampleLevel(PbrLutSampler, uv, 0);
    float3x3 minv = float3x3(t1.x, 0.0f, t1.y, 0.0f, 1.0f, 0.0f, t1.z, 0.0f, t1.w);
    float3 FssEss = F0 * (t2.x - t2.y) + t2.y;
    specular = LtcEvaluateQuad(N, V, P, minv, corners) * FssEss * SpecularEnergyCompensation(F0, NdotV, roughness);
#endif
    float3 kD = (1.0f - F0) * (1.0f - metallic);
    return albedo * kD * diffuse * diffuseTint + specular * specularTint;
}

float3 EvaluateRectLightLtc(float3 N, float3 V, float3 P, float3 center, float3 emitNormal,
    float width, float height, float3 albedo, float3 materialData)
{
    return EvaluateRectLightLtcTinted(N, V, P, center, emitNormal, width, height, albedo, materialData, 1.0f, 1.0f);
}


// ---------------------------------------------------------------------------------------------
// MegaLights M7：テクスチャ付き矩形ライト
//   Rect の Extra.w = テクスチャ番号 + 1（エンジンのテクスチャ SRV の並び。0 = なし）
//   LTC の解析解に掛ける色を、拡散は法線方向・鏡面は反射方向が発光面と交わる点のテクスチャで近似する
//   （Heitz 2016 の textured LTC の簡易版。ミップは距離と粗さから決める）
// ---------------------------------------------------------------------------------------------
#ifdef MEGALIGHTS_LIGHT_TEXTURES
Texture2D<float4> MegaLightTextures[512] : register(t0, space5);

float3 MLSampleRectTexture(uint textureIndex, float3 center, float3 emitNormal, float width, float height,
    float3 origin, float3 direction, float footprint)
{
    float3 right, up;
    RectLightBasis(emitNormal, right, up);
    // 発光面との交点（交わらないときは面への最近点）
    const float denominator = dot(direction, -emitNormal);
    float3 hit = origin + direction * max(dot(origin - center, emitNormal) / max(denominator, 1.0e-4f), 0.0f);
    if (denominator <= 1.0e-4f) hit = origin - emitNormal * dot(origin - center, emitNormal);
    const float2 uv = saturate(float2(dot(hit - center, right) / max(width, 1.0e-3f), -dot(hit - center, up) / max(height, 1.0e-3f)) + 0.5f);
    uint textureWidth, textureHeight, mipCount;
    MegaLightTextures[NonUniformResourceIndex(textureIndex)].GetDimensions(0, textureWidth, textureHeight, mipCount);
    const float texels = footprint / max(max(width, height), 1.0e-3f) * (float)max(textureWidth, textureHeight);
    const float lod = clamp(log2(max(texels, 1.0f)), 0.0f, (float)max(mipCount, 1u) - 1.0f);
    return MegaLightTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(TextureSampler, uv, lod).rgb;
}

float3 MLRectTextureMean(uint textureIndex)
{
    uint textureWidth, textureHeight, mipCount;
    MegaLightTextures[NonUniformResourceIndex(textureIndex)].GetDimensions(0, textureWidth, textureHeight, mipCount);
    return MegaLightTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(TextureSampler, float2(0.5f, 0.5f), (float)max(mipCount, 1u) - 1.0f).rgb;
}
#endif
// ---------------------------------------------------------------------------------------------
// 1 ライト分の評価（従来のライトループの本体を関数化。結果は acc に加算）
//   cbIndex >= 0 : LightParams 配列のライト。シャドウマップ / VSM / DXR 影 / コンタクト影 / 雲影は従来どおり
//   cbIndex <  0 : MegaLights グリッドのローカルライト
//   sampleWeight      : 寄与に掛ける重み（M2 では 1/(N·p)。それ以外は 1）
//   visibilityOverride: >= 0 なら影の計算をせずこの可視率を使う（M2 の影レイ / リファレンス）
// ---------------------------------------------------------------------------------------------
struct SurfaceLightAccumulator
{
    float3 PbrDirect;
    float3 DirSum;
    float3 ColorSum;
    float WeightSum;
    float MaxAttenuation;
    float VolumeSum;
    float RangeSum;
    float ShadowSum;
};

void AccumulateSurfaceLight(
    int cbIndex,
    float4 lightDirection,
    float4 lightPositionType,
    float4 lightExtra,
    float4 lightColorData,
    bool decalLight,
    bool gridCastShadow,
    float sampleWeight,
    float visibilityOverride,
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float2 pixelPosition,
    int shadowFilterRadius,
    bool evaluatePbr,
    float3 viewDir,
    float3 albedo,
    float3 materialData,
    float shadowSoftness,
    float3 monitorColor,
    inout SurfaceLightAccumulator acc)
{
        float3 singleDir;
        float singleAttenuation;
        float singleVolume;
        ResolveSingleLightCommon(worldPos, lightDirection, lightPositionType, lightExtra, singleDir, singleAttenuation, singleVolume);

		float lightIntensityValue = max(lightColorData.a, 0.0f);
		[branch]
		if (singleAttenuation <= 0.000001f || lightIntensityValue <= 0.000001f)
		{
			return;
		}

        float3 shadowVisibility = 1.0f;
        [branch]
        if (visibilityOverride >= 0.0f)
        {
            // MegaLights M2 / リファレンス：影レイの結果（可視率）が与えられている
            shadowVisibility = visibilityOverride;
        }
        else if (cbIndex >= 0)
        {
            const int i = cbIndex;
            const bool directionalShadow =
                LightPositionTypes[i].w < 0.5f && SceneLightCastsShadowCommon(i, LightShadowData[i].x >= -0.5f);
            const bool useRayShadow = !decalLight && directionalShadow &&
                RaytracingFlags.x > 0.5f && RaytracingFlags.z > 0.5f &&
                RaytracingSunDirection.w > 0.5f && dot(singleDir, RaytracingSunDirection.xyz) > 0.999f;
            // The primary sun already has a denoised visibility texture. Do not
            // issue an inline ray whose result would immediately be overwritten.
            shadowVisibility = decalLight || useRayShadow
    			? 1.0f
    			: SampleSurfaceShadowMap(
                    i,
                    worldPos,
                    worldPosDx,
                    worldPosDy,
                    normal,
                    singleDir,
                    shadowFilterRadius);

            float contactVisibility = !decalLight && directionalShadow
                ? SampleContactShadow(worldPos, normal, singleDir)
                : 1.0f;
#ifdef SHADER_RAYTRACING
            if (SceneRayFlags.x > 0.5f) contactVisibility = 1.0f;
#endif
            shadowVisibility *= lerp(1.0f, contactVisibility, 0.45f);
    		if (useRayShadow)
    		{
    			uint rayWidth;
    			uint rayHeight;
    			RaytracedVisibility.GetDimensions(rayWidth, rayHeight);
    			shadowVisibility = RaytracedVisibility.SampleLevel(TextureSampler, pixelPosition / float2(rayWidth, rayHeight), 0).r;
    		}
    		bool useLegacyDistanceShadow = !useRayShadow && !decalLight && directionalShadow;
#ifdef SHADER_RAYTRACING
            useLegacyDistanceShadow = useLegacyDistanceShadow && SceneRayFlags.x < 0.5f;
#endif
    		if (useLegacyDistanceShadow)
    		{
    			shadowVisibility *= SampleDistanceFieldShadow(worldPos, normal, singleDir);
    		}
            // Apply after raster/DXR visibility so neither path can overwrite cloud extinction.
            if (!decalLight && i == CloudSunIndex())
                shadowVisibility *= CloudSunVisibility(worldPos);
        }
#ifdef SHADER_RAYTRACING
        else if (gridCastShadow && SceneRayFlags.x > 0.5f && SceneRayFlags.z > 0.5f)
        {
            // MegaLights M1 グリッドのライト：DXR 影モードならライト中心へ影のレイを 1 本
            const float maximumDistance = min(length(lightPositionType.xyz - worldPos), SceneRayParameters.x);
            shadowVisibility = TraceSceneShadow(worldPos, normal, singleDir, maximumDistance);
        }
#endif
        float3 authoredLightColor = max(lightColorData.rgb, float3(0.0f, 0.0f, 0.0f));
#ifdef MEGALIGHTS_LIGHT_TEXTURES
        // M7：テクスチャ付き矩形ライト。非 PBR の陰影・平均ライトにはテクスチャの平均色を使う
        const uint lightTexture = (cbIndex < 0 && lightPositionType.w > 3.5f) ? (uint)round(lightExtra.w) : 0u;
        float3 textureMean = 1.0f;
        if (lightTexture != 0u)
        {
            textureMean = max(MLRectTextureMean(lightTexture - 1u), 1.0e-4f);
            authoredLightColor *= textureMean;
        }
#endif
		if (decalLight)
		{

			authoredLightColor *= monitorColor;
		}
        float3 singleColor = authoredLightColor * lightIntensityValue * singleAttenuation * shadowVisibility;
        if (!decalLight)
		{
			singleColor = ApplyAtmosphereToLightCommon(worldPos, singleDir, singleColor, singleVolume);
		}

        if (evaluatePbr)
        {
#ifdef SURFACE_PBR_LUTS
            const bool rectLight = !decalLight && lightPositionType.w > 3.5f && QualityFlags.y > 0.5f;
#else
            const bool rectLight = false;
#endif
            [branch]
            if (rectLight)
            {
                float3 toCenter = lightPositionType.xyz - worldPos;
                float rangeFade = saturate(1.0f - length(toCenter) / max(lightDirection.w, 0.01f));
                float3 emitterRadiance = authoredLightColor * lightIntensityValue * shadowVisibility * rangeFade * rangeFade;
                const float3 emitNormal = SafeNormalizeCommon(lightDirection.xyz, float3(0.0f, -1.0f, 0.0f));
                float3 diffuseTint = 1.0f;
                float3 specularTint = 1.0f;
#ifdef MEGALIGHTS_LIGHT_TEXTURES
                if (lightTexture != 0u)
                {
                    // 平均色は emitterRadiance に入っているので、ここでは平均との比を掛ける
                    const float distance = length(toCenter);
                    const float roughness = clamp(materialData.g, 0.04f, 1.0f);
                    diffuseTint = MLSampleRectTexture(lightTexture - 1u, lightPositionType.xyz, emitNormal, lightExtra.x, lightExtra.y,
                        worldPos, normal, distance * 2.0f) / textureMean;
                    specularTint = MLSampleRectTexture(lightTexture - 1u, lightPositionType.xyz, emitNormal, lightExtra.x, lightExtra.y,
                        worldPos, reflect(-viewDir, normal), distance * roughness * roughness * 2.0f) / textureMean;
                }
#endif
                acc.PbrDirect += sampleWeight * EvaluateRectLightLtcTinted(normal, viewDir, worldPos, lightPositionType.xyz,
                    emitNormal, lightExtra.x, lightExtra.y, albedo, materialData, diffuseTint, specularTint) * emitterRadiance;
            }
            else
            {
                acc.PbrDirect += sampleWeight * EvaluatePbrDirectLight(
                    normal, viewDir, singleDir, singleColor, albedo, materialData, shadowSoftness);
            }
        }

        float weight = max(dot(authoredLightColor * lightIntensityValue, float3(0.299f, 0.587f, 0.114f)), 0.0001f) * singleAttenuation * sampleWeight;
        acc.DirSum += singleDir * weight;
        acc.ColorSum += singleColor * sampleWeight;
        acc.WeightSum += weight;
        acc.MaxAttenuation = max(acc.MaxAttenuation, singleAttenuation);
        float visibilityLuminance = dot(shadowVisibility, float3(0.2126f, 0.7152f, 0.0722f));
        acc.VolumeSum += singleVolume * lightIntensityValue * visibilityLuminance;
        acc.RangeSum += saturate((max(lightDirection.w, 1.0f) - 1.0f) / 7.0f) * weight;
        acc.ShadowSum += visibilityLuminance * weight;
}

#if defined(MEGALIGHTS_GRID) && !defined(MEGALIGHTS_FORWARD)
// ---------------------------------------------------------------------------------------------
// MegaLights M2 / M3（ディファードのみ）
// ---------------------------------------------------------------------------------------------
void MLAccumulateSampleSet(uint2 samplePixel, float weight,
    float3 worldPos, float3 worldPosDx, float3 worldPosDy, float3 normal, float2 pixelPosition, int shadowFilterRadius,
    float3 viewDir, float3 albedo, float3 materialData, float shadowSoftness, float3 monitorColor,
    inout SurfaceLightAccumulator acc)
{
    const uint sampleCount = MLStochastic.y;
    const uint sampleBase = MLSampleBase(samplePixel);
    const float scale = weight / (float)max(sampleCount, 1u);
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const uint4 lightSample = MegaLightSamples[sampleBase + sampleIndex];
        if ((lightSample.x & ML_SAMPLE_VALID) == 0u) continue;
        const MegaLightData sampledLight = MegaLightBuffer[lightSample.x & ML_SAMPLE_INDEX];
        const float visibility = (lightSample.x & ML_SAMPLE_VISIBLE) != 0u ? 1.0f : 0.0f;
        AccumulateSurfaceLight(-1, sampledLight.DirectionRange, sampledLight.PositionType, sampledLight.Extra,
            sampledLight.Color, false, false, asfloat(lightSample.y) * scale, visibility,
            worldPos, worldPosDx, worldPosDy, normal, pixelPosition, shadowFilterRadius, true,
            viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
    }
}

// N 個のサンプルを 1/(N·p) の重みと影レイの可視率で評価（ML6）
//   半解像度：このピクセルを囲む 2x2 のサンプル空間のピクセルを、深度・法線が近いものだけ重み付きで使う。
//   各サンプルはこのピクセルの位置・法線・材質で評価し直す（選択確率と可視率は代表ピクセルのもの = 近似）
void MLAccumulateStochasticSamples(
    float3 worldPos, float3 worldPosDx, float3 worldPosDy, float3 normal, float2 pixelPosition, int shadowFilterRadius,
    float3 viewDir, float3 albedo, float3 materialData, float shadowSoftness, float3 monitorColor,
    inout SurfaceLightAccumulator acc)
{
    const uint2 pixel = uint2(pixelPosition);
    if (MLSampleScale() == 1u)
    {
        MLAccumulateSampleSet(pixel, 1.0f, worldPos, worldPosDx, worldPosDy, normal, pixelPosition, shadowFilterRadius,
            viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
        return;
    }
    const float viewZ = mul(float4(worldPos, 1.0f), MLWorldToView).z;
    // サンプルの代表ピクセルを基準にした双線形の位置
    const uint2 offsets[4] = { uint2(0u, 0u), uint2(1u, 1u), uint2(1u, 0u), uint2(0u, 1u) };
    const float2 position = (float2(pixel) - float2(offsets[MLSampleGrid.w & 3u])) * 0.5f;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float weights[4];
    uint2 candidates[4];
    float weightSum = 0.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const int2 c = base + int2(index & 1u, index >> 1u);
        weights[index] = 0.0f;
        candidates[index] = uint2(clamp(c, int2(0, 0), int2(MLSampleGrid.xy) - 1));
        if (any(c < 0) || any(c >= int2(MLSampleGrid.xy))) continue;
        const uint2 q = MLSampleToScreen(candidates[index]);
        const float qDepth = DepthTexture.Load(int3(q, 0));
        if (!MLIsGeometryDepth(qDepth)) continue;
        const float relative = abs(MLViewZFromDepth(qDepth) - viewZ) / max(viewZ, 1.0e-3f);
        const float3 qNormal = DecodeGBufferNormal(NormalTexture.Load(int3(q, 0)).xyz);
        const float bilinear = ((index & 1u) ? f.x : 1.0f - f.x) * ((index >> 1u) ? f.y : 1.0f - f.y);
        weights[index] = (bilinear + 1.0e-3f) * saturate(1.0f - relative / 0.05f) * pow(saturate(dot(qNormal, normal)), 16.0f);
        weightSum += weights[index];
    }
    if (weightSum <= 1.0e-5f)
    {
        // 近いサンプルが無い（細い物体・境界）：このピクセルを含むサンプルだけを使う
        MLAccumulateSampleSet(MLScreenToSample(pixel), 1.0f, worldPos, worldPosDx, worldPosDy, normal, pixelPosition,
            shadowFilterRadius, viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
        return;
    }
    [loop]
    for (uint candidate = 0u; candidate < 4u; ++candidate)
    {
        if (weights[candidate] <= 0.0f) continue;
        MLAccumulateSampleSet(candidates[candidate], weights[candidate] / weightSum, worldPos, worldPosDx, worldPosDy, normal,
            pixelPosition, shadowFilterRadius, viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
    }
}

// デモジュレーション係数：拡散アルベド + 鏡面の方向アルベド（DFG の split sum）
//   デノイズはこれで割った値（照明だけ）をぼかし、ディファードで掛け戻す → テクスチャの模様がぼけない
float3 MLRemodulation(float3 N, float3 V, float3 albedo, float3 materialData)
{
    const float metallic = saturate(materialData.r);
    const float roughness = clamp(materialData.g, 0.04f, 1.0f);
    const float3 F0 = lerp(saturate(materialData.b).xxx, albedo, metallic);
    const float2 dfg = SampleDfg(saturate(dot(N, V)), roughness);
    const float3 reflectance = albedo * (1.0f - metallic) + F0 * dfg.x + dfg.y;
    return max(reflectance, 0.02f);
}
#endif // MEGALIGHTS_GRID && !MEGALIGHTS_FORWARD

void ResolveSurfaceLightAggregateShadowed(
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float2 pixelPosition,
    int shadowFilterRadius,
    bool evaluatePbr,
    float3 viewDir,
    float3 albedo,
    float3 materialData,
    float shadowSoftness,
    out float3 pbrDirectLight,
    out float3 lightDir,
    out float3 lightColor,
    out float attenuation,
    out float volumeScatter,
    out float rangeBlend,
    out float aggregateShadowVisibility)
{
    pbrDirectLight = 0.0f;
    lightDir = SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f);
    attenuation = 1.0f;
    volumeScatter = 0.0f;
    rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
    aggregateShadowVisibility = 1.0f;

    int count = min((int)round(LightCount.x), MAX_SHADER_LIGHTS);
    SurfaceLightAccumulator acc = (SurfaceLightAccumulator)0;
#ifdef MEGALIGHTS_GRID
#ifdef MEGALIGHTS_FORWARD
    // M7：Forward / 半透明（グリッドが Forward 用に構築されたフレームだけ）
    const bool gridActive = MLForwardGridActive() && g_MLCellIndex != ML_INVALID_CELL;
#else
    const bool gridActive = MLGridActive() && g_MLCellIndex != ML_INVALID_CELL;
#endif
#endif

    bool useLightGrid = HasLightTileGridCommon();
    uint tileBase = useLightGrid ? LightTileBaseCommon(pixelPosition) : 0u;
    int iterationCount = useLightGrid
        ? min(MAX_LIGHTS_PER_TILE, max((int)round(LightCount.w), 1))
        : count;
    uint lightingWidth;
    uint lightingHeight;
    BaseColorTexture.GetDimensions(lightingWidth, lightingHeight);
    const float2 monitorUv = pixelPosition /
        float2(max(lightingWidth, 1u), max(lightingHeight, 1u));
    const float3 monitorColor = MonitorTexture.SampleLevel(TextureSampler, monitorUv, 0).rgb;
    [loop]
    for (int iteration = 0; iteration < MAX_SHADER_LIGHTS; ++iteration)
    {
        if (iteration >= iterationCount)
        {
            break;
        }
        uint lightIndex = useLightGrid
            ? LightTileIndices[tileBase + (uint)iteration]
            : (uint)iteration;
        if (lightIndex == 0xffffffffu || lightIndex >= (uint)count ||
            LightFlags[lightIndex].x < 0.5f || LightFlags[lightIndex].w >= 1.5f)
        {
            continue;
        }
#ifdef MEGALIGHTS_GRID
        // LightFlags.x == 2 : 不透明面はグリッドが受け持つ（グリッドが有効なフレームだけ二重評価を避ける）
        if (gridActive && LightFlags[lightIndex].x > 1.5f)
        {
            continue;
        }
#endif

        AccumulateSurfaceLight((int)lightIndex, LightDirections[lightIndex], LightPositionTypes[lightIndex],
            LightExtras[lightIndex], LightColors[lightIndex], LightFlags[lightIndex].w >= 0.5f, false, 1.0f, -1.0f,
            worldPos, worldPosDx, worldPosDy, normal, pixelPosition, shadowFilterRadius, evaluatePbr,
            viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
    }

    uint gridLightCount = 0u;
#ifdef MEGALIGHTS_GRID
    [branch]
    if (gridActive)
    {
        const uint2 cell = MegaLightCells[g_MLCellIndex];
        gridLightCount = cell.y;
#ifndef MEGALIGHTS_FORWARD
        [branch]
        if (evaluatePbr && MLDenoisedActive())
        {
            // MegaLights M3：ML6 の結果（デモジュレート済み）をデノイズしたものに、材質の反射率を掛け戻す
            const uint2 denoisePixel = min(uint2(pixelPosition), MLScreen.xy - 1u);
            const float4 denoised = MegaLightDenoised[denoisePixel.y * MLScreen.x + denoisePixel.x];
            acc.PbrDirect += max(denoised.rgb, 0.0f) * MLRemodulation(normal, viewDir, albedo, materialData);
        }
        else if (evaluatePbr && MLStochasticActive())
        {
            // MegaLights M2（ML6）：ML3 で選んだ N 個のサンプルだけを 1/(N·p) の重みで評価する（不偏推定）
            MLAccumulateStochasticSamples(worldPos, worldPosDx, worldPosDy, normal, pixelPosition, shadowFilterRadius,
                viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
        }
        else
#endif
        {
            // MegaLights M1：このピクセルのセル（16px タイル × 奥行きスライス）に入っているローカルライトを全評価
            //（M7 の Forward / 半透明もここ。Forward に影響しないライトは飛ばす）
#ifdef MEGALIGHTS_FORWARD
            const bool reference = false;
#else
            const bool reference = evaluatePbr && MLReferenceActive();
#endif
            [loop]
            for (uint gridIteration = 0u; gridIteration < cell.y; ++gridIteration)
            {
                const MegaLightData gridLight = MegaLightBuffer[MegaLightIndices[cell.x + gridIteration]];
#ifdef MEGALIGHTS_FORWARD
                if (!MLLightAffectsForward(gridLight)) continue;
                const float visibility = -1.0f;
#else
                const float visibility = reference
                    ? MLReferenceVisibility(gridLight, worldPos, normal, uint2(pixelPosition), gridIteration)
                    : -1.0f;
#endif
                AccumulateSurfaceLight(-1, gridLight.DirectionRange, gridLight.PositionType, gridLight.Extra, gridLight.Color,
                    false, MLLightCastsShadow(gridLight), 1.0f, visibility,
                    worldPos, worldPosDx, worldPosDy, normal, pixelPosition, shadowFilterRadius, evaluatePbr,
                    viewDir, albedo, materialData, shadowSoftness, monitorColor, acc);
            }
        }
    }
#endif
    pbrDirectLight = acc.PbrDirect;
    const float3 dirSum = acc.DirSum;
    const float3 colorSum = acc.ColorSum;
    const float weightSum = acc.WeightSum;
    const float maxAttenuation = acc.MaxAttenuation;
    const float volumeSum = acc.VolumeSum;
    const float rangeSum = acc.RangeSum;
    const float shadowSum = acc.ShadowSum;

    if (count <= 0 && gridLightCount == 0u)
    {
        float legacyVolume = 0.0f;
        ResolveLightCommon(worldPos, lightDir, attenuation, legacyVolume);
        lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f) * attenuation;
        lightColor = ApplyAtmosphereToLightCommon(worldPos, lightDir, lightColor, legacyVolume);
        if (evaluatePbr)
        {
            pbrDirectLight = EvaluatePbrDirectLight(
                normal, viewDir, lightDir, lightColor, albedo, materialData, shadowSoftness);
        }
        volumeScatter = legacyVolume * max(LightColor.a, 0.0f);
        rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
        return;
    }

    if (weightSum <= 0.000001f)
    {
        lightColor = float3(0.0f, 0.0f, 0.0f);
        attenuation = 0.0f;
        volumeScatter = 0.0f;
        rangeBlend = 0.0f;
        aggregateShadowVisibility = 1.0f;
        return;
    }

    lightDir = SafeNormalizeCommon(dirSum, SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f)));
    lightColor = colorSum + AtmosphereAmbientCommon(worldPos, lightDir);
    attenuation = saturate(maxAttenuation);
    volumeScatter = volumeSum;
    rangeBlend = saturate(rangeSum / weightSum);
    aggregateShadowVisibility = saturate(shadowSum / weightSum);
}

float VirtualMoonSurfaceVisibility(float3 worldPosition, float3 normal, float3 direction)
{
#ifdef SHADER_RAYTRACING
    if (SceneRayFlags.y > .5f && SceneRayFlags.z > .5f)
        return TraceSceneShadow(worldPosition, normal, direction, SceneRayParameters.x);
#endif
    // No authored shadow map belongs to the virtual moon. Without a valid
    // visibility provider it remains a sky disc, not an unoccluded surface light.
    return 0;
}

float3 EvaluateSurfaceEnvironment(SurfaceData surface, float3 viewDir, float3 worldPosition,
    float2 screenUv, float shadowSoftness, float3 pbrDirectLight)
{
    const float PI = 3.14159265359f;
    float3 N = surface.NormalWS;
    float3 V = viewDir;
    float NdotV = saturate(dot(N, V));
    float metallic = saturate(surface.Metallic);
    float roughness = clamp(surface.Roughness, 0.04f, 1.0f);
    float specularF0 = saturate(surface.F0);
    float3 albedo = surface.BaseColor;
    float3 F0 = lerp(float3(specularF0, specularF0, specularF0), albedo, metallic);

    float3 R = reflect(-V, N);
    R = normalize(R);

    float2 reflectionUV;
    reflectionUV.x = frac(atan2(R.z, R.x) / (2.0f * PI) + 0.5f);
    reflectionUV.y = acos(clamp(R.y, -1.0f, 1.0f)) / PI;

    float2 normalUV;
    normalUV.x = frac(atan2(N.z, N.x) / (2.0f * PI) + 0.5f);
    normalUV.y = acos(clamp(N.y, -1.0f, 1.0f)) / PI;

    float maxMip = 8.0f;

    float3 envSpecular = EnvironmentTexture.SampleLevel(
        TextureSampler, reflectionUV, roughness * maxMip).rgb;
#ifdef SHADER_RAYTRACING
    if (SceneRayFlags.x > 0.5f || RaytracingFlags.w > 0.5f)
        envSpecular = RaytracingFlags.w > 0.5f ? RaytracedReflections.SampleLevel(TextureSampler, screenUv, 0).rgb : 0;
#endif

    float3 irradiance = EnvironmentTexture.SampleLevel(TextureSampler,normalUV,maxMip).rgb;

    float oneMinusNdotV = 1.0f - NdotV;
    float oneMinusNdotV2 = oneMinusNdotV * oneMinusNdotV;
    float oneMinusNdotV5 = oneMinusNdotV2 * oneMinusNdotV2 * oneMinusNdotV;

    float3 roughnessF0Max = max(
    float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness),F0
    );

    float3 F_IBL = F0 + (roughnessF0Max - F0) * oneMinusNdotV5;
    float3 specularWeight = F_IBL;
    float3 ambientKD = 1.0f - F_IBL;
    if (QualityEnergyCompensation())
    {
        // Split sum DFG + multiple scattering (Fdez-Aguera 2019): rough surfaces no longer lose
        // the energy of multiply scattered light, diffuse gets exactly the remaining energy.
        float2 dfg = SampleDfg(NdotV, roughness);
        float3 FssEss = F0 * dfg.x + dfg.y;
        float Ess = dfg.x + dfg.y;
        float Ems = 1.0f - Ess;
        float3 Favg = F0 + (1.0f - F0) / 21.0f;
        float3 Fms = FssEss * Favg / max(1.0f - Ems * Favg, 0.0001f);
        specularWeight = FssEss + Fms * Ems;
        ambientKD = 1.0f - specularWeight;
    }
    ambientKD *= 1.0f - metallic;

    float3 ambientDiffuse = ambientKD * albedo * irradiance / ( 0.5f * PI);
    if (HdrFlags.w > 1.5f && Flags.w > 0)
    {
        ambientDiffuse = 0;
    }
    // Specular occlusion (Lagarde & de Rousiers 2014): derives the occlusion of the reflection
    // lobe from the material AO, roughness and view angle instead of applying AO uniformly.
    // Rough surfaces converge to AO, smooth surfaces at grazing angles keep their reflection.
    float materialAo = saturate(surface.AO);
    float specularOcclusion = saturate(pow(max(NdotV + materialAo, 0.0f),
        exp2(-16.0f * roughness - 1.0f)) - 1.0f + materialAo);
    float3 ambientSpecular = envSpecular * specularWeight;
    const uint model = g_ShadingModel.Model;
    const float modelStrength = g_ShadingModel.Strength;
    if (model == kShadingClearCoat && modelStrength > 0.0f)
    {
        float coatF = (0.04f + 0.96f * oneMinusNdotV5) * modelStrength;
        float3 coatEnvironment = EnvironmentTexture.SampleLevel(TextureSampler, reflectionUV, 0.06f * maxMip).rgb;
#ifdef SHADER_RAYTRACING
        if (RaytracingFlags.w > 0.5f) coatEnvironment = envSpecular;
#endif
        ambientDiffuse *= 1.0f - coatF;
        ambientSpecular = ambientSpecular * (1.0f - coatF) + coatEnvironment * coatF;
    }
    else if (model == kShadingSheen && modelStrength > 0.0f)
    {
        // Approximate directional albedo of the Charlie sheen lobe.
        float sheenAlbedo = lerp(0.12f, 0.35f, roughness) * (1.0f - NdotV * 0.5f);
        ambientSpecular += irradiance * lerp(1.0f, albedo, 0.5f) * sheenAlbedo * modelStrength;
    }

    float3 ambient = ambientDiffuse * materialAo + ambientSpecular * specularOcclusion;

    if (SkyParameters.x > 0.5f)
    {
        ambient *= lerp(0.025f, 1.0f, SkyParameters.y);
    }
    float3 moonDirection = VirtualMoonDirection();
    float moonVisibility = CloudTrace.z > 0.5 && CloudTrace.w > 0.5 ?
        CloudSolarTransmittance(worldPosition, moonDirection) : 1;
    float3 moonRadiance = VirtualMoonIrradiance() * moonVisibility *
        CelestialAtmosphereTransmission(worldPosition.y, moonDirection);
    [branch]
    if (any(moonRadiance > 1e-6))
    {
        moonRadiance *= VirtualMoonSurfaceVisibility(worldPosition, N, moonDirection);
        pbrDirectLight += EvaluatePbrDirectLight(N, V, moonDirection, moonRadiance,
            albedo, float3(surface.Metallic, surface.Roughness, surface.F0), shadowSoftness);
    }
    return pbrDirectLight + ambient + surface.Emissive;
}
float3 ApplySurfaceScreenEffects(float3 color, float3 emissive, float2 screenUv, float3 diffuseReflectance)
{
    // Isolated previews and forward-only scenes have no screen-space lighting history.
    if (Flags.w <= 0.0f)
    {
        return color + emissive;
    }
	float ambientVisibility = 1.0f;
    if (HdrFlags.z > 0.5f)
    {
        ambientVisibility = VisibilityBitmaskAO.SampleLevel(TextureSampler,screenUv,0);
    }

    float3 screenIndirect = 0.0f;
    if (HdrFlags.w > 0.5f)
    {
        screenIndirect = DeinterleavedSSGI.SampleLevel(TextureSampler,screenUv,0).rgb;
    }
	color *= lerp(0.38f, 1.0f, ambientVisibility);
    float rayAmbientFactor = 1.0f;
	if (RaytracingFlags.x > 0.5f && RaytracingFlags.y > 0.5f)
	{
		float rayAmbient = RaytracedVisibility.SampleLevel(TextureSampler, screenUv, 0).g;
		rayAmbientFactor = lerp(0.45f, 1.0f, rayAmbient);
	}
    if (HdrFlags.w > 1.5f)
    {
        // Ray traced GI already contains its own visibility (every bounce ray is occluded by
        // the scene). Applying the AO term on top darkened exactly the regions that should
        // receive bounce light, e.g. the underside of an object resting above a lit floor.
        color *= rayAmbientFactor;
        color += screenIndirect * max(diffuseReflectance, 0) / 3.14159265f;
    }
    else
    {
        color += screenIndirect * saturate(color + 0.18f);
        color *= rayAmbientFactor;
    }

    color += emissive;
    float4 atmosphereMedia = AtmosphereTexture.SampleLevel(TextureSampler, screenUv, 0);
    color = color * saturate(atmosphereMedia.a) + atmosphereMedia.rgb;
    return color;
}
#endif
