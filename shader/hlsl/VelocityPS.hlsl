#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> WorldPositionTexture : register(t0);
Texture2D<float> DepthTexture : register(t2);
SamplerState PointSampler : register(s0);

cbuffer VelocityConstants : register(b4)
{
    float4x4 PrevViewProjection;
    float2 RcpVelocitySize;
    float2 VelocityPadding;
};

float2 main(PSInputPostProcess input) : SV_Target
{
    float depth = DepthTexture.SampleLevel(PointSampler, input.TexCoord, 0);
    if (depth >= 0.9999f)
    {
        return 0.0f;
    }

    float3 worldPosition = WorldPositionTexture.SampleLevel(PointSampler, input.TexCoord, 0).xyz;
    float4 previousClip = mul(float4(worldPosition, 1.0f), PrevViewProjection);
    if (previousClip.w <= 0.00001f)
    {
        return 0.0f;
    }

    float2 previousNdc = previousClip.xy / previousClip.w;
    float2 previousUv = previousNdc * float2(0.5f, -0.5f) + 0.5f;
    return input.TexCoord - previousUv;
}
