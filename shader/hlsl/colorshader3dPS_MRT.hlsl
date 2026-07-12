#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

GBufferOutput main(in PSInput3D In)
{
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : 10.0f;
    float4 baseColor = In.Diffuse;
    float3 normal = -In.Normal.xyz;
    float3 worldPos = In.WorldPos;
    float3 position = In.Position.xyz / In.Position.w;
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        float3 tangent = SafeNormalizeCommon(mul(float3(1.0f, 0.0f, 0.0f), (float3x3) World), float3(1.0f, 0.0f, 0.0f));
        float3 bitangent = SafeNormalizeCommon(mul(float3(0.0f, -1.0f, 0.0f), (float3x3) World), float3(0.0f, 0.0f, 1.0f));
        normal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal, normal);
    }
    
    if (UseTexture)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }

    clip(baseColor.a - 0.01f);

    GBufferOutput output;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);
    output.Position = float4(worldPos, 1.0f);
    output.Depth = saturate(In.Position.z);
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : mMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : mRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow = usePartParams
        ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
        : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);
    output.RimStyle = usePartParams
        ? float4(partParams.Highlight.x, partParams.Highlight.y, partParams.RimStyle.x, partParams.RimStyle.y)
        : float4(RimStrength, RimThreshold, RimSoftness, RimPower);
    output.RimLight = usePartParams
        ? float4(partParams.RimLight.rgb, partParams.RimStyle.z)
        : float4(RimColor, RimAlbedoBlend);

    return output;
}
