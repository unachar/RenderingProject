#define SHADER_3D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

uint4 PSMain(PSInput3D input) : SV_Target0
{
    float materialPartId = MaterialMode == 1 ? (float)ShaderClass : max(input.Diffuse.a, 0.0f);
    float4 baseColor = float4(input.Diffuse.rgb, 1.0f);
    float3 normal = input.Normal * (FlipNormal != 0 ? -1.0f : 1.0f);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, input.TexCoord).xyz * 2.0f - 1.0f;
        float3 tangent = normalize(mul(float3(1.0f, 0.0f, 0.0f), (float3x3)World));
        float3 bitangent = normalize(mul(float3(0.0f, -1.0f, 0.0f), (float3x3)World));
        normal = normalize(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal);
    }
    if (UseTexture != 0) baseColor *= g_Texture.Sample(g_SamplerState, input.TexCoord);
    clip(baseColor.a - 0.01f);
    MaterialPartShaderParams part = ResolveMaterialPartParams(materialPartId);
    bool usePart = MaterialMode != 1;
    return PackVisibilityPayload(
        baseColor.rgb, normal, saturate(input.Position.z),
        usePart ? part.Basic.x : mMetallic,
        usePart ? part.Basic.y : mRoughness,
        usePart ? part.Basic.z : mFresnel,
        materialPartId,
        usePart ? part.Shadow0.x : ShadowThreshold,
        usePart ? part.Shadow0.y : ShadowSoftness,
        usePart ? part.Shadow0.z : ShadowStrength);
}
