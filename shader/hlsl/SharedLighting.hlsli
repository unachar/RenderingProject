// SharedLighting.hlsli
// Deferred と Forward+ で共有するライティング評価関数

#ifndef SHARED_LIGHTING_HLSLI
#define SHARED_LIGHTING_HLSLI

#include "RendererState.hlsli"
#include "MegaLightsCommon.hlsli"
#include "MaterialSurface.hlsli"
#include "PbrLuts.hlsli"
#include "SurfaceLighting.hlsli"

// ライトグリッドリソース
StructuredBuffer<uint> g_LightIndexBuffer : register(t40);
StructuredBuffer<uint> g_LightGridCounter : register(t41);
StructuredBuffer<MegaLightData> g_LightData : register(t42);
ConstantBuffer<GridConstants> g_GridConstants : register(b0);

// クラスターグリッドからライトリストを取得
uint GetTileLightCount(uint3 tileCoord)
{
    uint tileIdx = (tileCoord.z * g_GridConstants.TilesY + tileCoord.y) * g_GridConstants.TilesX + tileCoord.x;
    return g_LightGridCounter[tileIdx];
}

uint GetTileLightIndex(uint3 tileCoord, uint lightIdx)
{
    uint tileIdx = (tileCoord.z * g_GridConstants.TilesY + tileCoord.y) * g_GridConstants.TilesX + tileCoord.x;
    uint baseIndex = tileIdx * g_GridConstants.MaxLightsPerTile;
    return g_LightIndexBuffer[baseIndex + lightIdx];
}

// タイル座標を計算
uint3 ComputeTileCoord(float2 screenPos, float linearDepth)
{
    uint2 tileSize = uint2(g_TileSize, g_TileSize);
    uint2 tilesXY = uint2(g_GridConstants.TilesX, g_GridConstants.TilesY);
    
    uint tileX = min(uint(screenPos.x / g_TileSize), tilesXY.x - 1);
    uint tileY = min(uint(screenPos.y / g_TileSize), tilesXY.y - 1);
    
    float sliceFar = g_GridConstants.SliceFar;
    float nearClip = g_GridConstants.NearClip;
    float ratio = log2(max(linearDepth, nearClip) / nearClip) / log2(sliceFar / nearClip);
    uint slice = min(uint(ratio * g_GridConstants.Slices), g_GridConstants.Slices - 1);
    
    return uint3(tileX, tileY, slice);
}

