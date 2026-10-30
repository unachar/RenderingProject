// Native Hi-Z SSR resolve (see
// docs/rendering-technology-ssr-temporal-design.md): bilateral
// upsample of the half-resolution trace (T1) plus hit-point
// reprojected temporal accumulation (T2). The temporal fetch
// projects this frame's hit surface point through the PREVIOUS
// frame's view-projection and samples the previous accumulation
// there, so history follows the reflected surface rather than the
// reflecting pixel; the blend factor grows with roughness.

Texture2D<float4> HalfResult : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> GBufferNormal : register(t2);
Texture2D<float4> HitPoints : register(t3);
Texture2D<float4> PreviousAccumulation : register(t4);
Texture2D<float4> GBufferMaterial : register(t5);
RWTexture2D<float4> FullResult : register(u0);
RWTexture2D<float4> AccumulationOut : register(u1);
SamplerState LinearSampler : register(s0);

cbuffer ResolveConstants : register(b0)
{
    uint2 FullResolution;
    float2 InvFullResolution;
    float DepthNumerator;
    float DepthDenominator;
    uint2 HalfResolution;
    float4x4 PrevViewProjection;
    float HistoryBlend;
    uint HistoryValid;
    float2 ResolvePadding;
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
        AccumulationOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
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
    float4 current = weightSum > 1e-4f ? accumulation / weightSum : nearestTap;

    // T2: hit-point reprojected history blend.
    if (HistoryValid != 0u && current.a > 0.001f)
    {
        const int2 halfPixel = clamp(halfBase, int2(0, 0), int2(HalfResolution) - 1);
        const float4 hitData = HitPoints.Load(int3(halfPixel, 0));
        if (hitData.w > 0.0f)
        {
            const float4 prevClip = mul(float4(hitData.xyz, 1.0f), PrevViewProjection);
            if (prevClip.w > 0.0f)
            {
                const float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
                if (all(prevUv >= 0.0f) && all(prevUv <= 1.0f))
                {
                    const float4 history = PreviousAccumulation.SampleLevel(LinearSampler, prevUv, 0.0f);
                    if (history.a > 0.05f)
                    {
                        const float roughness = GBufferMaterial.Load(int3(pixel, 0)).g;
                        const float blend = clamp(HistoryBlend * (0.35f + 0.65f * roughness), 0.0f, 0.92f);
                        current.rgb = lerp(current.rgb, history.rgb, blend);
                        current.a = max(current.a, history.a);
                    }
                }
            }
        }
    }
    FullResult[pixel] = current;
    AccumulationOut[pixel] = current;
}
