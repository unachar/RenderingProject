#define SHADER_3D
#define POM_PIXEL_DEPTH_OUTPUT
#include "common.hlsl"

Texture2D TextureBaseColor : register(t0);
Texture2D TextureNormal : register(t2);
SamplerState Sampler : register(s0);

#include "MaterialSurface.hlsli"
#include "NormalMapDecode.hlsli"

PS_OUTPUT_GEOMETRY_DEPTH main(PSInput3D input)
{
    PS_OUTPUT_GEOMETRY_DEPTH output;

#ifdef SHADER_GPU_DRIVEN
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(materialID);
#else
    AssetMaterialParameters assetMaterial = LoadAssetMaterial(0);
#endif
    AssetParallaxResult pom =
        ApplyAssetParallaxEx(assetMaterial, input.TexCoord, input.WorldPos, input.Normal, input.CameraPos, Sampler);
    float2 materialUv = pom.Uv;
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(input.Diffuse.a, 0.0f);
    float4 baseColor = float4(input.Diffuse.rgb, 1.0f);
    if (assetMaterial.Surface.z > 0.5f)
    {
        baseColor = float4(1, 1, 1, 1);
    }
    float3 normal = input.Normal;
    if (UseTexture)
    {
        baseColor *= TextureBaseColor.Sample(Sampler, materialUv);
    }

    float assetMetallic = mMetallic;
    float assetRoughness = mRoughness;
    float4 assetEmissionAO;
    ApplyAssetMaterial(assetMaterial, materialUv, Sampler, baseColor, assetMetallic, assetRoughness,
                       assetEmissionAO);
    output.EmissionAO = assetEmissionAO;
    clip(baseColor.a - 0.01f);

    if (UseNormalMap != 0)
    {
        float3 tangentNormal = DecodeTangentNormalSample(TextureNormal.Sample(Sampler, materialUv));
        normal = ApplyNormalMapCommon(normal, input.WorldPos, input.TexCoord,
                                      ScaleAssetNormal(tangentNormal, assetMaterial));
    }
    else
    {
        // 法線マップが無いときは POM の高さマップから法線を作る。
        normal = ApplyAssetHeightNormal(assetMaterial, normal, input.WorldPos, input.TexCoord, materialUv, Sampler);
    }

    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);

    output.Depth = AssetParallaxPixelDepth(pom, input.WorldPos, input.CameraPos, saturate(input.Position.z));
    output.HardwareDepth = max(output.Depth.x, input.Position.z);
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : assetMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : assetRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow = usePartParams ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
                                  : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);
    output.Shadow.a = EncodeShadingModel(assetMaterial, 0.0f);
    WriteAssetParallaxSelfShadow(pom, input.WorldPos, Sampler, output.Shadow);

    return output;
}
