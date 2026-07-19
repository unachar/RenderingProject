#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float4> MaterialTexture : register(t3);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);

SamplerState TextureSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct LocalFogRayResult
{
    float3 Scattering;
    float Transmittance;
};

int GetAtmosphereStepCount(float viewDistance)
{



    float scale = clamp(Flags.w, 0.25f, 1.0f);
    float lowResolutionWeight = saturate((1.0f - scale) / 0.75f);
    int maxSteps = (int)round(lerp(28.0f, 36.0f, lowResolutionWeight));
    int distanceSteps = (int)ceil(viewDistance / 2.5f);
    return clamp(distanceSteps, 12, maxSteps);
}

float SampleAtmosphereShadowFast(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
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
        visibility += shadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(shadowUv + offsets[tap] * texelSize, shadowLayer),
            currentDepth);
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
    SamplerComparisonState shadowSampler)
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





    float cosTheta = clamp(dot(singleDir, viewToCamera), -1.0f, 1.0f);
    float rayleighPhase = RayleighPhaseCommon(cosTheta);
    float miePhase = HenyeyGreensteinCommon(cosTheta, AtmosphereParams1.z);
    float phase = saturate(miePhase * 6.0f);

    float shaftBoost =
        lerp(0.55f, 1.75f, phase) *
        lerp(1.0f, 2.35f, volumeLight);

    float3 atmospherePhase =
        AtmosphereColor0.rgb * max(AtmosphereParams0.y, 0.0f) * rayleighPhase +
        AtmosphereColor1.rgb * max(AtmosphereParams0.z, 0.0f) * miePhase;
    float3 result =
        atmospherePhase * sampleDensity * shadowedColor *
        lerp(1.0f, 0.45f, volumeLight);




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

bool IntersectRaySphereSegment(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    float3 sphereCenter,
    float sphereRadius,
    out float segmentStart,
    out float segmentEnd)
{
    float3 offset = rayOrigin - sphereCenter;
    float projected = dot(offset, rayDirection);
    float discriminant =
        projected * projected -
        (dot(offset, offset) - sphereRadius * sphereRadius);

    if (discriminant <= 0.0f)
    {
        segmentStart = 0.0f;
        segmentEnd = 0.0f;
        return false;
    }

    float root = sqrt(discriminant);
    segmentStart = max(-projected - root, 0.0f);
    segmentEnd = min(-projected + root, maxDistance);
    return segmentEnd > segmentStart + 0.0001f;
}

bool IntersectRayHorizontalCylinderSegment(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    float3 cylinderCenter,
    float cylinderRadius,
    out float segmentStart,
    out float segmentEnd)
{
    float2 offset = rayOrigin.xz - cylinderCenter.xz;
    float2 direction = rayDirection.xz;
    float a = dot(direction, direction);
    float c = dot(offset, offset) - cylinderRadius * cylinderRadius;

    if (a <= 0.000001f)
    {
        segmentStart = 0.0f;
        segmentEnd = c <= 0.0f ? maxDistance : 0.0f;
        return segmentEnd > segmentStart + 0.0001f;
    }

    float b = dot(offset, direction);
    float discriminant = b * b - a * c;
    if (discriminant <= 0.0f)
    {
        segmentStart = 0.0f;
        segmentEnd = 0.0f;
        return false;
    }

    float root = sqrt(discriminant);
    segmentStart = max((-b - root) / a, 0.0f);
    segmentEnd = min((-b + root) / a, maxDistance);
    return segmentEnd > segmentStart + 0.0001f;
}

