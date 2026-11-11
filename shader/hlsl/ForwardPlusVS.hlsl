// ForwardPlusVS.hlsl
// Forward+ 用頂点シェーダー

#include "RendererState.hlsli"
#include "FrameConstants.hlsli"
#include "MaterialSurface.hlsli"

struct VSInput
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float2 Tex : TEXCOORD0;
    float4 Tangent : TANGENT;
    float4 Color : COLOR;
};

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

VSOutput main(VSInput input)
{
    VSOutput output;
    
    float4 worldPos = mul(float4(input.Pos, 1.0f), g_Constants.World);
    output.WorldPos = worldPos.xyz;
    
    float3 worldNormal = normalize(mul(input.Normal, (float3x3)g_Constants.World));
    output.WorldNormal = worldNormal;
    
    float3 tangent = normalize(mul(input.Tangent.xyz, (float3x3)g_Constants.World));
    float3 bitangent = cross(worldNormal, tangent) * input.Tangent.w;
    output.Tangent = tangent;
    output.Bitangent = bitangent;
    
    output.Tex = input.Tex;
    output.Color = input.Color;
    
    float4 clipPos = mul(worldPos, g_Constants.ViewProjection);
    output.Pos = clipPos;
    output.ScreenUV = clipPos.xy / clipPos.w * 0.5f + 0.5f;
    output.Depth = clipPos.z / clipPos.w;
    
    return output;
}