#define SHADER_2D
#include "common.hlsl"
Texture2D InputTexture : register(t0);
Texture2D NormalTexture : register(t2);
SamplerState InputSampler : register(s0);

float4 main(PSInput2D input) : SV_Target
{
    float4 color = input.Color;
    if (UseTexture != 0)
    {
        color *= InputTexture.Sample(InputSampler, input.TexCoord);
    }
    color.a *= saturate(MaterialAlpha2D);

    float3 normal = input.Normal;
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = NormalTexture.Sample(InputSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        float3 tangent = float3(1.0f, 0.0f, 0.0f);
        float3 bitangent = float3(0.0f, -1.0f, 0.0f);
        normal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal, normal);
    }

    float3 lightDir = normalize(float3(-0.35f, 0.45f, -1.0f));
    float lighting = saturate(dot(normalize(normal), lightDir));
    color.rgb *= lerp(0.35f, 1.0f, lighting);
  
    color.rgb *= color.a;
    return color;
}
