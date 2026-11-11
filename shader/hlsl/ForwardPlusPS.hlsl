// ForwardPlusPS.hlsl
// Forward+ 用ピクセルシェーダー（不透明特殊マテリアル用）

#include "RendererState.hlsli"
#include "FrameConstants.hlsli"
#define MEGALIGHTS_CB_REGISTER 10
#include "MegaLightsCommon.hlsli"
#include "MaterialSurface.hlsli"
#include "PbrLuts.hlsli"
#include "SurfaceLighting.hlsli"
#include "SharedLighting.hlsli"

struct VSOutput
{
    float4 Pos : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 Tex : TEXCOORD2;
    float3 Tangent : TEXCOORD3;
    float3 Bitangent : TEXCOORD4;
    float4 Color : TEXCOORD5;
    float2 ScreenUV : TEXCOORD6;
    float Depth : TEXCOORD7;
};

ConstantBuffer<ConstantBuffer3D> g_Constants : register(b0);
ConstantBuffer<LightConstants> g_LightConstants : register(b1);
ConstantBuffer<GridConstants> g_GridConstants : register(b3);
ConstantBuffer<PBRConstants> g_PBRConstants : register(b5);

StructuredBuffer<uint> g_LightIndexBuffer : register(t0);
StructuredBuffer<uint> g_LightGridCounter : register(t1);
StructuredBuffer<MegaLightData> g_LightData : register(t2);

Texture2D<float> g_SceneDepth : register(t3);
Texture2DArray g_Textures : register(t4);
Texture2D g_EnvironmentMap : register(t30);
Texture2D g_PBRLuts : register(t31);
Texture2DArray g_ShadowMaps : register(t32);

SamplerState g_SamplerAniso : register(s0);
SamplerComparisonState g_SamplerShadow : register(s1);

float4 main(VSOutput input) : SV_TARGET0
{
    // マテリアルパラメータの取得
    MaterialParameters material = GetMaterialParameters(input.Tex, input.Color);
    
    // 法線マップ適用
    float3 worldNormal = input.WorldNormal;
    if (material.UseNormalMap)
    {
        worldNormal = ApplyNormalMap(material, input.Tex, input.Tangent, input.Bitangent, input.WorldNormal);
    }
    
    // 視線ベクトル
    float3 viewDir = normalize(g_Constants.CameraPos - input.WorldPos);
    
    // 線形深度
    float linearDepth = LinearDepthFromDepthBuffer(input.ScreenUV, g_SceneDepth);
    
    // タイル座標計算
    uint3 tileCoord = ComputeTileCoord(input.ScreenUV * float2(g_GridConstants.TilesX * g_TileSize, g_GridConstants.TilesY * g_TileSize), linearDepth);
    
    // クラスタードライティング評価
    float3 diffuse, specular;
    float3 lighting = EvaluateClusteredLightsForward(
        input.WorldPos, worldNormal, viewDir, material,
        input.ScreenUV, linearDepth, diffuse, specular);
    
    // IBL
    float3 ibl = EvaluateIBL(input.WorldPos, worldNormal, viewDir, material, linearDepth);
    
    // エミッション
    float3 emission = material.Emissive * material.EmissiveIntensity;
    
    // 最終色
    float3 color = lighting + ibl + emission;
    
    // トーンマッピングは後段のポストプロセスで行うため、ここではHDR値を出力
    return float4(color, material.Alpha);
}

// 簡易版：マテリアルパラメータ取得
MaterialParameters GetMaterialParameters(float2 texCoord, float4 vertexColor)
{
    MaterialParameters mat;
    mat.BaseColor = vertexColor.rgb;
    mat.Metallic = 0.0f;
    mat.Roughness = 0.5f;
    mat.Alpha = vertexColor.a;
    mat.NormalMap = -1;
    mat.Emissive = 0.0f;
    mat.EmissiveIntensity = 1.0f;
    mat.ClearCoat = 0.0f;
    mat.ClearCoatRoughness = 0.0f;
    mat.Anisotropy = 0.0f;
    mat.AnisotropyDir = float2(1, 0);
    mat.UseNormalMap = 0;
    return mat;
}

// 法線マップ適用
float3 ApplyNormalMap(MaterialParameters material, float2 texCoord, float3 tangent, float3 bitangent, float3 worldNormal)
{
    if (!material.UseNormalMap) return worldNormal;
    
    // 実装はMaterialSurface.hlsliのものを使用
    return worldNormal;
}