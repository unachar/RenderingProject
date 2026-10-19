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
    float4 Parallax; // UV depth, maximum steps, heap-relative height texture index, reserved.
};

AssetMaterialParameters LoadAssetMaterial(uint materialID)
{
    AssetMaterialParameters result;
#ifdef SHADER_GPU_DRIVEN
    result.BaseColor = LoadSceneMaterialVector(gSceneMaterials, materialID, 146u);
    result.Surface = LoadSceneMaterialVector(gSceneMaterials, materialID, 147u);
    result.Emission = LoadSceneMaterialVector(gSceneMaterials, materialID, 148u);
    result.Textures = asuint(LoadSceneMaterialVector(gSceneMaterials, materialID, 149u));
    result.Parallax = LoadSceneMaterialVector(gSceneMaterials, materialID, 150u);
#else
    result.BaseColor = AssetBaseColor;
    result.Surface = AssetSurface;
    result.Emission = AssetEmission;
    result.Textures = AssetTextures;
    result.Parallax = AssetParallax;
#endif
    return result;
}

float SampleParallaxDepth(uint index, float2 uv, float2 uvDx, float2 uvDy, SamplerState materialSampler)
{
    // Height maps use white for the original surface and black for the deepest recess.
    return 1.0f - saturate(ASSET_TEXTURE_HEAP[NonUniformResourceIndex(index)]
                              .SampleGrad(materialSampler, uv, uvDx, uvDy).r);
}

float2 ApplyAssetParallax(AssetMaterialParameters material, float2 uv, float3 worldPos, float3 meshNormal,
                         float3 cameraPos, SamplerState materialSampler)
{
#ifdef SHADER_VISIBILITY_RESOLVE
    float2 uvDx = VisibilityUvDx;
    float2 uvDy = VisibilityUvDy;
    float3 worldDx = VisibilityWorldDx;
    float3 worldDy = VisibilityWorldDy;
#else
    float2 uvDx = ddx(uv);
    float2 uvDy = ddy(uv);
    float3 worldDx = ddx(worldPos);
    float3 worldDy = ddy(worldPos);
#endif
    // Mask coverage and shadow passes use the original UVs; keep those materials aligned.
    if (material.Surface.z < 0.5f || (material.Textures.w & 3u) != 0u ||
        material.Parallax.x <= 0.0f || material.Parallax.z < 0.0f || material.Parallax.z >= 512.0f)
    {
        return uv;
    }
    float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) < 1e-18f)
    {
        return uv;
    }
    float3 normal = SafeNormalizeCommon(meshNormal, float3(0, 1, 0));
    float3 tangent = (worldDx * uvDy.y - worldDy * uvDx.y) / determinant;
    tangent -= normal * dot(normal, tangent);
    float3 uvBitangent = (worldDy * uvDx.x - worldDx * uvDy.x) / determinant;
    if (dot(tangent, tangent) < 1e-12f || dot(uvBitangent, uvBitangent) < 1e-12f)
    {
        return uv;
    }
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(normal, tangent));
    if (dot(bitangent, uvBitangent) < 0.0f)
    {
        bitangent = -bitangent;
    }
    float3 viewDirection = SafeNormalizeCommon(cameraPos - worldPos, normal);
    float facing = dot(normal, viewDirection);
    if (facing <= 0.0f)
    {
        return uv;
    }
    // Fade before the horizon to prevent an unbounded offset and unstable texture traversal.
    float depthScale = clamp(material.Parallax.x, 0.0f, 0.2f) * smoothstep(0.02f, 0.15f, facing);
    if (depthScale <= 0.0f)
    {
        return uv;
    }
    float2 offset = float2(dot(viewDirection, tangent), dot(viewDirection, bitangent)) *
                    (depthScale / max(facing, 0.1f));
    uint maxSteps = (uint)clamp(round(material.Parallax.y), 8.0f, 128.0f);
    uint steps = (uint)ceil(lerp((float)maxSteps, 8.0f, saturate(facing)));
    float layerStep = rcp((float)steps);
    float2 uvStep = offset * layerStep;
    uint heightIndex = (uint)material.Parallax.z;
    float2 currentUv = uv;
    float layerDepth = 0.0f;
    float sampledDepth = SampleParallaxDepth(heightIndex, currentUv, uvDx, uvDy, materialSampler);
    float previousDifference = sampledDepth;
    [loop] for (uint step = 0u; step < steps && layerDepth < sampledDepth; ++step)
    {
        previousDifference = sampledDepth - layerDepth;
        currentUv -= uvStep;
        layerDepth += layerStep;
        sampledDepth = SampleParallaxDepth(heightIndex, currentUv, uvDx, uvDy, materialSampler);
    }
    float difference = sampledDepth - layerDepth;
    float interval = previousDifference - difference;
    float previousWeight = interval > 1e-6f ? saturate(-difference / interval) : 0.0f;
    return currentUv + uvStep * previousWeight;
}

