#include "noise.hlsl"

// Pixel-independent procedural work. Escape fractals retain the CPU double-precision path.
cbuffer Parameters : register(b0)
{
    uint Width, Height, Kind, Output;
    uint BasisType, Fractal, Seed, Tileable;
    float Frequency;
    int Octaves;
    float Lacunarity, Gain;
    float WarpStrength, RidgeOffset, Contrast, Bias;
    float4 ColorA, ColorB, ColorC;
    float Turbulence, RingCount, Mortar, Time;
    int BricksX, BricksY, Pattern, RepeatX;
    int RepeatY;
    float PatternSize, PatternSoftness, NormalStrength;
    float Choppiness, Coverage, CloudSoftness, Padding;
    uint PreviewSize, Tiles, Padding1, Padding2;
    float Direction, Anisotropy, KernelFrequency, Phase;
};
RWStructuredBuffer<float4> Generated : register(u0);
StructuredBuffer<float4> ImagePixels : register(t0);
RWByteAddressBuffer PreviewPixels : register(u1);
// Enum values mirror proceduralstudio.h; GPU parity tests exercise every supported value.
static const uint TextureMarble = 1, TextureWood = 2, TextureGranite = 3, TextureVoronoi = 4;
static const uint TextureBrick = 5, TextureChecker = 6, TextureWater = 7, TextureCloud = 8;
static const uint TextureGeometric = 11;
static const uint OutputColor = 0;
static const int PatternStripes = 0, PatternGrid = 1, PatternDots = 2;
static const int PatternDiamonds = 3, PatternHexagons = 4, PatternTriangles = 5;

static const float Pi = 3.14159265358979323846;

float GeometricMask(float2 uv)
{
    float2 local = frac(uv * clamp(int2(RepeatX, RepeatY), 1, 128)) - 0.5;
    float size = clamp(PatternSize, 0.05, 0.95);
    float softness = clamp(PatternSoftness, 0.001, 0.15);
    float distance = 0;
    switch (Pattern)
    {
        case PatternStripes:
            distance = abs(local.x) - size * 0.5;
            break;
        case PatternGrid:
            distance = min(0.5 - abs(local.x), 0.5 - abs(local.y)) - size * 0.5;
            break;
        case PatternDots:
            distance = length(local) - size * 0.5;
            break;
        case PatternDiamonds:
            distance = (abs(local.x) + abs(local.y) - size * 0.5) / sqrt(2.0);
            break;
        case PatternHexagons:
            distance = max(abs(local.x) * 0.8660254 + abs(local.y) * 0.5, abs(local.y)) - size * 0.4330127;
            break;
        case PatternTriangles:
            distance = 1;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    float2 triangleLocal = local + int2(x, y);
                    distance = min(distance, max(abs(triangleLocal.x) * 0.8660254 + triangleLocal.y * 0.5, -triangleLocal.y) - size * 0.25);
                }
            }
            break;
        default:
            return 0;
    }
    return 1 - TextureNoise::Smooth(-softness * 0.5, softness * 0.5, distance);
}

