#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float> DepthTexture : register(t2);
SamplerState PointSampler : register(s0);

cbuffer VelocityConstants : register(b4)
{
    float4x4 PrevViewProjection;
    float2 RcpVelocitySize;
    float2 VelocityPadding;
};

float4 EncodeVelocity(float2 velocity)
{
    float2 displayVelocity = sign(velocity) * saturate(sqrt(abs(velocity) * 100.0f));
    return float4(displayVelocity * 0.5f + 0.5f, velocity);
}

float4 main(PSInputPostProcess input) : SV_Target
{
    float depth = DepthTexture.SampleLevel(PointSampler, input.TexCoord, 0);
    if (depth >= 0.9999f)
    {
        return EncodeVelocity(0.0f);
    }

    float3 worldPosition = ReconstructPostProcessWorldPositionCommon(input.TexCoord, depth);
    float4 previousClip = mul(float4(worldPosition, 1.0f), PrevViewProjection);
    if (previousClip.w <= 0.00001f)
    {
        return EncodeVelocity(0.0f);
    }

    float2 previousNdc = previousClip.xy / previousClip.w;
    float2 previousUv = previousNdc * float2(0.5f, -0.5f) + 0.5f;
    return EncodeVelocity(input.TexCoord - previousUv);
}
