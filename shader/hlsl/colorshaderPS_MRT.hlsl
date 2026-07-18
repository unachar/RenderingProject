#define SHADER_2D
#include "common.hlsl"

Texture2D InputTexture : register(t0);
Texture2D NormalTexture : register(t2);
SamplerState InputSampler : register(s0);

GBufferOutput main(PSInput2D input)
{
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : 10.0f;
    float4 baseColor = input.Color;
    if (UseTexture)
    {
        baseColor *= InputTexture.Sample(InputSampler, input.TexCoord);
    }

    clip(baseColor.a - 0.01f);
    float3 normal = input.Normal;
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = NormalTexture.Sample(InputSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        float3 tangent = float3(1.0f, 0.0f, 0.0f);
        float3 bitangent = float3(0.0f, -1.0f, 0.0f);
        normal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal, normal);
    }

    GBufferOutput output;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);
    output.Depth = 0.0f;
    output.Material = 0.0f;
    output.Material.a = materialPartId;
    output.Shadow = 0.0f;
    return output;
}
