#ifndef PROCEDURAL_TEXTURE_INCLUDED
#define PROCEDURAL_TEXTURE_INCLUDED
#include "noise.hlsl"
#include "NoiseCacheLayout.hlsli"
ByteAddressBuffer ProceduralNoiseSamples : register(t0, space15);
RWByteAddressBuffer ProceduralNoiseRequests : register(u0, space15);

namespace ProceduralTexture
{
struct Parameters
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

// Enum values mirror proceduralstudio.h; GPU parity tests exercise every supported value.
static const uint TextureMarble = 1, TextureWood = 2, TextureGranite = 3, TextureVoronoi = 4;
static const uint TextureBrick = 5, TextureChecker = 6, TextureWater = 7, TextureCloud = 8;
static const uint TextureGeometric = 11;
static const uint OutputColor = 0;
static const int PatternStripes = 0, PatternGrid = 1, PatternDots = 2;
static const int PatternDiamonds = 3, PatternHexagons = 4, PatternTriangles = 5;

static const float Pi = 3.14159265358979323846;

float GeometricMask(Parameters params, float2 uv)
{
    float2 local = frac(uv * clamp(int2(params.RepeatX, params.RepeatY), 1, 128)) - 0.5;
    float size = clamp(params.PatternSize, 0.05, 0.95);
    float softness = clamp(params.PatternSoftness, 0.001, 0.15);
    float distance = 0;
    switch (params.Pattern)
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

float Scalar(Parameters params, float2 uv)
{
    float3 p = float3(uv, 0);
    float2 noiseValues = 0;
    int noiseCount = (params.Kind == TextureVoronoi || params.Kind == TextureChecker || params.Kind == TextureGeometric) ? 0 : 1;
    if (params.Kind == TextureGranite || params.Kind == TextureWater || params.Kind == TextureCloud)
    {
        noiseCount = 2;
    }
    TextureNoise::NoiseSettings noiseSettings = {
        params.Frequency, params.Octaves, params.Lacunarity, params.Gain, params.WarpStrength, params.RidgeOffset, params.Tileable,
        params.Direction, params.Anisotropy, params.KernelFrequency, params.Phase
    };
    // A shared call site keeps the GPU program small instead of inlining the full
    // fractal evaluator separately into every material branch.

    for (int sampleIndex = 0; sampleIndex < noiseCount; ++sampleIndex)
    {
        float3 position = p;
        uint basis = params.BasisType;
        uint fractal = params.Fractal;
        uint seed = params.Seed;
        if (params.Kind == TextureGranite)
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
        else if (params.Kind == TextureWater)
        {
            float time = params.Time * 0.15;
            fractal = sampleIndex == 0 ? TextureNoise::FractalFBm : TextureNoise::FractalRidged;
            position += sampleIndex == 0 ? float3(time, time * 0.6, time * 0.3)
                                        : float3(-time * 0.7, time * 0.4, -time * 0.2);
            if (sampleIndex == 1)
            {
                seed ^= 0x51ed27;
            }
        }
        else if (params.Kind == TextureCloud)
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
    switch (params.Kind)
    {
        case TextureGeometric:
            value = GeometricMask(params, uv);
            break;
        case TextureMarble:
            value = 0.5 + 0.5 * sin((uv.x * floor(params.RingCount + 0.5) + noiseValues.x * params.Turbulence) * 2 * Pi);
            value = pow(value, 0.6);
            break;
        case TextureWood:
        {
            float radius = length(uv - 0.5) * params.RingCount * 2 + noiseValues.x * params.Turbulence * 0.5;
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
            int period = params.Tileable != 0 ? max(1, (int)floor(params.Frequency + 0.5)) : 0;
            float scale = params.Tileable != 0 ? (float)period : params.Frequency;
            float2 distances = TextureNoise::Worley(p * scale, params.Seed, period);
            value = saturate((distances.y - distances.x) * 3);
            break;
        }
        case TextureBrick:
        {
            float2 brick = uv * int2(params.BricksX, params.BricksY);
            int row = (int)floor(brick.y);
            float shifted = brick.x + (row & 1) * 0.5;
            float2 local = float2(frac(shifted), frac(brick.y));
            float edge = min(TextureNoise::Smooth(0, params.Mortar * params.BricksX * 0.5, min(local.x, 1 - local.x)),
                             TextureNoise::Smooth(0, params.Mortar * params.BricksY * 0.5, min(local.y, 1 - local.y)));
            float variation = TextureNoise::Unit(TextureNoise::Hash3(int3(TextureNoise::Wrap((int)floor(shifted), max(params.BricksX, 1)), TextureNoise::Wrap(row, max(params.BricksY, 1)), 0), params.Seed));
            value = edge * (0.6 + 0.4 * variation) * (0.85 + 0.15 * noiseValues.x);
            break;
        }
        case TextureChecker:
            value = (((int)floor(uv.x * params.BricksX) + (int)floor(uv.y * params.BricksX)) & 1) != 0 ? 1 : 0;
            break;
        case TextureWater:
        {
            value = saturate(lerp(noiseValues.x, noiseValues.y, params.Choppiness));
            break;
        }
        case TextureCloud:
        {
            float shape = noiseValues.x * 0.65 + (1 - noiseValues.y) * 0.35;
            value = TextureNoise::Smooth(1 - params.Coverage - params.CloudSoftness * 0.5, 1 - params.Coverage + params.CloudSoftness * 0.5, shape);
            break;
        }
        default:
            value = noiseValues.x;
            break;
    }
    return saturate((value - 0.5) * params.Contrast + 0.5 + params.Bias);
}

float3 Color(Parameters params, float value)
{
    switch (params.Kind)
    {
        case TextureGeometric:
            return lerp(params.ColorA.rgb, params.ColorB.rgb, value);
        case TextureBrick:
            return lerp(params.ColorB.rgb, params.ColorC.rgb * (0.6 + 0.6 * value), saturate(value * 4));
        case TextureWater:
        {
            float3 water = lerp(float3(0.01, 0.07, 0.13), float3(0.04, 0.32, 0.42), TextureNoise::Smooth(0.2, 0.75, value));
            return lerp(water, float3(0.92, 0.95, 0.98), TextureNoise::Smooth(0.72, 0.92, value) * params.Choppiness);
        }
        case TextureCloud:
            return lerp(float3(0.32, 0.52, 0.86), lerp(params.ColorB.rgb, 1.0, 0.5), value);
        default:
            return value < 0.5 ? lerp(params.ColorA.rgb, params.ColorB.rgb, value * 2) : lerp(params.ColorB.rgb, params.ColorC.rgb, (value - 0.5) * 2);
    }
}

// Rendering reads completed pages only. Each miss bucket has one request owner;
// frame-end barriers make the entire definition available to the merge dispatch.
bool CacheCoordinates(Parameters params, float2 uv, float2 footprint,
                      out NoiseCacheKey key, out uint2 vertex, out float2 blend)
{
    key = (NoiseCacheKey)0;
    vertex = 0;
    blend = 0;
    if (!any(uint2(params.Padding1, params.Padding2) != 0u) ||
        !all(isfinite(uv)) || !all(isfinite(footprint)) || any(abs(footprint) == 0))
    {
        return false;
    }
    float2 levels = floor(log2(abs(footprint)));
    if (any(levels < -120) || any(levels > 120))
    {
        return false;
    }
    float2 scaled = uv / exp2(levels);
    // Keep integer lattice arithmetic exact and leave room for the border vertex.
    // Beyond this range float UVs cannot describe every neighboring grid vertex.
    if (!all(isfinite(scaled)) || any(abs(scaled) > 8388590.0))
    {
        return false;
    }
    float2 cell = floor(scaled);
    int2 integerCell = int2(cell);
    // Signed integer division truncates toward zero; compensate to floor negatives.
    int2 negativeAdjustment = int2(integerCell.x < 0 ? 14 : 0, integerCell.y < 0 ? 14 : 0);
    int2 tile = (integerCell - negativeAdjustment) / int(NoiseCacheCellsPerSide);
    int2 localVertex = integerCell - tile * int(NoiseCacheCellsPerSide);
    if (any(localVertex < 0) || any(localVertex >= int(NoiseCacheCellsPerSide)))
    {
        return false;
    }
    float2 firstVertex = float2(tile) * float(NoiseCacheCellsPerSide);
    if (!all(isfinite(firstVertex * exp2(levels))) ||
        !all(isfinite((firstVertex + float(NoiseCacheCellsPerSide)) * exp2(levels))))
    {
        return false;
    }
    vertex = uint2(localVertex);
    blend = scaled - cell;
    uint2 packedLevels = uint2(int2(levels) + 128);
    key.Words = uint4(params.Padding1, params.Padding2,
                      packedLevels.x | (packedLevels.y << 8u), asuint(tile.x));
    key.TileY = asuint(tile.y);
    return true;
}

bool FindCachePage(NoiseCacheKey key, uint2 vertex, float2 blend, out float3 color)
{
    color = 0;
    if (ProceduralNoiseSamples.Load(0u) == 0u)
    {
        return false;
    }
    uint firstPage = NoiseCacheHash(key) * NoiseCacheWays;
    [unroll] for (uint way = 0; way < NoiseCacheWays; ++way)
    {
        uint page = firstPage + way;
        uint address = NoiseCacheHeaderBytes + page * NoiseCacheEntryBytes;
        [branch] if (all(ProceduralNoiseSamples.Load4(address) == key.Words) &&
                     ProceduralNoiseSamples.Load(address + 16u) == key.TileY)
        {
            float3 c00 = NoiseCacheUnpackColor(ProceduralNoiseSamples.Load2(NoiseCacheTexelAddress(page, vertex)));
            float3 c10 = NoiseCacheUnpackColor(ProceduralNoiseSamples.Load2(NoiseCacheTexelAddress(page, vertex + uint2(1, 0))));
            float3 c01 = NoiseCacheUnpackColor(ProceduralNoiseSamples.Load2(NoiseCacheTexelAddress(page, vertex + uint2(0, 1))));
            float3 c11 = NoiseCacheUnpackColor(ProceduralNoiseSamples.Load2(NoiseCacheTexelAddress(page, vertex + uint2(1, 1))));
            color = lerp(lerp(c00, c10, blend.x), lerp(c01, c11, blend.x), blend.y);
            return true;
        }
    }
    return false;
}

bool FindCachedColor(Parameters params, float2 uv, float2 footprint, out float3 color)
{
    NoiseCacheKey key;
    uint2 vertex;
    float2 blend;
    color = 0;
    if (!CacheCoordinates(params, uv, footprint, key, vertex, blend))
    {
        return false;
    }
    return FindCachePage(key, vertex, blend, color);
}

float3 CachedColor(Parameters params, float2 uv, float2 footprint)
{
    NoiseCacheKey key;
    uint2 vertex;
    float2 blend;
    bool cacheable = CacheCoordinates(params, uv, footprint, key, vertex, blend) &&
                     ProceduralNoiseSamples.Load(0u) != 0u;
    float3 color;
    if (cacheable)
    {
        [branch] if (FindCachePage(key, vertex, blend, color))
        {
            return color;
        }
    }
    color = Color(params, Scalar(params, uv));
    if (cacheable)
    {
        uint address = NoiseCacheHash(key) * NoiseCacheRequestBytes;
        uint previous;
        ProceduralNoiseRequests.InterlockedCompareExchange(address + NoiseCacheRequestStateOffset, 0u, 1u, previous);
        if (previous == 0u)
        {
            ProceduralNoiseRequests.Store4(address, key.Words);
            ProceduralNoiseRequests.Store(address + 16u, key.TileY);
            // Explicit vectors keep this shared include compatible with FXC SM5.
            uint definitionAddress = address + 32u;
            ProceduralNoiseRequests.Store4(definitionAddress, uint4(params.Width, params.Height, params.Kind, params.Output));
            ProceduralNoiseRequests.Store4(definitionAddress + 16u, uint4(params.BasisType, params.Fractal, params.Seed, params.Tileable));
            ProceduralNoiseRequests.Store4(definitionAddress + 32u, uint4(asuint(params.Frequency), asuint(params.Octaves), asuint(params.Lacunarity), asuint(params.Gain)));
            ProceduralNoiseRequests.Store4(definitionAddress + 48u, asuint(float4(params.WarpStrength, params.RidgeOffset, params.Contrast, params.Bias)));
            ProceduralNoiseRequests.Store4(definitionAddress + 64u, asuint(params.ColorA));
            ProceduralNoiseRequests.Store4(definitionAddress + 80u, asuint(params.ColorB));
            ProceduralNoiseRequests.Store4(definitionAddress + 96u, asuint(params.ColorC));
            ProceduralNoiseRequests.Store4(definitionAddress + 112u, asuint(float4(params.Turbulence, params.RingCount, params.Mortar, params.Time)));
            ProceduralNoiseRequests.Store4(definitionAddress + 128u, asuint(int4(params.BricksX, params.BricksY, params.Pattern, params.RepeatX)));
            ProceduralNoiseRequests.Store4(definitionAddress + 144u, uint4(asuint(params.RepeatY), asuint(params.PatternSize), asuint(params.PatternSoftness), asuint(params.NormalStrength)));
            ProceduralNoiseRequests.Store4(definitionAddress + 160u, asuint(float4(params.Choppiness, params.Coverage, params.CloudSoftness, params.Padding)));
            ProceduralNoiseRequests.Store4(definitionAddress + 176u, uint4(params.PreviewSize, params.Tiles, params.Padding1, params.Padding2));
            ProceduralNoiseRequests.Store4(definitionAddress + 192u, asuint(float4(params.Direction, params.Anisotropy, params.KernelFrequency, params.Phase)));
        }
    }
    return color;
}

// Secondary rays may reuse visible tiles, but do not request entire new pages
// for sparse random hits. Their misses remain analytical.
float3 CachedColorReadOnly(Parameters params, float2 uv, float2 footprint)
{
    float3 color;
    [branch] if (FindCachedColor(params, uv, footprint, color))
    {
        return color;
    }
    return Color(params, Scalar(params, uv));
}

// Callers without a reliable footprint retain the exact analytical result.
float3 CachedColor(Parameters params, float2 uv)
{
    return Color(params, Scalar(params, uv));
}

// Return a tangent-space normal, with the same V-axis convention as studio image export.
float3 Normal(Parameters params, float2 uv, float2 footprint)
{
    float2 e = max(abs(footprint), 0.00001.xx);
    float4 samples = 0;
    [loop] for (uint i = 0; i < 4; ++i)
    {
        float2 offset = i < 2 ? float2((i == 0 ? -1 : 1) * e.x, 0) :
                               float2(0, (i == 2 ? -1 : 1) * e.y);
        samples[i] = Scalar(params, uv + offset);
    }
    float2 slope = float2(samples.y - samples.x, samples.w - samples.z) / (2 * e);
    // Match NormalStrength at the studio's 512-pixel reference view, independently of export resolution.
    slope *= params.NormalStrength / 256.0;
    return normalize(float3(-slope.x, slope.y, 1));
}
}
#endif