bool ClipRayToQuadraticInterior(
    float quadraticA,
    float quadraticB,
    float quadraticC,
    inout float segmentStart,
    inout float segmentEnd)
{
    const float epsilon = 0.000001f;

    if (abs(quadraticA) <= epsilon)
    {
        if (abs(quadraticB) <= epsilon)
        {
            return quadraticC <= 0.0f;
        }

        float root = -quadraticC / quadraticB;
        if (quadraticB > 0.0f)
        {
            segmentEnd = min(segmentEnd, root);
        }
        else
        {
            segmentStart = max(segmentStart, root);
        }
        return segmentEnd > segmentStart + 0.0001f;
    }

    float discriminant =
        quadraticB * quadraticB -
        4.0f * quadraticA * quadraticC;
    if (discriminant < 0.0f)
    {
        float midpoint = (segmentStart + segmentEnd) * 0.5f;
        float midpointValue =
            (quadraticA * midpoint + quadraticB) * midpoint +
            quadraticC;
        return midpointValue <= 0.0f;
    }

    float rootTerm = sqrt(max(discriminant, 0.0f));
    float reciprocalDenominator = 0.5f / quadraticA;
    float root0 =
        (-quadraticB - rootTerm) * reciprocalDenominator;
    float root1 =
        (-quadraticB + rootTerm) * reciprocalDenominator;
    if (root0 > root1)
    {
        float swapRoot = root0;
        root0 = root1;
        root1 = swapRoot;
    }

    if (quadraticA > 0.0f)
    {
        segmentStart = max(segmentStart, root0);
        segmentEnd = min(segmentEnd, root1);
        return segmentEnd > segmentStart + 0.0001f;
    }




    if (segmentEnd <= root0 || segmentStart >= root1)
    {
        return true;
    }
    if (segmentStart < root0)
    {
        segmentEnd = min(segmentEnd, root0);
    }
    else if (segmentEnd > root1)
    {
        segmentStart = max(segmentStart, root1);
    }
    else
    {
        return false;
    }
    return segmentEnd > segmentStart + 0.0001f;
}

bool IntersectRayFiniteConeSegment(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    float3 coneOrigin,
    float3 coneAxis,
    float coneLength,
    float rootRadius,
    float endRadius,
    out float segmentStart,
    out float segmentEnd)
{
    float3 offset = rayOrigin - coneOrigin;
    float originAxial = dot(offset, coneAxis);
    float directionAxial = dot(rayDirection, coneAxis);

    segmentStart = 0.0f;
    segmentEnd = maxDistance;

    if (abs(directionAxial) <= 0.000001f)
    {
        if (originAxial < 0.0f || originAxial > coneLength)
        {
            return false;
        }
    }
    else
    {
        float cap0 = -originAxial / directionAxial;
        float cap1 = (coneLength - originAxial) / directionAxial;
        float axialStart = min(cap0, cap1);
        float axialEnd = max(cap0, cap1);
        segmentStart = max(segmentStart, axialStart);
        segmentEnd = min(segmentEnd, axialEnd);
        if (segmentEnd <= segmentStart + 0.0001f)
        {
            return false;
        }
    }

    float radiusSlope =
        (endRadius - rootRadius) /
        max(coneLength, 0.0001f);
    float radiusAtRayOrigin =
        rootRadius + radiusSlope * originAxial;
    float radiusAlongRay = radiusSlope * directionAxial;
    float3 offsetRadial = offset - coneAxis * originAxial;
    float3 directionRadial =
        rayDirection - coneAxis * directionAxial;

    float quadraticA =
        dot(directionRadial, directionRadial) -
        radiusAlongRay * radiusAlongRay;
    float quadraticB = 2.0f * (
        dot(offsetRadial, directionRadial) -
        radiusAtRayOrigin * radiusAlongRay);
    float quadraticC =
        dot(offsetRadial, offsetRadial) -
        radiusAtRayOrigin * radiusAtRayOrigin;

    return ClipRayToQuadraticInterior(
        quadraticA,
        quadraticB,
        quadraticC,
        segmentStart,
        segmentEnd);
}

