
#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
SamplerState TextureSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 color = SceneTexture.Sample(TextureSampler, input.TexCoord);


    float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));

    float4 result = lerp(color, float4(gray, gray, gray, color.a), Flags.y);
    result.rgb = ApplyHdrOutput(result.rgb);
    result.a = 1.0f;
    return result;
}
