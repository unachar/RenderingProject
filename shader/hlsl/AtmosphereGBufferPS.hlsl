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

int GetAtmosphereStepCount(float viewDistance)
{
    // Keep the world-space distance between samples reasonably stable. The
    // previous resolution-only 10 step march made one sample represent up to
    // ten world units, which caused volume lights to grow and shrink as the
    // camera moved.
    float scale = clamp(Flags.w, 0.25f, 1.0f);
    float lowResolutionWeight = saturate((1.0f - scale) / 0.75f);
    int maxSteps = (int)round(lerp(28.0f, 36.0f, lowResolutionWeight));
    int distanceSteps = (int)ceil(viewDistance / 2.5f);
    return clamp(distanceSteps, 12, maxSteps);
}

float SampleAtmosphereShadowFast(
    float3 worldPos,
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
    float2 shadowUv = float2(
        lightNdc.x * 0.5f + 0.5f,
        -lightNdc.y * 0.5f + 0.5f);

    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) ||
        lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float bias = max(shadowMapParams.y, shadowMapParams.z);
    float currentDepth = lightNdc.z - bias;
    float texelSize =
        shadowMapParams.x *
        (1.0f + saturate(AtmosphereCamera.w) * 2.0f);

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

    visibility *= 0.25f;
    return lerp(
        1.0f,
        visibility,
        saturate(abs(shadowMapParams.w)));
}

float ComputeAtmosphereShadow(
    float3 samplePos,
    float3 lightDir,
    float4 lightPositionTypeData,
    float4x4 lightViewProjection,
    float4 shadowParams,
    float shadowLayer,
    bool hasShadow,
    bool directionalMultiLevel,
    Texture2DArray<float> shadowMap,
    SamplerState shadowSampler)
{
    if (!hasShadow)
    {
        return 1.0f;
    }

    bool directional = lightPositionTypeData.w < 0.5f;

    if (directional &&
        directionalMultiLevel &&
        VirtualShadowGlobal.x > 0.5f &&
        VirtualShadowGlobal.x < 1.5f)
    {
        return SampleVirtualAtmosphereShadowMapCommon(
            samplePos,
            shadowMap,
            shadowSampler);
    }

    if (directional &&
        directionalMultiLevel &&
        VirtualShadowGlobal.x > 1.5f)
    {
        return SampleCascadedAtmosphereShadowMapCommon(
            samplePos,
            shadowMap,
            shadowSampler);
    }

    return SampleAtmosphereShadowFast(
        samplePos,
        shadowMap,
        shadowSampler,
        lightViewProjection,
        shadowParams,
        shadowLayer);
}

