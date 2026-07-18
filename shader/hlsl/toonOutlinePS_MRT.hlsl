#define SHADER_3D
#include "common.hlsl"

GBufferOutput main(PSInput3D input)
{
    GBufferOutput output;
    output.Color = input.Diffuse;
    output.Normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.Depth = saturate(input.Position.z);
    output.Material = 0.0f;
    output.Material.a = 10.0f;
    output.Shadow = 0.0f;
    return output;
}
