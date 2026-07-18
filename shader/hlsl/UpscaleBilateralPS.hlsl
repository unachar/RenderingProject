#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> LowResScene : register(t0);
Texture2D<float> LowResDepth : register(t1);
SamplerState LinearSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float depthCenter = LowResDepth.SampleLevel(LinearSampler, input.TexCoord, 0);
    float resolutionScale = max(abs(Flags.w), 0.001f);
    float2 step = float2(ddx(input.TexCoord.x), ddy(input.TexCoord.y)) / resolutionScale;

    float totalWeight = 0.0f;
    float4 result = 0.0f;

    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 sampleUV = input.TexCoord + float2(x, y) * step;
            float depthSample = LowResDepth.SampleLevel(LinearSampler, sampleUV, 0);
            float depthWeight = rcp(1.0f + abs(depthSample - depthCenter) * 100.0f);
            float4 color = LowResScene.SampleLevel(LinearSampler, sampleUV, 0);
            result += color * depthWeight;
            totalWeight += depthWeight;
        }
    }

	result /= max(totalWeight, 0.001f);
	result.a = 1.0f;
    return result;
}