// 共有ライティング評価関数
float3 EvaluateClusteredLights(
    float3 worldPos,
    float3 worldNormal,
    float3 viewDir,
    MaterialParameters material,
    uint3 tileCoord,
    float linearDepth,
    float2 screenUV,
    out float3 diffuseOut,
    out float3 specularOut)
{
    float3 diffuse = 0.0f;
    float3 specular = 0.0f;
    
    uint lightCount = GetTileLightCount(tileCoord);
    lightCount = min(lightCount, g_GridConstants.MaxLightsPerTile);
    
    for (uint i = 0; i < lightCount; ++i)
    {
        uint lightIdx = GetTileLightIndex(tileCoord, i);
        MegaLightData light = g_LightData[lightIdx];
        
        // ライトタイプチェック
        uint lightType = asuint(light.PositionType.w);
        if (lightType == LIGHT_TYPE_INVALID) continue;
        
        float3 lightPos = light.PositionType.xyz;
        float3 lightDir = light.DirectionRange.xyz;
        float lightRange = light.DirectionRange.w;
        float3 lightColor = light.Color.xyz;
        float lightIntensity = light.Color.w;
        float lightRadius = light.DirectionRange.y; // ソース半径
        
        // 方向性ライト
        if (lightType == LIGHT_TYPE_DIRECTIONAL)
        {
            lightDir = -lightDir; // DirectionRangeは方向を持つ
        }
        else
        {
            lightDir = normalize(lightPos - worldPos);
            float dist = length(lightPos - worldPos);
            
            // 距離減衰
            float attenuation = saturate(1.0f - dist / lightRange);
            attenuation *= attenuation;
            lightIntensity *= attenuation;
            
            // スポットライト
            if (lightType == LIGHT_TYPE_SPOT)
            {
                float spotCos = dot(-lightDir, light.DirectionRange.xyz);
                float innerCos = cos(radians(light.Extra.x));
                float outerCos = cos(radians(light.Extra.y));
                float spotFactor = smoothstep(outerCos, innerCos, spotCos);
                lightIntensity *= spotFactor;
            }
        }
        
        // 影の適用（簡易版 - 実際はシャドウマップ/RTシャドウを参照）
        float shadow = 1.0f;
        if (asuint(light.Flags.x) != 0) // CastShadow
        {
            // TODO: シャドウマップサンプリング
            shadow = 1.0f;
        }
        
        lightIntensity *= shadow;
        
        // BRDF評価
        float3 L = lightDir;
        float3 V = viewDir;
        float3 N = worldNormal;
        float3 H = normalize(L + V);
        
        float NdotL = saturate(dot(N, L));
        float NdotV = saturate(dot(N, V));
        float NdotH = saturate(dot(N, H));
        float LdotH = saturate(dot(L, H));
        
        if (NdotL > 0.0f && lightIntensity > 0.0f)
        {
            // Disney BRDF
            float3 F = SpecularFresnel(material.BaseColor, material.Metallic, LdotH);
            float G = SmithGGXCorrelated(NdotL, NdotV, material.Roughness);
            float D = GGXDistribution(NdotH, material.Roughness);
            
            float3 specularBRDF = F * G * D / (4.0f * NdotL * NdotV + 0.0001f);
            float3 diffuseBRDF = (1.0f - F) * material.BaseColor / PI * (1.0f - material.Metallic);
            
            // クリアコート
            if (material.ClearCoat > 0.0f)
            {
                float3 ccF = SpecularFresnel(1.0f, 0.0f, LdotH);
                float ccG = SmithGGXCorrelated(NdotL, NdotV, 0.25f * material.ClearCoatRoughness);
                float ccD = GGXDistribution(NdotH, 0.25f * material.ClearCoatRoughness);
                specularBRDF += 0.25f * material.ClearCoat * ccF * ccG * ccD / (4.0f * NdotL * NdotV + 0.0001f);
            }
            
            float3 lightContribution = lightColor * lightIntensity * NdotL;
            diffuse += diffuseBRDF * lightContribution;
            specular += specularBRDF * lightContribution;
        }
    }
    
    diffuseOut = diffuse;
    specularOut = specular;
    return diffuse + specular;
}

// フルスクリーン/ディファード用：GBufferから再構成
float3 EvaluateClusteredLightsDeferred(
    float2 screenUV,
    float3 worldPos,
    float3 worldNormal,
    float3 viewDir,
    MaterialParameters material,
    float linearDepth,
    out float3 diffuseOut,
    out float3 specularOut)
{
    uint3 tileCoord = ComputeTileCoord(screenUV * float2(g_GridConstants.TilesX * g_TileSize, g_GridConstants.TilesY * g_TileSize), linearDepth);
    return EvaluateClusteredLights(worldPos, worldNormal, viewDir, material, tileCoord, linearDepth, screenUV, diffuseOut, specularOut);
}

// フォワード用：ピクセル座標から直接計算
float3 EvaluateClusteredLightsForward(
    float3 worldPos,
    float3 worldNormal,
    float3 viewDir,
    MaterialParameters material,
    float2 screenUV,
    float linearDepth,
    out float3 diffuseOut,
    out float3 specularOut)
{
    uint3 tileCoord = ComputeTileCoord(screenUV * float2(g_GridConstants.TilesX * g_TileSize, g_GridConstants.TilesY * g_TileSize), linearDepth);
    return EvaluateClusteredLights(worldPos, worldNormal, viewDir, material, tileCoord, linearDepth, screenUV, diffuseOut, specularOut);
}

// 環境光（IBL）の評価
float3 EvaluateIBL(
    float3 worldPos,
    float3 worldNormal,
    float3 viewDir,
    MaterialParameters material,
    float linearDepth)
{
    // 簡易版 - 実際にはPreradiance/Reflection probeを使用
    float3 diffuseIBL = material.BaseColor * (1.0f - material.Metallic) * 0.03f;
    float3 specularIBL = material.BaseColor * material.Metallic * 0.03f; // 近似
    return diffuseIBL + specularIBL;
}

#endif // SHARED_LIGHTING_HLSLI