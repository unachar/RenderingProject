#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
SamplerState TextureSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 color = float4(SampleWithChromaticAberration(SceneTexture, TextureSampler, input.TexCoord), 1.0f);
    color.rgb = ApplyLensOutput(ApplyHdrOutput(color.rgb), input.TexCoord, input.Position.xy);
    return color;
}


