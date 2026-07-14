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

float InterleavedGradientNoiseAtmosphere(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(
        pixelPosition,
        float2(0.06711056f, 0.00583715f))));
}

float SampleIndexedAtmosphereShadow(
    int lightIndex,
    float3 samplePos,
    float3 lightDir)
{
    if (LightShadowData[lightIndex].x < -0.5f)
    {
        return 1.0f;
    }

    float4 perLightShadowParams = float4(
        LightShadowData[lightIndex].y,
        LightShadowData[lightIndex].z,
        LightShadowData[lightIndex].w,
        1.0f);

    return SampleAtmosphereShadowMap(
        samplePos,
        lightDir,
        ShadowMapTexture,
        ShadowSampler,
        LightViewProjections[lightIndex],
        perLightShadowParams,
        LightShadowData[lightIndex].x);
}

bool IntersectVolumetricLightBounds(
    float3 rayOrigin,
    float3 rayDirection,
    float maxRayDistance,
    float3 lightPosition,
    float lightRange,
    out float intervalStart,
    out float intervalEnd)
{
    float3 originToCenter = rayOrigin - lightPosition;
    float projected = dot(originToCenter, rayDirection);
    float discriminant =
        projected * projected -
        (dot(originToCenter, originToCenter) - lightRange * lightRange);

    if (discriminant < 0.0f)
    {
        intervalStart = 0.0f;
        intervalEnd = 0.0f;
        return false;
    }

    float root = sqrt(discriminant);
    intervalStart = max(-projected - root, 0.0f);
    intervalEnd = min(-projected + root, maxRayDistance);
    return intervalEnd > intervalStart;
}

// Sun/directional atmosphere. This intentionally keeps the same global
// atmosphere-density integration that was already producing a stable result.
float3 RayMarchDirectionalAtmosphere(
    float3 rayEnd,
    float2 pixelPosition)
{
    float atmosphereActive =
        step(0.5f, AtmosphereParams0.x) *
        step(0.0001f, AtmosphereColor0.a) *
        step(0.0001f, AtmosphereParams0.w);

    float3 cameraPos = AtmosphereCamera.xyz;
    float3 viewDelta = rayEnd - cameraPos;
    float rawViewDistance = length(viewDelta);
    float validDistance = step(0.0001f, rawViewDistance);
    float viewDistance = clamp(rawViewDistance, 0.0001f, 100.0f);
    float3 viewDir = viewDelta / max(rawViewDistance, 0.0001f);
    float3 viewToCamera = -viewDir;

    const int stepCount = 50;
    float stepLength = viewDistance / (float)stepCount;
    float scaledStep = stepLength * max(AtmosphereParams1.w, 0.0001f);
    float jitter = InterleavedGradientNoiseAtmosphere(pixelPosition);
    float transmittance = 1.0f;
    float3 result = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 5);

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float sampleOffset = 0.25f + jitter * 0.50f;
        float sampleDistance = min(
            ((float)stepIndex + sampleOffset) * stepLength,
            viewDistance);
        float3 samplePos = cameraPos + viewDir * sampleDistance;
        float sampleDensity = AtmosphereDensityCommon(samplePos);
        float3 stepScatter = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int lightIndex = 0; lightIndex < count; ++lightIndex)
        {
            if ((int)round(LightPositionTypes[lightIndex].w) != 0)
            {
                continue;
            }

            float3 lightDir;
            float attenuation;
            float unusedVolume;
            ResolveSingleLightCommon(
                samplePos,
                LightDirections[lightIndex],
                LightPositionTypes[lightIndex],
                LightExtras[lightIndex],
                lightDir,
                attenuation,
                unusedVolume);

            float shadowVisibility = SampleIndexedAtmosphereShadow(
                lightIndex,
                samplePos,
                lightDir);
            float3 lightColor =
                max(LightColors[lightIndex].rgb, float3(0.0f, 0.0f, 0.0f)) *
                max(LightColors[lightIndex].a, 0.0f) *
                attenuation *
                shadowVisibility;

            stepScatter += AtmosphereSingleScatterFromDensityCommon(
                sampleDensity,
                viewToCamera,
                lightDir,
                lightColor);
        }

        if (count <= 0 && (int)round(LightPositionType.w) == 0)
        {
            float3 lightDir;
            float attenuation;
            float unusedVolume;
            ResolveSingleLightCommon(
                samplePos,
                LightDirection,
                LightPositionType,
                LightExtra,
                lightDir,
                attenuation,
                unusedVolume);

            float shadowVisibility = SampleAtmosphereShadowMap(
                samplePos,
                lightDir,
                ShadowMapTexture,
                ShadowSampler,
                LightViewProjection,
                ShadowMapParams,
                0.0f);
            float3 lightColor =
                max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) *
                max(LightColor.a, 0.0f) *
                attenuation *
                shadowVisibility;

            stepScatter += AtmosphereSingleScatterFromDensityCommon(
                sampleDensity,
                viewToCamera,
                lightDir,
                lightColor);
        }

        result += stepScatter * transmittance * scaledStep;
        transmittance *= exp(
            -max(AtmosphereParams1.y, 0.0f) *
            sampleDensity *
            scaledStep);

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