float Scalar(float2 uv)
{
    float3 p = float3(uv, 0);
    float2 noiseValues = 0;
    int noiseCount = (Kind == TextureVoronoi || Kind == TextureChecker || Kind == TextureGeometric) ? 0 : 1;
    if (Kind == TextureGranite || Kind == TextureWater || Kind == TextureCloud)
    {
        noiseCount = 2;
    }
    TextureNoise::NoiseSettings noiseSettings = {
        Frequency, Octaves, Lacunarity, Gain, WarpStrength, RidgeOffset, Tileable,
        Direction, Anisotropy, KernelFrequency, Phase
    };
    // A shared call site keeps the GPU program small instead of inlining the full
    // fractal evaluator separately into every material branch.
    [loop]
    for (int sampleIndex = 0; sampleIndex < noiseCount; ++sampleIndex)
    {
        float3 position = p;
        uint basis = BasisType;
        uint fractal = Fractal;
        uint seed = Seed;
        if (Kind == TextureGranite)
        {
            if (sampleIndex == 0)
            {
                basis = TextureNoise::BasisWorley;
                fractal = TextureNoise::FractalFBm;
            }
            else
            {
                position *= 4;
            }
        }
        else if (Kind == TextureWater)
        {
            float time = Time * 0.15;
            fractal = sampleIndex == 0 ? TextureNoise::FractalFBm : TextureNoise::FractalRidged;
            position += sampleIndex == 0 ? float3(time, time * 0.6, time * 0.3)
                                        : float3(-time * 0.7, time * 0.4, -time * 0.2);
            if (sampleIndex == 1)
            {
                seed ^= 0x51ed27;
            }
        }
        else if (Kind == TextureCloud)
        {
            fractal = TextureNoise::FractalFBm;
            if (sampleIndex == 1)
            {
                basis = TextureNoise::BasisWorley;
                seed ^= 0x7f4a7c15;
            }
        }
        noiseValues[sampleIndex] = TextureNoise::Noise(position, basis, fractal, seed, noiseSettings);
    }
    float value = 0;
    switch (Kind)
    {
        case TextureGeometric:
            value = GeometricMask(uv);
            break;
        case TextureMarble:
            value = 0.5 + 0.5 * sin((uv.x * floor(RingCount + 0.5) + noiseValues.x * Turbulence) * 2 * Pi);
            value = pow(value, 0.6);
            break;
        case TextureWood:
        {
            float radius = length(uv - 0.5) * RingCount * 2 + noiseValues.x * Turbulence * 0.5;
            value = frac(radius);
            value = TextureNoise::Smooth(0, 0.8, value) * (1 - TextureNoise::Smooth(0.85, 1, value));
            break;
        }
        case TextureGranite:
            value = saturate(noiseValues.x * 0.6 + noiseValues.y * 0.4);
            value = value > 0.62 ? 1 : value < 0.3 ? 0 : value;
            break;
        case TextureVoronoi:
        {
            int period = Tileable != 0 ? max(1, (int)floor(Frequency + 0.5)) : 0;
            float scale = Tileable != 0 ? (float)period : Frequency;
            float2 distances = TextureNoise::Worley(p * scale, Seed, period);
            value = saturate((distances.y - distances.x) * 3);
            break;
        }
        case TextureBrick:
        {
            float2 brick = uv * int2(BricksX, BricksY);
            int row = (int)floor(brick.y);
            float shifted = brick.x + (row & 1) * 0.5;
            float2 local = float2(frac(shifted), frac(brick.y));
            float edge = min(TextureNoise::Smooth(0, Mortar * BricksX * 0.5, min(local.x, 1 - local.x)),
                             TextureNoise::Smooth(0, Mortar * BricksY * 0.5, min(local.y, 1 - local.y)));
            float variation = TextureNoise::Unit(TextureNoise::Hash3(int3(TextureNoise::Wrap((int)floor(shifted), max(BricksX, 1)), TextureNoise::Wrap(row, max(BricksY, 1)), 0), Seed));
            value = edge * (0.6 + 0.4 * variation) * (0.85 + 0.15 * noiseValues.x);
            break;
        }
        case TextureChecker:
            value = (((int)floor(uv.x * BricksX) + (int)floor(uv.y * BricksX)) & 1) != 0 ? 1 : 0;
            break;
        case TextureWater:
        {
            value = saturate(lerp(noiseValues.x, noiseValues.y, Choppiness));
            break;
        }
        case TextureCloud:
        {
            float shape = noiseValues.x * 0.65 + (1 - noiseValues.y) * 0.35;
            value = TextureNoise::Smooth(1 - Coverage - CloudSoftness * 0.5, 1 - Coverage + CloudSoftness * 0.5, shape);
            break;
        }
        default:
            value = noiseValues.x;
            break;
    }
    return saturate((value - 0.5) * Contrast + 0.5 + Bias);
}

