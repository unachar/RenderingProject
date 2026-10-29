#pragma once

namespace GBufferPreviewShader
{
    inline constexpr const char* Pixel = R"hlsl(
cbuffer PreviewSettings : register(b1) { uint PreviewType; };
Texture2D<float4> PreviewTexture : register(t0);
struct PixelInput
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR0;
    float2 Uv : TEXCOORD0;
};
float4 main(PixelInput input) : SV_Target
{
    uint width, height;
    PreviewTexture.GetDimensions(width, height);
    uint2 pixel = min(uint2(saturate(input.Uv) * float2(width, height)), uint2(width - 1, height - 1));
    float4 value = PreviewTexture.Load(int3(pixel, 0));
    float3 color = value.rgb;
    // Types follow GBufferType; 10 is the separate emission alpha (material AO) view.
    if (PreviewType == 2)
    {
        // Keep reverse-Z ordering and background zero, while revealing small far-depth values.
        color = pow(saturate(value.r), 0.2f).xxx;
    }
    else if (PreviewType == 5 || PreviewType == 6 || PreviewType == 9)
    {
        color = max(color, 0.0f);
        color = color / (1.0f + color);
    }
    else if (PreviewType == 7)
    {
        // RG already contains the renderer's signed, amplified display velocity; BA is raw motion.
        color = float3(value.rg, 0.5f);
    }
    else if (PreviewType == 10)
    {
        color = saturate(value.a).xxx;
    }
    // GBuffer alpha carries data (POM displacement, material ID, motion), never UI opacity.
    return float4(color, 1.0f);
}
)hlsl";
    inline constexpr const char* Vertex = R"hlsl(
cbuffer VertexSettings : register(b0) { float4x4 ProjectionMatrix; };
struct VertexInput { float2 Position : POSITION; float4 Color : COLOR0; float2 Uv : TEXCOORD0; };
struct VertexOutput { float4 Position : SV_POSITION; float4 Color : COLOR0; float2 Uv : TEXCOORD0; };
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.Position = mul(ProjectionMatrix, float4(input.Position, 0, 1));
    output.Color = input.Color;
    output.Uv = input.Uv;
    return output;
}
)hlsl";
}
