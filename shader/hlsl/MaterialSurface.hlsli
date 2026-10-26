#ifndef MATERIAL_SURFACE_INCLUDED
#define MATERIAL_SURFACE_INCLUDED
// ラスターシェーダーと GPU 駆動シェーダーで同じテクスチャヒープとパラメーターレイアウトを使う。
#include "MaterialTextureHeap.hlsli"
struct AssetMaterialParameters
{
    float4 BaseColor;
    float4 Surface;
    float4 Emission;
    uint4 Textures;
    ProceduralTexture::Parameters Noise[3];
    float4 Parallax; // UV depth, maximum steps, heap-relative height texture index, heap base.
};

ProceduralTexture::Parameters LoadAssetNoise(uint materialID, uint channel)
{
#ifndef SHADER_GPU_DRIVEN
    return AssetNoise[channel];
#else
    uint start = 151u + channel * 13u;
    float4 v[13];
    [unroll] for (uint i = 0; i < 13u; ++i)
        v[i] = LoadSceneMaterialVector(gSceneMaterials, materialID, start + i);
    ProceduralTexture::Parameters p;
    p.Width = asuint(v[0].x); p.Height = asuint(v[0].y); p.Kind = asuint(v[0].z); p.Output = asuint(v[0].w);
    p.BasisType = asuint(v[1].x); p.Fractal = asuint(v[1].y); p.Seed = asuint(v[1].z); p.Tileable = asuint(v[1].w);
    p.Frequency = v[2].x; p.Octaves = asint(v[2].y); p.Lacunarity = v[2].z; p.Gain = v[2].w;
    p.WarpStrength = v[3].x; p.RidgeOffset = v[3].y; p.Contrast = v[3].z; p.Bias = v[3].w;
    p.ColorA = v[4]; p.ColorB = v[5]; p.ColorC = v[6];
    p.Turbulence = v[7].x; p.RingCount = v[7].y; p.Mortar = v[7].z; p.Time = v[7].w;
    p.BricksX = asint(v[8].x); p.BricksY = asint(v[8].y); p.Pattern = asint(v[8].z); p.RepeatX = asint(v[8].w);
    p.RepeatY = asint(v[9].x); p.PatternSize = v[9].y; p.PatternSoftness = v[9].z; p.NormalStrength = v[9].w;
    p.Choppiness = v[10].x; p.Coverage = v[10].y; p.CloudSoftness = v[10].z; p.Padding = v[10].w;
    p.PreviewSize = asuint(v[11].x); p.Tiles = asuint(v[11].y); p.Padding1 = asuint(v[11].z); p.Padding2 = asuint(v[11].w);
    p.Direction = v[12].x; p.Anisotropy = v[12].y; p.KernelFrequency = v[12].z; p.Phase = v[12].w;
    return p;
#endif
}

