#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> HistoryColor : register(t1);
Texture2D<float> DepthBuffer : register(t2);
SamplerState LinearSampler : register(s0);

cbuffer AaConstants : register(b4)
{
    float4x4 PrevViewProj;
    float BlendFactor;
    float2 RcpFrame;
};

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 current = CurrentColor.SampleLevel(LinearSampler, input.TexCoord, 0);
    float4 result;
    result.rgb = ApplyHdrOutput(current.rgb);
    result.a = 1.0f;
    return result;
}
