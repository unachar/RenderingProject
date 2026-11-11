// ClusteredLightGridClearCS.hlsl
// ライトグリッドのカウンタをクリアする

#include "RendererState.hlsli"

struct GridConstants
{
    uint TilesX;
    uint TilesY;
    uint Slices;
    uint MaxLightsPerTile;
    float SliceFar;
    float NearClip;
    float FarClip;
    float Padding;
    float4x4 View;
    float4x4 Proj;
    float4x4 InvView;
    float4x4 InvProj;
};

ConstantBuffer<GridConstants> g_GridConstants : register(b0);

RWStructuredBuffer<uint> g_LightGridCounter : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint totalTiles = g_GridConstants.TilesX * g_GridConstants.TilesY * g_GridConstants.Slices;
    if (id.x >= totalTiles) return;
    
    g_LightGridCounter[id.x] = 0;
}