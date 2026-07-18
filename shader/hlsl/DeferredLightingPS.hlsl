#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"
#pragma warning(disable: 4000)

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float4> MaterialTexture : register(t3);
Texture2D<float4> ShadowGBufferTexture : register(t4);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);
Texture2D<float4> AtmosphereTexture : register(t8);
Texture2D<float4> MonitorTexture : register(t12);
Texture2D<float> VisibilityBitmaskAO : register(t13);
Texture2D<float4> DeinterleavedSSGI : register(t14);

SamplerState TextureSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

float IsMaterialClass(float value, float target)
{
    return abs(value - target) < 0.5f;
}

float ContactShadowPixelNoise(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

float DirectionalReceiverPlaneBias(
    float3 receiverWorldDx,
    float3 receiverWorldDy,
    float4x4 lightViewProjection,
    float baseBias,
    int filterRadius)
{
    // A constant comparison bias cannot follow a sloped receiver: adjacent
    // pixels alternately fall in front of and behind the quantized shadow
    // plane, producing bands whose spacing changes with the cascade/clipmap
    // world-texel size. Account for the depth change across this pixel quad
    // and expand it for the configured PCF footprint.
    float filterFootprint = 1.0f + (float)max(filterRadius, 1);
    // Directional shadow projections are orthographic, so transforming the
    // already-computed world-position derivatives gives the exact NDC depth
    // change without using gradient instructions inside the light/level loops.
    float depthDx = abs(mul(float4(receiverWorldDx, 0.0f), lightViewProjection).z);
    float depthDy = abs(mul(float4(receiverWorldDy, 0.0f), lightViewProjection).z);
    float derivativeBias = (depthDx + depthDy) * filterFootprint;

    // Keep the correction bounded by the authored per-level bias so thin
    // contact shadows remain attached to their casters.
    return min(
        derivativeBias,
        max(baseBias * 4.0f, 0.000004f));
}

float SampleConventionalShadowPcf9(
    float2 shadowUv,
    float shadowLayer,
    float texelSize,
    float currentDepth,
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
    float visibility = 0.0f;
    [unroll]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 offset = kernel[tap] * (float)max(filterRadius, 1) * texelSize;
        visibility += ShadowMapTexture.SampleCmpLevelZero(
            ShadowSampler,
            float3(shadowUv + offset, shadowLayer),
            currentDepth);
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
    out float visibility,
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
    // Callers pass the normalized G-buffer normal and the normalized result of
    // ResolveSingleLightCommon. Avoid repeating two reciprocal square roots for
    // every clipmap level tested by the same pixel.
    float nDotL = saturate(dot(normal, lightDir));
    float baseBias = max(params.z * (1.0f - nDotL), params.w);
    float currentDepth = lightNdc.z - baseBias -
        DirectionalReceiverPlaneBias(
            worldPosDx,
            worldPosDy,
            VirtualShadowViewProjections[level],
            baseBias,
            filterRadius);

    // Keep the kernel stable in world space to avoid self-shadow grain without
    // paying for temporal/spatial random rotation or a separate denoise pass.
    // Nine taps restores the optimized VSM cost while retaining smooth PCF.
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

    // Most kernels are fully contained in one 128x128 physical page. In that
    // case virtual->physical translation is affine, so translate once and use
    // ordinary texel offsets for every tap. Only the narrow page-edge band
    // needs per-tap residency and ring-address checks.
    float pageGrid = max(VirtualShadowPageOrigins[level].z, 1.0f);
    float pageTexels = rcp(max(params.y * pageGrid, 0.000001f));
    float2 texelInPage = frac(virtualUv * pageGrid) * pageTexels;
    float edgeTexels = min(
        min(texelInPage.x, texelInPage.y),
        min(pageTexels - texelInPage.x, pageTexels - texelInPage.y));
    bool kernelInOnePage = edgeTexels > (float)filterRadius + 1.0f;

    float visibilitySum = 0.0f;
    float sampleCount = 0.0f;
    [unroll]
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
            currentDepth);
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

float SampleDeferredShadowMap(
    int lightIndex,
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float3 lightDir,
    int filterRadius)
{
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
        float fineVisibility = 1.0f;
        float fineInterior = 1.0f;
        bool hasFine = false;

        [unroll]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= levelCount) break;
            float levelVisibility;
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
        return hasFine ? fineVisibility : 1.0f;
    }

    // Conventional cascades use one complete texture-array layer per level.
    // They share the level matrices with VSM, but must not use the virtual-page
    // address translation above.
    if (VirtualShadowGlobal.x > 1.5f && directionalMultiLevel)
    {
        int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
        float fineVisibility = 1.0f;
        float fineInterior = 1.0f;
        bool hasFine = false;

        [unroll]
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
            float baseBias = max(
                params.z * (1.0f - saturate(dot(normal, lightDir))),
                params.w);
            float currentDepth = lightNdc.z - baseBias -
                DirectionalReceiverPlaneBias(
                    worldPosDx,
                    worldPosDy,
                    VirtualShadowViewProjections[level],
                    baseBias,
                    filterRadius);
            float levelVisibility = SampleConventionalShadowPcf9(
                shadowUv,
                params.x,
                params.y * ShadowWorldFilterScaleCommon(level),
                currentDepth,
                filterRadius);
            float levelInterior = CascadedShadowLevelInteriorCommon(
                level, shadowUv, worldPos);

            if (!hasFine)
            {
                fineVisibility = levelVisibility;
                fineInterior = levelInterior;
                hasFine = true;
                if (fineInterior >= 0.999f || level == levelCount - 1)
                {
                    return fineVisibility;
                }
                continue;
            }
            return lerp(levelVisibility, fineVisibility, fineInterior);
        }
        return hasFine ? fineVisibility : 1.0f;
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
    float currentDepth = lightNdc.z - max(
        LightShadowData[lightIndex].z * (1.0f - saturate(dot(normal, lightDir))),
        abs(LightShadowData[lightIndex].w));
    return SampleConventionalShadowPcf9(
        shadowUv, shadowLayer, texelSize, currentDepth, filterRadius);
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
        // Center the jitter within each interval. This keeps coverage uniform
        // while breaking the visible shells produced by fixed ray steps.
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
        // Probabilistic union is smoother than selecting a single maximum
        // sample and does not expose one marching interval as a dark stripe.
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

