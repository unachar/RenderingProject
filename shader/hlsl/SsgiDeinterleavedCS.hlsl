Texture2DArray<float> DeinterleavedDepth : register(t0);
Texture2DArray<float4> DeinterleavedNormal : register(t1);
Texture2DArray<float4> DeinterleavedLighting : register(t2);
StructuredBuffer<uint> RayOrder : register(t3);
RWTexture2DArray<float4> OutIndirect : register(u0);

SamplerState LinearClampSampler : register(s0);

cbuffer ScreenSpaceConstants : register(b0)
{
    float4x4 InvViewProjection;
    uint2 FullExtent;
    uint2 DeinterleavedExtent;
    float4 EffectParams;
    uint4 FeatureFlags;
};

float3 DecodeNormal(float3 value)
{
    return normalize(value * 2.0f - 1.0f);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint linearIndex = RayOrder[dispatchThreadId.x];
    if (linearIndex == 0xffffffffu) return;
    uint2 pixel = uint2(linearIndex % FullExtent.x, linearIndex / FullExtent.x);
    if (any(pixel >= FullExtent)) return;

    uint slice = (pixel.x & 3u) | ((pixel.y & 3u) << 2u);
    uint2 dePixel = pixel >> 2u;
    float centerDepth = DeinterleavedDepth.Load(int4(dePixel, slice, 0));
    float3 normal = DecodeNormal(DeinterleavedNormal.Load(int4(dePixel, slice, 0)).xyz);
    if (centerDepth >= 0.9999f)
    {
        OutIndirect[uint3(dePixel, slice)] = 0.0f;
        return;
    }

    float3 indirect = 0.0f;
    float weightSum = 0.0f;
    float rotation = frac(dot(float2(pixel), float2(0.754877666f, 0.569840296f))) * 6.28318530718f;
    [unroll]
    for (uint directionIndex = 0u; directionIndex < 8u; ++directionIndex)
    {
        float angle = rotation + (float)directionIndex * 0.78539816339f;
        float2 direction = float2(cos(angle), sin(angle));
        [unroll]
        for (uint stepIndex = 1u; stepIndex <= 5u; ++stepIndex)
        {
            int2 samplePixel = int2(dePixel) + int2(round(direction * (float)stepIndex));
            samplePixel = clamp(samplePixel, int2(0, 0), int2(DeinterleavedExtent) - 1);
            float sampleDepth = DeinterleavedDepth.Load(int4(samplePixel, slice, 0));
            float3 sampleNormal = DecodeNormal(DeinterleavedNormal.Load(int4(samplePixel, slice, 0)).xyz);
            float depthDelta = centerDepth - sampleDepth;
            float facing = saturate(dot(normal, normalize(float3(direction, max(depthDelta * 180.0f, 0.05f))))) *
                saturate(dot(sampleNormal, -normalize(float3(direction, min(depthDelta * 180.0f, -0.05f)))));
            float proximity = exp2(-abs(depthDelta) * 320.0f) / (1.0f + (float)stepIndex);
            float weight = facing * proximity;
            indirect += DeinterleavedLighting.Load(int4(samplePixel, slice, 0)).rgb * weight;
            weightSum += weight;
        }
    }
    indirect = weightSum > 1.0e-4f ? indirect / weightSum : 0.0f;
    OutIndirect[uint3(dePixel, slice)] = float4(max(indirect, 0.0f) * EffectParams.z, 1.0f);
}
