Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> Destination : register(u0);

cbuffer HiZConstants : register(b0)
{
    uint2 DestinationExtent;
    uint SourceMip;
    uint Mode;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= DestinationExtent)) return;
    uint2 source = pixel * 2u;
    uint sourceWidth;
    uint sourceHeight;
    uint mipCount;
    SourceDepth.GetDimensions(SourceMip, sourceWidth, sourceHeight, mipCount);
    uint2 maximum = uint2(max(sourceWidth, 1u) - 1u, max(sourceHeight, 1u) - 1u);
    float depth0 = SourceDepth.Load(int3(min(source, maximum), SourceMip));
    float depth1 = SourceDepth.Load(int3(min(source + uint2(1u, 0u), maximum), SourceMip));
    float depth2 = SourceDepth.Load(int3(min(source + uint2(0u, 1u), maximum), SourceMip));
    float depth3 = SourceDepth.Load(int3(min(source + uint2(1u, 1u), maximum), SourceMip));
    Destination[pixel] = max(max(depth0, depth1), max(depth2, depth3));
}
