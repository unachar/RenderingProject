#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> BloomTexture : register(t15);
SamplerState TextureSampler : register(s0);

float3 ExtractBloom(float3 color)
{
    const float brightness = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    const float threshold = max(BloomParams.y, 0.0001f);
    const float knee = max(threshold * saturate(BloomParams.z), 0.00001f);
    float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 0.00001f);
    const float contribution = max(soft, brightness - threshold) / max(brightness, 0.0001f);
    return color * contribution;
}

float3 SampleBlurredBloom(float2 uv)
{
    float width, height;
    SceneTexture.GetDimensions(width, height);
    const float2 texel = max(BloomParams.w, 0.25f) * 1.5f / float2(width, height);

    float3 bloom = ExtractBloom(SceneTexture.Sample(TextureSampler, uv).rgb) * 0.20f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv + float2(texel.x, 0.0f)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv - float2(texel.x, 0.0f)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv + float2(0.0f, texel.y)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv - float2(0.0f, texel.y)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv + texel).rgb) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv + float2(texel.x, -texel.y)).rgb) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv + float2(-texel.x, texel.y)).rgb) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler, uv - texel).rgb) * 0.08f;
    return bloom;
}

float4 main(PSInputPostProcess input) : SV_TARGET
{
    const float3 scene = SceneTexture.Sample(TextureSampler, input.TexCoord).rgb;

    // BloomParams.x: 0 = one-pass fallback, 1 = extract to BloomBuffer, 2 = composite BloomBuffer.
    if (BloomParams.x > 1.5f)
    {
    // テクスチャサイズを取得してUVスケールを計算
        float bloomWidth, bloomHeight;
        BloomTexture.GetDimensions(bloomWidth, bloomHeight);
        float sceneWidth, sceneHeight;
        SceneTexture.GetDimensions(sceneWidth, sceneHeight);
        float2 bloomUv = input.TexCoord * float2(bloomWidth, bloomHeight) / float2(sceneWidth, sceneHeight);
    
        const float3 bloom = BloomTexture.Sample(TextureSampler, bloomUv).rgb;
        return float4(ApplyHdrOutput(scene + bloom * max(Flags.y, 0.0f)), 1.0f);
    }

    const float3 bloom = SampleBlurredBloom(input.TexCoord);
    if (BloomParams.x > 0.5f)
    {
        return float4(bloom, 1.0f);
    }

    return float4(ApplyHdrOutput(scene + bloom * max(Flags.y, 0.0f)), 1.0f);
}
