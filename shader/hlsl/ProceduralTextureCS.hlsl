#include "ProceduralTexture.hlsli"

// Pixel-independent procedural work. Escape fractals retain the CPU double-precision path.
cbuffer TextureParameters : register(b0)
{
    ProceduralTexture::Parameters Params;
};

RWStructuredBuffer<float4> Generated : register(u0);
StructuredBuffer<float4> ImagePixels : register(t0);
RWByteAddressBuffer PreviewPixels : register(u1);
float HeightAt(int2 pixel)
{
    int2 wrapped = int2(TextureNoise::Wrap(pixel.x, Params.Width), TextureNoise::Wrap(pixel.y, Params.Height));
    return ImagePixels[wrapped.y * Params.Width + wrapped.x].x;
}

[numthreads(8, 8, 1)]
void Generate(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Params.Width || thread.y >= Params.Height)
    {
        return;
    }
    float value = ProceduralTexture::Scalar(Params, (thread.xy + 0.5) / float2(Params.Width, Params.Height));
    if (Params.Kind == ProceduralTexture::TextureChecker)
    {
        // Integer cell addressing avoids reciprocal rounding at hard checker boundaries.
        uint cellX = ((thread.x * 2 + 1) * (uint)Params.BricksX) / (Params.Width * 2);
        uint cellY = ((thread.y * 2 + 1) * (uint)Params.BricksX) / (Params.Height * 2);
        value = saturate((((cellX + cellY) & 1) - 0.5) * Params.Contrast + 0.5 + Params.Bias);
    }
    float3 color = Params.Output == ProceduralTexture::OutputColor ? ProceduralTexture::Color(Params, value) : value.xxx;
    Generated[thread.y * Params.Width + thread.x] = float4(color, 1);
}

[numthreads(8, 8, 1)]
void Normals(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Params.Width || thread.y >= Params.Height)
    {
        return;
    }
    int2 pixel = int2(thread.xy);
    float dx = (HeightAt(pixel + int2(1, 0)) - HeightAt(pixel - int2(1, 0))) * Params.NormalStrength;
    float dy = (HeightAt(pixel + int2(0, 1)) - HeightAt(pixel - int2(0, 1))) * Params.NormalStrength;
    float3 normal = normalize(float3(-dx, dy, 1));
    Generated[thread.y * Params.Width + thread.x] = float4(normal * 0.5 + 0.5, 1);
}

float4 Pixel(int2 pixel)
{
    return ImagePixels[TextureNoise::Wrap(pixel.y, Params.Height) * Params.Width + TextureNoise::Wrap(pixel.x, Params.Width)];
}

[numthreads(8, 8, 1)]
void Preview(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Params.PreviewSize || thread.y >= Params.PreviewSize)
    {
        return;
    }
    float2 uv = (thread.xy + 0.5) / (float)Params.PreviewSize * Params.Tiles;
    float2 pixel = frac(uv) * float2(Params.Width, Params.Height) - 0.5;
    int2 start = (int2)floor(pixel);
    float2 blend = frac(pixel);
    float3 color = lerp(lerp(Pixel(start), Pixel(start + int2(1, 0)), blend.x),
                        lerp(Pixel(start + int2(0, 1)), Pixel(start + int2(1, 1)), blend.x), blend.y).rgb;
    if (Params.Output == ProceduralTexture::OutputColor)
    {
        color = saturate(color);
        color = float3(color.r <= 0.0031308 ? color.r * 12.92 : 1.055 * pow(color.r, 1.0 / 2.4) - 0.055,
                       color.g <= 0.0031308 ? color.g * 12.92 : 1.055 * pow(color.g, 1.0 / 2.4) - 0.055,
                       color.b <= 0.0031308 ? color.b * 12.92 : 1.055 * pow(color.b, 1.0 / 2.4) - 0.055);
    }
    uint3 packed = (uint3)(saturate(color) * 255 + 0.5);
    uint rowPitch = (Params.PreviewSize * 4 + 255) & ~255;
    PreviewPixels.Store(thread.y * rowPitch + thread.x * 4, packed.x | (packed.y << 8) | (packed.z << 16) | 0xff000000);
}
