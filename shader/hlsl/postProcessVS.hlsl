// postProcessVS.hlsl
#define SHADER_POSTPROCESS
#include "common.hlsl"

PSInputPostProcess main(uint vertexId : SV_VertexID)
{
    PSInputPostProcess output;
    

    output.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.TexCoord * float2(2, -2) + float2(-1, 1), 0, 1);
    
    return output;
}
