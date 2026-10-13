#ifndef CLOUD_COMMON
#define CLOUD_COMMON
#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float> CloudSceneDepth : register(t30);
Texture3D<float> CloudBaseNoise : register(t31);
Texture3D<float> CloudDetailNoise : register(t32);
Texture2D<float4> CloudScatteringTransmittance : register(t33);
Texture2D<float2> CloudDepth : register(t34);
Texture2D<float4> CloudSceneMaterial : register(t35);
SamplerState CloudWrap : register(s3);

// Explicit float4/uint4 groups match CloudRenderer::Constants and 16-byte packing.
cbuffer CloudParameters : register(b9)
{
    float4 CloudLayer;       // bottom, top, coverage, density multiplier
    float4 CloudShape;       // base scale, detail scale, erosion, extinction
    float4 CloudLighting;    // phase g, ambient strength, powder, jitter
    float4 CloudWind;        // wrapped base UV offset (xy), detail UV offset (zw)
    uint4 CloudQuality;     // view steps, light steps, debug mode, seed
    float4 CloudExtent;      // full width/height, cloud width/height
    float4 CloudTrace;       // max distance, noise slice, unused
};

bool CloudInterval(float3 origin, float3 direction, float limit, out float start, out float end)
{
    start = 0; end = limit;
    if (abs(direction.y) < 0.000001)
        return origin.y >= CloudLayer.x && origin.y <= CloudLayer.y && limit > 0;
    float2 slab = (CloudLayer.xy - origin.y) / direction.y;
    start = max(0, min(slab.x, slab.y));
    end = min(limit, max(slab.x, slab.y));
    return end > start;
}
float CloudDensity(float3 worldPosition)
{
    if (CloudLayer.z <= 0 || CloudLayer.w <= 0) return 0;
    float height = (worldPosition.y - CloudLayer.x) / max(CloudLayer.y - CloudLayer.x, 0.01);
    if (height <= 0 || height >= 1) return 0;
    float gradient = smoothstep(0, 0.15, height) * (1 - smoothstep(0.7, 1, height));
    float3 baseUv = worldPosition * CloudShape.x;
    baseUv.xz -= CloudWind.xy;
    float baseNoise = CloudBaseNoise.SampleLevel(CloudWrap, baseUv, 0);
    // The noise distribution is narrower than [0,1]; remap before the coverage threshold.
    baseNoise = saturate((baseNoise - 0.2) / 0.6);
    float shape = saturate((baseNoise - (1 - CloudLayer.z)) / max(CloudLayer.z, 0.00001));
    if (shape * gradient < 0.00001) return 0;
    float3 detailUv = worldPosition * CloudShape.y;
    detailUv.xz -= CloudWind.zw;
    float detail = CloudDetailNoise.SampleLevel(CloudWrap, detailUv, 0);
    return saturate(shape * gradient - detail * CloudShape.z * (1 - shape)) * CloudLayer.w;
}
float CloudHG(float cosine)
{
    float g = CloudLighting.x;
    return (1 - g * g) / (12.566370614 * pow(max(1 + g * g - 2 * g * cosine, 0.001), 1.5));
}
float CloudLightTransmittance(float3 position, float3 towardSun)
{
    float start, end;
    if (!CloudInterval(position, towardSun, CloudTrace.x, start, end)) return 1;
    float stepLength = end / CloudQuality.y;
    float opticalDepth = 0;
    [loop] for (uint i = 0; i < CloudQuality.y; ++i)
    {
        opticalDepth += CloudDensity(position + towardSun * ((i + 0.5) * stepLength)) * stepLength;
        if (opticalDepth * CloudShape.w > 9.21) break;
    }
    return exp(-opticalDepth * CloudShape.w);
}
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
    float start, end;
    if (!CloudInterval(PPCameraPos.xyz, ray, sceneDistance, start, end)) return output;
    float3 sunDirection = float3(0, 1, 0);
    float3 sunRadiance = 0;
    [loop] for (uint light = 0; light < min((uint)LightCount.x, 160u); ++light)
    {
        if (LightPositionTypes[light].w < 0.5 && LightFlags[light].w < 0.5)
        {
            sunDirection = SafeNormalizeCommon(LightDirections[light].xyz, float3(0, 1, 0));
            sunRadiance = max(LightColors[light].rgb * LightColors[light].w, 0);
            break;
        }
    }
    float phase = CloudHG(dot(ray, sunDirection));
    float3 ambient = max(AtmosphereColor0.rgb, 0) * CloudLighting.y * max(SkyParameters.z * 0.05, 0.02);
    float stepLength = (end - start) / CloudQuality.x;
    // Stable IGN per pixel: no frame randomization without temporal reprojection.
    float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float offset = lerp(0.5, jitter, CloudLighting.w);
    float transmittance = 1;
    float3 scattering = 0, debugLighting = 0;
    float densityIntegral = 0, depthWeight = 0, weightedDepth = 0;
    uint steps = 0;
    [loop] for (uint i = 0; i < CloudQuality.x; ++i)
    {
        ++steps;
        float distance = start + (i + offset) * stepLength;
        float3 position = PPCameraPos.xyz + ray * distance;
        float density = CloudDensity(position);
        if (density < 0.00001 && CloudQuality.z != 3) continue;
        float lightT = any(sunRadiance > 0) ? CloudLightTransmittance(position, sunDirection) : 1;
        float powder = 1 + CloudLighting.z * (1 - exp(-2 * density));
        float3 lighting = sunRadiance * phase * lightT * powder + ambient;
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
    output.Color = float4(scattering, transmittance);
    output.Depth.y = depthWeight > 0.000001 ? weightedDepth / depthWeight : CloudTrace.x;
    if (CloudQuality.z == 1) output.Color = float4(saturate(densityIntegral / max(end - start, 0.0001)).xxx, 0);
    if (CloudQuality.z == 2) output.Color = float4(transmittance.xxx, 0);
    if (CloudQuality.z == 3) output.Color = float4(debugLighting / max((float)steps, 1), 0);
    if (CloudQuality.z == 4) output.Color = float4((output.Depth.y / CloudTrace.x).xxx, 0);
    if (CloudQuality.z == 5) output.Color = float4((float(steps) / CloudQuality.x).xxx, 0);
    return output;
}
#endif
