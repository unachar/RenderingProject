// ForwardPlusParticleVS.hlsl
// パーティクル用頂点シェーダー

#include "RendererState.hlsli"
#include "FrameConstants.hlsli"

struct VSInput
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float2 Tex : TEXCOORD0;
    float4 Color : COLOR;
    float4 InstanceData0 : TEXCOORD1; // Position, Scale, Rotation
    float4 InstanceData1 : TEXCOORD2; // Velocity, Lifetime, Random
};

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

VSOutput main(VSInput input)
{
    VSOutput output;
    
    // インスタンスデータからワールド位置計算
    float3 worldPos = input.InstanceData0.xyz + input.Pos * input.InstanceData0.w;
    output.WorldPos = worldPos;
    output.WorldNormal = input.Normal;
    output.Tex = input.Tex;
    output.Color = input.Color;
    
    float4 clipPos = mul(float4(worldPos, 1.0f), g_Constants.ViewProjection);
    output.Pos = clipPos;
    output.ScreenUV = clipPos.xy / clipPos.w * 0.5f + 0.5f;
    output.Depth = clipPos.z / clipPos.w;
    
    return output;
}