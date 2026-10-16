#ifndef CLOUD_MEDIUM
#define CLOUD_MEDIUM
Texture3D<float> CloudBaseNoise : register(t31);
Texture3D<float> CloudDetailNoise : register(t32);
SamplerState CloudWrap : register(s3);

struct CloudLayerParameters
{
    float4 Layer;
    float4 Shape;
    float4 Wind;
};

// Explicit float4/uint4 groups match CloudRenderer::Constants and 16-byte packing.
cbuffer CloudParameters : register(b9)
{
    float4 CloudLayer;       // bottom, top, coverage, density multiplier
    float4 CloudShape;       // base scale, detail scale, erosion, extinction
    float4 CloudLighting;    // phase g, ambient strength, powder, jitter
    float4 CloudWind;        // wrapped base UV offset (xy), detail UV offset (zw)
    uint4 CloudQuality;     // view steps, light steps, debug mode, layer count
    float4 CloudExtent;      // full width/height, cloud width/height
    float4 CloudTrace;       // max distance, noise slice, enabled, shadows enabled
    CloudLayerParameters CloudAdditionalLayers[3];
};

CloudLayerParameters GetCloudLayer(uint index)
{
    CloudLayerParameters layer;
    if (index == 0)
    {
        layer.Layer = CloudLayer;
        layer.Shape = CloudShape;
        layer.Wind = CloudWind;
    }
    else
    {
        layer = CloudAdditionalLayers[index - 1];
    }
    return layer;
}

float2 CloudHeightBounds()
{
    [branch] if (CloudQuality.w <= 1)
    {
        return CloudLayer.xy;
    }
    float2 bounds = float2(20000, -10000);
    [loop] for (uint index = 0; index < CloudQuality.w; ++index)
    {
        float4 layer = GetCloudLayer(index).Layer;
        if (layer.z > 0 && layer.w > 0)
        {
            bounds.x = min(bounds.x, layer.x);
            bounds.y = max(bounds.y, layer.y);
        }
    }
    return bounds;
}

void SortCloudIntervalPair(inout float2 nearInterval, inout float2 farInterval)
{
    if (nearInterval.x > farInterval.x)
    {
        float2 previousNear = nearInterval;
        nearInterval = farInterval;
        farInterval = previousNear;
    }
}

// Sorted, merged occupied intervals: skip clear gaps and integrate overlaps only once.
uint CloudIntervals(float3 origin, float3 direction, float limit, out float2 intervals[4])
{
    uint count = 0;
    [unroll] for (uint clearIndex = 0; clearIndex < 4; ++clearIndex)
    {
        // Empty intervals sort after every occupied interval (whose start is < limit).
        intervals[clearIndex] = float2(limit, limit);
    }
    [loop] for (uint index = 0; index < CloudQuality.w; ++index)
    {
        float4 layer = GetCloudLayer(index).Layer;
        if (layer.z <= 0 || layer.w <= 0)
        {
            continue;
        }
        float start = 0;
        float end = limit;
        if (abs(direction.y) < 0.000001)
        {
            if (origin.y < layer.x || origin.y > layer.y)
            {
                continue;
            }
        }
        else
        {
            float2 slab = (layer.xy - origin.y) / direction.y;
            start = max(0, min(slab.x, slab.y));
            end = min(limit, max(slab.x, slab.y));
        }
        if (end > start)
        {
            intervals[index] = float2(start, end);
            ++count;
        }
    }
    [branch] if (CloudQuality.w <= 1)
    {
        return count;
    }
    // A fixed four-input sorting network avoids dynamically indexed insertion loops in FXC.
    SortCloudIntervalPair(intervals[0], intervals[1]);
    SortCloudIntervalPair(intervals[2], intervals[3]);
    SortCloudIntervalPair(intervals[0], intervals[2]);
    SortCloudIntervalPair(intervals[1], intervals[3]);
    SortCloudIntervalPair(intervals[1], intervals[2]);
    uint mergedCount = 0;
    [loop] for (uint interval = 0; interval < count; ++interval)
    {
        float2 current = intervals[interval];
        if (mergedCount > 0)
        {
            if (current.x <= intervals[mergedCount - 1].y)
            {
                intervals[mergedCount - 1].y = max(intervals[mergedCount - 1].y, current.y);
                continue;
            }
        }
        intervals[mergedCount] = current;
        ++mergedCount;
    }
    return mergedCount;
}

float CloudLayerDensity(float3 worldPosition, CloudLayerParameters layer, uint index)
{
    if (layer.Layer.z <= 0 || layer.Layer.w <= 0)
    {
        return 0;
    }
    float height = (worldPosition.y - layer.Layer.x) / max(layer.Layer.y - layer.Layer.x, 0.01);
    if (height <= 0 || height >= 1)
    {
        return 0;
    }
    float gradient = smoothstep(0, 0.15, height) * (1 - smoothstep(0.7, 1, height));
    // Fixed per-slot offsets decorrelate layers without generating more noise volumes.
    float3 noiseOffset = float3(0.173, 0.367, 0.619) * index;
    float3 baseUv = worldPosition * layer.Shape.x + noiseOffset;
    baseUv.xz -= layer.Wind.xy;
    float baseNoise = CloudBaseNoise.SampleLevel(CloudWrap, baseUv, 0);
    baseNoise = saturate((baseNoise - 0.2) / 0.6);
    float shape = saturate((baseNoise - (1 - layer.Layer.z)) / max(layer.Layer.z, 0.00001));
    if (shape * gradient < 0.00001)
    {
        return 0;
    }
    float3 detailUv = worldPosition * layer.Shape.y + noiseOffset;
    detailUv.xz -= layer.Wind.zw;
    float detail = CloudDetailNoise.SampleLevel(CloudWrap, detailUv, 0);
    return saturate(shape * gradient - detail * layer.Shape.z * (1 - shape)) * layer.Layer.w;
}

float CloudDensity(float3 worldPosition)
{
    float density = CloudLayerDensity(worldPosition, GetCloudLayer(0), 0);
    [branch] if (CloudQuality.w <= 1)
    {
        return density;
    }
    [loop] for (uint index = 1; index < CloudQuality.w; ++index)
    {
        density += CloudLayerDensity(worldPosition, GetCloudLayer(index), index);
    }
    return density;
}
float CloudHG(float cosine)
{
    float g = CloudLighting.x;
    return (1 - g * g) / (12.566370614 * pow(max(1 + g * g - 2 * g * cosine, 0.001), 1.5));
}
float CloudLightTransmittance(float3 position, float3 towardSun)
{
    float2 intervals[4];
    uint count = CloudIntervals(position, towardSun, CloudTrace.x, intervals);
    float opticalDepth = 0;
    [loop] for (uint interval = 0; interval < count; ++interval)
    {
        float stepLength = (intervals[interval].y - intervals[interval].x) / max(CloudQuality.y, 1u);
        [loop] for (uint sample = 0; sample < CloudQuality.y; ++sample)
        {
            float distance = intervals[interval].x + (sample + 0.5) * stepLength;
            opticalDepth += CloudDensity(position + towardSun * distance) * stepLength;
            if (opticalDepth * CloudShape.w > 9.21)
            {
                return 0;
            }
        }
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

