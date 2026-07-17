#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> HistoryColor : register(t1);
Texture2D<float> DepthBuffer : register(t2);
Texture2D<float4> VelocityBuffer : register(t3);
SamplerState LinearSampler : register(s0);

cbuffer AaConstants : register(b4)
{
    float4x4 PrevViewProj;
    float BlendFactor;
    float2 RcpFrame;
    float HistoryValid;
};

float4 main(PSInputPostProcess input) : SV_TARGET
{
	float2 uv = input.TexCoord;
	float4 current = CurrentColor.SampleLevel(LinearSampler, uv, 0);

    float2 velocity = VelocityBuffer.SampleLevel(LinearSampler, uv, 0).ba;
    float2 historyUv = uv - velocity;
    bool historyInBounds = all(historyUv >= 0.0f) && all(historyUv <= 1.0f);

    float3 neighborhoodMin = current.rgb;
    float3 neighborhoodMax = current.rgb;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUv = saturate(uv + float2(x, y) * RcpFrame);
			float3 sampleColor = CurrentColor.SampleLevel(LinearSampler, sampleUv, 0).rgb;
            neighborhoodMin = min(neighborhoodMin, sampleColor);
            neighborhoodMax = max(neighborhoodMax, sampleColor);
        }
    }

    float3 history = HistoryColor.SampleLevel(LinearSampler, saturate(historyUv), 0).rgb;
    history = clamp(history, neighborhoodMin, neighborhoodMax);

    float motionPixels = length(velocity / max(RcpFrame, 0.000001f));
    float motionConfidence = saturate(1.0f - motionPixels / 32.0f);
    float historyWeight = BlendFactor * motionConfidence;
    historyWeight *= HistoryValid * (historyInBounds ? 1.0f : 0.0f);

    float4 result;
    result.rgb = lerp(current.rgb, history, historyWeight);
    result.a = 1.0f;
    return result;
}
