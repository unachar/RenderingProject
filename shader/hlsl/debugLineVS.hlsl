#define SHADER_DEBUG_LINE
#include "common.hlsl"

PSInputDebugLine main(VSInputDebugLine input)
{
    PSInputDebugLine output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos  = mul(worldPos, View);
    output.Position = mul(viewPos, Projection);
    output.Color    = input.Color;
    output.WorldPos = worldPos.xyz;
    return output;
}
