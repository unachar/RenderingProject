#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float4> PositionTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> MaterialTexture : register(t4);
Texture2D<float4> ShadowGBufferTexture : register(t5);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);

SamplerState TextureSampler : register(s0);
SamplerState ShadowSampler : register(s1);

struct LocalFogRayResult
{
    float3 Scattering;
    float Transmittance;
};

int GetAtmosphereStepCount()
{
    float scale = clamp(Flags.w, 0.25f, 1.0f);
    float lowResolutionWeight = saturate((1.0f - scale) / 0.75f);
    return (int)round(lerp(10.0f, 18.0f, lowResolutionWeight));
}

int GetLocalFogStepCount()
{
    float scale = clamp(Flags.w, 0.25f, 1.0f);
    float lowResolutionWeight = saturate((1.0f - scale) / 0.75f);
    return (int)round(lerp(8.0f, 16.0f, lowResolutionWeight));
}

float SampleAtmosphereShadowFast(
    float3 worldPos,
    float3 lightDir,
    Texture2DArray<float> shadowMap,
    SamplerState shadowSampler,
    float4x4 lightViewProjection,
    float4 shadowMapParams,
    float shadowLayer)
{
    float4 lightClip = mul(float4(worldPos, 1.0f), lightViewProjection);
    if (lightClip.w <= 0.000001f)
    {
        return 1.0f;
    }

    float3 lightNdc = lightClip.xyz / lightClip.w;
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
        lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float currentDepth = lightNdc.z - max(shadowMapParams.y, abs(shadowMapParams.z));
    float scale = clamp(Flags.w, 0.25f, 1.0f);

    if (scale >= 0.60f)
    {
        float closestDepth = shadowMap.SampleLevel(
            shadowSampler, float3(shadowUv, shadowLayer), 0);
        return currentDepth <= closestDepth ? 1.0f : 0.0f;
    }

    float texelSize = shadowMapParams.x * (1.0f + saturate(AtmosphereCamera.w) * 2.0f);
    const float2 offsets[4] =
    {
        float2(-0.5f, -0.5f),
        float2( 0.5f, -0.5f),
        float2(-0.5f,  0.5f),
        float2( 0.5f,  0.5f)
    };

    float visibility = 0.0f;
    [unroll]
    for (int tap = 0; tap < 4; ++tap)
    {
        float closestDepth = shadowMap.SampleLevel(
            shadowSampler,
            float3(shadowUv + offsets[tap] * texelSize, shadowLayer),
            0);
        visibility += currentDepth <= closestDepth ? 1.0f : 0.0f;
    }
    return visibility * 0.25f;
}

