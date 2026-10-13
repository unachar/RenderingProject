Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer HiZConstants : register(b0) { uint2 DestinationExtent; uint SourceMip; uint Mode; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DestinationExtent)) return;
    uint width, height, mips; SourceDepth.GetDimensions(0, width, height, mips);
    uint2 pixel = min(id.xy * uint2(width, height) / DestinationExtent, uint2(width - 1, height - 1));
    float depth = SourceDepth.Load(int3(pixel, 0));
    float shade = pow(saturate(1 - depth), 0.25f);
    Destination[id.xy] = float4(shade.xxx, 1);
}
