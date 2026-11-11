// ClusteredLightGridBuildCS.hlsl
// クラスタード・ライトグリッドを構築する

#include "RendererState.hlsli"
#include "FrameConstants.hlsli"
#include "GpuSceneCommon.hlsli"
#define MEGALIGHTS_CB_REGISTER 0
#include "MegaLightsCommon.hlsli"

ConstantBuffer<GridConstants> g_GridConstants : register(b0);

StructuredBuffer<MegaLightData> g_LightData : register(t0);
Texture2D<float> g_DepthBuffer : register(t1);
RWStructuredBuffer<uint> g_LightIndexBuffer : register(u0);
RWStructuredBuffer<uint> g_LightGridCounter : register(u1);
StructuredBuffer<GPUInstanceData> g_Instances : register(t2);
StructuredBuffer<MegaLightData> g_Lights : register(t3);

cbuffer TileConfig : register(b1)
{
    uint g_TilesX;
    uint g_TilesY;
    uint g_Slices;
    uint g_MaxLightsPerTile;
};

uint GetTileIndex(uint tileX, uint tileY, uint slice)
{
    return (slice * g_TilesY + tileY) * g_TilesX + tileX;
}

float LinearDepth(float depth)
{
    // Reverse-Z: 0=far, 1=near
    float nearClip = g_GridConstants.NearClip;
    float farClip = g_GridConstants.FarClip;
    return nearClip * farClip / (farClip - depth * (farClip - nearClip));
}

uint ComputeSlice(float linearDepth)
{
    float sliceFar = g_GridConstants.SliceFar;
    float nearClip = g_GridConstants.NearClip;
    float farClip = g_GridConstants.FarClip;
    
    // 対数的なスライス分布（近くほど細かく）
    float ratio = log2(linearDepth / nearClip) / log2(sliceFar / nearClip);
    uint slice = uint(ratio * g_Slices);
    return min(slice, g_Slices - 1);
}

bool LightAffectsTile(const MegaLightData light, uint tileX, uint tileY, uint slice)
{
    // ライトのAABBとタイルのフリスカムをテスト
    float3 lightPos = light.PositionType.xyz;
    float lightRange = light.DirectionRange.w;
    uint lightType = asuint(light.PositionType.w);
    
    // タイルの中心をワールド座標で計算
    float2 tileCenterNDC = (float2(tileX, tileY) + 0.5f) * (float(g_TileSize) / float2(g_GridConstants.TilesX * g_TileSize, g_GridConstants.TilesY * g_TileSize)) * 2.0f - 1.0f;
    float sliceCenterDepth = (float(slice) + 0.5f) / float(g_Slices);
    
    // NDCからワールド座標へ（逆投影）
    float4 clipPos = float4(tileCenterNDC, sliceCenterDepth * 2.0f - 1.0f, 1.0f);
    float4 viewPos = mul(g_GridConstants.InvProj, clipPos);
    viewPos /= viewPos.w;
    float4 worldPos = mul(g_GridConstants.InvView, viewPos);
    float3 tileCenterWS = worldPos.xyz / worldPos.w;
    
    float dist = length(tileCenterWS - lightPos);
    
    if (lightType == 1) // LIGHT_TYPE_DIRECTIONAL
    {
        return true; // 方向性ライトは全タイルに影響
    }
    
    return dist <= lightRange;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint totalTiles = g_TilesX * g_TilesY * g_Slices;
    if (id.x >= totalTiles) return;
    
    uint tileIdx = id.x;
    uint slice = tileIdx / (g_TilesX * g_TilesY);
    uint tileY = (tileIdx % (g_TilesX * g_TilesY)) / g_TilesX;
    uint tileX = tileIdx % g_TilesX;
    
    uint lightCount = 0;
    uint baseIndex = tileIdx * g_MaxLightsPerTile;
    
    uint lightCountTotal = g_Lights.Length(); // 実際のライト数は定数バッファから取得すべき
    
    // ライトリストを走査してタイルに影響するライトを追加
    for (uint i = 0; i < lightCountTotal && lightCount < g_MaxLightsPerTile; ++i)
    {
        MegaLightData light = g_Lights[i];
        
        // 無効なライトをスキップ
        if (light.Color.w <= 0.0f) continue; // Intensity
        
        if (LightAffectsTile(light, tileX, tileY, slice))
        {
            g_LightIndexBuffer[baseIndex + lightCount] = i;
            lightCount++;
        }
    }
    
    g_LightGridCounter[tileIdx] = lightCount;
}