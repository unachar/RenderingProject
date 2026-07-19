#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
SamplerState TextureSampler : register(s0);


float4 main(PSInputPostProcess input) : SV_TARGET
{
    float4 originalColor = SceneTexture.Sample(TextureSampler, input.TexCoord);
    if (Flags.y <= 0.001f)
    {
        originalColor.rgb = ApplyHdrOutput(originalColor.rgb);
        originalColor.a = 1.0f;
        return originalColor;
    }

    float width, height;
    SceneTexture.GetDimensions(width, height);
    float2 texelSize = 1.0f / float2(width, height);

    float4 blurColor = 0;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float2 offset = float2(x, y) * texelSize;
            blurColor += SceneTexture.Sample(TextureSampler, input.TexCoord + offset);
        }
    }
    blurColor *= (1.0f / 9.0f);

    float4 result = lerp(originalColor, blurColor, Flags.y);
    result.rgb = ApplyHdrOutput(result.rgb);
    result.a = 1.0f;
    return result;
}