float3 IntegrateVolumeLightRay(
    float3 rayOrigin,
    float3 rayDirection,
    float maxDistance,
    float4 lightDirectionData,
    float4 lightColorData,
    float4 lightPositionTypeData,
    float4 lightExtraData,
    float4x4 lightViewProjection,
    float4 shadowParams,
    float shadowLayer,
    bool hasShadow,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler)
{
    if ((int)round(lightPositionTypeData.w) != 3 ||
        lightExtraData.z <= 0.0001f ||
        lightColorData.a <= 0.0001f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float lightRange = max(lightDirectionData.w, 0.01f);
    float3 lightForward = SafeNormalizeCommon(
        lightDirectionData.xyz,
        float3(0.0f, -1.0f, 0.0f));
    float outerCos = clamp(lightExtraData.y, 0.001f, 0.999f);
    float outerTan =
        sqrt(saturate(1.0f - outerCos * outerCos)) /
        outerCos;
    float coneEndRadius = lightRange * outerTan;
    float coneRootRadius = max(coneEndRadius * 0.04f, 0.05f);
    float cylinderRadius = coneEndRadius * 0.45f;
    bool cylinderVolume = lightExtraData.w >= 0.5f;
    float volumeRootRadius = cylinderVolume
        ? cylinderRadius
        : coneRootRadius;
    float volumeEndRadius = cylinderVolume
        ? cylinderRadius
        : coneEndRadius;

    float segmentStart;
    float segmentEnd;
    if (!IntersectRayFiniteConeSegment(
            rayOrigin,
            rayDirection,
            maxDistance,
            lightPositionTypeData.xyz,
            lightForward,
            lightRange,
            volumeRootRadius,
            volumeEndRadius,
            segmentStart,
            segmentEnd))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }




    const int stepCount = 24;
    float segmentLength = segmentEnd - segmentStart;
    float stepLength = segmentLength / (float)stepCount;
    float scaledStep =
        stepLength *
        max(AtmosphereParams1.w, 0.0001f);
    float3 viewToCamera = -rayDirection;
    float3 integratedScatter = float3(0.0f, 0.0f, 0.0f);
    float3 sampleStep = rayDirection * stepLength;
    float3 samplePos =
        rayOrigin + rayDirection * (segmentStart + 0.5f * stepLength);

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float sampleDistance =
            segmentStart +
            ((float)stepIndex + 0.5f) * stepLength;

        float3 singleDir;
        float singleAttenuation;
        float singleVolume;
        ResolveSingleLightCommon(
            samplePos,
            lightDirectionData,
            lightPositionTypeData,
            lightExtraData,
            singleDir,
            singleAttenuation,
            singleVolume);

        if (singleVolume <= 0.000001f &&
            singleAttenuation <= 0.000001f)
        {
            continue;
        }

        float shadowVisibility = ComputeAtmosphereShadow(
            samplePos,
            singleDir,
            lightPositionTypeData,
            lightViewProjection,
            shadowParams,
            shadowLayer,
            hasShadow,
            false,
            shadowMap,
            shadowSampler);
        float sampleDensity = AtmosphereDensityCommon(samplePos);
        float viewTransmittance = exp(
            -max(AtmosphereParams1.y, 0.0f) *
            sampleDensity *
            sampleDistance *
            max(AtmosphereParams1.w, 0.0001f));

        integratedScatter +=
            EvaluateAtmosphereLight(
                viewToCamera,
                sampleDensity,
                singleDir,
                singleAttenuation,
                singleVolume,
                lightColorData,
                lightPositionTypeData,
                shadowVisibility) *
            viewTransmittance *
            scaledStep;

        samplePos += sampleStep;
    }

    return integratedScatter;
}

float3 RayMarchAtmosphereViewFixed(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
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
    int count = min((int)round(LocalFogGlobal.y), 5);
    float3 sampleStep = viewDir * stepLength;
    float3 samplePos = cameraPos + sampleStep * 0.5f;

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
        float3 stepScatter = float3(0.0f, 0.0f, 0.0f);
        float sampleDensity =
            AtmosphereDensityCommon(samplePos);




        bool updateShadow = (stepIndex & 3) == 0;

        [loop]
        for (int instanceIndex = 0; instanceIndex < count; ++instanceIndex)
        {
            int lightIndex = (int)VolumetricLightIndices[instanceIndex];
            if (LightFlags[lightIndex].z < 0.5f ||
                LightFlags[lightIndex].w >= 0.5f)
            {
                continue;
            }




            if ((int)round(LightPositionTypes[lightIndex].w) == 3)
            {
                continue;
            }

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

        if (count <= 0 &&
            (int)round(LightPositionType.w) != 3)
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

        samplePos += sampleStep;
    }

    [loop]
    for (int instanceIndex = 0; instanceIndex < count; ++instanceIndex)
    {
        int volumeIndex = (int)VolumetricLightIndices[instanceIndex];
        if (LightFlags[volumeIndex].z < 0.5f ||
            LightFlags[volumeIndex].w >= 0.5f ||
            (int)round(LightPositionTypes[volumeIndex].w) != 3)
        {
            continue;
        }

        float4 perLightShadowParams = float4(
            LightShadowData[volumeIndex].y,
            LightShadowData[volumeIndex].z,
            LightShadowData[volumeIndex].w,
            1.0f);
        result += IntegrateVolumeLightRay(
            cameraPos,
            viewDir,
            viewDistance,
            LightDirections[volumeIndex],
            LightColors[volumeIndex],
            LightPositionTypes[volumeIndex],
            LightExtras[volumeIndex],
            LightViewProjections[volumeIndex],
            perLightShadowParams,
            LightShadowData[volumeIndex].x,
            LightShadowData[volumeIndex].x >= -0.5f,
            shadowMap,
            shadowSampler);
    }

    if (count <= 0 &&
        (int)round(LightPositionType.w) == 3)
    {
        result += IntegrateVolumeLightRay(
            cameraPos,
            viewDir,
            viewDistance,
            LightDirection,
            LightColor,
            LightPositionType,
            LightExtra,
            lightViewProjection,
            shadowMapParams,
            0.0f,
            true,
            shadowMap,
            shadowSampler);
    }

    return result * max(AtmosphereColor0.a, 0.0f);
}

