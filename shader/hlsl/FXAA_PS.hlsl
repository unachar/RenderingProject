#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> InputColor : register(t0);
SamplerState LinearSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 color = InputColor.Sample(LinearSampler, input.TexCoord);
    color.rgb = ApplyHdrOutput(color.rgb);
    color.a = 1.0f;
    return color;
}