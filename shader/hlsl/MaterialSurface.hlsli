#ifndef MATERIAL_SURFACE_INCLUDED
#define MATERIAL_SURFACE_INCLUDED
// ラスターシェーダーと GPU 駆動シェーダーで同じテクスチャヒープとパラメーターレイアウトを使う。
#ifndef SHADER_GPU_DRIVEN
Texture2D AssetTextureHeap[512] : register(t0, space1);
#define ASSET_TEXTURE_HEAP AssetTextureHeap
#else
#define ASSET_TEXTURE_HEAP gSceneTextures
#endif
struct AssetMaterialParameters
{
    float4 BaseColor;
    float4 Surface;
    float4 Emission;
    uint4 Textures;
};

AssetMaterialParameters LoadAssetMaterial(uint materialID)
{
    AssetMaterialParameters result;
#ifdef SHADER_GPU_DRIVEN
    result.BaseColor = LoadSceneMaterialVector(gSceneMaterials, materialID, 146u);
    result.Surface = LoadSceneMaterialVector(gSceneMaterials, materialID, 147u);
    result.Emission = LoadSceneMaterialVector(gSceneMaterials, materialID, 148u);
    result.Textures = asuint(LoadSceneMaterialVector(gSceneMaterials, materialID, 149u));
#else
    result.BaseColor = AssetBaseColor;
    result.Surface = AssetSurface;
    result.Emission = AssetEmission;
    result.Textures = AssetTextures;
#endif
    return result;
}

float4 SampleAssetTexture(uint index, float2 uv, SamplerState materialSampler, float4 fallback)
{
    if (index >= 512u)
    {
        return fallback;
    }
#ifdef SHADER_VISIBILITY_RESOLVE
    return ASSET_TEXTURE_HEAP[NonUniformResourceIndex(index)].SampleGrad(materialSampler, uv, VisibilityUvDx,
                                                                         VisibilityUvDy);
#else
    return ASSET_TEXTURE_HEAP[NonUniformResourceIndex(index)].Sample(materialSampler, uv);
#endif
}

void ApplyAssetMaterial(AssetMaterialParameters material, float2 uv, SamplerState materialSampler, inout float4 color,
                        inout float metallic, inout float roughness, out float4 emissionAO)
{
    emissionAO = float4(0, 0, 0, 1);
    if (material.Surface.z < 0.5f)
    {
        return;
    }
    color *= material.BaseColor;
    float2 metallicRoughness = SampleAssetTexture(material.Textures.x, uv, materialSampler, 1).rg;
    metallic *= metallicRoughness.r;
    roughness *= metallicRoughness.g;
    float occlusion = SampleAssetTexture(material.Textures.y, uv, materialSampler, 1).r;
    emissionAO.a = lerp(1.0f, occlusion, material.Surface.y);
    emissionAO.rgb = material.Emission.rgb * material.Emission.a *
                     SampleAssetTexture(material.Textures.z, uv, materialSampler, 1).rgb;
    const uint surface = material.Textures.w & 3u;
    if (surface == 1u)
    {
#ifndef SHADER_VISIBILITY_RESOLVE
        clip(color.a - material.Surface.w);
#endif
        color.a = 1.0f;
    }
    else if (surface == 0u)
    {
        color.a = 1.0f;
    }
}

// ディファードパスの拡張シェーディングモデルを、影の GBuffer の alpha（8 ビット）に格納する。
// bit 5-7 = モデル、bit 0-4 = 強度。CPU 側では Textures.w の bit 4-6 / 8-15 に詰める。
float EncodeShadingModel(AssetMaterialParameters material, float fallback)
{
    if (material.Surface.z < 0.5f)
    {
        return fallback;
    }
    uint model = (material.Textures.w >> 4u) & 7u;
    uint strength = (material.Textures.w >> 8u) & 255u;
    uint packed = (model << 5u) | ((strength * 31u + 127u) / 255u);
    return packed / 255.0f;
}

float3 ScaleAssetNormal(float3 tangentNormal, AssetMaterialParameters material)
{
    if (material.Surface.z > 0.5f)
    {
        tangentNormal.xy *= material.Surface.x;
    }
    return normalize(tangentNormal);
}
#endif
