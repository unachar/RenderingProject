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
    uint sourceWidth;
    uint sourceHeight;
    uint mipCount;
    SourceDepth.GetDimensions(SourceMip, sourceWidth, sourceHeight, mipCount);
    uint2 sourceExtent = uint2(sourceWidth, sourceHeight);
    uint2 first = pixel * sourceExtent / DestinationExtent;
    uint2 end = min(((pixel + 1u) * sourceExtent + DestinationExtent - 1u) / DestinationExtent, sourceExtent);
    float depth = 0;
    [loop] for (uint y = first.y; y < end.y; ++y)
        [loop] for (uint x = first.x; x < end.x; ++x)
            depth = max(depth, SourceDepth.Load(int3(x, y, SourceMip)));
    Destination[pixel] = depth;
}
