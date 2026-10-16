#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

#include "MaterialSurface.hlsli"

GBufferOutput main(in PSInput3D In)
{
#ifdef SHADER_GPU_DRIVEN
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(materialID);
#else
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(0);
#endif
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (assetMaterial.Surface.z > 0.5f)
    {
        baseColor = float4(1, 1, 1, 1);
    }
    if (UseTexture)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }

    float assetMetallic = mMetallic;
    float assetRoughness = mRoughness;
    float4 assetEmissionAO;
    ApplyAssetMaterial(assetMaterial, In.TexCoord, g_SamplerState, baseColor, assetMetallic, assetRoughness, assetEmissionAO);
    clip(baseColor.a - 0.01f);

    float3 normal = In.Normal;
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        normal = ApplyNormalMapCommon(normal, In.WorldPos, In.TexCoord, ScaleAssetNormal(tangentNormal, assetMaterial));
    }

    GBufferOutput output;
    output.EmissionAO = assetEmissionAO;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);

    output.Depth = saturate(In.Position.z);
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : assetMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : assetRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow = usePartParams
        ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
        : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);
    output.Shadow.a = EncodeShadingModel(assetMaterial, 0.0f);
    return output;
}


