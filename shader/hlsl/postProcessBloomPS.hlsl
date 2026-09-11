#define SHADER_POSTPROCESS
#include "common.hlsl"

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> BloomTexture : register(t15);
SamplerState TextureSampler : register(s0);

float3 ExtractBloom(float3 color)
{
    const float brightness = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    const float threshold = BloomParams.y;
    const float knee = threshold * saturate(BloomParams.z);
    const float soft = brightness - threshold + knee;
    const float softContribution = clamp(soft, 0.0f, 2.0f * knee);
    const float softPart = softContribution * softContribution / (4.0f * knee);
    const float hardPart = max(brightness - threshold, 0.0f);

    const float contribution = max(softPart, hardPart) / brightness;
    return color * contribution;
}

float3 SampleBlurredBloom(float2 uv)
{
    float width;
    float height;

    SceneTexture.GetDimensions(width, height);

    const float radius = max(BloomParams.w, 0.01f);
    const float2 texel = radius / float2(width, height);
    float3 bloom = 0.0f;

    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv).rgb) * 0.20f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv + float2(texel.x, 0.0f)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv - float2(texel.x, 0.0f)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv + float2(0.0f, texel.y)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv - float2(0.0f, texel.y)).rgb) * 0.12f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv + texel).rgb) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv + float2(texel.x, -texel.y)).rgb ) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv + float2(-texel.x, texel.y)).rgb) * 0.08f;
    bloom += ExtractBloom(SceneTexture.Sample(TextureSampler,uv - texel).rgb) * 0.08f;

    return bloom;
}

float4 main(PSInputPostProcess input) : SV_TARGET
{
    const float3 scene = SceneTexture.Sample( TextureSampler,input.TexCoord).rgb;

    const float3 bloom = SampleBlurredBloom(input.TexCoord);

    const float intensity = max(Flags.y, 0.0f);
    
    const float3 result = scene + bloom * intensity;

    return float4(ApplyHdrOutput(result),1.0f);
}