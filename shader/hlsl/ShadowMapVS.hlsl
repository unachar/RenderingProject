#define SHADER_3D
#include "common.hlsl"

struct ShadowVertexOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD;
    float Opacity : COLOR;
    float3 WorldPosition : TEXCOORD1;
    float3 NormalWS : NORMAL;
};

ShadowVertexOutput main(VSInput3D input)
{
    ShadowVertexOutput output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPos, LightViewProjection);
    output.TexCoord = input.TexCoord;
    output.Opacity = input.Diffuse.a;
    output.WorldPosition = worldPos.xyz;
    float3x3 linearWorld = (float3x3)World;
    float3x3 normalMatrix = float3x3(cross(linearWorld[1], linearWorld[2]), cross(linearWorld[2], linearWorld[0]),
                                     cross(linearWorld[0], linearWorld[1]));
    float determinant = dot(linearWorld[0], normalMatrix[0]);
    output.NormalWS = abs(determinant) > 0.000001f ? normalize(mul(input.Normal, normalMatrix) / determinant)
                                                   : normalize(input.Normal);
    return output;
}
