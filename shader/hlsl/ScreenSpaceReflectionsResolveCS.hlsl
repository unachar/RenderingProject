// Native Hi-Z SSR, step T1 (see
// docs/rendering-technology-ssr-temporal-design.md): bilateral
// upsample of the half-resolution trace result into the full
// resolution result the lighting pass consumes. Each full-res
// pixel blends the 2x2 half-res taps around it, weighted by depth
// and normal similarity against the tap's representative full-res
// pixel; rejected taps fall back to the nearest tap so the output
// is always defined.

Texture2D<float4> HalfResult : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> GBufferNormal : register(t2);
RWTexture2D<float4> FullResult : register(u0);

cbuffer ResolveConstants : register(b0)
{
    uint2 FullResolution;
    float2 InvFullResolution;
    float DepthNumerator;
    float DepthDenominator;
    uint2 HalfResolution;
};

float ViewZFromStoredDepth(float storedDepth)
{
    return DepthNumerator / (storedDepth - DepthDenominator);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= FullResolution.x || dispatchThreadId.y >= FullResolution.y)
    {
        return;
    }
    const int2 pixel = int2(dispatchThreadId.xy);
    const float centerDepth = SceneDepth.Load(int3(pixel, 0));
    if (centerDepth <= 1e-7f)
    {
        FullResult[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const float centerViewZ = ViewZFromStoredDepth(centerDepth);
    const float3 centerNormal = normalize(GBufferNormal.Load(int3(pixel, 0)).rgb * 2.0f - 1.0f);
    const int2 halfBase = pixel / 2;
    float4 accumulation = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    float4 nearestTap = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float nearestDistanceSq = 1e30f;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 halfPixel = clamp(halfBase + int2(dx, dy), int2(0, 0), int2(HalfResolution) - 1);
            const int2 repPixel = min(halfPixel * 2, int2(FullResolution) - 1);
            const float tapDepth = SceneDepth.Load(int3(repPixel, 0));
            const float tapViewZ = ViewZFromStoredDepth(tapDepth);
            const float3 tapNormal = normalize(GBufferNormal.Load(int3(repPixel, 0)).rgb * 2.0f - 1.0f);
            const float depthWeight = exp(-abs(centerViewZ - tapViewZ) / (0.02f * max(centerViewZ, 0.1f) + 1e-3f));
            const float normalWeight = pow(max(dot(centerNormal, tapNormal), 0.0f), 8.0f);
            const float weight = depthWeight * normalWeight;
            const float4 tap = HalfResult.Load(int3(halfPixel, 0));
            accumulation += tap * weight;
            weightSum += weight;
            const float2 d = float2(pixel - repPixel);
            const float distanceSq = dot(d, d);
            if (distanceSq < nearestDistanceSq)
            {
                nearestDistanceSq = distanceSq;
                nearestTap = tap;
            }
        }
    }
    FullResult[pixel] = weightSum > 1e-4f ? accumulation / weightSum : nearestTap;
}
