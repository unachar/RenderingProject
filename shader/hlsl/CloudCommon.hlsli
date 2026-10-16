#ifndef CLOUD_COMMON
#define CLOUD_COMMON
#define SHADER_POSTPROCESS
#include "common.hlsl"
#include "CelestialLighting.hlsli"

Texture2D<float> CloudSceneDepth : register(t30);
Texture2D<float4> CloudScatteringTransmittance : register(t33);
Texture2D<float2> CloudDepth : register(t34);
Texture2D<float4> CloudSceneMaterial : register(t35);
#include "CloudMedium.hlsli"

float SceneRayDistance(float2 uv)
{
    uint2 pixel = min(uint2(uv * CloudExtent.xy), uint2(CloudExtent.xy) - 1);
    // Match DeferredLightingPS: sky/environment meshes are background, not occluders.
    if (CloudSceneMaterial.Load(int3(pixel, 0)).a < -0.5) return CloudTrace.x;
    float depth = CloudSceneDepth.Load(int3(pixel, 0));
    if (depth >= 0.999999) return CloudTrace.x;
    return min(length(ReconstructPostProcessWorldPositionCommon(uv, depth) - PPCameraPos.xyz), CloudTrace.x);
}
struct CloudOutput
{
    float4 Color : SV_Target0; // RGB scattering; A transmittance, never opacity.
    float2 Depth : SV_Target1; // scene ray distance; opacity-weighted cloud ray distance.
};
CloudOutput MarchCloud(float2 uv, float2 pixel)
{
    CloudOutput output;
    output.Color = float4(0, 0, 0, 1);
    if (CloudQuality.z > 0) output.Color = float4(0, 0, 0, 0);
    if (CloudQuality.z == 2 || CloudQuality.z == 4) output.Color.rgb = 1;
    float sceneDistance = SceneRayDistance(uv);
    output.Depth = float2(sceneDistance, CloudTrace.x);
    if (CloudQuality.z >= 6)
    {
        float3 noiseUv = float3(uv, CloudTrace.y);
        float noise = CloudQuality.z == 6 ? CloudBaseNoise.SampleLevel(CloudWrap, noiseUv, 0) :
            CloudDetailNoise.SampleLevel(CloudWrap, noiseUv, 0);
        output.Color = float4(noise.xxx, 0);
        return output;
    }
    float3 ray = ReconstructPostProcessViewRayCommon(uv);
    float2 intervals[4];
    uint intervalCount = CloudIntervals(PPCameraPos.xyz, ray, sceneDistance, intervals);
    if (intervalCount == 0)
    {
        return output;
    }
    float2 heightBounds = CloudHeightBounds();
    float3 sunDirection = SafeNormalizeCommon(SkySunDirection.xyz, float3(0, 1, 0));
    float3 sunRadiance = 0;
    int sunIndex = CloudSunIndex();
    if (sunIndex >= 0)
        sunRadiance = max(LightColors[sunIndex].rgb * LightColors[sunIndex].w, 0);
    float3 moonDirection = VirtualMoonDirection();
    float3 moonRadiance = VirtualMoonIrradiance() * CelestialAtmosphereTransmission(heightBounds.x, moonDirection);
    float moonPhase = CloudHG(dot(ray, moonDirection));
    float phase = CloudHG(dot(ray, sunDirection));
    float3 solarBottom = sunRadiance * CelestialAtmosphereTransmission(heightBounds.x, sunDirection);
    float3 solarTop = sunRadiance * CelestialAtmosphereTransmission(heightBounds.y, sunDirection);
    float twilight = smoothstep(-0.12, 0.04, sunDirection.y) * (1 - smoothstep(0.04, 0.30, sunDirection.y));
    float3 ambientColor = lerp(float3(0.006, 0.009, 0.016), max(AtmosphereColor0.rgb, 0) * max(SkyParameters.z, 0) * 0.05, SkyParameters.y);
    ambientColor = lerp(ambientColor, float3(0.12, 0.035, 0.018) * max(SkyParameters.z, 0) * 0.05, twilight);
    float3 ambient = ambientColor * CloudLighting.y;
    // Stable IGN per pixel: no frame randomization without temporal reprojection.
    float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float offset = lerp(0.5, jitter, CloudLighting.w);
    float transmittance = 1;
    float3 scattering = 0, debugLighting = 0;
    float densityIntegral = 0, depthWeight = 0, weightedDepth = 0;
    uint steps = 0;
    float occupiedDistance = 0;
    [loop] for (uint interval = 0; interval < intervalCount; ++interval)
    {
        float intervalLength = intervals[interval].y - intervals[interval].x;
        occupiedDistance += intervalLength;
        float stepLength = intervalLength / CloudQuality.x;
        [loop] for (uint i = 0; i < CloudQuality.x; ++i)
        {
            ++steps;
            float distance = intervals[interval].x + (i + offset) * stepLength;
            float3 position = PPCameraPos.xyz + ray * distance;
            float density = CloudDensity(position);
            if (density < 0.00001 && CloudQuality.z != 3) continue;
            float3 solarIncident = lerp(solarBottom, solarTop,
                saturate((position.y - heightBounds.x) / max(heightBounds.y - heightBounds.x, 0.01)));
            float lightT = any(solarIncident > 0) ? CloudLightTransmittance(position, sunDirection) : 1;
            float moonT = any(moonRadiance > 0) ? CloudSolarTransmittance(position, moonDirection) : 1;
            float powder = 1 + CloudLighting.z * (1 - exp(-2 * density));
            float3 lighting = (solarIncident * phase * lightT + moonRadiance * moonPhase * moonT) * powder + ambient;
            float sampleT = exp(-density * CloudShape.w * stepLength);
            float weight = transmittance * (1 - sampleT);
            scattering += weight * lighting;
            debugLighting += lighting;
            densityIntegral += density * stepLength;
            weightedDepth += distance * weight;
            depthWeight += weight;
            transmittance *= sampleT;
            if (transmittance < 0.01) break;
        }
        if (transmittance < 0.01)
        {
            break;
        }
    }
    output.Color = float4(scattering, transmittance);
    output.Depth.y = depthWeight > 0.000001 ? weightedDepth / depthWeight : CloudTrace.x;
    if (CloudQuality.z == 1) output.Color = float4(saturate(densityIntegral / max(occupiedDistance, 0.0001)).xxx, 0);
    if (CloudQuality.z == 2) output.Color = float4(transmittance.xxx, 0);
    if (CloudQuality.z == 3) output.Color = float4(debugLighting / max((float)steps, 1), 0);
    if (CloudQuality.z == 4) output.Color = float4((output.Depth.y / CloudTrace.x).xxx, 0);
    if (CloudQuality.z == 5) output.Color = float4((float(steps) / (CloudQuality.x * intervalCount)).xxx, 0);
    return output;
}
#endif
