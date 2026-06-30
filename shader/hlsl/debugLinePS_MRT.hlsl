#define SHADER_DEBUG_LINE
#include "common.hlsl"

GBufferOutput main(PSInputDebugLine input)
{
    GBufferOutput output;
    output.Color = input.Color;
    output.Normal = MakeGBufferNormal(float3(0.0f, 1.0f, 0.0f));
    output.Position = float4(input.WorldPos, 1.0f);
    output.Depth = input.Position.z / input.Position.w;
    output.Material = 0.0f;
    output.Material.a = 10.0f;
    return output;
}
