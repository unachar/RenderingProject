Texture2DArray<float4> DeinterleavedIndirect : register(t0);
RWTexture2D<float4> OutIndirect : register(u0);

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
    uint slice = (pixel.x & 3u) | ((pixel.y & 3u) << 2u);
    uint2 dePixel = pixel >> 2u;
    float3 color = DeinterleavedIndirect.Load(int4(dePixel, slice, 0)).rgb;
    OutIndirect[pixel] = float4(color, 1.0f);
}
