#ifndef CLOUD_MEDIUM
#define CLOUD_MEDIUM
Texture3D<float> CloudBaseNoise : register(t31);
Texture3D<float> CloudDetailNoise : register(t32);
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
    float4 CloudTrace;       // max distance, noise slice, enabled, shadows enabled
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
    float stepLength = (end - start) / max(CloudQuality.y, 1u);
    float opticalDepth = 0;
    [loop] for (uint i = 0; i < CloudQuality.y; ++i)
    {
        opticalDepth += CloudDensity(position + towardSun * (start + (i + 0.5) * stepLength)) * stepLength;
        if (opticalDepth * CloudShape.w > 9.21) break;
    }
    return exp(-opticalDepth * CloudShape.w);
}

// Average four equal-area samples on the solar disc (angular radius ~0.266 degrees).
// The same incident-light model is used inside clouds and at surface receivers.
float CloudSolarTransmittance(float3 position, float3 towardSun)
{
    float3 tangent = normalize(cross(abs(towardSun.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), towardSun));
    float3 bitangent = cross(towardSun, tangent);
    const float discOffset = 0.00465 * 0.5;
    float transmission = 0;
    [unroll] for (uint sample = 0; sample < 4; ++sample)
    {
        float2 offset = float2((sample & 1) ? 1 : -1, (sample & 2) ? 1 : -1) * discOffset;
        float3 direction = normalize(towardSun + tangent * offset.x + bitangent * offset.y);
        if (direction.y > 0) transmission += CloudLightTransmittance(position, direction);
    }
    return transmission * 0.25;
}

// Index+1 is written by the CPU for the same Sun entity used by the sky.
int CloudSunIndex()
{
    int index = (int)round(SkySunDirection.w) - 1;
    return index >= 0 && index < min((int)LightCount.x, 160) ? index : -1;
}
float CloudSunVisibility(float3 position)
{
    if (CloudTrace.z < 0.5 || CloudTrace.w < 0.5 || CloudSunIndex() < 0) return 1;
    float3 towardSun = SafeNormalizeCommon(SkySunDirection.xyz, float3(0, 1, 0));
    if (towardSun.y <= 0) return 0; // Opaque planet blocks solar rays below the horizon.
    return CloudSolarTransmittance(position, towardSun);
}

#endif