float3 RayMarchAtmosphereViewFixed(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerState shadowSampler,
    float4x4 lightViewProjection,
    float4 shadowMapParams)
{
    [branch]
    if (AtmosphereParams0.x < 0.5f ||
        AtmosphereColor0.a <= 0.0001f ||
        AtmosphereParams0.w <= 0.0001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 cameraPos = AtmosphereCamera.xyz;
    float3 viewDelta = worldPos - cameraPos;
    float rawViewDistance = length(viewDelta);
    if (rawViewDistance <= 0.0001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float viewDistance = min(rawViewDistance, 100.0f);
    int stepCount = GetAtmosphereStepCount();
    float3 viewDir = viewDelta / rawViewDistance;
    float3 viewToCamera = -viewDir;
    float stepLength = viewDistance / (float)stepCount;
    float scaledStep = stepLength * max(AtmosphereParams1.w, 0.0001f);
    float transmittance = 1.0f;
    float3 result = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 3);

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 0.5f) / (float)stepCount;
        float3 samplePos = cameraPos + viewDir * viewDistance * t;
        float3 stepScatter = float3(0.0f, 0.0f, 0.0f);
        float sampleDensity = AtmosphereDensityCommon(samplePos);

        [loop]
        for (int lightIndex = 0; lightIndex < count; ++lightIndex)
        {
            float3 singleDir;
            float singleAttenuation;
            float singleVolume;
            ResolveSingleLightCommon(
                samplePos,
                LightDirections[lightIndex],
                LightPositionTypes[lightIndex],
                LightExtras[lightIndex],
                singleDir,
                singleAttenuation,
                singleVolume);

            float3 rawSingleColor =
                max(LightColors[lightIndex].rgb, float3(0.0f, 0.0f, 0.0f)) *
                max(LightColors[lightIndex].a, 0.0f);
            float3 singleColor = rawSingleColor * singleAttenuation;

            float shadowVisibility = 1.0f;
            if (LightShadowData[lightIndex].x >= -0.5f)
            {
                const bool virtualDirectional =
                    VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f &&
                    LightPositionTypes[lightIndex].w < 0.5f &&
                    LightShadowData[lightIndex].w < 0.0f;
                if (virtualDirectional)
                {
                    shadowVisibility = SampleVirtualAtmosphereShadowMapCommon(
                        samplePos, shadowMap, shadowSampler);
                }
                else if (VirtualShadowGlobal.x > 1.5f &&
                         LightPositionTypes[lightIndex].w < 0.5f &&
                         LightShadowData[lightIndex].w < 0.0f)
                {
                    shadowVisibility = SampleCascadedAtmosphereShadowMapCommon(
                        samplePos, shadowMap, shadowSampler);
                }
                else
                {
                    float4 perLightShadowParams = float4(
                        LightShadowData[lightIndex].y,
                        LightShadowData[lightIndex].z,
                        LightShadowData[lightIndex].w,
                        1.0f);
                    shadowVisibility = SampleAtmosphereShadowFast(
                        samplePos,
                        singleDir,
                        shadowMap,
                        shadowSampler,
                        LightViewProjections[lightIndex],
                        perLightShadowParams,
                        LightShadowData[lightIndex].x);
                }
            }

            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionTypes[lightIndex].w);
            float volumeVisibility = lerp(shadowVisibility, max(shadowVisibility, 0.45f), localLight);
            float3 volumeColor = rawSingleColor * sqrt(saturate(singleAttenuation)) * volumeVisibility;
            float volumeLight =
                step(2.5f, LightPositionTypes[lightIndex].w) *
                step(LightPositionTypes[lightIndex].w, 3.5f);
            float localVolumeStepBoost = lerp(
                1.0f,
                0.35f / max(AtmosphereParams1.w, 0.0001f),
                localLight);
            float phase = saturate(
                HenyeyGreensteinCommon(
                    clamp(dot(singleDir, viewToCamera), -1.0f, 1.0f),
                    AtmosphereParams1.z) * 6.0f);
            float shaftBoost =
                lerp(0.55f, 1.75f, phase) *
                lerp(1.0f, 2.35f, volumeLight);

            float3 singleAtmosphere = AtmosphereSingleScatterFromDensityCommon(
                sampleDensity,
                viewToCamera,
                singleDir,
                shadowedColor);
            stepScatter += singleAtmosphere * lerp(1.0f, 0.45f, volumeLight);

            float volumeMediumDensity = lerp(
                sampleDensity,
                max(AtmosphereParams0.w, 0.0001f),
                localLight);
            stepScatter +=
                volumeColor * singleVolume * volumeMediumDensity *
                shaftBoost * localVolumeStepBoost *
                lerp(0.22f, lerp(0.82f, 1.05f, volumeLight), localLight);
        }

        if (count <= 0)
        {
            float3 singleDir;
            float singleAttenuation;
            float singleVolume;
            ResolveSingleLightCommon(
                samplePos,
                LightDirection,
                LightPositionType,
                LightExtra,
                singleDir,
                singleAttenuation,
                singleVolume);

            float3 rawSingleColor =
                max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) *
                max(LightColor.a, 0.0f);
            float3 singleColor = rawSingleColor * singleAttenuation;
            float shadowVisibility = 1.0f;
            if (VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f && LightPositionType.w < 0.5f)
            {
                shadowVisibility = SampleVirtualAtmosphereShadowMapCommon(samplePos, shadowMap, shadowSampler);
            }
            else if (VirtualShadowGlobal.x > 1.5f && LightPositionType.w < 0.5f)
            {
                shadowVisibility = SampleCascadedAtmosphereShadowMapCommon(samplePos, shadowMap, shadowSampler);
            }
            else
            {
                shadowVisibility = SampleAtmosphereShadowFast(
                    samplePos,
                    singleDir,
                    shadowMap,
                    shadowSampler,
                    lightViewProjection,
                    shadowMapParams,
                    0.0f);
            }

            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionType.w);
            float volumeVisibility = lerp(shadowVisibility, max(shadowVisibility, 0.45f), localLight);
            float3 volumeColor = rawSingleColor * sqrt(saturate(singleAttenuation)) * volumeVisibility;
            float volumeLight = step(2.5f, LightPositionType.w) * step(LightPositionType.w, 3.5f);
            float localVolumeStepBoost = lerp(
                1.0f,
                0.35f / max(AtmosphereParams1.w, 0.0001f),
                localLight);
            float phase = saturate(
                HenyeyGreensteinCommon(
                    clamp(dot(singleDir, viewToCamera), -1.0f, 1.0f),
                    AtmosphereParams1.z) * 6.0f);
            float shaftBoost =
                lerp(0.55f, 1.75f, phase) *
                lerp(1.0f, 2.35f, volumeLight);
            float3 singleAtmosphere = AtmosphereSingleScatterFromDensityCommon(
                sampleDensity,
                viewToCamera,
                singleDir,
                shadowedColor);
            stepScatter += singleAtmosphere * lerp(1.0f, 0.45f, volumeLight);

            float volumeMediumDensity = lerp(
                sampleDensity,
                max(AtmosphereParams0.w, 0.0001f),
                localLight);
            stepScatter +=
                volumeColor * singleVolume * volumeMediumDensity *
                shaftBoost * localVolumeStepBoost *
                lerp(0.22f, lerp(0.82f, 1.05f, volumeLight), localLight);
        }

        result += stepScatter * transmittance * scaledStep;
        transmittance *= exp(
            -max(AtmosphereParams1.y, 0.0f) *
            sampleDensity * scaledStep);
        if (transmittance <= 0.01f)
        {
            break;
        }
    }

    return result * max(AtmosphereColor0.a, 0.0f);
}

