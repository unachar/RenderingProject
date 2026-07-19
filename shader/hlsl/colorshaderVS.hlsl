#define SHADER_2D
#include "common.hlsl"

PSInput2D main(float3 position : POSITION, float4 color : COLOR, float2 texCoord : TEXCOORD)
{
    PSInput2D result;
    result.Position = float4(position, 1.0f);


    result.Position.xy *= Scale;


    result.Position.x *= (1.0f / AspectRatio);


    result.Position.xy += Offset;

    result.Normal = float3(0.0f, 0.0f, -1.0f);
    result.Color = color;
    result.TexCoord = texCoord;
    result.WorldPos = float3(result.Position.xy, 0.0f);

    return result;
}
