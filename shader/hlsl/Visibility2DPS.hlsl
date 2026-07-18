#define SHADER_2D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"

Texture2D InputTexture : register(t0);
Texture2D NormalTexture : register(t2);
SamplerState InputSampler : register(s0);

uint4 main(PSInput2D input) : SV_Target0
{
    float4 baseColor = input.Color;
    if (UseTexture != 0) baseColor *= InputTexture.Sample(InputSampler, input.TexCoord);
    clip(baseColor.a - 0.01f);
    float3 normal = input.Normal;
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = NormalTexture.Sample(InputSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        normal = SafeNormalizeCommon(
            tangentNormal.x * float3(1.0f, 0.0f, 0.0f) +
            tangentNormal.y * float3(0.0f, -1.0f, 0.0f) +
            tangentNormal.z * normal,
            normal);
    }
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : 10.0f;
    return PackVisibilityPayload(
        baseColor.rgb, normal, 0.0f, 0.0f, 0.5f, 0.04f,
        materialPartId, 0.5f, 0.05f, 1.0f);
}