void ResolveDeferredLightAggregateShadowed(
    float3 worldPos,
    float3 worldPosDx,
    float3 worldPosDy,
    float3 normal,
    float2 pixelPosition,
    int shadowFilterRadius,
    out float3 lightDir,
    out float3 lightColor,
    out float attenuation,
    out float volumeScatter,
    out float rangeBlend,
    out float aggregateShadowVisibility)
{
    lightDir = SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f);
    attenuation = 1.0f;
    volumeScatter = 0.0f;
    rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
    aggregateShadowVisibility = 1.0f;

    int count = min((int)round(LightCount.x), MAX_SHADER_LIGHTS);
    float3 dirSum = float3(0.0f, 0.0f, 0.0f);
    float3 colorSum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    float maxAttenuation = 0.0f;
    float volumeSum = 0.0f;
    float rangeSum = 0.0f;
    float shadowSum = 0.0f;

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

        int i = (int)lightIndex;
        float3 singleDir;
        float singleAttenuation;
        float singleVolume;
        ResolveSingleLightCommon(worldPos, LightDirections[i], LightPositionTypes[i], LightExtras[i], singleDir, singleAttenuation, singleVolume);

		// Local lights outside their finite range (or outside a spot cone) cannot
		// contribute.  Reject them before VSM/PCF and screen-space ray marching;
		// the old order paid those costs for every configured light at every pixel.
		float lightIntensityValue = max(LightColors[i].a, 0.0f);
		[branch]
		if (singleAttenuation <= 0.000001f || lightIntensityValue <= 0.000001f)
		{
			continue;
		}

		const bool decalLight = LightFlags[i].w >= 0.5f;
        float shadowVisibility = decalLight
			? 1.0f
			: SampleDeferredShadowMap(
                i,
                worldPos,
                worldPosDx,
                worldPosDy,
                normal,
                singleDir,
                shadowFilterRadius);
		// Contact shadows fill near-field detail for the directional VSM.  Running
		// the depth ray march once for every shadowed local light multiplies the
		// full-screen cost and provides little useful information.
		const bool directionalShadow =
			LightPositionTypes[i].w < 0.5f && LightShadowData[i].x >= -0.5f;
        float contactVisibility = !decalLight && directionalShadow
            ? SampleContactShadow(worldPos, normal, singleDir)
            : 1.0f;
        shadowVisibility *= lerp(1.0f, contactVisibility, 0.45f);
		if (!decalLight && LightPositionTypes[i].w < 0.5f)
		{
			shadowVisibility *= SampleDistanceFieldShadow(worldPos, normal, singleDir);
		}
        float3 authoredLightColor = max(LightColors[i].rgb, float3(0.0f, 0.0f, 0.0f));
		if (decalLight)
		{
			// One stage/monitor texture can drive every cheap decal light without
			// duplicating animation state across hundreds of authored emitters.
			authoredLightColor *= monitorColor;
		}
        float3 singleColor = authoredLightColor * lightIntensityValue * singleAttenuation * shadowVisibility;
        if (!decalLight)
		{
			singleColor = ApplyAtmosphereToLightCommon(worldPos, singleDir, singleColor, singleVolume);
		}

        float weight = max(dot(authoredLightColor * lightIntensityValue, float3(0.299f, 0.587f, 0.114f)), 0.0001f) * singleAttenuation;
        dirSum += singleDir * weight;
        colorSum += singleColor;
        weightSum += weight;
        maxAttenuation = max(maxAttenuation, singleAttenuation);
        volumeSum += singleVolume * lightIntensityValue * shadowVisibility;
        rangeSum += saturate((max(LightDirections[i].w, 1.0f) - 1.0f) / 7.0f) * weight;
        shadowSum += shadowVisibility * weight;
    }

    if (count <= 0)
    {
        float legacyVolume = 0.0f;
        ResolveLightCommon(worldPos, lightDir, attenuation, legacyVolume);
        lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f) * attenuation;
        lightColor = ApplyAtmosphereToLightCommon(worldPos, lightDir, lightColor, legacyVolume);
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

