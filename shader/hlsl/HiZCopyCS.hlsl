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
    Destination[pixel] = SourceDepth.Load(int3(pixel, 0));
}