LocalFogRayResult RayMarchLocalFogBackground(float3 rayEnd)
{
    LocalFogRayResult result;
    result.Scattering = float3(0.0f, 0.0f, 0.0f);
    result.Transmittance = 1.0f;

    int fogCount = min((int)round(LocalFogGlobal.x), 16);
    if (fogCount <= 0)
    {
        return result;
    }

    float3 cameraPos = PPCameraPos.xyz;
    float3 viewDelta = rayEnd - cameraPos;
    float rawDistance = length(viewDelta);
    if (rawDistance <= 0.0001f)
    {
        return result;
    }

    float viewDistance = min(rawDistance, 100.0f);
    float3 viewDir = viewDelta / rawDistance;
    int stepCount = GetLocalFogStepCount();
    float stepLength = viewDistance / (float)stepCount;

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 0.5f) / (float)stepCount;
        float3 samplePos = cameraPos + viewDir * viewDistance * t;
        float sampleDensity = 0.0f;
        float3 sampleColorSum = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int fogIndex = 0; fogIndex < fogCount; ++fogIndex)
        {
            float4 data0 = LocalFogData0[fogIndex];
            float4 data1 = LocalFogData1[fogIndex];
            if (data1.w < 0.5f)
            {
                continue;
            }

            float3 delta = samplePos - data0.xyz;
            float radius = max(data0.w, 0.01f);
            float horizontal = saturate(1.0f - length(delta.xz) / radius);
            float heightWeight = exp(-abs(delta.y) * max(data1.x, 0.0001f));
            float sphereWeight = saturate(1.0f - length(delta) / radius);
            float volumeWeight = lerp(
                horizontal * heightWeight,
                sphereWeight,
                step(0.5f, data1.z));
            float contribution = volumeWeight * max(data1.y, 0.0f);

            sampleDensity += contribution;
            sampleColorSum += LocalFogColors[fogIndex].rgb * contribution;
        }

        if (sampleDensity > 0.00001f)
        {
            float3 sampleColor = sampleColorSum / sampleDensity;
            float sampleOpacity = 1.0f - exp(-sampleDensity * stepLength * 0.08f);
            sampleOpacity = saturate(sampleOpacity);
            result.Scattering += sampleColor * sampleOpacity * result.Transmittance;
            result.Transmittance *= 1.0f - sampleOpacity;
        }

        if (result.Transmittance <= 0.01f)
        {
            break;
        }
    }

    return result;
}

float4 main(PSInputPostProcess input) : SV_Target
{
    float4 baseColor = BaseColorTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    float4 position = PositionTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    float depth = DepthTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    float4 material = MaterialTexture.SampleLevel(TextureSampler, input.TexCoord, 0);

    bool background =
        material.a < -0.5f ||
        depth >= 0.9999f ||
        position.w <= 0.0001f;

    float3 viewRay = ReconstructPostProcessViewRayCommon(input.TexCoord);
    float3 rayEnd = background
        ? PPCameraPos.xyz + viewRay * 100.0f
        : position.xyz;

    float3 scatter = RayMarchAtmosphereViewFixed(
        rayEnd,
        ShadowMapTexture,
        ShadowSampler,
        LightViewProjection,
        ShadowMapParams);

    if (background && LocalFogGlobal.x > 0.5f)
    {
        LocalFogRayResult localFog = RayMarchLocalFogBackground(rayEnd);
        float3 sceneBeforeFog =
            baseColor.rgb +
            AtmosphereBackgroundCommon(input.TexCoord) +
            scatter;
        float3 foggedScene =
            sceneBeforeFog * localFog.Transmittance +
            localFog.Scattering;
        scatter += foggedScene - sceneBeforeFog;
    }

    return float4(scatter, 1.0f);
}
