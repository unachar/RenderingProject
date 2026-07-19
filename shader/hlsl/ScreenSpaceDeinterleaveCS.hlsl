Texture2D<float> InputDepth : register(t0);
Texture2D<float4> InputNormal : register(t1);
Texture2D<float4> PreviousLighting : register(t2);

RWTexture2DArray<float> OutDepth : register(u0);
RWTexture2DArray<float4> OutNormal : register(u1);
RWTexture2DArray<float4> OutLighting : register(u2);

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
    uint2 deinterleavedPixel = dispatchThreadId.xy;
    uint slice = dispatchThreadId.z;
    if (any(deinterleavedPixel >= DeinterleavedExtent) || slice >= 16u) return;

    uint2 offset = uint2(slice & 3u, slice >> 2u);
    uint2 pixel = deinterleavedPixel * 4u + offset;
    if (any(pixel >= FullExtent))
    {
        OutDepth[uint3(deinterleavedPixel, slice)] = 1.0f;
        OutNormal[uint3(deinterleavedPixel, slice)] = float4(0.5f, 1.0f, 0.5f, 0.0f);
        OutLighting[uint3(deinterleavedPixel, slice)] = 0.0f;
        return;
    }

    OutDepth[uint3(deinterleavedPixel, slice)] = InputDepth.Load(int3(pixel, 0));
    OutNormal[uint3(deinterleavedPixel, slice)] = InputNormal.Load(int3(pixel, 0));
    OutLighting[uint3(deinterleavedPixel, slice)] =
        EffectParams.w > 0.5f ? PreviousLighting.Load(int3(pixel, 0)) : 0.0f;
}
