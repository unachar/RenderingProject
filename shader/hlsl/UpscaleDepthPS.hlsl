#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float> LowResDepth : register(t1);

float main(PSInputPostProcess input) : SV_Depth
{
    uint width;
    uint height;
    LowResDepth.GetDimensions(width, height);

    // Map each full-resolution pixel to its nearest low-resolution depth texel.
    // Load is deliberately used instead of the linear sampler: interpolating
    // depth across silhouettes can incorrectly occlude transparent fragments.
    uint2 pixel = min(uint2(input.TexCoord * float2(width, height)),
        uint2(width - 1, height - 1));
    return LowResDepth.Load(int3(pixel, 0));
}