// Point/spot/volume shafts use the same camera-ray volume integration as the
// sun and fog, but only over the ray segment that intersects each local light.
// The march spacing is therefore tied to world space and the light volume,
// rather than to the distance from the camera to the background.
float3 RayMarchLocalLightVolumes(
    float3 rayEnd,
    float2 pixelPosition)
{
    float atmosphereActive =
        step(0.5f, AtmosphereParams0.x) *
        step(0.0001f, AtmosphereColor0.a);

    if (atmosphereActive <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 cameraPos = AtmosphereCamera.xyz;
    float3 viewDelta = rayEnd - cameraPos;
    float rawViewDistance = length(viewDelta);
    float viewDistance = min(rawViewDistance, 100.0f);
    if (viewDistance <= 0.0001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 viewDir = viewDelta / max(rawViewDistance, 0.0001f);
    float3 viewToCamera = -viewDir;
    float baseJitter = InterleavedGradientNoiseAtmosphere(
        pixelPosition + float2(11.0f, 37.0f));
    float3 result = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 5);

    [loop]
    for (int lightIndex = 0; lightIndex < count; ++lightIndex)
    {
        int lightType = (int)round(LightPositionTypes[lightIndex].w);
        if (lightType <= 0)
        {
            continue;
        }

        float lightRange = max(LightDirections[lightIndex].w, 0.01f);
        float intervalStart;
        float intervalEnd;
        if (!IntersectVolumetricLightBounds(
                cameraPos,
                viewDir,
                viewDistance,
                LightPositionTypes[lightIndex].xyz,
                lightRange,
                intervalStart,
                intervalEnd))
        {
            continue;
        }

        float intervalLength = intervalEnd - intervalStart;
        const float targetWorldStep = 0.35f;
        const int minLocalSteps = 8;
        const int maxLocalSteps = 48;
        int localStepCount = clamp(
            (int)ceil(intervalLength / targetWorldStep),
            minLocalSteps,
            maxLocalSteps);
        float localStepLength =
            intervalLength / (float)max(localStepCount, 1);
        float lightJitter = frac(baseJitter + (float)lightIndex * 0.381966f);
        float localTransmittance = 1.0f;
        float3 previousPreviousContribution = float3(0.0f, 0.0f, 0.0f);
        float3 previousContribution = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int stepIndex = 0; stepIndex < maxLocalSteps; ++stepIndex)
        {
            if (stepIndex >= localStepCount)
            {
                break;
            }

            float sampleOffset = 0.20f + lightJitter * 0.60f;
            float sampleDistance =
                intervalStart +
                ((float)stepIndex + sampleOffset) * localStepLength;
            float3 samplePos = cameraPos + viewDir * sampleDistance;

            float3 lightDir;
            float attenuation;
            float mediumDensity;
            ResolveSingleLightCommon(
                samplePos,
                LightDirections[lightIndex],
                LightPositionTypes[lightIndex],
                LightExtras[lightIndex],
                lightDir,
                attenuation,
                mediumDensity);

            mediumDensity = max(mediumDensity, 0.0f);
            float3 contribution = float3(0.0f, 0.0f, 0.0f);
            if (mediumDensity > 0.000001f && attenuation > 0.000001f)
            {
                float shadowVisibility = SampleIndexedAtmosphereShadow(
                    lightIndex,
                    samplePos,
                    lightDir);
                float3 lightColor =
                    max(LightColors[lightIndex].rgb, float3(0.0f, 0.0f, 0.0f)) *
                    max(LightColors[lightIndex].a, 0.0f) *
                    attenuation *
                    shadowVisibility;

                float cosTheta = clamp(
                    dot(lightDir, viewToCamera),
                    -1.0f,
                    1.0f);
                float phase =
                    0.18f +
                    HenyeyGreensteinCommon(
                        cosTheta,
                        AtmosphereParams1.z) *
                    1.35f;

                contribution =
                    lightColor *
                    mediumDensity *
                    phase *
                    localStepLength *
                    0.85f;
            }

            // Small Gaussian along the ray. Jitter breaks coherent bands and
            // this [1 2 1] kernel removes the remaining visible layer edges.
            float3 filteredContribution = contribution;
            if (stepIndex == 1)
            {
                filteredContribution =
                    (previousContribution + contribution) * 0.5f;
            }
            else if (stepIndex > 1)
            {
                filteredContribution =
                    (previousPreviousContribution +
                     previousContribution * 2.0f +
                     contribution) *
                    0.25f;
            }

            result += filteredContribution * localTransmittance;
            previousPreviousContribution = previousContribution;
            previousContribution = contribution;

            localTransmittance *= exp(
                -mediumDensity *
                localStepLength *
                0.10f);
            if (localTransmittance <= 0.01f)
            {
                break;
            }
        }
    }

    if (count <= 0 && (int)round(LightPositionType.w) > 0)
    {
        float lightRange = max(LightDirection.w, 0.01f);
        float intervalStart;
        float intervalEnd;
        if (IntersectVolumetricLightBounds(
                cameraPos,
                viewDir,
                viewDistance,
                LightPositionType.xyz,
                lightRange,
                intervalStart,
                intervalEnd))
        {
            float intervalLength = intervalEnd - intervalStart;
            const int fallbackSteps = 32;
            float stepLength = intervalLength / (float)fallbackSteps;
            float localTransmittance = 1.0f;

            [loop]
            for (int stepIndex = 0; stepIndex < fallbackSteps; ++stepIndex)
            {
                float sampleDistance =
                    intervalStart +
                    ((float)stepIndex + baseJitter) *
                    stepLength;
                float3 samplePos = cameraPos + viewDir * sampleDistance;
                float3 lightDir;
                float attenuation;
                float mediumDensity;
                ResolveSingleLightCommon(
                    samplePos,
                    LightDirection,
                    LightPositionType,
                    LightExtra,
                    lightDir,
                    attenuation,
                    mediumDensity);

                mediumDensity = max(mediumDensity, 0.0f);
                if (mediumDensity <= 0.000001f)
                {
                    continue;
                }

                float shadowVisibility = SampleAtmosphereShadowMap(
                    samplePos,
                    lightDir,
                    ShadowMapTexture,
                    ShadowSampler,
                    LightViewProjection,
                    ShadowMapParams,
                    0.0f);
                float3 lightColor =
                    max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) *
                    max(LightColor.a, 0.0f) *
                    attenuation *
                    shadowVisibility;
                float phase =
                    0.18f +
                    HenyeyGreensteinCommon(
                        clamp(dot(lightDir, viewToCamera), -1.0f, 1.0f),
                        AtmosphereParams1.z) *
                    1.35f;

                result +=
                    lightColor *
                    mediumDensity *
                    phase *
                    stepLength *
                    0.85f *
                    localTransmittance;
                localTransmittance *= exp(
                    -mediumDensity *
                    stepLength *
                    0.10f);
            }
        }
    }

    return result * max(AtmosphereColor0.a, 0.0f);
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
    float sampleJitter =
        InterleavedGradientNoiseAtmosphere(pixelPosition + 17.0f);

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
            float horizontal =
                saturate(1.0f - length(delta.xz) / radius);
            float heightWeight =
                exp(-abs(delta.y) * max(data1.x, 0.0001f));
            float sphereWeight =
                saturate(1.0f - length(delta) / radius);
            float volumeWeight = lerp(
                horizontal * heightWeight,
                sphereWeight,
                step(0.5f, data1.z));
            float contribution =
                volumeWeight * max(data1.y, 0.0f);

            sampleDensity += contribution;
            sampleColorSum +=
                LocalFogColors[fogIndex].rgb * contribution;
        }

        if (sampleDensity > 0.00001f)
        {
            float3 sampleColor =
                sampleColorSum / sampleDensity;
            float sampleOpacity =
                1.0f -
                exp(-sampleDensity * stepLength * 0.08f);
            sampleOpacity = saturate(sampleOpacity);
            result.Scattering +=
                sampleColor *
                sampleOpacity *
                result.Transmittance;
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
        ReconstructPostProcessViewRayCommon(input.TexCoord);
    float3 rayEnd = background
        ? PPCameraPos.xyz + viewRay * 100.0f
        : position.xyz;

    float3 scatter =
        RayMarchDirectionalAtmosphere(
            rayEnd,
            input.Position.xy);
    scatter +=
        RayMarchLocalLightVolumes(
            rayEnd,
            input.Position.xy);

    if (background)
    {
        LocalFogRayResult localFog =
            RayMarchLocalFogBackground(
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
