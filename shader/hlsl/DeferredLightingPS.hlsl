#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"
#pragma warning(disable: 4000)

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float4> PositionTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> MaterialTexture : register(t4);
Texture2D<float4> ShadowGBufferTexture : register(t5);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);
Texture2D<float4> AtmosphereTexture : register(t8);
Texture2D<float4> RimStyleTexture : register(t9);
Texture2D<float4> RimLightTexture : register(t10);

SamplerState TextureSampler : register(s0);
SamplerState ShadowSampler : register(s1);

float IsMaterialClass(float value, float target)
{
    return abs(value - target) < 0.5f;
}

float ShadowNoiseHash(float3 value)
{
    return frac(sin(dot(value, float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
}

bool ProjectVirtualShadowLevel(
    int level,
    float3 worldPos,
    out float3 lightNdc,
    out float2 virtualUv,
    out float levelInterior)
{
    float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
    if (clip.w <= 0.000001f)
    {
        lightNdc = 0.0f;
        virtualUv = 0.0f;
        levelInterior = 0.0f;
        return false;
    }

    lightNdc = clip.xyz / clip.w;
    virtualUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    bool resident;
    int2 localPage;
    VirtualShadowMapUvCommon(level, virtualUv, resident, localPage);
    if (!resident || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        levelInterior = 0.0f;
        return false;
    }

    float pageGrid = max(VirtualShadowPageOrigins[level].z, 1.0f);
    float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
    float firstResident = (pageGrid - residentGrid) * 0.5f;
    float2 pagePosition = virtualUv * pageGrid;
    float2 edgeDistance = min(
        pagePosition - firstResident,
        firstResident + residentGrid - pagePosition);
    levelInterior = smoothstep(
        0.12f,
        max(VirtualShadowGlobal.w, 0.25f),
        min(edgeDistance.x, edgeDistance.y));
    return true;
}

bool SampleVirtualShadowLevel(
    int level,
    float3 worldPos,
    float3 normal,
    float3 lightDir,
    out float visibility,
    out float levelInterior)
{
    float3 lightNdc;
    float2 virtualUv;
    if (!ProjectVirtualShadowLevel(level, worldPos, lightNdc, virtualUv, levelInterior))
    {
        visibility = 1.0f;
        return false;
    }

    float4 params = VirtualShadowParams[level];
    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float nDotL = saturate(dot(n, l));
    float levelBiasScale = 1.0f + (float)level * 0.55f;
    float currentDepth = lightNdc.z -
        max(params.z * (1.0f - nDotL), params.w) * levelBiasScale;

    const float2 poisson[9] =
    {
        float2(0.0f, 0.0f),
        float2(-0.613392f, 0.617481f),
        float2(0.170019f, -0.040254f),
        float2(-0.299417f, 0.791925f),
        float2(0.645680f, 0.493210f),
        float2(-0.651784f, -0.717887f),
        float2(0.421003f, 0.027070f),
        float2(-0.817194f, -0.271096f),
        float2(-0.705374f, -0.668203f)
    };

    int filterRadius = clamp((int)round(VirtualShadowGlobal.z), 0, 3);
    int tapCount = filterRadius <= 0 ? 1 : 9;
    float angle = ShadowNoiseHash(worldPos * 17.0f + (float)level) * 6.28318530718f;
    float sineValue;
    float cosineValue;
    sincos(angle, sineValue, cosineValue);
    float2x2 rotation = float2x2(cosineValue, -sineValue, sineValue, cosineValue);

    float visibilitySum = 0.0f;
    float sampleCount = 0.0f;
    [unroll]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 offset = mul(poisson[tap], rotation) * (float)max(filterRadius, 1);
        float2 tapVirtualUv = virtualUv + offset * params.y;
        bool tapResident;
        int2 tapPage;
        float2 physicalUv = VirtualShadowMapUvCommon(
            level,
            tapVirtualUv,
            tapResident,
            tapPage);
        if (!tapResident) continue;

        float closestDepth = ShadowMapTexture.SampleLevel(
            ShadowSampler,
            float3(physicalUv, max(params.x, 0.0f)),
            0);
        visibilitySum += currentDepth <= closestDepth ? 1.0f : 0.0f;
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

float SampleDeferredShadowMap(int lightIndex, float3 worldPos, float3 normal, float3 lightDir)
{
    float shadowLayer = LightShadowData[lightIndex].x;
    [branch]
    if (shadowLayer < -0.5f)
    {
        return 1.0f;
    }

    const bool virtualShadow =
        VirtualShadowGlobal.x > 0.5f &&
        LightShadowData[lightIndex].w < 0.0f;
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
                    normal,
                    lightDir,
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

    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
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
        LightShadowData[lightIndex].z * (1.0f - saturate(dot(n, l))),
        abs(LightShadowData[lightIndex].w));
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float closestDepth = ShadowMapTexture.SampleLevel(
                ShadowSampler,
                float3(shadowUv + float2(x, y) * texelSize, shadowLayer),
                0);
            visibility += currentDepth <= closestDepth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
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

    const int stepCount = clamp((int)round(ShadowRuntimeGlobal.z), 6, 24);
    const float rayLength = max(ShadowRuntimeGlobal.y, 0.02f);
    const float3 safeNormal = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    const float3 safeLightDir = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    const float stepLength = rayLength / (float)stepCount;
    const float3 origin = worldPos + safeNormal * max(0.018f, stepLength * 0.55f);
    const float jitter = ShadowNoiseHash(worldPos * 31.0f + safeNormal * 7.0f);

    float occlusion = 0.0f;
    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 0.25f + jitter * 0.65f) / (float)stepCount;
        float3 rayPosition = origin + safeLightDir * rayLength * t;
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
            smoothstep(0.0015f, thickness * 0.35f, depthDelta) *
            (1.0f - smoothstep(thickness * 0.72f, thickness, depthDelta));
        occlusion = max(occlusion, hit * (1.0f - t * 0.65f));
    }

    float cameraDistance = length(worldPos - PPCameraPos.xyz);
    float distanceFade = 1.0f - smoothstep(24.0f, 55.0f, cameraDistance);
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
    const float3 origin = worldPos + SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f)) * 0.04f;
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
    float3 normal,
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

    [loop]
    for (int i = 0; i < count; ++i)
    {
        float3 singleDir;
        float singleAttenuation;
        float singleVolume;
        ResolveSingleLightCommon(worldPos, LightDirections[i], LightPositionTypes[i], LightExtras[i], singleDir, singleAttenuation, singleVolume);

        float shadowVisibility = SampleDeferredShadowMap(i, worldPos, normal, singleDir);
        float contactVisibility = LightShadowData[i].x >= -0.5f
            ? SampleContactShadow(worldPos, normal, singleDir)
            : 1.0f;
        shadowVisibility *= lerp(1.0f, contactVisibility, 0.45f);
		if (LightPositionTypes[i].w < 0.5f)
		{
			shadowVisibility *= SampleDistanceFieldShadow(worldPos, normal, singleDir);
		}
        float lightIntensityValue = max(LightColors[i].a, 0.0f);
        float3 singleColor = max(LightColors[i].rgb, float3(0.0f, 0.0f, 0.0f)) * lightIntensityValue * singleAttenuation * shadowVisibility;
        singleColor = ApplyAtmosphereToLightCommon(worldPos, singleDir, singleColor, singleVolume);

        float weight = max(dot(max(LightColors[i].rgb, float3(0.0f, 0.0f, 0.0f)) * lightIntensityValue, float3(0.299f, 0.587f, 0.114f)), 0.0001f) * singleAttenuation;
        dirSum += singleDir * weight;
        colorSum += singleColor;
        weightSum += weight;
        maxAttenuation = max(maxAttenuation, singleAttenuation);
        volumeSum += singleVolume * lightIntensityValue * shadowVisibility;
        rangeSum += saturate((max(LightDirections[i].w, 1.0f) - 1.0f) / 7.0f) * weight;
        shadowSum += shadowVisibility * weight;
    }

    if (count <= 0 || weightSum <= 0.000001f)
    {
        float legacyVolume = 0.0f;
        ResolveLightCommon(worldPos, lightDir, attenuation, legacyVolume);
        lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f) * attenuation;
        lightColor = ApplyAtmosphereToLightCommon(worldPos, lightDir, lightColor, legacyVolume);
        volumeScatter = legacyVolume * max(LightColor.a, 0.0f);
        rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
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
    float4 baseColor = BaseColorTexture.Sample(TextureSampler, input.TexCoord);
    float4 normal = NormalTexture.Sample(TextureSampler, input.TexCoord);
    float4 position = PositionTexture.Sample(TextureSampler, input.TexCoord);
    float4 material = MaterialTexture.Sample(TextureSampler, input.TexCoord);
    float4 shadowParams = ShadowGBufferTexture.Sample(TextureSampler, input.TexCoord);
    float4 rimStyle = RimStyleTexture.Sample(TextureSampler, input.TexCoord);
    float4 rimLight = RimLightTexture.Sample(TextureSampler, input.TexCoord);

    float shaderClass = material.a;
    bool background = shaderClass < -0.5f;
    bool transparent = IsMaterialClass(shaderClass, 0.0f);
    bool hair = IsMaterialClass(shaderClass, 1.0f);
    bool cloth = IsMaterialClass(shaderClass, 2.0f);
    bool skin = IsMaterialClass(shaderClass, 3.0f);
    bool toon = IsMaterialClass(shaderClass, 4.0f);
    bool shadow = IsMaterialClass(shaderClass, 5.0f);
    bool metallic = IsMaterialClass(shaderClass, 6.0f);
    bool selfShadow = IsMaterialClass(shaderClass, 7.0f);
    bool lit = IsMaterialClass(shaderClass, 8.0f);
    bool eye = IsMaterialClass(shaderClass, 9.0f);
    bool pbr = IsMaterialClass(shaderClass, 11.0f);
    bool brdf = IsMaterialClass(shaderClass, 12.0f);
    bool btdf = IsMaterialClass(shaderClass, 13.0f);
    bool bsdf = IsMaterialClass(shaderClass, 14.0f);

    float3 atmosphereViewScatter = AtmosphereTexture.SampleLevel(TextureSampler, input.TexCoord, 0).rgb;
    
    if (background)
    {
        baseColor.rgb += AtmosphereBackgroundCommon(input.TexCoord) + atmosphereViewScatter;
        return baseColor;
    }

    if (transparent)
    {
        return baseColor;
    }

   
    float shadowThreshold = shadowParams.r;
    float shadowSoftness = max(shadowParams.g, 0.0001f);
    float shadowStrength = saturate(shadowParams.b);



    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    float aggregateShadowVisibility;
    float3 surfaceNormal = SafeNormalizeCommon(normal.xyz, float3(0.0f, 1.0f, 0.0f));
    ResolveDeferredLightAggregateShadowed(position.xyz, surfaceNormal, lightDir, lightColor, lightAttenuation, volumeScatter, rangeBlend, aggregateShadowVisibility);

    const int shadowDebugMode = (int)round(ShadowDebugGlobal.x);
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

    
    float Metallic = material.r;
    float Roughness = material.g;
    float f0 = material.b;
    
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

        float NdotLScalar = saturate(dot(N, L));
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

        float shadowVisibility = 1.0f;

        float3 directLight =
        (diffuseBRDF + specularBRDF)
        * lightColor.rgb
        * lightIntensity
        * NdotLScalar
        * shadowVisibility;
        
        float3 R = reflect(-V, N);
        R = normalize(R);

        float2 reflectionUV;
        reflectionUV.x = atan2(R.z, R.x) / (2.0f * PI);
        reflectionUV.y = acos(R.y) / PI;

        float2 normalUV;
        normalUV.x = atan2(N.z, N.x) / (2.0f * PI);
        normalUV.y = acos(N.y) / PI;

        float maxMip = 8.0f;

        float2 ssrUV = (roughness < 0.65f)
            ? ScreenSpaceRayMarch(position.xyz, R, DepthTexture, TextureSampler)
            : float2(-1.0f, -1.0f);
        float3 envSpecular;
        if (all(ssrUV >= 0.0f))
        {
            envSpecular = BaseColorTexture.SampleLevel(TextureSampler, ssrUV, 0).rgb;
        }
        else
        {
            envSpecular = EnvironmentTexture.SampleLevel(TextureSampler, reflectionUV, roughness * maxMip).rgb;
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

        baseColor.rgb = directLight;
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

    baseColor.rgb += atmosphereViewScatter;
    baseColor.rgb = ApplyLocalHeightFogCommon(baseColor.rgb, position.xyz, PPCameraPos.xyz);
    baseColor.a = 1.0f;
    return baseColor;
}