bool AssetParallaxEnabled(AssetMaterialParameters material)
{
    return material.Surface.z > 0.5f && (material.Textures.w & 3u) == 0u && material.Parallax.x > 0.0f &&
           material.Parallax.z >= 0.0f && material.Parallax.z < 512.0f;
}

// POM は UV をずらすだけなので、ベースカラー／法線マップが無いと見た目が一切変わらない。
// 法線マップ未設定時は高さマップの勾配から接空間法線を作り、凹凸をライティングに反映する。
// 高さ h（白=1）を UV 単位の深さ D でスケールすると、ワールド勾配は D * dh/du となり
// メッシュのスケールに依存しない（ParallaxDepth と見た目の深さが一致する）。
float3 ApplyAssetHeightNormal(AssetMaterialParameters material, float3 normal, float3 worldPos, float2 texCoord,
                              float2 materialUv, SamplerState materialSampler)
{
#ifdef SHADER_VISIBILITY_RESOLVE
    float2 uvDx = VisibilityUvDx;
    float2 uvDy = VisibilityUvDy;
#else
    float2 uvDx = ddx(texCoord);
    float2 uvDy = ddy(texCoord);
#endif
    if (!AssetParallaxEnabled(material))
    {
        return normal;
    }
    uint heightIndex = (uint)material.Parallax.z;
    uint width, height, levels;
    ASSET_TEXTURE_HEAP[NonUniformResourceIndex(heightIndex)].GetDimensions(0u, width, height, levels);
    float2 texel = 1.0f / float2(max(width, 1u), max(height, 1u));
    // 差分幅は 1 テクセルと画素フットプリントの大きい方。遠景での法線エイリアス（ちらつき）を抑える。
    float2 footprint = float2(max(abs(uvDx.x), abs(uvDy.x)), max(abs(uvDx.y), abs(uvDy.y)));
    float2 e = max(texel, footprint);
    float left = 1.0f - SampleParallaxDepth(heightIndex, materialUv - float2(e.x, 0), uvDx, uvDy, materialSampler);
    float right = 1.0f - SampleParallaxDepth(heightIndex, materialUv + float2(e.x, 0), uvDx, uvDy, materialSampler);
    float down = 1.0f - SampleParallaxDepth(heightIndex, materialUv - float2(0, e.y), uvDx, uvDy, materialSampler);
    float up = 1.0f - SampleParallaxDepth(heightIndex, materialUv + float2(0, e.y), uvDx, uvDy, materialSampler);
    float depthScale = clamp(material.Parallax.x, 0.0f, 0.2f);
    float2 slope = float2(right - left, up - down) / (2.0f * e) * depthScale;
    float3 tangentNormal = normalize(float3(-slope, 1.0f));
    return ApplyNormalMapCommon(normal, worldPos, texCoord, tangentNormal);
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