AssetMaterialParameters LoadAssetMaterial(uint materialID)
{
    AssetMaterialParameters result;
    [unroll] for (uint channel = 0; channel < 3; ++channel)
        result.Noise[channel] = LoadAssetNoise(materialID, channel);
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

float SampleParallaxDepth(ProceduralTexture::Parameters noise, uint index, float2 uv, float2 uvDx, float2 uvDy, SamplerState materialSampler)
{
    if (noise.Width != 0)
        return 1.0f - ProceduralTexture::Scalar(noise, uv);
    // Height maps use white for the original surface and black for the deepest recess.
    return 1.0f - saturate(LoadMaterialTexture(index).SampleGrad(materialSampler, uv, uvDx, uvDy).r);
}

// POM の結果。UV だけでなく、ピクセル深度オフセットとセルフシャドウに必要な接空間とヒット深度を返す。
struct AssetParallaxResult
{
    float2 Uv;          // ずらした UV
    float HitDepth;     // ヒット位置の深さ（0 = 元の面、1 = 最深部）
    float DepthScale;   // UV 単位の深さ（地平線フェード込み）
    float Facing;       // dot(N, V)
    float WorldPerUv;   // UV 1 単位あたりのワールド長
    float3 Tangent;
    float3 Bitangent;
    float3 Normal;
    float2 UvDx;
    float2 UvDy;
    uint HeightIndex;
    ProceduralTexture::Parameters HeightNoise;
    uint MaxSteps;
    bool Active;
};

AssetParallaxResult ApplyAssetParallaxEx(AssetMaterialParameters material, float2 uv, float3 worldPos,
                                         float3 meshNormal, float3 cameraPos, SamplerState materialSampler)
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
    AssetParallaxResult result = (AssetParallaxResult)0;
    result.Uv = uv;
    result.UvDx = uvDx;
    result.UvDy = uvDy;
    result.Active = false;
    // Mask coverage and shadow passes use the original UVs; keep those materials aligned.
    if (material.Surface.z < 0.5f || (material.Textures.w & 3u) != 0u ||
        material.Parallax.x <= 0.0f ||
        (material.Noise[2].Width == 0 && (material.Parallax.z < 0.0f || material.Parallax.z >= 512.0f)))
    {
        return result;
    }
    float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) < 1e-18f)
    {
        return result;
    }
    float3 normal = SafeNormalizeCommon(meshNormal, float3(0, 1, 0));
    float3 tangent = (worldDx * uvDy.y - worldDy * uvDx.y) / determinant;
    tangent -= normal * dot(normal, tangent);
    float3 uvBitangent = (worldDy * uvDx.x - worldDx * uvDy.x) / determinant;
    if (dot(tangent, tangent) < 1e-12f || dot(uvBitangent, uvBitangent) < 1e-12f)
    {
        return result;
    }
    // dP/du・dP/dv の長さ（UV → ワールドの縮尺）。深度オフセットをワールド長に直すのに使う。
    float worldPerUv = sqrt(length(tangent) * length(uvBitangent));
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(normal, tangent));
    if (dot(bitangent, uvBitangent) < 0.0f)
    {
        bitangent = -bitangent;
    }
    float3 viewDirection = SurfaceViewDirectionCommon(worldPos, cameraPos);
    float facing = dot(normal, viewDirection);
    if (facing <= 0.0f)
    {
        return result;
    }
    // Fade before the horizon to prevent an unbounded offset and unstable texture traversal.
    float depthScale = clamp(material.Parallax.x, 0.0f, 0.2f) * smoothstep(0.02f, 0.15f, facing);
    if (depthScale <= 0.0f)
    {
        return result;
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
    float sampledDepth = 0;
    float previousDifference = 0;
    [loop] for (uint step = 0u; step <= steps; ++step)
    {
        sampledDepth = SampleParallaxDepth(material.Noise[2], heightIndex, currentUv, uvDx, uvDy, materialSampler);
        if (step == steps || layerDepth >= sampledDepth) break;
        previousDifference = sampledDepth - layerDepth;
        currentUv -= uvStep;
        layerDepth += layerStep;
    }
    float difference = sampledDepth - layerDepth;
    float interval = previousDifference - difference;
    float previousWeight = interval > 1e-6f ? saturate(-difference / interval) : 0.0f;
    result.Uv = currentUv + uvStep * previousWeight;
    result.HitDepth = saturate(layerDepth - layerStep * previousWeight);
    result.DepthScale = depthScale;
    result.Facing = facing;
    result.WorldPerUv = worldPerUv;
    result.Tangent = tangent;
    result.Bitangent = bitangent;
    result.Normal = normal;
    result.HeightIndex = heightIndex;
    result.HeightNoise = material.Noise[2];
    result.MaxSteps = maxSteps;
    result.Active = true;
    return result;
}

float2 ApplyAssetParallax(AssetMaterialParameters material, float2 uv, float3 worldPos, float3 meshNormal,
                         float3 cameraPos, SamplerState materialSampler)
{
    return ApplyAssetParallaxEx(material, uv, worldPos, meshNormal, cameraPos, materialSampler).Uv;
}

// ピクセル深度オフセット：POM のヒット点（面の奥）をワールドへ戻し、その深度を返す。
// 結果は常に fallbackDepth 以下（Reverse-Z・GREATER 比較で SV_DepthLessEqual の条件を満たす）。
// 接地部分の影・SSAO・ライティングの位置復元・反射レイの始点が凹凸に一致する。
float AssetParallaxPixelDepth(AssetParallaxResult pom, float3 worldPos, float3 cameraPos, float fallbackDepth)
{
    if (!pom.Active || pom.HitDepth <= 0.0f)
    {
        return fallbackDepth;
    }
    float3 viewDirection = SurfaceViewDirectionCommon(worldPos, cameraPos);
    // POM の UV ずれ（HitDepth * DepthScale / facing）と同じ距離だけ視線方向へ潜る。
    float distance = pom.HitDepth * pom.DepthScale / max(pom.Facing, 0.1f) * pom.WorldPerUv;
    float3 displaced = worldPos - viewDirection * distance;
    float4 clip = mul(mul(float4(displaced, 1.0f), View), Projection);
    if (clip.w <= 1e-6f)
    {
        return fallbackDepth;
    }
    return min(saturate(clip.z / clip.w), fallbackDepth);
}

// POM セルフシャドウ：ヒット点からライト方向へ高さマップをたどり、手前の凸部による遮蔽を求める。
// 返り値は可視率（1 = 遮蔽なし）。距離に応じて柔らかくする（Tatarchuk 2006 の soft shadow）。
float AssetParallaxSelfShadow(AssetParallaxResult pom, float3 lightDirection, SamplerState materialSampler)
{
    if (!pom.Active || pom.HitDepth <= 1e-3f)
    {
        return 1.0f;
    }
    float3 lightTS = float3(dot(lightDirection, pom.Tangent), dot(lightDirection, pom.Bitangent),
                            dot(lightDirection, pom.Normal));
    if (lightTS.z <= 0.0f)
    {
        return 1.0f; // 裏側からの光は N·L が 0 にするので、ここでは扱わない
    }
    // 深さ 1 上るごとの UV 移動量。寝た光で発散しないよう z を下限で抑える。
    float2 uvPerDepth = lightTS.xy / max(lightTS.z, 0.05f) * pom.DepthScale;
    uint steps = (uint)clamp(round(pom.MaxSteps * 0.5f), 6.0f, 32.0f);
    float shadow = 0.0f;
    [loop] for (uint i = 1u; i <= steps; ++i)
    {
        float fraction = (float)i / (float)steps;
        float rayDepth = pom.HitDepth * (1.0f - fraction);
        float2 sampleUv = pom.Uv + uvPerDepth * (pom.HitDepth - rayDepth);
        float surfaceDepth = SampleParallaxDepth(pom.HeightNoise, pom.HeightIndex, sampleUv, pom.UvDx, pom.UvDy, materialSampler);
        // 面が光線より上（深さが浅い）なら遮蔽。ヒット点に近い遮蔽ほど影を濃くする。
        shadow = max(shadow, (rayDepth - surfaceDepth) * (1.0f - fraction));
        if (rayDepth <= 0.0f)
        {
            break;
        }
    }
    return 1.0f - saturate(shadow * 8.0f);
}

