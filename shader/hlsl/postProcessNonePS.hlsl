#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
SamplerState TextureSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 color = SceneTexture.Sample(TextureSampler, input.TexCoord);
    color.rgb = ApplyHdrOutput(color.rgb);
    color.a = 1.0f;
    return color;
}
