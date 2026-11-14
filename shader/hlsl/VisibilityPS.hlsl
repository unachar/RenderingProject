#define SHADER_3D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"
#include "NormalMapDecode.hlsli"
#include "ToonLighting.hlsli"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

uint4 main(PSInput3D input) : SV_Target0
{
    float materialPartId = MaterialMode == 1 ? ResolveLiveToonMaterialClass(input.Diffuse.a, (float)ShaderClass) : max(input.Diffuse.a, 0.0f);
    // SNI ヘッドマスク: materialPartId + 100.0f でエンコードされている
    float headMask = step(100.0f, materialPartId);
    materialPartId = materialPartId - headMask * 100.0f;
    float4 baseColor = float4(input.Diffuse.rgb, 1.0f);
    float3 normal = input.Normal.xyz * ((FlipNormal != 0) ? -1.0f : 1.0f);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = DecodeTangentNormalSample(g_NormalTexture.Sample(g_SamplerState, input.TexCoord));
        float3 tangent = SafeNormalizeCommon(mul(float3(1.0f, 0.0f, 0.0f), (float3x3)World), float3(1.0f, 0.0f, 0.0f));
        float3 bitangent =
            SafeNormalizeCommon(mul(float3(0.0f, -1.0f, 0.0f), (float3x3)World), float3(0.0f, 0.0f, 1.0f));
        normal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal,
                                     normal);
    }
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, input.TexCoord);
    }
    clip(baseColor.a - 0.01f);

    // 球面法線補間 (SNI): ヘッドマスク (materialPartId + 100 でエンコード) を使用
    float3 sniNormal = SafeNormalizeCommon(input.SniVector, normal);
    normal = lerp(normal, sniNormal, headMask);

    MaterialPartShaderParams part = ResolveMaterialPartParams(materialPartId);
    bool usePart = MaterialMode != 1;
    return PackVisibilityPayload(baseColor.rgb, normal, saturate(input.Position.z), usePart ? part.Basic.x : mMetallic,
                                 usePart ? part.Basic.y : mRoughness, usePart ? part.Basic.z : mFresnel, materialPartId,
                                 usePart ? part.Shadow0.x : ShadowThreshold, usePart ? part.Shadow0.y : ShadowSoftness,
                                 usePart ? part.Shadow0.z : ShadowStrength);
}