// セルフシャドウを計算する主光源（LightParams b1 の先頭ライト）の方向。
// GPU 駆動のルートシグネチャは POM_LIGHT_CBV を定義したシェーダーだけ b1 を持つ。
#if !defined(SHADER_GPU_DRIVEN) || defined(POM_LIGHT_CBV)
#define POM_HAS_MAIN_LIGHT 1
float3 PomMainLightDirection(float3 worldPos)
{
    if (LightPositionType.w < 0.5f)
    {
        return SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    }
    return SafeNormalizeCommon(LightPositionType.xyz - worldPos, float3(0.0f, 1.0f, 0.0f));
}
#endif

// 影の GBuffer（PBR 画素では r / b を使わない）にセルフシャドウを詰める。
// r = 1 を印、b = 可視率。DeferredLightingPS 側で PBR 画素かつ r = 1 のときだけ読む。
void WriteAssetParallaxSelfShadow(AssetParallaxResult pom, float3 worldPos, SamplerState materialSampler,
                                  inout float4 shadowGBuffer)
{
#ifdef POM_HAS_MAIN_LIGHT
    if (pom.Active)
    {
        shadowGBuffer.r = 1.0f;
        shadowGBuffer.b = AssetParallaxSelfShadow(pom, PomMainLightDirection(worldPos), materialSampler);
    }
#endif
}

bool AssetParallaxEnabled(AssetMaterialParameters material)
{
    return material.Surface.z > 0.5f && (material.Textures.w & 3u) == 0u && material.Parallax.x > 0.0f &&
           (material.Noise[2].Width != 0 || (material.Parallax.z >= 0.0f && material.Parallax.z < 512.0f));
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
    bool normalNoise = material.Surface.z > 0.5f && material.Noise[1].Width != 0;
    if (!normalNoise && !AssetParallaxEnabled(material)) return normal;
    ProceduralTexture::Parameters noise = material.Noise[2];
    if (normalNoise) noise = material.Noise[1];
    uint heightIndex = (uint)material.Parallax.z;
    float2 texel = 0.00001.xx;
    if (noise.Width == 0)
    {
        uint width, height, levels;
        LoadMaterialTexture(heightIndex).GetDimensions(0u, width, height, levels);
        texel = 1.0f / float2(max(width, 1u), max(height, 1u));
    }
    float2 footprint = float2(max(abs(uvDx.x), abs(uvDy.x)), max(abs(uvDx.y), abs(uvDy.y)));
    float2 e = max(texel, footprint);
    float4 samples = 0;
    // A single call site avoids duplicating the entire noise evaluator four times in DXIL.
    [loop] for (uint i = 0; i < 4; ++i)
    {
        float2 offset = i < 2 ? float2((i == 0 ? -1 : 1) * e.x, 0) : float2(0, (i == 2 ? -1 : 1) * e.y);
        samples[i] = 1.0f - SampleParallaxDepth(noise, heightIndex, materialUv + offset, uvDx, uvDy, materialSampler);
    }
    float scale = normalNoise ? noise.NormalStrength * material.Surface.x / 256.0f : clamp(material.Parallax.x, 0.0f, 0.2f);
    float2 slope = float2(samples.y - samples.x, samples.w - samples.z) / (2.0f * e) * scale;
    float3 tangentNormal = normalize(float3(-slope.x, normalNoise ? slope.y : -slope.y, 1.0f));
    return ApplyNormalMapCommon(normal, worldPos, texCoord, tangentNormal);
}

float4 SampleAssetTexture(uint index, float2 uv, SamplerState materialSampler, float4 fallback)
{
    if (index >= 512u)
    {
        return fallback;
    }
#ifdef SHADER_VISIBILITY_RESOLVE
    return LoadMaterialTexture(index).SampleGrad(materialSampler, uv, VisibilityUvDx, VisibilityUvDy);
#else
    return LoadMaterialTexture(index).Sample(materialSampler, uv);
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
    if (material.Noise[0].Width != 0)
    {
        float value = ProceduralTexture::Scalar(material.Noise[0], uv);
        color.rgb *= ProceduralTexture::Color(material.Noise[0], value);
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


