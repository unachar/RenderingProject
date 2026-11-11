// ForwardPlusParticlePS.hlsl
// パーティクル用ピクセルシェーダー（加算合成）

#include "RendererState.hlsli"
#include "FrameConstants.hlsli"
#define MEGALIGHTS_CB_REGISTER 10
#include "MegaLightsCommon.hlsli"
#include "SharedLighting.hlsli"

struct VSOutput
{
    float4 Pos : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 Tex : TEXCOORD2;
    float4 Color : TEXCOORD3;
    float2 ScreenUV : TEXCOORD4;
    float Depth : TEXCOORD5;
};

ConstantBuffer<ConstantBuffer3D> g_Constants : register(b0);
ConstantBuffer<LightConstants> g_LightConstants : register(b1);
ConstantBuffer<GridConstants> g_GridConstants : register(b3);

StructuredBuffer<uint> g_LightIndexBuffer : register(t0);
StructuredBuffer<uint> g_LightGridCounter : register(t1);
StructuredBuffer<MegaLightData> g_LightData : register(t2);

Texture2D<float> g_SceneDepth : register(t3);
Texture2DArray g_Textures : register(t4);

SamplerState g_SamplerAniso : register(s0);

float4 main(VSOutput input) : SV_TARGET0
{
    // パーティクルは簡易ライティング（非ライティング or 簡易ライティング）
    float3 color = input.Color.rgb * input.Color.a;
    
    // 必要に応じて簡易ライティング追加
    // float3 viewDir = normalize(g_Constants.CameraPos - input.WorldPos);
    // float linearDepth = LinearDepthFromDepthBuffer(input.ScreenUV, g_SceneDepth);
    // ...
    
    return float4(color, input.Color.a);
}