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

// Stable per-pixel sub-step offset. It converts coherent ray-march bands into
// fine noise that the existing linear upsample/TAA can remove much more easily.
float InterleavedGradientNoiseAtmosphere(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

// The cone radius approaches zero at a point/spot/volume light origin. Without
// an emitter-sized fade, one ray-march sample can become a camera-dependent hot
// blob. Fade over a small world-space region derived from light range instead.
float LocalLightEmitterFade(
    float3 samplePos,
    float4 lightDirectionData,
    float4 lightPositionTypeData)
{
    int lightType = (int)round(lightPositionTypeData.w);
    if (lightType <= 0)
    {
        return 1.0f;
    }

    float lightRange = max(lightDirectionData.w, 0.01f);
    float3 fromLight = samplePos - lightPositionTypeData.xyz;
    float sourceDistance = length(fromLight);

    if (lightType == 2 || lightType == 3)
    {
        float3 spotForward = SafeNormalizeCommon(
            lightDirectionData.xyz,
            float3(0.0f, -1.0f, 0.0f));
        float axialDistance = dot(fromLight, spotForward);
        sourceDistance = (lightType == 3)
            ? abs(axialDistance)
            : max(axialDistance, 0.0f);
    }

    float sourceRadiusRatio = (lightType == 3)
        ? 0.050f
        : ((lightType == 2) ? 0.025f : 0.015f);
    float fadeStart = max(lightRange * sourceRadiusRatio, 0.05f);
    float fadeEnd = fadeStart * 2.75f;
    return smoothstep(fadeStart, fadeEnd, sourceDistance);
}

float3 RayMarchAtmosphereViewFixed(
    float3 worldPos,
    float2 pixelPosition,
    Texture2DArray<float> shadowMap,
    SamplerState shadowSampler,
    float4x4 lightViewProjection,
    float4 shadowMapParams)
{
    float atmosphereActive =
        step(0.5f, AtmosphereParams0.x) *
        step(0.0001f, AtmosphereColor0.a) *
        step(0.0001f, AtmosphereParams0.w);

    float3 cameraPos = AtmosphereCamera.xyz;
    float3 viewDelta = worldPos - cameraPos;
    float rawViewDistance = length(viewDelta);
    float validDistance = step(0.0001f, rawViewDistance);
    float viewDistance = clamp(rawViewDistance, 0.0001f, 100.0f);

    // Keep approximately the same world-space distance between samples as the
    // camera moves. The old fixed 50 steps made the first cone sample move by
    // metres on long background rays, which changed the apparent shaft root.
    const float targetWorldStep = 1.20f;
    const int minStepCount = 32;
    const int maxStepCount = 64;
    int stepCount = clamp(
        (int)ceil(viewDistance / targetWorldStep),
        minStepCount,
        maxStepCount);

    float3 viewDir = viewDelta / max(rawViewDistance, 0.0001f);
    float3 viewToCamera = -viewDir;
    float stepLength = viewDistance / (float)max(stepCount, 1);
    float scaledStep = stepLength * max(AtmosphereParams1.w, 0.0001f);
    float sampleJitter = InterleavedGradientNoiseAtmosphere(pixelPosition);
    float transmittance = 1.0f;
    float3 result = float3(0.0f, 0.0f, 0.0f);
    float3 previousPreviousStepScatter = float3(0.0f, 0.0f, 0.0f);
    float3 previousStepScatter = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 5);

    [loop]
    for (int stepIndex = 0; stepIndex < maxStepCount; ++stepIndex)
    {
        if (stepIndex >= stepCount)
        {
            break;
        }

        float sampleOffset = 0.20f + sampleJitter * 0.60f;
        float sampleDistance = min(
            ((float)stepIndex + sampleOffset) * stepLength,
            viewDistance);
        float3 samplePos = cameraPos + viewDir * sampleDistance;
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
                float4 perLightShadowParams = float4(
                    LightShadowData[lightIndex].y,
                    LightShadowData[lightIndex].z,
                    LightShadowData[lightIndex].w,
                    1.0f);
                shadowVisibility = SampleAtmosphereShadowMap(
                    samplePos,
                    singleDir,
                    shadowMap,
                    shadowSampler,
                    LightViewProjections[lightIndex],
                    perLightShadowParams,
                    LightShadowData[lightIndex].x);
            }

            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionTypes[lightIndex].w);
            float volumeVisibility = lerp(
                shadowVisibility,
                max(shadowVisibility, 0.45f),
                localLight);
            float3 volumeColor =
                rawSingleColor *
                sqrt(saturate(singleAttenuation)) *
                volumeVisibility;
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
            float emitterFade = LocalLightEmitterFade(
                samplePos,
                LightDirections[lightIndex],
                LightPositionTypes[lightIndex]);
            stepScatter +=
                volumeColor * singleVolume * volumeMediumDensity *
                shaftBoost * localVolumeStepBoost * emitterFade *
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
            float shadowVisibility = SampleAtmosphereShadowMap(
                samplePos,
                singleDir,
                shadowMap,
                shadowSampler,
                lightViewProjection,
                shadowMapParams,
                0.0f);
            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionType.w);
            float volumeVisibility = lerp(
                shadowVisibility,
                max(shadowVisibility, 0.45f),
                localLight);
            float3 volumeColor =
                rawSingleColor *
                sqrt(saturate(singleAttenuation)) *
                volumeVisibility;
            float volumeLight =
                step(2.5f, LightPositionType.w) *
                step(LightPositionType.w, 3.5f);
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
            float emitterFade = LocalLightEmitterFade(
                samplePos,
                LightDirection,
                LightPositionType);
            stepScatter +=
                volumeColor * singleVolume * volumeMediumDensity *
                shaftBoost * localVolumeStepBoost * emitterFade *
                lerp(0.22f, lerp(0.82f, 1.05f, volumeLight), localLight);
        }

        // A compact [1 2 1] Gaussian over adjacent world-space samples hides
        // the remaining cell boundaries without a separate full-screen pass.
        float3 filteredStepScatter = stepScatter;
        if (stepIndex == 1)
        {
            filteredStepScatter = (previousStepScatter + stepScatter) * 0.5f;
        }
        else if (stepIndex > 1)
        {
            filteredStepScatter =
                (previousPreviousStepScatter +
                 previousStepScatter * 2.0f +
                 stepScatter) * 0.25f;
        }
        result += filteredStepScatter * transmittance * scaledStep;
        previousPreviousStepScatter = previousStepScatter;
        previousStepScatter = stepScatter;

        transmittance *= exp(
            -max(AtmosphereParams1.y, 0.0f) *
            sampleDensity * scaledStep);
        if (transmittance <= 0.01f)
        {
            break;
        }
    }

    return result *
        max(AtmosphereColor0.a, 0.0f) *
        atmosphereActive *
        validDistance;
}