LocalFogRayResult IntegrateLocalFogRay(float3 rayEnd)
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

    float totalOpticalDepth = 0.0f;
    float3 opticalDepthColor = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int fogIndex = 0; fogIndex < fogCount; ++fogIndex)
    {
        float4 data0 = LocalFogData0[fogIndex];
        float4 data1 = LocalFogData1[fogIndex];
        if (data1.w < 0.5f || data1.y <= 0.00001f)
        {
            continue;
        }

        float radius = max(data0.w, 0.01f);
        bool sphereFog = data1.z >= 0.5f;
        float segmentStart;
        float segmentEnd;
        bool intersects = sphereFog
            ? IntersectRaySphereSegment(
                cameraPos,
                viewDir,
                viewDistance,
                data0.xyz,
                radius,
                segmentStart,
                segmentEnd)
            : IntersectRayHorizontalCylinderSegment(
                cameraPos,
                viewDir,
                viewDistance,
                data0.xyz,
                radius,
                segmentStart,
                segmentEnd);

        if (!intersects)
        {
            continue;
        }




        const int stepCount = 24;
        float segmentLength = segmentEnd - segmentStart;
        float stepLength = segmentLength / (float)stepCount;
        float densityIntegral = 0.0f;
        float3 sampleStep = viewDir * stepLength;
        float3 samplePos =
            cameraPos + viewDir * (segmentStart + 0.5f * stepLength);

        [loop]
        for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            float3 delta = samplePos - data0.xyz;
            float horizontal = saturate(
                1.0f - length(delta.xz) / radius);
            float heightWeight = exp(
                -abs(delta.y) *
                max(data1.x, 0.0001f));
            float sphereWeight = saturate(
                1.0f - length(delta) / radius);
            float volumeWeight = sphereFog
                ? sphereWeight
                : horizontal * heightWeight;

            densityIntegral +=
                volumeWeight *
                max(data1.y, 0.0f) *
                stepLength;

            samplePos += sampleStep;
        }

        float opticalDepth = densityIntegral * 0.08f;
        totalOpticalDepth += opticalDepth;
        opticalDepthColor +=
            LocalFogColors[fogIndex].rgb *
            opticalDepth;
    }

    result.Transmittance = exp(-totalOpticalDepth);
    if (totalOpticalDepth > 0.00001f)
    {
        float3 fogColor =
            opticalDepthColor /
            totalOpticalDepth;
        result.Scattering =
            fogColor *
            (1.0f - result.Transmittance);
    }

    return result;
}

float4 main(PSInputPostProcess input) : SV_Target
{
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
        depth >= 0.9999f;

    float3 viewRay =
        ReconstructPostProcessViewRayCommon(
            input.TexCoord);
    float3 rayEnd =
        background
        ? PPCameraPos.xyz +
          viewRay * 100.0f
        : ReconstructPostProcessWorldPositionCommon(input.TexCoord, depth);

    float3 scatter =
        RayMarchAtmosphereViewFixed(
            rayEnd,
            ShadowMapTexture,
            ShadowSampler,
            LightViewProjection,
            ShadowMapParams);

    LocalFogRayResult localFog =
        IntegrateLocalFogRay(rayEnd);




    float3 participatingMedia =
        scatter * localFog.Transmittance +
        localFog.Scattering;
    return float4(
        participatingMedia,
        localFog.Transmittance);
}