float3 EvaluateAtmosphereLight(
    float3 viewToCamera,
    float sampleDensity,
    float3 singleDir,
    float singleAttenuation,
    float singleVolume,
    float4 lightColorData,
    float4 lightPositionTypeData,
    float shadowVisibility)
{
    float3 rawSingleColor =
        max(lightColorData.rgb, float3(0.0f, 0.0f, 0.0f)) *
        max(lightColorData.a, 0.0f);
    float3 singleColor = rawSingleColor * singleAttenuation;

    float3 shadowedColor = singleColor * shadowVisibility;
    float localLight = step(0.5f, lightPositionTypeData.w);
    float volumeVisibility = lerp(
        shadowVisibility,
        max(shadowVisibility, 0.45f),
        localLight);
    float3 volumeColor =
        rawSingleColor *
        sqrt(saturate(singleAttenuation)) *
        volumeVisibility;

    float volumeLight =
        step(2.5f, lightPositionTypeData.w) *
        step(lightPositionTypeData.w, 3.5f);

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

    float3 result =
        AtmosphereSingleScatterFromDensityCommon(
            sampleDensity,
            viewToCamera,
            singleDir,
            shadowedColor) *
        lerp(1.0f, 0.45f, volumeLight);

    // Directional light follows the global atmosphere height profile.
    // Local point/spot/volume lights use their own medium density so their
    // shafts do not disappear when the light is moved above world Y = 0.
    float volumeMediumDensity = lerp(
        sampleDensity,
        max(AtmosphereParams0.w, 0.0001f),
        localLight);

    result +=
        volumeColor *
        singleVolume *
        volumeMediumDensity *
        shaftBoost *
        localVolumeStepBoost *
        lerp(
            0.22f,
            lerp(0.82f, 1.05f, volumeLight),
            localLight);

    return result;
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
    int stepCount = GetAtmosphereStepCount(viewDistance);
    float3 viewDir = viewDelta / rawViewDistance;
    float3 viewToCamera = -viewDir;
    float stepLength = viewDistance / (float)stepCount;
    float scaledStep =
        stepLength *
        max(AtmosphereParams1.w, 0.0001f);

    float transmittance = 1.0f;
    float3 result = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 5);

    float cachedShadowVisibility[5];
    [unroll]
    for (int cacheIndex = 0; cacheIndex < 5; ++cacheIndex)
    {
        cachedShadowVisibility[cacheIndex] = 1.0f;
    }

    float legacyShadowVisibility = 1.0f;

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t =
            ((float)stepIndex + 0.5f) /
            (float)stepCount;
        float3 samplePos =
            cameraPos +
            viewDir * viewDistance * t;
        float3 stepScatter = float3(0.0f, 0.0f, 0.0f);
        float sampleDensity =
            AtmosphereDensityCommon(samplePos);

        // Shadow visibility changes much more slowly than the volume density.
        // Reusing it for four adjacent march samples preserves the shaft shape
        // while avoiding a 4-tap PCF lookup on every sample.
        bool updateShadow = (stepIndex & 3) == 0;

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

            if (updateShadow)
            {
                bool hasShadow =
                    LightShadowData[lightIndex].x >= -0.5f;
                bool directionalMultiLevel =
                    LightShadowData[lightIndex].w < 0.0f;

                float4 perLightShadowParams = float4(
                    LightShadowData[lightIndex].y,
                    LightShadowData[lightIndex].z,
                    LightShadowData[lightIndex].w,
                    1.0f);

                cachedShadowVisibility[lightIndex] =
                    ComputeAtmosphereShadow(
                        samplePos,
                        singleDir,
                        LightPositionTypes[lightIndex],
                        LightViewProjections[lightIndex],
                        perLightShadowParams,
                        LightShadowData[lightIndex].x,
                        hasShadow,
                        directionalMultiLevel,
                        shadowMap,
                        shadowSampler);
            }

            stepScatter += EvaluateAtmosphereLight(
                viewToCamera,
                sampleDensity,
                singleDir,
                singleAttenuation,
                singleVolume,
                LightColors[lightIndex],
                LightPositionTypes[lightIndex],
                cachedShadowVisibility[lightIndex]);
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

            if (updateShadow)
            {
                legacyShadowVisibility =
                    ComputeAtmosphereShadow(
                        samplePos,
                        singleDir,
                        LightPositionType,
                        lightViewProjection,
                        shadowMapParams,
                        0.0f,
                        true,
                        false,
                        shadowMap,
                        shadowSampler);
            }

            stepScatter += EvaluateAtmosphereLight(
                viewToCamera,
                sampleDensity,
                singleDir,
                singleAttenuation,
                singleVolume,
                LightColor,
                LightPositionType,
                legacyShadowVisibility);
        }

        result +=
            stepScatter *
            transmittance *
            scaledStep;

        transmittance *= exp(
            -max(AtmosphereParams1.y, 0.0f) *
            sampleDensity *
            scaledStep);

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

    int fogCount =
        min((int)round(LocalFogGlobal.x), 16);
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

    // Local fog volumes are relatively small in world space. Keeping the
    // original 32 samples prevents them from popping or scaling with camera
    // distance. This path runs only for background pixels and only when at
    // least one local fog volume is active.
    const int stepCount = 32;
    float stepLength =
        viewDistance /
        (float)stepCount;

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t =
            ((float)stepIndex + 0.5f) /
            (float)stepCount;
        float3 samplePos =
            cameraPos +
            viewDir * viewDistance * t;

        float sampleDensity = 0.0f;
        float3 sampleColorSum =
            float3(0.0f, 0.0f, 0.0f);

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
            float horizontal =
                saturate(
                    1.0f -
                    length(delta.xz) /
                    radius);
            float heightWeight = exp(
                -abs(delta.y) *
                max(data1.x, 0.0001f));
            float sphereWeight =
                saturate(
                    1.0f -
                    length(delta) /
                    radius);

            float volumeWeight = lerp(
                horizontal * heightWeight,
                sphereWeight,
                step(0.5f, data1.z));

            float contribution =
                volumeWeight *
                max(data1.y, 0.0f);

            sampleDensity += contribution;
            sampleColorSum +=
                LocalFogColors[fogIndex].rgb *
                contribution;
        }

        if (sampleDensity > 0.00001f)
        {
            float3 sampleColor =
                sampleColorSum /
                sampleDensity;
            float sampleOpacity =
                1.0f -
                exp(
                    -sampleDensity *
                    stepLength *
                    0.08f);
            sampleOpacity =
                saturate(sampleOpacity);

            result.Scattering +=
                sampleColor *
                sampleOpacity *
                result.Transmittance;
            result.Transmittance *=
                1.0f -
                sampleOpacity;
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
    float4 baseColor =
        BaseColorTexture.SampleLevel(
            TextureSampler,
            input.TexCoord,
            0);
    float4 position =
        PositionTexture.SampleLevel(
            TextureSampler,
            input.TexCoord,
            0);
    float depth =
        DepthTexture.SampleLevel(
            TextureSampler,
            input.TexCoord,
            0);
    float4 material =
        MaterialTexture.SampleLevel(
            TextureSampler,
            input.TexCoord,
            0);

    bool background =
        material.a < -0.5f ||
        depth >= 0.9999f ||
        position.w <= 0.0001f;

    float3 viewRay =
        ReconstructPostProcessViewRayCommon(
            input.TexCoord);
    float3 rayEnd =
        background
        ? PPCameraPos.xyz +
          viewRay * 100.0f
        : position.xyz;

    float3 scatter =
        RayMarchAtmosphereViewFixed(
            rayEnd,
            ShadowMapTexture,
            ShadowSampler,
            LightViewProjection,
            ShadowMapParams);

    if (background)
    {
        LocalFogRayResult localFog =
            RayMarchLocalFogBackground(rayEnd);

        float3 sceneBeforeFog =
            baseColor.rgb +
            AtmosphereBackgroundCommon(input.TexCoord) +
            scatter;
        float3 foggedScene =
            sceneBeforeFog *
            localFog.Transmittance +
            localFog.Scattering;

        // DeferredLightingPS adds base color and analytic sky later. Store the
        // signed delta so the final background becomes the correctly fogged
        // scene instead of adding fog on top of it.
        scatter +=
            foggedScene -
            sceneBeforeFog;
    }

    return float4(scatter, 1.0f);
}
