
#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
SamplerState TextureSampler : register(s0);

float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 color = SceneTexture.Sample(TextureSampler, input.TexCoord);


    float3 sepiaColor;
    sepiaColor.r = dot(color.rgb, float3(0.393, 0.769, 0.189));
    sepiaColor.g = dot(color.rgb, float3(0.349, 0.686, 0.168));
    sepiaColor.b = dot(color.rgb, float3(0.272, 0.534, 0.131));

    float4 result = lerp(color, float4(sepiaColor, color.a), Flags.y);
    result.rgb = ApplyHdrOutput(result.rgb);
    result.a = 1.0f;
    return result;
}
