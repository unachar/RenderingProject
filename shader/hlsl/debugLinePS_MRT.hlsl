#define SHADER_DEBUG_LINE
#include "common.hlsl"

GBufferOutput main(PSInputDebugLine input)
{
    GBufferOutput output;
    output.Color = input.Color;
    output.Normal = MakeGBufferNormal(float3(0.0f, 1.0f, 0.0f));
    output.Position = float4(input.WorldPos, 1.0f);
    output.Depth = saturate(input.Position.z);
    output.Material = 0.0f;
    output.Material.a = 10.0f;
    output.Shadow = 0.0f;
    output.RimStyle = 0.0f;
    output.RimLight = 0.0f;
    return output;
}
