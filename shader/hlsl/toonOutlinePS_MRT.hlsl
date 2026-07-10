#define SHADER_3D
#include "common.hlsl"

GBufferOutput main(PSInput3D input)
{
    GBufferOutput output;
    output.Color = input.Diffuse;
    output.Normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.Position = float4(input.WorldPos, 1.0f);
    output.Depth = saturate(input.Position.z / input.Position.w);
    output.Material = 0.0f;
    output.Material.a = (ShaderClass == 99) ? 99.0f : 10.0f;
    output.Shadow = 0.0f;
    output.RimStyle = 0.0f;
    output.RimLight = 0.0f;
    return output;
}