float3 Color(float value)
{
    switch (Kind)
    {
        case TextureGeometric:
            return lerp(ColorA.rgb, ColorB.rgb, value);
        case TextureBrick:
            return lerp(ColorB.rgb, ColorC.rgb * (0.6 + 0.6 * value), saturate(value * 4));
        case TextureWater:
        {
            float3 water = lerp(float3(0.01, 0.07, 0.13), float3(0.04, 0.32, 0.42), TextureNoise::Smooth(0.2, 0.75, value));
            return lerp(water, float3(0.92, 0.95, 0.98), TextureNoise::Smooth(0.72, 0.92, value) * Choppiness);
        }
        case TextureCloud:
            return lerp(float3(0.32, 0.52, 0.86), lerp(ColorB.rgb, 1.0, 0.5), value);
        default:
            return value < 0.5 ? lerp(ColorA.rgb, ColorB.rgb, value * 2) : lerp(ColorB.rgb, ColorC.rgb, (value - 0.5) * 2);
    }
}

float HeightAt(int2 pixel)
{
    int2 wrapped = int2(TextureNoise::Wrap(pixel.x, Width), TextureNoise::Wrap(pixel.y, Height));
    return ImagePixels[wrapped.y * Width + wrapped.x].x;
}

[numthreads(8, 8, 1)]
void Generate(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Width || thread.y >= Height)
    {
        return;
    }
    float value = Scalar((thread.xy + 0.5) / float2(Width, Height));
    if (Kind == TextureChecker)
    {
        // Integer cell addressing avoids reciprocal rounding at hard checker boundaries.
        uint cellX = ((thread.x * 2 + 1) * (uint)BricksX) / (Width * 2);
        uint cellY = ((thread.y * 2 + 1) * (uint)BricksX) / (Height * 2);
        value = saturate((((cellX + cellY) & 1) - 0.5) * Contrast + 0.5 + Bias);
    }
    float3 color = Output == OutputColor ? Color(value) : value.xxx;
    Generated[thread.y * Width + thread.x] = float4(color, 1);
}

[numthreads(8, 8, 1)]
void Normals(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Width || thread.y >= Height)
    {
        return;
    }
    int2 pixel = int2(thread.xy);
    float dx = (HeightAt(pixel + int2(1, 0)) - HeightAt(pixel - int2(1, 0))) * NormalStrength;
    float dy = (HeightAt(pixel + int2(0, 1)) - HeightAt(pixel - int2(0, 1))) * NormalStrength;
    float3 normal = normalize(float3(-dx, dy, 1));
    Generated[thread.y * Width + thread.x] = float4(normal * 0.5 + 0.5, 1);
}

float4 Pixel(int2 pixel)
{
    return ImagePixels[TextureNoise::Wrap(pixel.y, Height) * Width + TextureNoise::Wrap(pixel.x, Width)];
}

[numthreads(8, 8, 1)]
void Preview(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= PreviewSize || thread.y >= PreviewSize)
    {
        return;
    }
    float2 uv = (thread.xy + 0.5) / (float)PreviewSize * Tiles;
    float2 pixel = frac(uv) * float2(Width, Height) - 0.5;
    int2 start = (int2)floor(pixel);
    float2 blend = frac(pixel);
    float3 color = lerp(lerp(Pixel(start), Pixel(start + int2(1, 0)), blend.x),
                        lerp(Pixel(start + int2(0, 1)), Pixel(start + int2(1, 1)), blend.x), blend.y).rgb;
    if (Output == OutputColor)
    {
        color = saturate(color);
        color = float3(color.r <= 0.0031308 ? color.r * 12.92 : 1.055 * pow(color.r, 1.0 / 2.4) - 0.055,
                       color.g <= 0.0031308 ? color.g * 12.92 : 1.055 * pow(color.g, 1.0 / 2.4) - 0.055,
                       color.b <= 0.0031308 ? color.b * 12.92 : 1.055 * pow(color.b, 1.0 / 2.4) - 0.055);
    }
    uint3 packed = (uint3)(saturate(color) * 255 + 0.5);
    uint rowPitch = (PreviewSize * 4 + 255) & ~255;
    PreviewPixels.Store(thread.y * rowPitch + thread.x * 4, packed.x | (packed.y << 8) | (packed.z << 16) | 0xff000000);
}
