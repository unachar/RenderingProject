#define SHADER_3D
#include "common.hlsl"

GBufferOutput main(PSInput3D input)
{
    GBufferOutput output;
    output.Color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.Normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.Position = float4(input.WorldPos, 1.0f);
    output.Depth = saturate(input.Position.z / input.Position.w);
    output.Material = 0.0f;
    output.Material.a = 10.0f;
    return output;
}