LocalFogRayResult RayMarchLocalFogBackground(
    float3 rayEnd,
    float2 pixelPosition)
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
    float viewDistance = clamp(rawDistance, 0.0001f, 100.0f);
    float3 viewDir = viewDelta / max(rawDistance, 0.0001f);

    const float targetWorldStep = 0.90f;
    const int minStepCount = 32;
    const int maxStepCount = 64;
    int stepCount = clamp(
        (int)ceil(viewDistance / targetWorldStep),
        minStepCount,
        maxStepCount);
    float stepLength = viewDistance / (float)max(stepCount, 1);
    float sampleJitter = InterleavedGradientNoiseAtmosphere(pixelPosition + 17.0f);

    [loop]
    for (int stepIndex = 0; stepIndex < maxStepCount; ++stepIndex)
    {
        if (stepIndex >= stepCount)
        {
            break;
        }

        float sampleOffset = 0.20f + sampleJitter * 0.60f;
        float sampleDistance = min(
            ((float)stepIndex + sampleOffset) * stepLength,
            viewDistance);
        float3 samplePos = cameraPos + viewDir * sampleDistance;
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
            result.Scattering +=
                sampleColor * sampleOpacity * result.Transmittance;
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
        input.Position.xy,
        ShadowMapTexture,
        ShadowSampler,
        LightViewProjection,
        ShadowMapParams);

    if (background)
    {
        LocalFogRayResult localFog = RayMarchLocalFogBackground(
            rayEnd,
            input.Position.xy);

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