float4 main(PSInputPostProcess input) : SV_Target
{
    
    const float PI = 3.14159265358979323846f;
    float4 material = MaterialTexture.Sample(TextureSampler, input.TexCoord);
    float shaderClass = material.a;
    bool background = shaderClass < -0.5f;
    bool transparent = IsMaterialClass(shaderClass, 0.0f);
	float4 baseColor = BaseColorTexture.Sample(TextureSampler, input.TexCoord);

	if (transparent)
	{
		return baseColor;
	}

	float4 atmosphereMedia = AtmosphereTexture.SampleLevel(
		TextureSampler, input.TexCoord, 0);
	float fogTransmittance = saturate(atmosphereMedia.a);
	if (background)
	{
		baseColor.rgb =
			(baseColor.rgb + AtmosphereBackgroundCommon(input.TexCoord)) *
			fogTransmittance + atmosphereMedia.rgb;
		return baseColor;
	}

	bool toon = IsMaterialClass(shaderClass, 4.0f);
	bool shadow = IsMaterialClass(shaderClass, 5.0f);
	bool lit = IsMaterialClass(shaderClass, 8.0f);
	bool pbr = IsMaterialClass(shaderClass, 11.0f);
	const int shadowDebugMode = (int)round(ShadowDebugGlobal.x);
	const bool needsDeferredLighting =
		toon || shadow || lit || pbr || shadowDebugMode > 0;
	[branch]
	if (!needsDeferredLighting)
	{
		baseColor.rgb = baseColor.rgb * fogTransmittance + atmosphereMedia.rgb;
		baseColor.a = 1.0f;
		return baseColor;
	}

	float4 normal = NormalTexture.Sample(TextureSampler, input.TexCoord);
	float depth = DepthTexture.Sample(TextureSampler, input.TexCoord);
	float3 position = ReconstructPostProcessWorldPositionCommon(input.TexCoord, depth);
    float3 positionDx = ddx(position);
    float3 positionDy = ddy(position);
	float4 shadowParams = ShadowGBufferTexture.Sample(TextureSampler, input.TexCoord);

   
    float shadowThreshold = shadowParams.r;
    float shadowSoftness = max(shadowParams.g, 0.0001f);
    float shadowStrength = saturate(shadowParams.b);



    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    float aggregateShadowVisibility;
    float3 surfaceNormal = DecodeGBufferNormal(normal.xyz);
    int materialShadowFilterRadius =
        pbr
            ? clamp(
                (int)round(lerp(
                    max(VirtualShadowGlobal.z, 1.0f),
                    3.0f,
                    saturate(shadowSoftness * 2.0f))),
                1,
                3)
            : clamp((int)round(VirtualShadowGlobal.z), 0, 3);
    ResolveDeferredLightAggregateShadowed(
        position,
        positionDx,
        positionDy,
        surfaceNormal,
        input.Position.xy,
        materialShadowFilterRadius,
        lightDir,
        lightColor,
        lightAttenuation,
        volumeScatter,
        rangeBlend,
        aggregateShadowVisibility);

    if (shadowDebugMode > 0 && VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f)
    {
        if (shadowDebugMode == 1)
        {
            return float4(aggregateShadowVisibility.xxx, 1.0f);
        }
        int selectedLevel = max((int)round(VirtualShadowGlobal.y) - 1, 0);
        float2 selectedUv = float2(0.5f, 0.5f);
        [unroll]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= (int)round(VirtualShadowGlobal.y)) break;
            float4 clip = mul(float4(position.xyz, 1.0f), VirtualShadowViewProjections[level]);
            float3 ndc = clip.xyz / max(clip.w, 0.000001f);
            float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
            selectedUv = uv;
            float pageGrid = max(ShadowDebugGlobal.w, 1.0f);
            float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
            float firstResident = (pageGrid - residentGrid) * 0.5f;
            float2 pageCoord = floor(saturate(uv) * pageGrid);
            if (all(uv >= 0.0f) && all(uv < 1.0f) &&
                all(pageCoord >= firstResident) &&
                all(pageCoord < firstResident + residentGrid))
            {
                selectedLevel = level;
                break;
            }
        }
        if (shadowDebugMode == 2)
        {
            const float3 levelColors[4] = {
                float3(0.10f, 0.85f, 0.30f), float3(0.15f, 0.45f, 1.00f),
                float3(1.00f, 0.70f, 0.10f), float3(0.95f, 0.18f, 0.25f) };
            return float4(levelColors[selectedLevel], 1.0f);
        }
        float2 page = floor(saturate(selectedUv) * max(ShadowDebugGlobal.w, 1.0f));
        float pageHash = frac(dot(page, float2(0.06711056f, 0.00583715f)));
        float3 pageColor = frac(pageHash + float3(0.0f, 0.333f, 0.667f));
        return float4(pageColor, 1.0f);
    }
    float lightIntensity = LightColor.a;

    
    float Roughness = material.g;
    
    float3 NdotL = saturate(dot(surfaceNormal, lightDir));
    float3 environmentColor = EnvironmentTexture.SampleLevel(TextureSampler, input.TexCoord, Roughness * 10.0f).rgb;
    NdotL += lightIntensity;
    
    
    float3 diffuse = 0.0f;
    {
        float3 light = lightColor.rgb * saturate(dot(lightDir, surfaceNormal));
        
        float2 iblTexcoord;
        iblTexcoord.x = -atan2(surfaceNormal.x, surfaceNormal.z) / (PI * 2);
        iblTexcoord.y = asin(surfaceNormal.y) / PI;
        light += EnvironmentTexture.SampleLevel(TextureSampler, iblTexcoord, Roughness  * 10.0f).rgb * 6;
        diffuse = light * baseColor.rgb / PI;
    }
    
    if(shadow)
    {
        float castShadow = saturate(1.0f - aggregateShadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.015f, 0.014f, 0.017f) + environmentColor * 0.16f + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (0.12f + NdotL * (0.22f + lightColor.rgb * 0.72f));
        baseColor.rgb = lerp(litColor, baseColor.rgb * shadowTint - 0.08f, shadowDensity);
    }

    if(toon)
    {
        float castShadow = saturate(1.0f - aggregateShadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float selfShadowMask = smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
        shadowDensity = saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.018f, 0.016f, 0.020f) + environmentColor + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (lightColor.rgb * 0.72f + NdotL);
        baseColor.rgb = diffuse * lerp(litColor, litColor * shadowTint, shadowDensity);
    }

    if(lit)
    {
        float castShadow = saturate(1.0f - aggregateShadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float selfShadowMask = smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
        shadowDensity = saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.018f, 0.016f, 0.020f) + environmentColor + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (lightColor.rgb * 0.72f + NdotL);
        baseColor.rgb = diffuse * lerp(litColor, litColor * shadowTint, shadowDensity);
    }
    
    if (pbr)
    {
        float PI = 3.14159265359f;

        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(PPCameraPos.xyz - position.xyz + 0.00001f);
        float3 H = normalize(L + V + 0.00001f);

        float rawNdotL = dot(N, L);
        float NdotLScalar = saturate(rawNdotL);
        float NdotV = saturate(dot(N, V));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));

        float metallic = saturate(material.r);
        float roughness = clamp(material.g, 0.04f, 1.0f);
        float specularF0 = max(material.b, 0.04f);

        float3 albedo = baseColor.rgb;

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

        float3 specularBRDF = D * G * F;
        specularBRDF /= max(4.0f * NdotLScalar * NdotV, 0.00001f);

        float3 kS = F;
        float3 kD = 1.0f - kS;
        kD *= 1.0f - metallic;

        float3 diffuseBRDF = kD * albedo / PI;

        // PBR faces benefit from a wrapped diffuse terminator: cast shadows
        // still come from PCF, while the curved surface no longer snaps from
        // lit to black at N.L=0. Specular remains on the physical N.L term.
        float terminatorWidth = lerp(
            0.08f,
            0.45f,
            saturate(shadowSoftness * 2.0f));
        float wrappedNdotL = saturate(
            (rawNdotL + terminatorWidth) / (1.0f + terminatorWidth));
        wrappedNdotL =
            wrappedNdotL * wrappedNdotL * (3.0f - 2.0f * wrappedNdotL);

        float3 directDiffuse =
            diffuseBRDF * lightColor.rgb * lightIntensity * wrappedNdotL;
        float3 directSpecular =
            specularBRDF * lightColor.rgb * lightIntensity * NdotLScalar;
        float3 directLight = directDiffuse + directSpecular;
        
        float3 R = reflect(-V, N);
        R = normalize(R);

        float2 reflectionUV;
        reflectionUV.x = frac(atan2(R.z, R.x) / (2.0f * PI) + 0.5f);
        reflectionUV.y = acos(clamp(R.y, -1.0f, 1.0f)) / PI;

        float2 normalUV;
        normalUV.x = frac(atan2(N.z, N.x) / (2.0f * PI) + 0.5f);
        normalUV.y = acos(clamp(N.y, -1.0f, 1.0f)) / PI;

        float maxMip = 8.0f;

        // Move the ray off the current surface to avoid self-reflection. The
        // returned hit is additionally checked against the world-position
        // G-buffer, rejecting depth discontinuities that create white streaks.
        float3 ssrHit = (roughness < 0.65f && dot(R, V) < 0.0f)
            ? ScreenSpaceRayMarch(position + N * 0.04f + R * 0.02f, R, DepthTexture, TextureSampler)
            : float3(-1.0f, -1.0f, -1.0f);
        float3 envSpecular = EnvironmentTexture.SampleLevel(
            TextureSampler, reflectionUV, roughness * maxMip).rgb;
        if (all(ssrHit.xy >= 0.0f) && all(ssrHit.xy <= 1.0f))
        {
            float3 rayOrigin = position + N * 0.04f + R * 0.02f;
            float3 rayHitPosition = rayOrigin + R * ssrHit.z;
            float hitDepth = DepthTexture.SampleLevel(TextureSampler, ssrHit.xy, 0);
            float3 gbufferHitPosition = ReconstructPostProcessWorldPositionCommon(ssrHit.xy, hitDepth);
            float4 gbufferHitMaterial = MaterialTexture.SampleLevel(TextureSampler, ssrHit.xy, 0);
            float hitTolerance = 0.10f + ssrHit.z * 0.015f;
            bool validGeometryHit = gbufferHitMaterial.a >= -0.5f &&
                hitDepth < 0.9999f && distance(rayHitPosition, gbufferHitPosition) <= hitTolerance;

            if (validGeometryHit)
            {
                float2 edgeDistance = min(ssrHit.xy, 1.0f - ssrHit.xy);
                float edgeConfidence = saturate(min(edgeDistance.x, edgeDistance.y) * 24.0f);
                float distanceConfidence = saturate(1.0f - ssrHit.z / 15.0f);
                float roughnessConfidence = saturate((0.65f - roughness) / 0.25f);
                float ssrConfidence = edgeConfidence * distanceConfidence * roughnessConfidence;
                float3 ssrColor = BaseColorTexture.SampleLevel(TextureSampler, ssrHit.xy, roughness * 2.0f).rgb;
                envSpecular = lerp(envSpecular, ssrColor, ssrConfidence);
            }
        }

        float3 irradiance = EnvironmentTexture.SampleLevel(TextureSampler,normalUV,maxMip).rgb;

        float oneMinusNdotV = 1.0f - NdotV;
        float oneMinusNdotV2 = oneMinusNdotV * oneMinusNdotV;
        float oneMinusNdotV5 = oneMinusNdotV2 * oneMinusNdotV2 * oneMinusNdotV;

        float3 roughnessF0Max = max(
        float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness),F0);

        float3 F_IBL = F0 + (roughnessF0Max - F0) * oneMinusNdotV5;

        float3 ambientKD = 1.0f - F_IBL;
        ambientKD *= 1.0f - metallic;

        float3 ambientDiffuse = ambientKD * albedo * irradiance / ( 0.5f * PI);
        float3 ambientSpecular = envSpecular * F_IBL;

        float3 ambient = ambientDiffuse + ambientSpecular;
        // Preserve the original deferred PBR appearance. Environment/SSR IBL
        // was intentionally excluded here; reintroducing it made dielectric,
        // fully rough materials look metallic.
        baseColor.rgb = directLight + ambient;
    }

    //{
    //    float rimConfigWeight = step(
    //        0.0001f,
    //        dot(abs(rimStyle), float4(1.0f, 1.0f, 1.0f, 1.0f)) +
    //        dot(abs(rimLight.rgb), float3(1.0f, 1.0f, 1.0f)));
    //    rimStyle = lerp(float4(RimStrength, RimThreshold, RimSoftness, RimPower), rimStyle, rimConfigWeight);
    //    rimLight = lerp(float4(RimColor, RimAlbedoBlend), rimLight, rimConfigWeight);

    //    float3 viewDir = SafeNormalizeCommon(PPCameraPos.xyz - position.xyz, float3(0.0f, 0.0f, -1.0f));
    //    float rimStrength = max(rimStyle.x, 0.0f);
    //    float rimThreshold = saturate(rimStyle.y);
    //    float rimSoftness = max(rimStyle.z, 0.0001f);
    //    float rimPower = max(rimStyle.w, 0.05f);
    //    float rimAlbedoBlend = saturate(rimLight.a);
    //    float rimLightBlend = saturate(RimLightBlend);

    //    float rimRaw = 1.0f - saturate(dot(surfaceNormal, viewDir));
    //    float rimCurved = pow(rimRaw, rimPower);
    //    float rimMask = smoothstep(rimThreshold - rimSoftness, rimThreshold + rimSoftness, rimCurved);
    //    rimMask *= smoothstep(-0.25f, 0.35f, dot(surfaceNormal, lightDir));
    //    rimMask *= saturate(0.35f + aggregateShadowVisibility * 0.65f);

    //    float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
    //    float3 normalizedLightColor = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
    //    float3 authoredRimColor = max(rimLight.rgb, 0.0f);
    //    float3 rimTint = lerp(authoredRimColor, authoredRimColor * baseColor.rgb, rimAlbedoBlend);
    //    rimTint = lerp(rimTint, rimTint * normalizedLightColor, rimLightBlend);
    //    baseColor.rgb += rimTint * rimMask * rimStrength * max(lightAttenuation, 0.25f);
    //}

	float ambientVisibility = HdrFlags.z > 0.5f
		? VisibilityBitmaskAO.SampleLevel(TextureSampler, input.TexCoord, 0)
		: 1.0f;
	float3 screenIndirect = HdrFlags.w > 0.5f
		? DeinterleavedSSGI.SampleLevel(TextureSampler, input.TexCoord, 0).rgb
		: 0.0f;
	baseColor.rgb = baseColor.rgb * lerp(0.38f, 1.0f, ambientVisibility) +
		screenIndirect * saturate(baseColor.rgb + 0.18f);

    baseColor.rgb =
        baseColor.rgb * fogTransmittance +
        atmosphereMedia.rgb;
    baseColor.a = 1.0f;
    return baseColor;
}
