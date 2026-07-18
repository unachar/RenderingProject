Texture2D<float> InputDepth : register(t0);
Texture2D<float4> InputNormal : register(t1);
RWTexture2D<float> OutOcclusion : register(u0);

cbuffer ScreenSpaceConstants : register(b0)
{
    float4x4 InvViewProjection;
    uint2 FullExtent;
    uint2 DeinterleavedExtent;
    float4 EffectParams;
    uint4 FeatureFlags;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= FullExtent)) return;
    float centerDepth = InputDepth.Load(int3(pixel, 0));
    if (centerDepth >= 0.9999f)
    {
        OutOcclusion[pixel] = 1.0f;
        return;
    }

    float3 normal = normalize(InputNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    static const int2 directions[8] =
    {
        int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1),
        int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1)
    };
    uint occlusionMask = 0u;
    float accumulatedOcclusion = 0.0f;
    uint rotation = ((pixel.x ^ pixel.y) + (pixel.x >> 3u)) & 7u;
    int stepPixels = max((int)round(EffectParams.x * 2.0f), 1);
    [unroll]
    for (uint directionIndex = 0u; directionIndex < 8u; ++directionIndex)
    {
        int2 direction = directions[(directionIndex + rotation) & 7u];
        float2 directionNormal = normalize((float2)direction);
        float directionalOcclusion = 0.0f;
        [unroll]
        for (uint stepIndex = 1u; stepIndex <= 3u; ++stepIndex)
        {
            int2 samplePixel = int2(pixel) + direction * (int)stepIndex * stepPixels;
            samplePixel = clamp(samplePixel, int2(0, 0), int2(FullExtent) - 1);
            float sampleDepth = InputDepth.Load(int3(samplePixel, 0));
            float depthDelta = centerDepth - sampleDepth;
            float rangeWeight = saturate(1.0f - abs(depthDelta) * 96.0f);
            float horizon = saturate((depthDelta - 0.00015f) * 640.0f) * rangeWeight;
            float normalWeight = 0.4f + 0.6f * saturate(1.0f - dot(normal.xy, directionNormal));
            directionalOcclusion = max(directionalOcclusion, horizon * normalWeight);
        }
        if (directionalOcclusion > 0.08f) occlusionMask |= 1u << directionIndex;
        accumulatedOcclusion += directionalOcclusion;
    }

    float coveredSectors = (float)countbits(occlusionMask) / 8.0f;
    float horizonStrength = accumulatedOcclusion / 8.0f;
    float visibility = 1.0f - saturate(coveredSectors * 0.55f + horizonStrength * 0.45f);
    OutOcclusion[pixel] = pow(saturate(visibility), EffectParams.y);
}
